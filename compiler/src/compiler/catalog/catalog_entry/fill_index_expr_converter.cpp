#include "neug/compiler/catalog/catalog_entry/fill_index_expr_converter.h"

#include <utility>
#include <unordered_set>

#include "neug/compiler/binder/expression/case_expression.h"
#include "neug/compiler/binder/expression/expression.h"
#include "neug/compiler/binder/expression/literal_expression.h"
#include "neug/compiler/binder/expression/node_rel_expression.h"
#include "neug/compiler/binder/expression/property_expression.h"
#include "neug/compiler/binder/expression/scalar_function_expression.h"
#include "neug/compiler/binder/expression/variable_expression.h"
#include "neug/compiler/common/types/timestamp_t.h"
#include "neug/compiler/common/constants.h"
#include "neug/compiler/common/enums/expression_type.h"
#include "neug/compiler/common/types/types.h"
#include "neug/generated/proto/plan/expr.pb.h"
#include "neug/utils/exception/exception.h"

namespace neug {
namespace catalog {

namespace {

using common::ExpressionType;
using common::LogicalType;
using common::LogicalTypeID;

// ---------------------------------------------------------------------------
// Type mapping (subset): LogicalType -> common::IrDataType.
// Mirrors gopt::GPhysicalTypeConverter::convertSimpleLogicalType for the node
// kinds that FILL aggregate expressions can legally contain. Anything else
// throws — the AST must never carry a type the execution parser misreads.
// ---------------------------------------------------------------------------
std::unique_ptr<::common::IrDataType> ConvertLogicalType(
    const LogicalType& type) {
  auto result = std::make_unique<::common::IrDataType>();
  switch (type.getLogicalTypeID()) {
  case LogicalTypeID::BOOL:
    result->mutable_data_type()->set_primitive_type(
        ::common::PrimitiveType::DT_BOOL);
    break;
  case LogicalTypeID::INT32:
    result->mutable_data_type()->set_primitive_type(
        ::common::PrimitiveType::DT_SIGNED_INT32);
    break;
  case LogicalTypeID::UINT32:
    result->mutable_data_type()->set_primitive_type(
        ::common::PrimitiveType::DT_UNSIGNED_INT32);
    break;
  case LogicalTypeID::INT64:
    result->mutable_data_type()->set_primitive_type(
        ::common::PrimitiveType::DT_SIGNED_INT64);
    break;
  case LogicalTypeID::UINT64:
    result->mutable_data_type()->set_primitive_type(
        ::common::PrimitiveType::DT_UNSIGNED_INT64);
    break;
  case LogicalTypeID::FLOAT:
    result->mutable_data_type()->set_primitive_type(
        ::common::PrimitiveType::DT_FLOAT);
    break;
  case LogicalTypeID::DOUBLE:
    result->mutable_data_type()->set_primitive_type(
        ::common::PrimitiveType::DT_DOUBLE);
    break;
  case LogicalTypeID::STRING: {
    auto strType = std::make_unique<::common::String>();
    strType->mutable_var_char()->set_max_length(
        LogicalType::getDefaultStringMaxLen());
    result->mutable_data_type()->set_allocated_string(strType.release());
    break;
  }
  case LogicalTypeID::INTERNAL_ID:
    result->mutable_data_type()->set_primitive_type(
        ::common::PrimitiveType::DT_SIGNED_INT64);
    break;
  case LogicalTypeID::TIMESTAMP: {
    auto temporalType = std::make_unique<::common::Temporal>();
    auto datetime = std::make_unique<::common::Temporal_DateTime>();
    datetime->set_date_time_format(
        ::common::Temporal::DateTimeFormat::
            Temporal_DateTimeFormat_DTF_YYYY_MM_DD_HH_MM_SS_SSS);
    datetime->set_time_zone_format(
        ::common::Temporal::TimeZoneFormat::Temporal_TimeZoneFormat_TZF_UTC);
    temporalType->set_allocated_date_time(datetime.release());
    result->mutable_data_type()->set_allocated_temporal(
        temporalType.release());
    break;
  }
  case LogicalTypeID::NODE: {
    auto graphType = std::make_unique<::common::GraphDataType>();
    graphType->set_element_opt(::common::GraphDataType::GraphElementOpt::
                                   GraphDataType_GraphElementOpt_VERTEX);
    result->set_allocated_graph_type(graphType.release());
    break;
  }
  default:
    THROW_BINDER_EXCEPTION("Unsupported data type in FILL expression: " +
                           type.toString());
  }
  return result;
}

// ---------------------------------------------------------------------------
// Literal Value -> proto const (subset; mirrors gopt convertValue).
// ---------------------------------------------------------------------------
std::unique_ptr<::common::Value> ConvertLiteralValue(
    const common::Value& value) {
  auto valuePB = std::make_unique<::common::Value>();
  if (value.isNull()) {
    valuePB->set_allocated_none(new ::common::None());
    return valuePB;
  }
  switch (value.getDataType().getLogicalTypeID()) {
  case LogicalTypeID::BOOL:
    valuePB->set_boolean(value.getValue<bool>());
    break;
  case LogicalTypeID::INT32:
    valuePB->set_i32(value.getValue<int32_t>());
    break;
  case LogicalTypeID::INT64:
    valuePB->set_i64(value.getValue<int64_t>());
    break;
  case LogicalTypeID::UINT32:
    valuePB->set_u32(value.getValue<uint32_t>());
    break;
  case LogicalTypeID::UINT64:
    valuePB->set_u64(value.getValue<uint64_t>());
    break;
  case LogicalTypeID::FLOAT:
    valuePB->set_f32(value.getValue<float>());
    break;
  case LogicalTypeID::DOUBLE:
    valuePB->set_f64(value.getValue<double>());
    break;
  case LogicalTypeID::STRING:
    valuePB->set_str(value.getValue<std::string>());
    break;
  case LogicalTypeID::TIMESTAMP:
    valuePB->set_str(
        common::Timestamp::toString(value.getValue<common::timestamp_t>()));
    break;
  default:
    THROW_BINDER_EXCEPTION("Unsupported literal type in FILL expression: " +
                           value.getDataType().toString());
  }
  return valuePB;
}

std::unique_ptr<::common::Property> MakePropertyPB(const std::string& name) {
  auto propPB = std::make_unique<::common::Property>();
  if (name == common::InternalKeyword::ID) {
    propPB->set_allocated_id(new ::common::IdKey());
  } else if (name == common::InternalKeyword::LABEL) {
    propPB->set_allocated_label(new ::common::LabelKey());
  } else {
    propPB->mutable_key()->set_name(name);
  }
  return propPB;
}

std::unique_ptr<::common::ExprOpr> MakeVarOp(uint32_t tag,
                                             const LogicalType& dataType) {
  auto opr = std::make_unique<::common::ExprOpr>();
  auto var = std::make_unique<::common::Variable>();
  var->mutable_tag()->set_id(tag);
  var->set_allocated_node_type(ConvertLogicalType(dataType).release());
  opr->set_allocated_var(var.release());
  opr->set_allocated_node_type(ConvertLogicalType(dataType).release());
  return opr;
}

std::unique_ptr<::common::ExprOpr> MakePropertyVarOp(
    uint32_t tag, const std::string& propName, const LogicalType& dataType) {
  auto opr = MakeVarOp(tag, dataType);
  opr->mutable_var()->set_allocated_property(
      MakePropertyPB(propName).release());
  return opr;
}

std::unique_ptr<::common::ExprOpr> MakeLabelVarOp(uint32_t tag,
                                                  const LogicalType& dataType) {
  auto opr = MakeVarOp(tag, dataType);
  auto propPB = std::make_unique<::common::Property>();
  propPB->set_allocated_label(new ::common::LabelKey());
  opr->mutable_var()->set_allocated_property(propPB.release());
  return opr;
}

std::unique_ptr<::common::ExprOpr> MakeConstOp(const common::Value& value) {
  auto opr = std::make_unique<::common::ExprOpr>();
  // CAST of a constant timestamp is folded before the FILL AST is stored.
  // Its compiler Value has a textual wire representation, but maintenance
  // must evaluate it as a timestamp, just like the initial source query.
  // Keep the declared type on NULL as well so record evaluation preserves
  // SQL UNKNOWN instead of introducing a string operand.
  if (value.getDataType().getLogicalTypeID() == LogicalTypeID::TIMESTAMP) {
    opr->set_allocated_node_type(
        ConvertLogicalType(value.getDataType()).release());
    if (!value.isNull()) {
      opr->mutable_to_datetime()->set_datetime_str(
          common::Timestamp::toString(value.getValue<common::timestamp_t>()));
      return opr;
    }
  }
  opr->set_allocated_const_(ConvertLiteralValue(value).release());
  return opr;
}

std::unique_ptr<::common::ExprOpr> MakeLogicalOp(::common::Logical logical,
                                                 const LogicalType& dataType) {
  auto opr = std::make_unique<::common::ExprOpr>();
  opr->set_logical(logical);
  opr->set_allocated_node_type(ConvertLogicalType(dataType).release());
  return opr;
}

std::unique_ptr<::common::ExprOpr> MakeArithOp(::common::Arithmetic arith,
                                               const LogicalType& dataType) {
  auto opr = std::make_unique<::common::ExprOpr>();
  opr->set_arith(arith);
  opr->set_allocated_node_type(ConvertLogicalType(dataType).release());
  return opr;
}

std::unique_ptr<::common::ExprOpr> MakeBrace(::common::ExprOpr::Brace brace) {
  auto opr = std::make_unique<::common::ExprOpr>();
  opr->set_brace(brace);
  return opr;
}

std::unique_ptr<::common::ExprOpr> MakeToDatetimeOp(
    const std::string& datetime, const LogicalType& dataType) {
  auto opr = std::make_unique<::common::ExprOpr>();
  auto toDatetime = std::make_unique<::common::ToDatetime>();
  toDatetime->set_datetime_str(datetime);
  opr->set_allocated_to_datetime(toDatetime.release());
  opr->set_allocated_node_type(ConvertLogicalType(dataType).release());
  return opr;
}

class FillIndexExprConverter {
 public:
  explicit FillIndexExprConverter(
      std::function<std::optional<uint32_t>(const std::string&)> resolveVar,
      bool replaceAggregate = false)
      : resolveVar_(std::move(resolveVar)),
        replaceAggregate_(replaceAggregate) {}

  std::shared_ptr<::common::Expression> Convert(
      const binder::Expression& expr) {
    auto result = std::make_shared<::common::Expression>();
    EmitInto(expr, result.get());
    if (replaceAggregate_ && aggregateNames_.size() != 1) {
      THROW_BINDER_EXCEPTION(
          "A finalized FILL expression must contain exactly one aggregate "
          "function.");
    }
    return result;
  }

 private:
  // Resolve a graph variable (raw or unique name) to its pattern node tag.
  // Throws when unresolvable: a stored AST with a dangling variable would
  // misbind at evaluation time, so this must never degrade silently.
  uint32_t ResolveTag(const std::string& rawName,
                      const std::string& uniqueName) {
    if (!rawName.empty()) {
      auto tag = resolveVar_(rawName);
      if (tag.has_value())
        return *tag;
    }
    if (!uniqueName.empty()) {
      auto tag = resolveVar_(uniqueName);
      if (tag.has_value())
        return *tag;
    }
    THROW_BINDER_EXCEPTION("Cannot resolve FILL expression variable '" +
                           (rawName.empty() ? uniqueName : rawName) +
                           "' to a pattern node");
  }

  void EmitInto(const binder::Expression& expr, ::common::Expression* target) {
    switch (expr.expressionType) {
    case ExpressionType::LITERAL: {
      const auto& lit = expr.constCast<binder::LiteralExpression>();
      target->add_operators()->CopyFrom(*MakeConstOp(lit.getValue()));
      break;
    }
    case ExpressionType::PROPERTY: {
      const auto& prop = expr.constCast<binder::PropertyExpression>();
      uint32_t tag =
          ResolveTag(prop.getRawVariableName(), prop.getVariableName());
      // NOTE: prop.getVariableName() is the unique var name; raw name first.
      target->add_operators()->CopyFrom(
          *MakePropertyVarOp(tag, prop.getPropertyName(), prop.getDataType()));
      break;
    }
    case ExpressionType::VARIABLE: {
      const auto& var = expr.constCast<binder::VariableExpression>();
      uint32_t tag = ResolveTag(var.getVariableName(), expr.getUniqueName());
      target->add_operators()->CopyFrom(*MakeVarOp(tag, var.getDataType()));
      break;
    }
    case ExpressionType::PATTERN: {
      const auto& nodeOrRel = expr.constCast<binder::NodeOrRelExpression>();
      if (nodeOrRel.getDataType().getLogicalTypeID() != LogicalTypeID::NODE) {
        THROW_BINDER_EXCEPTION(
            "Rel pattern reference is not supported in a FILL expression");
      }
      uint32_t tag =
          ResolveTag(nodeOrRel.getVariableName(), expr.getUniqueName());
      target->add_operators()->CopyFrom(
          *MakeVarOp(tag, nodeOrRel.getDataType()));
      break;
    }
    case ExpressionType::EQUALS:
    case ExpressionType::NOT_EQUALS:
    case ExpressionType::GREATER_THAN:
    case ExpressionType::GREATER_THAN_EQUALS:
    case ExpressionType::LESS_THAN:
    case ExpressionType::LESS_THAN_EQUALS:
    case ExpressionType::AND:
    case ExpressionType::OR: {
      EmitBinary(expr, target);
      break;
    }
    case ExpressionType::NOT: {
      EmitUnary(::common::Logical::NOT, expr, target);
      break;
    }
    case ExpressionType::IS_NULL: {
      EmitUnary(::common::Logical::ISNULL, expr, target);
      break;
    }
    case ExpressionType::IS_NOT_NULL: {
      // IS NOT NULL => NOT ( ISNULL ( x ) ), mirroring
      // GExprConverter::convertIsNotNull. Fully bracketed.
      auto notOp = MakeLogicalOp(::common::Logical::NOT, expr.getDataType());
      auto isNullOp =
          MakeLogicalOp(::common::Logical::ISNULL, expr.getDataType());
      target->add_operators()->CopyFrom(
          *MakeBrace(::common::ExprOpr::Brace::ExprOpr_Brace_LEFT_BRACE));
      target->add_operators()->CopyFrom(*notOp);
      target->add_operators()->CopyFrom(
          *MakeBrace(::common::ExprOpr::Brace::ExprOpr_Brace_LEFT_BRACE));
      target->add_operators()->CopyFrom(*isNullOp);
      EmitInto(*expr.getChild(0), target);
      target->add_operators()->CopyFrom(
          *MakeBrace(::common::ExprOpr::Brace::ExprOpr_Brace_RIGHT_BRACE));
      target->add_operators()->CopyFrom(
          *MakeBrace(::common::ExprOpr::Brace::ExprOpr_Brace_RIGHT_BRACE));
      break;
    }
    case ExpressionType::CASE_ELSE: {
      const auto& caseExpr = expr.constCast<binder::CaseExpression>();
      if (caseExpr.getNumCaseAlternatives() == 0) {
        THROW_BINDER_EXCEPTION(
            "CASE with no alternatives is not supported in a FILL expression");
      }
      if (!caseExpr.getElseExpression()) {
        THROW_BINDER_EXCEPTION(
            "CASE without ELSE is not supported in a FILL expression");
      }
      auto opr = std::make_unique<::common::ExprOpr>();
      auto casePB = std::make_unique<::common::Case>();
      for (common::idx_t i = 0; i < caseExpr.getNumCaseAlternatives(); ++i) {
        auto* alt = caseExpr.getCaseAlternative(i);
        auto whenThen = casePB->add_when_then_expressions();
        EmitInto(*alt->whenExpression, whenThen->mutable_when_expression());
        EmitInto(*alt->thenExpression,
                 whenThen->mutable_then_result_expression());
      }
      EmitInto(*caseExpr.getElseExpression(),
               casePB->mutable_else_result_expression());
      opr->set_allocated_case_(casePB.release());
      target->add_operators()->CopyFrom(*opr);
      break;
    }
    case ExpressionType::FUNCTION: {
      const auto& funcExpr = expr.constCast<binder::ScalarFunctionExpression>();
      const std::string& funcName = funcExpr.getFunction().name;
      const auto& children = funcExpr.getChildren();
      if (children.size() == 2 &&
          (funcName == "+" || funcName == "-" || funcName == "*" ||
           funcName == "/" || funcName == "%")) {
        EmitArith(funcName, funcExpr, target);
        break;
      }
      if (funcName == "LABEL") {
        // label(n) — bound shape: ScalarFunction("LABEL", [n._ID, label-list]).
        if (children.size() != 2 ||
            children[0]->expressionType != ExpressionType::PROPERTY) {
          THROW_BINDER_EXCEPTION(
              "Unsupported LABEL() shape in FILL expression");
        }
        const auto& idProp =
            children[0]->constCast<binder::PropertyExpression>();
        uint32_t tag =
            ResolveTag(idProp.getRawVariableName(), idProp.getVariableName());
        target->add_operators()->CopyFrom(
            *MakeLabelVarOp(tag, funcExpr.getDataType()));
        break;
      }
      if (funcName == "STRUCT_EXTRACT") {
        // label(var) on a bound variable: STRUCT_EXTRACT(var, "LABEL").
        if (children.size() != 2 ||
            children[1]->expressionType != ExpressionType::LITERAL) {
          THROW_BINDER_EXCEPTION(
              "Unsupported STRUCT_EXTRACT shape in FILL expression");
        }
        const auto& keyLit =
            children[1]->constCast<binder::LiteralExpression>();
        const std::string& key = keyLit.getValue().getValue<std::string>();
        if (key != common::InternalKeyword::LABEL) {
          THROW_BINDER_EXCEPTION(
              "Unsupported STRUCT_EXTRACT key in FILL expression: " + key);
        }
        const auto& child = *children[0];
        if (child.expressionType == ExpressionType::VARIABLE) {
          const auto& var = child.constCast<binder::VariableExpression>();
          uint32_t tag =
              ResolveTag(var.getVariableName(), child.getUniqueName());
          target->add_operators()->CopyFrom(
              *MakeLabelVarOp(tag, funcExpr.getDataType()));
        } else if (child.expressionType == ExpressionType::PATTERN) {
          const auto& node = child.constCast<binder::NodeOrRelExpression>();
          uint32_t tag =
              ResolveTag(node.getVariableName(), child.getUniqueName());
          target->add_operators()->CopyFrom(
              *MakeLabelVarOp(tag, funcExpr.getDataType()));
        } else {
          THROW_BINDER_EXCEPTION(
              "Unsupported STRUCT_EXTRACT first argument in FILL expression");
        }
        break;
      }
      if (funcName == "LABELS") {
        THROW_BINDER_EXCEPTION(
            "LABELS() (multi-label list) comparisons are not supported in "
            "FILL expressions; use label() = '<label>' checks instead");
      }
      if (funcName == "TIMESTAMP" || funcName == "timestamp") {
        if (children.size() != 1) {
          THROW_BINDER_EXCEPTION(
              "TIMESTAMP() in FILL expression expects exactly one argument");
        }
        target->add_operators()->CopyFrom(
            *MakeToDatetimeOp(children[0]->toString(), funcExpr.getDataType()));
        break;
      }
      if (replaceAggregate_ && (funcName == "SQRT" || funcName == "ROUND" ||
                               funcName.starts_with("CAST"))) {
        auto* operation = target->add_operators();
        auto* function = operation->mutable_scalar_func();
        function->set_unique_name(funcExpr.getFunction().signatureName);
        for (const auto& child : children)
          EmitInto(*child, function->add_parameters());
        operation->set_allocated_node_type(ConvertLogicalType(expr.getDataType()).release());
        break;
      }
      THROW_BINDER_EXCEPTION(
          "Unsupported scalar function in FILL expression: " + funcName);
    }
    case ExpressionType::AGGREGATE_FUNCTION:
      if (replaceAggregate_) {
        aggregateNames_.insert(expr.getUniqueName());
        target->add_operators()->CopyFrom(
            *MakeVarOp(kFillAggregateStateTag, expr.getDataType()));
        break;
      }
      THROW_BINDER_EXCEPTION(
          "Nested aggregate functions are not supported in a FILL expression "
          "(e.g. scalar computations embedding SUM/COUNT). Split the "
          "computation into separate aggregate columns.");
    default:
      THROW_BINDER_EXCEPTION(
          "Unsupported expression type in FILL expression: " + expr.toString());
    }
  }

  void EmitBinary(const binder::Expression& expr,
                  ::common::Expression* target) {
    ::common::Logical logical;
    switch (expr.expressionType) {
    case ExpressionType::EQUALS:
      logical = ::common::Logical::EQ;
      break;
    case ExpressionType::NOT_EQUALS:
      logical = ::common::Logical::NE;
      break;
    case ExpressionType::GREATER_THAN:
      logical = ::common::Logical::GT;
      break;
    case ExpressionType::GREATER_THAN_EQUALS:
      logical = ::common::Logical::GE;
      break;
    case ExpressionType::LESS_THAN:
      logical = ::common::Logical::LT;
      break;
    case ExpressionType::LESS_THAN_EQUALS:
      logical = ::common::Logical::LE;
      break;
    case ExpressionType::AND:
      logical = ::common::Logical::AND;
      break;
    case ExpressionType::OR:
      logical = ::common::Logical::OR;
      break;
    default:
      THROW_BINDER_EXCEPTION("Not a binary operator: " + expr.toString());
    }
    target->add_operators()->CopyFrom(
        *MakeBrace(::common::ExprOpr::Brace::ExprOpr_Brace_LEFT_BRACE));
    EmitInto(*expr.getChild(0), target);
    target->add_operators()->CopyFrom(
        *MakeLogicalOp(logical, expr.getDataType()));
    EmitInto(*expr.getChild(1), target);
    target->add_operators()->CopyFrom(
        *MakeBrace(::common::ExprOpr::Brace::ExprOpr_Brace_RIGHT_BRACE));
  }

  void EmitUnary(::common::Logical logical, const binder::Expression& expr,
                 ::common::Expression* target) {
    target->add_operators()->CopyFrom(
        *MakeBrace(::common::ExprOpr::Brace::ExprOpr_Brace_LEFT_BRACE));
    target->add_operators()->CopyFrom(
        *MakeLogicalOp(logical, expr.getDataType()));
    EmitInto(*expr.getChild(0), target);
    target->add_operators()->CopyFrom(
        *MakeBrace(::common::ExprOpr::Brace::ExprOpr_Brace_RIGHT_BRACE));
  }

  void EmitArith(const std::string& funcName,
                 const binder::ScalarFunctionExpression& expr,
                 ::common::Expression* target) {
    ::common::Arithmetic arith;
    if (funcName == "+") {
      arith = ::common::Arithmetic::ADD;
    } else if (funcName == "-") {
      arith = ::common::Arithmetic::SUB;
    } else if (funcName == "*") {
      arith = ::common::Arithmetic::MUL;
    } else if (funcName == "/") {
      arith = ::common::Arithmetic::DIV;
    } else {
      arith = ::common::Arithmetic::MOD;
    }
    target->add_operators()->CopyFrom(
        *MakeBrace(::common::ExprOpr::Brace::ExprOpr_Brace_LEFT_BRACE));
    EmitInto(*expr.getChild(0), target);
    target->add_operators()->CopyFrom(*MakeArithOp(arith, expr.getDataType()));
    EmitInto(*expr.getChild(1), target);
    target->add_operators()->CopyFrom(
        *MakeBrace(::common::ExprOpr::Brace::ExprOpr_Brace_RIGHT_BRACE));
  }

  std::function<std::optional<uint32_t>(const std::string&)> resolveVar_;
  bool replaceAggregate_{false};
  std::unordered_set<std::string> aggregateNames_;
};

}  // namespace

std::shared_ptr<::common::Expression> ConvertFillIndexExprToAst(
    const binder::Expression& expr,
    const std::function<std::optional<uint32_t>(const std::string&)>&
        resolveVar) {
  FillIndexExprConverter converter{resolveVar};
  return converter.Convert(expr);
}

std::shared_ptr<::common::Expression> ConvertFillIndexFinalizerToAst(
    const binder::Expression& expr,
    const std::function<std::optional<uint32_t>(const std::string&)>&
        resolveVar) {
  FillIndexExprConverter converter{resolveVar, true};
  return converter.Convert(expr);
}

}  // namespace catalog
}  // namespace neug
