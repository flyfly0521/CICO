# compiler/：Cypher 编译器

改写引擎（`engine/rewrite_engine.cc`）建立在这个 Cypher 编译器之上：parser、binder、索引改写器（`src/compiler/binder/rewriter/`，其中 RewriterV4 是主要入口）、planner 和 gopt。代码最初来自 NeuG（IncGQ），现在作为 CICO 的一部分维护，直接在这里修改即可。

## 目录

| 路径 | 内容 |
| --- | --- |
| `include/neug/…`、`src/…` | 编译器源码。C++ 命名空间与头文件前缀沿用 `neug`，与来源保持一致，便于对照上游 |
| `proto/` | 计划/响应 protobuf 定义；构建时用源码编出的 protoc 生成代码 |
| `grammar/` | `Cypher.g4` 语法与关键字表（`third_party/antlr4_cypher` 中的解析器由它生成） |
| `generated/` | 构建期常量头文件（`config.h`、`system_config.h` 等）与空的扩展加载器 |
| `stubs/execution_stubs.cc` | 编译器引用的三个执行层符号（见下文） |
| `upstream/` | 与上游同步用的元数据：`sources.txt`（参与编译的 430 个翻译单元）、`patches/`（CICO 对上游代码的修改，已应用）、`provenance.json`（来源提交与文件哈希） |
| `CMakeLists.txt` | 目标 `cypher_compiler`（object library，整体链接进 `cico_engine`） |

常用入口：

- `src/compiler/main/client_context.cpp`：`ClientContext::prepare`，引擎调用的编译入口（绑定 → 改写 → 规划）；
- `src/compiler/binder/rewriter/rewriter_v4.cpp`：索引改写主流程；
- `src/compiler/binder/bind/copy/bind_fill.cpp`：`FILL` 索引定义的绑定；
- `src/compiler/catalog/`、`src/compiler/main/metadata_manager.cpp`：内存目录与 schema 加载。

## 范围

只包含编译器及其直接依赖（utils、protobuf 生成代码、值/列类型、CSV 读写器等），不包含 NeuG 的执行引擎、存储引擎和事务层。范围由链接器确定：把 NeuG 全部 544 个目标文件静态链接进引擎，从链接映射追踪每个非编译器目标被引入的原因，发现编译器只在三处引用执行期代码，且都只在**执行**计划时运行。`upstream/sources.txt` 因此是编译器的全部 376 个目标，加上截断这三处后仍需要的 54 个支持目标；头文件取它们的依赖文件（`*.o.d`）闭包。

`stubs/execution_stubs.cc` 定义这三个符号：`match_files_with_pattern`（原样复制的 glob 帮助函数）、`FillIndexMaintenanceRegistry::Unregister`（只在有存储引擎时由 `DROP INDEX` 调用）、`parallel_weighted_shortest_path_procedure`（`CALL` 过程执行体）。后两者若被调用会抛出 `NOT_SUPPORTED` 异常。`cico_engine` 从不打开数据库，这些路径不可达。

## 修改与同步上游

- 日常开发直接修改这里的源码。
- 需要合并上游 NeuG 的新改动时：先把本地修改整理成补丁放入 `upstream/patches/` 并加入 `series`，然后运行
  `python3 tools/sync_compiler.py --neug-source /path/to/IncGQ --clean`（需要一个已构建的 NeuG）。工具重新复制源码和头文件闭包、按顺序应用补丁（上下文不匹配即失败），并更新 `provenance.json`；上游新增的编译器源文件需先加入 `sources.txt`。
- 当前代码来自 IncGQ `develop` 提交 `c88ca1f1` 及其工作区改动（与当时引擎链接的 `libneug.so` 一致），并应用了 `upstream/patches/` 中的四个补丁。

许可：Apache-2.0（`LICENSE`）；该编译器的部分代码源自 Kùzu（MIT）。
