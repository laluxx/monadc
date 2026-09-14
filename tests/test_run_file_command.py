"""`monad run FILE.mon` compiles and executes one standalone source file."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

from monad_binary import resolve_monad_binary, resolve_runtime_archive


ROOT = Path(__file__).resolve().parents[1]
MONAD = resolve_monad_binary()
RUNTIME = resolve_runtime_archive(MONAD)


class RunFileCommandTests(unittest.TestCase):
    def test_run_with_source_compiles_and_executes_that_file(self):
        with tempfile.TemporaryDirectory(prefix="monadc-run-file-") as td:
            work = Path(td)
            source = work / "One Shot.mon"
            source.write_text("show 73\n", encoding="utf-8")
            env = os.environ.copy()
            env["HOME"] = str(work / "home")
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)
            Path(env["HOME"]).mkdir()

            result = subprocess.run(
                [str(MONAD), "run", source.name], cwd=work, env=env,
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=30,
            )

            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("(73)\n", result.stdout)
            self.assertTrue((work / "One Shot").exists())


if __name__ == "__main__":
    unittest.main()
