from .base import Dialect


class NeugDialect(Dialect):
    """NeuG Cypher: `(n:A:B)` is a label UNION, as in the binder."""
    name = 'neug'


class KuzuDialect(Dialect):
    """Kuzu shares NeuG's grammar family (NeuG's compiler derives from Kuzu):
    label unions `(n:A:B)`, `label(x)`, `CAST(x AS T)`.  Differences are
    confined to spellings overridden here."""
    name = 'kuzu'

    UNBOUNDED = 4294967295

    def special_expression(self, e, printer):
        # RewriterV4 replaces COUNT by SUM over stored counts and keeps the
        # COUNT's unique name. Kuzu's SUM over INT64 is INT128 (COUNT is
        # INT64), so cast it back -- only in the statement's last projection:
        # Kuzu rejects aggregating a WITH alias whose expression wraps an
        # aggregate ("nested aggregation"), and a later QueryPart (possibly
        # printed separately) may aggregate an intermediate one.
        if e.get('k') == 'AGGREGATE_FUNCTION' and e.get('function') == 'SUM' and \
                e.get('type') == 'INT64' and getattr(printer, 'in_last_part', False) and \
                str(e.get('uid', '')).lstrip('_0123456789').upper().startswith('COUNT'):
            inner = self.aggregate('SUM', [printer.expr(c) for c in e.get('children') or []],
                                   e.get('distinct', False))
            return self.cast(inner, 'INT64')
        return super().special_expression(e, printer)

    def var_length(self, lower, upper):
        # Kuzu rejects upper bounds above var_length_extend_max_depth (default
        # 30) and reads an open `*l..` as l..max.  The Kuzu frontend therefore
        # equates `*l..` with NeuG's unbounded walk; this is exact while no
        # path is longer than Kuzu's cap (e.g. LDBC reply trees).
        return f'*{lower}..' if upper == self.UNBOUNDED else f'*{lower}..{upper}'
