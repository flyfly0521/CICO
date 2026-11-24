# NeuG 查询经 CICO 改写的覆盖（2026-11-24）

目标（用户 2026-11-24）：把中间件用到 NeuG（IncGQ）上。NeuG 索引全部经 CICO 注册，不使用 NeuG 引擎自带的索引处理；
然后检查所有 NeuG 查询能否被 NeuG 索引改写。只检查改写结果，不在库上执行。

脚本：`tests/neug_coverage.py`（结果在 `cico_work/cov_neug_base_d.*`）。

- 方言 `neug`；schema 用实验文档的 NeuG 导入配置（`NeuG/scripts/import/configs/`：BI `ldbc/graph-bi.yaml`、IC `ldbc/graph.yaml`、
  FinBench、LSQB）。Hetionet 用本地库的 `graph.yaml`。`/mnt/sdb1/feiyang/databases/` 下的 NeuG 库是旧导入，例如 IC 的
  `joinDate` 在那里是 INT64。
- 索引：`NeuG/queries/index/<workload>/` 的 DDL/FILL 全部发给 CICO（`register_indexes`），注册顺序、模板和参数与 Kuzu/Neo4j
  覆盖率脚本相同。
- 查询：NeuG runner（`NeuG/scripts/benchmark/run_workload.py`，只导入、不运行）的 baseline 查询集合（`queries_for`）、
  baseline setup（`baseline_setup_path`，其中 baseline 自有的 FILL 同样经 CICO 注册）、`{}` 模板（`fill_template`）和写语句判定。
- 参数：共享清单第 0 组（即索引所用的参数值），用 NeuG runner 自己的函数转换。FinBench 的时间窗口在 runner 里是 epoch
  毫秒整数，NeuG 执行时把它当作对应时刻；交给 CICO 时转成同一个 UTC 时刻的 datetime（CICO 按值的类型绑定参数）。

## 结果

| workload | 只读查询改写 | 读到的已注册索引 |
| --- | --- | --- |
| hetionet | 26/26 | 23/23 |
| ldbc-bi | 11/21 | 12/31 |
| ldbc-ic | 1/5（ic5） | 1/2 |
| lsqb | 4/9（q1、q4、q5、q8） | 6/13 |
| finbench | 6/21（tcr-6/7/12/13、tsr-4/5），另有 15 条写语句 | 6/16 |
| 合计 | **48/82** | **48/85** |

BI 改写成功的：bi2、bi3、bi4（只读 ROOT_POST）、bi5、bi8、bi9、bi10、bi11、bi13、bi16、bi17。

未改写的 34 条：

- **没有自己的索引目录（16 条）**：bi12、ic1、ic3、ic12、q3、q9、tcr-1/2/3/4/5/8、tsr-1/6/7/8。其中 bi12 在 Kuzu 上靠 input 版本
  （`count(message)` → `count(DISTINCT message)`）用上了 ROOT_POST。
- **WSHORTEST（7 个文件）**：bi15a/baseline、bi19a/shortest_path、bi20/baseline 是主查询；bi15a/parameterized、
  bi19a/without_precomputation(_score) 是变体，NeuG runner 没有登记它们的模板映射；bi15a/parameterized_schema 是 DDL 模板。
  CICO 的编译器副本没有登记 WSHORTEST（执行体是桩），Kuzu/Neo4j 上的 bi-15/19/20 主查询也一样。NeuG 的权重在 baseline
  setup 里本来就由 baseline 自有的 FILL 物化（bi15_base_*、bi20_base_*），这些 FILL 经 CICO 注册成功。
- **有索引但形状不匹配（11 条）**：bi14a（REL 列无法作为下一 QueryPart 的输入桩）、bi6、ic14、q2、q6、q7、tcr-9、tcr-10、
  tcr-11、tsr-2、tsr-3。Kuzu 上与它们对应的 baseline 也都需要 input 版本（见 kuzu-input-rewrites.md）；NeuG 的对应写法在 `NeuG/queries/optimized`，见下节。
  bi4 只读到了 ROOT_POST，没读到 BI4_MESSAGE_COUNT。

没读到的 37 个索引对应上面几类：bi14 系列（含 bi15/bi19a 的一级方向计数）×8、bi15u/bi15/bi15w 的权重与窗口计数 ×5、
bi19u/bi19a 权重 ×2、bi20 ×2、BI4_MESSAGE_COUNT、
PERSON(likeCount)；ic14_weight_v2；q2 ×2、q6、q7 ×4；tcr9 ×3、tcr10 ×2、tcr11、tsr2 ×2、tsr3 ×2。
另有 BI 合并目录里 4 个 FILL（bi3、bi9、bi14 ×2）因载体归属冲突注册失败，Kuzu/Neo4j 上也一样。

## 修改

- `cico/schema.py`：导入配置把时间类型写成 `temporal: timestamp`。NeuG 把它与 `datetime` 读成同一个类型（`kTimestampMs`，
  写回库时为 `datetime`），而 CICO 的编译器元数据会读成 TIMESTAMP64，`TIMESTAMP('…')` 字面量与它无法比较。CICO 现在加载时
  规范为 `datetime`（`tests/test_neug_schema.py`）。修复后 ic5 改写成功。
- 编译器同步到 IncGQ `c988ea9d`（`tools/sync_compiler.py --clean`）：唯一的上游变化是 RewriterV4 中跨 QueryPart 引用替换时
  聚合名的匹配从子串改为相等；补丁全部照常应用，Kuzu/Neo4j 覆盖率不变（84/85、82/85，各 63/106）。
- `tests/workloads.py`：`parameters(..., text=None)` 返回整组参数。

## NeuG input（`NeuG/queries/optimized`）

NeuG 的 optimized 查询是 NeuG 改写器的输入：多数保留原图查询形态，由 FILL 元数据触发改写（见其 README），作用与
Kuzu/Neo4j 的 `queries/input` 相同。CICO 的 `neug` 方言就是 NeuG 本身（`IdentityFrontend`），查询原样进入编译器，不做方言转换。
`tests/neug_coverage.py --query-set optimization`（结果 `cico_work/cov_neug_opt_a.*`）：

| workload | 只读查询改写 | 读到的已注册索引 |
| --- | --- | --- |
| hetionet | 24/26 | 22/23 |
| ldbc-bi | 7/13 | 7/31 |
| ldbc-ic | 2/5（ic5、ic14） | 2/2 |
| lsqb | 6/9（q1、q4–q8） | 11/13 |
| finbench | 10/11（除 tcr-10） | 12/16 |
| 合计 | **49/64** | **54/85** |

与 baseline 合起来，85 个已注册索引中 60 个至少被一组查询读到。

input 中没有改写的 15 条：

- 已经手写为读物化属性，不需要改写：hetionet 3-07、3-12，bi11。
- bi4 读 `forum.bi4MaxCountryMemberCount`，`NeuG/queries/index` 中没有任何脚本建这一列（文档自身不一致）。
- WSHORTEST：bi15a、bi19a、bi20。
- 没有索引：ic1、ic3、ic12、q3、q9。
- 有索引但没有匹配上：bi14a、q2、tcr-10。

两组都没读到的 25 个索引：bi14 系列 ×8、bi15/bi15u/bi15w ×5、bi19 ×2、bi20 ×2、BI4_MESSAGE_COUNT、PERSON(likeCount)；
q2 ×2；tcr9TransferIn（与 tcr6TransferIn 定义相同，tcr-9 读的是后者）、tsr2TransferIn（tsr-2 只读到了出边那一组）、tcr10 ×2。

## NeuG rewrite（`NeuG/queries/rewrite`）

`NeuG/queries/rewrite/` 是从 optimized 中挑出的两类查询，按原相对路径复制，optimized 本身不动：
与 baseline 文本相同的 25 条，以及不读物化结果的其他图模式写法 29 条。筛选规则和清单见该目录的 README，分类脚本为
`cico_work/tools/classify_opt.py`。Kuzu、Neo4j 的 optimized 全部手写读取物化结果，没有符合条件的查询。

`tests/neug_coverage.py --query-set rewrite`（结果 `cico_work/cov_neug_rewrite_a.*`）：**49/54** 改写成功，读到 54/85 个索引。
未改写的 5 条中，ic12、q3、q9 没有索引；有索引而没有匹配上的只有 bi14a 和 q2。
