"""Physical targets of the index registry: identity, EXPLAIN pre-check and
auto-commit execution of materialization statements.

An executor is ``executor(statement)`` (returns only after the statement
committed) with optional ``validate(statements)`` (bind/plan every pending
statement before the first write), ``release()`` and a ``target`` identity
string naming one physical database generation.
"""
import contextlib
import json
import re
from pathlib import Path

from .statements import classify


class Neo4jIndexExecutor:
    """Auto-commit executor and identity of the physical Neo4j database."""

    def __init__(self, uri, database='neo4j', auth=None):
        from neo4j import GraphDatabase
        self.driver = GraphDatabase.driver(uri, auth=auth)
        self.database = database
        try:
            with self.driver.session(database=database) as session:
                info = session.run('CALL db.info() YIELD id RETURN id').single()
                if info is None or not info['id']:
                    raise ValueError('Neo4j did not return a physical database id')
                self.target = json.dumps({'uri': uri, 'database': database, 'database_id': info['id']}, sort_keys=True)
        except BaseException:
            self.close()
            raise

    def __call__(self, statement):
        with self.driver.session(database=self.database) as session:
            session.run(statement).consume()

    def validate(self, statements):
        """Compile every pending target statement before the first reset."""
        with self.driver.session(database=self.database) as session:
            for statement in statements:
                session.run('EXPLAIN ' + statement).consume()

    def close(self):
        self.driver.close()


_KUZU_ADD_COLUMN = re.compile(
    r'ALTER\s+TABLE\s+(`[^`]+`|\w+)\s+ADD\s+IF\s+NOT\s+EXISTS\s+(`[^`]+`|\w+)\s+(.+?)(?:\s+DEFAULT\s+.*)?$',
    re.I | re.S)
_KUZU_CREATE_REL = re.compile(r'CREATE\s+REL\s+TABLE\s+IF\s+NOT\s+EXISTS\s+(`[^`]+`|\w+)\s*\((.*)\)\s*$',
                              re.I | re.S)


def _kuzu_name(text):
    return text[1:-1].replace('``', '`') if text.startswith('`') else text


def _kuzu_type(text):
    return re.sub(r'\s+', '', text).upper()


class KuzuIndexExecutor:
    """Embedded Kuzu target (``kuzu`` Python package, 0.10 format).

    Kuzu has no server-side database id, and an open database holds a
    process-exclusive write lock. The identity is the resolved store path plus
    its filesystem device/inode: a copied or restored store is a different
    physical generation and needs its own state file. The database is opened
    only while a reload validates or builds and is released afterwards.
    """

    def __init__(self, path, threads=4, buffer_pool_gb=8):
        self.path = Path(path).expanduser().resolve()
        if not self.path.exists():
            raise ValueError(f'Kuzu database {self.path} does not exist')
        stat = self.path.stat()
        self.target = json.dumps({'system': 'kuzu', 'path': str(self.path),
                                  'device': stat.st_dev, 'inode': stat.st_ino}, sort_keys=True)
        self.threads = threads
        self.buffer_pool = int(buffer_pool_gb * (1 << 30)) if buffer_pool_gb else 0
        self._db = self._conn = None

    def connection(self):
        if self._conn is None:
            import kuzu
            # Bounded buffer pool: Kuzu otherwise reserves most of the host's RAM.
            self._db = kuzu.Database(str(self.path), buffer_pool_size=self.buffer_pool)
            self._conn = kuzu.Connection(self._db, num_threads=self.threads)
        return self._conn

    @staticmethod
    def _drain(result):
        results = result if isinstance(result, list) else [result]
        rows = []
        for item in results:
            while item.has_next():
                rows.append(item.get_next())
            item.close()
        return rows

    def query(self, statement, parameters=None):
        return self._drain(self.connection().execute(statement, parameters=parameters or {}))

    def __call__(self, statement):
        # Auto-commit: returns only after Kuzu committed the statement.
        self.query(statement)

    def _check_existing(self, statement):
        """Kuzu's IF NOT EXISTS silently keeps an older, different carrier.

        A changed definition must not build into a column or relationship
        table of another type/shape; require an explicit physical drop.
        """
        tables = {row[1].upper(): row[2] for row in self.query('CALL show_tables() RETURN *')}
        match = _KUZU_ADD_COLUMN.fullmatch(statement.strip())
        if match:
            table, column, wanted = _kuzu_name(match[1]), _kuzu_name(match[2]), _kuzu_type(match[3])
            if table.upper() not in tables:
                raise ValueError(f'Kuzu table {table} does not exist')
            for row in self.query(f"CALL table_info('{table}') RETURN *"):
                if row[1] == column and _kuzu_type(row[2]) != wanted:
                    raise ValueError(f'Kuzu {table}.{column} exists as {row[2]}, definition needs {match[3]}; '
                                     'drop the stale carrier column before rebuilding')
            return
        match = _KUZU_CREATE_REL.fullmatch(statement.strip())
        if match:
            name = _kuzu_name(match[1])
            if name.upper() not in tables:
                return
            if tables[name.upper()] != 'REL':
                raise ValueError(f'Kuzu table {name} exists and is not a relationship table')
            wanted_pairs, wanted_props = set(), {}
            for part in re.split(r',\s*(?![^()]*\))', match[2]):
                pair = re.fullmatch(r'FROM\s+(`[^`]+`|\w+)\s+TO\s+(`[^`]+`|\w+)', part.strip(), re.I)
                if pair:
                    wanted_pairs.add((_kuzu_name(pair[1]).upper(), _kuzu_name(pair[2]).upper()))
                elif part.strip():
                    prop = re.fullmatch(r'(`[^`]+`|\w+)\s+(.+?)(?:\s+DEFAULT\s+.*)?', part.strip(), re.I | re.S)
                    wanted_props[_kuzu_name(prop[1])] = _kuzu_type(prop[2])
            pairs = {(r[0].upper(), r[1].upper()) for r in self.query(f"CALL show_connection('{name}') RETURN *")}
            props = {r[1]: _kuzu_type(r[2]) for r in self.query(f"CALL table_info('{name}') RETURN *")}
            # Extra columns are tolerated: hidden NeuG state is added by the
            # ALTERs that follow, and stale columns of older builds are unset.
            if pairs != wanted_pairs or any(props.get(k) != v for k, v in wanted_props.items()):
                raise ValueError(f'Kuzu relationship table {name} exists with endpoints {sorted(pairs)} and '
                                 f'properties {props}; definition needs {sorted(wanted_pairs)} {wanted_props}. '
                                 'Drop the stale carrier table before rebuilding')

    def validate(self, statements):
        """Bind every pending statement before the first physical write.

        Kuzu has a schema, so writes into new carriers only bind after their
        DDL. Run the DDL inside one transaction, EXPLAIN everything else in
        order, and roll back: nothing reaches the store.
        """
        with self.rolled_back():
            for statement in statements:
                if classify(statement) == 'ddl':
                    self._check_existing(statement)
                    self.query(statement)
                else:
                    self.query('EXPLAIN ' + statement)

    @contextlib.contextmanager
    def rolled_back(self):
        """One manual transaction that is always rolled back.

        Kuzu aborts the transaction itself when a statement fails; the
        original error is the one to report then, not the ROLLBACK's.
        """
        conn = self.connection()
        conn.execute('BEGIN TRANSACTION')
        try:
            yield self
        except BaseException:
            try:
                conn.execute('ROLLBACK')
            except RuntimeError:
                pass
            raise
        conn.execute('ROLLBACK')

    def release(self):
        if self._conn is not None:
            self._conn.close()
            self._db.close()
        self._db = self._conn = None

    close = release
