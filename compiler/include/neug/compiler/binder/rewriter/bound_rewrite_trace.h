#pragma once

#include <memory>
#include <string>
#include "neug/utils/api.h"

namespace neug {
namespace main { class ClientContext; }
namespace planner { class LogicalOperator; }
namespace catalog { class IndexCatalogEntry; }
namespace common { struct PatternIR; }
namespace binder {
class BoundStatement;

// Opt-in, thread-local compiler evidence. Normal production compilation does
// not allocate snapshots. Nested FILL-source binds are excluded by depth.
class NEUG_API BoundRewriteTrace {
 public:
  BoundRewriteTrace();
  ~BoundRewriteTrace();
  BoundRewriteTrace(const BoundRewriteTrace&) = delete;
  BoundRewriteTrace& operator=(const BoundRewriteTrace&) = delete;
  std::string toJson() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl;
  friend class BoundRewriteTraceQueryScope;
  friend class BoundRewriteTraceStep;
};

class NEUG_API BoundRewriteTraceQueryScope {
 public:
  BoundRewriteTraceQueryScope(BoundStatement&, main::ClientContext&);
  ~BoundRewriteTraceQueryScope();
  void normalized();
 private:
  BoundRewriteTrace* recorder = nullptr;
  BoundStatement* statement = nullptr;
  main::ClientContext* context = nullptr;
  bool root = false;
};

// Preparation may bind several isolated alternatives. Only the selected
// alternative belongs in a semantic proof of the executed query.
class NEUG_API BoundRewriteTracePause {
 public:
  BoundRewriteTracePause();
  ~BoundRewriteTracePause();
  BoundRewriteTracePause(const BoundRewriteTracePause&) = delete;
  BoundRewriteTracePause& operator=(const BoundRewriteTracePause&) = delete;
 private:
  BoundRewriteTrace* previous = nullptr;
};

class NEUG_API BoundRewriteTraceStep {
 public:
  BoundRewriteTraceStep(const catalog::IndexCatalogEntry&,
                       const common::PatternIR& source,
                       const common::PatternIR& current);
  void commit();
 private:
  BoundRewriteTrace* recorder = nullptr;
  std::string before;
  std::string index;
};

// Pure snapshot; does not install or redirect the active semantic trace.
NEUG_API std::string structuredBoundStatementJson(
    const BoundStatement&, main::ClientContext&);

NEUG_API std::string structuredLogicalPlanJson(
    const planner::LogicalOperator&, main::ClientContext&);
}  // namespace binder
}  // namespace neug
