#include "neug/compiler/binder/expression/index_predicate_key.h"

#include <algorithm>
#include <exception>
#include <set>
#include <string_view>
#include <vector>

#include "neug/compiler/binder/expression/case_expression.h"
#include "neug/compiler/binder/expression/literal_expression.h"
#include "neug/compiler/binder/expression/property_expression.h"
#include "neug/compiler/binder/expression/scalar_function_expression.h"
#include "neug/compiler/common/serializer/buffered_serializer.h"
#include "neug/compiler/common/serializer/serializer.h"

namespace neug::binder {
namespace {
using namespace common;

// Length prefixes keep property names and string/binary literals from being
// interpreted as syntax, even when they contain our delimiters or graph names.
void appendField(std::string& key, const std::string& field) {
  key += std::to_string(field.size()) + ":" + field;
}

std::string literalPayload(const LiteralExpression& literal) {
  if (literal.isNull()) return "null";
  auto buffer = std::make_shared<BufferedSerializer>();
  Serializer serializer(buffer);
  literal.getValue().serialize(serializer);
  const auto data = buffer->getData();
  static constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(data.size * 2);
  for (uint64_t i = 0; i < data.size; ++i) {
    result += hex[data.data[i] >> 4];
    result += hex[data.data[i] & 15];
  }
  return result;
}

struct CanonicalKeyNode {
  std::string key;
  std::string type;
};

// Parse complete length-prefixed fields, never delimiter-looking bytes inside
// names or serialized literals. A failed parse leaves the whole key opaque.
std::optional<std::vector<std::string>> keyFields(std::string_view input) {
  std::vector<std::string> fields;
  while (!input.empty()) {
    size_t digits = 0, length = 0;
    while (digits < input.size() && input[digits] >= '0' && input[digits] <= '9') {
      const auto digit = static_cast<size_t>(input[digits] - '0');
      if (digit > input.size() || length > (input.size() - digit) / 10) return std::nullopt;
      length = length * 10 + digit;
      ++digits;
    }
    if (!digits || digits == input.size() || input[digits] != ':' ||
        (digits > 1 && input[0] == '0')) return std::nullopt;
    input.remove_prefix(digits + 1);
    if (length > input.size()) return std::nullopt;
    fields.emplace_back(input.substr(0, length));
    input.remove_prefix(length);
  }
  return fields;
}

std::optional<CanonicalKeyNode> canonicalKeyNode(std::string_view input, size_t depth) {
  if (depth > 128) return std::nullopt;
  auto parsed = keyFields(input);
  if (!parsed || parsed->size() < 2 || (*parsed)[1].empty()) return std::nullopt;
  auto& fields = *parsed;
  const auto is = [&](ExpressionType kind) {
    return fields[0] == std::to_string(static_cast<uint32_t>(kind));
  };
  size_t childrenBegin = 2;
  if (is(ExpressionType::PROPERTY)) {
    if (fields.size() != 4) return std::nullopt;
    return CanonicalKeyNode{std::string(input), fields[1]};
  }
  if (is(ExpressionType::PATTERN) || is(ExpressionType::VARIABLE) || is(ExpressionType::LITERAL)) {
    if (fields.size() != 3) return std::nullopt;
    if (is(ExpressionType::LITERAL)) {
      if (fields[1] == "BOOL") {
        static const auto truePayload = literalPayload(LiteralExpression(Value(true), "True"));
        static const auto falsePayload = literalPayload(LiteralExpression(Value(false), "False"));
        if (fields[2] != "null" && fields[2] != truePayload && fields[2] != falsePayload)
          return std::nullopt;
      } else if (fields[2] != "null" && (fields[2].empty() || fields[2].size() % 2 != 0 ||
                 !std::all_of(fields[2].begin(), fields[2].end(), [](unsigned char ch) {
                   return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
                 }))) return std::nullopt;
    }
    return CanonicalKeyNode{std::string(input), fields[1]};
  }
  if (is(ExpressionType::FUNCTION)) {
    static const std::set<std::string> supported = {
        "+", "-", "*", "/", "%", "LABEL", "STRUCT_EXTRACT", "TIMESTAMP", "TO_DATE", "TO_INTERVAL", "CAST"};
    if (fields.size() < 4 || !supported.contains(fields[2])) return std::nullopt;
    childrenBegin = 4;
  } else if (is(ExpressionType::CASE_ELSE)) {
    if (fields.size() < 3 || fields.size() % 2 != 1) return std::nullopt;
  } else if (is(ExpressionType::NOT) || is(ExpressionType::IS_NULL) || is(ExpressionType::IS_NOT_NULL)) {
    if (fields.size() != 3 || fields[1] != "BOOL") return std::nullopt;
  } else if (is(ExpressionType::AND) || is(ExpressionType::OR) || is(ExpressionType::XOR) ||
             is(ExpressionType::EQUALS) || is(ExpressionType::NOT_EQUALS) ||
             is(ExpressionType::GREATER_THAN) || is(ExpressionType::GREATER_THAN_EQUALS) ||
             is(ExpressionType::LESS_THAN) || is(ExpressionType::LESS_THAN_EQUALS)) {
    if (fields.size() != 4 || fields[1] != "BOOL") return std::nullopt;
  } else {
    return std::nullopt;
  }
  std::vector<CanonicalKeyNode> children;
  for (size_t i = childrenBegin; i < fields.size(); ++i) {
    auto child = canonicalKeyNode(fields[i], depth + 1);
    if (!child) return std::nullopt;
    children.push_back(std::move(*child));
  }
  if (is(ExpressionType::NOT) || is(ExpressionType::AND) ||
      is(ExpressionType::OR) || is(ExpressionType::XOR)) {
    if (std::any_of(children.begin(), children.end(), [](const auto& child) {
          return child.type != "BOOL";
        })) return std::nullopt;
  }
  if (is(ExpressionType::EQUALS) && children[0].type == "BOOL" && children[1].type == "BOOL") {
    static const auto trueKey = [] {
      std::string key;
      appendField(key, std::to_string(static_cast<uint32_t>(ExpressionType::LITERAL)));
      appendField(key, "BOOL");
      appendField(key, literalPayload(LiteralExpression(Value(true), "True")));
      return key;
    }();
    for (size_t i = 0; i < 2; ++i)
      if (children[i].key == trueKey) return children[1 - i];
  }
  if (is(ExpressionType::EQUALS) || is(ExpressionType::NOT_EQUALS))
    std::sort(children.begin(), children.end(), [](const auto& left, const auto& right) {
      return left.key < right.key;
    });
  std::string key;
  for (size_t i = 0; i < childrenBegin; ++i) appendField(key, fields[i]);
  for (const auto& child : children) appendField(key, child.key);
  return CanonicalKeyNode{std::move(key), fields[1]};
}

std::optional<std::string> structuralKey(
    const Expression& expression, const IndexPredicateBindings& bindings) {
  std::string key;
  appendField(key, std::to_string(static_cast<uint32_t>(expression.expressionType)));
  appendField(key, expression.getDataType().toString());
  switch (expression.expressionType) {
  case ExpressionType::PROPERTY: {
    const auto& property = expression.constCast<PropertyExpression>();
    const auto bound = bindings.find(property.getVariableName());
    if (bound == bindings.end()) return std::nullopt;
    appendField(key, bound->second);
    appendField(key, property.getPropertyName());
    return key;
  }
  case ExpressionType::PATTERN:
  case ExpressionType::VARIABLE: {
    const auto bound = bindings.find(expression.getUniqueName());
    if (bound == bindings.end()) return std::nullopt;
    appendField(key, bound->second);
    return key;
  }
  case ExpressionType::LITERAL:
    appendField(key, literalPayload(expression.constCast<LiteralExpression>()));
    return key;
  case ExpressionType::CASE_ELSE: {
    const auto& conditional = expression.constCast<CaseExpression>();
    for (size_t i = 0; i < conditional.getNumCaseAlternatives(); ++i) {
      const auto* alternative = conditional.getCaseAlternative(i);
      for (const auto& child : {alternative->whenExpression,
                                 alternative->thenExpression}) {
        if (!child) return std::nullopt;
        const auto childKey = structuralKey(*child, bindings);
        if (!childKey) return std::nullopt;
        appendField(key, *childKey);
      }
    }
    if (!conditional.getElseExpression()) return std::nullopt;
    const auto otherwise = structuralKey(*conditional.getElseExpression(), bindings);
    if (!otherwise) return std::nullopt;
    appendField(key, *otherwise);
    return key;
  }
  case ExpressionType::FUNCTION: {
    const auto& function = expression.constCast<ScalarFunctionExpression>().getFunction();
    // These deterministic scalar operations have their complete semantics in
    // their typed arguments/result. The FILL maintenance converter separately
    // verifies that the stored source supports incremental evaluation.
    static const std::set<std::string> supported = {
        "+", "-", "*", "/", "%", "LABEL", "STRUCT_EXTRACT", "TIMESTAMP", "TO_DATE", "TO_INTERVAL", "CAST"};
    if (!supported.contains(function.name)) return std::nullopt;
    appendField(key, function.name);
    appendField(key, function.signatureToString());
    break;
  }
  case ExpressionType::OR:
  case ExpressionType::XOR:
  case ExpressionType::AND:
  case ExpressionType::NOT:
  case ExpressionType::EQUALS:
  case ExpressionType::NOT_EQUALS:
  case ExpressionType::GREATER_THAN:
  case ExpressionType::GREATER_THAN_EQUALS:
  case ExpressionType::LESS_THAN:
  case ExpressionType::LESS_THAN_EQUALS:
  case ExpressionType::IS_NULL:
  case ExpressionType::IS_NOT_NULL:
    break;
  default:
    return std::nullopt;
  }
  std::vector<std::string> children;
  for (const auto& child : expression.getChildren()) {
    if (!child) return std::nullopt;
    const auto childKey = structuralKey(*child, bindings);
    if (!childKey) return std::nullopt;
    children.push_back(*childKey);
  }
  if (expression.expressionType == ExpressionType::EQUALS ||
      expression.expressionType == ExpressionType::NOT_EQUALS) {
    if (children.size() != 2) return std::nullopt;
    std::sort(children.begin(), children.end());
  }
  for (const auto& child : children) appendField(key, child);
  return key;
}
}  // namespace

std::optional<std::string> makeIndexPredicateKey(
    const Expression& expression, const IndexPredicateBindings& bindings) {
  try {
    const auto key = structuralKey(expression, bindings);
    if (!key) return std::nullopt;
    return normalizeIndexPredicateKey("@typed-predicate-v1:" + *key);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::string normalizeIndexPredicateKey(const std::string& key) {
  constexpr std::string_view prefix = "@typed-predicate-v1:";
  if (!key.starts_with(prefix)) return key;
  const auto canonical = canonicalKeyNode(std::string_view(key).substr(prefix.size()), 0);
  return canonical ? std::string(prefix) + canonical->key : key;
}
}  // namespace neug::binder
