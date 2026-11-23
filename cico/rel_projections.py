"""Replace a deterministic per-edge calculation with its stored FILL value.

The source must scan all edges of one relationship type and return their
ordered endpoint keys. An explicit unique-edge constraint guarantees one
source row per carrier edge. Pure intermediate WITHs only name expressions;
the final bound projection already contains their definitions.
"""
import json
import struct

from .canonical import Canonicalizer
from .engine import EngineError
from .printer import children_of
from .statements import parse_fill


_SCALAR_TYPES = {'BOOL', 'INT8', 'INT16', 'INT32', 'INT64', 'INT128',
                 'UINT8', 'UINT16', 'UINT32', 'UINT64', 'FLOAT', 'DOUBLE'}
_SCALAR_FUNCTIONS = {'+', '-', '*', '/', 'CAST', 'SQRT', 'ROUND'}
_OPERATORS = {'EQUALS', 'NOT_EQUALS', 'GREATER_THAN', 'LESS_THAN',
              'GREATER_THAN_EQUALS', 'LESS_THAN_EQUALS'}


def _parts(ir):
    queries = ir.get('single_queries') or []
    if len(queries) != 1 or ir.get('pre_parts') or ir.get('pre_expressions') or ir.get('post_query'):
        return []
    return queries[0]['parts']


def _edge(part):
    reading = part['reading']
    if len(reading) != 1 or part['updating']:
        return None
    clause = reading[0]
    if clause['kind'] != 'MATCH' or clause.get('optional') or clause.get('has_hint'):
        return None
    graphs = clause.get('graphs') or []
    if len(graphs) != 1 or len(graphs[0]['nodes']) != 2 or len(graphs[0]['rels']) != 1:
        return None
    rel = graphs[0]['rels'][0]
    if rel.get('direction') != 'SINGLE' or rel.get('recursive') or len(rel['tables']) != 1:
        return None
    nodes = {node['uid']: node for node in graphs[0]['nodes']}
    src, dst = nodes[rel['src']], nodes[rel['dst']]
    if src['uid'] == dst['uid'] or len(src['tables']) != 1 or len(dst['tables']) != 1:
        return None
    return rel, src, dst


def _signature(edge):
    return tuple(e['tables'][0].upper() for e in edge)


def _plain(part):
    projection = part.get('projection')
    return projection and not part.get('predicate') and not any(
        projection.get(key) for key in ('distinct', 'aggregates', 'skip', 'limit', 'order_by'))


def _literal_is_exact(expression):
    value = expression['value']
    # Nested literals don't carry per-item floating-point bits.
    if 'items' in value:
        return False
    if value.get('null') or expression['type'] not in ('DOUBLE', 'FLOAT'):
        return True
    fmt = '>d' if expression['type'] == 'DOUBLE' else '>f'
    try:
        bits = int.from_bytes(struct.pack(fmt, float(value['text'])), 'big')
    except (ValueError, OverflowError, struct.error):
        return False
    # The printer/materializer must be able to reproduce the exact literal.
    return bits == expression.get('literal_bits')


def _stable(expression, owner, destinations=()):
    if expression.get('unsupported'):
        return False
    kind = expression['k']
    if kind == 'LITERAL' and expression['type'] == 'STRING':
        # Binding a numeric CAST introduces the target type as a string.
        return expression['value'].get('text') in _SCALAR_TYPES
    if expression['type'] not in _SCALAR_TYPES:
        return False
    if kind == 'PROPERTY':
        prop = expression['property']
        return expression.get('owner') == owner and not prop.startswith('_') and prop not in destinations
    if kind == 'LITERAL':
        return _literal_is_exact(expression)
    if kind == 'SCALAR_FUNCTION':
        if expression.get('function', '').upper() not in _SCALAR_FUNCTIONS:
            return False
    elif kind not in _OPERATORS and kind != 'CASE_ELSE':
        return False
    return all(_stable(child, owner, destinations) for child in children_of(expression))


def _key(expression, owner):
    return json.dumps(Canonicalizer({owner: 'edge'}).expr(expression), sort_keys=True)


def _candidates(mw, unique):
    fills = tuple((name, tuple(statements)) for name, statements in mw.fills.items())
    cached = getattr(mw, '_rel_projection_candidates', None)
    if cached and cached[0] is mw.schema and cached[1] == fills:
        return cached[2]
    result = {}
    for name, statements in fills:
        if len(statements) != 1:
            continue
        fill = parse_fill(statements[0])
        if not fill.columns or fill.table.upper() not in unique:
            continue
        kind, table = mw.schema.table(fill.table)
        if kind != 'REL' or len(table['vertex_type_pair_relations']) != 1:
            continue
        try:
            parts = _parts(mw.engine.bind(fill.source, {})['ir'])
        except EngineError:
            continue
        if not parts or any(not _plain(part) for part in parts):
            continue
        edge = _edge(parts[0])
        if edge is None or parts[0]['reading'][0].get('predicate') or any(
                part['reading'] or part['updating'] for part in parts[1:]):
            continue
        rel, src, dst = edge
        pair = table['vertex_type_pair_relations'][0]
        signature = _signature(edge)
        if signature != (fill.table.upper(), pair['source_vertex'].upper(), pair['destination_vertex'].upper()):
            continue
        outputs = parts[-1]['projection']['expressions']
        if len(outputs) != len(fill.columns) + 2:
            continue
        if any(expr['k'] != 'PROPERTY' or expr.get('owner') != node['uid'] or
               mw.schema.table(node['tables'][0])[1].get('primary_keys') != [expr['property']]
               for expr, node in zip(outputs, (src, dst))):
            continue
        properties = mw.schema.properties(fill.table)
        for column, expr in zip(fill.columns, outputs[2:]):
            if expr['type'] == properties.get(column) and _stable(expr, rel['uid'], fill.columns):
                result.setdefault((signature, _key(expr, rel['uid'])), (name, column))
    mw._rel_projection_candidates = mw.schema, fills, result
    return result


def rewrite(mw, internal, types):
    if not mw.profile.enabled('rel_projections') or getattr(mw, '_isolating', False):
        return None
    unique = {item.rel.upper(): item for item in mw.constraints.items if item.kind == 'unique_edge'}
    if not unique:
        return None
    candidates = _candidates(mw, unique)
    if not candidates:
        return None
    try:
        ir = mw.engine.bind(internal, types)['ir']
    except EngineError:
        return None
    parts = _parts(ir)
    if not parts:
        return None
    edge = _edge(parts[0])
    if edge is None:
        return None
    signature = _signature(edge)
    if signature[0] not in unique:
        return None

    rel = edge[0]
    replacements, used = {}, set()
    for part in parts:
        projection = part.get('projection')
        if part is not parts[0] and (part['reading'] or part['updating']):
            break
        for expr in (projection or {}).get('expressions') or []:
            # Preserve the name of the projected column and subsequent alias uses.
            if not expr.get('alias') or 'entity' in expr or not _stable(expr, rel['uid']):
                continue
            selected = candidates.get((signature, _key(expr, rel['uid'])))
            if selected:
                name, column = selected
                replacements[expr['uid']] = column
                used.add(name)
    if not used:
        return None

    def substitute(value):
        if isinstance(value, list):
            return [substitute(item) for item in value]
        if not isinstance(value, dict):
            return value
        column = replacements.get(value.get('uid'))
        if column and value.get('k') and value.get('type'):
            return dict(k='PROPERTY', uid=value['uid'], type=value['type'],
                        alias=value.get('alias', ''), owner=rel['uid'], owner_raw=rel['raw'],
                        property=column, children=[])
        if value.get('k') == 'LITERAL' and not _literal_is_exact(value):
            raise ValueError('inexact printed literal in edge projection query')
        return {key: substitute(item) for key, item in value.items()}

    try:
        return substitute(ir), sorted(used), unique[signature[0]].describe()
    except ValueError:
        return None
