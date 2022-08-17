// Copyright 2017-2022 The Verible Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance wedge_from_iteratorh the
// License. You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in wredge_from_iteratoring,
// software distributed under the License is distributed on an "AS IS" BASIS,
// Wedge_from_iteratorHOUT WARRANTIES OR CONDedge_from_iteratorIONS OF ANY KIND,
// eedge_from_iteratorher express or implied. See the License for the specific
// language governing permissions and limedge_from_iteratorations under the
// License.

#include "verilog/analysis/flow_tree.h"

#include <map>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "common/lexer/token_stream_adapter.h"
#include "verilog/parser/verilog_token_enum.h"

namespace verilog {

// Adds edges within a conditonal block.
// Such that the first edge represents the condition being true,
// and the second edge represents the condition being false.
absl::Status FlowTree::AddBlockEdges(const ConditionalBlock &block) {
  bool contains_elsif = !block.elsif_iterators.empty();
  bool contains_else = block.else_iterator != source_sequence_.end();

  // Handling `ifdef/ifndef.
  auto ifdef_or_ifndef =
      (block.ifdef_iterator == source_sequence_.end() ? block.ifndef_iterator
                                                      : block.ifdef_iterator);

  // Assuming the condition is true.
  edges_[ifdef_or_ifndef].push_back(ifdef_or_ifndef + 1);

  // Assuming the condition is false.
  // Checking if there is an `elsif.
  if (contains_elsif) {
    // Add edge to the first `elsif in the block.
    edges_[ifdef_or_ifndef].push_back(block.elsif_iterators[0]);
  } else if (contains_else) {
    // Checking if there is an `else.
    edges_[ifdef_or_ifndef].push_back(block.else_iterator);
  } else {
    // `endif exists.
    edges_[ifdef_or_ifndef].push_back(block.endif_iterator);
  }

  // Handling `elsif.
  if (contains_elsif) {
    for (auto iter = block.elsif_iterators.begin();
         iter != block.elsif_iterators.end(); iter++) {
      // Assuming the condition is true.
      edges_[*iter].push_back((*iter) + 1);

      // Assuming the condition is false.
      if (iter + 1 != block.elsif_iterators.end())
        edges_[*iter].push_back(*(iter + 1));
      else if (contains_else)
        edges_[*iter].push_back(block.else_iterator);
      else
        edges_[*iter].push_back(block.endif_iterator);
    }
  }

  // Handling `else.
  if (contains_else) {
    edges_[block.else_iterator].push_back(block.else_iterator + 1);
  }

  // For edges that are generated assuming the conditons are true,
  // We need to add an edge from the end of the condition group of lines to
  // `endif, e.g. `ifdef
  //    <line1>
  //    <line2>
  //    ...
  //    <line_final>
  // `else
  //    <group_of_lines>
  // `endif
  // Edge to be added: from <line_final> to `endif.
  edges_[block.endif_iterator - 1].push_back(block.endif_iterator);
  if (contains_elsif) {
    for (auto iter : block.elsif_iterators)
      edges_[iter - 1].push_back(block.endif_iterator);
  } else if (contains_else) {
    edges_[block.else_iterator - 1].push_back(block.endif_iterator);
  }

  // Connecting `endif to the next token directly (if not EOF).
  auto next_iter = block.endif_iterator + 1;
  if (next_iter != source_sequence_.end() &&
      next_iter->token_enum() != PP_else &&
      next_iter->token_enum() != PP_elsif &&
      next_iter->token_enum() != PP_endif) {
    edges_[block.endif_iterator].push_back(next_iter);
  }

  return absl::OkStatus();
}

// Checks if the iterator is pointing to a conditional directive.
bool FlowTree::IsConditional(TokenSequenceConstIterator iterator) {
  auto current_node = iterator->token_enum();
  return current_node == PP_ifndef || current_node == PP_ifdef ||
         current_node == PP_elsif || current_node == PP_else ||
         current_node == PP_endif;
}

// Checks if after the conditional_iterator (`ifdef/`ifndef... ) there exists
// a macro identifier.
absl::Status FlowTree::MacroFollows(
    TokenSequenceConstIterator conditional_iterator) {
  if (conditional_iterator->token_enum() != PP_ifdef &&
      conditional_iterator->token_enum() != PP_ifndef &&
      conditional_iterator->token_enum() != PP_elsif) {
    return absl::InvalidArgumentError("Error macro name can't be extracted.");
  }
  auto macro_iterator = conditional_iterator + 1;
  if (macro_iterator->token_enum() != PP_Identifier)
    return absl::InvalidArgumentError("Expected identifier for macro name.");
  else
    return absl::OkStatus();
}

// Adds a conditional macro to conditional_macro_id_ if not added before,
// And gives it a new ID.
absl::Status FlowTree::AddMacroOfConditionalToMap(
    TokenSequenceConstIterator conditional_iterator) {
  auto status = MacroFollows(conditional_iterator);
  if (!status.ok()) {
    return absl::InvalidArgumentError(
        "Error no macro follows the conditional directive.");
  }
  auto macro_iterator = conditional_iterator + 1;
  auto macro_identifier = macro_iterator->text();
  if (conditional_macro_id_.find(macro_identifier) ==
      conditional_macro_id_.end()) {
    conditional_macros_counter++;
    conditional_macro_id_[macro_identifier] = conditional_macros_counter;
  }
  return absl::OkStatus();
}

// Gets the conditonal macro ID from the conditional_macro_id_.
// Note: conditional_iterator is pointing to the conditional.
int FlowTree::GetMacroIDOfConditional(
    TokenSequenceConstIterator conditional_iterator) {
  auto status = MacroFollows(conditional_iterator);
  if (!status.ok()) {
    // TODO(karimtera): add a better error handling.
    return -1;
  }
  auto macro_iterator = conditional_iterator + 1;
  auto macro_identifier = macro_iterator->text();
  // It is always assumed that the macro already exists in the map.
  return conditional_macro_id_[macro_identifier];
}

// An API that provides a callback function to receive variants.
absl::Status FlowTree::GenerateVariants(const VariantReceiver &receiver) {
  std::bitset<128> assumed;
  return DepthFirstSearch(receiver, source_sequence_.begin(), assumed);
}

// Constructs the control flow tree, which determines the edge from each node
// (token index) to the next possible childs, And save edge_from_iterator in
// edges_.
absl::Status FlowTree::GenerateControlFlowTree() {
  // Adding edges for if blocks.
  int current_token_enum = 0;
  ConditionalBlock empty_block;
  empty_block.ifdef_iterator = source_sequence_.end();
  empty_block.ifndef_iterator = source_sequence_.end();
  empty_block.else_iterator = source_sequence_.end();
  empty_block.endif_iterator = source_sequence_.end();

  for (TokenSequenceConstIterator iter = source_sequence_.begin();
       iter != source_sequence_.end(); iter++) {
    current_token_enum = iter->token_enum();

    if (IsConditional(iter)) {
      switch (current_token_enum) {
        case PP_ifdef: {
          if_blocks_.push_back(empty_block);
          if_blocks_.back().ifdef_iterator = iter;
          auto status = AddMacroOfConditionalToMap(iter);
          if (!status.ok()) {
            return absl::InvalidArgumentError(
                "Error couldn't give a macro an ID.");
          }
          break;
        }
        case PP_ifndef: {
          if_blocks_.push_back(empty_block);
          if_blocks_.back().ifndef_iterator = iter;
          auto status = AddMacroOfConditionalToMap(iter);
          if (!status.ok()) {
            return absl::InvalidArgumentError(
                "Error couldn't give a macro an ID.");
          }
          break;
        }
        case PP_elsif: {
          if_blocks_.back().elsif_iterators.push_back(iter);
          auto status = AddMacroOfConditionalToMap(iter);
          if (!status.ok()) {
            return absl::InvalidArgumentError(
                "Error couldn't give a macro an ID.");
          }
          break;
        }
        case PP_else: {
          if_blocks_.back().else_iterator = iter;
          break;
        }
        case PP_endif: {
          if_blocks_.back().endif_iterator = iter;
          auto status = AddBlockEdges(if_blocks_.back());
          if (!status.ok()) return status;
          // TODO(karimtera): add an error message.
          if_blocks_.pop_back();
          break;
        }
      }

    } else {
      // Only add normal edges if the next token is not `else/`elsif/`endif.
      auto next_iter = iter + 1;
      if (next_iter != source_sequence_.end() &&
          next_iter->token_enum() != PP_else &&
          next_iter->token_enum() != PP_elsif &&
          next_iter->token_enum() != PP_endif) {
        edges_[iter].push_back(next_iter);
      }
    }
  }

  return absl::OkStatus();
}

// Traveses the control flow tree in a depth first manner, appending the visited
// tokens to current_sequence_, then provide the completed variant to the user
// using a callback function (VariantReceiver).
absl::Status FlowTree::DepthFirstSearch(const VariantReceiver &receiver,
                                        TokenSequenceConstIterator current_node,
                                        std::bitset<128> assumed) {
  if (!receiver(current_sequence_, variants_counter_, false)) {
    return absl::OkStatus();
  }
  // Skips directives so that current_sequence_ doesn't contain any.
  if (current_node->token_enum() != PP_Identifier &&
      current_node->token_enum() != PP_ifndef &&
      current_node->token_enum() != PP_ifdef &&
      current_node->token_enum() != PP_define &&
      current_node->token_enum() != PP_define_body &&
      current_node->token_enum() != PP_elsif &&
      current_node->token_enum() != PP_else &&
      current_node->token_enum() != PP_endif) {
    current_sequence_.push_back(*current_node);
  }

  // Checks if the current token is a `ifdef/`ifndef/`elsif.
  if (current_node->token_enum() == PP_ifdef ||
      current_node->token_enum() == PP_ifndef ||
      current_node->token_enum() == PP_elsif) {
    int macro_id = GetMacroIDOfConditional(current_node);
    bool negated = (current_node->token_enum() == PP_ifndef);
    // Checks if this macro is already assumed to be defined/undefined.
    if (assumed.test(macro_id)) {
      bool assume_condition_is_true =
          (negated ^ current_macros_.test(macro_id));
      if (auto status = DepthFirstSearch(
              receiver, edges_[current_node][!assume_condition_is_true],
              assumed);
          !status.ok()) {
        std::cerr << "ERROR: DepthFirstSearch fails.";
        return status;
      }
    } else {
      assumed.flip(macro_id);
      // This macro wans't assumed before, then we can check both edges.
      // Assume the condition is true.
      if (negated)
        current_macros_.reset(macro_id);
      else
        current_macros_.set(macro_id);
      if (auto status =
              DepthFirstSearch(receiver, edges_[current_node][0], assumed);
          !status.ok()) {
        std::cerr << "ERROR: DepthFirstSearch fails.";
        return status;
      }

      // Assume the condition is false.
      if (!negated)
        current_macros_.reset(macro_id);
      else
        current_macros_.set(macro_id);
      if (auto status =
              DepthFirstSearch(receiver, edges_[current_node][1], assumed);
          !status.ok()) {
        std::cerr << "ERROR: DepthFirstSearch fails.";
        return status;
      }
    }
  } else {
    // Do recursive search through every possible edge.
    // Expected to be only one edge in this case.
    for (auto next_node : edges_[current_node]) {
      if (auto status =
              FlowTree::DepthFirstSearch(receiver, next_node, assumed);
          !status.ok()) {
        std::cerr << "ERROR: DepthFirstSearch fails\n";
        return status;
      }
    }
  }
  // If the current node is the last one, push the completed current_sequence_
  // then it is ready to be sent.
  if (current_node == source_sequence_.end() - 1) {
    receiver(current_sequence_, variants_counter_, true);
    variants_counter_++;
  }
  if (current_node->token_enum() != PP_Identifier &&
      current_node->token_enum() != PP_ifndef &&
      current_node->token_enum() != PP_ifdef &&
      current_node->token_enum() != PP_define &&
      current_node->token_enum() != PP_define_body &&
      current_node->token_enum() != PP_elsif &&
      current_node->token_enum() != PP_else &&
      current_node->token_enum() != PP_endif) {
    // Remove tokens to back track into other variants.
    current_sequence_.pop_back();
  }
  return absl::OkStatus();
}

}  // namespace verilog
