# CICO

CICO (Cypher In, Cypher Out) is a middleware for graph databases that lets you define, build, and manage indexes and materialized views. It automatically rewrites incoming Cypher queries to use them when applicable and emits Cypher in the target database’s dialect.

```
目标系统方言的 Cypher ──► [CICO] ──► 改写后的 Cypher（同一方言）──► Neo4j / Kuzu / NeuG …
       FILL / DDL      ──► [CICO]：运行时注册索引、维护中间件自己的 schema
```

- **运行时写入索引**：启动后直接发送 `FILL T(col) FROM (...)` 即可注册新索引；也支持 `ALTER TABLE` / `CREATE REL TABLE` / `DROP TABLE`。FILL 的目标列或派生关系表不存在时，中间件根据源查询推断类型并自动补齐，同时返回推断出的 DDL。
- **安全契约**：输出只有两种可能：与输入语义等价的改写，或者原样返回输入。每条改写都会被翻译回内部方言、重新绑定，再与编译器选中的方案做结构比对；只有比对通过、并且确实读取了索引列或派生边，才返回改写后的语句。
- **按 QueryPart 改写**：查询在顶层 `WITH` 处切成 QueryPart，每段单独改写后拼回原文（依据《QueryPart 索引改写的语义等价定理》：单段改写对任意输入 binding 都等价）。`IndexMiddleware(..., scope='query')` 可切回整条语句改写。
- **自带编译器**：引擎建立在 [`compiler/`](compiler/README.md) 中的 Cypher 编译器（binder 与 RewriterV4 等索引改写器）之上，该编译器源自 NeuG，现随本仓库维护，**从本仓库源码直接编译**。引擎只编译、不打开数据库。
- **资源上限**：引擎进程地址空间默认限制为 8 GB（`CICO_ENGINE_MEMORY_GB`）；超限时该请求原样返回，引擎自动重启并重新注册全部 FILL。
- **索引物化**：`build_statements()` / `cico build` 把已注册的 FILL 翻译成目标系统可直接执行的建索引语句（DDL、重置、写回），Kuzu、Neo4j 上的索引也能由中间件自己建出来。
- **多方言**：`neug`、`kuzu`、`neo4j` 三种方言内置；接入新系统见 [docs/dialects.md](docs/dialects.md)。

## 构建

依赖：Linux、GCC ≥ 11（或支持 C++20 的 Clang）、CMake ≥ 3.20、OpenSSL 开发包（`libssl-dev`）、Python ≥ 3.10 与 PyYAML。

```sh
tools/fetch_deps.sh                          # 可选：预先下载并校验固定版本的第三方源码包到 .deps/
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # 加 -G Ninja 更快
cmake --build build -j
pip install pyyaml
```

产物为 `build/bin/cico_engine`；Python 端自动找到它（也可用 `CICO_ENGINE` 指定其他路径）。

- **组成**：`compiler/` 编成 `cypher_compiler`，与 `engine/` 链接为 `cico_engine`；`third_party/` 中的小型库（antlr4、re2、utf8proc 及若干头文件库）随仓库提供；Abseil 20240116.0、Protobuf 29.6、yaml-cpp 0.8.0、glog 0.6.0、Arrow 18.0.0 由 [`cmake/Dependencies.cmake`](cmake/Dependencies.cmake) 按 SHA-256 获取（与 NeuG 使用的版本相同）。首次构建约需数分钟（主要是 Arrow 和 Protobuf）。
- **离线构建**：`tools/fetch_deps.sh` 下载的包放在 `.deps/`（`-DCICO_DEPS_DIR` 可改位置），配置时优先使用；也可用 `-DFETCHCONTENT_SOURCE_DIR_<ABSL|PROTOBUF|YAML_CPP|GLOG>=<源码目录>`、`-DCICO_ARROW_SOURCE_DIR=<arrow 源码>` 或 `-DCICO_USE_SYSTEM_ARROW=ON`。
- **选项**：`-DCICO_OPTIMIZE_FOR_HOST=ON` 使用 `-march=native`（NeuG 的默认值，生成的程序不可移植）。
- **NeuG 执行器（可选，仅测试）**：`tools/neug_exec` 在 NeuG *数据库* 上执行 Cypher，用于把 NeuG 当作目标系统做端到端比对（`tests/e2e_neug.py`）。NeuG 数据库与 Kuzu/Neo4j 一样是外部目标系统，因此它链接外部 NeuG 构建：`-DCICO_BUILD_NEUG_EXEC=ON -DNEUG_SOURCE_DIR=/path/to/IncGQ`。
- **合并上游 NeuG 改动**（一般不需要）：`tools/sync_compiler.py`，见 [compiler/README.md](compiler/README.md)。

## 使用

以下命令在仓库根目录执行；从其他目录导入时设置 `PYTHONPATH=/path/to/CICO`。

### Python

```python
from cico import IndexMiddleware
with IndexMiddleware('schemas/ldbc-bi.yaml', dialect='kuzu', constraints='constraints/ldbc-snb.yaml') as mw:
    mw.execute("FILL PERSON(countryName) FROM (MATCH (c:PLACE)<-[:ISPARTOF]-(:PLACE)"
               "<-[:ISLOCATEDIN]-(p:PERSON) RETURN p.id, c.name)")
    r = mw.execute(query_text, parameter_types={'tag': 'STRING'})   # 也可以传 parameters={...}，自动推断类型
    r.cypher, r.rewritten, r.reason, r.indexes
    mw.drop_index('PERSON(countryName)')
```

`Result.reason` 的取值：

- `rewritten`：已改写并通过验证。`specialized` 非空时，索引只对这组参数值成立：参数已内联为字面量，改写结果只对这组值有效。
- `no_index_selected`：没有匹配到索引，或改写后的语句并未读取任何索引。
- `not_read_only`：写语句，不做改写。
- `unsupported`：选中的方案中有打印器不支持的结构。
- `frontend_unsupported`：源方言里有暂时无法翻译的结构。
- `verification_mismatch` / `verification_bind_failed`：改写结果没有通过验证。
- `engine_error:<stage>`：编译器在该阶段报错。

除 `rewritten` 外，`cypher` 都等于输入原文。FILL 和 DDL 的结果包含 `index_id`、`entries`、`inferred_ddl`。

### 方言、FILL 与索引注册表

启动时指定目标数据库方言：`IndexMiddleware(schema, dialect='kuzu', constraints='constraints/ldbc-snb.yaml')`，CLI 为 `--dialect kuzu --constraints ...`。方言声明负责输入转换、输出打印和语义差异；改写规则所有方言共用，每条规则声明自己需要的钩子和语义前提，满足的自动启用（`profile.rule_status()` 说明原因）。**FILL 一律按 NeuG 语义绑定**，与方言无关。

带 `--registry R.yaml` 时，索引只通过语句入口增删，注册表文件只由中间件写入：

| 语句 | 效果 | `Result.reason` |
| --- | --- | --- |
| `FILL T(c) FROM (<NeuG 查询>)` | 新身份新增；与已有定义等价则不变；同身份不同定义则替换，并按依赖顺序重建它和下游 | `added` / `unchanged` / `changed` + `:active` 或 `:pending`；失败为 `failed:<阶段>` |
| `DROP_INDEX T(c)`（`DROP_INDEX IF EXISTS ...`） | 删除注册表条目并停用；仍被下游读取时拒绝；不删除库里的物理载体 | `removed` / `failed:dependents` / `failed:missing` |
| 其他 DDL | 目标库自己的语句，原样返回 | `target_ddl`（`route=target`） |

每次变更先编译整个注册表（绑定、载体重叠、依赖、物化计划），再对待构建语句做目标库 EXPLAIN 预检，通过后才原子写注册表，随后按依赖顺序构建，回执（`--index-state`）落盘后启用。带 DDL 的 NeuG 索引脚本可一次性导入：`python3 -m cico import ... --registry R.yaml SCRIPT.cypher`（如 `indexes/ldbc-bi-kuzu.cypher`）。

### 命令行

```sh
python3 -m cico rewrite --schema S.yaml --dialect kuzu --index-script index.cypher \
    --query "MATCH ..." --param-types '{"tag":"STRING"}'
python3 -m cico script  --schema S.yaml --dialect neug --file statements.cypher     # DDL、FILL、查询混合脚本
python3 -m cico serve   --schema S.yaml --dialect neo4j --index-script index.cypher --port 8765
python3 -m cico build   --schema S.yaml --dialect kuzu --index-script index.cypher       # 目标方言的建索引语句
python3 -m cico schema  --kuzu /path/to/kuzu-db --reference graph.yaml --output S.yaml  # 从目标库提取 schema
# Kuzu：导入索引脚本并物化，然后以服务方式运行（需要 kuzu Python 包）
python3 -m cico import --schema schemas/ldbc-bi.yaml --dialect kuzu --constraints constraints/ldbc-snb.yaml \
    --registry kuzu-bi.registry.yaml --index-state kuzu-bi.state.json --kuzu-db /path/to/kuzu-bi \
    --materialize-new indexes/ldbc-bi-kuzu.cypher
```

schema 要描述目标系统（同一数据集在不同系统中属性类型可能不同），可用 `cico schema --kuzu` 从目标库提取；Neo4j 无 schema，可使用同一数据集 Kuzu 库的 schema 配合 `profiles/` 中的映射文件。建索引顺序：先执行全部 `ddl`，再执行全部 `reset`，最后执行全部 `fill`。

### HTTP 接口（`cico serve`）

| 请求 | 说明 |
| --- | --- |
| `POST /statement {"cypher", "parameters"?, "parameter_types"?, "index_id"?}` | 执行一条 FILL、DDL 或查询，返回 Result |
| `POST /script {"script"}` | 按分号拆分后依次执行 |
| `POST /index {"id", "fills": [...]}` | 注册多分支索引，全部成功或全部回滚 |
| `POST /index/neo4j {"script", "id"?, "parameters"?}` | 注册 Neo4j 物化索引脚本 |
| `DELETE /index/<id>` | 删除索引 |
| `GET /indexes`、`GET /registry`、`GET /schema`、`GET /health` | 查看已注册索引、注册表、当前 schema、健康状态 |
| `GET /build` | 已注册索引在目标方言中的建索引语句 |

## 移植到新系统

分步指南与可运行示例见 [guide/new-database.md](guide/new-database.md)。

方言由两部分组成：**dialect** 把 Bound IR 打印成目标语法，**frontend** 把目标语法翻译成内部的 NeuG 方言。接入新系统：写 schema 映射文件（schema 相同可省略）→ 在 `cico/dialects/` 中继承 `Dialect` 覆盖不同写法 → 输入语法有差异时写 frontend（参考 `cico/neo4j.py`）→ 在 `cico/profiles.py` 中 `register()`。验证守卫会把新方言的输出翻译回内部方言重新绑定，方言实现中的错误只会导致回退原查询。已处理的方言语义差异（Neo4j 关系唯一性、子类型标签、变长路径 trail/walk、缺失函数等）见 [docs/dialects.md](docs/dialects.md)。

## 目录

```
cico/                           Python 中间件：middleware.py（IndexMiddleware）、printer.py、canonical.py（验证守卫）、
                                materialize.py、registry.py、targets.py、dialects/、neo4j.py、server.py、__main__.py …
schemas/ constraints/ profiles/ 数据集 schema、声明约束、Neo4j 映射
indexes/                        可导入的 NeuG 索引脚本
tests/                          单元测试、语料回归、覆盖率、真实系统验收；baseline 查询的等价改写版本读自实验文档 `<System>/queries/input/`（CICO_QUERY_INPUT=0 关闭）
engine/rewrite_engine.cc        cico_engine：只做编译的改写服务（内存目录、运行时 FILL、选中方案复现，JSON lines 协议）
engine/bound_ir.h               Bound IR v2 序列化（引擎 → Python 打印器）
compiler/                       Cypher 编译器源码（parser、binder、索引改写器、planner）、proto、语法
third_party/                    随仓库提供的小型第三方库
CMakeLists.txt, cmake/          C++ 构建与固定版本依赖
tools/                          fetch_deps.sh（离线依赖）、sync_compiler.py（合并上游 NeuG）、neug_exec/（可选测试执行器）
guide/                          新数据库接入指南与示例
docs/                           方言说明；docs/notes/ 为实现记录、迁移记录与历史验证结果
```

## 测试

```sh
python3 -m unittest discover -s tests -v      # 单元与集成测试（需要已构建的引擎）
# IncGQ-experiment-docs 全部 Kuzu/Neo4j 查询的改写覆盖率（CICO_DOCS 指向实验文档仓库）
python3 tests/workload_coverage.py --workloads hetionet ldbc-bi ldbc-ic lsqb finbench --output cov.json
# 真实系统验证：在 baseline 库副本上用中间件生成的语句建索引，再比较原查询与改写结果
<kuzu-python> tests/target_validation.py --system kuzu --workload ldbc-bi --store <kuzu-bi-baseline> --samples 10
<kuzu-python> tests/kuzu_rules_acceptance.py --output report.json
```

更多验收脚本（Kuzu/Neo4j BI SF1、注册表、约束检查等）及其运行环境见 [docs/notes/middleware-README-kuzu.md](docs/notes/middleware-README-kuzu.md) 与 [docs/notes/](docs/notes/)。

## 许可

CICO 为 MIT 许可（`LICENSE`）。`compiler/` 中的代码源自 NeuG，按 Apache-2.0 发布（`compiler/LICENSE`）；`third_party/` 中各库保留各自的许可文件。
