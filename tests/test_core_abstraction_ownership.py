import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def source(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


class CoreAbstractionOwnershipTests(unittest.TestCase):
    def test_numeric_typeclass_methods_have_no_concrete_module_copies(self):
        forbidden = ("inc", "dec", "double", "square", "cube", "abs", "signum")
        for module in ("core/prelude/Data/Int.mon", "core/prelude/Data/Float.mon"):
            text = source(module)
            for name in forbidden:
                self.assertNotRegex(
                    text,
                    rf"(?m)^method\s+{re.escape(name)}\s+::",
                    f"{module} duplicates canonical Numeric.{name}",
                )

    def test_integral_methods_are_not_reimplemented_by_math_or_data_int(self):
        self.assertNotRegex(source("core/Math.mon"), r"(?m)^define\s+(even\?|odd\?|gcd|lcm)\s+::")
        self.assertNotRegex(source("core/prelude/Data/Int.mon"), r"(?m)^method\s+(even\?|odd\?)\s+::")

    def test_ordering_algorithms_have_one_owner(self):
        for module in ("core/Math.mon", "core/prelude/Data/Int.mon", "core/prelude/Data/Float.mon"):
            text = source(module)
            self.assertNotRegex(text, r"(?m)^(?:define|method)\s+(?:min|max|clamp)\s+::")

        ord_source = source("core/prelude/Data/Ord.mon")
        self.assertRegex(ord_source, r"(?m)^class\s+Ord\s+a\s+where")
        self.assertRegex(ord_source, r"(?m)^\s*clamp\s+::")


if __name__ == "__main__":
    unittest.main()
