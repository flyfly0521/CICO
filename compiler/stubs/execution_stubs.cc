// Execution-layer symbols referenced by the compiler.
//
// cypher_compiler contains the whole compiler but not NeuG's execution
// engine, storage engine or transaction manager.  The compiler references
// exactly three symbols from those layers, all from code that only runs when
// a bound plan is *executed* against a database (table-function bodies and
// DROP INDEX maintenance).  cico_engine never opens a database, so these paths
// are unreachable; they are defined here so the compiler links on its own.
//
//  * match_files_with_pattern: copied verbatim from NeuG's
//    execution/execute/ops/batch/batch_update_utils.cc (POSIX glob helper used
//    by LocalFileSystem::glob).
//  * FillIndexMaintenanceRegistry::Unregister: only called by DROP INDEX when a
//    storage engine owns a maintenance registry; there is none here.
//  * parallel_weighted_shortest_path_procedure: body of a CALL procedure.
//
// The latter two throw if ever reached instead of returning a wrong answer.
#include <glob.h>

#include <filesystem>
#include <string>
#include <vector>

#include "neug/execution/common/operators/retrieve/weighted_shortest_path_procedure.h"
#include "neug/execution/execute/ops/batch/batch_update_utils.h"
#include "neug/transaction/fill_index_maintenance.h"
#include "neug/utils/exception/exception.h"

namespace neug {

bool FillIndexMaintenanceRegistry::Unregister(const std::string& indexName) {
  THROW_NOT_SUPPORTED_EXCEPTION("CICO compile-only NeuG: FILL index maintenance is not available (" +
                      indexName + ")");
}

namespace execution {

neug::result<Context> parallel_weighted_shortest_path_procedure(
    const StorageReadInterface&, Context&&, const function::WeightedShortestPathFuncInput&) {
  THROW_NOT_SUPPORTED_EXCEPTION(
      "CICO compile-only NeuG: procedure execution is not available");
}

namespace ops {

std::vector<std::string> match_files_with_pattern(const std::string& file_path) {
  std::vector<std::string> result;
  if (file_path.find('*') != std::string::npos ||
      file_path.find('?') != std::string::npos) {
    glob_t glob_result;
    int flags = GLOB_TILDE | GLOB_MARK;
    int ret = glob(file_path.c_str(), flags, nullptr, &glob_result);
    if (ret == 0) {
      for (size_t i = 0; i < glob_result.gl_pathc; ++i) {
        result.push_back(glob_result.gl_pathv[i]);
      }
    }
    globfree(&glob_result);
  } else {
    std::filesystem::path p = std::filesystem::absolute(file_path);
    if (!std::filesystem::exists(p)) {
      THROW_IO_EXCEPTION("Provided path is not a file: " + file_path + ".");
    }
    result.emplace_back(file_path);
  }
  return result;
}

}  // namespace ops
}  // namespace execution
}  // namespace neug
