/**
 * Copyright 2026 Alibaba Group Holding Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once
#include "neug/compiler/index/index_candidate_enumerator.h"
namespace neug::binder {
class BoundRegularQuery;
class NormalizedQueryPart;
class NormalizedSingleQuery;
}
namespace neug::index {
// Input must be a freshly bound, unre-written query. Unsupported parts are
// retained as diagnostics rather than having clauses/expressions discarded.
NEUG_API std::vector<QueryPart> extractQueryParts(
    const binder::BoundRegularQuery& query, const Constraints& constraints,
    const std::string& source = "query");
// Shared typed-expression adapter for a closed first query part. Callers must
// independently establish that it has no upstream/correlated input rows.
NEUG_API QueryPart extractStandaloneQueryPart(
    const binder::NormalizedQueryPart& part, const Constraints& constraints);
// Ordered, closed pipeline, with scalar identities carried across WITH scopes.
NEUG_API std::vector<QueryPart> extractClosedQueryParts(
    const binder::NormalizedSingleQuery& query, const Constraints& constraints);
}  // namespace neug::index
