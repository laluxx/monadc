"""Canonical solved-grade equations shared by QTT proof paths."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class QttRuleTests(unittest.TestCase):
    def test_canonical_resource_equations(self):
        source = r'''
#include "qtt/rules.h"
#include <assert.h>

int main(void) {
    QttQuantity zero = qtt_quantity_finite(0);
    QttQuantity one = qtt_quantity_finite(1);
    QttQuantity two = qtt_quantity_finite(2);
    QttQuantity three = qtt_quantity_finite(3);
    assert(qtt_quantity_equal(qtt_rule_sequence(two, three),
                              qtt_quantity_finite(5)));
    assert(qtt_quantity_equal(qtt_rule_choice(two, three), three));
    assert(qtt_quantity_equal(
        qtt_rule_let(two, three, one, false),
        qtt_quantity_finite(5)));
    assert(qtt_quantity_equal(
        qtt_rule_let(two, three, one, true), two));
    assert(qtt_quantity_equal(qtt_rule_capture(zero, false), zero));
    assert(qtt_quantity_equal(qtt_rule_capture(three, false), one));
    assert(qtt_quantity_equal(qtt_rule_capture(three, true), zero));
    QttQuantity arguments[] = {one, three};
    QttQuantity domains[] = {two, one};
    assert(qtt_quantity_equal(
        qtt_rule_direct_application(one, two, arguments, domains, 2),
        qtt_quantity_finite(8)));
    assert(qtt_quantity_equal(
        qtt_rule_direct_application(
            zero, zero, arguments, domains, 2),
        qtt_quantity_finite(5)));
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "qtt_rules_test.c"
            executable = directory / "qtt_rules_test"
            harness.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-iquote", str(ROOT), str(harness),
                    str(ROOT / "qtt" / "quantity.c"),
                    str(ROOT / "qtt" / "rules.c"),
                    "-o", str(executable),
                ],
                cwd=ROOT, check=True,
            )
            subprocess.run([str(executable)], cwd=ROOT, check=True)


if __name__ == "__main__":
    unittest.main()
