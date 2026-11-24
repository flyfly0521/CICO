# Neo4j 查询改写补全（2026-11-23 / 24）

目标（用户 2026-11-24）：仿照 Kuzu 的做法（[kuzu-input-rewrites.md](kuzu-input-rewrites.md)），让 Neo4j 上应当读索引的查询都改写成功。
允许在语义不变的前提下修改原始查询（实验文档 `Neo4j/queries/input/`，见其 README），可以新增方言转换规则、增强改写匹配。
验证口径与 Kuzu 相同：只看改写是否成功、输出是否与原查询语义相同（验证守卫 + 阅读改写文本），**不在库上执行**。

## 结果

`tests/kuzu_optimized_coverage.py --system neo4j`（结果 `cico_work/cov_neo4j_base_c.*`；同一时刻 Kuzu 为 `cov_kuzu_base_c.*`）：

| | 11-23 晚（`_b`） | 11-24（`_c`） | Kuzu（`_c`） |
| --- | --- | --- | --- |
| baseline 改写（106 条，含写语句和无索引查询） | 58 | **63** | 63 |
| 至少被一条改写读到的已注册索引 | 68/85 | **82/85** | 84/85 |

新增改写：bi-15/19/20 的 GDS 投影（setup）、bi-16、tcr-11；bi-3 新增读 `PERSON(countryName)`（之前只读 `ROOT_POST()`）；两级 FILL `weight_bi19`、`bi20_weight`、`bi15w_weight` 经
`weights/` 分阶段投影读到；bi-14 的 case2/case4 等价候选、bi3/bi9/bi14 冲突定义的隔离注册在 BI 合并目录分析中覆盖（31/33）。

未读到的 3 个：

- `KNOWS(bi15_interaction)`（bi15u）、`KNOWS(bi15_weight)`：不带窗口的 bi-15 权重，按 Kuzu baseline bi-15 定义；Neo4j 的 bi-15（LDBC 参考）
  带论坛时间窗口，对应 `bi15w`，没有读这两个索引的 Neo4j 查询（与 Kuzu 上 bi15w 原本的情形相同）。
- `Account(tcr9TransferIn*)`：与 tcr6TransferIn 定义相同，被后者选中（Kuzu 同）。

覆盖率脚本中"窗口后移一天"的检查在 Neo4j 上显示为已改写：读的是与窗口无关的 `ROOT_POST()`（根路径派生边），
两个窗口计数 FILL 没有被用到，这是正确的结果。

## 中间件修改

Neo4j 前端（`cico/neo4j.py`）：

1. **路径边谓词**：`MATCH p = (a)-[:T*l..u]->(b) WHERE all(x IN relationships(p) WHERE P)` → 内部 `*l..u (x, n | WHERE P)`。
   只在 p 没有别的用处、路径只有这一个关系模式、P 只读 x（以及参数、字面量）、WHERE 是合取时转换。
2. **trail = walk 的可达性**：变长关系是其 MATCH 子句中唯一的关系模式、有向、下界 0 或 1、路径和关系变量都没有被使用，
   且子句后面（可隔一个 WHERE）是不含聚合的 `WITH DISTINCT` 时，标记为 walk，不再翻成 `TRAIL`。
   依据：长度 k ≥ 1 的 walk 包含同起点、同终点、长度 1..k 的 trail，且只用到原 walk 的边子集（有向闭 walk 含经过起点的有向环）。
   无向关系不适用：a-b-a 是 walk，却不是 trail。
3. **变长关系的表剪枝**：从有类型的起点出发，只保留 walk 能用到的关系表（闭包）。tcr-11 中 `guarantee` 映射到
   Person/Company 两张担保表，起点是 Person 时只剩 `PersonGuaranteePerson`。
4. **`datetime.truncate('day', x)`** → `date_trunc('day', x)`（常量折叠已支持）；其他粒度报 Unsupported。
5. **嵌在过程里的查询**（`embedded_queries`、`splice`）：`gds.graph.project.cypher(name, nodeQuery, relQuery[, config])` 的两个
   查询字符串各自作为独立语句改写，参数从 config 的 `parameters: {k: $k}` 传入（字符串里引用了没有传入的参数时跳过），
   再按单引号转义填回原位。中间件（`_rewrite_embedded`）只替换这两个字符串，其余文本保持不变。
6. **无环类型的关系唯一性**（`_edge_into_path_start`）：同一子句里的固定边 x -[:T]-> s 和变长路径 s -[:T*]-> … 不可能共享边
   （共享就会回到 s，构成环），OPTIONAL 子句也一样。打印器（`Neo4jDialect.match_text`）加了同样的豁免。
7. **根路径重锚定**（relax 阶段，`_reanchor_root_paths`）：子句里有 c -[:T]-> t 时，`(t)-[:T*0..]->(e …)` 读作 `(c)-[:T*0..]->(e …)`。
   前提：T 无环且无上界，c 至多有一条出边 T（声明的 at_most_one，记入 premises），c 的表与 e 的表不相交（从 c 出发的路径长度 ≥ 1）。
   这样 Neo4j 写法 `(t1)-[:REPLY_OF*0..]->(:Post)`（Neo4j 语义下合法）就能匹配 FILL 的 `(comment)-[:REPLYOF*0..]->(:POST)`
   （FILL 是同态语义）。

中间件（`cico/middleware.py`）：

- `_rewrite_embedded`（见上文第 5 点）。
- 声明约束重试（relax）在部分 QueryPart 已改写时也会尝试，改写的段更多才采用（参数特化原本就这样处理）。bi-3 的国家部分需要
  由 endpoint_labels 推出 `:City`/`:Country`，而回复链部分在普通流程里已经改写。
- 逐段改写的基线探针：relax 阶段的翻译无法打印回目标方言时（例如重锚定后的根路径在 Neo4j 中会被唯一性约束禁止），
  改用普通翻译来打印基线。探针只比较桩和尾部，relax 不改变这两部分。

规则（`cico/profiles.py`）：`rel_projections` 不再要求 homomorphism。它只改写首段恰好匹配一条关系、之后各段不再读图的语句，
这种情况下 isomorphism 与 homomorphism 一致。`weights/` 的两级权重投影靠这条规则读到权重列。

编译器：11-23 的 Neo4j 求和改动（Neo4j 前端把 `sum` 读成 `CASE WHEN sum IS NOT NULL THEN sum ELSE 0 END`；
投影中包着本段聚合的标量逐个验证聚合；被覆盖聚合的副本也算覆盖；闭包检查把 CAST 视为确定性函数）
记录为 `compiler/upstream/patches/cico-neo4j-sum-scalars.patch`，并加入 `series`。

覆盖率脚本：setup 改写时传入参数；BI 合并目录分析也对 Neo4j 运行（窗口 setup 用 input `bi-15-create-graph`，
分阶段投影用 `weights/*-create-graph.cypher`）。

测试：`tests/test_neo4j_input_rules.py`（17 例：上述规则的正例和反例，以及一条 GDS 投影读取 FILL 的端到端改写）；
`test_dialects` 按新的规则集合更新。全部单测通过。

## 前提

input 注释里写明的前提，未在库上检查（用户要求不跑查询）：

- KNOWS 无自环，且每对人之间只存一条 KNOWS（LDBC 的 knows 是对称关系，每对存一次）：用于把无向 KNOWS 拆成两个方向的 UNION ALL。
- STUDY_AT 只指向 University：用于 bi-20 去掉 `:University`。
- tcr-11：truncation_limit = 500，担保出度最大为 2（与 Kuzu baseline 和 NeuG 索引 tcr11 相同的已审计前提）。
- 日期边界按 UTC（bi-16 的 `datetime.truncate`、bi-15 的 epochMillis 改为参数）。

其余依赖的都是已声明约束（Tag.name 唯一、unique_edge KNOWS、REPLY_OF/CONTAINER_OF 至多一条），会记入改写结果的 premises。
