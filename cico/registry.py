"""Index registry: the middleware's own, formatted record of its indexes.

The registry file is written only by the middleware, from two statements it
receives like any other:

    FILL T(c1, ...) FROM (<NeuG query>)     add T(c1, ...), or replace it
    DROP_INDEX T(c1, ...)                   remove it (DROP_INDEX IF EXISTS ...)

A FILL's query always has NeuG semantics, whatever the middleware's dialect.
An index is identified by its carrier: the table and the column list.  The
registry stores, per index, the canonical (normalized) NeuG FILL, the NeuG
declaration of its carrier, its dependencies and the text as submitted:

    version: 1
    indexes:
    - id: PERSON(cached)
      fill: FILL PERSON(cached) FROM (MATCH ... RETURN p.id AS cico_fill_0, count(c) AS cico_fill_1)
      carrier: [ALTER TABLE PERSON ADD cached INT64]
      depends_on: []
      submitted: "FILL Person(cached) FROM (MATCH ...)"
      updated: '2026-11-18T08:00:00+00:00'

It is logical: the same registry can serve several targets.  Which
definitions are physically built is per target, in the build receipts
(``state_file``, keyed by the target identity).  A definition enters the
rewrite catalog only with a successful receipt for this target.

Every change compiles the whole registry first (binding, carrier overlap,
dependencies, materialization plans), EXPLAINs the pending target statements
(``executor.validate``), and only then writes the registry atomically,
deactivates what will be rebuilt, builds in dependency order and activates
each index once its receipt is durable.  A failure before the write changes
nothing; a build failure keeps the registered definition, leaves it and its
unbuilt dependents inactive, and ``build()`` retries.
"""
import contextlib
import copy
import datetime
import fcntl
import hashlib
import json
import os
import re
import tempfile
from collections import OrderedDict
from pathlib import Path

import yaml

from .engine import Engine
from .middleware import IndexMiddleware, inline_parameters
from .neo4j import tokenize
from .statements import classify, parse_fill, split_script, strip_comments

HEADER = ('# CICO index registry. Written by the middleware from FILL / DROP_INDEX statements;\n'
          '# do not edit by hand. See cico/registry.py.\n')


class RegistryError(ValueError):
    """stage: parse | bind | register | plan | dependencies | dependents |
    validate | build | missing."""

    def __init__(self, stage, message, index_id=None, report=None):
        self.stage, self.index_id, self.report = stage, index_id, report
        super().__init__(f'[{stage}]{" " + index_id + ":" if index_id else ""} {message}')

    def to_dict(self):
        return {'stage': self.stage, 'index_id': self.index_id, 'error': str(self)}


def _digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=False,
                                     separators=(',', ':')).encode()).hexdigest()


def _tokens(text):
    # Plans are printed by the compiler, which already normalizes keyword
    # case. Do NOT lowercase identifiers or string literals here.
    return [(t.kind, t.text) for t in tokenize(strip_comments(text))]


def _walk(value):
    if isinstance(value, dict):
        yield value
        for child in value.values():
            yield from _walk(child)
    elif isinstance(value, list):
        for child in value:
            yield from _walk(child)


def _now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds')


def index_id(table, columns):
    return f'{table.upper()}({",".join(columns)})'


def parse_drop_index(statement):
    """DROP_INDEX [IF EXISTS] T(c, ...) -> (index id, if_exists) or None."""
    s = strip_comments(statement).strip().rstrip(';').strip()
    m = re.fullmatch(r'DROP_INDEX\s+(IF\s+EXISTS\s+)?(`[^`]+`|[A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*)\)', s, re.I | re.S)
    if not m:
        return None
    table = m[2][1:-1] if m[2].startswith('`') else m[2]
    columns = [c.strip().strip('`') for c in m[3].split(',') if c.strip()]
    return index_id(table, columns), bool(m[1])


class IndexRegistry:
    VERSION = 1          # registry file format
    PLAN_FORMAT = 1      # fingerprint context; unchanged from the earlier FILL-file catalog

    def __init__(self, middleware, path, state_file, target, executor=None):
        if middleware.fills:
            raise ValueError('Attach the registry before registering other indexes')
        if not target:
            raise ValueError('A physical target database identity is required')
        self.mw = middleware
        self.path = Path(path).expanduser().resolve()
        self.state_file = Path(state_file).expanduser().resolve()
        self.target = target
        self.executor = executor
        self.base_schema = middleware.schema.copy()
        self.entries = self._read_registry()
        self.last_report = None
        middleware.registry = self

    # ------------------------------------------------------------- public
    def start(self):
        """Compile the registry, enable definitions with a matching receipt
        and, with an executor, build the others."""
        return self._change(None)

    def fill(self, statement, parameters=None, carrier=None):
        """Add or replace the index a FILL statement defines.  `carrier`:
        optional NeuG DDL declaring its carrier (import of documented
        scripts); otherwise the existing declaration is kept, or inferred."""
        return self._change(('fill', statement, parameters, carrier))

    def drop(self, iid, if_exists=False):
        return self._change(('drop', iid, if_exists))

    def build(self):
        """Retry pending builds (e.g. after a failed one)."""
        return self._change(None)

    def import_script(self, text, parameters=None):
        """Import a NeuG index script (DDL + FILL, e.g. the experiment docs):
        each FILL becomes an entry whose carrier is the script's DDL for it.
        DROP TABLE and statements of other kinds are not imported."""
        declared, changes = [], []
        for statement in split_script(text):
            kind = classify(statement)
            if kind == 'ddl':
                if not re.match(r'\s*DROP\b', statement, re.I):
                    declared.append(statement)
            elif kind == 'fill':
                fill = parse_fill(inline_parameters(statement, parameters or {}))
                if fill is None:
                    raise RegistryError('parse', f'invalid FILL statement: {statement[:100]}')
                changes.append(('fill', statement, parameters, [d for d in declared if _declares(d, fill)]))
            elif kind != 'noop':
                raise RegistryError('parse', f'not an index definition: {statement[:100]}')
        return self._change(changes)

    def describe(self):
        state = self._read_state()['indexes']
        return {'registry': str(self.path), 'target': self.target,
                'indexes': [dict(e, status=state.get(e['id'], {}).get('status', 'pending'),
                                 active=e['id'] in self.mw.fills) for e in self.entries]}

    # ------------------------------------------------------------ storage
    def _read_registry(self):
        if not self.path.exists():
            return []
        doc = yaml.safe_load(self.path.read_text()) or {}
        if doc.get('version') != self.VERSION or not isinstance(doc.get('indexes'), list):
            raise ValueError(f'{self.path}: not an index registry (version {self.VERSION})')
        return doc['indexes']

    def _write_registry(self, entries):
        doc = {'version': self.VERSION, 'base_schema': _digest(self.base_schema.doc), 'indexes': entries}
        _atomic_write(self.path, HEADER + yaml.dump(doc, Dumper=_Dumper, sort_keys=False, allow_unicode=True,
                                                    width=1 << 20))

    def _read_state(self):
        if not self.state_file.exists():
            return {'version': self.VERSION, 'target': self.target, 'indexes': {}}
        state = json.loads(self.state_file.read_text())
        if state.get('version') != self.VERSION or state.get('target') != self.target:
            raise ValueError('Index state version/physical target mismatch; use a separate state file')
        return state

    def _save_state(self, state):
        # A crash never turns a partial file into a successful build receipt.
        _atomic_write(self.state_file, json.dumps(state, ensure_ascii=False, indent=2) + '\n')

    @contextlib.contextmanager
    def _locked(self):
        self.state_file.parent.mkdir(parents=True, exist_ok=True)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self.mw._lock, open(str(self.path) + '.lock', 'a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            yield

    # ------------------------------------------------------------ changes
    def _change(self, changes):
        if changes is not None and not isinstance(changes, list):
            changes = [changes]
        try:
            with self._locked():
                self.entries = self._read_registry()   # another process may have changed it
                entries, notes = list(self.entries), []
                for change in changes or []:
                    entries, note = (self._apply_fill(entries, *change[1:]) if change[0] == 'fill'
                                     else self._apply_drop(entries, *change[1:]))
                    notes.append(note)
                return self._commit(entries, notes, write=any(n['status'] != 'unchanged' for n in notes))
        finally:
            release = getattr(self.executor, 'release', None)
            if release is not None:
                release()

    def _stage(self):
        mw = self.mw
        return IndexMiddleware(self.base_schema.copy(), profile=copy.deepcopy(mw.profile),
                               verify=mw.verify, engine_binary=mw._engine_args[0],
                               engine_log=mw._engine_args[1], specialize=mw.specialize, scope=mw.scope)

    def _apply_fill(self, entries, statement, parameters, carrier):
        text = inline_parameters(strip_comments(statement).strip().rstrip(';').strip(), parameters or {})
        if any(t.kind == 'param' for t in tokenize(text)):
            raise RegistryError('parse', 'unbound FILL parameters (pass their values)')
        fill = parse_fill(text)
        if fill is None or classify(text) != 'fill':
            raise RegistryError('parse', f'not a FILL statement: {text[:100]}')
        iid = index_id(fill.table, fill.columns)
        old = next((e for e in entries if e['id'] == iid), None)
        if carrier is None:
            carrier = list(old['carrier']) if old else []
        stage = self._stage()
        try:
            # The new definition is bound with every other carrier declared
            # (it may read them) and its own declared or to be inferred.
            try:
                for entry in entries:
                    if entry['id'] != iid:
                        for ddl in entry['carrier']:
                            stage.apply_ddl(ddl, input_dialect='neug')
                for ddl in carrier:
                    stage.apply_ddl(ddl, input_dialect='neug')
                canonical = self._canonical(stage, fill)
                candidate = stage.schema.copy()
                carrier = carrier + stage._infer_carriers(parse_fill(canonical), candidate)
            except RegistryError:
                raise
            except Exception as error:
                raise RegistryError('bind', error, iid) from error
        finally:
            stage.close()
        entry = {'id': iid, 'fill': canonical, 'carrier': carrier, 'depends_on': [],
                 'submitted': text, 'updated': _now()}
        if old is not None and old['fill'] == canonical and old['carrier'] == carrier:
            return entries, {'status': 'unchanged', 'index_id': iid}
        if old is None:
            return entries + [entry], {'status': 'added', 'index_id': iid}
        return [entry if e['id'] == iid else e for e in entries], {'status': 'changed', 'index_id': iid}

    def _apply_drop(self, entries, iid, if_exists):
        if not any(e['id'] == iid for e in entries):
            if if_exists:
                return entries, {'status': 'unchanged', 'index_id': iid}
            raise RegistryError('missing', 'no such index in the registry', iid)
        dependents = [e['id'] for e in entries if iid in e.get('depends_on', [])]
        if dependents:
            raise RegistryError('dependents', f'read by {", ".join(dependents)}; drop those first', iid)
        return [e for e in entries if e['id'] != iid], {'status': 'removed', 'index_id': iid}

    def _canonical(self, stage, fill):
        bound = stage.engine.bind(fill.source)
        _check_count_presence(stage, bound['ir'])
        from .printer import Printer
        from .dialects import get_dialect
        dialect = get_dialect('neug')
        dialect.set_schema(stage.schema)
        ir = copy.deepcopy(bound['ir'])
        projection = ir['single_queries'][0]['parts'][-1]['projection']
        printer = Printer(dialect)
        entity_names = printer._assign_names(ir)
        occupied = {t.text.strip('`') for t in tokenize(fill.source) if t.kind in ('ident', 'quoted')}
        names = []
        for i, expression in enumerate(projection['expressions']):
            name = f'cico_fill_{i}'
            while name in occupied:
                name += '_'
            if 'entity' in expression:
                name = entity_names[expression['entity']]
            occupied.add(name)
            names.append(name)
        projection['names'] = names
        printed = printer.print(ir).cypher
        # The registered text is this stable form too: NeuG derives hidden
        # state column names from the source text, not its bound IR.
        return (f'FILL {dialect.ident(fill.table.upper())}({",".join(dialect.ident(c) for c in fill.columns)}) '
                f'FROM ({printed})')

    def _compile(self, entries):
        """(stage, plans, order, dependencies, fingerprints) of `entries`."""
        stage = self._stage()
        try:
            for entry in entries:
                for ddl in entry['carrier']:
                    try:
                        stage.apply_ddl(ddl, input_dialect='neug')
                    except Exception as error:
                        raise RegistryError('register', f'carrier declaration {ddl!r}: {error}', entry['id'])
            for entry in entries:
                try:
                    stage.register_index([entry['fill']], entry['id'], input_dialect='neug')
                except Exception as error:
                    raise RegistryError('register', error, entry['id']) from error
            if stage.indexes()['failed']:
                raise RegistryError('register', f'FILL candidates failed: {stage.indexes()["failed"]}')
            plans = OrderedDict()
            for item in stage.build_statements(include_internal_state=True):
                if not item['ok']:
                    raise RegistryError('plan', f'cannot materialize: {item["error"]}', item['index_id'])
                plans.setdefault(item['index_id'], []).append(item)
            try:
                order, dependencies, fingerprints = self._definitions(stage, plans)
            except ValueError as error:
                raise RegistryError('dependencies', error) from error
            return stage, plans, order, dependencies, fingerprints
        except BaseException:
            stage.close()
            raise

    def _commit(self, entries, notes, write):
        executor = self.executor
        state = self._read_state()
        stage, plans, order, dependencies, fingerprints = self._compile(entries)
        try:
            for entry in entries:
                entry['depends_on'] = dependencies[entry['id']]
            previous = state['indexes']
            ready = [i for i in order if previous.get(i, {}).get('status') == 'ready'
                     and previous[i].get('fingerprint') == fingerprints[i]]
            pending = [i for i in order if i not in ready]
            report = {'registry': str(self.path), 'target': self.target, 'changes': notes,
                      'unchanged': list(ready), 'pending': list(pending), 'built': [], 'failed': {},
                      'active': list(ready), 'removed': sorted(set(previous) - set(order)),
                      'dependencies': dependencies, 'fingerprints': fingerprints,
                      'plans': [p for i in pending for p in plans[i]]}
            validator = getattr(executor, 'validate', None)
            if validator is not None and pending:
                try:
                    validator([s for i in pending for phase in ('ddl', 'reset', 'fill_statements')
                               for item in plans[i] for s in item[phase]])
                except Exception as error:
                    raise RegistryError('validate', f'{type(error).__name__}: {error}') from error
            if write:
                self._write_registry(entries)
            self.entries = entries
            # A changed definition cannot keep using its old receipt even if a
            # previous build is still physically present.
            self._install(stage, ready)
            self.last_report = report
            if executor is None:
                return report
            state['indexes'] = {i: dict(previous.get(i, {}) if i in ready else {},
                                        fingerprint=fingerprints[i],
                                        status='ready' if i in ready else 'pending',
                                        dependencies=dependencies[i]) for i in order}
            self._save_state(state)
            try:
                for i in pending:
                    state['indexes'][i]['status'] = 'building'
                    self._save_state(state)
                    try:
                        # Per index: finish an upstream index before resetting
                        # or filling any of its consumers.
                        for phase in ('ddl', 'reset', 'fill_statements'):
                            for statement in dict.fromkeys(s for item in plans[i] for s in item[phase]):
                                executor(statement)
                    except Exception as error:
                        report['failed'][i] = f'{type(error).__name__}: {error}'
                        raise RegistryError('build', report['failed'][i], i, report) from error
                    state['indexes'][i].update(status='ready', built_at=_now())
                    self._save_state(state)
                    ready.append(i)
                    report['built'].append(i)
                    report['pending'].remove(i)
            finally:
                self._install(stage, [i for i in order if i in ready])
                report['active'] = [i for i in order if i in ready]
                report['plans'] = [p for i in report['pending'] for p in plans[i]]
                self.last_report = report
            return report
        finally:
            stage.close()

    # ------------------------------------------------------- dependencies
    def _definitions(self, stage, plans):
        reads, writes, shapes = {}, {}, {}
        child_groups = {}
        for edge in stage.schema.edges:
            for pair in edge.get('vertex_type_pair_relations') or []:
                child_groups[f'{edge["type_name"]}_{pair["source_vertex"]}_{pair["destination_vertex"]}'.upper()] = \
                    edge['type_name'].upper()

        def table_name(name):
            return child_groups.get(name.upper(), name.upper())

        for iid, fills in stage.fills.items():
            reads[iid], writes[iid], shapes[iid] = set(), set(), []
            for text in fills:
                fill = parse_fill(text)
                kind, carrier = stage.schema.table(fill.table)
                if carrier is None:
                    raise ValueError(f'{iid}: declare the relationship group as the FILL carrier')
                cols = fill.columns or [p['property_name'] for p in carrier.get('properties') or []]
                writes[iid].update((fill.table.upper(), c) for c in cols)
                if not fill.columns:
                    writes[iid].add((fill.table.upper(), '@rows'))
                shapes[iid].append({
                    'kind': kind, 'table': fill.table.upper(), 'columns': cols,
                    'keys': carrier.get('primary_keys'),
                    'pairs': carrier.get('vertex_type_pair_relations'),
                    'types': {c: stage.schema.properties(fill.table)[c] for c in cols},
                    'defaults': {c: _tokens(stage.schema.defaults.get((fill.table.upper(), c), 'NULL')) for c in cols},
                })
                ir = stage.engine.bind(fill.source)['ir']
                nodes = list(_walk(ir))
                entities = {n['uid']: n for n in nodes if n.get('kind') in ('NODE', 'REL') and 'uid' in n}
                for n in nodes:
                    if n.get('kind') == 'REL':
                        reads[iid].update((table_name(t), '@rows') for t in n.get('tables') or [])
                    if n.get('k') == 'PROPERTY' and n.get('property') != '_ID':
                        owner = entities.get(n.get('owner'))
                        if owner is None:
                            raise ValueError(f'{iid}: cannot determine property dependency owner')
                        reads[iid].update((table_name(t), n['property']) for t in owner.get('tables') or [])
        producers = {}
        for iid, columns in writes.items():
            for column in columns:
                if column in producers:
                    raise ValueError(f'Overlapping FILL carriers: {producers[column]} and {iid}: {column}')
                producers[column] = iid
        dependencies = {iid: sorted({producers[c] for c in columns if c in producers} - {iid})
                        for iid, columns in reads.items()}
        order, visiting, done = [], set(), set()

        def visit(iid):
            if iid in visiting:
                raise ValueError(f'Cyclic FILL dependency involving {iid}')
            if iid in done:
                return
            visiting.add(iid)
            for dep in dependencies[iid]:
                visit(dep)
            visiting.remove(iid)
            done.add(iid)
            order.append(iid)

        for iid in plans:
            visit(iid)
        context = _digest({'base_schema': self.base_schema.doc,
                           'base_defaults': sorted((t, c, v) for (t, c), v in self.base_schema.defaults.items()),
                           'profile': self.mw.profile.name,
                           # The mapping's effective content, not where its
                           # constraints were read from (shared file or inline).
                           'mapping': {k: v for k, v in getattr(self.mw.frontend, 'm', {}).items()
                                       if k not in ('constraints_file', 'constraints_source')},
                           'plan_format': self.PLAN_FORMAT})
        fingerprints = {}
        for iid in order:
            normalized = [{phase: [_tokens(s) for s in item[phase]]
                           for phase in ('ddl', 'reset', 'fill_statements')} for item in plans[iid]]
            fingerprints[iid] = _digest({'context': context, 'carrier': shapes[iid], 'plan': normalized,
                                        'dependencies': {d: fingerprints[d] for d in dependencies[iid]}})
        return order, dependencies, fingerprints

    def _install(self, stage, ready):
        # Compile the selected catalog before replacing the live engine.
        engine = Engine(*self.mw._engine_args)
        try:
            engine.load(stage.schema.to_yaml(), self.mw.stats)
            for iid in ready:
                engine.register(iid, stage.fills[iid])
            if engine.indexes().get('failed_candidates'):
                raise ValueError('Ready index catalog contains failed candidates')
        except BaseException:
            engine.close()
            raise
        old = self.mw.engine
        self.mw.engine = engine
        self.mw.schema = stage.schema.copy()
        self.mw.profile.dialect.set_schema(self.mw.schema)
        if hasattr(self.mw.frontend, 'set_schema'):
            self.mw.frontend.set_schema(self.mw.schema)
        self.mw.fills = OrderedDict((iid, list(stage.fills[iid])) for iid in ready)
        self.mw.failed = []
        self.mw._cache.clear()
        old.close()


def _declares(ddl, fill):
    """Does NeuG DDL `ddl` declare (part of) the carrier `fill` writes?"""
    s = strip_comments(ddl)
    table = re.escape(fill.table)
    if re.match(rf'\s*CREATE\s+REL\s+TABLE(?:\s+IF\s+NOT\s+EXISTS)?\s+`?{table}`?\s*\(', s, re.I):
        return not fill.columns
    m = re.match(rf'\s*ALTER\s+TABLE\s+`?{table}`?\s+ADD\s+(?:IF\s+NOT\s+EXISTS\s+)?`?(\w+)`?', s, re.I)
    return bool(m) and m[1] in fill.columns


def _check_count_presence(stage, ir):
    # A nullable COUNT can be zero for a nonempty group. The current kernel
    # sometimes uses the visible count as its presence guard, losing that
    # group. Do not enable such a definition until that kernel route has a
    # separate presence proof (not a text heuristic).
    nodes = list(_walk(ir))
    entities = {n['uid']: n for n in nodes if n.get('kind') in ('NODE', 'REL') and 'uid' in n}
    for node in nodes:
        if node.get('k') != 'AGGREGATE_FUNCTION' or node.get('function') != 'COUNT':
            continue
        children = node.get('children') or []
        if len(children) != 1:
            continue
        arg = children[0]
        if 'entity' in arg or arg.get('property') == '_ID':
            continue
        owner = entities.get(arg.get('owner'), {})
        tables = owner.get('tables') or []
        if arg.get('k') == 'PROPERTY' and tables and all(
                arg.get('property') in (stage.schema.table(t)[1] or {}).get('primary_keys', []) for t in tables):
            continue
        if arg.get('k') == 'LITERAL' and not (arg.get('value') or {}).get('null', True):
            continue
        raise RegistryError('bind', 'COUNT(nullable/computed value): kernel group-presence proof is missing; '
                            'the registry refuses to enable this definition')


class _Dumper(yaml.SafeDumper):
    """Multi-line texts (submitted statements) as literal blocks."""


_Dumper.add_representer(str, lambda dumper, text: dumper.represent_scalar(
    'tag:yaml.org,2002:str', '\n'.join(line.rstrip() for line in text.split('\n')), style='|')
    if '\n' in text else dumper.represent_scalar('tag:yaml.org,2002:str', text))


def _atomic_write(path, text):
    fd, name = tempfile.mkstemp(prefix=path.name + '.', dir=path.parent)
    try:
        with os.fdopen(fd, 'w') as handle:
            handle.write(text)
            handle.flush()
            os.fsync(handle.fileno())
        if path.exists():
            os.chmod(name, path.stat().st_mode & 0o777)
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)
