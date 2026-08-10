"""Core owns convenient console IO without hiding effects or failures."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CoreIOConsoleTests(unittest.TestCase):
    def test_unit_value_can_be_passed_to_a_unit_domain(self):
        program = r'''
module UnitValueProbe []
define accept-unit :: () -> Int
  _ -> 7
show (accept-unit ())
'''
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source = work / "UnitValueProbe.mon"
            executable = work / "UnitValueProbe"
            source.write_text(program)
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            environment["HOME"] = str(work / "home")
            (work / "home").mkdir()
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(source), "-o", str(executable)],
                cwd=work, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ran = subprocess.run(
                [str(executable)], cwd=work, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            self.assertEqual(ran.stdout, "7\n")

    def test_console_api_uses_compact_io_arrows_and_structured_results(self):
        source = (ROOT / "core" / "IO" / "Console.mon").read_text()
        for declaration in (
            "define read-stdin-byte :: () -io-> Either IOError Int",
            "define write-stdout :: String -io-> Either IOError Int",
            "define write-stderr :: String -io-> Either IOError Int",
            "define write-line :: String -io-> Either IOError Int",
            "define write-error-line :: String -io-> Either IOError Int",
        ):
            self.assertIn(declaration, source)
        self.assertNotIn("where e has io", source)
        self.assertIn("_ -> read-byte-result stdin", source)

    def test_console_stdout_stderr_lines_and_stdin_execute(self):
        program = r'''
import IO
import IO.Console
import Data.Either
module ConsoleProbe []
show (fromRight (write-line "out") (0 - 1))
show (fromRight (write-error-line "err") (0 - 1))
show (fromRight (read-stdin-byte ()) (0 - 1))
'''
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source = work / "ConsoleProbe.mon"
            executable = work / "ConsoleProbe"
            source.write_text(program)
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            cache_home = work / "home"
            cache_home.mkdir()
            environment["HOME"] = str(cache_home)
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(source), "-o", str(executable)],
                cwd=work, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ran = subprocess.run(
                [str(executable)], cwd=work, input="Z", text=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertEqual(ran.stdout, "out\n4\n4\n90\n")
            self.assertEqual(ran.stderr, "err\n")


if __name__ == "__main__":
    unittest.main()
