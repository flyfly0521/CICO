# 中间件迁入 Kuzu 仓库与 Kuzu 目标适配（2026-11-18）

用户要求：参照中间件在 Neo4j 上的迁移，完成中间件在 Kuzu 上的应用迁移。本文记录迁移方式、为 Kuzu 目标新增的部分、验收结果和剩余问题。代码在 `/mnt/sdb1/feiyang/kuzu` 的分支 `develop`（基于 `origin/develop` = `master` `89f0263c`），**尚未提交**。

## 1. 迁移方式

| 来源 | 迁入后 |
| --- | --- |
| Neo4j 仓库 `develop` 提交 `2df42a93` 中**已跟踪**的 `middleware/` | `kuzu/middleware/` |
| 同一提交的 `implementation/middleware/`（设计、进度、补丁、query_edits、验证记录） | `kuzu/implementation/middleware/` |
| Neo4j 仓库的 `middleware/runtime/`（Neo4j BI SF1 实例）、`engine/bin/`、`engine/build/` | **未复制**（运行环境与构建产物） |

- 用 `git archive HEAD middleware implementation/middleware` 导出，保证只带入已提交的源码和文档，不带入忽略的数据库、日志、CSV。
- 引擎在新位置独立构建：`cmake -S middleware/engine -B middleware/engine/build -DCICO_BUNDLED_PATCH=ON`，`cmake --build ... --target incgq_rewrite_engine_patched cico_exec_patched`。补丁应用在 IncGQ 当前源码（`c88ca1f1`）上，三个编译单元的补丁后哈希与 Neo4j 仓库的构建**完全一致**（`rewriter_v4.cpp` `530311b5`、`index_predicate_key.cpp` `ff1203be`、`complete_key_group_rewriter.cpp` `65ba4328`）。
- 默认路径不需要改：CMake 的 `NEUG_SOURCE_DIR` 查找同级 `IncGQ/`（`/mnt/sdb1/feiyang/IncGQ`），`tests/workloads.py` 的覆盖层是相对路径 `implementation/middleware/query_edits`。
- 迁入后原样回归：**110/110** 通过（与 Neo4j 仓库相同）。

两个仓库从此各有一份代码。Neo4j 仓库副本继续服务 Neo4j；本副本以 Kuzu 为目标，`neo4j` profile 仍可用。共享的修复（见 2.4）需要时应同步到另一份。

## 2. 为 Kuzu 目标新增的部分

> 第 2、3 节是第一轮（迁入 + Kuzu 目标）的记录。第二轮按用户要求改了三处，见第 5 节：中间件以 `dialect` 参数选择方言转换声明；FILL 一律按 NeuG 语义；`--fill-file` 受管文件改为只由中间件写入的索引注册表（FILL / `DROP_INDEX`）。执行器移到 `cico/targets.py`，`cico/index_files.py` 已删除。

Neo4j 迁移的核心是“受管 NeuG FILL 文件 → 目标方言物化 → 成功回执后才启用改写”（[`neo4j-fill-file-reload.md`](neo4j-fill-file-reload.md)）。指纹、依赖、回执、原子落盘、失败恢复等逻辑与目标无关，直接复用；目标相关的只有执行器和入口。Neo4j 是服务端、无 schema；Kuzu 是嵌入式、有 schema，差异集中在下面几点。

### 2.1 `KuzuIndexExecutor`（`cico/index_files.py`）

- **物理身份**：Kuzu 没有 `db.info().id`。身份取解析后的库路径加目录的 device/inode。复制、恢复出的库 inode 不同，被视为另一个物理代，必须使用独立的状态文件（与 Neo4j 一样，混用会被拒绝）。
- **只在重载期间占用库**：Kuzu 同一时刻只允许一个读写进程。执行器在预检/构建时才打开库，`IndexFileCatalog.reload()` 结束（成功或失败）都会调用执行器的 `release()` 释放。新增的 `release` 钩子对没有它的执行器（Neo4j）没有影响。
- **预检（`validate`）**：Kuzu 有 schema，`SET p.cached = ...`、`CREATE (a)-[:ROOT_POST]->(b)` 只有在对应 DDL 之后才能绑定。预检在一个手动事务里依次执行 DDL、对其他语句 `EXPLAIN`，最后 `ROLLBACK`。Kuzu 0.10.0 实测支持事务内 DDL，回滚后列、表都不存在。语句失败时 Kuzu 会自行中止事务，此时报告原始错误，而不是 ROLLBACK 的报错。
- **`IF NOT EXISTS` 的陷阱**：物化 DDL 用 `ALTER TABLE ... ADD IF NOT EXISTS` / `CREATE REL TABLE IF NOT EXISTS`。定义变化后，如果同名列已以另一种类型存在，Kuzu 会静默保留旧列，随后写入被隐式转换。预检因此会读 `table_info` / `show_connection`：列类型不同、关系表端点不同或声明属性类型不同时，在首条物理写入前报错，要求先手工删除旧载体（与 Neo4j 流程一致，中间件不自动删除物理数据）。关系表上多出的列（NeuG 隐藏状态、旧构建遗留的状态列）允许存在。
- **执行**：每条语句自动提交，结果读完、关闭后才返回，满足“返回即已成功提交”的执行器约定。
- 默认 4 线程、8 GB buffer pool（Kuzu 默认会占用大部分内存，这台机器是共享的）。

### 2.2 CLI

`--kuzu-db PATH`、`--kuzu-threads`、`--kuzu-buffer-pool-gb`。`--fill-file` 接受 `--kuzu-db` 作为目标；`--materialize-new` 要求 profile 与目标匹配（`kuzu` + `--kuzu-db` 或 `neo4j` + `--neo4j-uri`）；两个目标不能同时给。HTTP 接口不变（`/index-files`、`/index-files/reload`）。

### 2.3 注册文件 `middleware/indexes/ldbc-bi-kuzu.cypher`

与 `ldbc-bi-native.cypher` 相同的 NeuG 静态定义（countryName、ROOT_POST、BI5 四个计数、BI6 PERSON.likeCount），另加 NeuG BI4 的 `BI4_MESSAGE_COUNT`。NeuG 语法与目标无关，同一份文件也可以用于 Neo4j；单独建文件只是为了让 Kuzu 的物化与 Neo4j 的回执分开管理。

### 2.4 共享修复：派生关系载体的隐藏状态列缺少 DDL（`cico/materialize.py`）

`include_internal_state=True` 时，聚合派生关系（如 `BI4_MESSAGE_COUNT {messageCount}`）的写回会带上 NeuG 隐藏状态列 `__neug_fill_state_n_rows_<hash>`，但 DDL 只按 schema 中的属性建表。Neo4j 无 schema，所以没有暴露；Kuzu 预检报 `Cannot find property __neug_fill_state_n_rows_... for .`。现在对 schema 中没有的写回列追加 `ALTER TABLE <rel> ADD IF NOT EXISTS <col> <type>`。Neo4j 方言的 `add_column_ddl` 返回空列表，**Neo4j 的计划与指纹不变**。

## 3. 验收

除特别说明外，Kuzu 相关命令使用 Kuzu 0.10.0 venv：`KPY=/mnt/sdb1/dwk/imported_data/kuzu/tools/kuzu-0.10.0-venv/bin/python`。

### 3.1 回归

- `python3 -m unittest discover -s middleware/tests`：**120/120**（系统 Python 没有 `kuzu` 包，跳过 5 个需要真实 Kuzu 库的用例）。
- `$KPY -m unittest middleware/tests/test_kuzu_target.py`：**10/10**（执行器身份、事务内 DDL 后 EXPLAIN 并回滚、绑定错误原样报告、类型/端点冲突拒绝、自动提交后释放锁、CLI 目标校验）。

### 3.2 Kuzu 小图重载语义（`tests/kuzu_index_file_acceptance.py`）

在临时目录新建私有 Kuzu 库（3 个 Person、2 个 City），全部通过（**8/8**；第一轮记录，验收脚本与结果已被 5.4 的注册表验收替代），每次改写都在 Kuzu 上 EXPLAIN 并比较原查询与改写的结果多重集：

| 检查 | 物理写入 | 结果 |
| --- | --- | --- |
| 首次构建：计数和隐藏状态列 | 5 条 | 物理值 `(1,2,2),(2,1,1),(3,0,0)`；改写一致 |
| 重启（新中间件实例） | 0 | 不重建；改写一致 |
| 只改注释、空白、关键字大小写 | 0 | 不重建 |
| 新增依赖 `cached` 的 `doubled` | 只建 `doubled` | 不重写 `cached` |
| 修改 `cached` 的过滤 | 按依赖顺序建 `cached`、`doubled` | 改写一致 |
| 新增派生关系 `CACHED_LIVES` | 3 条（建表、删除、CREATE） | 改写一致 |
| 把 `doubled` 改成 DOUBLE（已有 INT64 列） | 0 | 预检拒绝，回执与活跃目录不变 |
| 预检后检查表结构 | – | 只有已构建的列，预检的 DDL 已回滚 |

CLI 冒烟（同样是私有临时库）：`cico build --profile kuzu --fill-file ... --kuzu-db ... --materialize-new` 第一次构建 1 项，第二次 0 项；`cico rewrite` 使用受管目录命中该索引。

### 3.3 BI 抽测（`tests/kuzu_bi_acceptance.py`）

口径与 Neo4j 相同：索引取实验文档 `NeuG/queries/index/ldbc-bi`（提交 `08f1a265`），原始查询取 `Kuzu/queries/baseline/ldbc-bi`，参数取 SF1 清单。按用户要求**只验证物化语句能正确执行、改写正确**（回译验证通过且输出在 Kuzu 上可 EXPLAIN），不执行 BI 查询。

**目标库**：从 `/mnt/sdb1/feiyang/datasets/bi-sf1-composite-projected-fk` 的原始 CSV 用实验文档的 `Kuzu/scripts/import/import_ldbc.py` 重新导入到 `middleware/runtime/bi_sf1/kuzu-db`（Kuzu 0.10.0，35 s；8 个点表、16 个边表，计数与 `Kuzu/docs/import/ic-bi-sf1.md` 逐项一致；提取的 schema 与 `middleware/schemas/ldbc-bi.yaml` 完全相同；导入报告 `runtime/bi_sf1/import-report.json`）。该库保持为干净 baseline，验收在副本 `runtime/bi_sf1/work/kuzu-bi` 上进行。完整结果见 [`validation/kuzu-bi-sf1-20261118.json`](validation/kuzu-bi-sf1-20261118.json)。

**注册计划与 EXPLAIN**（每个 `<case>/index.cypher` 单独经 `IndexFileCatalog`，kuzu profile）：

| 结果 | case |
| --- | --- |
| 生成计划，全部语句通过 Kuzu EXPLAIN（**12/12，144 条语句**） | bi3、bi4、bi5、bi6、bi9、bi10、bi11、bi14、bi15、bi17、bi19a、bi20 |
| 只有 schema 初始化，FILL 在参数文件里 | bi2、bi8、bi13、bi16 |
| 不能原样加载（未渲染的 `{}` 位置模板） | bi15w |

与 Neo4j 的审计结果（12 个计划、4 个只有 schema、bi15w 模板）相同。bi4 首轮 EXPLAIN 失败暴露了 2.4 的隐藏状态列问题，修复后通过。

**改写命中**（每个 case 只注册该 case 的定义，参数文件按 SF1 第 0 组渲染；Kuzu 原始查询，输出在库副本上加载载体 DDL 后 EXPLAIN 并回滚）：**6/17** 改写，6/6 输出通过 EXPLAIN。

| case | 结果 | 使用的索引 |
| --- | --- | --- |
| bi3、bi9、bi17 | 改写 | `ROOT_POST()` |
| bi5 | 改写 | `COMMENT/POST(replyCount)` |
| bi8 | 改写（参数特化：startDate、endDate、tag） | `PERSON(bi8MessageCount)` |
| bi13 | 改写（参数特化：endDate） | `PERSON(bi13MessageCount)` |
| bi10 | 前端不支持：`KNOWS* SHORTEST 1..$maxPathDistance` NeuG 解析失败 | – |
| bi15、bi15w、bi19a | 前端不支持：`WSHORTEST` 不在 NeuG 函数目录 | – |
| bi4 | 文档原文：`OPTIONAL MATCH ... WHERE rootForum IN topForums` NeuG 不支持；query_edit 版本只注册 BI4 定义时未命中 | – |
| bi2、bi6、bi11、bi14、bi16 | `no_index_selected` | – |
| bi20 | `ignored`（原样返回） | – |

**物化与改写验证**（`--build middleware/indexes/ldbc-bi-kuzu.cypher --samples 10`，不加 `--compare-results`）：

- 受管目录经 `KuzuIndexExecutor` 先在事务内预检全部语句并回滚，再按依赖顺序真实执行 DDL、reset、fill：8 个定义 **8/8 ready**（183 s），全部进入活跃改写目录。紧接着第二次重载 **0 构建**（0.5 s）。
- 物化数据的聚合核对（只读，**7/7 一致**）：

| 核对 | 物化结果 | 基础数据 |
| --- | --- | --- |
| `ROOT_POST` 边数 = 消息数 | 2,860,664 | 2,860,664 |
| `ROOT_POST` 终点都没有 `REPLYOF` 出边 | 0 | 0 |
| `COMMENT/POST.likeCount` 之和 = `LIKES` 边数 | 1,870,268 | 1,870,268 |
| `COMMENT/POST.replyCount` 之和 = `REPLYOF` 边数 | 1,739,438 | 1,739,438 |
| `PERSON.likeCount` 之和 = 作者消息被点赞次数 | 1,870,268 | 1,870,268 |
| 有 `countryName` 的 Person | 10,295 | 10,295 |
| `BI4_MESSAGE_COUNT.messageCount` 之和 = 有作者的消息数 | 2,860,664 | 2,860,664 |

- 在已物化的库上改写全部 Kuzu BI 原始查询（每条 10 组参数），使用的索引都已物化的改写结果全部 EXPLAIN：**50/50 通过**，涉及 bi-3、bi-5、bi-9、bi-17（文档原文）和 bi-4（`query_edits` 版本，读 `ROOT_POST`）。多索引目录下 bi-4 的 query_edit 版本可以改写，单独注册 bi4 目录时不能（它只有 `BI4_MESSAGE_COUNT`，改写用的是 ROOT_POST）。
- 每条改写输出都已通过中间件自己的回译守卫：翻译回内部方言、在无索引目录重新绑定、与 RewriterV4 选中的方案做结构比对，并确认确实读取了索引载体。
- 参数化定义（bi8、bi13）只做了注册、改写和 EXPLAIN，没有按参数物化。

此前在旧库副本（`cico_work/kuzu/bi-baseline`）上曾实际执行比对 bi-3/4/5/9/17 共 15 次，结果多重集全部一致（[`validation/kuzu-bi-compare-oldstore-20261118.json`](validation/kuzu-bi-compare-oldstore-20261118.json)）；在新库上这一步按要求未做，脚本保留 `--compare-results` 选项。

## 4. 剩余问题与下一步

1. **Kuzu BI 改写覆盖**：原查询 9/17（第 6 节）；加上第 7 节的 3 份查询编辑后 12/17，与 Neo4j 的 12/17 持平。仍不改写的：bi15/15w/19 的 Kuzu 查询只剩 `WSHORTEST`（权重由 setup 脚本预先算好），NeuG 不支持这种语法，也没有可替换的部分；bi20 在 Kuzu 工作负载中标注为不支持；bi4 见第 7 节。
2. **当前 NeuG 内核规定一个载体只能由一个 FILL 拥有**（“A FILL already owns this target property” / “owned by only one FILL definition”），所以注册表一个身份对应一条 FILL；内核支持多分支后再扩展条目格式。
3. **两份代码的同步**：方言声明、共享约束、FILL=NeuG、注册表、2.4 的物化修复都只在本仓库；Neo4j 输出逐字节不变（5.4），可以回合到 Neo4j 仓库。Neo4j 仓库现有的 `--fill-file` 用户需要改用注册表（`cico import` 导入旧文件，回执可沿用）。
4. **Kuzu 版本**：执行器和测试按 Kuzu 0.10.0（目录格式库）验证。本仓库源码是 0.11.x（单文件库），0.10.0 打不开 0.11 建出的库；换用 0.11 时需重新验证身份（单文件 inode）和事务内 DDL。
5. **旧载体不自动删除**：`DROP_INDEX` 和定义替换都不删物理列/关系表（与 Neo4j 流程一致）；之后若以另一种类型重建同一列，预检会拒绝，需要手工 `ALTER TABLE ... DROP`。
6. 本次没有提交，分支 `develop` 上的 `middleware/`、`implementation/middleware/` 均为未跟踪文件。

## 5. 第二轮：方言声明、FILL 语义与索引注册表（2026-11-18）

### 5.1 方言参数与转换声明

- 中间件参数 `dialect`（CLI `--dialect`，旧名 `profile` 仍可用）选择一份**方言转换声明**（`cico/profiles.py`）：frontend（输入转换）、dialect（输出打印）、`Semantics`（关系唯一性、变长路径、开放上界、类型类写法、过程调用、原生 DDL）（第三轮起，规则不再在声明里列出，见第 6 节）。内置 `neug`、`kuzu`、`neo4j`；新数据库按 [`middleware/docs/dialects.md`](../../middleware/docs/dialects.md) 写声明并 `register()`。
- 核心流程中原有的 7 处 `frontend.name == 'neo4j'` 判断全部换成声明：原生 DDL 透传 → `semantics.native_ddl`；过程类型推断 → `semantics.procedures`；四条规则 → `rules`。剩下唯一的名字判断是 `register_neo4j_index`（它本来就只处理 Neo4j 原生脚本）。

### 5.2 共享约束

- 声明约束按数据集写一次：[`middleware/constraints/ldbc-snb.yaml`](../../middleware/constraints/ldbc-snb.yaml)，内部表名，含类定义（Message = COMMENT|POST；City = PLACE 且 `type='City'`）和 `acyclic: [REPLYOF]`。中间件参数 `constraints=` / `--constraints`。
- Neo4j mapping 改为 `constraints_file: ../constraints/ldbc-snb.yaml`，加载时翻译成 Neo4j 词汇；翻译结果与原内联的 12 条约束逐条相同（单测断言）。类名必须是含义相同的 Neo4j 标签，否则拒绝加载。
- 约束检查语句按方言生成。12 条 Kuzu 检查在新导入的 BI SF1 上违反数全为 0；另用 6 条故意写错的约束做反向对照，全部检出违反（如 Message 至少一条 REPLYOF 出边：违反 1,121,226 = POST 数）。

### 5.3 FILL 一律 NeuG；索引注册表

- `register_index` 不再经过目标 frontend：FILL 源在任何方言下都按 NeuG 绑定。回归：同一 MATCH 中两条 KNOWS/LIVES_IN 的 FILL，在 Neo4j 方言下注册的文本不带关系唯一性不等式；Neo4j 词汇（`Message`、`HAS_CREATOR`）的 FILL 被拒绝。
- 用户 2026-11-18 决定：索引注册表是带格式的文件，只通过 FILL 输入、`DROP_INDEX` 删除，写入规范化后的索引；不再支持手工编辑。实现：[`cico/registry.py`](../../middleware/cico/registry.py)（`IndexRegistry`）。
  - 条目：`id`（载体表+列）、`fill`（规范化 NeuG FILL）、`carrier`（载体声明：推断出的，或 `cico import` 导入脚本时的 DDL，如 `DEFAULT 0`）、`depends_on`、`submitted`、`updated`。
  - FILL：新身份新增；规范文本与载体都相同则 `unchanged`（不写文件、不构建）；同身份不同定义则替换并按依赖顺序重建它和下游。`DROP_INDEX`：有下游读取时拒绝并列出；删除条目和回执，不动物理数据。
  - 每次变更：编译整个注册表 → 目标 EXPLAIN 预检 → 原子写注册表 → 停用待重建项 → 构建 → 回执落盘后启用。失败阶段在 `Result.reason`（`failed:parse|bind|register|plan|dependencies|dependents|missing|validate|build`）。
  - 注册表是逻辑定义；构建回执仍按物理目标单独存（`--index-state`）。指纹算法未变：同一份 `ldbc-bi-kuzu.cypher` 经注册表导入后 8 个指纹与旧目录在旧库上算出的**完全相同**，已有回执可沿用。
  - HTTP：`/statement` 处理 FILL / `DROP_INDEX`；`GET /registry`；`POST /registry/build` 重试。删除了 `/index-files*`。CLI：`--registry`，`cico import`。
- 被替换的第一轮实现（文件追加、分支组合并、按行号报错）已删除。

### 5.4 验证

- 单测：系统 Python **133/133**（跳过 5 个需要 `kuzu` 包的用例），Kuzu venv 的 `test_kuzu_target` 10/10。新增 `test_dialects.py`（12）、`test_registry.py`（17）。
- **Neo4j 与 Kuzu 输出不变**：改动前保存了 10 组 system × workload 的全部实验文档查询逐条记录和 Neo4j 的 NeuG-FILL BI 矩阵（12/17）；第一轮改造后与注册表改造后各重跑一次，**239 条记录 0 差异**（`tests/dialect_regression.py`，基线 `/mnt/sdb1/feiyang/cico_work/dialect_baseline`）。
- Kuzu 私有小图经语句入口的注册表验收 **10/10**（[`validation/kuzu-registry-20261118.json`](validation/kuzu-registry-20261118.json)）：FILL 物化（推断载体无 DEFAULT，未匹配的 Person 为 NULL，改写结果仍一致）；重启零构建；等价 FILL 零构建；新增下游只建自己；替换上游按顺序重建上下游；派生关系；有下游时拒绝 DROP_INDEX；删除条目不写库；删除后以 DOUBLE 重新 FILL 同一列被预检拒绝（库里留有 INT64 列）且注册表不变；预检回滚。
- BI SF1（新导入库副本，注册表导入 `ldbc-bi-kuzu.cypher`）：8/8 物化，重启 0 构建，10 组参数的 50 条改写输出在已物化库上 EXPLAIN 全部通过；注册 12 个 case、144 条语句 EXPLAIN 通过；改写 6/17。与第一轮结果相同。
- `tests/neo4j_registry_acceptance.py`（Neo4j 小图）已迁移到注册表，但没有运行：7693 实例属于 Neo4j 仓库的运行环境，未对其写入。

## 6. 第三轮：规则改为共享实现，按前提自动启用（2026-11-18）

用户确认的方向：改写规则对所有方言是同一份实现，每条规则声明自己需要的钩子、语义前提和约束，满足的声明自动启用；方言之间只有“语义补偿”类规则（`path_carriers`）因语义不同而不同。

### 6.1 规则框架（`cico/profiles.py`）

- `RULES` 里每条规则是一个 `Rule`：`hooks`（frontend/dialect 上需要的方法）、`applies`（对 `Semantics` 的前提）、`constraints`（是否需要数据集约束）。`Profile.enabled(rule)` 现场判断，`rule_status()` 给出每条规则未启用的原因；`disabled` / `get_profile(..., disabled=...)` 显式关闭。声明里的 `rules=` 列表已删除。
- 推出的结果：Neo4j 五条全开，与改动前完全相同（`distinct_partitions` 原先没有开关，现在也受前提约束）。Kuzu（加载约束时）启用 `scalar_regions`、`edge_carriers`、`constraint_retry`；`path_carriers` 因为同态语义不需要，`distinct_partitions` 缺钩子（Kuzu 没有 `CALL { }`）。

### 6.2 各规则在 Kuzu 上的实现

| 规则 | 改动 | 按 theory.md 核对的前提 |
| --- | --- | --- |
| `scalar_regions` | 实现本来就与方言无关，只是开关 | 探针走普通 frontend、打印器和验证守卫；Kuzu 的 `COUNT { MATCH … }` 与 NeuG 同为同态计数 |
| `edge_carriers` | 无环关系改从共享约束读（经 `frontend.rel_tables` 映射到内部表）；边方向的写法交给 `dialect.edge_carrier_test` | Kuzu 是同态匹配，NeuG 的 FILL 源与查询模式直接相等，不需要 Neo4j 那一步“关系不等式由无环性蕴含”。**Kuzu 0.10 的 `start_node(k)` 在无向模式上返回遍历方向的起点而不是存储起点**（实测 `(2)-[k]-(1)` 对边 1→2 返回 2），所以 Kuzu 用两个有向 OPTIONAL MATCH：多条边产生的多行存在性取值相同，由规则原有的 `WITH DISTINCT` 论证合并 |
| `constraint_retry` | 新增 [`cico/relax.py`](../../middleware/cico/relax.py)，在内部方言上实现：R1 删除被 endpoint_labels 蕴含的类型谓词 `x.type='City'`；T1 带常量键的节点改用键重新绑定（类型类的键要连同类型守卫一起移过去）；T2 已绑定节点经键相等改写。T3（常量键进入 `size()/COUNT{}`）改为 Neo4j 与内部方言共用的一份实现。`_projection_premises` 改按约束的类判断，不再用 Neo4j 的标签表 | R1 只用同一子句里的边和留下的谓词推出，逐条删除、不循环论证；T1 只用于必选 MATCH（OPTIONAL 里节点可能为 NULL）。PLACE 的 `name` 不是键（城市可以和国家同名），只有带 `type='Country'` 守卫的 Country 才是 |
| `path_carriers` | 不变，只在同构语义下启用 | Kuzu 是同态 + walk，普通 QueryPart 改写已覆盖（Kuzu bi17 本来就能改写） |

另外两处让 bi10 能走通：

- 打印器支持 `* SHORTEST l..u` / `* ALL SHORTEST`（`dialect.shortest_length`），递归关系的长度属性打印成 `length(path)`（`dialect.path_length`）；Neo4j 两者都保持原输出。
- 路径上下界里的参数（`1..$maxPathDistance`）NeuG 解析不了，`surface.py` 判为“需要字面量”，交给已有的参数特化；输出只对给定的上界值成立，`Result.specialized` 会列出该参数。这与 Kuzu 运行脚本把上界渲染成字面量的做法一致。

### 6.3 验证

- 单测：系统 Python 149 个通过（跳过 5 个），新增 `tests/test_rules.py`（14 个：哪些类型谓词可以删、哪些不能删，OR 下不删，另一子句的边不能作依据，OPTIONAL 中不做键身份，PLACE 名不是键，钩子输出，参数路径上界）；Kuzu venv 的 `test_kuzu_target` 10/10。
- Kuzu 私有小图（[`tests/kuzu_rules_acceptance.py`](../../middleware/tests/kuzu_rules_acceptance.py)，[`validation/kuzu-rules-20261118.json`](validation/kuzu-rules-20261118.json)）：FILL 经注册表真实物化，约束检查全为 0，5 个用例全部改写、EXPLAIN 通过、改写前后结果多重集相等：
  - BI11 形状的键身份（图中有一个与国家同名的城市，检验类型守卫不能丢）；
  - BI10 形状的类型类推导 + `SHORTEST` + 参数上界；
  - BI14 形状的边载体：KNOWS 两种存储方向和平行边；
  - 边载体在 NULL 锚点上（一个没有 KNOWS 的人）；
  - `COUNT {}` 标量区域。
- **Neo4j 输出不变**：239 条记录与改动前的基线相比，只有 1 条不同，就是 Kuzu 的 bi-14，从不改写变为改写第 2–5 部分（`edge_carriers`；回归脚本不加载约束，所以不含约束重试）。Neo4j 的全部记录和 NeuG-FILL BI 矩阵 0 差异。
- BI SF1（新库副本，加载共享约束，[`validation/kuzu-bi-sf1-rules-20261118.json`](validation/kuzu-bi-sf1-rules-20261118.json)）：注册 12 个 case、144 条物化语句 EXPLAIN 全部通过；改写 **9/17**（新增 bi10、bi11、bi14），9 个输出在载体 DDL 之后 EXPLAIN 全部通过。按要求没有执行工作负载查询。
  - bi10：`countryName` 替换位置链，只对给定的 `$maxPathDistance` 成立。
  - bi11：三个部分都读 `countryName`；前提为 8 条声明约束。
  - bi14：四个 OPTIONAL 部分读 KNOWS 计数载体（case 2/4 与 case 1/3 的模式同构，复用其反向载体）。
- 已知限制：参数以字符串传入时，Kuzu bi11 里的 `CAST($d AS TIMESTAMP) <= CAST(k.creationDate AS TIMESTAMP)` 会使 RewriterV4 不选索引（参数为 TIMESTAMP 时绑定器会消去 CAST，可以改写）。这是内核匹配的问题，与约束规则无关。

## 7. 第四轮：参照 optimized 查询编辑 Kuzu 原查询（2026-11-18）

用户要求：参考实验文档 `Kuzu/queries/optimized` 里的查询修改 Kuzu 原查询，语义保持不变，能改写成功即可。编辑放在已有的覆盖层 [`query_edits/Kuzu/queries/baseline/ldbc-bi/`](query_edits/Kuzu/queries/baseline/ldbc-bi/)（相对路径与实验文档相同，`tests/workloads.py` 自动替换原文件；差异汇总在 [`query_edits/kuzu-edits.diff`](query_edits/kuzu-edits.diff)）。编辑后的查询仍然是不读索引的原始查询，索引替换仍由中间件完成。每份编辑的文件头注释写明了等价理由。

| 查询 | 原查询为什么不能改写 | 编辑 | 等价理由 |
| --- | --- | --- | --- |
| bi2 | 窗口边界由开头的 `WITH` 绑成列，FILL 里是字面量 | 把三个边界表达式代入谓词 | 边界是 `$date` 的确定表达式，开头的 `WITH` 只产生一行，代入不改变结果 |
| bi6 | Kuzu 改编版把 LDBC 的结构压成 `count(DISTINCT like)`，对不上按 person2 计数的 FILL | 按 LDBC 参考查询的结构：`WITH DISTINCT person1, person2`，再按 person2 计点赞数后求和 | 每条点赞只连一条消息，每条消息最多一个创建者，所以不同 person2 的点赞集合互不相交。`count(person3)` 与 `count(like)` 在 OPTIONAL MATCH 中同时为空或同时非空 |
| bi16 | “截断到日期后相等”对不上 FILL 的半开时间区间；朋友计数整体是 `count(DISTINCT friend)` | 日期写成 `[date_trunc('day', $d), +1 天)`；先按朋友统计符合条件的消息数，再统计计数为正的朋友（参考 optimized/bi-16） | 对 TIMESTAMP 两种日期写法是同一条件；没有朋友或朋友没有符合的消息时得到 NULL，被 `count(DISTINCT)` 跳过 |

中间件为此补了两处与方言无关的常量折叠（`normalize.fold_constants`），并在 `surface.py` 里把 `date_trunc` 标为“需要字面量”，交给参数特化：

- `CAST(TIMESTAMP(lit) AS TIMESTAMP)` 和 `CAST(TIMESTAMP(lit), 'TIMESTAMP')` 折叠成 `TIMESTAMP(lit)`；
- `date_trunc('day', TIMESTAMP(lit))` 和 `CAST(CAST(TIMESTAMP(lit) AS DATE) AS TIMESTAMP)` 折叠成当天零点。

bi16 没有用嵌套 CAST：同一参数出现在多个子句时，Kuzu 0.10 会报 “Cannot change parameter expression data type from DATE to TIMESTAMP”。

**bi4 不能只靠编辑查询解决。** 已有编辑和一份按(人, 论坛)分组的新写法都试过，NeuG 改写器都不选 `BI4_MESSAGE_COUNT`，连该 FILL 自己的源查询都不改写（源查询含 `REPLYOF*0..`）。作为对照，定长源查询的关系聚合载体会被选中，但验证一步不通过（`verification_mismatch`），是另一个待查的问题。Neo4j 上 bi4 靠中间件的 `distinct_partitions` 规则，该规则只认 `CALL { }` 分支，而且它的输出要把 UNWIND 出来的节点放进模式，Kuzu 不允许这样写。要在 Kuzu 上改写 bi4，需要把这条规则扩展到 QueryPart 写法，并给 Kuzu 加打印钩子（按 id 重新匹配论坛）。

### 7.1 验证

- **编辑本身（不经过中间件，不用索引）**：用 [`tests/kuzu_query_edit_check.py`](../../middleware/tests/kuzu_query_edit_check.py) 在 SF1 库副本上，按 SF1 参数分别执行原查询和编辑版本，比较结果多重集。这是正确性核对，不是性能测评。结果在 [`validation/kuzu-query-edits-check-20261118.json`](validation/kuzu-query-edits-check-20261118.json)。
  - 共 16 组全部相等：bi2 3 组（100/100/88 行），bi6 3 组（各 100 行），bi16 10 组。
  - bi16 的 SF1 参数大多得到空结果（10 组中只有 1 组有 1 行）。因此另外比较了 A 分支截到 `friendCountA` 的中间结果：5 组参数各 150–300 行，绝大多数人朋友计数不为零，全部相等。
  - bi6 依赖的前提“每条消息最多一个创建者”，在 SF1 上违反数为 0。
- **中间件改写**：用 `kuzu_bi_acceptance.py` 的改写阶段测试（[`validation/kuzu-bi-sf1-edits-20261118.json`](validation/kuzu-bi-sf1-edits-20261118.json)）。三份编辑都改写成功，输出在载体 DDL 之后 EXPLAIN 通过。改写结果只对给定的参数值成立（FILL 用的是参数字面量）：bi2 依赖 `$date`，bi16 依赖 `$dateA/$dateB/$tagA/$tagB`。加上原查询能改写的 9 个，共 12/17，12 个输出 EXPLAIN 全部通过。
- **回归**：242 条记录中 7 条不同，全部是 Kuzu ldbc-bi：bi-14 由第 6 节的规则带来；bi-2/6/16 的记录由原文件变为编辑版本，各计新旧两条。Neo4j 0 差异。单测 150 个通过（跳过 5 个）。

