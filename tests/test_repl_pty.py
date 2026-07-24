import os
import pty
import re
import select
import signal
import tempfile
import time
import unittest

from monad_binary import resolve_monad_binary


MONAD = resolve_monad_binary()
ANSI_RE = re.compile(rb"\x1b\[[0-9;?]*[ -/]*[@-~]")


def terminal_text(value: bytes) -> bytes:
    return (
        ANSI_RE.sub(b"", value)
        .replace(b"\x01", b"")
        .replace(b"\x02", b"")
        .replace(b"\r", b"")
    )


class PtyRepl:
    def __init__(self):
        self.home = tempfile.TemporaryDirectory(prefix="monadc-repl-pty-")
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            env = os.environ.copy()
            env["HOME"] = self.home.name
            env["TERM"] = "xterm-256color"
            env.pop("MONAD_NO_PROMPT", None)
            os.execve(str(MONAD), [str(MONAD), "repl"], env)
        self.output = b""

    def send(self, value: bytes):
        os.write(self.fd, value)

    def read_until(self, pattern: bytes, timeout: float = 20.0) -> bytes:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            clean = terminal_text(self.output)
            if pattern in clean:
                return clean
            readable, _, _ = select.select([self.fd], [], [], 0.1)
            if readable:
                try:
                    chunk = os.read(self.fd, 65536)
                except OSError:
                    break
                if not chunk:
                    break
                self.output += chunk
        self.close()
        raise AssertionError(
            f"timed out waiting for {pattern!r}; output={self.output!r}"
        )

    def wait(self, timeout: float = 10.0) -> int:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid == self.pid:
                self.pid = 0
                return os.waitstatus_to_exitcode(status)
            time.sleep(0.05)
        self.close()
        raise AssertionError("REPL did not exit after terminal EOF")

    def close(self):
        if self.pid:
            try:
                os.kill(self.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                os.waitpid(self.pid, 0)
            except ChildProcessError:
                pass
            self.pid = 0
        try:
            os.close(self.fd)
        except OSError:
            pass
        self.home.cleanup()


class ReplPtyTests(unittest.TestCase):
    def setUp(self):
        self.repl = PtyRepl()

    def tearDown(self):
        self.repl.close()

    def test_readline_prompt_and_core_function_match_the_user_experience(self):
        self.repl.read_until("Monad λ ".encode())
        self.repl.output = b""
        self.repl.send(b"id 3\n")
        output = self.repl.read_until(b"\n3\n")
        self.assertNotIn(b"error:", output.lower())

    def test_indented_multiline_wisp_runs_after_blank_line(self):
        self.repl.read_until("Monad λ ".encode())
        self.repl.output = b""
        self.repl.send(b"show\n")
        self.repl.send(b"  id 42\n")
        self.repl.send(b"\n")
        output = self.repl.read_until(b"\n42\n")
        self.assertNotIn(b"error:", output.lower())

    def test_ctrl_c_cancels_partial_form_and_next_expression_runs(self):
        self.repl.read_until("Monad λ ".encode())
        self.repl.output = b""
        self.repl.send(b"show\n")
        self.repl.send(b"\x03")
        self.repl.read_until("Monad λ ".encode())
        self.repl.output = b""
        self.repl.send(b"id 7\n")
        output = self.repl.read_until(b"\n7\n")
        self.assertNotIn(b"requires at least", output)

    def test_ctrl_d_exits_cleanly(self):
        self.repl.read_until("Monad λ ".encode())
        self.repl.send(b"\x04")
        self.assertEqual(self.repl.wait(), 0)
        self.repl.pid = 0


if __name__ == "__main__":
    unittest.main()
