// Codegen for the hierarchical data structure

#include "taichi/ir/ir.h"
#include "taichi/ir/expression.h"
#include "taichi/program/snode_expr_utils.h"
#include "taichi/program/program.h"
#include "struct.h"

TLANG_NAMESPACE_BEGIN

void StructCompiler::collect_snodes(SNode &snode) {
  snodes.push_back(&snode);
  for (int ch_id = 0; ch_id < (int)snode.ch.size(); ch_id++) {
    auto &ch = snode.ch[ch_id];
    collect_snodes(*ch);
  }
}

void StructCompiler::infer_snode_properties(SNode &snode) {
  for (int ch_id = 0; ch_id < (int)snode.ch.size(); ch_id++) {
    auto &ch = snode.ch[ch_id];
    ch->parent = &snode;
    for (int i = 0; i < taichi_max_num_indices; i++) {
      ch->extractors[i].num_elements *= snode.extractors[i].num_elements;
      bool found = false;
      for (int k = 0; k < taichi_max_num_indices; k++) {
        if (snode.physical_index_position[k] == i) {
          found = true;
          break;
        }
      }
      if (found)
        continue;
      if (snode.extractors[i].active) {
        snode.physical_index_position[snode.num_active_indices++] = i;
      }
    }

    std::memcpy(ch->physical_index_position, snode.physical_index_position,
                sizeof(snode.physical_index_position));
    ch->num_active_indices = snode.num_active_indices;

    if (snode.type == SNodeType::bit_struct ||
        snode.type == SNodeType::bit_array) {
      ch->is_bit_level = true;
    } else {
      ch->is_bit_level = snode.is_bit_level;
    }

    infer_snode_properties(*ch);
  }

  // infer extractors
  int acc_offsets = 0;
  for (int i = taichi_max_num_indices - 1; i >= 0; i--) {
    snode.extractors[i].acc_offset = acc_offsets;
    acc_offsets += snode.extractors[i].num_bits;
  }
  if (snode.type == SNodeType::dynamic) {
    int active_extractor_counder = 0;
    for (int i = 0; i < taichi_max_num_indices; i++) {
      if (snode.extractors[i].num_bits != 0) {
        active_extractor_counder += 1;
        SNode *p = snode.parent;
        while (p) {
          TI_ASSERT_INFO(
              p->extractors[i].num_bits == 0,
              "Dynamic SNode must have a standalone dimensionality.");
          p = p->parent;
        }
      }
    }
    TI_ASSERT_INFO(active_extractor_counder == 1,
                   "Dynamic SNode can have only one index extractor.");
  }

  snode.total_num_bits = 0;
  for (int i = 0; i < taichi_max_num_indices; i++) {
    snode.total_num_bits += snode.extractors[i].num_bits;
  }
  // The highest bit is for the sign.
  constexpr int kMaxTotalNumBits = 64;
  TI_ERROR_IF(
      snode.total_num_bits >= kMaxTotalNumBits,
      "SNode={}: total_num_bits={} exceeded limit={}. This implies that "
      "your requested shape is too large.",
      snode.id, snode.total_num_bits, kMaxTotalNumBits);

  if (snode.has_null()) {
    ambient_snodes.push_back(&snode);
  }

  if (snode.ch.empty()) {
    if (snode.type != SNodeType::place && snode.type != SNodeType::root) {
      TI_ERROR("{} node must have at least one child.",
               snode_type_name(snode.type));
    }
  }

  if (!snode.index_offsets.empty()) {
    TI_ASSERT(snode.index_offsets.size() == snode.num_active_indices);
  }
}

void StructCompiler::compute_trailing_bits(SNode &snode) {
  std::function<void(SNode &)> bottom_up = [&](SNode &s) {
    for (auto &c : s.ch) {
      bottom_up(*c);
      if (s.type != SNodeType::root)
        for (int i = 0; i < taichi_max_num_indices; i++) {
          auto trailing_bits_according_to_this_child =
              c->extractors[i].num_bits + c->extractors[i].trailing_bits;

          if (s.extractors[i].trailing_bits == 0) {
            s.extractors[i].trailing_bits =
                trailing_bits_according_to_this_child;
          } else if (trailing_bits_according_to_this_child != 0) {
            TI_ERROR_IF(s.extractors[i].trailing_bits !=
                            trailing_bits_according_to_this_child,
                        "Inconsistent trailing bit configuration. Please make "
                        "sure the children of the SNodes are providing the "
                        "same amount of trailing bit.");
          }
        }
    }
  };

  bottom_up(snode);

  std::function<void(SNode &)> top_down = [&](SNode &s) {
    for (auto &c : s.ch) {
      if (s.type != SNodeType::root)
        for (int i = 0; i < taichi_max_num_indices; i++) {
          c->extractors[i].trailing_bits =
              s.extractors[i].trailing_bits - c->extractors[i].num_bits;
        }
      top_down(*c);
    }
  };

  top_down(snode);
}

TLANG_NAMESPACE_END
