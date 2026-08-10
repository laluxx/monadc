"""Core IO keeps pure text policy separate from certified world interaction."""

from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CoreIOTests(unittest.TestCase):
    def test_io_modules_have_explicit_effect_boundaries(self):
        io = (ROOT / "core" / "IO.mon").read_text()
        readline = (ROOT / "core" / "IO" / "Readline.mon").read_text()
        editing = (ROOT / "core" / "prelude" / "Text" / "LineEditor.mon").read_text()
        posix = (ROOT / "core" / "System" / "Posix" / "Term.mon").read_text()

        self.assertIn("define read-byte :: Int -io-> Int", io)
        self.assertIn(
            "define write-byte :: Int -> Int -io-> Int", io)
        self.assertIn(
            "define write-text :: Int -> String -io-> Int", io)
        self.assertIn("data IOError", io)
        self.assertIn("= IOEndOfInput", io)
        self.assertNotIn("IOReadUnavailable", io)
        self.assertIn("def status (read-buf fd &byte 1)", io)
        self.assertIn("status < 0 then Left (IOStatus status)", io)
        self.assertIn(
            "define read-byte-result :: Int -io-> Either IOError Int", io)
        self.assertIn(
            "define write-byte-result :: Int -> Int -io-> Either IOError Int",
            io)
        self.assertIn(
            "define write-text-result :: Int -> String -io-> Either IOError Int",
            io)
        self.assertNotIn("where e has io", io)
        self.assertIn("module IO.Readline", readline)
        self.assertIn(
            "define read-escape-after :: Int -> Int -io-> Int", readline)
        self.assertIn(
            "define read-line-loop :: Int -> Int -> [Int] -io-> [Int]",
            readline)
        self.assertIn("define read-line :: () -io-> [Int]", readline)
        self.assertIn(
            "_ -> read-line-with-saved (tty-cbreak-mode stdin)",
            readline,
        )
        self.assertNotIn(
            "unit -> read-line-with-saved (tty-cbreak-mode stdin)",
            readline,
        )
        self.assertNotIn("where e has io", readline)
        self.assertNotIn("readLineFd", readline)
        self.assertNotIn("read-char", editing)
        self.assertNotIn("tty-cbreak-mode", editing)
        self.assertNotIn("import System.Posix.Term", editing)
        self.assertIn(
            "define sys-read :: Int -> *U8 -> Int -foreign-> Int",
            posix)
        self.assertIn(
            "define sys-write :: Int -> *U8 -> Int -foreign-> Int",
            posix)
        self.assertIn(
            "define sys-ioctl :: Int -> Int -> *a -foreign-> Int",
            posix)
        self.assertGreaterEqual(posix.count("where e has foreign"), 4)

    def test_real_core_io_reads_and_writes_through_a_pipe(self):
        program = r'''
import IO
module CoreIOProbe []
show (write-byte stdout 65)
show (write-text stdout "BC")
show (read-byte stdin)
'''
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source = work / "CoreIOProbe.mon"
            executable = work / "CoreIOProbe"
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
            interfaces = list((cache_home / ".cache" / "monad" / "core").glob(
                "*.mqti"))
            io_interfaces = [path.read_text() for path in interfaces
                             if re.search(r"^MODULE IO$", path.read_text(),
                                          re.MULTILINE)]
            self.assertEqual(len(io_interfaces), 1, interfaces)
            self.assertGreaterEqual(len(re.findall(
                r"predicates=\d+:io", io_interfaces[0])), 6)
            for exported in (
                "read-byte-result", "write-byte-result", "write-text-result"):
                self.assertRegex(
                    io_interfaces[0],
                    rf"(?m)^CONTRACT {re.escape(exported)} ")
            ran = subprocess.run(
                [str(executable)], cwd=work, input="Z", text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            # Direct syscalls are observed before stdio-buffered `show` output.
            self.assertEqual(ran.stdout, "ABC1\n2\n90\n")

    def test_structured_read_distinguishes_byte_and_eof(self):
        program = r'''
import IO
import Data.Either
module CoreIOResultProbe []
show (fromRight (read-byte-result stdin) (0 - 999))
'''
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source = work / "CoreIOResultProbe.mon"
            executable = work / "CoreIOResultProbe"
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
            byte = subprocess.run(
                [str(executable)], cwd=work, input="Z", text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(byte.returncode, 0, byte.stdout)
            self.assertEqual(byte.stdout, "90\n")
            eof = subprocess.run(
                [str(executable)], cwd=work, input="", text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(eof.returncode, 0, eof.stdout)
            self.assertEqual(eof.stdout, "-999\n")

    def test_readline_fd_executes_with_io_effect_contract(self):
        program = r'''
import IO
import IO.Readline
module CoreReadlineProbe []
show (read-key-fd stdin)
'''
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source = work / "CoreReadlineProbe.mon"
            executable = work / "CoreReadlineProbe"
            source.write_text(program)
            cache_home = work / "home"
            cache_home.mkdir()
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            environment["HOME"] = str(cache_home)
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(source), "-o", str(executable)],
                cwd=work, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ran = subprocess.run(
                [str(executable)], cwd=work, input="Q", text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            self.assertEqual(ran.stdout, "81\n")



if __name__ == "__main__":
    unittest.main()
