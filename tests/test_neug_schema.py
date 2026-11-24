"""NeuG schema files: an import config's `temporal: timestamp` is NeuG's
datetime (kTimestampMs), so a TIMESTAMP literal compares with it."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from cico import IndexMiddleware  # noqa: E402
from cico.schema import Schema  # noqa: E402
import workloads as W  # noqa: E402

IC_IMPORT_SCHEMA = W.DOCS / 'NeuG/scripts/import/configs/ldbc/graph.yaml'


class NeugSchema(unittest.TestCase):
    def test_timestamp_spelling_is_datetime(self):
        schema = Schema.from_text('''
schema:
  vertex_types:
    - type_name: A
      type_id: 0
      properties:
        - {property_id: 0, property_name: id, property_type: {primitive_type: DT_SIGNED_INT64}}
        - property_id: 1
          property_name: t
          property_type:
            temporal:
              timestamp:
      primary_keys: [id]
  edge_types: []
''')
        prop = schema.doc['schema']['vertex_types'][0]['properties'][1]
        self.assertEqual(prop['property_type'], {'temporal': {'datetime': ''}})
        self.assertEqual(schema.properties('A')['t'], 'TIMESTAMP')

    @unittest.skipUnless(IC_IMPORT_SCHEMA.is_file(), 'experiment docs not available')
    def test_import_config_binds_timestamp_comparison(self):
        with IndexMiddleware(IC_IMPORT_SCHEMA, dialect='neug') as mw:
            mw.engine.bind("MATCH (f:FORUM)-[m:HASMEMBER]->(p:PERSON) "
                           "WHERE m.joinDate > TIMESTAMP('2011-01-01 00:00:00') RETURN count(*) AS n", {})


if __name__ == '__main__':
    unittest.main()
