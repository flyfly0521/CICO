#include "neug/compiler/binder/expression/fill_predicate_implication.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace neug::binder {
namespace {

using Bound = __int128_t;
constexpr Bound INF = Bound{1} << 120;
constexpr size_t MAX_SYMBOLS = 64;
constexpr size_t MAX_PREMISES = 128;

struct Term {
  std::string symbol;  // empty denotes the constant-zero DBM node
  Bound constant{0};
  int bits{64};
};

struct Comparison {
  Term left;
  Term right;
  ::common::Logical operation;
};

std::optional<Term> parseTerm(const ::common::ExprOpr& op) {
  if (op.has_const_()) {
    if (op.const_().has_i64())
      return Term{"", op.const_().i64(), 64};
    if (op.const_().has_i32())
      return Term{"", op.const_().i32(), 32};
    return std::nullopt;
  }
  if (!op.has_var())
    return std::nullopt;
  const auto& var = op.var();
  if (!var.tag().has_id() || !var.has_property() ||
      !var.node_type().has_data_type() ||
      !var.node_type().data_type().has_primitive_type()) {
    return std::nullopt;
  }
  const auto type = var.node_type().data_type().primitive_type();
  if (type != ::common::DT_SIGNED_INT32 && type != ::common::DT_SIGNED_INT64)
    return std::nullopt;
  return Term{var.SerializeAsString(), 0,
              type == ::common::DT_SIGNED_INT32 ? 32 : 64};
}

std::optional<Comparison> parseComparison(const ::common::Expression& ast) {
  std::vector<const ::common::ExprOpr*> tokens;
  for (const auto& token : ast.operators()) {
    if (!token.has_brace())
      tokens.push_back(&token);
  }
  // Conjuncts have already been split by the binder. Removing redundant
  // parentheses is safe only for this exact atom/operator/atom grammar.
  if (tokens.size() != 3 || !tokens[1]->has_logical())
    return std::nullopt;
  const auto operation = tokens[1]->logical();
  if (operation != ::common::EQ && operation != ::common::NE &&
      operation != ::common::LT && operation != ::common::LE &&
      operation != ::common::GT && operation != ::common::GE) {
    return std::nullopt;
  }
  const auto left = parseTerm(*tokens[0]);
  const auto right = parseTerm(*tokens[2]);
  if (!left || !right)
    return std::nullopt;
  return Comparison{*left, *right, operation};
}

// Difference-bound closure: distance[i][j] bounds xi-xj. __int128 avoids
// overflow when strict inequalities cross INT64_MIN/MAX or paths are added.
class IntegerConstraints {
 public:
  explicit IntegerConstraints(const std::vector<Comparison>& premises,
                              const Comparison& conclusion) {
    symbols_.emplace("", 0);
    auto remember = [&](const Term& term) {
      if (symbols_.contains(term.symbol))
        return;
      symbols_.emplace(term.symbol, symbols_.size());
      widths_.emplace(term.symbol, term.bits);
    };
    for (const auto& premise : premises) {
      remember(premise.left);
      remember(premise.right);
    }
    remember(conclusion.left);
    remember(conclusion.right);
    if (symbols_.size() > MAX_SYMBOLS)
      return;
    distance_.assign(symbols_.size(), std::vector<Bound>(symbols_.size(), INF));
    for (size_t i = 0; i < distance_.size(); ++i)
      distance_[i][i] = 0;
    for (const auto& [symbol, bits] : widths_) {
      const auto i = symbols_.at(symbol);
      distance_[i][0] = (Bound{1} << (bits - 1)) - 1;
      distance_[0][i] = Bound{1} << (bits - 1);
    }
    for (const auto& premise : premises) {
      // TRUE(integer comparison) requires both operands to be non-NULL.
      nonnull_.insert(premise.left.symbol);
      nonnull_.insert(premise.right.symbol);
      constrain(premise);
    }
    nonnull_.insert("");
    for (size_t k = 0; k < distance_.size(); ++k)
      for (size_t i = 0; i < distance_.size(); ++i)
        for (size_t j = 0; j < distance_.size(); ++j) {
          if (distance_[i][k] == INF || distance_[k][j] == INF)
            continue;
          distance_[i][j] =
              std::min(distance_[i][j], distance_[i][k] + distance_[k][j]);
          // A negative cycle proves the (error-free) premises inconsistent.
          if (i == j && distance_[i][j] < 0) {
            inconsistent_ = true;
            return;
          }
        }
  }

  bool proves(const Comparison& c) const {
    if (distance_.empty())
      return false;
    if (inconsistent_)
      return true;
    if (!nonnull_.contains(c.left.symbol) || !nonnull_.contains(c.right.symbol))
      return false;
    switch (c.operation) {
    case ::common::LE:
      return impliesBound(c.left, c.right, 0);
    case ::common::LT:
      return impliesBound(c.left, c.right, -1);
    case ::common::GE:
      return impliesBound(c.right, c.left, 0);
    case ::common::GT:
      return impliesBound(c.right, c.left, -1);
    case ::common::EQ:
      return impliesBound(c.left, c.right, 0) &&
             impliesBound(c.right, c.left, 0);
    case ::common::NE:
      return impliesBound(c.left, c.right, -1) ||
             impliesBound(c.right, c.left, -1);
    default:
      return false;
    }
  }

 private:
  void addBound(const Term& left, const Term& right, Bound strict) {
    auto& current =
        distance_[symbols_.at(left.symbol)][symbols_.at(right.symbol)];
    current = std::min(current, right.constant - left.constant + strict);
  }

  bool impliesBound(const Term& left, const Term& right, Bound strict) const {
    return distance_[symbols_.at(left.symbol)][symbols_.at(right.symbol)] <=
           right.constant - left.constant + strict;
  }

  void constrain(const Comparison& c) {
    switch (c.operation) {
    case ::common::LE:
      addBound(c.left, c.right, 0);
      break;
    case ::common::LT:
      addBound(c.left, c.right, -1);
      break;
    case ::common::GE:
      addBound(c.right, c.left, 0);
      break;
    case ::common::GT:
      addBound(c.right, c.left, -1);
      break;
    case ::common::EQ:
      addBound(c.left, c.right, 0);
      addBound(c.right, c.left, 0);
      break;
    // Disequality is disjunctive. It establishes non-NULL operands but adds
    // no bound; insufficient information remains an unknown result.
    default:
      break;
    }
  }

  std::unordered_map<std::string, size_t> symbols_;
  std::unordered_map<std::string, int> widths_;
  std::unordered_set<std::string> nonnull_;
  std::vector<std::vector<Bound>> distance_;
  bool inconsistent_{false};
};

}  // namespace

bool provesFillIntegerPredicate(
    const std::vector<std::shared_ptr<::common::Expression>>& premises,
    const ::common::Expression& conclusion) {
  if (premises.size() > MAX_PREMISES)
    return false;
  const auto goal = parseComparison(conclusion);
  if (!goal)
    return false;
  std::vector<Comparison> comparisons;
  comparisons.reserve(premises.size());
  for (const auto& premise : premises) {
    if (!premise)
      return false;
    const auto parsed = parseComparison(*premise);
    if (!parsed)
      return false;
    comparisons.push_back(*parsed);
  }
  return IntegerConstraints(comparisons, *goal).proves(*goal);
}

}  // namespace neug::binder
