#include "Attention.h"
#include "../Model.h"
#include "../Tensor.h"
#include "GemmWS.h"
#include "Softmax.h"

Attention::Attention(SimulationConfig config, Model* model,
               onnx::NodeProto& node_proto)
    : Operation(config, model, node_proto) {
    onnx = true;
    for (auto attribute : node_proto.attribute()) {
        if (attribute.name() == "num_heads") {
            _nh = attribute.i();
        }
    }

    /* Load weight info from node */
    _input_shape = get_input(0)->get_dims();
    _weight_shape = get_input(1)->get_dims();
    _bias_shape = get_input(2)->get_dims();
    _mask_shape = get_input(3)->get_dims();
    if (node_proto.input().size()==5) {
        _kv_cache_shape = get_input(4)->get_dims();
        /* If "past_seq_len" is not 0 */
        if (_kv_cache_shape.at(3))
            has_kv_cache = true;
    }
    assert(_input_shape.size()==3);
    _batch_size = _input_shape.at(0);
    _dmodel = _weight_shape.at(0);
    _nkvh = _nh;
    _dk = _dmodel / _nh;
    _q_len = _input_shape.at(1);
    if (has_kv_cache)
        _seq = _kv_cache_shape.at(3) + 1;
    else
        _seq = _input_shape.at(1);

    _query_shape = std::vector<uint32_t>{_nh, _q_len, _dk};
    _key_shape = std::vector<uint32_t>{_nh, _dk, _seq};
    _value_shape = std::vector<uint32_t>{_nh, _seq, _dk};

    _output_shape = std::vector<uint32_t>{_batch_size, _q_len, _dmodel};
    _liner_output_shape = std::vector<uint32_t>{_batch_size, _q_len, _weight_shape[1]};
    _projection_output_shape = std::vector<uint32_t>{_batch_size, _q_len, _weight_shape[1]/3};
    spdlog::debug("Fused attention: input shape: [{}, {}, {}]", _input_shape.at(0), _input_shape.at(1), _input_shape.at(2));
    spdlog::debug("Fused attention: output shape: [{}, {}, {}]", _output_shape.at(0), _output_shape.at(1), _output_shape.at(2));
    spdlog::debug("Fused attention: query shape: [{}, {}, {}]", _query_shape.at(0), _query_shape.at(1), _query_shape.at(2));
    spdlog::debug("Fused attention: key shape: [{}, {}, {}]", _key_shape.at(0), _key_shape.at(1), _key_shape.at(2));
    spdlog::debug("Fused attention: value shape: [{}, {}, {}]", _value_shape.at(0), _value_shape.at(1), _value_shape.at(2));

    Tensor* pre_defind_tensor = _model->find_tensor(node_proto.output(0));
    if (pre_defind_tensor == nullptr) {
        std::unique_ptr<Tensor> output_tensor = std::make_unique<Tensor>(
            _id, node_proto.output(0), _output_shape, _config.precision, false);
            _outputs.push_back(output_tensor.get()->get_id());
        _model->add_tensor(std::move(output_tensor));
    } else {
        pre_defind_tensor->redefine_tensor(_id, _output_shape);
    }
}

Attention::Attention(SimulationConfig config, Model* model, 
        std::string name, std::map<std::string, std::string>& attributes)
    :Operation(config, model, name, attributes) {
    _batch_size = 1;
    _q_len = std::stoi(get_attribute("num_tokens"));
    _nh = std::stoi(get_attribute("num_heads"));
    _nkvh = std::stoi(get_attribute("num_kv_heads"));
    _dmodel = std::stoi(get_attribute("hidden_size"));
    _dk = _dmodel / _nh;
}

void Attention::initialize_tiles(MappingTable& mapping_table) {
    if(_outputs.empty()) {
        _output_shape =  {_q_len, _dmodel};
        auto output_tensor = std::make_unique<Tensor> (
            _id, name_gen(_name, "output"), _output_shape, _config.precision, false);
        _outputs.push_back(output_tensor.get()->get_id());
        _model->add_tensor(std::move(output_tensor));
        _input_shape = get_input(0)->get_dims();
        _seq = get_input(1)->get_dims()[0]; //key first dim
        _weight_shape = get_input(1)->get_dims();
        _liner_output_shape = std::vector<uint32_t>{_q_len, _weight_shape[1]};
        _query_shape = std::vector<uint32_t>{_nh, _q_len, _dk};
        _key_shape = std::vector<uint32_t>{_nkvh, _seq, _dk};
        _value_shape = std::vector<uint32_t>{_nkvh, _seq, _dk};
    }
    Mapping mapping;
    calculate_loops(mapping);

    /* Check using fusion */
    if (!use_fused && onnx) {
        initialize_non_fused_tiles(mapping_table);
        return;
    }
    /* Create linear node and tensors */
    uint32_t fused_op_id = 0;
    /* Fused Attention body */

    spdlog::info("Mapping info {}", mapping.to_string());
    int core_id = -1;
    for (int N = 0; N < mapping.tile_out_loop.N; N++) {
        int head_off = N * mapping.tile_in_loop.N / _q_len;
        int heads_per_tile = ceil_div(mapping.tile_in_loop.N, _q_len);
        for(int M = 0; M < mapping.tile_out_loop.M; M++) {
            if (M == 0) {
                core_id = (core_id + 1) % _config.num_cores;
            }
            std::unique_ptr<Tile> tile = std::make_unique<Tile>(Tile{
                .status = Tile::Status::INITIALIZED,
                .optype = get_name(),
                .layer_id = _id,
                .fused_op_id = fused_op_id++,
                .Q = 1,
                .P = 1, 
                .M = (uint32_t) M,
                .C = 1,
                .S = 1,
                .R = 1,
                .accum = M != 0,
                .core_id = core_id,
            });
            /* dummy mapping */
            _tiles.push_back(std::move(tile));
            initialize_instructions(_tiles.back().get(), mapping, head_off, heads_per_tile);
        }
    }
}

void Attention::initialize_onnx_tiles(MappingTable& mapping_table) {
    calculate_loops();

    /* Check using fusion */
    if (!use_fused) {
        initialize_non_fused_tiles(mapping_table);
        return;
    }

    /* Create linear node and tensors */
    uint32_t fused_op_id = 0;
    _projection_node = new GemmWS(_config, mapping_table, _input_shape, _weight_shape, _liner_output_shape);
    std::unique_ptr<Tensor> key_projection = std::make_unique<Tensor>(
        _id, "", _projection_output_shape, _config.precision, false);
    std::unique_ptr<Tensor> query_projection = std::make_unique<Tensor>(
        _id, "", _projection_output_shape, _config.precision, false);
    std::unique_ptr<Tensor> value_projection = std::make_unique<Tensor>(
       _id, "", _projection_output_shape, _config.precision, false);

    /* Link tensors to linear node */
    _projection_node->set_model(_model);
    _projection_node->add_input(_inputs.at(0));
    _projection_node->add_input(_inputs.at(1));
    _projection_node->add_input(_inputs.at(2));
    _projection_node->add_output(key_projection.get()->get_id());
    _projection_node->add_output(query_projection.get()->get_id());
    _projection_node->add_output(value_projection.get()->get_id());
    get_input(0)->add_child_node(_projection_node);
    key_projection->add_child_node(this);
    query_projection->add_child_node(this);
    value_projection->add_child_node(this);

    /* Link key query value to attention node */
    _key_projection_id = _INPUT_OPERAND + _inputs.size();
    _inputs.push_back(key_projection.get()->get_id());
    _query_projection_id = _INPUT_OPERAND + _inputs.size();
    _inputs.push_back(query_projection.get()->get_id());
    _value_projection_id = _INPUT_OPERAND + _inputs.size();
    _inputs.push_back(value_projection.get()->get_id());

    /* Register tensor */
    _model->add_tensor(std::move(key_projection));
    _model->add_tensor(std::move(query_projection));
    _model->add_tensor(std::move(value_projection));

    /* Fused Attention body */
    for (int req_idx = 0; req_idx < _batch_size; req_idx++) {
        int heads_per_tile = _heads_per_tile[req_idx];
        for (int head_off=0; head_off<_nh; head_off+=heads_per_tile) {
            uint32_t remain_heads = std::min(_nh-head_off, (uint32_t)heads_per_tile);
            std::unique_ptr<Tile> tile = std::make_unique<Tile>(Tile{
                .status = Tile::Status::INITIALIZED,
                .optype = get_name(),
                .layer_id = _id,
                .fused_op_id = fused_op_id++,
                //.K = 0,
                .accum = false,
            });
            /* dummy mapping */
            _tiles.push_back(std::move(tile));
            initialize_instructions(_tiles.back().get(), head_off, heads_per_tile);
        }
    }
}

// 일단 한 tile에는 최대 하나의 request만 있는 경우부터.
void Attention::initialize_instructions(Tile* tile, int head_idx, int num_heads) {
    // head_idx # start idx
    // num_heads
    uint32_t q_len = _q_len;
    uint32_t seq_len = _seq;
    uint32_t value_offset = ceil_div(num_heads, _nkvh);
    addr_type sram_query_base = SPAD_BASE;
    addr_type sram_key_base = sram_query_base + q_len * _dk * num_heads * _config.precision;
    addr_type sram_value_base = sram_key_base + _dk * seq_len * value_offset * _config.precision;
    addr_type sram_logit_base = ACCUM_SPAD_BASE;  // for logits

    addr_type query_addr = get_operand_addr(_INPUT_OPERAND);
    addr_type key_addr = get_operand_addr(_INPUT_OPERAND + 1);
    addr_type value_addr = get_operand_addr(_INPUT_OPERAND + 2);
    addr_type ouput_addr = get_operand_addr(_OUTPUT_OPERAND);
    assert(num_heads <= _nkvh);
    int kv_head_idx = head_idx / _nkvh;
    addr_type sram_k_ofs = sram_key_base + kv_head_idx * (_dk * seq_len) * _config.precision;
    addr_type sram_v_ofs = sram_value_base + kv_head_idx * (_dk * seq_len) * _config.precision;
    std::set<addr_type> dram_kv_addrs;    // = _key[req_idx]->get_all_addrs();

    for(int seq_idx = 0; seq_idx < seq_len; seq_idx++) {
        for(int i = 0; i <_dk; i++) {
            std::vector<uint32_t> idx = {(uint32_t)(kv_head_idx), (uint32_t)seq_idx, (uint32_t)i};
            dram_kv_addrs.insert(make_address(idx, _key_shape));
        }
    }
    std::vector<addr_type> key_addrs, value_addrs;
    for(auto itr = dram_kv_addrs.begin(); itr != dram_kv_addrs.end(); itr++) {
        key_addrs.push_back(key_addr + *itr);
        value_addrs.push_back(value_addr + *itr);
    }
    tile->instructions.push_back(std::make_unique<Instruction>(Instruction{
        .opcode = Opcode::MOVIN,
        .dest_addr = sram_k_ofs,
        .size = (uint32_t)value_addrs.size(),
        .src_addrs = key_addrs,
        .operand_id = _INPUT_OPERAND + 1,  // key
    }));
    tile->instructions.push_back(std::make_unique<Instruction>(Instruction{
        .opcode = Opcode::MOVIN,
        .dest_addr = sram_v_ofs,
        .size = (uint32_t)value_addrs.size(),
        .src_addrs = value_addrs,
        .operand_id = _INPUT_OPERAND + 2,  // value
    }));
    for (int h_ofs = 0; h_ofs < num_heads; h_ofs++) {
        int h_idx = head_idx + h_ofs;
        addr_type sram_q_ofs = sram_query_base + h_ofs * (q_len * _dk) * _config.precision;
        addr_type sram_l_ofs = sram_logit_base + h_ofs * (q_len * seq_len) * _config.precision;
        std::set<addr_type> dram_query_addrs;  // = _query[req_idx]->get_all_addrs();
        std::set<addr_type> dram_output_addrs;
        for (int i = 0; i < _dk; i++) {
            for (int seq_idx = 0; seq_idx < q_len; seq_idx++) {
                // key:  h, d_k, seq_len
                std::vector<uint32_t> query_idx = {(uint32_t)(h_idx), (uint32_t)seq_idx, (uint32_t)i};
                std::vector<uint32_t> output_idx = {(uint32_t)(h_idx), (uint32_t)seq_idx, (uint32_t)i};
                dram_query_addrs.insert(query_addr + make_address(query_idx, _query_shape));
                dram_output_addrs.insert(ouput_addr + make_address(output_idx, _query_shape)); // Used query_shape intentionally
            }
        }
        // -- load --
        // MOVIN query, key, value
        tile->instructions.push_back(std::make_unique<Instruction>(Instruction{
            .opcode = Opcode::MOVIN,
            .dest_addr = sram_q_ofs,
            .size = (uint32_t)dram_query_addrs.size(),
            .src_addrs = std::vector<addr_type>(dram_query_addrs.begin(), dram_query_addrs.end()),
            .operand_id = _INPUT_OPERAND,  // query
        }));
        // -- compute --
        // GEMM (q*k -> l)
        tile->instructions.push_back(std::make_unique<Instruction>(Instruction{
            .opcode = Opcode::GEMM,
            .dest_addr = sram_l_ofs,
            .size = q_len * seq_len * _config.precision / _config.dram_req_size,
            .compute_size =  ceil_div(q_len, _config.core_height) * seq_len,
            .src_addrs = std::vector<addr_type>{sram_q_ofs, sram_k_ofs},

            .tile_m = seq_len,
            .tile_k = _dk,
            .tile_n = q_len,
        }));
        // Softmax (l -> l)

        tile->instructions.push_back(std::make_unique<Instruction>(Instruction{
            .opcode = Opcode::SOFTMAX,
            .dest_addr = sram_l_ofs,
            .size = q_len * seq_len * _config.precision / _config.dram_req_size,
            .compute_size = seq_len * _config.precision,
            .src_addrs = std::vector<addr_type>{sram_l_ofs},
            .tile_m = q_len,
            .src_from_accum = true,
        }));

        // [ ] change output offset
        // GEMM (l*v -> acc)
        tile->instructions.push_back(std::make_unique<Instruction>(Instruction{
            .opcode = Opcode::GEMM,
            .dest_addr = sram_l_ofs,
            .size = q_len * _dk * _config.precision / _config.dram_req_size,
            .compute_size = q_len * _dk,
            .src_addrs = std::vector<addr_type>{sram_l_ofs, sram_v_ofs},

            .tile_m = _dk,
            .tile_k = seq_len,
            .tile_n = q_len,
            .src_from_accum = true,
        }));

        // MOVOUT
        tile->instructions.push_back(std::make_unique<Instruction>(Instruction{
            .opcode = Opcode::MOVOUT,
            .dest_addr = sram_l_ofs,
            .size = (uint32_t)dram_output_addrs.size(),
            .src_addrs = std::vector<addr_type>(dram_output_addrs.begin(), dram_output_addrs.end()),
            .operand_id = _OUTPUT_OPERAND,
        }));
    }
}

void Attention::initialize_instructions(Tile* tile, Mapping mapping, int head_idx, int num_heads) {
    // head_idx # start idx
    // num_heads
    uint32_t q_len = mapping.tile_in_loop.N;
    uint32_t seq_len = mapping.tile_in_loop.M;
    uint32_t value_offset = ceil_div(num_heads, _nkvh);
    addr_type sram_query_base = SPAD_BASE;
    addr_type sram_key_base = sram_query_base + q_len * _dk * num_heads * _config.precision;
    addr_type sram_value_base = sram_key_base + _dk * seq_len * value_offset * _config.precision;
    addr_type sram_logit_base = ACCUM_SPAD_BASE;  // for logits
    std::vector<uint32_t> output_shape = {_seq, _nh, _dk};
    addr_type query_addr = get_operand_addr(_INPUT_OPERAND);
    addr_type key_addr = get_operand_addr(_INPUT_OPERAND + 1);
    addr_type value_addr = get_operand_addr(_INPUT_OPERAND + 2);
    addr_type ouput_addr = get_operand_addr(_OUTPUT_OPERAND);
    addr_type logits_addr = get_operand_addr(_OUTPUT_OPERAND) + _dmodel * _q_len * _config.precision; // logits addr for scale
    assert(num_heads <= _nkvh);
    int kv_head_idx = head_idx / _nkvh;
    addr_type sram_k_ofs = sram_key_base + kv_head_idx * (_dk * seq_len) * _config.precision;
    addr_type sram_v_ofs = sram_value_base + kv_head_idx * (_dk * seq_len) * _config.precision;
    std::set<addr_type> dram_kv_addrs;    // = _key[req_idx]->get_all_addrs();
    for(int seq_idx = 0; seq_idx < seq_len; seq_idx++) {
        int kv_seq_index = tile->M * seq_len + seq_idx;
        for(int i = 0; i <_dk; i++) {
            std::vector<uint32_t> idx = {(uint32_t)(kv_head_idx), (uint32_t)seq_idx, (uint32_t)i};
            dram_kv_addrs.insert(make_address(idx, _key_shape));
        }
    }

    std::vector<addr_type> key_addrs, value_addrs;
    for(auto itr = dram_kv_addrs.begin(); itr != dram_kv_addrs.end(); itr++) {
        key_addrs.push_back(key_addr + *itr);
        value_addrs.push_back(value_addr + *itr);
    }
    tile->instructions.push_back(std::make_unique<Instruction>(Instruction{
        .opcode = Opcode::MOVIN,
        .dest_addr = sram_k_ofs,
        .size = (uint32_t)value_addrs.size(),
        .src_addrs = key_addrs,
        .operand_id = _INPUT_OPERAND + 1,  // key
    }));
    tile->instructions.push_back(std::make_unique<Instruction>(Instruction{
        .opcode = Opcode::MOVIN,
        .dest_addr = sram_v_ofs,
        .size = (uint32_t)value_addrs.size(),
        .src_addrs = value_addrs,
        .operand_id = _INPUT_OPERAND + 2,  // value
    }));
    for (int h_ofs = 0; h_ofs < num_heads; h_ofs++) {
        int h_idx = head_idx + h_ofs;
        addr_type sram_q_ofs = sram_query_base + h_ofs * (q_len * _dk) * _config.precision;
        addr_type sram_l_ofs = sram_logit_base + h_ofs * (q_len * seq_len) * _config.precision;
        addr_type sram_logits_offset = sram_l_ofs + num_heads * (q_len * seq_len) * _config.precision;
        std::set<addr_type> dram_query_addrs;  // = _query[req_idx]->get_all_addrs();
        std::set<addr_type> dram_output_addrs;
        for (int seq_idx = 0; seq_idx < q_len; seq_idx++) {
            for (int i = 0; i < _dk; i++) {
                // key:  h, d_k, seq_len
                int q_index = tile->M * q_len + seq_idx;
                std::vector<uint32_t> query_idx = {(uint32_t)(h_idx), (uint32_t)q_index, (uint32_t)i};
                std::vector<uint32_t> output_idx = {(uint32_t)q_index, (uint32_t)(h_idx), (uint32_t)i};
                dram_query_addrs.insert(query_addr + make_address(query_idx, _query_shape));
                dram_output_addrs.insert(ouput_addr + make_address(output_idx, output_shape)); // Used query_shape intentionally
            }
        }
        // -- load --
        // MOVIN query, key, value
        tile->instructions.push_back(std::make_unique<Instruction>(Instruction{
            .opcode = Opcode::MOVIN,
            .dest_addr = sram_q_ofs,
            .size = (uint32_t)dram_query_addrs.size(),
            .src_addrs = std::vector<addr_type>(dram_query_addrs.begin(), dram_query_addrs.end()),
            .operand_id = _INPUT_OPERAND,  // query
        }));
        // -- compute --
        // GEMM (q*k -> l)
        for(int qitr = 0; qitr < ceil_div(q_len, _config.core_height); qitr++) {
            for(int kitr = 0; kitr < ceil_div(_dk, _config.core_height); kitr++) {
                tile->instructions.push_back(std::make_unique<Instruction>(Instruction{
                    .opcode = Opcode::GEMM_PRELOAD,
                    .dest_addr = sram_l_ofs,
                    .size =  seq_len * _config.precision / _config.dram_req_size,
                    .compute_size = seq_len,
                    .src_addrs = std::vector<addr_type>{sram_q_ofs, sram_k_ofs},
                    .tile_m = seq_len,
                    .tile_k = _dk,
                    .tile_n = q_len,
                }));
            }
        }
        // Softmax (l -> l)

        tile->instructions.push_back(std::make_unique<Instruction>(Instruction{
            .opcode = Opcode::ADDTREE,
            .dest_addr = sram_logits_offset,
            .size = q_len * seq_len * _config.precision / _config.dram_req_size,
            .compute_size = seq_len * _config.precision + 1, // 1 for prior max
            .src_addrs = std::vector<addr_type>{sram_l_ofs},
            .tile_m = q_len,
            .src_from_accum = true,
        }));// On chip, compute 𝑚(𝑗) = max(𝑚(𝑗−1),rowmax(S(𝑗))) ∈ R𝐵𝑟
        tile->instructions.push_back(std::make_unique<Instruction>(Instruction{ 
            .opcode = Opcode::ADD,
            .dest_addr = sram_l_ofs,
            .size = q_len * seq_len * _config.precision / _config.dram_req_size,
            .compute_size = q_len * seq_len * _config.precision,
            .src_addrs = std::vector<addr_type>{sram_l_ofs, sram_logits_offset},
            .tile_m = q_len,
            .src_from_accum = true,
        })); // S(𝑗) −𝑚(𝑗)
        tile->instructions.push_back(std::make_unique<Instruction>(Instruction{
            .opcode = Opcode::EXP,
            .dest_addr = sram_l_ofs,
            .size = q_len * seq_len * _config.precision / _config.dram_req_size,
            .compute_size = q_len * (seq_len + 1) * _config.precision, // 1 for prior max
            .src_addrs = std::vector<addr_type>{sram_l_ofs},
            .tile_m = q_len,
            .src_from_accum = true,
        })); // P(j) = exp(S(𝑗) −𝑚(𝑗))
        tile->instructions.push_back(std::make_unique<Instruction>(Instruction{
            .opcode = Opcode::ADDTREE,
            .dest_addr = sram_logits_offset,
            .size = q_len * seq_len * _config.precision / _config.dram_req_size,
            .compute_size = seq_len * _config.precision,
            .src_addrs = std::vector<addr_type>{sram_l_ofs},
            .tile_m = q_len,
            .src_from_accum = true,
        }));// rowsum(P(𝑗)) ∈ R𝐵𝑟
        tile->instructions.push_back(std::make_unique<Instruction>(Instruction{
            .opcode = Opcode::MAC,
            .dest_addr = sram_logits_offset,
            .size = q_len * seq_len * _config.precision / _config.dram_req_size,
            .compute_size = q_len * seq_len * _config.precision,
            .src_addrs = std::vector<addr_type>{sram_logits_offset},
            .tile_m = q_len,
            .src_from_accum = true,
        })); 
        tile->instructions.push_back(std::make_unique<Instruction>(Instruction{
            .opcode = Opcode::DIV,
            .dest_addr = sram_l_ofs,
            .size = q_len * _config.precision / _config.dram_req_size,
            .compute_size = q_len  * _config.precision,
            .src_addrs = std::vector<addr_type>{sram_logits_offset},
            .tile_m = q_len,
            .src_from_accum = true,
        })); // diag(exp(m(j-1) - m(j))-1)
        tile->instructions.push_back(std::make_unique<Instruction>(Instruction{
            .opcode = Opcode::MUL,
            .dest_addr = sram_l_ofs,
            .size = q_len * seq_len* _config.precision / _config.dram_req_size,
            .compute_size = q_len * seq_len * _config.precision,
            .src_addrs = std::vector<addr_type>{sram_l_ofs, sram_logits_offset},
            .tile_m = q_len,
            .src_from_accum = true,
        })); // diag(exp(m(j-1) - m(j))-1) * O(j-1)
        // [ ] change output offset
        // GEMM (l*v -> acc)
        for(int sitr = 0; sitr < ceil_div(seq_len, _config.core_height); sitr++) {
            for(int kitr = 0; kitr < ceil_div(_dk, _config.core_height); kitr++) {
                tile->instructions.push_back(std::make_unique<Instruction>(Instruction{
                    .opcode = Opcode::GEMM_PRELOAD,
                    .dest_addr = sram_l_ofs,
                    .size = q_len * _config.precision / _config.dram_req_size,
                    .compute_size = q_len,
                    .src_addrs = std::vector<addr_type>{sram_l_ofs, sram_v_ofs},
                    .src_from_accum = true,
                }));
            }
        }
        
        if(tile->M == mapping.tile_out_loop.M -1) {
            tile->instructions.push_back(std::make_unique<Instruction>(Instruction{
                .opcode = Opcode::DIV,
                .dest_addr = sram_l_ofs,
                .size = _q_len * _config.precision / _config.dram_req_size,
                .compute_size = _q_len  * _config.precision,
                .src_addrs = std::vector<addr_type>{sram_logits_offset},
                .tile_m = q_len,
                .src_from_accum = true,
            })); // diag(l(Tc))-1
            tile->instructions.push_back(std::make_unique<Instruction>(Instruction{
                .opcode = Opcode::MUL,
                .dest_addr = sram_l_ofs,
                .size = _q_len * _dk * _config.precision / _config.dram_req_size,
                .compute_size = _q_len * _dk * _config.precision,
                .src_addrs = std::vector<addr_type>{sram_l_ofs, sram_logits_offset},
                .tile_m = q_len,
                .src_from_accum = true,
            })); // diag(l(Tc))-1 * O(Tc)
            // MOVOUT
            tile->instructions.push_back(std::make_unique<Instruction>(Instruction{
                .opcode = Opcode::MOVOUT,
                .dest_addr = sram_l_ofs,
                .size = (uint32_t)dram_output_addrs.size(),
                .src_addrs = std::vector<addr_type>(dram_output_addrs.begin(), dram_output_addrs.end()),
                .operand_id = _OUTPUT_OPERAND,
            }));
        }
    }
}

void Attention::initialize_non_fused_tiles(MappingTable& mapping_table) {
    /* Create linear node and tensors */
    uint32_t fused_op_id = 0;
    std::vector<uint32_t> single_head_query_shape = std::vector<uint32_t>{_q_len, _dk};
    std::vector<uint32_t> single_head_key_shape = std::vector<uint32_t>{_dk, _seq};
    std::vector<uint32_t> single_head_value_shape = std::vector<uint32_t>{_seq, _dk};
    std::vector<uint32_t> single_output_shape = std::vector<uint32_t>{_q_len, _dk};
    std::vector<uint32_t> query_key_shape = std::vector<uint32_t>{_q_len, _seq};

    /* Fused Attention body */
    for (int req_idx = 0; req_idx < _batch_size; req_idx++) {
        for (int head_off=0; head_off<_nh; head_off++) {
            /* Key query matmul */
            GemmWS key_query = GemmWS(_config, mapping_table, single_head_query_shape, single_head_key_shape, query_key_shape);
            /* Todo. dram addr */
            key_query.has_bias = false;
            key_query.initialize_tiles(mapping_table);
            std::deque<std::unique_ptr<Tile>>& key_query_tiles = key_query.get_tiles();
            for (const auto& tile : key_query_tiles) {
                tile->layer_id = _id;
                tile->fused_op_id = fused_op_id;
            }
            _tiles.insert(
                _tiles.end(),
                std::make_move_iterator(key_query.get_tiles().begin()),
                std::make_move_iterator(key_query.get_tiles().end())
            );
            _tiles.push_back(std::make_unique<Tile>(Tile{.status = Tile::Status::BAR, .layer_id = _id}));
            fused_op_id++;

            /* Softmax */
            Softmax attention_score = Softmax(_config, mapping_table, query_key_shape);
            /* Todo. dram addr */
            attention_score.initialize_tiles(mapping_table);
            std::deque<std::unique_ptr<Tile>>& attention_score_tiles = key_query.get_tiles();
            for (const auto& tile : attention_score_tiles) {
                tile->layer_id = _id;
            }
            _tiles.insert(
                _tiles.end(),
                std::make_move_iterator(key_query.get_tiles().begin()),
                std::make_move_iterator(key_query.get_tiles().end())
            );
            _tiles.push_back(std::make_unique<Tile>(Tile{.status = Tile::Status::BAR, .layer_id = _id}));

            /* attention x value */
            GemmWS attention = GemmWS(_config, mapping_table, query_key_shape, single_head_value_shape, single_output_shape);
            /* Todo. dram addr */
            attention.has_bias = false;
            attention.initialize_tiles(mapping_table);
            std::deque<std::unique_ptr<Tile>>& attention_tiles = attention.get_tiles();
            for (const auto& tile : attention_tiles) {
                tile->layer_id = _id;
                tile->fused_op_id = fused_op_id;
            }
            _tiles.insert(
                _tiles.end(),
                std::make_move_iterator(attention.get_tiles().begin()),
                std::make_move_iterator(attention.get_tiles().end())
            );
            _tiles.push_back(std::make_unique<Tile>(Tile{.status = Tile::Status::BAR, .layer_id = _id}));
            fused_op_id++;
        }
    }
}

void Attention::calculate_loops() {
    for (int i = 0; i < _batch_size; i++) {
        uint32_t q_len = _q_len;
        uint32_t seq_len = _seq;

        uint32_t total_spad_size_per_head = 2*seq_len*_dk + q_len*_dk;
        uint32_t total_acc_size_per_head = seq_len*q_len;
        spdlog::info("[Attention] total_spad_size_per_head: {}", total_spad_size_per_head);
        spdlog::info("[Attention] total_acc_size_per_head: {}", total_acc_size_per_head);
        spdlog::info("[Attention] q_len: {}, seq_len: {}, dk: {}", q_len, seq_len, _dk);
        spdlog::info("[Attention] Spad size {}", _config.spad_size KB / 2);
        
        // query: q_len * _dk
        // key: seq_len * _dk
        // value: seq_len * _dk
        // query_key: seq_len * q_len
        // out: q_len * _dk
        int heads_per_kv = _nh / _nkvh;
        total_acc_size_per_head *= _config.precision;
        total_spad_size_per_head *= _config.precision;

        uint32_t spad_capacity = _config.spad_size KB / 2;  // unit: byte
        uint32_t acc_spad_capacity = _config.accum_spad_size KB / 2;

        uint32_t heads_per_tile = std::min(spad_capacity / total_spad_size_per_head,
                                            acc_spad_capacity/ total_acc_size_per_head);
        if (heads_per_tile <= 0) {
            use_fused = false;
            spdlog::info("[Attention] Use non fusion attention!");
            break;
        }
        if (heads_per_tile > heads_per_kv) heads_per_tile = heads_per_kv;
        if (_nh / heads_per_tile < _config.num_cores * 2)
            heads_per_tile = ceil_div(_nh, _config.num_cores * 2);
        if(heads_per_tile % heads_per_kv != 0) heads_per_tile = 1;
 

        spdlog::info("[Fused Attention] ({}) heads_per_tile: {}", i, heads_per_tile);
        spdlog::info("[Fused Attention] q_len: {}, seq_len: {}, dk: {}", q_len, seq_len, _dk);
        spdlog::info("[Fused Attention] spad capacity: 0x{:x}, acc spad capacity: 0x{:x}, " \
            "one head spad size: 0x{:x}, acc spad size: 0x{:x}",
            spad_capacity, acc_spad_capacity, total_spad_size_per_head, total_acc_size_per_head);
        if (heads_per_tile <=0) {
            spdlog::error("Spad capacity is too small!");
            exit(EXIT_FAILURE);
        }
        _heads_per_tile.push_back(heads_per_tile);
    }
}

void Attention::calculate_loops(Mapping& mapping) {
    for (int i = 0; i < _batch_size; i++) {
        uint32_t q_len = _q_len;
        uint32_t seq_len = _seq;
        mapping.total_loop.N = q_len * _nh;
        mapping.total_loop.C = _dk;
        mapping.total_loop.M = _seq;
        mapping.tile_out_loop.C = 1;
        mapping.tile_in_loop.C = _dk;

        uint32_t total_spad_size_per_head = 2*seq_len*_dk + q_len*_dk;
        uint32_t total_acc_size_per_head = seq_len*q_len;
        spdlog::info("[Attention] total_spad_size_per_head: {}", total_spad_size_per_head);
        spdlog::info("[Attention] total_acc_size_per_head: {}", total_acc_size_per_head);
        spdlog::info("[Attention] q_len: {}, seq_len: {}, dk: {}", q_len, seq_len, _dk);
        spdlog::info("[Attention] Spad size {}", _config.spad_size KB / 2);
        
        // query: q_len * _dk
        // key: seq_len * _dk
        // value: seq_len * _dk
        // query_key: seq_len * q_len
        // out: q_len * _dk
        int heads_per_kv = _nh / _nkvh;
        total_acc_size_per_head *= _config.precision;
        total_acc_size_per_head += q_len * 2 * 4; // for softmax
        total_spad_size_per_head *= _config.precision;

        uint32_t spad_capacity = _config.spad_size KB / 2;  // unit: byte
        uint32_t acc_spad_capacity = _config.accum_spad_size KB / 2;

        uint32_t heads_per_tile = std::min(spad_capacity / total_spad_size_per_head,
                                            acc_spad_capacity/ total_acc_size_per_head);
        uint32_t tile_per_heads = std::max(ceil_div(total_spad_size_per_head, spad_capacity),
                                           ceil_div(total_acc_size_per_head, acc_spad_capacity));

        if (heads_per_tile <= 0) {
            use_fused = false; // ONNX
            need_scale = true; // LangModel
            heads_per_tile = 1;
            int q_tile = ceil_div( q_len, tile_per_heads);
            int seq_tile = ceil_div(seq_len, tile_per_heads);
            mapping.tile_in_loop.N = q_tile;
            mapping.tile_in_loop.M = seq_tile;
            mapping.tile_out_loop.N = ceil_div(mapping.total_loop.N, mapping.tile_in_loop.N);
            mapping.tile_out_loop.M = ceil_div(mapping.total_loop.M, mapping.tile_in_loop.M);
            // break;
        }
        else {
            if (heads_per_tile > heads_per_kv) heads_per_tile = heads_per_kv;
            if (_nh / heads_per_tile < _config.num_cores * 2)
                heads_per_tile = ceil_div(_nh, _config.num_cores * 2);
            if(heads_per_tile % heads_per_kv != 0) heads_per_tile = 1;
            _tiles_per_head.push_back(1);
            int out_tiles = ceil_div(_nh,heads_per_tile);
            mapping.tile_out_loop.N = out_tiles;
            mapping.tile_in_loop.N = heads_per_tile * q_len;
            mapping.tile_out_loop.M = 1;
            mapping.tile_in_loop.M = seq_len;
        }
 
        spdlog::info("[Fused Attention] ({}) heads_per_tile: {}", i, heads_per_tile);
        spdlog::info("[Fused Attention] q_len: {}, seq_len: {}, dk: {}", q_len, seq_len, _dk);
        spdlog::info("[Fused Attention] spad capacity: 0x{:x}, acc spad capacity: 0x{:x}, " \
            "one head spad size: 0x{:x}, acc spad size: 0x{:x}",
            spad_capacity, acc_spad_capacity, total_spad_size_per_head, total_acc_size_per_head);
        if (heads_per_tile <=0) {
            spdlog::error("Spad capacity is too small!");
            exit(EXIT_FAILURE);
        }
        _heads_per_tile.push_back(heads_per_tile);
    }
}

uint32_t Attention::sram_size_needed() { return 0; }