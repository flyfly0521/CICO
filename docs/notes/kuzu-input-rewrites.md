# Kuzu 查询改写补全（2026-11-23）

目标（用户 2026-11-22）：CICO 迁到 Kuzu 后，用 NeuG 语言建索引、把 Kuzu 查询转成 NeuG 再改写；对仍改写不成功的查询，
允许在语义相同的前提下重写原始查询（实验文档 `Kuzu/queries/input/`）、新增方言转换规则、增强改写匹配。
浮点求和按实数语义判等（用户 2026-11-22 选择）：把存储的部分和再求和、或把重复行之和写成“值×次数”，只改变浮点求和顺序，
目标系统的并行聚合本身也不固定这个顺序。

## 结果

`tests/kuzu_optimized_coverage.py`（全部 NeuG 索引注册进一个 Kuzu 方言会话，baseline 查询用 input 版本替换）：

| workload | baseline 改写（11-19 → 11-23） | 用到的索引 |
| --- | --- | --- |
| hetionet | 26/26 → 26/26 | 26/26 |
| ldbc-bi | 16/17 → 16/17 | 未用的只有 bi-15/19/20 主查询的权重 FILL |
| ldbc-ic | 0/5 → 1/5（ic5） | ic14 未用 |
| lsqb | 3/9 → 7/9 | 13/13 |
| finbench | 0/11 → 11/11 | 全部（tcr9TransferIn 与 tcr6TransferIn 定义相同，选中后者） |
| 合计 | 45/68 → 61/68 | 48 → 74（共 83） |

剩下的 7 条：ic1/ic3/ic12、lsqb q3/q9 没有 NeuG 索引；ic14 见“未完成”；bi-15/19/20 主查询（WSHORTEST/GDS，Kuzu 实验本来就排除，
bi-15/19 的 setup 已改写）。

用户 2026-11-23 要求：之后只检查改写是否成功、输出与原查询语义相同，不再实际执行查询。下面的真机比对是在此之前做的；
之后的修改（下文 12、Kuzu CAST 规则的限定）只按改写结果文本和验证守卫检查，`tests/kuzu_nested_aggregation_scan.py <coverage.json>` 检查全部改写输出没有
Kuzu 不接受的“聚合包着聚合的别名”。

真实库三方比对（`tests/kuzu_three_way_check.py`：baseline = input = CICO 改写后的 input；比较行、列名、值类型；浮点相对误差 1e-9）：

| 库 | 查询 | 结果 | 典型耗时（baseline → 改写） |
| --- | --- | --- | --- |
| LSQB SF1 | q1 q2 q4 q5 q6 q7 q8 | 7/7 | q1 4.2s→0.09s，q6 3.2s→0.33s，q2 0.19s→0.02s |
| IC SF1 | ic5 × 5 组参数 | 5/5 | ≈0.15s→0.15s |
| FinBench SF10 | 11 条 × 3 组参数（tsr-4/5、tcr-11 另用有数据的 id） | 全部一致 | tcr-7 0.02s→0.004s，tcr-13 0.40s→0.03s |
| Hetionet | 26 条 | 26/26 | 4-02 18.5s→0.03s |
| BI SF1（回归，未跑完） | bi-2、bi-8、bi-9 一致；发现 bi-3、bi-5、bi-6 的问题（见下，已修） | | bi-9 27s→1.8s |

## 编译器（RewriterV4 等）修改

记录在 `compiler/upstream/patches/cico-kuzu-input-rewrites.patch`（含 11-19 未提交的改动），回归测试 `tests/test_kuzu_rewrite_semantics.py`。

正确性修复（修复前会输出错误结果或静默丢掉改写）：

1. **方向感知的组存在性测试读错列**（11-19 引入）：值读 `q2_cnt_rev` 时存在性测试读成 `q2_cnt_fwd <> 0`，漏掉 fwd=0、rev>0 的边。现按值列的方向取同一对中的列。
2. **无分组的 COUNT 被替换成 SUM 后，空输入返回 NULL**（Kuzu/NeuG 的 `sum` 空集为 NULL，`count` 为 0）。无分组键的投影改为
   `CASE WHEN sum IS NULL THEN 0 ELSE sum END`（原 q4 改写即有此问题）。
3. **FILL 写入 REL 组的某个子表**（`FILL q5_index_Post_Tag()`）时，`applyIndexExpressionRewrites` 找不到表就直接返回：模式已换成派生边，
   `count(*)` 却没换，等于数派生边。现在按 (src, dst) 表解析到唯一子表；仍找不到时抛出，候选被拒绝。
4. 另外四处按名字查 REL 组会抛异常（aggregate_endpoint_set、complete_key_group、partial_count、distinct_count_partition），改为先 `containsTable`。

能力增强：

5. OPTIONAL MATCH + COUNT：允许无分组键（依赖 2）；派生边载体（每个非空组恰一条边）允许 OPTIONAL 中带残余模式；
   NODE 载体带残余模式时要求投影只有这一个聚合、且分组键不引用本子句变量。
6. 公开 COUNT(*) 列与私有 `__neug_fill_state_n_rows_*` 值相同；模板匹配到私有列、OPTIONAL 存在性测试都改读公开列
   （私有列不在用户 schema 中，验证守卫无法绑定）。
7. 浮点 SUM 不再要求“完整组读取”（实数语义，见上）。
8. 完整组读取（查询分组 = FILL 键）扩展到 SUM/COUNT；OPTIONAL 下 NODE 载体用公开行数做保护：
   `CASE WHEN <行数> > 0 THEN 值 ELSE NULL（COUNT 为 0）END`，与列的 DEFAULT 无关。
9. 谓词键：比较运算按操作数排序并镜像（`a < b` ≡ `b > a`）；`TIMESTAMP('…')` 按时刻而不是拼写成键（`.640` ≡ `.64`）。
10. REL 聚合列的模板变量记录关系变量（`count(edge)`、`sum(edge.amount)`），以前只记点变量，模板永远匹配不上。
11. 完整组输入闭包检查的确定性函数加入 ROUND、ABS、FLOOR、CEIL、SIGN、CAST_TO_FLOAT。
12. **变长锚点拆分丢掉前一 QueryPart 的绑定**（11-19 以前即有，BI SF1 比对发现）：`stageSelectiveVariadicAnchorSplit` 把 MATCH 拆成
    `MATCH 前缀 WITH DISTINCT anchor` + 其余模式，投影只保留 anchor。bi-3 的 input 版本中 `person` 由第 1 段绑定，拆分后
    `(forum)-[:HASMODERATOR]->(person)` 匹配任意 person，结果错误（原查询 0 行，改写后有行）。现在本段读到前面 QueryPart 的任何绑定时不拆分。
13. 模式绑定不再隐藏 FILL 行数状态 `__neug_fill_state_n_rows_*`（与 11-19 `Binder::reservedInPropertyLookup` 的设计一致；其他私有聚合状态仍隐藏）。
    没有公开 COUNT(*) 列的关系 FILL（如 `bi20_direct = min(...)+1`）的组存在性测试因此能通过验证：bi-20-setup 改写为读 `bi20_direct`
    （BI 17/17，含 setup；用到的索引 75/83）。物化（`cico build`）本来就建这一列。

## 中间件修改

- 部分 QueryPart 已改写、其余未改写时也尝试参数特化（FILL 常按固定窗口建），特化结果改写的段更多才采用。
- 逐段改写一段都没改写时，整条语句再交给 RewriterV4 一次（分阶段 FILL 跨多个 QueryPart，如 tcr11）。
- FILL 源查询注册时与查询走同样的规范化（时间字面量、字面量/属性比较方向），按拼写比较的匹配（DISTINCT 前缀形状）两边一致。
- Kuzu 方言：RewriterV4 由 COUNT 替换出的 SUM（保留 COUNT 的 uid）打印成 `CAST(sum(…) AS INT64)`，Kuzu 的 `sum(INT64)` 是 INT128；
  只在整条语句的最后一个投影里加：Kuzu 不允许再聚合“包着聚合的 WITH 别名”，而逐段改写时后面的段看不到
  （第一版按“同一语句里是否再被聚合”判断，bi-5、bi-6、tcr-9 因此在 Kuzu 上报 nested aggregation，已改）。
- 打印递归关系谓词 `*l..u (r, n | WHERE …)`（引擎 IR 增加 `inner_rel`/`inner_node`），tcr11 的索引才能在 Kuzu 上物化。
- 物化：REL 组子表 FILL 的 reset 只删自己子表的边（以前删整个组，单独重建一个子表会清掉兄弟 FILL）。
- `schemas/` 增加 lsqb、finbench、ldbc-ic、hetionet 的目标 schema（测试用）。

## 实验文档查询的等价改写

见实验文档 `Kuzu/queries/input/README.md` 的表格（lsqb q2/q4–q7、ic5、FinBench 7 条）。每个文件开头注释写明改动与等价理由；
ic5 依赖 LDBC 约束“每对 (forum, person) 至多一条 HASMEMBER”，IC SF1 上检查为 0 违反。

## 未完成

- **ic14**：Kuzu 写法用 `list_transform`（NeuG 不支持），而且每条路径边的 OPTIONAL 计数要把 UNWIND 出来的关系当输入列，
  逐段桩不支持 REL/嵌套列表输入列；需要扩展 `queryparts` 的桩，或在不依赖 `list_transform` 的前提下保证路径节点顺序。
- 两级权重 FILL（`bi15_weight`、`weight_bi19`、`bi20_weight`：建在其他索引列上的逐边计算）：查询按其源查询形状读索引列时
  RewriterV4 能改写（计算前缀路径），但 Kuzu setup 不读索引列，第一轮改写的输出也不是该形状，需要迭代改写 + 输出规范化。
  `bi15w` ×3 带 forum 时间窗口，Kuzu bi-15 没有窗口，语义不同，不适用。ic14_weight_v2 见上。
- 已知 Kuzu 限制：`WITH CASE WHEN sum(x) IS NULL …`（编译器修改 2）之后再聚合该别名会报 nested aggregation，验证守卫在 NeuG 上绑定，
  发现不了。目前全部改写输出经 `scan_nested.py` 检查都不触发；如出现，需要 Kuzu 方言把这类投影拆成两个 WITH。
