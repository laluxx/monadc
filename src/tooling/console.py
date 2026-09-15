from __future__ import annotations

import os
import re
import shutil
import sys
import textwrap
from dataclasses import dataclass, field

ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")


@dataclass(frozen=True)
class Theme:
    """Monad's restrained terminal palette.

    Color is hierarchy, not decoration: magenta identifies the product/sections,
    cyan identifies active commands and paths, and traffic-light colors are
    reserved for status.
    """

    brand: str = "1;35"
    accent: str = "1;36"
    success: str = "1;32"
    warning: str = "1;33"
    error: str = "1;31"
    strong: str = "1"
    muted: str = "2"
    rule: str = "2;35"
    accent_soft: str = "36"
    warning_soft: str = "33"
    error_soft: str = "31"


@dataclass
class Console:
    color: bool = True
    theme: Theme = field(default_factory=Theme)
    project: str = "monad"
    glyph_style: str = field(default_factory=lambda: glyph_style_from_env())

    def supports_glyph(self, glyph: str) -> bool:
        if self.glyph_style == "ascii":
            return False
        try:
            glyph.encode(sys.stdout.encoding or "utf-8")
            return True
        except Exception:
            return False

    def glyph(self, preferred: str, fallback: str) -> str:
        return preferred if self.supports_glyph(preferred) else fallback

    def paint(self, code: str, text: str) -> str:
        return f"\033[{code}m{text}\033[0m" if self.color else text

    def brand(self, text: str) -> str:
        return self.paint(self.theme.brand, text)

    def accent(self, text: str) -> str:
        return self.paint(self.theme.accent, text)

    def dim(self, text: str) -> str:
        return self.paint(self.theme.muted, text)

    def strong(self, text: str) -> str:
        return self.paint(self.theme.strong, text)

    @staticmethod
    def strip_ansi(text: str) -> str:
        return ANSI_RE.sub("", text)

    def visible_width(self, text: str) -> int:
        return len(self.strip_ansi(text))

    def width(self) -> int:
        try:
            value = shutil.get_terminal_size((96, 24)).columns
        except OSError:
            value = 96
        return max(40, min(value, 112))

    def rule(self, width: int | None = None) -> str:
        return self.glyph("─", "-") * max(1, width or self.width())

    def mark(self, kind: str) -> str:
        marks = {
            "bullet": ("◆", "*"),
            "contract": ("◇", "C"),
            "ok": ("✓", "OK"),
            "fail": ("✗", "X"),
            "warn": ("▲", "!"),
            "step": ("›", ">"),
            "note": ("•", "*"),
            "hint": ("↳", "->"),
        }
        preferred, fallback = marks[kind]
        return self.glyph(preferred, fallback)

    def header(self, title: str, subtitle: str | None = None) -> None:
        leader = f"{self.brand(self.mark('bullet'))} {self.brand(self.project)}"
        if title:
            leader += f"  {self.dim(self.glyph('·', '/'))}  {self.accent(title)}"
        print(leader)
        if subtitle:
            for line in textwrap.wrap(
                subtitle,
                width=max(24, self.width() - 2),
                break_long_words=False,
                break_on_hyphens=False,
            ) or [subtitle]:
                print(f"  {self.dim(line)}")
        print(self.paint(self.theme.rule, self.rule()))

    def section(self, title: str) -> None:
        prefix = f"{self.brand(self.mark('bullet'))} {self.brand(title)} "
        remaining = max(3, self.width() - self.visible_width(prefix))
        print(f"{prefix}{self.dim(self.rule(remaining))}")

    def emit(self, prefix: str, message: str, *, stream=None) -> None:
        stream = stream or sys.stdout
        available = max(18, self.width() - self.visible_width(prefix))
        lines = textwrap.wrap(
            message,
            width=available,
            break_long_words=False,
            break_on_hyphens=False,
            replace_whitespace=False,
        ) or [""]
        continuation = " " * self.visible_width(prefix)
        for index, line in enumerate(lines):
            print((prefix if index == 0 else continuation) + line, file=stream, flush=True)

    def ok(self, message: str) -> None:
        self.emit(f"{self.paint(self.theme.success, self.mark('ok'))} ", message)

    def fail(self, message: str) -> None:
        self.emit(f"{self.paint(self.theme.error, self.mark('fail'))} ", message, stream=sys.stderr)

    def warn(self, message: str) -> None:
        self.emit(f"{self.paint(self.theme.warning, self.mark('warn'))} ", message, stream=sys.stderr)

    def note(self, message: str) -> None:
        self.emit(f"{self.accent(self.mark('note'))} ", message)

    def step(self, message: str) -> None:
        self.emit(f"{self.accent(self.mark('step'))} ", message)

    def hint(self, message: str) -> None:
        self.emit(f"  {self.dim(self.mark('hint'))} ", self.dim(message))

    def pad_right(self, text: str, width: int) -> str:
        return text + " " * max(0, width - self.visible_width(text))

    def row(self, name: str, value: str, status: str | None = None) -> None:
        label_width = 20 if self.width() >= 70 else 14
        status_col = self.pad_right(status or "", 4)
        label = self.pad_right(self.dim(name), label_width)
        self.emit(f"  {status_col} {label} ", value)


def color_enabled(mode: str = "always") -> bool:
    if "NO_COLOR" in os.environ or os.environ.get("CLICOLOR", "").strip() == "0":
        return False
    if mode == "never":
        return False
    if mode == "always":
        return True
    return (sys.stdout.isatty() or sys.stderr.isatty()) and os.environ.get("TERM", "") != "dumb"


def glyph_style_from_env() -> str:
    """Return the presentation glyph policy, independent of TTY detection.

    Unicode is deliberately the default: redirected output is commonly read in
    Emacs compilation buffers, which support the same rich glyphs as a terminal.
    MAKE_ASCII remains a compatibility override for narrow or legacy logs.
    """
    raw = os.environ.get("MONAD_GLYPHS", "").strip().lower()
    if not raw and os.environ.get("MAKE_ASCII", "").strip().lower() in {"1", "true", "yes", "on"}:
        raw = "ascii"
    return "ascii" if raw in {"ascii", "plain", "fallback"} else "unicode"
