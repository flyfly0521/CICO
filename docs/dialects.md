# 方言转换声明：为一个数据库接入中间件

中间件启动时用一个参数指定目标数据库的方言：

```python
IndexMiddleware(schema, dialect='kuzu', constraints='constraints/ldbc-snb.yaml')
```

```sh
python3 -m cico serve --schema S.yaml --dialect kuzu --constraints constraints/ldbc-snb.yaml ...
```

`dialect` 选中一份**方言转换声明**（`cico/profiles.py`，旧名 profile，`--profile` 仍可用）。普通查询按这个方言读入，改写结果按这个方言输出。**FILL 例外：FILL 后面的查询一律按 NeuG 语义解析**，与 `dialect` 无关。FILL 和 `DROP_INDEX` 像普通语句一样发给中间件；带 `--registry` 时它们写入由中间件维护的索引注册表（见 README“方言参数、FILL 与索引注册表”）。

内置声明：

| dialect | 输入/输出 | 关系唯一性 | 变长路径 | 类型类写法 | 自动启用的规则 |
| --- | --- | --- | --- | --- | --- |
| `neug` | NeuG Cypher（内部方言） | 同态 | walk | 属性 | `scalar_regions`、`edge_carriers`、`constraint_retry`（需加载约束） |
| `kuzu` | Kuzu Cypher | 同态 | walk，`*l..` 上限 30 | 属性（`PLACE.type='City'`） | `scalar_regions`、`edge_carriers`、`constraint_retry`（需加载约束） |
| `neo4j` | Neo4j Cypher + schema mapping | 同构（同一子句关系互异） | trail | 标签（`:City`） | 以上三条 + `path_carriers`、`distinct_partitions` |

最后一列不是在声明里列出来的，而是中间件按每条规则的前提自动推出的（见第 4 节）。`IndexMiddleware(...).profile.rule_status()` 给出每条规则启用或未启用的原因。

## 声明由什么组成

```python
from cico.profiles import Profile, Semantics, register

register('mydb', lambda constraints=None: Profile(
    name='mydb',
    frontend=MyFrontend(),          # 目标 Cypher -> 内部 NeuG Cypher（输入转换）
    dialect=MyDialect(),            # Bound IR -> 目标 Cypher（输出打印）
    semantics=Semantics(...),       # 目标与 NeuG 的匹配语义差异
    disabled=frozenset(),           # 可选：显式关闭某些规则（排查问题用）
    description='...',
    constraints=load_constraints(constraints) if constraints else None))
```

之后 `IndexMiddleware(..., dialect='mydb')` 或 `--dialect mydb` 即可使用。核心流程里**没有任何按方言名字的判断**：只读 `semantics`，并检查 frontend/dialect 提供了哪些钩子。声明里不列规则，规则是否启用由中间件推出（第 4 节）。

### 1. frontend（输入转换）

把目标方言的只读查询翻译成 NeuG 能绑定的内部 Cypher，做不到时抛 `cico.errors.Unsupported`（该 QueryPart 原样透传）。要求：

- **转换成功即可绑定**：转换出来的文本必须能被 NeuG 绑定，NeuG 不支持的写法要在转换阶段判为不支持（`cico/surface.py` 会再检查一遍）。
- **语义差异显式化**：目标与 NeuG 语义不同的地方，要么翻译成等价的内部写法，要么判为不支持。例如 Neo4j 的关系唯一性翻译成 `id(r1) <> id(r2)`。
- 需要的方法：`to_internal(cypher)`、`parameter_types(types)`、`ddl_to_internal(statement)`、`table_to_internal(name)`。语法与 NeuG 相同的系统可以继承 `cico.frontends.KuzuFrontend`；语法差异大的参考 `cico/neo4j.py` 的 `Neo4jFrontend` + mapping 文件。
- FILL 不经过 frontend（FILL 是 NeuG），所以 `fill_to_internal` 不再被调用。

### 2. dialect（输出打印）

继承 `cico.dialects.Dialect`，只覆盖与 NeuG 不同的写法：标识符、标签与标签并集、关系类型、变长路径、字面量、函数名、CAST、物化 DDL（`add_column_ddl`、`create_rel_table_ddl`）和写回（`fill_statement`）。验证守卫会把输出再经 frontend 翻译回内部方言重新绑定并做结构比对，所以打印错误只会导致回退原文，不会产生错误结果。

### 3. semantics（语义差异）

| 字段 | 取值 | 作用 |
| --- | --- | --- |
| `relationship_uniqueness` | `homomorphism` / `isomorphism` | 同一 MATCH 子句里两个关系变量能否绑定同一条边。`isomorphism` 时 frontend 必须补上或证明不等式 |
| `variable_length` | `walk` / `trail` | 变长关系的语义；trail 只在声明为 `acyclic` 的关系类型上与 walk 一致 |
| `variable_length_cap` | 整数或 None | 目标对开放上界 `*l..` 的限制（Kuzu 30） |
| `type_classes` | `property` / `label` | 类型类（City ⊂ PLACE）在目标里怎么写 |
| `procedures` | bool | 是否有带类型契约的不透明 CALL 过程（Neo4j APOC） |
| `native_ddl` | 正则或 None | 目标原生、中间件不解释的语句（如 Neo4j `CREATE INDEX`），原样返回 `route='target'` |

### 4. 规则：共享实现，按前提自动启用

改写规则对所有方言是**同一份实现**，正确性按 NeuG（内部方言）语义证明；与方言有关的部分（怎么打印、类型类怎么写）交给钩子。每条规则在 `cico/profiles.py` 的 `RULES` 里声明自己需要什么：

- 钩子：frontend 或 dialect 上的方法/属性；
- 语义前提：对 `Semantics` 的要求；
- 是否需要数据集约束。

声明满足一条规则的全部前提，规则就自动启用。`get_profile(name, disabled={...})` 或声明里的 `disabled` 可以显式关闭某条规则。

| 规则 | 作用 | 前提 | NeuG/Kuzu 钩子的实现 | Neo4j 钩子的实现 |
| --- | --- | --- | --- | --- |
| `scalar_regions` | 相关 `size()`/`COUNT{}`/`EXISTS{}` 局部改写（BI8） | 无（探针走普通 frontend、打印器和验证守卫） | – | – |
| `edge_carriers` | OPTIONAL 存在性部分改读关系计数载体（BI14） | `dialect.edge_carrier_test`、`frontend.rel_tables` | 两个有向 OPTIONAL MATCH 判断边的方向 | 一个无向匹配加 `startNode(k) = a` |
| `constraint_retry` | 声明约束重试：类型类推导、键身份、常量键（BI10/11 等） | `frontend.relax`、`frontend.key_constants`，并加载约束 | `cico/relax.py`：类型类是表上的谓词，删去被约束蕴含的 `x.type = 'City'` | `neo4j.py`：类型类是标签，`:City` 换成 `:Place` |
| `path_carriers` | 变长路径换成关系载体 | `relationship_uniqueness == 'isomorphism'`，加 frontend 的子句分析 | 不适用：同态语义下普通 QueryPart 改写已经覆盖 | 子句分析 |
| `distinct_partitions` | `CALL { }` 分支里的 DISTINCT 计数 | `frontend.rels`、`rel_pairs`、`constraints` | 未实现（Kuzu 没有 `CALL { }` 子查询） | Neo4j frontend |

为什么 Kuzu 的 `edge_carrier_test` 不用 `start_node()`：Kuzu 0.10 在无向模式 `(a)-[k]-(b)` 上，`start_node(k)` 和 `k._src` 返回的是遍历方向的起点，不是边的存储起点（已在 Kuzu 上确认）。所以改用两个有向 OPTIONAL MATCH。同一对节点之间多条边会产生多行，但这些行的存在性取值相同，规则后面的 `WITH DISTINCT` 会把它们合并。

QueryPart 改写、参数特化、验证守卫、物化与索引注册表对所有方言都启用，不属于上面的规则。

打印器的钩子还包括 `shortest_length`（`* SHORTEST l..u`，Kuzu bi10 用到）和 `path_length`（`length(path)`）。路径上下界里的参数（`1..$max`）NeuG 解析不了，由 `surface.py` 判为“需要字面量”，交给参数特化：输出只对给定的参数值成立，`Result.specialized` 会列出这些参数。

## 数据集约束（与方言无关）

声明约束按**数据集**写一次，用内部表名，放在 `constraints/<dataset>.yaml`，所有方言共用（`constraints/ldbc-snb.yaml` 是例子）：

```yaml
classes:                       # 数据模型的节点类
  Message: {tables: [COMMENT, POST]}
  City: {table: PLACE, property: type, value: City}     # 类型类：表上的谓词
acyclic: [REPLYOF]             # 无环关系类型
constraints:
  - {kind: at_most_one, rel: REPLYOF, from: Message}
  - {kind: endpoint_labels, rel: ISLOCATEDIN, from: Person, to: City}
```

种类：`at_most_one`、`at_least_one`、`endpoint_labels`、`unique_property`、`required_property`（见 `cico/constraints.py`）。约束只是个别改写步骤的前提；用到约束的改写会在 `Result.premises` 中列出。信任这些改写前，要先在目标库上运行 `IndexMiddleware.constraint_checks()` 生成的检查语句，每条的违反数都应为 0。检查语句按方言生成：内部名字的约束生成 NeuG/Kuzu Cypher，Neo4j 生成带标签的 Cypher。

词汇不同的方言在加载时翻译：Neo4j 的 mapping 写 `constraints_file: ../constraints/ldbc-snb.yaml`，`load_mapping` 把关系表名翻译成 Neo4j 关系类型，并检查每个类名都是含义相同的 Neo4j 标签。

## 接入新系统的检查清单

1. 写 frontend 和 dialect，语法与 NeuG 相同的部分直接继承。
2. 填写 `Semantics`，逐项对照目标系统的文档。
3. 实现想用的规则需要的钩子（第 4 节的表）；规则会自动启用。用 `profile.rule_status()` 确认。
4. 用 `register()` 注册（或加入 `cico/profiles.py` 的 `PROFILES`）。
5. 如有执行器（物化、EXPLAIN 预检），参照 `cico/targets.py` 的 `KuzuIndexExecutor` / `Neo4jIndexExecutor`；注册表（`cico/registry.py`）与方言无关，不需要改。
6. 回归：`tests/test_dialects.py`、`tests/test_rules.py` 的写法；用目标真机验证物化语句可执行、改写输出可 EXPLAIN。对每条启用的规则，在小图上对比改写前后的结果多重集，参照 `tests/kuzu_rules_acceptance.py`。
