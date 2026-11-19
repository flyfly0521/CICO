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

#pragma once

#include "expression.h"
#include "neug/compiler/common/string_format.h"
#include "neug/compiler/common/types/value/value.h"

namespace neug {
namespace binder {

class NEUG_API ParameterExpression final : public Expression {
  static constexpr common::ExpressionType expressionType =
      common::ExpressionType::PARAMETER;

 public:
  explicit ParameterExpression(const std::string& parameterName,
                               common::Value value)
      : Expression{expressionType, value.getDataType().copy(),
                   createUniqueName(parameterName)},
        parameterName(parameterName),
        value{std::move(value)} {}

  ParameterExpression(const std::string& parameterName,
                      const common::LogicalType& type)
      : ParameterExpression(parameterName, common::Value::createNullValue(type)) {
    hasValue_ = false;
  }

  void cast(const common::LogicalType& type) override;

  bool hasValue() const { return hasValue_; }
  common::Value getValue() const {
    if (!hasValue_) {
      THROW_BINDER_EXCEPTION(common::stringFormat(
          "Parameter ${} has a type declaration but no compile-time value.",
          parameterName));
    }
    return value;
  }

  std::string getName() const { return parameterName; }

  std::unique_ptr<Expression> copy() const override {
    auto result = std::make_unique<ParameterExpression>(parameterName, value);
    result->hasValue_ = hasValue_;
    result->dataType = dataType.copy();
    result->alias = alias;
    result->setParsedName(this->getParsedName());
    result->setRawNameVariables(rawNameVariables);
    return result;
  }

 private:
  std::string toStringInternal() const override { return "$" + parameterName; }
  static std::string createUniqueName(const std::string& input) {
    return "$" + input;
  }

 private:
  std::string parameterName;
  common::Value value;
  bool hasValue_ = true;
};

}  // namespace binder
}  // namespace neug
