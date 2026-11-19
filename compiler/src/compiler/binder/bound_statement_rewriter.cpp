/**
 * Copyright 2020 Alibaba Group Holding Limited.
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

/**
 * This file is originally from the Kùzu project
 * (https://github.com/kuzudb/kuzu) Licensed under the MIT License. Modified by
 * Zhou Xiaoli in 2025 to support Neug-specific features.
 */

#include "neug/compiler/binder/bound_statement_rewriter.h"

#include <chrono>
#include <cstdlib>
#include <string_view>

#include "neug/compiler/binder/binder.h"
#include "neug/compiler/binder/bound_statement_visitor.h"
#include "neug/compiler/binder/expression/literal_expression.h"
#include "neug/compiler/binder/expression/scalar_function_expression.h"
#include "neug/compiler/binder/query/bound_regular_query.h"
#include "neug/compiler/binder/rewriter/bound_rewrite_trace.h"
#include "neug/compiler/binder/rewriter/computation_prefix_rewriter.h"
#include "neug/compiler/binder/rewriter/partial_count_rewriter.h"
#include "neug/compiler/binder/rewriter/distinct_count_partition_normalizer.h"
#include "neug/compiler/binder/rewriter/filtered_count_normalizer.h"
#include "neug/compiler/binder/rewriter/match_clause_pattern_label_rewriter.h"
#include "neug/compiler/binder/rewriter/normalized_query_part_match_rewriter.h"
#include "neug/compiler/binder/rewriter/with_clause_projection_rewriter.h"
#include "neug/compiler/binder/rewriter/rewriter_v4.h"
#include "neug/compiler/binder/visitor/default_type_solver.h"

namespace neug {
namespace binder {
namespace {
using namespace common;

// WHERE b = true, WHERE true = b and WHERE b have the same three-valued
// Boolean semantics. Canonicalize the bound predicate, including equalities
// synthesized by MATCH property maps, before either estimation or rewriting.
// Do not rewrite projections or move a predicate across an OPTIONAL boundary.
std::shared_ptr<Expression> normalizeBooleanPredicate(
    const std::shared_ptr<Expression>& expression) {
  if (!expression || expression->getDataType().getLogicalTypeID() != LogicalTypeID::BOOL)
    return expression;
  const auto trueLiteral = [](const std::shared_ptr<Expression>& value) {
    if (value->expressionType != ExpressionType::LITERAL ||
        value->getDataType().getLogicalTypeID() != LogicalTypeID::BOOL) return false;
    const auto& literal = value->constCast<LiteralExpression>();
    return !literal.isNull() && literal.getValue().getValue<bool>();
  };
  if (expression->expressionType == ExpressionType::EQUALS && expression->getNumChildren() == 2) {
    for (uint32_t i = 0; i < 2; ++i) {
      const auto& other = expression->getChild(1 - i);
      if (trueLiteral(expression->getChild(i)) &&
          other->getDataType().getLogicalTypeID() == LogicalTypeID::BOOL)
        return normalizeBooleanPredicate(other);
    }
  }
  const auto kind = expression->expressionType;
  if (kind != ExpressionType::AND && kind != ExpressionType::OR &&
      kind != ExpressionType::XOR && kind != ExpressionType::NOT) return expression;
  expression_vector children;
  bool changed = false;
  for (const auto& child : expression->getChildren()) {
    auto normalized = normalizeBooleanPredicate(child);
    changed |= normalized != child;
    children.push_back(std::move(normalized));
  }
  if (!changed) return expression;
  // Bound expressions can also be referenced by a projection. Rebuild only
  // the changed Boolean path instead of mutating such shared expressions.
  const auto& scalar = expression->constCast<ScalarFunctionExpression>();
  auto uniqueName = ScalarFunctionExpression::getUniqueName(scalar.getFunction().name, children);
  auto normalized = std::make_shared<ScalarFunctionExpression>(kind,
      scalar.getFunction().copy(), scalar.getBindData()->copy(), std::move(children), std::move(uniqueName));
  normalized->setParsedName(expression->getParsedName());
  normalized->setRawNameVariables(expression->getRawNameVariables());
  if (expression->hasAlias()) normalized->setAlias(expression->getAlias());
  return normalized;
}

class BooleanPredicateNormalizer final : public BoundStatementVisitor {
 protected:
  void visitRegularQueryUnsafe(BoundStatement& statement) override {
    auto& query = statement.cast<BoundRegularQuery>();
    for (auto* part : query.getPreQueryPartUnsafe()) visitQueryPartUnsafe(*part);
    BoundStatementVisitor::visitRegularQueryUnsafe(statement);
    if (auto* post = query.getPostSingleQueryUnsafe()) visitSingleQueryUnsafe(*post);
  }
  void visitQueryPartUnsafe(NormalizedQueryPart& part) override {
    for (uint32_t i = 0; i < part.getNumReadingClause(); ++i) {
      auto* clause = part.getReadingClause(i);
      clause->setPredicate(normalizeBooleanPredicate(clause->getPredicate()));
    }
    part.setProjectionBodyPredicate(normalizeBooleanPredicate(part.getProjectionBodyPredicate()));
  }
};
}  // namespace

void BoundStatementRewriter::rewrite(BoundStatement& boundStatement,
                                     main::ClientContext& clientContext, Binder* binder) {
  BoundRewriteTraceQueryScope traceScope(boundStatement, clientContext);
  //   auto withClauseProjectionRewriter = WithClauseProjectionRewriter();
  //   withClauseProjectionRewriter.visitUnsafe(boundStatement);

  auto normalizedQueryPartMatchRewriter =
      NormalizedQueryPartMatchRewriter(&clientContext);
  normalizedQueryPartMatchRewriter.visitUnsafe(boundStatement);

  auto matchClausePatternLabelRewriter =
      MatchClausePatternLabelRewriter(clientContext);
  matchClausePatternLabelRewriter.visitUnsafe(boundStatement);

  auto defaultTypeSolver = DefaultTypeSolver();
  defaultTypeSolver.visit(boundStatement);

  BooleanPredicateNormalizer predicateNormalizer;
  predicateNormalizer.visitUnsafe(boundStatement);

  traceScope.normalized();

  const auto* disableEnv = std::getenv("NEUG_DISABLE_INDEX_REWRITE");
  if (disableEnv != nullptr && std::string_view(disableEnv) == "1") {
    return;
  }

  // Computation prefixes use the same explicit replay cursor as local V4
  // substitutions. Partial counts remain in the independently prepared legacy
  // route until that pass can account for every substitution in the cursor.
  if (binder != nullptr) binder->resetIndexRewriteApplied();
  rewriteComputationPrefixes(boundStatement, clientContext, binder);
  if (binder == nullptr || !binder->hasIndexRewriteReplay()) {
    rewritePartialCounts(boundStatement, clientContext, binder);
  }
  RewriterV4 rewriter(&clientContext, binder);
  const auto* profileEnv = std::getenv("NEUG_PROFILE_INDEX_REWRITE");
  const auto profileIndexRewrite =
      profileEnv != nullptr && std::string_view(profileEnv) == "1";
  // Expose per-anchor DISTINCT counts and filtered OPTIONAL counts to FILL
  // COUNT reads; restore unconsumed normalizations in reverse order.
  const auto distinctPartitions =
      normalizeDistinctCountPartitions(boundStatement, clientContext, binder);
  const auto filteredCounts =
      normalizeFilteredCounts(boundStatement, clientContext, binder);
  if (!profileIndexRewrite) {
    rewriter.visitUnsafe(boundStatement);
    restoreUnusedFilteredCounts(filteredCounts);
    restoreUnusedDistinctCountPartitions(distinctPartitions);
    predicateNormalizer.visitUnsafe(boundStatement);
    return;
  }
  const auto rewriteStarted = std::chrono::steady_clock::now();
  rewriter.visitUnsafe(boundStatement);
  restoreUnusedFilteredCounts(filteredCounts);
  restoreUnusedDistinctCountPartitions(distinctPartitions);
  predicateNormalizer.visitUnsafe(boundStatement);
  const auto elapsedUs = std::chrono::duration<double, std::micro>(
                             std::chrono::steady_clock::now() - rewriteStarted)
                             .count();
  const auto* label = std::getenv("NEUG_INDEX_REWRITE_PROFILE_LABEL");
  LOG(INFO) << "NEUG_INDEX_REWRITE_PROFILE label="
            << (label == nullptr ? "unlabeled" : label)
            << " elapsed_us=" << elapsedUs;
}

}  // namespace binder
}  // namespace neug
