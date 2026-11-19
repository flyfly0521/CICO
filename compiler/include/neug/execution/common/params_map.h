/** Copyright 2020 Alibaba Group Holding Limited.
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

#pragma once
#include <map>
#include <string>

namespace neug {

struct DataType;
namespace execution {
class Value;
using ParamsMap = std::map<std::string, Value>;
using ParamsMetaMap = std::map<std::string, DataType>;

inline constexpr const char* kInternalNumThreadsParam =
    "__neug_internal_num_threads";
inline constexpr const char* kInternalEnableFillRootLookupParam =
    "__neug_internal_enable_fill_root_lookup";
inline constexpr const char* kInternalEnableFillIntersectParallelParam =
    "__neug_internal_enable_fill_intersect_parallel";
inline constexpr const char* kInternalEnableFillScanParallelParam =
    "__neug_internal_enable_fill_scan_parallel";
inline constexpr const char* kInternalEnableFillGroupByParallelParam =
    "__neug_internal_enable_fill_group_by_parallel";
inline constexpr const char* kInternalEnableWeightedShortestProcedureParam =
    "__neug_internal_enable_weighted_shortest_procedure";
}  // namespace execution
}  // namespace neug
