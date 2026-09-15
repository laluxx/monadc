"""QTT compiler sources live behind one explicit subsystem boundary."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[3]


class QttLayoutTests(unittest.TestCase):
    def test_qtt_sources_are_not_kept_at_repository_root(self):
        expected = {
            "bindings.c",
            "bindings.h",
            "quantity.c",
            "quantity.h",
            "usage.c",
            "usage.h",
        }
        self.assertTrue((ROOT / "src" / "qtt").is_dir())
        self.assertTrue(expected.issubset(
            {path.name for path in (ROOT / "src" / "qtt").iterdir()}
        ))
        self.assertEqual([], list(ROOT.glob("qtt_*.c")))
        self.assertEqual([], list(ROOT.glob("qtt_*.h")))


if __name__ == "__main__":
    unittest.main()
