import os
import pty
import re
import select
import signal
import tempfile
import time
import unittest

from src.testing.monad_binary import resolve_monad_binary


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

    def test_first_prompt_begins_at_terminal_origin(self):
        output = self.repl.read_until("Monad λ ".encode())
        self.assertTrue(
            output.startswith(b"Monad "),
            f"unexpected output before first prompt: {output!r}",
        )

    def test_pasted_glyph_definition_is_one_multiline_form(self):
        self.repl.read_until("Monad λ ".encode())
        self.repl.output = b""
        self.repl.send('''define numsign :: Num a => a -> String
  n ├─ < 0 -> "negative"
    ╰─╮
      ├─ = 0 -> "zero"
      ╰───▶ "positive"
'''.encode())
        definition = self.repl.read_until("Monad λ ".encode())
        self.assertNotIn(b"expected one complete form", definition)
        self.repl.output = b""
        self.repl.send(b"numsign 0\n")
        result = self.repl.read_until(b'\n"zero"\n')
        self.assertNotIn(b"error:", result.lower())

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

    def test_typed_definition_continuation_aligns_beneath_prompt_input(self):
        self.repl.read_until("Monad λ ".encode())
        self.repl.output = b""
        # Continuation clauses are deliberately sent at column zero.  The
        # interactive editor must insert their two-space Wisp indentation.
        self.repl.send(b"define last :: [a] -> a?\n")
        self.repl.send(b"[] -> nil\n")
        self.repl.send(b"[x] -> x\n")
        self.repl.send(b"[_|xs] -> last xs\n")
        # C-p/C-n stay inside the multiline buffer instead of selecting
        # history until the cursor crosses its first/last logical line.
        self.repl.send(b"\x10\x0e")
        self.repl.send(b"\n")
        definition_output = self.repl.read_until("Monad λ ".encode())
        self.assertIn(b"\n          []", definition_output)
        self.assertIn(b"\n          [x]", definition_output)
        self.assertIn(b"\n          [_|xs]", definition_output)
        self.repl.output = b""
        self.repl.send(b"last [1 2 3]\n")
        output = self.repl.read_until(b"\n3\n")
        self.assertNotIn(b"error:", output.lower())

        # M-p bypasses line navigation and recalls the complete prior form.
        self.repl.output = b""
        self.repl.send(b"\x1bp\n")
        recalled = self.repl.read_until(b"\n3\n")
        self.assertNotIn(b"error:", recalled.lower())

    def test_where_continuation_indents_two_columns_deeper(self):
        self.repl.read_until("Monad λ ".encode())
        self.repl.output = b""
        self.repl.send(b"define doubled :: Int -> Int\n")
        self.repl.send(b"n -> helper n\n")
        self.repl.send(b"where\n")
        self.repl.send(b"helper x -> x * 2\n")
        self.repl.send(b"\n")
        output = self.repl.read_until("Monad λ ".encode())
        self.assertIn(b"\n          where", output)
        self.assertIn(b"\n            helper", output)

    def test_backspace_deletes_an_empty_electric_pair(self):
        self.repl.read_until("Monad λ ".encode())
        self.repl.output = b""
        self.repl.send(b"(")
        self.repl.send(b"\x7f")
        self.repl.send(b"id 7\n")
        output = self.repl.read_until(b"\n7\n")
        self.assertNotIn(b"error:", output.lower())

    def test_ctrl_meta_p_moves_to_the_previous_balanced_expression(self):
        self.repl.read_until("Monad λ ".encode())
        self.repl.output = b""
        self.repl.send(b"(id 1) (id 2)")
        self.repl.send(b"\x1b\x10")  # C-M-p / backward-sexp
        self.repl.send(b"\x0b\n")    # kill through end, submit first form
        output = self.repl.read_until(b"\n1\n")
        self.assertNotIn(b"error:", output.lower())

    def test_ctrl_meta_n_moves_past_the_next_balanced_expression(self):
        self.repl.read_until("Monad λ ".encode())
        self.repl.output = b""
        self.repl.send(b"(id 1) (id 2)")
        self.repl.send(b"\x01\x1b\x0e")  # C-a, C-M-n / forward-sexp
        self.repl.send(b"\x0b\n")         # kill second form, submit first
        output = self.repl.read_until(b"\n1\n")
        self.assertNotIn(b"error:", output.lower())

    def test_ctrl_meta_pair_motion_does_not_walk_atoms(self):
        self.repl.read_until("Monad λ ".encode())
        self.repl.output = b""
        self.repl.send(b"x (id 1)")
        self.repl.send(b"\x01\x1b\x0eX\x0c")
        time.sleep(0.1)
        self.repl.send(b"\x03")
        forward = self.repl.read_until("Monad λ ".encode())
        self.assertIn(b"Xx (id 1)", forward)

        self.repl.output = b""
        self.repl.send(b"(id 1) x")
        self.repl.send(b"\x1b\x10X\x0c")
        time.sleep(0.1)
        self.repl.send(b"\x03")
        backward = self.repl.read_until("Monad λ ".encode())
        self.assertIn(b"(id 1) xX", backward)

    def test_ctrl_meta_k_kills_a_sexp_for_ctrl_y(self):
        self.repl.read_until("Monad λ ".encode())
        self.repl.output = b""
        self.repl.send(b"(id 1) (id 2)")
        self.repl.send(b"\x01\x1b\x0b\x1be\n")  # kill, M-e, submit remainder
        first = self.repl.read_until(b"\n2\n")
        self.assertNotIn(b"error:", first.lower())
        self.repl.output = b""
        self.repl.send(b"\x19\n")           # C-y restores killed (id 1)
        yanked = self.repl.read_until(b"\n1\n")
        self.assertNotIn(b"error:", yanked.lower())

    def test_ctrl_p_removes_display_offset_when_crossing_to_first_line(self):
        self.repl.read_until("Monad λ ".encode())
        self.repl.output = b""
        self.repl.send(b"define abcdefghij :: Int -> Int\n")
        self.repl.send(b"\x10X\x0c")  # edit and redraw the complete buffer
        time.sleep(0.1)
        self.repl.send(b"\x03")
        output = self.repl.read_until("Monad λ ".encode())
        self.assertIn(b"deXfine", output)

    def test_ctrl_n_adds_display_offset_when_crossing_from_first_line(self):
        self.repl.read_until("Monad λ ".encode())
        self.repl.output = b""
        self.repl.send(b"define abcdefghij :: Int -> Int\n")
        self.repl.send(b"id 7")
        self.repl.send(b"\x1ba\x06\x06\x0eX\x0c")  # M-a, first col 2, C-n
        time.sleep(0.1)
        self.repl.send(b"\x03")
        output = self.repl.read_until("Monad λ ".encode())
        self.assertIn(b"Xid 7", output)

    def test_backspace_edits_semantic_indent_then_crosses_display_padding(self):
        self.repl.read_until("Monad λ ".encode())
        self.repl.output = b""
        self.repl.send(b"define last :: [a] -> a?\n[] -> nil")
        self.repl.send(b"\x01\x7f\x0c")
        time.sleep(0.1)
        self.repl.send(b"\x7f\x0c")
        time.sleep(0.1)
        self.repl.send(b"\x7f\x0c")
        time.sleep(0.1)
        self.repl.send(b"\x03")
        output = self.repl.read_until("Monad λ ".encode())
        self.assertIn(b"\n         [] -> nil", output)
        self.assertIn(b"\n        [] -> nil", output)
        self.assertIn(b"a?[] -> nil", output)

    def test_ctrl_b_crosses_display_padding_without_entering_it(self):
        self.repl.read_until("Monad λ ".encode())
        self.repl.output = b""
        self.repl.send(b"define last :: [a] -> a?\n[] -> nil")
        self.repl.send(b"\x01\x02\x02\x02X\x0c")
        time.sleep(0.1)
        self.repl.send(b"\x03")
        output = self.repl.read_until("Monad λ ".encode())
        self.assertIn(b"define last :: [a] -> a?X\n          [] -> nil", output)

    def test_return_splits_at_point_and_indents_the_moved_suffix(self):
        self.repl.read_until("Monad λ ".encode())
        self.repl.output = b""
        header = b"define last :: [a] -> a?"
        self.repl.send(header + b"[] -> nil")
        self.repl.send(b"\x01" + b"\x06" * len(header) + b"\n\x0c")
        time.sleep(0.1)
        self.repl.send(b"\x03")
        output = self.repl.read_until("Monad λ ".encode())
        self.assertIn(header + b"\n          [] -> nil", output)

    def test_ctrl_a_moves_to_first_nonspace_of_current_line(self):
        self.repl.read_until("Monad λ ".encode())
        self.repl.output = b""
        self.repl.send(b"define abcdefghij :: Int -> Int\nid 7")
        self.repl.send(b"\x01X\x0c")
        time.sleep(0.1)
        self.repl.send(b"\x03")
        output = self.repl.read_until("Monad λ ".encode())
        self.assertIn(b"\n          Xid 7", output)

    def test_ctrl_e_moves_to_end_of_current_line(self):
        self.repl.read_until("Monad λ ".encode())
        self.repl.output = b""
        self.repl.send(b"define abcdefghij :: Int -> Int\nid 7")
        self.repl.send(b"\x10\x05X\x0c")
        time.sleep(0.1)
        self.repl.send(b"\x03")
        output = self.repl.read_until("Monad λ ".encode())
        self.assertIn(b"IntX\n", output)

    def test_meta_a_and_meta_e_span_the_complete_input(self):
        self.repl.read_until("Monad λ ".encode())
        self.repl.output = b""
        self.repl.send(b"define abcdefghij :: Int -> Int\nid 7")
        self.repl.send(b"\x1baX\x1beY\x0c")
        time.sleep(0.1)
        self.repl.send(b"\x03")
        output = self.repl.read_until("Monad λ ".encode())
        self.assertIn(b"Xdefine abcdefghij :: Int -> Int", output)
        self.assertIn(b"id 7Y", output)

    def test_ctrl_o_inserts_a_literal_newline_without_submitting(self):
        self.repl.read_until("Monad λ ".encode())
        self.repl.output = b""
        self.repl.send(b"define abcdefghij :: Int -> Int\nid 7")
        self.repl.send(b"\x01\x06\x06\x0fX\x0c")
        time.sleep(0.1)
        self.repl.send(b"\x03")
        output = self.repl.read_until("Monad λ ".encode())
        self.assertIn(b"idX\n         7", output)

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

    def test_quoted_list_is_complete_after_balanced_expression(self):
        self.repl.read_until("Monad λ ".encode())
        self.repl.output = b""
        self.repl.send(b"'(1 2 3 4)\n")
        output = self.repl.read_until(b"\n(1 2 3 4)\n")
        self.assertNotIn(b"error:", output.lower())


if __name__ == "__main__":
    unittest.main()
