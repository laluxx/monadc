import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from monad_binary import resolve_monad_binary


MONAD = resolve_monad_binary()


class EmptySourceTests(unittest.TestCase):
    def compile_source(self, source: str):
        with tempfile.TemporaryDirectory(prefix="monadc-empty-source-") as td:
            root = Path(td)
            path = root / "Buffer.mon"
            path.write_text(source, encoding="utf-8")
            env = os.environ.copy()
            env["HOME"] = str(root / "home")
            return subprocess.run(
                [str(MONAD), str(path)],
                cwd=root,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
                timeout=15,
            )

    def test_empty_buffer_is_a_successful_no_op_for_editor_checkers(self):
        result = self.compile_source("")

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertNotIn("error:", result.stdout.lower())

    def test_comment_only_buffer_is_also_a_successful_no_op(self):
        result = self.compile_source(
            ";; A buffer containing only documentation is still valid.\n"
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertNotIn("error:", result.stdout.lower())


if __name__ == "__main__":
    unittest.main()
