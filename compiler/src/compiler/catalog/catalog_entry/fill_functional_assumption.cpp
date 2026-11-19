#include "neug/compiler/catalog/catalog_entry/fill_functional_assumption.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string_view>
#include <unordered_set>

#include "rapidjson/document.h"
#include "neug/compiler/binder/expression/property_expression.h"
#include "neug/compiler/binder/expression/rel_expression.h"
#include "neug/compiler/binder/query/query_graph.h"
#include "neug/compiler/catalog/catalog.h"
#include "neug/compiler/catalog/catalog_entry/rel_group_catalog_entry.h"
#include "neug/compiler/catalog/catalog_entry/rel_table_catalog_entry.h"
#include "neug/compiler/main/client_context.h"
#include "neug/utils/exception/exception.h"

namespace neug::catalog {
namespace {

constexpr std::string_view MARKER = "NEUG_ASSUME_FUNCTIONAL_V1";

[[noreturn]] void invalid(const std::string& reason) {
  THROW_BINDER_EXCEPTION("Invalid NEUG_ASSUME_FUNCTIONAL_V1 declaration: " + reason);
}

std::string_view trim(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
    text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
    text.remove_suffix(1);
  return text;
}

// Scan lexical comments, never strings or quoted identifiers. Keeping this
// separate from JSON parsing prevents a string literal containing a plausible
// hint from granting a functional assumption to an unrelated source.
std::optional<std::string> declaration(const std::string& source) {
  std::optional<std::string> result;
  for (size_t i = 0; i < source.size();) {
    const auto ch = source[i];
    if (ch == '\'' || ch == '"' || ch == '`') {
      const auto quote = ch;
      ++i;
      while (i < source.size()) {
        if (source[i] == '\\' && quote != '`') {
          i += std::min<size_t>(2, source.size() - i);
        } else if (source[i] == quote) {
          if (i + 1 < source.size() && source[i + 1] == quote) i += 2;
          else { ++i; break; }
        } else ++i;
      }
      continue;
    }
    if (ch == '/' && i + 1 < source.size() && source[i + 1] == '/') {
      const auto end = source.find_first_of("\r\n", i + 2);
      i = end == std::string::npos ? source.size() : end;
      continue;
    }
    if (ch != '/' || i + 1 >= source.size() || source[i + 1] != '*') {
      ++i;
      continue;
    }
    const auto end = source.find("*/", i + 2);
    if (end == std::string::npos) invalid("unterminated source comment");
    auto comment = trim(std::string_view(source).substr(i + 2, end - i - 2));
    if (comment.starts_with('+')) comment = trim(comment.substr(1));
    if (comment.starts_with("NEUG_ASSUME_FUNCTIONAL")) {
      if (!comment.starts_with(MARKER) ||
          (comment.size() > MARKER.size() &&
           !std::isspace(static_cast<unsigned char>(comment[MARKER.size()]))))
        invalid("unsupported marker/version");
      if (result) invalid("duplicate source declaration");
      result = std::string(trim(comment.substr(MARKER.size())));
    }
    i = end + 2;
  }
  return result;
}

void fields(const rapidjson::Value& value, const std::set<std::string>& expected) {
  if (!value.IsObject()) invalid("expected an object");
  std::set<std::string> actual;
  for (auto it = value.MemberBegin(); it != value.MemberEnd(); ++it) {
    const std::string name(it->name.GetString(), it->name.GetStringLength());
    if (!actual.insert(name).second) invalid("duplicate field " + name);
  }
  if (actual != expected) invalid("unknown or missing fields");
}

std::string name(const rapidjson::Value& value) {
  if (!value.IsString() || value.GetStringLength() == 0)
    invalid("bindings and types must be nonempty strings");
  std::string result(value.GetString(), value.GetStringLength());
  if (result.find('\0') != std::string::npos) invalid("NUL in identifier");
  return result;
}

struct RelationshipAssumption {
  std::set<common::table_id_t> tables;
  bool outgoing = false;
  bool incoming = false;
};

}  // namespace

std::optional<std::string> proveFillFunctionalAssumption(
    const std::string& sourceQuery,
    const binder::QueryGraphCollection& graph,
    const binder::PropertyExpression& anchorKey,
    const TableCatalogEntry& target, main::ClientContext& context) {
  const auto text = declaration(sourceQuery);
  if (!text) return std::nullopt;
  rapidjson::Document document;
  document.Parse(text->data(), text->size());
  if (document.HasParseError()) invalid("invalid JSON");
  fields(document, {"anchor", "relationships"});
  const auto anchor = name(document["anchor"]);
  const auto& relationships = document["relationships"];
  if (!relationships.IsArray() || relationships.Empty())
    invalid("relationships must be a nonempty array");
  if (!anchorKey.isSingleLabel() ||
      anchorKey.getSingleTableID() != target.getTableID() ||
      !anchorKey.isPrimaryKey(target.getTableID()))
    invalid("anchor must be the target table primary key");
  const auto nodes = graph.getQueryNodes();
  const auto rels = graph.getQueryRels();
  bool foundAnchor = false;
  for (const auto& node : nodes) {
    if (node->getEntries().size() != 1)
      invalid("source nodes must have singleton label domains");
    if (node->getUniqueName() == anchorKey.getVariableName()) {
      foundAnchor = node->getVariableName() == anchor;
    }
  }
  if (!foundAnchor) invalid("anchor binding does not match the returned primary key");
  auto* catalog = context.getCatalog();
  const auto* transaction = context.getTransaction();
  std::map<std::string, RelationshipAssumption> assumptions;
  for (const auto& value : relationships.GetArray()) {
    fields(value, {"binding", "types", "at_most_one_outgoing", "at_most_one_incoming"});
    const auto binding = name(value["binding"]);
    if (!value["at_most_one_outgoing"].IsBool() ||
        !value["at_most_one_incoming"].IsBool())
      invalid("multiplicity flags must be booleans");
    RelationshipAssumption assumption;
    assumption.outgoing = value["at_most_one_outgoing"].GetBool();
    assumption.incoming = value["at_most_one_incoming"].GetBool();
    if (!assumption.outgoing && !assumption.incoming)
      invalid("each relationship needs a directional row bound");
    const auto& types = value["types"];
    if (!types.IsArray() || types.Empty()) invalid("types must be a nonempty array");
    std::set<std::string> seen;
    for (const auto& type : types.GetArray()) {
      const auto typeName = name(type);
      if (!seen.insert(typeName).second) invalid("duplicate relationship type");
      if (catalog->containsRelGroup(transaction, typeName)) {
        for (const auto id : catalog->getRelGroupEntry(transaction, typeName)->getRelTableIDs())
          assumption.tables.insert(id);
      } else if (catalog->containsTable(transaction, typeName)) {
        const auto* entry = catalog->getTableCatalogEntry(transaction, typeName);
        if (entry->getType() != CatalogEntryType::REL_TABLE_ENTRY)
          invalid("declared type is not a relationship: " + typeName);
        assumption.tables.insert(entry->getTableID());
      } else invalid("unknown relationship type: " + typeName);
    }
    if (assumption.tables.empty()) invalid("empty declared relationship domain");
    if (!assumptions.emplace(binding, std::move(assumption)).second)
      invalid("duplicate relationship binding");
  }
  if (assumptions.size() != rels.size())
    invalid("every bound source relationship must have exactly one declaration");
  for (const auto& rel : rels) {
    if (rel->isRecursive() || rel->getDirectionType() != binder::RelDirectionType::SINGLE ||
        rel->getEntries().empty()) invalid("only fixed directed relationships are supported");
    const auto found = assumptions.find(rel->getVariableName());
    if (found == assumptions.end()) invalid("undeclared source relationship binding");
    // The assertion bounds the JOINT union, so a type-pruned subset inherits
    // its bound. Separate per-type assertions must never be combined into a
    // bound on R|S: duplicate aliases were rejected above.
    for (const auto* entry : rel->getEntries())
      if (!found->second.tables.contains(entry->getTableID()))
        invalid("declared type union does not cover the bound relationship");
  }
  std::unordered_set<std::string> reached{anchorKey.getVariableName()}, proved;
  bool progress = true;
  while (progress) {
    progress = false;
    for (const auto& rel : rels) {
      if (proved.contains(rel->getUniqueName())) continue;
      const auto& assumption = assumptions.at(rel->getVariableName());
      if ((assumption.outgoing && reached.contains(rel->getSrcNodeName())) ||
          (assumption.incoming && reached.contains(rel->getDstNodeName()))) {
        reached.insert(rel->getSrcNodeName());
        reached.insert(rel->getDstNodeName());
        proved.insert(rel->getUniqueName());
        progress = true;
      }
    }
  }
  if (proved.size() != rels.size() ||
      std::any_of(nodes.begin(), nodes.end(), [&](const auto& node) {
        return !reached.contains(node->getUniqueName());
      })) invalid("directional closure does not prove at most one complete source row per anchor");
  return std::string(MARKER) + ':' + *text;
}

std::optional<std::vector<DeclaredRelationshipMultiplicity>>
parseFillRelationshipDeclarations(const std::string& sourceQuery, main::ClientContext& context) {
  try {
    const auto text = declaration(sourceQuery);
    if (!text) return std::nullopt;
    rapidjson::Document document;
    document.Parse(text->data(), text->size());
    if (document.HasParseError()) return std::nullopt;
    fields(document, {"anchor", "relationships"});
    name(document["anchor"]);
    const auto& relationships = document["relationships"];
    if (!relationships.IsArray() || relationships.Empty()) return std::nullopt;
    auto* catalog = context.getCatalog();
    const auto* transaction = context.getTransaction();
    std::vector<DeclaredRelationshipMultiplicity> result;
    for (const auto& value : relationships.GetArray()) {
      fields(value, {"binding", "types", "at_most_one_outgoing", "at_most_one_incoming"});
      name(value["binding"]);
      if (!value["at_most_one_outgoing"].IsBool() || !value["at_most_one_incoming"].IsBool())
        return std::nullopt;
      DeclaredRelationshipMultiplicity declared;
      declared.atMostOneOutgoing = value["at_most_one_outgoing"].GetBool();
      declared.atMostOneIncoming = value["at_most_one_incoming"].GetBool();
      if (!declared.atMostOneOutgoing && !declared.atMostOneIncoming) return std::nullopt;
      const auto& types = value["types"];
      if (!types.IsArray() || types.Empty()) return std::nullopt;
      std::set<std::string> seen;
      for (const auto& type : types.GetArray()) {
        const auto typeName = name(type);
        if (!seen.insert(typeName).second) return std::nullopt;
        if (catalog->containsRelGroup(transaction, typeName)) {
          for (const auto id : catalog->getRelGroupEntry(transaction, typeName)->getRelTableIDs())
            declared.tables.insert(id);
        } else if (catalog->containsTable(transaction, typeName)) {
          const auto* entry = catalog->getTableCatalogEntry(transaction, typeName);
          if (entry->getType() != CatalogEntryType::REL_TABLE_ENTRY) return std::nullopt;
          declared.tables.insert(entry->getTableID());
        } else return std::nullopt;
      }
      if (declared.tables.empty()) return std::nullopt;
      result.push_back(std::move(declared));
    }
    return result;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

}  // namespace neug::catalog
