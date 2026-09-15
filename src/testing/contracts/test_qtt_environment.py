"""Existential closure environments preserve instance-qualified origins."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class QttEnvironmentTests(unittest.TestCase):
    def test_environment_validation_and_substitution(self):
        source = r'''
#include "qtt/environment.h"
#include <assert.h>

int main(void) {
    QttEnvironmentOrigin origins[] = {
        qtt_environment_parameter(0),
        qtt_environment_local(77),
        qtt_environment_expression(900),
    };
    QttClosureEnvironment *environment =
        qtt_environment_new(11, 2, 3, origins, 3, 1);
    assert(environment && qtt_environment_validate(environment));

    QttEnvironmentOrigin arguments[] = {
        qtt_environment_expression(501),
    };
    QttClosureEnvironment *composed =
        qtt_environment_substitute(environment, arguments, 1);
    assert(composed && qtt_environment_validate(composed));
    assert(composed->origins[0].kind == QTT_ENVIRONMENT_EXPRESSION);
    assert(composed->origins[0].identity == 501);
    assert(composed->origins[1].kind == QTT_ENVIRONMENT_LOCAL);
    assert(composed->origins[1].identity == 77);
    assert(composed->origins[2].identity == 900);

    QttEnvironmentOrigin missing[] = {qtt_environment_parameter(4)};
    assert(!qtt_environment_new(12, 2, 3, missing, 1, 1));
    assert(!qtt_environment_substitute(environment, NULL, 0));

    qtt_environment_free(composed);
    qtt_environment_free(environment);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "environment.c"
            executable = directory / "environment"
            harness.write_text(source)
            subprocess.run([
                "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                "-iquote", str(ROOT / "src"), str(harness),
                str(ROOT / "src" / "qtt" / "environment.c"), "-o", str(executable),
            ], cwd=ROOT, check=True)
            subprocess.run([str(executable)], cwd=ROOT, check=True)


if __name__ == "__main__":
    unittest.main()
