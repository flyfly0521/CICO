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

#include "neug/compiler/binder/expression/scalar_function_expression.h"
#include "neug/compiler/binder/expression_binder.h"

using namespace neug::common;
using namespace neug::parser;
using namespace neug::function;

namespace neug {
namespace binder {

std::shared_ptr<Expression> ExpressionBinder::bindNullOperatorExpression(
    const ParsedExpression& parsedExpression) {
  expression_vector children;
  for (auto i = 0u; i < parsedExpression.getNumChildren(); ++i) {
    children.push_back(bindExpression(*parsedExpression.getChild(i)));
  }
  return bindNullOperatorExpression(parsedExpression.getExpressionType(),
                                    children);
}

std::shared_ptr<Expression> ExpressionBinder::bindNullOperatorExpression(
    ExpressionType expressionType, const expression_vector& children) {
  expression_vector childrenAfterCast;
  std::vector<LogicalTypeID> inputTypeIDs;
  for (auto& child : children) {
    inputTypeIDs.push_back(child->getDataType().getLogicalTypeID());
    if (child->dataType.getLogicalTypeID() == LogicalTypeID::ANY) {
      childrenAfterCast.push_back(
          implicitCastIfNecessary(child, LogicalType::BOOL()));
    } else {
      childrenAfterCast.push_back(child);
    }
  }
  auto functionName = ExpressionTypeUtil::toString(expressionType);
  // Constant folding uses the vector evaluator, independently of the runtime
  // expression converter. A missing executor silently left its BOOL result at
  // the default false, even for CAST(NULL, type) IS NULL. Null tests must read
  // the operand's null mask and always produce a non-null Boolean result.
  const bool testNull = expressionType == ExpressionType::IS_NULL;
  function::scalar_func_exec_t execFunc =
      [testNull](const std::vector<std::shared_ptr<ValueVector>>& parameters,
                 const std::vector<SelectionVector*>& parameterSelections,
                 ValueVector& result, SelectionVector* resultSelection, void*) {
        NEUG_ASSERT(parameters.size() == 1 && parameterSelections.size() == 1);
        const auto& operand = *parameters[0];
        const auto& selected = *parameterSelections[0];
        for (sel_t i = 0; i < resultSelection->getSelSize(); ++i) {
          const auto sourcePos = selected[operand.state->isFlat() ? 0 : i];
          const auto resultPos = (*resultSelection)[i];
          result.setNull(resultPos, false);
          result.setValue<bool>(resultPos, operand.isNull(sourcePos) == testNull);
        }
      };
  function::scalar_func_select_t selectFunc = nullptr;
  auto bindData =
      std::make_unique<function::FunctionBindData>(LogicalType::BOOL());
  auto uniqueExpressionName =
      ScalarFunctionExpression::getUniqueName(functionName, childrenAfterCast);
  auto func = std::make_unique<ScalarFunction>(
      functionName, inputTypeIDs, LogicalTypeID::BOOL, execFunc, selectFunc);
  return make_shared<ScalarFunctionExpression>(
      expressionType, std::move(func), std::move(bindData),
      std::move(childrenAfterCast), uniqueExpressionName);
}

}  // namespace binder
}  // namespace neug
