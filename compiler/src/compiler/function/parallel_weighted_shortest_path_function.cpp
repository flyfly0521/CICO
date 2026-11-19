/**
 * Copyright 2020 Alibaba Group Holding Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * 	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "neug/compiler/function/parallel_weighted_shortest_path_function.h"

#include "neug/compiler/function/neug_call_function.h"
#include "neug/execution/common/operators/retrieve/weighted_shortest_path_procedure.h"
#include "neug/storages/graph/graph_interface.h"
#include "neug/utils/exception/exception.h"

namespace neug {
namespace function {

function_set ParallelWeightedShortestPathFunction::getFunctionSet() {
  function_set functions;
  auto function = std::make_unique<NeugCallFunction>(
      name, std::vector<common::LogicalTypeID>{});
  function->execPipelineFunc =
      [](const CallFuncInputBase& input_base, IStorageInterface& graph,
         const execution::ParamsMap&, execution::Context&& ctx)
          -> neug::result<execution::Context> {
    const auto* input =
        dynamic_cast<const WeightedShortestPathFuncInput*>(&input_base);
    if (input == nullptr) {
      THROW_RUNTIME_ERROR(
          "parallel_weighted_shortest_path: unexpected input type");
    }
    const auto& read_graph = dynamic_cast<const StorageReadInterface&>(graph);
    return execution::parallel_weighted_shortest_path_procedure(
        read_graph, std::move(ctx), *input);
  };
  functions.push_back(std::move(function));
  return functions;
}

}  // namespace function
}  // namespace neug
