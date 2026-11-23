# 新数据库接入指南

本文说明如何把 CICO 接入一个新的图数据库：从“能改写”到“能自己建索引”，再到上线前的验证。按步骤做，语法与 Kuzu 接近的系统通常半天到一天即可完成第 1–4 步。

可运行的完整示例：[`example_mydb.py`](example_mydb.py)（虚构的目标库 `mydb`，与 Kuzu 只差一处写法），测试见 `tests/test_guide_example.py`。各接口的参考说明见 [`docs/dialects.md`](../docs/dialects.md)。

## 0. 先理解要做什么

```
目标库的 Cypher ──frontend──► 内部 Cypher（NeuG 方言）──cico_engine──► 改写方案（Bound IR）──dialect──► 目标库的 Cypher
                                                                                                    │
                                    验证守卫：输出再经 frontend 翻回内部方言、重新绑定、与方案比对 ◄──┘
```

接入一个数据库 = 写一份**方言声明**（`Profile`），由三部分组成：

| 部分 | 作用 | 写错的后果 |
| --- | --- | --- |
| `frontend` | 输入转换：目标 Cypher → 内部 Cypher | 查询翻译不了，原样返回；或守卫比对失败，原样返回 |
| `dialect` | 输出打印：改写方案 → 目标 Cypher | 守卫比对失败，原样返回 |
| `semantics` | 目标库与 NeuG 在匹配语义上的差异 | **可能产生不等价的改写**，务必逐项核对 |

改写规则、QueryPart 切分、参数特化、验证守卫、索引注册表对所有方言共用，**不需要修改**。核心流程里没有按方言名字的判断。

FILL（索引定义）后面的查询**始终按 NeuG 语义**解析，与目标方言无关；所以接入新库不影响已有的索引定义。

## 1. 评估目标库（约 30 分钟）

先用现成方言跑一遍你的查询，看差异有多大：

```python
from cico import IndexMiddleware
with IndexMiddleware('your-schema.yaml', dialect='kuzu') as mw:   # 或 'neug'
    mw.execute('FILL ...')                    # 你的索引定义
    r = mw.execute(your_query)
    print(r.reason, r.detail)
```

| 目标库特点 | 选择 |
| --- | --- |
| openCypher，语法与 Kuzu/NeuG 基本一致 | 继承 `KuzuFrontend` / `KuzuDialect`，只改不同的写法（本指南的主线） |
| 标签模型、动态类型、语义差异大（类似 Neo4j） | 参照 `cico/neo4j.py`：`Neo4jFrontend` + schema 映射文件（`profiles/*.yaml`） |

`Result.reason` 的分布能告诉你问题在哪：

- `frontend_unsupported`：输入里有翻译不了的写法 → 补 frontend；
- `verification_*`（如 `verification_frontend_failed`、`verification_mismatch`）：输出写法不对，或输出翻不回来 → 补 dialect 或 frontend；
- `no_index_selected`：没有可用索引，或编译器不支持该查询形状（例如分组键或过滤条件包在函数里），与方言无关。

同时整理一份目标库与 Kuzu 的**差异清单**：函数名、聚合名、字面量（日期、时长）、CAST、变长路径写法、标识符引用、DDL 与写回语法，以及第 3 步的语义差异。

## 2. 准备 schema

中间件需要一份描述**目标库实际类型**的 schema YAML，格式同 [`schemas/ldbc-bi.yaml`](../schemas/ldbc-bi.yaml)（`vertex_types`、`edge_types`、`vertex_type_pair_relations`）。

- Kuzu：`python3 -m cico schema --kuzu <db> --reference <参考 schema> --output S.yaml`；
- 其他库：手写，或参照 `cico/introspect.py` 写一个提取器。

类型必须与目标库一致。同一数据集在不同库里可能不同（例如 `creationDate` 在 NeuG 是 INT64，在 Kuzu/Neo4j 是 TIMESTAMP），写错会让改写结果不成立。

## 3. 写方言声明

新建 `cico/dialects/<yourdb>.py`，可以直接从 [`example_mydb.py`](example_mydb.py) 复制。示例中的目标库把聚合 `sum()` 写成 `total()`：

```python
from cico.constraints import load_constraints
from cico.dialects.neug import KuzuDialect
from cico.frontends import KuzuFrontend
from cico.neo4j import render, tokenize
from cico.profiles import Profile, Semantics

TO_INTERNAL = {'TOTAL': 'sum'}          # 目标写法（大写）-> 内部写法

class MyDbFrontend(KuzuFrontend):       # 输入转换
    name = 'mydb'
    def to_internal(self, cypher):
        tokens = tokenize(cypher)       # 在词法单元上改，不要做字符串替换（字面量里可能有同名文本）
        for tok, nxt in zip(tokens, tokens[1:]):
            if tok.kind == 'ident' and nxt.text == '(' and tok.upper in TO_INTERNAL:
                tok.text = TO_INTERNAL[tok.upper]
        return super().to_internal(render(tokens))

class MyDbDialect(KuzuDialect):         # 输出打印：只覆盖不同的写法
    name = 'mydb'
    def aggregate(self, name, args, distinct):
        text = super().aggregate(name, args, distinct)
        return 'total' + text[len('sum'):] if name == 'SUM' else text

def mydb_profile(constraints=None):
    return Profile(
        name='mydb', frontend=MyDbFrontend(), dialect=MyDbDialect(),
        semantics=Semantics(relationship_uniqueness='homomorphism', variable_length='walk',
                            variable_length_cap=None, type_classes='property'),
        description='...',
        constraints=load_constraints(constraints) if constraints else None)
```

### 3.1 frontend（输入）

- `to_internal(cypher)`：转换后的文本必须能被 NeuG 绑定。语义与 NeuG 不同的写法，要么翻译成等价的内部写法，要么抛 `cico.errors.Unsupported`。抛出后该 QueryPart 原样透传，其他 QueryPart 照常改写。
- 视需要覆盖：`parameter_types(types)`（参数类型名映射）、`ddl_to_internal(statement)`（目标 DDL → 内部）、`table_to_internal(name)`（表名映射）。
- 可复用的工具：`cico.neo4j.tokenize/render`（词法单元）、`cico/normalize.py`（`coalesce` 转 CASE 等规范化）。

### 3.2 dialect（输出）

继承 `KuzuDialect`（或 `cico.dialects.base.Dialect`），常用钩子：

| 钩子 | 管什么 |
| --- | --- |
| `ident`、`node_pattern`、`rel_types`、`property` | 标识符、标签、关系类型、属性 |
| `var_length`、`shortest_length`、`path_length` | 变长路径与最短路径 |
| `literal`、`temporal_literal`、`string`、`parameter` | 字面量与参数 |
| `aggregate`、`function`、`call`、`SPELLING`、`cast`、`label_of` | 聚合、函数名、CAST |
| `match_text` | MATCH 子句如何拼写（同构语义的库要拆子句，见 `neo4j.py`） |
| `add_column_ddl`、`create_rel_table_ddl`、`fill_statement` | 物化：建载体列/派生边表、写回语句 |

不确定输出对不对时不用担心：守卫会把输出经 frontend 翻回内部方言重新绑定，不一致就返回原查询，不会产生错误结果。

### 3.3 semantics（务必逐项核对目标库文档）

| 字段 | 取值 | 含义 |
| --- | --- | --- |
| `relationship_uniqueness` | `homomorphism` / `isomorphism` | 同一 MATCH 中两个关系变量能否绑定同一条边。Kuzu/NeuG 可以（同态），Neo4j 不可以（同构）；同构时 frontend 必须补上 `id(r1) <> id(r2)` 或证明它们不可能相同 |
| `variable_length` | `walk` / `trail` | 变长路径能否重复经过同一条边 |
| `variable_length_cap` | 整数或 `None` | 目标对开放上界 `*l..` 的隐含上限（Kuzu 为 30） |
| `type_classes` | `property` / `label` | City ⊂ PLACE 这类类型类写成属性谓词还是标签 |
| `procedures` | bool | 是否有带类型契约的不透明 `CALL` 过程 |
| `native_ddl` | 正则或 `None` | 目标原生、中间件不解释的语句（如 `CREATE INDEX`），原样返回给目标库 |

**这是接入中唯一会影响正确性的部分。** frontend 和 dialect 的错误都会被守卫拦下，semantics 声明错了则拦不住。

## 4. 注册并试跑

在 `cico/profiles.py` 的 `PROFILES` 中加入 `'mydb': mydb_profile`（命令行和 HTTP 服务通过它找到方言）。只在 Python 中使用时，也可以直接调用 `cico.profiles.register('mydb', mydb_profile)`。

```sh
python3 guide/example_mydb.py        # 先跑通示例
python3 -m cico rewrite --schema S.yaml --dialect mydb --index-script index.cypher --query "MATCH ..."
```

示例的输出（改写后用 `total()` 打印）：

```
rewrite: rewritten
MATCH (p:PERSON)-[cico_r1:ISLOCATEDIN]->(c:PLACE) WHERE (p.knowsCount <> 0)
RETURN c.name AS city, total(CASE WHEN (p.knowsCount IS NULL) THEN 0 ELSE p.knowsCount END) AS n
```

查看哪些增强规则已经启用：

```python
mw.profile.rule_status()
# {'scalar_regions': 'enabled', 'edge_carriers': 'enabled',
#  'constraint_retry': 'no declared constraints loaded', ...}
```

基础的 QueryPart 改写不需要任何额外钩子。增强规则在满足前提后自动启用：

| 规则 | 需要 |
| --- | --- |
| `scalar_regions` | 无 |
| `edge_carriers` | `dialect.edge_carrier_test`、`frontend.rel_tables`（继承 Kuzu 即有） |
| `constraint_retry` | `frontend.relax`、`frontend.key_constants`（继承 Kuzu 即有），并加载数据集约束 `constraints/<dataset>.yaml` |
| `path_carriers` | `relationship_uniqueness='isomorphism'`，加 frontend 的子句分析钩子 |
| `distinct_partitions` | `frontend.rels`、`rel_pairs`、`constraints` |

到这一步已经可以使用：中间件返回改写后的 Cypher，由你的应用发给目标库执行。索引可以手动建：`python3 -m cico build ... --dialect mydb` 生成语句，按“全部 ddl → 全部 reset → 全部 fill”的顺序执行。

## 5. （可选）让中间件自己建索引

想让 `FILL` / `DROP_INDEX` 经索引注册表自动物化，需要一个**执行器**。接口很小（见 `cico/targets.py`）：

```python
class MyDbIndexExecutor:
    def __init__(self, client):
        self.client = client
        self.target = 'mydb://host/db#<物理库标识>'   # 标识一个物理库；复制出的库要用不同的标识

    def __call__(self, statement):        # 执行一条语句，提交后才返回
        self.client.run(statement)

    def validate(self, statements):       # 可选：首次写入前预检全部语句（如 EXPLAIN）
        for s in statements:
            self.client.run('EXPLAIN ' + s)

    def release(self):                    # 可选：构建结束后释放连接（嵌入式库需要）
        ...
```

```python
from cico.registry import IndexRegistry
executor = MyDbIndexExecutor(client)
registry = IndexRegistry(mw, 'mydb.registry.yaml', 'mydb.state.json', executor.target, executor=executor)
registry.start()                                   # 编译注册表，构建尚未建好的索引
registry.fill('FILL PERSON(knowsCount) FROM (...)')  # 预检 → 写注册表 → 按依赖顺序构建 → 启用
registry.drop('PERSON(knowsCount)')
```

注册表本身与方言无关，不需要改。有 schema 的库，新载体列要在 DDL 之后才能绑定，`validate` 可以参照 `KuzuIndexExecutor`：在事务里先执行 DDL、再 EXPLAIN 其余语句、最后回滚。命令行（`cico serve/import --registry`）目前只接了 Kuzu 和 Neo4j 执行器，新库需要在 `cico/__main__.py` 中按同样方式加一个分支。

## 6. 验证（上线前必做）

1. **单元测试**：参照 `tests/test_guide_example.py`、`tests/test_dialects.py`，为新方言写“能改写”和“写错时回退”两类用例。
2. **约束检查**：用了数据集约束时，在目标库上执行 `mw.constraint_checks()` 生成的每条检查语句，违反数都必须为 0。用到约束的改写会在 `Result.premises` 中列出。
3. **语句可执行**：在库副本上执行 `build_statements()` 生成的建索引语句，并对改写输出做 EXPLAIN。
4. **结果等价**：在小图或库副本上，对每条启用的规则和你的典型查询，比较改写前后的**结果多重集**，参照 `tests/kuzu_rules_acceptance.py`、`tests/target_validation.py`。
5. **回归**：`python3 -m unittest discover -s tests`。

## 7. 检查清单

- [ ] schema 描述的是目标库的实际类型
- [ ] frontend：转换后能被 NeuG 绑定；翻译不了的写法抛 `Unsupported`
- [ ] dialect：只覆盖不同的写法，物化 DDL 与写回语句在目标库上可执行
- [ ] semantics：关系唯一性、变长路径、上界、类型类逐项对照目标库文档
- [ ] 已加入 `PROFILES`，`rule_status()` 符合预期
- [ ] （可选）执行器：`__call__`、`validate`、`target`
- [ ] 约束检查全部为 0；改写前后结果多重集一致；单元测试通过

## 常见问题

**改写没有发生，`reason` 是 `no_index_selected`。** 先换成 `dialect='neug'` 跑同一查询：如果也不改写，说明是索引定义或编译器支持范围的问题，与方言无关。

**`verification_frontend_failed`。** 守卫把你的输出翻回内部方言时失败了：通常是 dialect 打印了某个写法，frontend 却不认识（示例里如果 frontend 不把 `total` 翻回 `sum`，就是这个结果）。

**能不能不写 frontend？** 能：继承 `KuzuFrontend` 什么都不覆盖即可。前提是目标库的查询写法与 Kuzu 完全一致。
