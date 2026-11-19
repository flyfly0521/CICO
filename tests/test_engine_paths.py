"""Binary selection: $CICO_ENGINE / $CICO_EXEC override, else build/bin."""
import os
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from cico import engine
from cico.engine import resolve_binary


class EnginePathTests(unittest.TestCase):
    def test_default_is_the_cmake_build(self):
        with patch.dict(os.environ, {}, clear=True):
            root = Path(engine.__file__).resolve().parents[1]
            for name in ('cico_engine', 'neug_exec'):
                self.assertEqual(resolve_binary(name, 'CICO_TEST_BINARY'), root / 'build/bin' / name)

    def test_override_is_resolved_even_when_missing(self):
        with patch.dict(os.environ, {'CICO_TEST_BINARY': './missing-engine'}):
            self.assertEqual(resolve_binary('ignored', 'CICO_TEST_BINARY'), Path('missing-engine').resolve())


if __name__ == '__main__':
    unittest.main()
