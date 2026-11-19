# IncGQ 索引中间件（Cypher in → Cypher out）

把 IncGQ 的 FILL 索引改写能力做成一个可移植的中间件：

```
目标系统方言的 Cypher ──► [中间件] ──► 改写后的 Cypher（同一方言）──► Neo4j / Kuzu / NeuG …
       FILL / DDL      ──► [中间件]：运行时注册索引、维护中间件自己的 schema
```

- **运行时写入索引**：中间件启动后，直接发送 `FILL T(col) FROM (...)` 即可注册新索引；也支持 `ALTER TABLE` / `CREATE REL TABLE` / `DROP TABLE`。如果 FILL 的目标列或派生关系表还不存在，中间件会根据源查询推断出类型并自动补齐，同时把推断出的 DDL 返回，供目标系统建索引时使用。
- **安全契约**：输出只有两种可能：与输入语义等价的改写，或者原样返回输入。每一条改写都会被翻译回内部方言、重新绑定，再与 NeuG 选中的方案做结构比对；只有比对通过，并且确实读取了索引列或派生边，才会返回改写后的语句。
- **按 QueryPart 改写**：查询在顶层 `WITH` 处切成 QueryPart，每段单独改写后拼回原文（依据 wiki《QueryPart 索引改写的语义等价定理》：单段改写对任意输入 binding 都等价）。某一段用了 NeuG 不支持的语法，不影响其他段。`IndexMiddleware(..., scope='query')` 可切回整条语句改写。
- **改写逻辑不重复实现**：引擎部分直接复用 NeuG 编译器（binder 和 RewriterV4），不打开数据库，也不修改 NeuG 内核。默认不提供统计信息，因此不做代价比较。
- **资源上限**：引擎进程的地址空间默认限制在 8 GB（`CICO_ENGINE_MEMORY_GB`）；个别查询会让 NeuG 规划器无限增长内存，超限时该请求原样返回，引擎自动重启并重新注册全部 FILL。
- **索引物化**：`build_statements()` / `cico build` 把每个已注册的 FILL 翻译成目标系统可直接执行的建索引语句（DDL、重置、写回），因此 Kuzu、Neo4j 上的索引也能由中间件自己建出来。
- **已在真实系统上验证**：IncGQ-experiment-docs 中被改写的 Kuzu/Neo4j 查询，在 Kuzu 0.10 和 Neo4j 5.26 的真实库上（用中间件生成的语句建好索引），改写前后结果全部一致（Hetionet、BI SF1、LSQB SF1，共 116 次比对）。

完整的目标、思路、设计说明、实现进度和已知问题见 [`../implementation/middleware/README.md`](../implementation/middleware/README.md)。

**位置**：本仓库是 `flyfly0521/kuzu` 的克隆（`/mnt/sdb1/feiyang/kuzu`，分支 `develop`）。中间件于 2026-11-18 从 Neo4j 仓库（`/mnt/sdb1/feiyang/neo4j`，提交 `2df42a93`）的 `middleware/` 迁入，用于 Kuzu；Neo4j 仓库中的副本继续服务 Neo4j。两份代码相同（`neo4j` profile 在这里仍然可用），Kuzu 专有部分见下一节和 [Kuzu 迁移记录](../implementation/middleware/kuzu-migration.md)。改写引擎仍然链接 IncGQ（NeuG）的 `build/src/libneug.so`，通过 `INCGQ_ROOT` 找到；未设置时默认查找与本仓库同级的 `IncGQ/`。

## 方言参数、FILL 与索引注册表（2026-11-18）

**方言参数。** 中间件启动时指定目标数据库的方言：`IndexMiddleware(schema, dialect='kuzu', constraints='constraints/ldbc-snb.yaml')`，CLI `--dialect kuzu --constraints ...`（`--profile` 仍可用）。它选中一份方言转换声明（输入转换、输出打印、语义差异）；改写规则是所有方言共用的实现，每条规则声明自己需要的钩子和语义前提，满足的就自动启用（`profile.rule_status()` 说明原因）；普通查询按该方言读入、改写结果按该方言输出。为新数据库编写声明见 [docs/dialects.md](docs/dialects.md)。声明约束按数据集写在 `constraints/<dataset>.yaml`（内部表名），所有方言共用。

**FILL 一律是 NeuG 语义**，与方言无关：FILL 后面的查询按 NeuG 绑定（例如 Neo4j 方言下也不加关系唯一性）。

**索引注册表。** 带 `--registry R.yaml` 时，索引只通过语句入口增删，注册表文件只由中间件写入：

| 语句 | 效果 | `Result.reason` |
| --- | --- | --- |
| `FILL T(c) FROM (<NeuG 查询>)` | 新身份新增；与已有定义等价则不变；同身份不同定义则替换，并按依赖顺序重建它和下游 | `added` / `unchanged` / `changed` + `:active` 或 `:pending`；失败为 `failed:<阶段>` |
| `DROP_INDEX T(c)`（`DROP_INDEX IF EXISTS ...`） | 删除注册表条目并停用；仍被下游读取时拒绝；不删除库里的物理载体 | `removed` / `failed:dependents` / `failed:missing` |
| 其他 DDL | 目标库自己的语句，原样返回 | `target_ddl`（`route=target`） |

- 定义身份是载体：表 + 列。注册表（YAML）每条记录规范化后的 NeuG FILL、载体声明（推断出的或导入脚本声明的）、依赖、原始输入和时间；示例见 `cico/registry.py` 文档字符串。
- 每次变更先编译整个注册表（绑定、载体重叠、依赖、物化计划），再对待构建语句做目标库 EXPLAIN 预检，通过后才原子写注册表；随后停用待重建项，按依赖顺序构建，回执落盘后启用。失败阶段在 `Result.reason`（`failed:bind|register|plan|dependencies|validate|build|...`）。构建中途失败时定义保留在注册表里，失败项和下游保持停用，`POST /registry/build` 或重启重试。
- 注册表是逻辑定义，可供多个目标共用；哪些已在某个物理库上建好记在该库的回执 `--index-state`（键为目标身份）。
- 实验文档里带 DDL 的 NeuG 脚本用一次性导入：`python3 -m cico import ... --registry R.yaml SCRIPT.cypher`，脚本中为载体声明的 DDL（如 `DEFAULT 0`）会记入对应条目；`DROP TABLE` 不导入。`indexes/ldbc-bi-kuzu.cypher`、`indexes/ldbc-bi-native.cypher` 是这类导入源。
- 没有 `--registry` 时，FILL 和 `DROP_INDEX` 只作用于内存中的目录（原行为）。

## Kuzu 上使用（2026-11-18）

Kuzu 是嵌入式数据库：中间件输出改写后的 Kuzu Cypher，由调用方用自己的 Kuzu 连接执行；中间件只在**预检/物化期间**打开目标库，结束即释放（Kuzu 同一时刻只允许一个读写进程）。需要 `kuzu` Python 包（BI/IC/LSQB 库为 0.10.0 格式，使用 `/mnt/sdb1/dwk/imported_data/kuzu/tools/kuzu-0.10.0-venv/bin/python`，其中已有 PyYAML）。

```sh
# 仓库根目录；KPY 为带 kuzu 的 Python
KPY=/mnt/sdb1/dwk/imported_data/kuzu/tools/kuzu-0.10.0-venv/bin/python
COMMON="--schema middleware/schemas/ldbc-bi.yaml --dialect kuzu --constraints middleware/constraints/ldbc-snb.yaml \
  --registry /path/kuzu-bi.registry.yaml --index-state /path/kuzu-bi.state.json --kuzu-db /path/to/kuzu-bi"
# 1) 一次性导入实验文档的 NeuG 索引脚本并物化
PYTHONPATH=middleware $KPY -m cico import $COMMON --materialize-new middleware/indexes/ldbc-bi-kuzu.cypher
# 2) 服务：之后的索引变更都通过语句入口
PYTHONPATH=middleware $KPY -m cico serve $COMMON --materialize-new --port 8765
curl -s localhost:8765/statement -d '{"cypher": "FILL PERSON(x) FROM (MATCH ... RETURN p.id, count(*))"}'
curl -s localhost:8765/statement -d '{"cypher": "DROP_INDEX PERSON(x)"}'
curl -s localhost:8765/registry
```

- **目标身份**：`--kuzu-db` 解析后的路径 + 目录的 device/inode。复制或恢复出的库是另一个物理代，需要独立的回执文件（混用会被拒绝）。
- **预检**（`cico/targets.py` 的 `KuzuIndexExecutor.validate`）：Kuzu 有 schema，写入新载体的语句要在 DDL 之后才能绑定。预检在一个手动事务里依次执行 DDL、对其余语句 `EXPLAIN`，最后 `ROLLBACK`，不留任何改动。预检还检查 `IF NOT EXISTS` 会静默保留的旧载体：同名列类型不同、同名关系表端点或声明属性不同，都会在首条物理写入前报错（例如 `DROP_INDEX` 后用另一种类型重新 FILL 同一列），要求先手工删除旧载体。
- **资源**：`--kuzu-threads`（默认 4）、`--kuzu-buffer-pool-gb`（默认 8；0 为 Kuzu 默认，即占用大部分内存）。
- **验收**：`tests/kuzu_registry_acceptance.py`（私有临时库上经语句入口的 FILL/DROP_INDEX，10 项）、`tests/kuzu_rules_acceptance.py`（私有临时库上逐条规则对比改写前后结果，5 项）、`tests/test_kuzu_target.py`（执行器与 CLI）、`tests/kuzu_bi_acceptance.py`（NeuG BI 定义 → Kuzu 计划、EXPLAIN、改写命中，可选在库副本上物化）。结果见 [Kuzu 迁移记录](../implementation/middleware/kuzu-migration.md)。

### Kuzu BI SF1 本机入口（2026-11-18）

从 `/mnt/sdb1/feiyang/datasets/bi-sf1-composite-projected-fk` 的原始 CSV，用实验文档的导入脚本重新导入（Kuzu 0.10.0，35 秒，8 个点表、16 个边表，计数与文档逐项一致）：

```sh
R=/mnt/sdb1/feiyang/kuzu/middleware/runtime/bi_sf1      # 已被 middleware/.gitignore 忽略
cd /mnt/sdb1/feiyang/IncGQ-experiment-docs
$KPY Kuzu/scripts/import/import_ldbc.py bi --source-dir /mnt/sdb1/feiyang/datasets/bi-sf1-composite-projected-fk \
  --target $R/kuzu-db --report $R/import-report.json --threads 8 --buffer-pool-size $((16<<30))
```

- `$R/kuzu-db`：干净的 baseline 库，不在上面建索引；从中提取的 schema（`$R/ldbc-bi.extracted.yaml`）与 `schemas/ldbc-bi.yaml` 完全相同。
- `$R/work/kuzu-bi`：验收用的副本，已由受管目录物化 `indexes/ldbc-bi-kuzu.cypher` 的 8 个定义，回执在 `$R/work/build-state.json`。

```sh
# 仓库根目录；只验证物化语句可执行、改写输出可 EXPLAIN，不执行 BI 查询
$KPY middleware/tests/kuzu_bi_acceptance.py --store $R/kuzu-db --workdir $R/work --output $R/work/report.json \
  --build middleware/indexes/ldbc-bi-kuzu.cypher --samples 10      # 加 --compare-results 才实际执行并比对结果
```

以下“使用”中 Neo4j 专有的段落（BI SF1 本机入口、`runtime/bi_sf1`、`register_neo4j_index` 验收等）描述的是 Neo4j 仓库中的运行环境，本仓库没有复制 `runtime/`。

## 构建

```sh
# 前提：IncGQ 的 build/ 中已有 libneug.so；本中间件只链接它，不会触发 NeuG 重编译
# 以下命令在本仓库根目录执行；IncGQ 不在同级时设置 INCGQ_ROOT
cmake -S middleware/engine -B middleware/engine/build -DCICO_BUNDLED_PATCH=ON
cmake --build middleware/engine/build --target incgq_rewrite_engine_patched cico_exec_patched -j 2
pip install pyyaml        # Python 侧只依赖 PyYAML
```

`CICO_BUNDLED_PATCH=ON` 用 `prepare_patch.py` 在 IncGQ **当前源码**上应用仓库补丁，在本地构建目录生成修复版，保留当前源码已有的更新。补丁上下文不兼容时明确失败，不退回历史版本；生成文件旁的 `patched/*.json` 保存输入、输出和补丁的来源哈希。当前覆盖 RewriterV4、谓词键和完整分组证明三个编译单元。需要 `patch` 和 Python 3，不依赖历史 Git 对象，也不修改 IncGQ 工作区。头文件与 `libneug.so` 仍须 ABI 兼容。

仅构建原版可使用独立目录并设置 `-DCICO_BUNDLED_PATCH=OFF`；自定义 NeuG 路径可传 `-DNEUG_SOURCE_DIR=/path/to/IncGQ -DNEUG_BUILD_DIR=/path/to/build`。

本仓库迁入时没有复制 Neo4j 仓库的 `engine/bin/`，而是在 `engine/build/` 独立重新编译了补丁版程序；`engine/bin/` 若存在只用于本机，已排除在 Git 源码文件之外。不设 `CICO_ENGINE` 时，查找顺序为 **build/patched → bin/patched → build/原版 → bin/原版**，避免一次原版编译使现有修复失效；`CICO_EXEC` 的执行器采用相同规则。显式设置这两个变量可选择具体程序，包括相对路径。构建目录与 Python 缓存也由 `middleware/.gitignore` 排除。

测试 NeuG 内核补丁时不必重编 `libneug.so`：加上 `-DCICO_INTERPOSE_SOURCES=<补丁后的 .cpp>`，会额外生成 `incgq_rewrite_engine_patched` 和 `cico_exec_patched`；再用环境变量 `CICO_ENGINE` / `CICO_EXEC` 指定它们即可。要求头文件与 `build/` 编译时一致。

## 使用

**Neo4j 仓库中的 FILL 文件注册（历史）**：Neo4j 仓库曾用 `--fill-file` 加载可编辑的 NeuG FILL 文件（[文件注册与重载](../implementation/middleware/neo4j-fill-file-reload.md)）。本仓库已改为上面的索引注册表：索引只通过 FILL / `DROP_INDEX` 语句增删，`--fill-file`、`/index-files/reload` 不再存在；旧文件可用 `cico import` 导入。指纹算法未变，同一定义在同一物理库上的回执可以继续使用。

以下 Python、CLI 和测试命令均在 `middleware/` 下执行，先从仓库根目录运行 `cd middleware`。若需要从其他目录导入，设置 `PYTHONPATH=/绝对路径/kuzu/middleware`；无需修改 Kuzu 自身的 CMake 工程。

### Python

```python
from cico import IndexMiddleware
with IndexMiddleware('graph.yaml', stats='statistics.json', profile='neo4j') as mw:
    mw.execute("FILL Person(countryName) FROM (MATCH (c:Country)<-[:IS_PART_OF]-(:City)"
               "<-[:IS_LOCATED_IN]-(p:Person) RETURN p.id, c.name)")
    r = mw.execute(query_text, parameter_types={'tag': 'STRING'})   # 也可以传 parameters={...}，自动推断类型
    r.cypher, r.rewritten, r.reason, r.indexes
    mw.drop_index('PERSON(countryName)')
```

`Result.reason` 的取值：

- `rewritten`：已改写，并通过验证。如果 `specialized` 非空，说明索引只对这组参数值成立：中间件已把这些参数内联为字面量，改写结果也只对这组值有效。
- `no_index_selected`：没有匹配到索引，或代价比较后保留原查询，或改写后的语句并未读取任何索引。
- `not_read_only`：输入是写语句，不做改写。
- `unsupported`：选中的方案中有打印器不支持的结构。
- `frontend_unsupported`：源方言里有暂时无法翻译的结构。
- `verification_mismatch` / `verification_bind_failed`：改写结果没有通过验证。
- `engine_error:<stage>`：NeuG 编译器在该阶段报错。

除 `rewritten` 外，`cypher` 字段都等于输入原文。FILL 和 DDL 返回的结果中包含 `index_id`、`entries`、`inferred_ddl`。

### 命令行

```sh
python3 -m cico rewrite --schema S.yaml --profile kuzu --index-script index.cypher \
    --query "MATCH ..." --param-types '{"tag":"STRING"}'
python3 -m cico script  --schema S.yaml --profile neug --file statements.cypher     # DDL、FILL、查询混合脚本
python3 -m cico serve   --schema S.yaml --profile neo4j --index-script index.cypher --port 8765
python3 -m cico build   --schema S.yaml --profile kuzu --index-script index.cypher       # 目标方言的建索引语句
python3 -m cico schema  --kuzu /path/to/kuzu-db --reference graph.yaml --output S.yaml  # 从目标库提取 schema
```

**schema 要描述目标系统。** 同一数据集在不同系统中的属性类型可能不同（例如 IC 的 `creationDate`：NeuG 库存 INT64，Kuzu/Neo4j 存 TIMESTAMP），所以 schema 要用 `cico schema --kuzu` 从目标库提取。Neo4j 是无 schema 的，可以使用同一数据集 Kuzu 库的 schema 配合 mapping 文件。

**建索引的顺序**：先执行全部 `ddl`，再执行全部 `reset`，最后执行全部 `fill`。重置会把载体值恢复为 DDL 中的 DEFAULT（派生边会被删除），因此可以重复构建。Neo4j 上的写入都包在 `CALL {...} IN TRANSACTIONS` 中，需要在自动提交的会话里执行。

### HTTP 接口（与语言无关，`cico serve`）

| 请求 | 说明 |
| --- | --- |
| `POST /statement {"cypher", "parameters"?, "parameter_types"?, "index_id"?}` | 执行一条 FILL、DDL 或查询，返回 Result |
| `POST /script {"script"}` | 按分号拆分后依次执行 |
| `POST /index {"id", "fills": [...]}` | 注册多分支索引，全部成功或全部回滚 |
| `DELETE /index/<id>` | 删除索引 |
| `GET /indexes`、`GET /schema`、`GET /health` | 查看已注册索引、当前 schema、健康状态 |
| `GET /build` | 已注册索引在目标方言中的建索引语句 |

## 移植到新系统（profile）

profile 由两部分组成：

- **dialect**：负责输出，把 Bound IR 打印成目标系统的语法。
- **frontend**：负责输入，把目标系统的语法翻译成内部的 NeuG 方言。

内置三个 profile：

| profile | 输入与输出 | schema 映射 |
| --- | --- | --- |
| `neug` | NeuG Cypher | 不需要 |
| `kuzu` | Kuzu Cypher（与 NeuG 同属一个语法家族） | 不需要 |
| `neo4j` | Neo4j Cypher | 每个数据集一个 mapping 文件：[`ldbc-snb-neo4j.yaml`](profiles/ldbc-snb-neo4j.yaml)（BI/IC：带类型的标签 `Country` ↔ `PLACE.type='Country'`，`HAS_CREATOR` ↔ `HASCREATOR`）、[`lsqb-neo4j.yaml`](profiles/lsqb-neo4j.yaml)（`IS_PART_OF` ↔ `City_isPartOf_Country`…）、[`finbench-neo4j.yaml`](profiles/finbench-neo4j.yaml)（`transfer` ↔ `AccountTransferAccount`…）、[`hetionet-neo4j.yaml`](profiles/hetionet-neo4j.yaml)（名称一致）。用法：`IndexMiddleware(..., profile=neo4j_profile('profiles/lsqb-neo4j.yaml'))` |

接入一个新系统需要以下几步：

1. 写一个 mapping 文件。schema 相同的系统可以省略这一步。
2. 如果语法与 Neo4j 或 Kuzu 不同，在 `cico/dialects/` 中继承 `Dialect`，覆盖不同的写法：标识符、标签、变长路径、字面量、函数名、CAST。
3. 如果输入语法有差异，写一个 frontend（参考 `cico/neo4j.py`）。
4. 在 `cico/profiles.py` 中用 `register()` 注册。

验证守卫会自动把新方言的输出翻译回内部方言并重新绑定，所以方言实现中的错误只会导致回退原查询，不会产生错误的结果。

### 方言之间的语义差异（已处理）

- **Neo4j 关系唯一性**：Neo4j 要求同一个 MATCH 中的关系绑定到不同的边，Kuzu/NeuG 没有这个限制。处理方式：输入时补上 `id(r1) <> id(r2)`（无法相同的关系对不加）；输出时把必选 MATCH 拆成每个关系一个 MATCH；OPTIONAL MATCH 中如果有两个关系可能落到同一条边，就回退原文。
- **Neo4j 子类型标签**：内部的 `PLACE {type:'City'}` 输出为 `(x:City)`，不写成 `(x:Place) ... WHERE x:City`；后者会让 Neo4j 按两个标签的选择率相乘估算，选出很差的计划。
- **变长路径**：Neo4j 是 trail、内部是 walk，两者在无环关系类型（如 REPLY_OF）上一致。Kuzu 把 `*l..` 的上界限制为 `var_length_extend_max_depth`（默认 30），中间件假设路径长度不超过这个上限。
- **NeuG 缺少的函数**：`coalesce` 转成 CASE；Neo4j 的链式比较 `a < b < c` 拆成合取；`duration({hours: x})`、`duration('PT4H')` 转成内部的 `duration('4 hours')`，输出到 Neo4j 时再转回 ISO 格式。

## 目录

```
engine/incgq_rewrite_engine.cc  只做编译的引擎：内存目录、运行时 FILL、选中方案复现
engine/bound_ir.h               Bound IR v2 序列化（UNWIND、变长路径、嵌套字面量、子查询）
engine/cico_exec.cc             测试用执行器：在 NeuG 数据库上执行 Cypher
cico/middleware.py              IndexMiddleware：execute / register_index / drop_index / rewrite
cico/printer.py                 Bound IR → Cypher
cico/canonical.py               规范化与结构比对（验证守卫）
cico/materialize.py             索引物化：FILL → 目标方言的 DDL、重置、写回语句
cico/index_files.py             受管 NeuG FILL 文件：指纹、依赖、成功回执；Kuzu/Neo4j 执行器
cico/normalize.py, frontends.py 输入规范化（coalesce、链式比较、常量折叠），Kuzu frontend
cico/introspect.py              从 Kuzu 库提取 schema
cico/schema.py, statements.py   中间件 schema 与 DDL 处理、脚本拆分与语句分类
cico/dialects/, neo4j.py        各系统方言与 Neo4j 输入翻译
cico/server.py, __main__.py     HTTP 服务与命令行
profiles/                       各系统的 schema 映射
tests/                          单元测试（test_middleware.py）、语料回归、LDBC BI 覆盖率、e2e
```

## 测试

```sh
python3 -m unittest discover -s tests -v       # 含引擎路径选择、HTTP 与改写集成测试
# IncGQ-experiment-docs 全部 Kuzu/Neo4j 查询的覆盖率
python3 tests/workload_coverage.py --workloads hetionet ldbc-bi ldbc-ic lsqb finbench --output cov.json [--scope query]
# 文档查询的等价改写版本（implementation/middleware/query_edits，同相对路径）会自动替换原文件；
# CICO_QUERY_OVERLAY='' 关闭。每个改写版本先在真实系统上与原查询比对：
<kuzu-python> tests/equivalence_check.py --system kuzu --workload ldbc-bi --query bi-4 --modified <file> --store <kuzu-db> --samples 10
# 不建索引、只核对改写版本本身：原查询与改写版本在 Kuzu 库上逐参数比较结果多重集，并统计改写依赖的数据模型前提
<kuzu-python> tests/kuzu_query_edit_check.py --store <kuzu-bi-sf1> --output edits.json --queries bi-2 bi-6 bi-16 --samples 3
# 真机验证：在 baseline 库副本上用中间件生成的语句建索引，再比较原查询与改写结果
<kuzu-python> tests/target_validation.py --system kuzu --workload ldbc-bi --store <kuzu-bi-baseline> --samples 10
tests/neo4j_clone.sh <neo4j-store> /tmp/n4j 7692 8g && python3 tests/target_validation.py --system neo4j --workload ldbc-bi --uri bolt://127.0.0.1:7692
python3 tests/e2e_ldbc.py --db ../ldbc_sf0.1 --index-script <IncGQ-experiment-docs>/NeuG/queries/index/ldbc-bi/index.cypher \
    --queries '<IncGQ-experiment-docs>/NeuG/queries/baseline/ldbc-bi/*/*.cypher'
python3 tests/bi_coverage.py --profile neo4j --schema S.yaml --index-script ... --queries '.../Neo4j/queries/baseline/ldbc-bi/bi-*.cypher' --params ... --output report.json
```

## Neo4j BI SF1 本机入口（2026-11-17）

已从 `IncGQ-experiment-docs` 的提交 `08f1a265` 导入 BI SF1。独立实例目录为
`middleware/runtime/bi_sf1/`（数据、日志和 CSV 均已忽略，不进入 Git），Bolt 地址为
`bolt://127.0.0.1:7693`，数据库名为 `neo4j`，仅监听本机且无需认证。
源数据为 `/mnt/sdb1/feiyang/datasets/bi-sf1-composite-projected-fk/graphs/csv/bi/composite-projected-fk/initial_snapshot`。
运行时使用本机已有的 Neo4j Community 5.26.28 与 Java 21，启动和停止：

```sh
export JAVA_HOME=/mnt/sdb1/dwk/imported_data/neo4j/neo4j-5.26.28/tools/jdk-21.0.12+8-jre
export NEO4J_CONF="$PWD/middleware/runtime/bi_sf1/conf"
/mnt/sdb1/dwk/imported_data/neo4j/neo4j-5.26.28/tools/neo4j-community-5.26.28/bin/neo4j status
# 同一命令末尾改为 start 或 stop 即可启动或停止。
```

新增的 `register_neo4j_index(script, index_id=None)` 可以直接注册 Neo4j 的物化索引脚本：

```python
from pathlib import Path
from cico import IndexMiddleware
from cico.neo4j import neo4j_profile

# 在 middleware/ 下执行
with IndexMiddleware('schemas/ldbc-bi.yaml', profile=neo4j_profile()) as mw:
    script = Path('../../IncGQ-experiment-docs/Neo4j/queries/index/ldbc-bi/bi11/index.cypher').read_text()
    mw.register_neo4j_index(script)
    result = mw.execute(query, parameters=params)
    print(result.rewritten, result.reason, result.cypher)
```

注册只更新中间件元数据。将输出用于目标库前，应先执行原生索引脚本或
`mw.build_statements()` 生成的建索引语句。普通 `execute(MATCH ... SET ...)` 仍按写语句处理。
支持显式节点标签的 `MATCH ... SET node.property = expression`、`count { MATCH ... }`、
`MERGE (src)-[:TYPE]->(dst)`，以及批处理 `CALL (vars) { ... } IN TRANSACTIONS` 和旧式导入 `WITH`。
BI9 的自环与正长度闭包合并成一条零长度闭包定义，并同时推断 Post→Post、Comment→Post 分区。
注册失败会回滚本次 schema 和索引元数据。不支持的复杂条件写入会明确报错。

CLI 的 `--neo4j-index-script FILE` 可重复指定，适用于 `rewrite`、`build`、`serve`、`script`；
HTTP 为 `POST /index/neo4j {"script": "...", "id": "optional"}`。
原生 `CREATE [RANGE|TEXT|POINT|FULLTEXT|VECTOR|LOOKUP] INDEX`、`CREATE CONSTRAINT` 和对应 DROP
经 `execute()` 返回 `kind=ddl, reason=target_ddl, route=target`，由调用者交给 Neo4j 执行。
这些原生检索索引无需注册 FILL 元数据。

```sh
# 仓库根目录；重新验收只改写并 EXPLAIN，不执行 BI 查询。
python3 middleware/tests/neo4j_bi_acceptance.py --output /tmp/neo4j-bi.json
# 若需要重建五组物化索引，增加 --build；会执行索引脚本中的清理和重建写入。
```

验收使用 Neo4j 版 bi10、bi9、bi5、bi6、bi11 五组脚本，注册 8 个载体定义，实际执行 14 条建索引语句。
BI3、BI5、BI9、BI11 成功改写，全部通过回译绑定验证和 Neo4j EXPLAIN；没有执行 BI 工作负载查询。
完整逐条结果和生成语句见
[`neo4j-bi-sf1-20261117.json`](../implementation/middleware/validation/neo4j-bi-sf1-20261117.json)。
当前仍不支持任意 `CALL` 查询子句、GDS 过程、复杂条件和多属性关系物化脚本；对应查询保留原文。
本次验收不代表全部 20 条 BI 查询都可改写。

### 全部 BI 查询检查（2026-11-17，后续轮次）

**当前结果已更新**：文档已有 15 组 Neo4j BI 索引中，9/15 组可注册、8 条对应原始查询的全部样本成功改写、80/80 输出通过 EXPLAIN。后续按 wiki 理论增强了相关标量图计数/存在性、有限记录进入 CALL 和探针绑定隔离，65/65 回归通过，13/13 小图用例的实际结果多重集一致；临时数据全部回滚。BI8/16 的索引源已命中，完整查询还受未公开 Tag 接口的限制，不能只补语法或假设名称唯一。入口见 [当前交接](../implementation/middleware/neo4j-bi-handoff.md) 和 [理论约束增强](../implementation/middleware/neo4j-theory-hardening.md)；下面 6/20 为修复前历史结果。

新增 CALL 子查询局部改写与参数化定义：

```python
# parameters 的值固定到索引定义中；更换值需要删除旧定义后重新注册并物化。
results = mw.register_neo4j_index(parameter_script, parameters={'endDate': end_date})
result = mw.execute(query, parameters={'endDate': end_date, 'country': country})
```

HTTP `/index/neo4j` 对应增加 `parameters` 字段。CALL 内部在导入变量的类型上下文中进行改写，
UNION 各分支独立验证；写入语句整条返回原文。多候选目录异常时，隔离候选重试仍启用回译守卫。

全量 20 条查询、191 组参数已经检查，当前为 **6/20 条查询全部样本成功改写，60/191 个样本成功**，
全部 60 条输出通过 Neo4j EXPLAIN。新增 BI6、BI13；BI5 同时命中点赞和回复计数。
参数化索引本轮只注册元数据；未按这些参数逐组重建物理属性，没有执行 BI 工作负载查询。
44/44 回归测试通过。完整结果和剩余障碍见
[`全量 BI 验证记录`](../implementation/middleware/validation/neo4j-bi-all-20261117.md)。

```sh
# 仓库根目录；覆盖率未达 20/20 时退出码为 1，输出仍完整保存。
python3 middleware/tests/neo4j_bi_rewrite_matrix.py --samples 10 --output /tmp/neo4j-bi-all.json
```
