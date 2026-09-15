#!/usr/bin/env -S python3 -B
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
import platform
import re
import shlex
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import textwrap
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

sys.dont_write_bytecode = True
os.environ.setdefault("PYTHONDONTWRITEBYTECODE", "1")
os.environ.setdefault("PYTHONPYCACHEPREFIX", str(Path(tempfile.gettempdir()) / "make-pycache"))
sys.pycache_prefix = os.environ["PYTHONPYCACHEPREFIX"]

ROOT = Path(__file__).resolve().parent
BUILD_ROOT = ROOT / "build"
DIST_DIR = BUILD_ROOT / "dist"
VENDOR_DIR = BUILD_ROOT / "vendor"
STATIC_DIR = BUILD_ROOT / "static"
COMPDB_PATH = BUILD_ROOT / "compile_commands.json"
COMPDB_LINK = ROOT / "compile_commands.json"
LOADED_CONFIGS: list[Path] = []

### Console

ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")


@dataclass(frozen=True)
class Theme:
    # Deliberately small palette: identity, accent, and status.
    # Ordinary prose stays terminal-default so color provides hierarchy, not noise.
    brand: str = "1;35"
    accent: str = "1;36"
    info: str = "1;36"
    success: str = "1;32"
    warning: str = "1;33"
    error: str = "1;31"
    strong: str = "1"
    muted: str = "2"
    rule: str = "2;35"
    subtle: str = "2"
    accent_soft: str = "36"
    info_soft: str = "36"
    warning_soft: str = "33"
    error_soft: str = "31"


@dataclass
class Renderer:
    color: bool = True
    verbose: bool = False
    theme: Theme = field(default_factory=Theme)

    @staticmethod
    def supports_glyph(glyph: str) -> bool:
        if os.environ.get("MAKE_ASCII", "").strip().lower() in {"1", "true", "yes", "on"}:
            return False
        if os.environ.get("TERM") == "dumb":
            return False
        enc = sys.stdout.encoding or "utf-8"
        try:
            glyph.encode(enc)
            return True
        except Exception:
            return False

    def glyph(self, preferred: str, fallback: str) -> str:
        return preferred if self.supports_glyph(preferred) else fallback

    def paint(self, code: str, text: str) -> str:
        return f"\033[{code}m{text}\033[0m" if self.color else text

    @staticmethod
    def strip_ansi(text: str) -> str:
        return ANSI_RE.sub("", text)

    def visible_width(self, text: str) -> int:
        return len(self.strip_ansi(text))

    def pad_right(self, text: str, width: int) -> str:
        return text + " " * max(0, width - self.visible_width(text))

    @staticmethod
    def width() -> int:
        try:
            width = shutil.get_terminal_size((96, 24)).columns
        except OSError:
            width = 96
        return max(40, min(width, 112))

    def rule(self, char: str | None = None, width: int | None = None) -> str:
        char = char or self.glyph("─", "-")
        return char * max(1, width or self.width())

    def dim(self, text: str) -> str:
        return self.paint(self.theme.muted, text)

    def brand(self, text: str) -> str:
        return self.paint(self.theme.brand, text)

    def accent(self, text: str) -> str:
        return self.paint(self.theme.accent, text)

    def strong(self, text: str) -> str:
        return self.paint(self.theme.strong, text)

    def mark(self, kind: str) -> str:
        marks = {
            "ok": ("✓", "OK"),
            "fail": ("✗", "X"),
            "warn": ("▲", "!"),
            "step": ("›", ">"),
            "bullet": ("◆", "*"),
            "note": ("•", "*"),
            "hint": ("↳", "->"),
        }
        preferred, fallback = marks[kind]
        return self.glyph(preferred, fallback)

    def tag(self, name: str, code: str | None = None) -> str:
        return self.paint(code or self.theme.brand, name.upper())

    def emit_wrapped(self, prefix: str, message: str, *, stream=None, code: str | None = None) -> None:
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
            rendered = self.paint(code, line) if code else line
            print((prefix if index == 0 else continuation) + rendered, file=stream, flush=True)

    def log(self, name: str, msg: str, role: str = "brand") -> None:
        code = getattr(self.theme, role)
        mark = self.paint(code, self.mark("bullet"))
        label = self.pad_right(self.tag(name, code), 10)
        self.emit_wrapped(f"{mark} {label} ", msg)

    def step(self, msg: str) -> None:
        self.emit_wrapped(f"{self.paint(self.theme.accent, self.mark('step'))} ", msg)

    def ok(self, msg: str) -> None:
        self.emit_wrapped(f"{self.paint(self.theme.success, self.mark('ok'))} ", msg)

    def note(self, msg: str) -> None:
        self.emit_wrapped(f"{self.paint(self.theme.info, self.mark('note'))} ", msg)

    def hint(self, msg: str, *, stream=None) -> None:
        self.emit_wrapped(f"  {self.dim(self.mark('hint'))} ", msg, stream=stream, code=self.theme.muted)

    def error_detail(self, msg: str) -> None:
        self.emit_wrapped(
            f"  {self.paint(self.theme.error_soft, self.mark('hint'))} ", msg, stream=sys.stderr, code=self.theme.error_soft
        )

    def warn(self, msg: str) -> None:
        sys.stdout.flush()
        prefix = (
            f"{self.paint(self.theme.warning, self.mark('warn'))} "
            f"{self.paint(self.theme.warning, 'warning')} {self.dim('·')} "
        )
        self.emit_wrapped(prefix, msg, stream=sys.stderr)

    def error(self, msg: str) -> None:
        sys.stdout.flush()
        prefix = (
            f"{self.paint(self.theme.error, self.mark('fail'))} "
            f"{self.paint(self.theme.error, 'error')} {self.dim('·')} "
        )
        self.emit_wrapped(prefix, msg, stream=sys.stderr)

    def header(self, title: str, subtitle: str | None = None) -> None:
        width = self.width()
        project = PROJECT.name if "PROJECT" in globals() else "make"
        leader = f"{self.brand(self.mark('bullet'))} {self.brand(project)}"
        if title:
            leader += f"  {self.paint(self.theme.subtle, self.glyph('·', '/'))}  {self.accent(title)}"
        print(leader)
        if subtitle:
            lines = textwrap.wrap(
                subtitle,
                width=max(24, width - 2),
                break_long_words=False,
                break_on_hyphens=False,
            ) or [subtitle]
            for line in lines:
                print(f"  {self.paint(self.theme.subtle, line)}")
        print(self.paint(self.theme.rule, self.rule(width=width)), flush=True)

    def section(self, title: str) -> None:
        width = self.width()
        prefix = f"{self.brand(self.mark('bullet'))} {self.brand(title)} "
        remaining = max(3, width - self.visible_width(prefix))
        print(f"{prefix}{self.paint(self.theme.subtle, self.rule(width=remaining))}")

    def status_word(self, good: bool) -> str:
        code = self.theme.success if good else self.theme.error
        return self.paint(code, self.mark("ok" if good else "fail"))

    def row(self, name: str, state: str | None, value: str, *, good: bool | None = None) -> None:
        state_s = "" if state is None else state
        if good is not None:
            state_s = self.status_word(good)
        name_width = 20 if self.width() >= 70 else 14
        state_col = self.pad_right(state_s, 4)
        name_col = self.pad_right(self.dim(name), name_width)
        prefix = f"  {state_col} {name_col} "
        available = max(16, self.width() - self.visible_width(prefix))
        lines = textwrap.wrap(value, width=available, break_long_words=False, break_on_hyphens=False) or [""]
        print(prefix + lines[0])
        continuation = " " * self.visible_width(prefix)
        for line in lines[1:]:
            print(continuation + line)

    def kv_rows(self, rows: list[tuple[str, str]], *, indent: int = 2) -> None:
        if not rows:
            return
        width = max(self.visible_width(k) for k, _ in rows)
        prefix = " " * indent
        for key, value in rows:
            print(
                f"{prefix}{self.pad_right(self.strong(key), width)}  "
                f"{self.dim(self.glyph('·', ':'))}  {value}"
            )

    def option_rows(self, rows: list[tuple[str, str]], *, indent: int = 2) -> None:
        if not rows:
            return
        total_width = self.width()
        longest = max(self.visible_width(k) for k, _ in rows)
        key_width = min(longest, max(14, min(34, total_width // 2 - 3)))
        prefix = " " * indent
        for key, description in rows:
            if self.visible_width(key) > key_width:
                print(f"{prefix}{self.paint(self.theme.accent, key)}")
                desc_width = max(18, total_width - indent - 2)
                wrapped = textwrap.wrap(
                    description, width=desc_width, break_long_words=False, break_on_hyphens=False
                ) or [""]
                for line in wrapped:
                    print(f"{prefix}  {line}")
                continue
            desc_width = max(18, total_width - indent - key_width - 3)
            wrapped = textwrap.wrap(
                description, width=desc_width, break_long_words=False, break_on_hyphens=False
            ) or [""]
            print(f"{prefix}{self.pad_right(self.paint(self.theme.accent, key), key_width)}   {wrapped[0]}")
            for line in wrapped[1:]:
                print(f"{prefix}{' ' * key_width}   {line}")

    def artifact(self, label: str, path: Path | str, detail: str | None = None) -> None:
        prefix = f"{self.paint(self.theme.success, self.mark('ok'))} {self.strong(label)}  "
        self.emit_wrapped(prefix, str(path), code=self.theme.accent)
        if detail:
            self.hint(detail)

    def color_build_line(self, line: str) -> str:
        if not self.color or not line or "\x1b[" in line:
            return line
        low = line.lower()
        stripped = line.lstrip()
        if (
            "error:" in low
            or "fatal error:" in low
            or "undefined reference" in low
            or stripped.startswith(("FAILED:", "FAIL:"))
            or " failed" in low
            or "***" in line
            or re.search(r"\bError \d+\b", line)
        ):
            return self.paint(self.theme.error, line)
        if "warning:" in low or " warning" in low:
            return self.paint(self.theme.warning, line)
        if "note:" in low:
            return self.paint(self.theme.accent_soft, line)
        if stripped.startswith("[") and "]" in stripped[:16]:
            end = stripped.find("]") + 1
            lead = line[: len(line) - len(stripped)]
            return lead + self.paint(self.theme.accent, stripped[:end]) + stripped[end:]
        if stripped.startswith("--"):
            return self.paint(self.theme.accent_soft, line)
        return line


@dataclass(frozen=True)
class CliError(Exception):
    message: str
    code: int = 2
    hint: str | None = None
    details: tuple[str, ...] = ()

    def __str__(self) -> str:
        return self.message


UI = Renderer()


def parse_color_mode(raw: str | None) -> str:
    value = (raw or "always").strip().lower()
    if value in ("always", "on", "1", "true", "yes"):
        return "always"
    if value in ("never", "off", "0", "false", "no"):
        return "never"
    if value in ("auto", "tty", "default", ""):
        return "auto"
    raise CliError(f"invalid color mode '{raw}' (expected auto|always|never)")


def color_enabled(mode: str) -> bool:
    if "NO_COLOR" in os.environ or os.environ.get("CLICOLOR", "").strip() == "0":
        return False
    if mode == "never":
        return False
    force = os.environ.get("FORCE_COLOR") or os.environ.get("CLICOLOR_FORCE")
    if force is not None and force.strip().lower() not in {"", "0", "false", "no", "off"}:
        return True
    if mode == "always":
        return True
    return (sys.stdout.isatty() or sys.stderr.isatty()) and os.environ.get("TERM", "") != "dumb"


def die(msg: str, code: int = 2, *, hint: str | None = None, details: tuple[str, ...] = ()) -> None:
    raise CliError(msg, code=code, hint=hint, details=details)

### Host

def host_os() -> str:
    s = platform.system()
    if s == "Linux":
        return "linux"
    if s == "Darwin":
        return "macos"
    if s == "Windows" or s.startswith(("MSYS_NT", "MINGW", "CYGWIN_NT")):
        return "windows"
    return s.lower() or "unknown"


def host_arch() -> str:
    return platform.machine() or "unknown"


def which(name: str) -> str:
    return shutil.which(name) or ""


def run_capture(
    cmd: list[str] | str,
    *,
    cwd: Path | None = None,
    shell: bool = False,
    timeout: int | None = None,
) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            cmd,
            cwd=str(cwd or ROOT),
            shell=shell,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired) as exc:
        return subprocess.CompletedProcess(cmd, 127, "", str(exc))


def command_display(cmd: list[str] | str) -> str:
    if isinstance(cmd, str):
        return cmd
    return " ".join(shlex.quote(str(x)) for x in cmd)


def format_elapsed(seconds: float) -> str:
    if seconds < 1:
        return f"{int(seconds * 1000)}ms"
    if seconds < 60:
        return f"{seconds:.1f}s"
    minutes = int(seconds // 60)
    return f"{minutes}m{seconds - minutes * 60:04.1f}s"


def run(
    cmd: list[str] | str,
    *,
    cwd: Path | None = None,
    shell: bool = False,
    env: dict[str, str] | None = None,
    quiet: bool = False,
    announce: bool = True,
) -> None:
    merged = os.environ.copy()
    if env:
        merged.update(env)
    if UI.color:
        merged.setdefault("FORCE_COLOR", "1")
        merged.setdefault("CLICOLOR_FORCE", "1")
    if UI.verbose and announce:
        UI.step("$ " + command_display(cmd))
    if quiet:
        res = subprocess.run(
            cmd,
            cwd=str(cwd or ROOT),
            shell=shell,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=merged,
        )
        if res.returncode != 0:
            if res.stdout:
                sys.stderr.write(res.stdout)
            raise subprocess.CalledProcessError(res.returncode, cmd)
        return
    proc = subprocess.Popen(
        cmd,
        cwd=str(cwd or ROOT),
        shell=shell,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=merged,
        bufsize=1,
    )
    assert proc.stdout is not None
    for line in proc.stdout:
        print(UI.color_build_line(line.rstrip("\n")), flush=True)
    rc = proc.wait()
    if rc != 0:
        raise subprocess.CalledProcessError(rc, cmd)


### Config

def config_candidates(project_name: str) -> list[Path]:
    explicit = os.environ.get("MAKE_CONFIG", "").strip()
    out: list[Path] = []
    if explicit:
        out.append(Path(explicit).expanduser())
    out.extend([ROOT / ".make" / "config", ROOT / "make.config"])
    xdg = Path(os.environ.get("XDG_CONFIG_HOME", "") or (Path.home() / ".config"))
    out.extend([xdg / project_name / "make.config", xdg / "make" / "config"])
    seen: set[str] = set()
    final: list[Path] = []
    for p in out:
        key = str(p)
        if key not in seen:
            final.append(p)
            seen.add(key)
    return final


def load_config(project_name: str) -> None:
    for path in config_candidates(project_name):
        try:
            text = path.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        loaded = False
        for line in text.splitlines():
            s = line.strip()
            if not s or s.startswith(("#", ";")):
                continue
            if s.startswith("export "):
                s = s[7:].strip()
            if "=" not in s:
                continue
            k, v = s.split("=", 1)
            k = k.strip()
            if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", k) or k in os.environ:
                continue
            v = v.strip().strip("'\"").replace("${ROOT}", str(ROOT)).replace("$ROOT", str(ROOT))
            os.environ[k] = os.path.expandvars(os.path.expanduser(v))
            loaded = True
        if loaded:
            LOADED_CONFIGS.append(path)


def env_flag(name: str, default: bool = False) -> bool:
    raw = os.environ.get(name, "").strip().lower()
    if not raw:
        return default
    if raw in ("1", "true", "yes", "on", "y"):
        return True
    if raw in ("0", "false", "no", "off", "n"):
        return False
    return default


def env_list(name: str) -> list[str]:
    raw = os.environ.get(name, "").strip()
    if not raw:
        return []
    return [p for p in re.split(r"[:;,]", raw) if p]


### Project discovery

@dataclass
class Project:
    name: str
    target: str
    runtime_lib: str
    build_target: str
    release_target: str
    source_dirs: list[str]
    source_files: list[str]
    package_files: list[str]


def makefile_path() -> Path:
    for name in ("Makefile", "makefile", "GNUmakefile"):
        p = ROOT / name
        if p.exists():
            return p
    return ROOT / "Makefile"


def makefile_text() -> str:
    p = makefile_path()
    try:
        return p.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return ""


def parse_make_var_raw(name: str, default: str = "") -> str:
    text = makefile_text()
    m = re.search(rf"^\s*{re.escape(name)}\s*(?::=|\+=|=)\s*(.*?)\s*$", text, re.M)
    if not m:
        return default
    return m.group(1).strip().strip('"\'')


def expand_make_value(value: str, seen: set[str] | None = None) -> str:
    seen = seen or set()

    def repl(match: re.Match[str]) -> str:
        name = match.group(1)
        if name in seen:
            return match.group(0)
        raw = parse_make_var_raw(name, os.environ.get(name, ""))
        return expand_make_value(raw, seen | {name}) if raw else ""

    return re.sub(r"\$\(([A-Za-z_][A-Za-z0-9_]*)\)", repl, value)


def parse_make_var(name: str, default: str = "") -> str:
    return expand_make_value(parse_make_var_raw(name, default))


def make_targets() -> set[str]:
    targets: set[str] = set()
    for line in makefile_text().splitlines():
        if not line or line.startswith("\t") or ":" not in line:
            continue
        left = line.split(":", 1)[0]
        if "=" in left:
            continue
        for target in left.split():
            if re.match(r"^[A-Za-z0-9_.@/+%-]+$", target) and not target.startswith("."):
                targets.add(target)
    return targets


def make_has_target(target: str) -> bool:
    return target in make_targets()


def find_system_make() -> str:
    for name in ("gmake", "make"):
        p = which(name)
        if p and Path(p).resolve() != Path(__file__).resolve():
            return p
    return ""


def system_make() -> str:
    p = find_system_make()
    if p:
        return p
    die("system make was not found")


def discover_project() -> Project:
    target = os.environ.get("MAKE_TARGET", "").strip()
    if not target:
        target_base = parse_make_var("TARGET_BASE")
        target = target_base or parse_make_var("TARGET")
        if "$(" in target:
            target = target_base or "monad"
        if not target and (ROOT / "src" / "main.c").exists():
            target = "monad"
    if host_os() == "windows" and target.endswith(".exe"):
        target = target[:-4]
    name = os.environ.get("MAKE_PROJECT_NAME", "").strip() or target or ROOT.name
    runtime_lib = os.environ.get("MAKE_RUNTIME_LIB", "").strip() or parse_make_var("RUNTIME_LIB") or "libmonad.a"
    source_dirs = [p for p in ("src", "include", "core", "lib", "tests", "context", "docs", "scripts") if (ROOT / p).exists()]
    source_files = [p for p in ("Makefile", "makefile", "GNUmakefile", "make", "main.c", "runtime.c", "runtime.h") if (ROOT / p).exists()]
    package_files = [
        p
        for p in (
            "README", "README.md", "LICENSE", "LICENSE.txt", "COPYING",
            "CHANGELOG.md", "pyproject.toml", "CMakeLists.txt",
        )
        if (ROOT / p).exists()
    ]
    # The Python frontend is the build system.  Keep the target names as
    # descriptive metadata even when no legacy Makefile is present.
    build_target = os.environ.get("MAKE_BUILD_TARGET", "").strip() or ("all" if (ROOT / "src" / "main.c").exists() else "")
    release_target = os.environ.get("MAKE_RELEASE_TARGET", "").strip() or ("release" if build_target else "")
    return Project(
        name=name,
        target=target,
        runtime_lib=runtime_lib,
        build_target=build_target,
        release_target=release_target,
        source_dirs=source_dirs,
        source_files=source_files,
        package_files=package_files,
    )


PROJECT = discover_project()
load_config(PROJECT.name)
PROJECT = discover_project()


@dataclass(frozen=True)
class AppContext:
    project: Project
    jobs: int
    jobs_source: str
    ui: Renderer


@dataclass(frozen=True)
class BuildConfig:
    """Canonical native build configuration owned by the Python frontend."""

    mode: str = "debug"

    @property
    def cc(self) -> str:
        return os.environ.get("CC", "cc")

    @property
    def ar(self) -> str:
        return os.environ.get("AR", "ar")

    @property
    def cppflags(self) -> list[str]:
        return shlex.split(os.environ.get("CPPFLAGS", "")) + [
            "-iquote", str(ROOT / "src"),
            "-iquote", str(ROOT / "src" / "tooling"),
            "-I", str(ROOT / "src" / "embed" / "include"),
        ]

    @property
    def llvm_cflags(self) -> list[str]:
        result = run_capture(["llvm-config", "--cflags"])
        return shlex.split(result.stdout.strip()) if result.returncode == 0 else []

    @property
    def ffi_cflags(self) -> list[str]:
        result = run_capture(["pkg-config", "--cflags", "libclang"])
        if result.returncode == 0:
            return shlex.split(result.stdout.strip())
        return ["-I/usr/lib/llvm/include"]

    @property
    def common_cflags(self) -> list[str]:
        flags = ["-Wall", "-Wextra", "-std=c99", "-fPIC"]
        flags.extend(self.llvm_cflags)
        flags.extend(shlex.split(os.environ.get("CFLAGS", "")))
        if self.mode == "debug":
            flags.extend(["-g", "-DDEBUG"])
        elif self.mode == "release":
            flags.extend(["-DNDEBUG", "-O2"])
        elif self.mode == "asan":
            flags.extend(["-g", "-fsanitize=address", "-fno-omit-frame-pointer", "-DDEBUG"])
        elif self.mode == "ubsan":
            flags.extend(["-g", "-fsanitize=undefined", "-fno-omit-frame-pointer",
                          "-fno-sanitize-recover=undefined", "-DDEBUG"])
        elif self.mode == "perf":
            flags.extend(["-O2", "-g", "-fno-omit-frame-pointer", "-DNDEBUG"])
        else:
            raise CliError(f"unknown build mode '{self.mode}'")
        return flags

    @property
    def link_flags(self) -> list[str]:
        llvm = run_capture(["llvm-config", "--ldflags", "--libs", "core", "orcjit", "native", "passes"])
        llvm_flags = shlex.split(llvm.stdout.strip()) if llvm.returncode == 0 else []
        flags = ["-lm", "-lreadline", "-lpthread", "-lgmp", *llvm_flags, "-lclang"]
        flags.extend(shlex.split(os.environ.get("LDFLAGS", "")))
        if self.mode == "asan":
            flags.extend(["-fsanitize=address", "-fno-omit-frame-pointer"])
        elif self.mode == "ubsan":
            flags.extend(["-fsanitize=undefined", "-fno-sanitize-recover=undefined"])
        return flags


def _source_sets() -> dict[str, list[Path]]:
    src = ROOT / "src"
    runtime = [src / name for name in ("arena.c", "runtime.c", "runtime_errors.c")]
    compiler = sorted(src.glob("*.c"))
    compiler = [path for path in compiler if path not in runtime]
    if host_os() == "windows":
        compiler = [path for path in compiler if path.name != "debugger.c"]
    for directory in ("qtt", "concurrency", "effects", "tooling"):
        compiler.extend(sorted((src / directory).glob("*.c")))
    embed = [src / "embed" / name for name in ("embed.c", "monad.c")]
    api_names = (
        "embed/compiler.c", "embed/surface_compiler.c", "embed/surface_load.c",
        "embed/compiler_native.c", "embed/frontend_transaction.c", "embed/native_compile.c",
        "embed/infer_support.c", "infer.c", "features.c", "macro.c", "pmatch.c",
        "reader.c", "reader_diagnostic.c", "reader_syntax.c", "types.c", "wisp.c",
        "wisp_syntax_policy.c", "codegen.c", "env.c", "typeclass.c", "module.c",
        "asm.c", "ffi.c", "effects/effect.c", "effects/constraints.c",
        "qtt/foreign_type.c", "qtt/type_identity.c", "qtt/constraints.c",
        "qtt/environment.c", "qtt/quantity.c", "qtt/bindings.c", "qtt/elaboration.c",
        "qtt/pipeline.c", "qtt/anf.c", "qtt/core.c", "qtt/demand.c", "qtt/graded.c",
        "qtt/signature.c", "qtt/call.c", "qtt/resource.c", "qtt/signature_env.c",
        "qtt/backend.c", "qtt/compiler.c", "qtt/compiler_module.c", "qtt/core_effect.c",
        "qtt/interface.c", "qtt/semantic_ir.c", "qtt/effect_runtime.c", "qtt/module.c",
        "qtt/drop.c", "qtt/evidence.c", "qtt/closure_policy.c", "qtt/closure.c",
        "qtt/core_usage.c", "qtt/semantic_anf.c",
    )
    api = [src / name for name in api_names]
    return {"runtime": runtime, "compiler": compiler, "embed": embed, "api": api}


def _object_path(source: Path, kind: str) -> Path:
    relative = source.relative_to(ROOT).with_suffix(".o")
    return BUILD_ROOT / "obj" / kind / relative


def _header_dependencies() -> list[Path]:
    return sorted(path for path in (ROOT / "src").rglob("*.h"))


def _compile_command(source: Path, output: Path, config: BuildConfig, *, embed: bool = False) -> list[str]:
    flags = config.common_cflags.copy()
    if embed:
        flags.extend(["-fvisibility=hidden", "-DMONAD_EMBED_BUILD"])
    elif source.name == "ffi.c":
        flags.extend(config.ffi_cflags)
    return [config.cc, *config.cppflags, *flags, "-c", str(source), "-o", str(output)]


def _compile_sources(
    sources: list[Path], kind: str, config: BuildConfig, *, embed: bool = False,
    jobs: int = 1, executor: ThreadPoolExecutor | None = None,
) -> list[dict[str, object]]:
    headers = _header_dependencies()
    entries: list[dict[str, object]] = []
    pending: list[tuple[Path, list[str]]] = []
    for source in sources:
        output = _object_path(source, kind)
        output.parent.mkdir(parents=True, exist_ok=True)
        command = _compile_command(source, output, config, embed=embed)
        entries.append({"directory": str(ROOT), "file": str(source.resolve()), "arguments": command})
        newest_dependency = max([source, *headers], key=lambda path: path.stat().st_mtime_ns)
        if output.exists() and output.stat().st_mtime_ns >= newest_dependency.stat().st_mtime_ns:
            continue
        pending.append((source, command))

    def compile_one(item: tuple[Path, list[str]]) -> None:
        _source, command = item
        run(command, announce=False)

    if pending:
        if executor is not None:
            list(executor.map(compile_one, pending))
        else:
            with ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
                list(pool.map(compile_one, pending))
    return entries


def build_project(mode: str = "debug", *, jobs: int = 0) -> Path:
    """Build all native outputs without delegating policy back to GNU Make."""
    config = BuildConfig(mode)
    sets = _source_sets()
    worker_count = jobs if jobs > 0 else resolve_jobs(0)[0]
    with ThreadPoolExecutor(max_workers=worker_count) as executor:
        runtime_objects = _compile_sources(sets["runtime"], "runtime", config, jobs=worker_count, executor=executor)
        compiler_objects = _compile_sources(sets["compiler"], "compiler", config, jobs=worker_count, executor=executor)
        embed_objects = _compile_sources(sets["embed"], "embed", config, embed=True, jobs=worker_count, executor=executor)
        api_objects = _compile_sources(sets["api"], "compiler-api", config, jobs=worker_count, executor=executor)

    lib_dir = BUILD_ROOT / "lib"
    bin_dir = BUILD_ROOT / "bin"
    lib_dir.mkdir(parents=True, exist_ok=True)
    bin_dir.mkdir(parents=True, exist_ok=True)
    runtime_lib = lib_dir / "libmonad.a"
    embed_static = lib_dir / "libmonad-embed.a"
    embed_shared = lib_dir / ("libmonad-embed.dll" if host_os() == "windows" else "libmonad-embed.so")
    compiler_static = lib_dir / "libmonad-compiler.a"
    compiler_shared = lib_dir / ("libmonad-compiler.dll" if host_os() == "windows" else "libmonad-compiler.so")
    target = bin_dir / ("monad.exe" if host_os() == "windows" else "monad")

    def archive(output: Path, objects: list[Path]) -> None:
        if output.exists() and output.stat().st_mtime_ns >= max(path.stat().st_mtime_ns for path in objects):
            return
        run([config.ar, "rcs", str(output), *(str(path) for path in objects)])

    runtime_paths = [_object_path(source, "runtime") for source in sets["runtime"]]
    embed_paths = [_object_path(source, "embed") for source in sets["embed"]]
    compiler_paths = [_object_path(source, "compiler") for source in sets["compiler"]]
    api_paths = [_object_path(source, "compiler-api") for source in sets["api"]]
    archive(runtime_lib, runtime_paths)
    archive(embed_static, embed_paths)
    if not embed_shared.exists() or embed_shared.stat().st_mtime_ns < max(path.stat().st_mtime_ns for path in embed_paths):
        run([config.cc, "-shared", "-o", str(embed_shared), *(str(path) for path in embed_paths), "-lpthread"])
    archive(compiler_static, [*api_paths, *runtime_paths])
    if not compiler_shared.exists() or compiler_shared.stat().st_mtime_ns < max(path.stat().st_mtime_ns for path in [*api_paths, *runtime_paths, embed_shared]):
        run([config.cc, "-shared", "-o", str(compiler_shared), *(str(path) for path in [*api_paths, *runtime_paths]),
             f"-Wl,--version-script={ROOT / 'src' / 'embed' / 'compiler.exports'}",
             f"-L{lib_dir}", "-lmonad-embed", "-lpthread", "-lgmp", "-lclang",
             *shlex.split(run_capture(["llvm-config", "--ldflags", "--libs", "core", "orcjit", "native", "passes"]).stdout)])
    if not target.exists() or target.stat().st_mtime_ns < max(path.stat().st_mtime_ns for path in [*compiler_paths, runtime_lib]):
        link_flags = config.link_flags.copy()
        link_prefix: list[str] = []
        if host_os() != "windows":
            link_prefix.append("-rdynamic")
        run([config.cc, *config.cppflags, *config.common_cflags, *link_prefix, "-o", str(target),
             *(str(path) for path in compiler_paths), str(runtime_lib), *link_flags])
    return target


def rel(path: Path) -> str:
    """Display a checkout-relative path without dereferencing symlinks."""
    try:
        absolute = path.absolute() if path.is_absolute() else (ROOT / path).absolute()
        return str(absolute.relative_to(ROOT.absolute()))
    except Exception:
        return str(path)


### File helpers

def copy_file(src: Path, dst: Path) -> None:
    if not src.exists() or not src.is_file():
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def copy_file_unique(src: Path, dst: Path) -> bool:
    """Copy src only when dst does not already contain the same file."""
    if not src.exists() or not src.is_file():
        return False
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists():
        try:
            if src.stat().st_size == dst.stat().st_size and file_sha256(src) == file_sha256(dst):
                return False
        except OSError:
            pass
        die(f"vendor filename collision: {src} and {dst}")
    shutil.copy2(src, dst)
    return True


def relative_symlink(target: Path, link: Path) -> None:
    """Create a relocatable symlink inside an artifact tree."""
    link.parent.mkdir(parents=True, exist_ok=True)
    if link.exists() or link.is_symlink():
        link.unlink()
    link.symlink_to(os.path.relpath(target, link.parent))


def copytree_replace(src: Path, dst: Path, ignore=None) -> None:
    if not src.exists():
        return
    shutil.rmtree(dst, ignore_errors=True)
    shutil.copytree(src, dst, symlinks=True, ignore=ignore)


def chmod_executable(path: Path) -> None:
    try:
        path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    except OSError:
        pass


def file_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def write_manifest(root: Path, extra: dict[str, object] | None = None, *, filename: str = "MANIFEST.json") -> None:
    files: list[dict[str, object]] = []
    for p in sorted(root.rglob("*")):
        if p.name == filename:
            continue
        if p.is_symlink():
            try:
                target = os.readlink(p)
            except OSError:
                target = ""
            files.append({"path": str(p.relative_to(root)), "symlink": target})
        elif p.is_file():
            files.append({"path": str(p.relative_to(root)), "size": p.stat().st_size, "sha256": file_sha256(p)})
    data: dict[str, object] = {
        "project": PROJECT.name,
        "target": PROJECT.target,
        "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "host": {"os": host_os(), "arch": host_arch()},
        "files": files,
    }
    if extra:
        data.update(extra)
    (root / filename).write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def make_tar_gz(archive_base: Path, parent: Path, package_name: str) -> Path:
    tar_path = archive_base.with_suffix(".tar.gz")
    tar_path.parent.mkdir(parents=True, exist_ok=True)
    tar_bin = which("tar")
    if tar_bin:
        tmp = tar_path.with_suffix(".tar.gz.tmp")
        if tmp.exists():
            tmp.unlink()
        run([tar_bin, "-C", str(parent), "-czf", str(tmp), package_name], quiet=True)
        tmp.replace(tar_path)
        return tar_path
    with tarfile.open(tar_path, "w:gz") as tf:
        tf.add(parent / package_name, arcname=package_name, recursive=True)
    return tar_path


def possible_binary_paths() -> list[Path]:
    exe = ".exe" if host_os() == "windows" else ""
    out: list[Path] = []
    if PROJECT.target:
        for prefix in (ROOT, BUILD_ROOT, BUILD_ROOT / "release", BUILD_ROOT / "debug", ROOT / "bin", BUILD_ROOT / "bin"):
            out.append(prefix / (PROJECT.target + exe))
        # Source packages may carry a verified portable launcher. Keep it last
        # so a freshly built project binary always wins.
        out.append(VENDOR_DIR / "run")
    return out


def find_binary(build_if_missing: bool = False, jobs: int = 0) -> Path:
    for p in possible_binary_paths():
        if p.exists() and p.is_file():
            return p
    if build_if_missing:
        UI.step("build: running Python build plan")
        build_project("debug", jobs=jobs)
        for p in possible_binary_paths():
            if p.exists() and p.is_file():
                return p
    if PROJECT.target:
        die(f"could not find binary '{PROJECT.target}'. Build it first or set MAKE_TARGET.")
    die("could not infer binary target. Set MAKE_TARGET=name.")


def resolve_program(name_or_path: str) -> Path:
    p = Path(name_or_path).expanduser()
    if p.exists():
        return p.resolve()
    found = which(name_or_path)
    if found:
        return Path(found).resolve()
    die(f"command or binary not found: {name_or_path}")


### Dependency report

def detected_link_libraries() -> list[str]:
    names = re.findall(r"(?:^|\s)-l([A-Za-z0-9_+.-]+)", makefile_text())
    if (ROOT / "src" / "main.c").exists():
        names.extend(("clang", "gmp", "m", "monad-embed", "pthread", "readline"))
    names.extend(env_list("MAKE_VENDOR_LIBS"))
    return sorted(dict.fromkeys(names))


def detected_pkg_config_packages() -> list[str]:
    """Extract package operands from simple pkg-config shell expressions."""
    pkgs: list[str] = []
    for match in re.finditer(r"pkg-config\s+([^\n\r`$()]*)", makefile_text()):
        # Ignore fallback shell branches such as `|| echo -I/usr/...`; they are
        # not pkg-config package names.
        command = re.split(r"\|\||&&|;", match.group(1), maxsplit=1)[0].strip()
        try:
            tokens = shlex.split(command)
        except ValueError:
            tokens = command.split()
        for token in tokens:
            if token.startswith("-") or re.match(r"^[0-9]*[<>]", token):
                continue
            if ">/" in token or token in {"true", "false"}:
                continue
            if re.fullmatch(r"[A-Za-z0-9_.+-]+", token):
                pkgs.append(token)
    if (ROOT / "src" / "main.c").exists():
        pkgs.append("libclang")
    pkgs.extend(env_list("MAKE_VENDOR_PKGS"))
    return sorted(dict.fromkeys(pkgs))


def llvm_config_path() -> str:
    raw = os.environ.get("LLVM_CONFIG", "").strip()
    for name in [
        raw,
        "llvm-config", "llvm-config-22", "llvm-config-21", "llvm-config-20",
        "llvm-config-19", "llvm-config-18", "llvm-config-17", "llvm-config-16",
    ]:
        if not name:
            continue
        if os.sep in name and Path(name).exists():
            return name
        found = which(name)
        if found:
            return found
    return ""


def dependency_report() -> dict[str, object]:
    return {
        "link_libraries": detected_link_libraries(),
        "pkg_config_packages": detected_pkg_config_packages(),
        "llvm_config": llvm_config_path(),
    }


### Runtime closure

def ldd_dependency_info(binary: Path) -> tuple[list[tuple[str, Path]], list[str]]:
    """Return Linux dynamic dependencies as (soname, path) plus unresolved sonames."""
    if host_os() != "linux" or not which("ldd"):
        return [], []
    res = run_capture(["ldd", str(binary)], timeout=10)
    text = (res.stdout or "") + "\n" + (res.stderr or "")
    found: list[tuple[str, Path]] = []
    missing: list[str] = []
    for line in text.splitlines():
        s = line.strip()
        if not s or "linux-vdso" in s or "not a dynamic executable" in s.lower() or "statically linked" in s.lower():
            continue
        m_missing = re.match(r"(\S+)\s+=>\s+not found", s)
        if m_missing:
            missing.append(m_missing.group(1))
            continue
        m = re.match(r"(\S+)\s+=>\s+(/\S+)", s)
        if m and Path(m.group(2)).exists():
            found.append((m.group(1), Path(m.group(2))))
            continue
        if s.startswith("/"):
            p = Path(s.split()[0])
            if p.exists():
                found.append((p.name, p))
    dedup: dict[tuple[str, str], tuple[str, Path]] = {}
    for name, path in found:
        dedup[(name, str(path))] = (name, path)
    return list(dedup.values()), list(dict.fromkeys(missing))


def elf_needed_names(binary: Path) -> list[str]:
    if host_os() != "linux" or not which("readelf"):
        return []
    res = run_capture(["readelf", "-d", str(binary)], timeout=10)
    if res.returncode != 0:
        return []
    out: list[str] = []
    for line in res.stdout.splitlines():
        if "(NEEDED)" not in line:
            continue
        m = re.search(r"\[(.*?)\]", line)
        if m:
            out.append(m.group(1))
    return list(dict.fromkeys(out))


def otool_paths(binary: Path) -> list[Path]:
    if host_os() != "macos" or not which("otool"):
        return []
    res = run_capture(["otool", "-L", str(binary)], timeout=10)
    out: list[Path] = []
    for line in res.stdout.splitlines()[1:]:
        first = line.strip().split(" ", 1)[0]
        if first.startswith("/") and Path(first).exists():
            out.append(Path(first))
        elif first.startswith("@loader_path/"):
            p = binary.parent / first[len("@loader_path/"):]
            if p.exists():
                out.append(p)
        elif first.startswith("@executable_path/"):
            p = binary.parent / first[len("@executable_path/"):]
            if p.exists():
                out.append(p)
    return list(dict.fromkeys(out))


def binary_linkage_status(binary: Path) -> tuple[str, str]:
    """Classify a binary as static, dynamic, or unknown without guessing from an empty dependency list."""
    if not binary.exists() or not binary.is_file():
        return "unknown", "file does not exist"
    if host_os() == "linux":
        if which("ldd"):
            res = run_capture(["ldd", str(binary)], timeout=10)
            raw = ((res.stdout or "") + "\n" + (res.stderr or "")).strip()
            low = raw.lower()
            needed = elf_needed_names(binary)
            if "not a dynamic executable" in low or "statically linked" in low:
                return "static", "ELF static executable"
            if "=>" in raw or needed:
                count = len(needed)
                noun = "entry" if count == 1 else "entries"
                return "dynamic", f"ELF dynamic executable · {count} direct DT_NEEDED {noun}"
        if which("readelf"):
            res = run_capture(["readelf", "-l", str(binary)], timeout=10)
            if "Requesting program interpreter" in res.stdout:
                return "dynamic", "ELF program interpreter present"
        if which("file"):
            detail = run_capture(["file", "-b", str(binary)], timeout=5).stdout.strip()
            low = detail.lower()
            if "statically linked" in low:
                return "static", detail
            if "dynamically linked" in low:
                return "dynamic", detail
            return "unknown", detail
    if host_os() == "macos":
        deps = otool_paths(binary)
        if deps:
            return "dynamic", f"{len(deps)} Mach-O dependencies"
        if which("file"):
            return "unknown", run_capture(["file", "-b", str(binary)], timeout=5).stdout.strip()
    return "unknown", "linkage inspection unavailable on this host"


def runtime_search_dirs(binary: Path) -> list[Path]:
    out: list[Path] = [binary.parent]
    for var in ("LD_LIBRARY_PATH", "LIBRARY_PATH", "DYLD_LIBRARY_PATH"):
        for item in os.environ.get(var, "").split(os.pathsep):
            if item.strip():
                out.append(Path(item).expanduser())
    cfg = llvm_config_path()
    if cfg:
        libdir = run_capture([cfg, "--libdir"], timeout=5).stdout.strip().splitlines()
        if libdir:
            out.append(Path(libdir[0]))
    for p in (
        "/lib", "/lib64", "/usr/lib", "/usr/lib64", "/usr/local/lib", "/usr/local/lib64",
        "/usr/lib/x86_64-linux-gnu", "/lib/x86_64-linux-gnu",
        "/usr/lib/aarch64-linux-gnu", "/lib/aarch64-linux-gnu",
    ):
        out.append(Path(p))
    final: list[Path] = []
    seen: set[str] = set()
    for p in out:
        try:
            key = str(p.resolve())
        except Exception:
            key = str(p)
        if key not in seen and p.exists() and p.is_dir():
            seen.add(key)
            final.append(p)
    return final


def soname_major(name: str) -> str:
    m = re.search(r"\.so\.(\d+)", name)
    return m.group(1) if m else ""


def resolve_soname(name: str, search_dirs: list[Path]) -> Path | None:
    for d in search_dirs:
        p = d / name
        if p.exists():
            return p
    if ".so" not in name:
        return None
    stem = name.split(".so", 1)[0] + ".so"
    required_major = soname_major(name)
    candidates: list[Path] = []
    for d in search_dirs:
        try:
            candidates.extend(d.glob(stem + "*"))
        except OSError:
            pass
    for p in sorted(candidates, key=lambda x: (0 if x.name == name else 1, len(x.name))):
        if not p.exists():
            continue
        if required_major:
            candidate_major = soname_major(p.name)
            try:
                resolved_major = soname_major(p.resolve().name)
            except Exception:
                resolved_major = candidate_major
            if candidate_major not in ("", required_major) or resolved_major not in ("", required_major):
                continue
        return p
    return None


def is_system_runtime_name(name: str) -> bool:
    n = Path(name).name
    base = n.split(".so", 1)[0]
    if n.startswith(("ld-linux", "ld-musl")):
        return True
    if n.startswith("libSystem"):
        return True
    return base in {"libc", "libm", "libdl", "libpthread", "librt", "libutil", "libgcc_s", "libstdc++"}


def dependency_closure_details(binary: Path, *, include_system: bool = True) -> tuple[list[Path], list[str]]:
    """Return transitive runtime dependencies and unresolved sonames."""
    queue: list[Path] = [binary]
    seen_inputs: set[str] = set()
    seen_output: set[str] = set()
    out: list[Path] = []
    unresolved: set[str] = set()
    search_dirs = runtime_search_dirs(binary)
    root_key = str(binary.resolve()) if binary.exists() else str(binary)

    while queue:
        cur = queue.pop(0)
        try:
            cur_key = str(cur.resolve())
        except Exception:
            cur_key = str(cur)
        if cur_key in seen_inputs or not cur.exists():
            continue
        seen_inputs.add(cur_key)

        deps: list[tuple[str, Path]] = []
        missing: list[str] = []
        if host_os() == "linux":
            deps, missing = ldd_dependency_info(cur)
            resolved_names = {name for name, _ in deps}
            for needed in elf_needed_names(cur):
                if needed in resolved_names:
                    continue
                resolved = resolve_soname(needed, search_dirs)
                if resolved is not None:
                    deps.append((needed, resolved))
                    resolved_names.add(needed)
                elif needed not in missing:
                    missing.append(needed)
        else:
            deps = [(p.name, p) for p in otool_paths(cur)]

        for name in missing:
            if include_system or not is_system_runtime_name(name):
                unresolved.add(name)

        for name, dep in deps:
            if not include_system and is_system_runtime_name(name):
                continue
            try:
                real = dep.resolve()
            except Exception:
                real = dep
            if not real.exists() or not real.is_file():
                unresolved.add(name)
                continue
            key = str(real)
            if key != root_key and key not in seen_output:
                seen_output.add(key)
                out.append(real)
            if key not in seen_inputs:
                queue.append(real)
            if real.parent not in search_dirs:
                search_dirs.insert(0, real.parent)

    return out, sorted(unresolved)


def elf_soname(path: Path) -> str:
    """Return an ELF DT_SONAME when available."""
    if host_os() != "linux" or not which("readelf"):
        return ""
    proc = run_capture(["readelf", "-d", str(path)], timeout=10)
    if proc.returncode != 0:
        return ""
    match = re.search(r"\(SONAME\).*?\[([^]]+)\]", proc.stdout)
    return match.group(1).strip() if match else ""


def ensure_elf_soname_alias(path: Path, lib_dir: Path) -> None:
    """Materialize the loader-visible SONAME alias for a copied ELF library."""
    soname = elf_soname(path)
    if not soname or soname == path.name:
        return
    alias = lib_dir / soname
    if alias.exists() or alias.is_symlink():
        return
    relative_symlink(path, alias)
    if UI.verbose:
        UI.log("LINK", f"{alias.name} -> {path.name}", "accent_soft")


def copy_runtime_flat(binary: Path, bin_dir: Path, lib_dir: Path, *, include_system: bool = True) -> tuple[Path, list[Path]]:
    """Create a relocatable runtime layout and fail closed on unresolved dependencies."""
    bin_dir.mkdir(parents=True, exist_ok=True)
    lib_dir.mkdir(parents=True, exist_ok=True)
    binary_dst = bin_dir / binary.name
    copy_file(binary, binary_dst)
    chmod_executable(binary_dst)
    closure, unresolved = dependency_closure_details(binary, include_system=include_system)
    if unresolved:
        message = "unresolved runtime dependencies: " + ", ".join(unresolved)
        if env_flag("MAKE_VENDOR_ALLOW_UNRESOLVED", False):
            UI.warn(message)
        else:
            die(message + " (set MAKE_VENDOR_ALLOW_UNRESOLVED=1 to override)")
    copied: list[Path] = []
    for dep in closure:
        dst = lib_dir / dep.name
        if copy_file_unique(dep, dst):
            copied.append(dst)
            if UI.verbose:
                UI.log("LIB", dep.name, "accent_soft")
        ensure_elf_soname_alias(dst, lib_dir)
    return binary_dst, copied


def copy_binary_rootfs(binary: Path, rootfs: Path, *, include_system: bool = True) -> list[Path]:
    """Optional chroot view. Not used by the default lean vendor layout."""
    copied: list[Path] = []
    rootfs.mkdir(parents=True, exist_ok=True)
    dst = rootfs / "bin" / binary.name
    copy_file(binary, dst)
    chmod_executable(dst)
    copied.append(dst)
    closure, unresolved = dependency_closure_details(binary, include_system=include_system)
    if unresolved and not env_flag("MAKE_VENDOR_ALLOW_UNRESOLVED", False):
        die("rootfs unresolved runtime dependencies: " + ", ".join(unresolved))
    for dep in closure:
        dep_dst = rootfs / str(dep).lstrip("/")
        copy_file(dep, dep_dst)
        copied.append(dep_dst)
    return copied

### LLVM vendor

def llvm_library_matches(name: str, major: str) -> bool:
    """Accept generic linker names and libraries belonging to the active LLVM major only."""
    if not name.startswith(("libLLVM", "libclang")):
        return False
    if name in {"libLLVM.so", "libclang.so", "libclang-cpp.so", "libLLVM.dylib", "libclang.dylib", "libclang-cpp.dylib"}:
        return True
    return bool(re.search(rf"(?:^|[-.]){re.escape(major)}(?:[.-]|$)", name))


def llvm_required_link_names(llvm_libs: str) -> list[str]:
    names = set(detected_link_libraries())
    names.update(re.findall(r"(?:^|\s)-l([^\s]+)", llvm_libs))
    return sorted(name for name in names if name.startswith(("LLVM", "clang")))


def bundle_llvm(vendor_dir: Path) -> None:
    if not project_needs_llvm() and not env_flag("MAKE_VENDOR_LLVM", False):
        return
    cfg = llvm_config_path()
    if not cfg:
        UI.warn("vendor: llvm-config was not found; LLVM/Clang headers were not bundled")
        return
    version = run_capture([cfg, "--version"]).stdout.strip() or "unknown"
    major = version.split(".", 1)[0]
    inc_s = run_capture([cfg, "--includedir"]).stdout.strip().splitlines()
    lib_s = run_capture([cfg, "--libdir"]).stdout.strip().splitlines()
    cflags = run_capture([cfg, "--cflags"]).stdout.strip()
    libs = run_capture([cfg, "--libs", "all", "--system-libs"]).stdout.strip()
    ldflags = run_capture([cfg, "--ldflags"]).stdout.strip()
    if not inc_s:
        UI.warn("vendor: llvm-config did not report an include directory")
        return
    inc = Path(inc_s[0])
    libdir = Path(lib_s[0]) if lib_s else Path()
    vendor_include = vendor_dir / "include"
    vendor_lib = vendor_dir / "lib"
    vendor_bin = vendor_dir / "bin"

    # C API headers are the lean default. Shipping the full LLVM/Clang C++
    # header trees costs tens of megabytes and remains explicitly opt-in.
    header_sets = ["llvm-c", "clang-c"]
    if env_flag("MAKE_VENDOR_LLVM_CPP_HEADERS", False):
        header_sets += ["llvm", "clang"]
    for sub in header_sets:
        src = inc / sub
        if src.exists():
            copytree_replace(src, vendor_include / sub)

    # Copy only libraries that the project or active llvm-config actually asks
    # the linker for. This deliberately avoids unrelated libclang-cpp and old
    # LLVM installations. Versioned payload bytes are stored once; linker names
    # are tiny relocatable symlinks.
    required = llvm_required_link_names(libs)
    if libdir.exists():
        for link_name in required:
            generic_names = [f"lib{link_name}.so", f"lib{link_name}.dylib"]
            source: Path | None = None
            alias_name = ""
            for generic in generic_names:
                cand = libdir / generic
                if cand.exists() or cand.is_symlink():
                    source = cand
                    alias_name = generic
                    break
            if source is None:
                matches = [
                    cand for cand in sorted(libdir.glob(f"lib{link_name}.*"))
                    if (cand.is_file() or cand.is_symlink()) and llvm_library_matches(cand.name, major)
                ]
                if matches:
                    source = matches[0]
                    alias_name = generic_names[0] if ".so" in source.name else generic_names[1]
            if source is None:
                UI.warn(f"vendor: linker library -l{link_name} not found in {libdir}")
                continue
            try:
                real = source.resolve()
            except Exception:
                real = source
            if not real.exists() or not real.is_file():
                continue
            if not llvm_library_matches(real.name, major) and real.name.startswith(("libLLVM", "libclang")):
                UI.warn(f"vendor: ignoring non-active LLVM library {real.name}")
                continue
            payload = vendor_lib / real.name
            copy_file_unique(real, payload)
            alias = vendor_lib / alias_name
            if alias.name != payload.name and not alias.exists() and not alias.is_symlink():
                relative_symlink(payload, alias)

    if not env_flag("MAKE_VENDOR_NO_CLANG_BUILTINS", False):
        for cand in (
            inc.parent / "lib" / "clang" / major / "include",
            libdir.parent / "lib" / "clang" / major / "include",
            libdir / "clang" / major / "include",
            Path("/usr/lib") / "clang" / major / "include",
        ):
            if cand.exists() and any(cand.iterdir()):
                copytree_replace(cand, vendor_include / "clang-builtins")
                break

    escaped_cflags = cflags.replace('"', '\"')
    portable_ldflags = re.sub(r"(?:^|\s)-L\S+", "", ldflags).strip()
    escaped_ldflags = portable_ldflags.replace('"', '\"')
    escaped_libs = libs.replace('"', '\"')
    vendor_bin.mkdir(parents=True, exist_ok=True)
    script = f'''#!/usr/bin/env sh
### Generated by ./make vendor
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/..
case "${{1:-}}" in
  --version) echo "{version}" ;;
  --includedir) echo "$here/include" ;;
  --libdir) echo "$here/lib" ;;
  --cflags) echo "{escaped_cflags}" | sed "s|-I[^ ]*|-I$here/include|g" ;;
  --ldflags) echo "-L$here/lib {escaped_ldflags}" ;;
  --libs) echo "{escaped_libs}" ;;
  --system-libs) echo "" ;;
  *) exit 1 ;;
esac
'''
    path = vendor_bin / "llvm-config"
    path.write_text(script, encoding="utf-8")
    chmod_executable(path)
    detail = ", ".join(f"-l{name}" for name in required) or "runtime closure only"
    UI.log("VENDOR", f"vendored llvm-config {version}: {detail}")


### Dependency sources

def vendor_source_ignore(_dir: str, names: list[str]) -> set[str]:
    ignored = {".git", ".hg", ".svn", "build", "cmake-build-debug", "cmake-build-release", "__pycache__", ".pytest_cache"}
    return {n for n in names if n in ignored or n.endswith((".o", ".a", ".so", ".dylib", ".dll", ".exe", ".pyc"))}


def local_dependency_source_dirs() -> list[Path]:
    candidates = [Path(p).expanduser() for p in env_list("MAKE_VENDOR_SOURCE_DIRS")]
    candidates += [ROOT / p for p in ("vendor/src", "deps", "third_party", "third-party", "external", "extern", "subprojects")]
    out: list[Path] = []
    seen: set[str] = set()
    for p in candidates:
        if not p.exists() or not p.is_dir():
            continue
        try:
            key = str(p.resolve())
        except Exception:
            key = str(p)
        if key not in seen:
            out.append(p)
            seen.add(key)
    return out


def local_dependency_archives() -> list[Path]:
    candidates = [Path(p).expanduser() for p in env_list("MAKE_VENDOR_SOURCE_ARCHIVES")]
    for folder in (ROOT / "vendor", ROOT / "deps", ROOT / "third_party"):
        if folder.exists():
            for pat in ("*.tar.gz", "*.tgz", "*.tar.xz", "*.tar.bz2", "*.zip"):
                candidates.extend(folder.glob(pat))
    return [p for p in candidates if p.exists() and p.is_file()]


def apt_source_names() -> list[str]:
    raw = os.environ.get("MAKE_VENDOR_APT_SOURCES", "").strip()
    if raw:
        return [x for x in re.split(r"[\s,]+", raw) if x]
    mapping = {"readline": "readline", "gmp": "gmp", "clang": "llvm-toolchain", "LLVM": "llvm-toolchain", "z": "zlib", "ffi": "libffi"}
    return sorted(dict.fromkeys(mapping[x] for x in detected_link_libraries() if x in mapping))


def try_download_apt_sources(dst: Path) -> list[str]:
    if not env_flag("MAKE_VENDOR_FETCH_SOURCES", False):
        return []
    if host_os() != "linux" or not which("apt-get"):
        UI.warn("MAKE_VENDOR_FETCH_SOURCES=1 but apt-get is unavailable")
        return []
    dst.mkdir(parents=True, exist_ok=True)
    fetched: list[str] = []
    for name in apt_source_names():
        UI.step(f"vendor: fetching source package {name}")
        res = run_capture(["apt-get", "source", "--download-only", name], cwd=dst)
        if res.returncode != 0:
            UI.warn(f"apt-get source failed for {name}")
            if res.stderr:
                sys.stderr.write(res.stderr)
            continue
        fetched.append(name)
    return fetched


def bundle_dependency_sources(vendor_dir: Path) -> dict[str, object]:
    source_root = vendor_dir / "src"
    local_root = source_root / "local"
    archive_root = source_root / "archives"
    system_root = source_root / "system"
    copied_dirs: list[str] = []
    copied_archives: list[str] = []
    for src in local_dependency_source_dirs():
        dst = local_root / src.name
        copytree_replace(src, dst, ignore=vendor_source_ignore)
        copied_dirs.append(rel(src))
    for archive in local_dependency_archives():
        archive_root.mkdir(parents=True, exist_ok=True)
        copy_file(archive, archive_root / archive.name)
        copied_archives.append(rel(archive))
    fetched = try_download_apt_sources(system_root)
    source_root.mkdir(parents=True, exist_ok=True)
    info = {
        "local_source_dirs": copied_dirs,
        "source_archives": copied_archives,
        "fetched_system_sources": fetched,
        "detected_dependencies": dependency_report(),
    }
    (source_root / "DEPENDENCIES.json").write_text(json.dumps(info, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if copied_dirs:
        UI.log("VENDOR", f"local dependency source trees: {', '.join(copied_dirs)}")
    if copied_archives:
        UI.log("VENDOR", f"dependency source archives: {', '.join(copied_archives)}")
    if fetched:
        UI.log("VENDOR", f"downloaded system dependency sources: {', '.join(fetched)}")
    if not copied_dirs and not copied_archives and not fetched:
        msg = "no dependency source trees found; set MAKE_VENDOR_SOURCE_DIRS or MAKE_VENDOR_FETCH_SOURCES=1"
        if env_flag("MAKE_VENDOR_REQUIRE_SOURCES", False):
            die("vendor: " + msg)
        if UI.verbose:
            UI.log("SOURCE", msg, "muted")
    return info


### Vendor

def write_runtime_launcher(root: Path, binary: Path, lib_dir: Path, *, name: str = "run") -> Path:
    """Write a launcher that avoids injecting a bundled libc into the host loader."""
    path = root / name
    bin_rel = os.path.relpath(binary, root).replace(os.sep, "/")
    lib_rel = os.path.relpath(lib_dir, root).replace(os.sep, "/")
    text = '#!/usr/bin/env sh\nset -eu\nhere=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\n'
    if host_os() == "linux":
        loaders = sorted(lib_dir.glob("ld-linux*.so*")) + sorted(lib_dir.glob("ld-musl*.so*"))
        if loaders:
            loader_rel = os.path.relpath(loaders[0], root).replace(os.sep, "/")
            text += f'exec "$here/{loader_rel}" --library-path "$here/{lib_rel}" "$here/{bin_rel}" "$@"\n'
        else:
            text += f'export LD_LIBRARY_PATH="$here/{lib_rel}${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}}"\n'
            text += f'exec "$here/{bin_rel}" "$@"\n'
    elif host_os() == "macos":
        text += f'export DYLD_LIBRARY_PATH="$here/{lib_rel}${{DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}}"\n'
        text += f'exec "$here/{bin_rel}" "$@"\n'
    else:
        text += f'exec "$here/{bin_rel}" "$@"\n'
    path.write_text(text, encoding="utf-8")
    chmod_executable(path)
    return path


def write_vendor_env(vendor_dir: Path, binary_name: str) -> None:
    lib_dir = vendor_dir / "lib"
    write_runtime_launcher(vendor_dir, vendor_dir / "bin" / binary_name, lib_dir, name="run")
    shim_dir = vendor_dir / "shim"
    shim_dir.mkdir(parents=True, exist_ok=True)
    shim = shim_dir / binary_name
    shim.write_text(
        '#!/usr/bin/env sh\n'
        'here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\n'
        'exec "$here/../run" "$@"\n',
        encoding="utf-8",
    )
    chmod_executable(shim)
    has_bundled_loader = bool(list(lib_dir.glob("ld-linux*.so*")) or list(lib_dir.glob("ld-musl*.so*")))
    lines = [
        "#!/usr/bin/env sh",
        "### Source this file from Bash or Zsh to activate the vendored build environment.",
        'if [ -n "${BASH_VERSION:-}" ] && [ -n "${BASH_SOURCE:-}" ]; then',
        '  _make_vendor_source=$BASH_SOURCE',
        'elif [ -n "${ZSH_VERSION:-}" ]; then',
        "  _make_vendor_source=$(eval 'printf \"%s\" \"${(%):-%N}\"')",
        'else',
        '  printf "%s\n" "env.sh: source this file from Bash or Zsh" >&2',
        '  return 2 2>/dev/null || exit 2',
        'fi',
        '_make_vendor_root=$(CDPATH= cd -- "$(dirname -- "$_make_vendor_source")" && pwd)',
        'export PATH="$_make_vendor_root/shim:$_make_vendor_root/bin${PATH:+:$PATH}"',
    ]
    if not has_bundled_loader:
        lines.extend([
            'export LD_LIBRARY_PATH="$_make_vendor_root/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"',
            'export DYLD_LIBRARY_PATH="$_make_vendor_root/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"',
        ])
    lines.extend([
        'export LIBRARY_PATH="$_make_vendor_root/lib${LIBRARY_PATH:+:$LIBRARY_PATH}"',
        'export CPATH="$_make_vendor_root/include${CPATH:+:$CPATH}"',
        'if [ -x "$_make_vendor_root/bin/llvm-config" ]; then export LLVM_CONFIG="$_make_vendor_root/bin/llvm-config"; fi',
        f'printf "%s\n" "{PROJECT.name} vendor environment ready"',
        'printf "  %-8s %s\n" "runner" "$_make_vendor_root/run"',
        'unset _make_vendor_source _make_vendor_root',
        "",
    ])
    path = vendor_dir / "env.sh"
    path.write_text("\n".join(lines), encoding="utf-8")
    chmod_executable(path)

def run_vendor(args: list[str], ctx: AppContext) -> int:
    jobs = ctx.jobs
    if args:
        binary = resolve_program(args[0])
        vendor_dir = Path(args[1]).expanduser() if len(args) > 1 else VENDOR_DIR
    else:
        binary = find_binary(build_if_missing=True, jobs=jobs)
        vendor_dir = VENDOR_DIR
    shutil.rmtree(vendor_dir, ignore_errors=True)
    bin_dir = vendor_dir / "bin"
    lib_dir = vendor_dir / "lib"
    UI.log("VENDOR", f"binary {rel(binary)}")
    binary_dst, runtime_libs = copy_runtime_flat(
        binary, bin_dir, lib_dir, include_system=not env_flag("MAKE_VENDOR_NO_SYSTEM_LIBS", False)
    )
    bundle_llvm(vendor_dir)
    source_info = bundle_dependency_sources(vendor_dir)
    write_vendor_env(vendor_dir, binary.name)
    rootfs_files: list[str] = []
    if env_flag("MAKE_VENDOR_ROOTFS", False):
        rootfs = vendor_dir / "rootfs"
        copied = copy_binary_rootfs(binary, rootfs, include_system=not env_flag("MAKE_VENDOR_NO_SYSTEM_LIBS", False))
        rootfs_files = [str(p.relative_to(vendor_dir)) for p in copied if p.exists()]
        UI.log("VENDOR", f"optional rootfs files: {len(copied)}")
    write_manifest(vendor_dir, {
        "binary": str(binary),
        "runtime_binary": str(binary_dst.relative_to(vendor_dir)),
        "runtime_libs": [str(p.relative_to(vendor_dir)) for p in runtime_libs],
        "rootfs_files": rootfs_files,
        "dependency_sources": source_info,
    })
    UI.log("VENDOR", f"runtime libs: {len(runtime_libs)} unique files")
    UI.artifact("vendor ready", rel(vendor_dir), f"run: {rel(vendor_dir / 'run')} --help")
    return 0


### Static

def run_static(args: list[str], ctx: AppContext) -> int:
    jobs = ctx.jobs
    cmd = args[0]
    if cmd == "bin":
        binary = find_binary(build_if_missing=True, jobs=jobs)
        shutil.rmtree(STATIC_DIR, ignore_errors=True)
        binary_dst, libs = copy_runtime_flat(
            binary,
            STATIC_DIR / "bin",
            STATIC_DIR / "lib",
            include_system=not env_flag("MAKE_STATIC_NO_SYSTEM_LIBS", False),
        )
        write_runtime_launcher(STATIC_DIR, binary_dst, STATIC_DIR / "lib", name="run")
        write_manifest(STATIC_DIR, {
            "binary": str(binary),
            "runtime_binary": str(binary_dst.relative_to(STATIC_DIR)),
            "runtime_libs": [str(p.relative_to(STATIC_DIR)) for p in libs],
        })
        UI.log("STATIC", f"binary {rel(binary)}")
        UI.log("STATIC", f"bundled {len(libs)} unique shared libs")
        UI.artifact("portable bundle ready", rel(STATIC_DIR), f"run: {rel(STATIC_DIR / 'run')}")
        return 0
    if cmd == "check":
        if len(args) < 2:
            die("usage: ./make static check <binary>")
        binary = Path(args[1]).expanduser()
        if not binary.is_absolute():
            binary = ROOT / binary
        status, detail = binary_linkage_status(binary)
        UI.section("Linkage")
        UI.row("binary", None, rel(binary))
        state = UI.status_word(True) if status == "static" else (UI.status_word(False) if status == "dynamic" else UI.dim("?"))
        UI.row("classification", state, status)
        if detail:
            UI.row("detail", None, detail)
        if status == "dynamic":
            deps, unresolved = dependency_closure_details(binary)
            if deps:
                print("")
                UI.section("Runtime dependencies")
                for dep in deps:
                    UI.note(str(dep))
            if unresolved:
                print("")
                UI.error("unresolved runtime dependencies")
                for name in unresolved:
                    UI.error_detail(name)
            return 1
        return 0 if status == "static" else 2
    die(f"unknown static command: {cmd}")


### Tar

def tar_source_ignore(_dir: str, names: list[str]) -> set[str]:
    ignored_names = {
        ".git", ".hg", ".svn", ".cache", ".pytest_cache", "__pycache__",
        "build", "dist", "tmp", "temp", "CMakeFiles", "CMakeCache.txt",
        "compile_commands.json", ".monadc-test-artifacts", ".last-failures",
        ".test-results.json", ".last-first-failure.org",
    }
    ignored_suffixes = (
        ".o", ".obj", ".a", ".so", ".dylib", ".dll", ".exe", ".pyc", ".pyo",
        ".gcda", ".gcno", ".profraw", ".profdata", ".mqti", ".ll", ".bc", ".tmp",
    )
    return {
        name for name in names
        if name in ignored_names or name.startswith(".#") or name.endswith("~") or name.endswith(ignored_suffixes)
    }


SOURCE_PACKAGE_REQUIRED = (
    "CMakeLists.txt",
    "make",
    "src/main.c",
    "src/runtime.c",
    "src/tooling/lsp.c",
    "src/tooling/repl.c",
    "src/testing/runner.py",
    "src/testing/core_runner.py",
    "core",
    "tests",
)


def validate_source_package_surface(root: Path = ROOT) -> None:
    missing = [item for item in SOURCE_PACKAGE_REQUIRED if not (root / item).exists()]
    if missing:
        raise CliError("source package surface is incomplete", details=("missing: " + ", ".join(missing),))
    test_files = [path for path in (root / "tests").rglob("*") if path.is_file()]
    non_monad = [rel(path) for path in test_files if path.suffix != ".mon"]
    if non_monad:
        preview = ", ".join(non_monad[:8])
        suffix = " ..." if len(non_monad) > 8 else ""
        raise CliError("tests/ must contain authored .mon files only", details=(preview + suffix,))


def source_package_entry(path: Path, *, with_context: bool) -> bool:
    """Whether a root entry belongs in the minimal complete source package."""
    essential_dirs = {"src", "core", "tests", "how_to", "examples", ".githooks", ".github"}
    if with_context:
        # Context verification references the research/glyph assets, so this is
        # one opt-in documentation payload rather than a subtly incomplete one.
        essential_dirs.update({"context", "glyph", "etc"})
    essential_files = {
        "CMakeLists.txt", "make", "README.md", ".gitignore",
        "LICENSE", "LICENSE.txt", "COPYING", "CHANGELOG.md",
    }
    return path.name in essential_dirs or path.name in essential_files


def copy_source_tree(package_dir: Path, *, with_context: bool) -> None:
    """Copy the complete build/verification surface without repository baggage."""
    for src in sorted(ROOT.iterdir(), key=lambda path: path.name):
        if not source_package_entry(src, with_context=with_context):
            continue
        dst = package_dir / src.name
        if src.is_dir():
            copytree_replace(src, dst, ignore=tar_source_ignore)
        elif src.is_file():
            copy_file(src, dst)
    validate_source_package_surface(package_dir)


def write_agent_build(package_dir: Path, *, with_binaries: bool, with_vendor: bool, with_context: bool) -> None:
    target = PROJECT.target or "target"
    included = [
        "src/ — compiler, runtime, embedding, tooling, and host verification infrastructure",
        "core/ — shipped Monad core library",
        "tests/ — authored .mon verification corpus only",
        "how_to/ and examples/ — executable language examples",
        "CMakeLists.txt and ./make — canonical build frontends",
        ".githooks/ and .github/ — clean-tree enforcement and CI verification contract",
        "SOURCE_MANIFEST.json — package hashes and build metadata",
    ]
    if with_vendor:
        included.append("build/vendor/ — optional relocatable dependency/toolchain bundle")
    if with_binaries:
        included.append("build/static/ — optional portable binary folder")
    if with_context:
        included.append("context/, glyph/, etc/ — optional design/research context payload")

    lines = [
        f"# Agent build notes for {PROJECT.name}",
        "",
        "This archive was produced by `./make tar` from the canonical source layout.",
        "",
        "## Included",
        "",
        *(f"- {item}" for item in included),
        "",
        "## Build and verify",
        "",
        "```sh",
    ]
    if with_vendor:
        lines.append(". build/vendor/env.sh")
    lines.extend([
        "./make doctor",
        "./make all",
        "./make test",
        "```",
        "",
        "`./make all` refreshes `compile_commands.json` for clangd automatically.",
        "Run `./make compdb` explicitly when you only want to refresh editor metadata.",
        "",
        "## Clean-tree invariant",
        "",
        "```sh",
        "./make clean --check",
        "```",
        "",
        "Generated compiler/test/build state belongs under `build/` and is never source.",
    ])
    if with_vendor:
        lines.extend([
            "",
            "## Vendored runner",
            "",
            "```sh",
            "build/vendor/run --help 2>/dev/null || build/vendor/run",
            "```",
            "",
            "Optional chroot view:",
            "",
            "```sh",
            "MAKE_VENDOR_ROOTFS=1 ./make vendor",
            f"sudo chroot build/vendor/rootfs /bin/{target}",
            "```",
        ])
    if with_binaries:
        lines.extend([
            "",
            "## Portable binary folder",
            "",
            "```sh",
            "build/static/run --help 2>/dev/null || build/static/run",
            "```",
        ])
    (package_dir / "AGENT_BUILD.md").write_text("\n".join(lines).rstrip() + "\n", encoding="utf-8")


def run_tar(args: list[str], ctx: AppContext) -> int:
    with_binaries = "--with-binaries" in args or env_flag("MAKE_TAR_WITH_BINARIES", False)
    with_vendor = "--with-vendor" in args or env_flag("MAKE_TAR_WITH_VENDOR", False)
    with_context = "--with-context" in args or env_flag("MAKE_TAR_WITH_CONTEXT", False)
    known = {"--with-binaries", "--with-vendor", "--with-context"}
    unknown = [arg for arg in args if arg not in known]
    if unknown:
        die("tar: unknown option(s): " + " ".join(unknown))

    validate_source_package_surface()
    if missing_referenced_sources():
        report_source_integrity(fail=True)

    DIST_DIR.mkdir(parents=True, exist_ok=True)
    package_name = f"{PROJECT.name}-static" if with_binaries else f"{PROJECT.name}-source"
    package_dir = DIST_DIR / package_name
    shutil.rmtree(package_dir, ignore_errors=True)
    package_dir.mkdir(parents=True, exist_ok=True)
    UI.log("TAR", f"package {package_name}", "accent")

    copy_source_tree(package_dir, with_context=with_context)

    if with_vendor:
        if not (VENDOR_DIR / "env.sh").exists():
            UI.step("tar: building optional vendor bundle")
            run_vendor([], ctx)
        UI.log("VENDOR", f"including {rel(VENDOR_DIR)}")
        copytree_replace(VENDOR_DIR, package_dir / "build" / "vendor")
    if with_binaries:
        UI.log("STATIC", "building portable binary folder")
        run_static(["bin"], ctx)
        copytree_replace(STATIC_DIR, package_dir / "build" / "static")

    write_agent_build(
        package_dir,
        with_binaries=with_binaries,
        with_vendor=with_vendor,
        with_context=with_context,
    )
    write_manifest(package_dir, {
        "with_binaries": with_binaries,
        "with_vendor": with_vendor,
        "with_context": with_context,
        "dependency_report": dependency_report(),
    }, filename="SOURCE_MANIFEST.json")
    archive = make_tar_gz(package_dir, DIST_DIR, package_name)
    detail = format_bytes(archive.stat().st_size) if archive.exists() else "created"
    UI.artifact("archive ready", rel(archive), detail)
    if not with_vendor:
        UI.hint("vendor toolchains are excluded by default; add --with-vendor only when portability requires them")
    return 0


### Developer workflow

def total_memory_bytes() -> int:
    try:
        if host_os() == "linux":
            for line in Path("/proc/meminfo").read_text(encoding="utf-8", errors="ignore").splitlines():
                if line.startswith("MemTotal:"):
                    return int(line.split()[1]) * 1024
        if host_os() == "macos":
            res = run_capture(["sysctl", "-n", "hw.memsize"], timeout=3)
            if res.returncode == 0 and res.stdout.strip().isdigit():
                return int(res.stdout.strip())
        if host_os() == "windows":
            import ctypes

            class MEMORYSTATUSEX(ctypes.Structure):
                _fields_ = [
                    ("dwLength", ctypes.c_ulong),
                    ("dwMemoryLoad", ctypes.c_ulong),
                    ("ullTotalPhys", ctypes.c_ulonglong),
                    ("ullAvailPhys", ctypes.c_ulonglong),
                    ("ullTotalPageFile", ctypes.c_ulonglong),
                    ("ullAvailPageFile", ctypes.c_ulonglong),
                    ("ullTotalVirtual", ctypes.c_ulonglong),
                    ("ullAvailVirtual", ctypes.c_ulonglong),
                    ("sullAvailExtendedVirtual", ctypes.c_ulonglong),
                ]

            status = MEMORYSTATUSEX()
            status.dwLength = ctypes.sizeof(MEMORYSTATUSEX)
            if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
                return int(status.ullTotalPhys)
    except Exception:
        pass
    return 0


def format_bytes(value: int) -> str:
    if value <= 0:
        return "unknown"
    units = ("B", "KiB", "MiB", "GiB", "TiB")
    n = float(value)
    for unit in units:
        if n < 1024 or unit == units[-1]:
            return f"{n:.1f} {unit}" if unit not in ("B", "KiB") else f"{int(n)} {unit}"
        n /= 1024
    return str(value)


def resolve_jobs(requested: int) -> tuple[int, str]:
    if requested > 0:
        return requested, "explicit"
    raw = os.environ.get("MAKE_JOBS", "").strip()
    if raw.isdigit() and int(raw) > 0:
        return int(raw), "MAKE_JOBS"
    cpu = max(1, os.cpu_count() or 1)
    jobs = max(1, int(cpu * 0.75))
    if cpu >= 2:
        jobs = max(2, jobs)
    memory = total_memory_bytes()
    per_job_mb = int(os.environ.get("MAKE_JOB_MEMORY_MB", "1536") or "1536")
    if memory > 0 and per_job_mb > 0:
        memory_jobs = max(1, memory // (per_job_mb * 1024 * 1024))
        jobs = min(jobs, int(memory_jobs))
    return max(1, jobs), f"auto from {cpu} CPUs/{format_bytes(memory)} RAM"


def env_words(name: str) -> list[str]:
    raw = os.environ.get(name, "").strip()
    return [x for x in re.split(r"[\s,;]+", raw) if x] if raw else []


SOURCE_REFERENCE_SUFFIXES = ("c", "cc", "cpp", "cxx", "h", "hh", "hpp")


def referenced_source_files() -> list[str]:
    """Collect literal native-source references from the declared build files."""
    pattern = re.compile(
        r"(?<![A-Za-z0-9_])([A-Za-z0-9_./+-]+\.(?:"
        + "|".join(SOURCE_REFERENCE_SUFFIXES)
        + r"))(?![A-Za-z0-9_])"
    )
    refs: set[str] = set()
    for name in ("Makefile", "makefile", "GNUmakefile", "CMakeLists.txt"):
        path = ROOT / name
        if not path.is_file():
            continue
        raw = path.read_text(encoding="utf-8", errors="replace")
        # Build-file comments are prose, not dependency declarations.  Strip
        # them before scanning so documentation such as ``features.h`` does
        # not become a false source-integrity requirement.
        build_text = "\n".join(line.split("#", 1)[0] for line in raw.splitlines())
        for match in pattern.finditer(build_text):
            value = match.group(1)
            if value.startswith("/") or "$" in value or value.startswith("<"):
                continue
            refs.add(value)
    return sorted(refs)


def missing_referenced_sources() -> list[str]:
    return [name for name in referenced_source_files() if not (ROOT / name).exists()]


def report_source_integrity(*, fail: bool = False) -> int:
    refs = referenced_source_files()
    missing = missing_referenced_sources()
    UI.section("Source integrity")
    UI.row("referenced native files", None, str(len(refs)))
    UI.row("missing", None, str(len(missing)), good=not missing)
    if missing:
        sys.stdout.flush()
        visible = missing if UI.verbose else missing[:12]
        for name in visible:
            UI.error_detail(name)
        if len(missing) > len(visible):
            UI.hint(f"{len(missing) - len(visible)} more missing source paths; use --verbose for the complete list")
        if fail:
            raise CliError(
                f"source package is incomplete: {len(missing)} build-referenced native source files are missing",
                hint="recreate the package from a complete checkout; source archives copy the full repository minus generated state",
            )
    return len(missing)


def source_hygiene_files() -> list[Path]:
    default_exts = {
        ".c", ".h", ".cc", ".cpp", ".cxx", ".hh", ".hpp", ".m", ".mm",
        ".py", ".sh", ".rs", ".go", ".java", ".kt", ".mon", ".ny", ".cmake",
    }
    configured = env_words("MAKE_HYGIENE_EXTENSIONS")
    exts = {x if x.startswith(".") else "." + x for x in configured} if configured else default_exts
    out: list[Path] = []
    for name in PROJECT.source_dirs:
        root = ROOT / name
        if not root.exists():
            continue
        for p in root.rglob("*"):
            if p.is_file() and (p.suffix.lower() in exts or p.name in {"Makefile", "makefile", "GNUmakefile"}):
                out.append(p)
    for name in PROJECT.source_files:
        p = ROOT / name
        if p.exists() and p.is_file() and p not in out:
            out.append(p)
    return out


def check_source_hygiene() -> int:
    """Fail on corrupt text inputs or unresolved merge-conflict markers."""
    bad: list[str] = []
    conflict = re.compile(r"^(?:<<<<<<< |>>>>>>> )")
    max_bytes = int(os.environ.get("MAKE_HYGIENE_MAX_FILE_MB", "16") or "16") * 1024 * 1024
    files = source_hygiene_files()
    for p in files:
        try:
            if p.stat().st_size > max_bytes:
                continue
            data = p.read_bytes()
        except OSError as exc:
            bad.append(f"{rel(p)}: unreadable: {exc}")
            continue
        if b"\x00" in data:
            bad.append(f"{rel(p)}: contains NUL byte(s)")
            continue
        text = data.decode("utf-8", errors="replace")
        for line_no, line in enumerate(text.splitlines(), 1):
            if conflict.match(line):
                bad.append(f"{rel(p)}:{line_no}: unresolved merge-conflict marker")
                break
        if len(bad) >= 50:
            break
    if bad:
        UI.error("source hygiene failed")
        for item in bad:
            UI.error_detail(item)
        return 1
    UI.ok(f"source hygiene ({len(files)} files)")
    return 0


GENERATED_DIR_NAMES = frozenset({
    "__pycache__", ".pytest_cache", ".monadc-test-artifacts", "CMakeFiles",
})
GENERATED_SUFFIXES = frozenset({
    ".mqti", ".ll", ".bc", ".o", ".obj", ".pyc", ".pyo",
    ".gcda", ".gcno", ".profraw", ".profdata", ".tmp",
})
GENERATED_EXACT_PATHS = frozenset({
    "MANIFEST.json",
    "compile_commands.json",
    "CMakeCache.txt",
    "tests/.test-results.json",
    "tests/.last-first-failure.org",
    "tests/.fuzz-results.json",
    "tests/.fuzz-history.json",
})
GENERATED_TREE_PATHS = frozenset({
    "build",
    "dist",
    ".dryc",
    "tests/.last-failures",
})
PRESERVED_SOURCE_ARTIFACTS = frozenset()


def generated_root_outputs() -> set[Path]:
    names = {PROJECT.target, PROJECT.runtime_lib}
    for var in (
        "TARGET", "TARGET_BASE", "RUNTIME_LIB", "EMBED_STATIC_LIB", "EMBED_SHARED_LIB",
        "COMPILER_STATIC_LIB", "COMPILER_SHARED_LIB",
    ):
        value = parse_make_var(var).strip()
        if value and "$" not in value and "/" not in value and "\\" not in value:
            names.add(value)
    out: set[Path] = set()
    for name in names:
        if not name:
            continue
        path = ROOT / name
        if path.exists() and path.is_file():
            out.add(path)
        exe = ROOT / f"{name}.exe"
        if exe.exists() and exe.is_file():
            out.add(exe)
    return out


def generated_artifacts() -> list[Path]:
    """Return generated repository state covered by the canonical clean policy."""
    found: set[Path] = set(generated_root_outputs())
    for relpath in GENERATED_TREE_PATHS | GENERATED_EXACT_PATHS:
        path = ROOT / relpath
        if path.exists() or path.is_symlink():
            found.add(path)

    for current, dirs, files in os.walk(ROOT, topdown=True):
        base = Path(current)
        rel_base = base.relative_to(ROOT)
        if rel_base.parts and rel_base.parts[0] == ".git":
            dirs[:] = []
            continue
        # Whole generated trees are already represented once; do not enumerate them.
        pruned: list[str] = []
        for name in list(dirs):
            path = base / name
            rel = path.relative_to(ROOT).as_posix()
            if rel in GENERATED_TREE_PATHS or name in GENERATED_DIR_NAMES:
                found.add(path)
            else:
                pruned.append(name)
        dirs[:] = pruned

        for name in files:
            path = base / name
            rel = path.relative_to(ROOT).as_posix()
            if rel in PRESERVED_SOURCE_ARTIFACTS:
                continue
            if name.startswith(".#") or name.endswith("~") or name == ".DS_Store":
                found.add(path)
                continue
            if path.suffix.lower() in GENERATED_SUFFIXES:
                found.add(path)
                continue
            # Authored test inputs are exclusively .mon files; executable
            # extensionless files under tests/ are compiler-produced fixtures.
            if rel.startswith(("tests/", "how_to/")) and path.suffix == "" and os.access(path, os.X_OK):
                found.add(path)
                continue
            # Core JSON is compiler interface/cache output. JSON elsewhere may be source/golden data.
            if path.suffix.lower() == ".json" and rel.startswith("core/"):
                found.add(path)
    return sorted(found, key=lambda path: (len(path.parts), path.as_posix()))


def artifact_size(path: Path) -> int:
    try:
        if path.is_symlink() or path.is_file():
            return path.lstat().st_size
        if path.is_dir():
            return sum(artifact_size(child) for child in path.iterdir())
    except OSError:
        return 0
    return 0


def remove_artifact(path: Path) -> None:
    if path.is_symlink() or path.is_file():
        path.unlink(missing_ok=True)
    elif path.is_dir():
        shutil.rmtree(path, ignore_errors=False)


def command_clean(args: list[str], _ctx: AppContext) -> int:
    """Deep-clean the repository or enforce cleanliness without mutating it."""
    check_only = "--check" in args
    unknown = [arg for arg in args if arg != "--check"]
    if unknown:
        die("clean: unknown option(s): " + " ".join(unknown))

    artifacts = generated_artifacts()
    if check_only:
        if not artifacts:
            UI.ok("repository is clean")
            return 0
        total = sum(artifact_size(path) for path in artifacts)
        UI.error(f"generated repository state detected ({len(artifacts)} paths · {format_bytes(total)})")
        visible = artifacts if UI.verbose else artifacts[:10]
        for path in visible:
            UI.error_detail(path.relative_to(ROOT).as_posix())
        if len(artifacts) > len(visible):
            UI.hint(f"{len(artifacts) - len(visible)} more paths; use --verbose to inspect all", stream=sys.stderr)
        UI.hint("run ./make clean, then stage the deletion before committing", stream=sys.stderr)
        return 1

    if not artifacts:
        UI.ok("repository already clean")
        return 0
    total = sum(artifact_size(path) for path in artifacts)
    removed = 0
    for path in sorted(artifacts, key=lambda item: len(item.parts), reverse=True):
        if not (path.exists() or path.is_symlink()):
            continue
        if UI.verbose:
            UI.log("CLEAN", rel(path), "accent_soft")
        remove_artifact(path)
        removed += 1
    UI.ok(f"deep clean removed {removed} paths · reclaimed {format_bytes(total)}")
    return 0


def append_flags(env: dict[str, str], name: str, flags: str) -> None:
    env[name] = ((env.get(name, "") + " " + flags).strip())


def command_sanitizer(kind: str, args: list[str], ctx: AppContext) -> int:
    build_only = "--build-only" in args
    forwarded = [a for a in args if a != "--build-only"]
    if forwarded:
        die(f"{kind}: unknown option(s): " + " ".join(forwarded))
    started = time.perf_counter()
    build_project(kind, jobs=ctx.jobs)
    if not build_only:
        command_test(["--no-build"], ctx)
    UI.ok(f"{kind} completed in {format_elapsed(time.perf_counter() - started)}")
    return 0


def command_test(args: list[str], ctx: AppContext) -> int:
    """Run the project's canonical test frontend when one is present.

    Monad keeps authored fixtures in tests/ and the host runner in src/testing;
    the Python runner is the canonical test implementation for this project.
    """
    jobs = ctx.jobs
    no_build = "--no-build" in args
    forwarded = [arg for arg in args if arg != "--no-build"]
    runner = ROOT / "src" / "testing" / "runner.py"
    started = time.perf_counter()

    if runner.is_file():
        metadata_only = any(
            arg in {"--list", "--list-targets", "--list-tiers",
                    "--validate-context-links", "--validate-metadata"}
            for arg in forwarded
        )
        if not no_build and not metadata_only:
            build_project("debug", jobs=jobs)
        env = os.environ.copy()
        if not metadata_only:
            binary = find_binary(build_if_missing=not no_build, jobs=jobs)
            env["MONAD_BINARY"] = str(binary)
        proc = subprocess.run(
            [sys.executable, "-B", "-m", "src.testing.runner", *forwarded],
            cwd=str(ROOT), env=env, check=False,
        )
        if proc.returncode == 0:
            UI.ok(f"tests completed in {format_elapsed(time.perf_counter() - started)}")
        return proc.returncode

    die("canonical src/testing runner is missing")


def run_python_module(module: str, args: list[str] | None = None, *, env: dict[str, str] | None = None) -> int:
    command = [sys.executable, "-B", "-m", module, *(args or [])]
    proc = subprocess.run(command, cwd=str(ROOT), env={**os.environ, **(env or {})}, check=False)
    return proc.returncode


def command_core_tests(args: list[str], ctx: AppContext) -> int:
    build_project("debug", jobs=ctx.jobs)
    env = {"MONAD_BINARY": str(BUILD_ROOT / "bin" / ("monad.exe" if host_os() == "windows" else "monad"))}
    return run_python_module("src.testing.core_runner", args, env=env)


def command_embedding_tests(args: list[str], ctx: AppContext) -> int:
    if args:
        die("test-embedding: no arguments are supported")
    binary = build_project("debug", jobs=ctx.jobs)
    env = {"MONAD_BINARY": str(binary)}
    return run_python_module("unittest", ["discover", "-s", "src/testing/contracts", "-p", "test_embedding*.py"], env=env)


def command_repl_tests(args: list[str], ctx: AppContext) -> int:
    if args:
        die("repl: no arguments are supported")
    binary = build_project("debug", jobs=ctx.jobs)
    env = {"MONAD_BINARY": str(binary)}
    return run_python_module(
        "unittest",
        ["src.testing.contracts.test_repl", "src.testing.contracts.test_repl_pty", "src.testing.contracts.test_repl_cache"],
        env=env,
    )


def command_bytecode_tests(args: list[str], ctx: AppContext) -> int:
    if args:
        die("bytecode: no arguments are supported")
    binary = build_project("debug", jobs=ctx.jobs)
    return run_python_module("src.testing.contracts.test_bytecode", env={"MONAD_BINARY": str(binary), "BYTECODE_VISUAL": "1"})


def command_runner_contract(args: list[str], ctx: AppContext) -> int:
    if args:
        die("test-runner: no arguments are supported")
    binary = build_project("debug", jobs=ctx.jobs)
    proc = subprocess.run([str(binary), "test", "runner"], cwd=str(ROOT), env={**os.environ, "MONAD_BINARY": str(binary)}, check=False)
    return proc.returncode


def command_how_to_tests(args: list[str], ctx: AppContext) -> int:
    if args:
        die("test-how-to: no arguments are supported")
    binary = build_project("debug", jobs=ctx.jobs)
    return run_python_module("src.testing.contracts.test_how_to_examples", env={"MONAD_BINARY": str(binary)})


def command_context_tests(module: str) -> CommandFunc:
    def handler(args: list[str], _ctx: AppContext) -> int:
        return run_python_module(module, args)
    return handler


def command_verify_context(args: list[str], _ctx: AppContext) -> int:
    if args:
        die("verify-context: no arguments are supported")
    for module, module_args in (
        ("src.testing.contracts.test_context_lint", ["--verbose"]),
        ("src.testing.contracts.test_context_refs", []),
        ("src.testing.contracts.test_context_visualizer", []),
        ("src.testing.contracts.test_context_graph", []),
    ):
        result = run_python_module(module, module_args)
        if result != 0:
            return result
    return subprocess.run(
        [sys.executable, "-B", "src/tooling/context/context_lint.py", "--skip-info",
         "--check-src-refs", "--check-test-contexts", "--check-record-refs"],
        cwd=str(ROOT), check=False,
    ).returncode


def command_verify_context_strict(args: list[str], _ctx: AppContext) -> int:
    if args:
        die("verify-context-strict: no arguments are supported")
    return subprocess.run(
        [sys.executable, "-B", "src/tooling/context/context_lint.py", "--skip-info", "--all",
         "--check-src-refs", "--check-test-contexts", "--check-orphaned", "--check-empty-headings",
         "--check-description"], cwd=str(ROOT), check=False,
    ).returncode


def command_fuzzing(args: list[str], ctx: AppContext) -> int:
    binary = build_project("debug", jobs=ctx.jobs)
    return run_python_module("src.testing.contracts.fuzzing.fuzz_codegen", args, env={"MONAD_BINARY": str(binary)})


def command_generate_asm(which: str) -> CommandFunc:
    def handler(args: list[str], _ctx: AppContext) -> int:
        if args:
            die(f"{which}: no arguments are supported")
        script = "create_asm_tests.py" if which == "generate-asm-tests" else "create_asm_tests_extra.py"
        return subprocess.run([sys.executable, "-B", f"src/tooling/generators/{script}"], cwd=str(ROOT), check=False).returncode
    return handler


def command_verify_push(args: list[str], ctx: AppContext) -> int:
    if args:
        die("verify-push: no arguments are supported")
    if command_clean(["--check"], ctx) != 0:
        return 1
    return command_check([], ctx)


def command_check(args: list[str], ctx: AppContext) -> int:
    jobs = ctx.jobs
    no_build = "--no-build" in args
    no_tests = "--no-tests" in args or "--quick" in args
    unknown = [a for a in args if a not in {"--no-build", "--no-tests", "--quick"}]
    if unknown:
        die("check: unknown option(s): " + " ".join(unknown))
    started = time.perf_counter()
    if check_source_hygiene() != 0:
        return 1

    explicit = env_words("MAKE_CHECK_TARGETS")
    if explicit:
        for target in explicit:
            spec = COMMANDS.get(target)
            if spec is None:
                die(f"MAKE_CHECK_TARGETS names unknown Python command: {target}")
            result = execute_command(spec, ["--no-build"] if target == "test" else [], ctx)
            if result != 0:
                return result
        UI.ok(f"check completed in {format_elapsed(time.perf_counter() - started)}")
        return 0

    if not no_build:
        build_project("debug", jobs=jobs)
    if not no_tests:
        result = command_test(["--no-build"], ctx)
        if result != 0:
            return result
    UI.ok(f"check completed in {format_elapsed(time.perf_counter() - started)}")
    return 0


def read_os_release() -> dict[str, str]:
    p = Path("/etc/os-release")
    out: dict[str, str] = {}
    try:
        for line in p.read_text(encoding="utf-8", errors="ignore").splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                out[k.strip()] = v.strip().strip('"')
    except OSError:
        pass
    return out


def project_needs_llvm() -> bool:
    hay = makefile_text().lower()
    libs = {x.lower() for x in detected_link_libraries()}
    pkgs = {x.lower() for x in detected_pkg_config_packages()}
    names = libs | pkgs
    return any("llvm" in x or "clang" in x for x in names) or "llvm-config" in hay


def pkg_config_exists(name: str) -> bool:
    tool = which("pkg-config") or which("pkgconf")
    return bool(tool and run_capture([tool, "--exists", name], timeout=5).returncode == 0)


def required_dependency_state() -> tuple[list[str], list[str]]:
    missing_tools: list[str] = []
    if not (os.environ.get("CC") or which("cc") or which("clang") or which("gcc")):
        missing_tools.append("cc")
    text = makefile_text().lower()
    if "pkg-config" in text and not (which("pkg-config") or which("pkgconf")):
        missing_tools.append("pkg-config")
    if "cmake" in text and not which("cmake"):
        missing_tools.append("cmake")
    if "ninja" in text and not which("ninja"):
        missing_tools.append("ninja")
    if project_needs_llvm() and not llvm_config_path():
        missing_tools.append("llvm")
    missing_pkgs = [
        pkg for pkg in detected_pkg_config_packages()
        if not pkg_config_exists(pkg) and not (pkg == "libclang" and llvm_config_path())
    ]
    return list(dict.fromkeys(missing_tools)), missing_pkgs


def install_dependencies(missing: list[str]) -> None:
    if not missing:
        return
    os_name = host_os()
    if os_name == "linux":
        info = read_os_release()
        distro = info.get("ID", "").lower()
        like = info.get("ID_LIKE", "").lower()
        prefix = [] if hasattr(os, "geteuid") and os.geteuid() == 0 else (["sudo"] if which("sudo") else [])
        if distro in {"debian", "ubuntu", "linuxmint", "pop", "raspbian"} or "debian" in like:
            mapping = {
                "make": "build-essential",
                "cc": "build-essential",
                "pkg-config": "pkg-config",
                "cmake": "cmake",
                "ninja": "ninja-build",
                "llvm": "llvm-dev clang",
            }
            pkgs = sorted({p for item in missing for p in mapping.get(item, item).split()})
            run([*prefix, "apt-get", "update"])
            run([*prefix, "apt-get", "install", "-y", *pkgs])
            return
        if distro in {"arch", "manjaro"} or "arch" in like:
            mapping = {
                "make": "base-devel",
                "cc": "base-devel",
                "pkg-config": "pkgconf",
                "cmake": "cmake",
                "ninja": "ninja",
                "llvm": "llvm clang",
            }
            pkgs = sorted({p for item in missing for p in mapping.get(item, item).split()})
            run([*prefix, "pacman", "-S", "--needed", "--noconfirm", *pkgs])
            return
        if distro in {"fedora", "rhel", "centos", "rocky"} or "fedora" in like or "rhel" in like:
            mapping = {
                "make": "make",
                "cc": "gcc",
                "pkg-config": "pkgconf-pkg-config",
                "cmake": "cmake",
                "ninja": "ninja-build",
                "llvm": "llvm-devel clang",
            }
            pkgs = sorted({p for item in missing for p in mapping.get(item, item).split()})
            run([*prefix, "dnf", "install", "-y", *pkgs])
            return
    if os_name == "macos":
        if ("make" in missing or "cc" in missing) and not which("cc"):
            die("install Apple command-line tools first: xcode-select --install")
        if not which("brew"):
            die("Homebrew is required for automatic dependency installation on macOS")
        mapping = {"pkg-config": "pkg-config", "cmake": "cmake", "ninja": "ninja", "llvm": "llvm"}
        pkgs = sorted({mapping[item] for item in missing if item in mapping})
        if pkgs:
            run(["brew", "install", *pkgs])
        return
    die("automatic dependency installation is not implemented for this host; use ./make deps for the missing list")


def command_deps(args: list[str], _ctx: AppContext) -> int:
    install = "--install" in args
    unknown = [a for a in args if a != "--install"]
    if unknown:
        die("deps: unknown option(s): " + " ".join(unknown))
    missing, missing_pkgs = required_dependency_state()
    if install and missing:
        install_dependencies(missing)
        missing, missing_pkgs = required_dependency_state()
    if missing:
        UI.error("missing build tools: " + ", ".join(missing))
    if missing_pkgs:
        UI.error("missing pkg-config packages: " + ", ".join(missing_pkgs))
    if missing or missing_pkgs:
        if not install:
            UI.hint("run ./make deps --install for supported host tools; project libraries may still need manual packages")
        return 1
    UI.ok("build dependencies available")
    return 0


def command_hooks(_args: list[str], _ctx: AppContext) -> int:
    if not which("git"):
        die("hooks: git was not found")
    hook_dir = ROOT / ".githooks"
    if not hook_dir.is_dir():
        die("hooks: .githooks directory was not found")
    hooks = [p for p in sorted(hook_dir.iterdir()) if p.is_file() and not p.name.startswith(".")]
    if not hooks:
        die("hooks: .githooks contains no hook files")
    if host_os() != "windows":
        for hook in hooks:
            chmod_executable(hook)
    run(["git", "config", "--local", "core.hooksPath", ".githooks"])
    UI.ok(f"Git hooks installed ({len(hooks)} files)")
    return 0


def command_run(args: list[str], ctx: AppContext) -> int:
    jobs = ctx.jobs
    literal_args = bool(args and args[0] == "--")
    if literal_args:
        args = args[1:]
    binary = find_binary(build_if_missing=True, jobs=jobs)
    return subprocess.run([str(binary), *args], cwd=str(ROOT), env=os.environ.copy()).returncode


### Doctor and env

def run_doctor(_args: list[str], ctx: AppContext) -> int:
    dep = dependency_report()
    missing_tools, missing_pkgs = required_dependency_state()
    jobs, jobs_source = ctx.jobs, ctx.jobs_source
    UI.section("Layout")
    UI.row("root", None, str(ROOT), good=True)
    UI.row("Python frontend", None, rel(ROOT / "make"), good=(ROOT / "make").is_file())
    build_state = UI.status_word(True) if BUILD_ROOT.exists() else UI.dim(UI.glyph("·", "-"))
    UI.row("build dir", build_state, rel(BUILD_ROOT))
    bad = not (ROOT / "make").is_file()
    print("")
    missing_sources = report_source_integrity()
    bad = bad or bool(missing_sources)
    print("")
    UI.section("Host")
    UI.row("os", None, host_os())
    UI.row("arch", None, host_arch())
    UI.row("memory", None, format_bytes(total_memory_bytes()))
    UI.row("jobs", None, f"{jobs} ({jobs_source})")
    print("")
    UI.section("Required tools")
    cc = os.environ.get("CC") or which("cc") or which("clang") or which("gcc")
    checks = [
        ("python3", sys.executable, True),
        ("cc", cc or "-", bool(cc)),
    ]
    if project_needs_llvm():
        checks.append(("llvm-config", llvm_config_path() or "-", bool(llvm_config_path())))
    for name, value, good in checks:
        UI.row(name, None, value, good=good)
        bad = bad or not good
    print("")
    UI.section("Common optional tools")
    for name in ("git", "tar", "gzip", "ldd", "readelf", "otool", "pkg-config", "cmake", "ninja", "bear"):
        if name == "otool" and host_os() != "macos":
            continue
        value = which(name)
        state = UI.status_word(True) if value else UI.dim(UI.glyph("·", "-"))
        UI.row(name, state, value or "not installed")
    print("")
    UI.section("Detected project")
    UI.row("name", None, PROJECT.name)
    UI.row("target", None, PROJECT.target or "-")
    UI.row("build target", None, PROJECT.build_target or "-")
    UI.row("release target", None, PROJECT.release_target or "-")
    UI.row("runtime lib", None, PROJECT.runtime_lib or "-")
    UI.row("link libs", None, ", ".join(dep["link_libraries"]) or "-")
    UI.row("pkg-config", None, ", ".join(dep["pkg_config_packages"]) or "-")
    UI.row("vendor", None, rel(VENDOR_DIR))
    UI.row("static", None, rel(STATIC_DIR))
    UI.row("compdb", None, rel(COMPDB_LINK) if COMPDB_LINK.exists() or COMPDB_LINK.is_symlink() else "not generated")
    if LOADED_CONFIGS:
        UI.row("config", None, "; ".join(rel(p) for p in LOADED_CONFIGS))
    binary = next((p for p in possible_binary_paths() if p.exists() and p.is_file()), None)
    if binary and host_os() == "linux":
        _, unresolved = dependency_closure_details(binary)
        UI.row("runtime closure", None, "complete" if not unresolved else "missing: " + ", ".join(unresolved), good=not unresolved)
        bad = bad or bool(unresolved)
    print("")
    if missing_pkgs:
        UI.error("missing pkg-config packages: " + ", ".join(missing_pkgs))
        bad = True
    if missing_tools:
        UI.error("missing tools: " + ", ".join(missing_tools))
        bad = True
    if bad:
        UI.error("doctor found missing required pieces")
        return 1
    UI.ok("doctor passed")
    return 0


def run_env(args: list[str], ctx: AppContext) -> int:
    if any(a != "--json" for a in args):
        die("env: only --json is supported")
    jobs, jobs_source = ctx.jobs, ctx.jobs_source
    data = {
        "ROOT": str(ROOT),
        "BUILD_ROOT": str(BUILD_ROOT),
        "DIST_DIR": str(DIST_DIR),
        "VENDOR_DIR": str(VENDOR_DIR),
        "STATIC_DIR": str(STATIC_DIR),
        "PROJECT_NAME": PROJECT.name,
        "PROJECT_TARGET": PROJECT.target,
        "RUNTIME_LIB": PROJECT.runtime_lib,
        "HOST_OS": host_os(),
        "HOST_ARCH": host_arch(),
        "HOST_MEMORY_BYTES": total_memory_bytes(),
        "JOBS": jobs,
        "JOBS_SOURCE": jobs_source,
        "DEPENDENCIES": dependency_report(),
    }
    if args and args[0] == "--json":
        print(json.dumps(data, indent=2, sort_keys=True))
    else:
        for k, v in data.items():
            raw = json.dumps(v, sort_keys=True, separators=(",", ":")) if isinstance(v, (dict, list)) else str(v)
            print(f"{k}={shlex.quote(raw)}")
    return 0


### Command model

CommandFunc = Callable[[list[str], AppContext], int]


@dataclass(frozen=True)
class CommandSpec:
    name: str
    group: str
    usage: str
    summary: str
    handler: CommandFunc
    options: tuple[tuple[str, str], ...] = ()
    present: bool = True
    help_on_empty: bool = False
    no_args: bool = False


GROUP_ORDER = ("Build", "Quality", "Portable", "Inspect")
HELP_TOKENS = {"-h", "--help", "help"}

GLOBAL_OPTIONS: tuple[tuple[str, str], ...] = (
    ("-j, --jobs N", "Parallel jobs. The default is capped from both CPU count and available system memory."),
    ("-v, --verbose", "Show executed subprocesses and fine-grained timing information."),
    ("--color MODE", "Color policy: always (default), auto, or never."),
    ("--no-color", "Disable ANSI color while preserving the same textual hierarchy."),
    ("-h, --help", "Show top-level help, or focused help when used with a command."),
)

ENVIRONMENT_HELP: tuple[tuple[str, str], ...] = (
    ("MAKE_TARGET=name", "Override the inferred project binary."),
    ("MAKE_PROJECT_NAME=name", "Override the package/project display name."),
    ("MAKE_JOBS=N", "Override automatic parallelism when -j is absent."),
    ("MAKE_JOB_MEMORY_MB=1536", "Estimated memory budget per automatic build job."),
    ("MAKE_ASCII=1", "Force ASCII-only UI glyphs for constrained terminals and logs."),
    ("MAKE_CHECK_TARGETS=a,b", "Ordered ./make commands used by the composed quality gate."),
    ("MAKE_HYGIENE_EXTENSIONS=c,h,...", "Override text-source extensions scanned by source hygiene."),
    ("MAKE_COMPDB=0", "Disable automatic compilation-database refresh after successful builds."),
    ("MAKE_VENDOR_SOURCE_DIRS=a:b", "Local dependency source trees to preserve in a vendor bundle."),
    ("MAKE_VENDOR_SOURCE_ARCHIVES=a:b", "Dependency source archives to preserve in a vendor bundle."),
    ("MAKE_VENDOR_FETCH_SOURCES=1", "Attempt apt-get source for detected system dependencies."),
    ("MAKE_VENDOR_NO_SYSTEM_LIBS=1", "Omit system runtime libraries from the vendor bundle."),
    ("MAKE_VENDOR_ALLOW_UNRESOLVED=1", "Allow an incomplete runtime closure instead of failing closed."),
    ("MAKE_VENDOR_LLVM_CPP_HEADERS=1", "Include LLVM/Clang C++ header trees."),
    ("MAKE_VENDOR_NO_CLANG_BUILTINS=1", "Omit Clang resource headers."),
    ("MAKE_VENDOR_ROOTFS=1", "Also construct an optional chroot-compatible rootfs."),
)


def list_make_targets(_args: list[str], _ctx: AppContext) -> int:
    # Preserve the familiar command while sourcing it from Python's registry.
    for target in sorted(COMMANDS):
        print(target)
    return 0


def _join_shell_continuations(text: str) -> list[str]:
    lines: list[str] = []
    pending = ""
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        if pending:
            pending += " " + line
        else:
            pending = line
        if pending.endswith("\\"):
            pending = pending[:-1].rstrip()
            continue
        lines.append(pending)
        pending = ""
    if pending:
        lines.append(pending)
    return lines


def _native_compdb_from_make() -> list[dict[str, object]]:
    """Generate a compilation database from the canonical Python build plan."""
    config = BuildConfig("debug")
    sets = _source_sets()
    candidates: dict[str, tuple[int, dict[str, object]]] = {}
    for kind, rank, embed in (("runtime", 1, False), ("compiler", 0, False),
                              ("embed", 2, True), ("api", 3, False)):
        for source in sets[kind]:
            output = _object_path(source, kind)
            command = _compile_command(source, output, config, embed=embed)
            entry = {"directory": str(ROOT), "file": str(source.resolve()), "arguments": command}
            current = candidates.get(entry["file"])
            if current is None or rank < current[0]:
                candidates[entry["file"]] = (rank, entry)
    return [item[1] for item in sorted(candidates.values(), key=lambda item: str(item[1]["file"]))]


def _publish_compdb(path: Path) -> None:
    if COMPDB_LINK.exists() or COMPDB_LINK.is_symlink():
        COMPDB_LINK.unlink()
    try:
        relative_symlink(path, COMPDB_LINK)
    except OSError:
        shutil.copy2(path, COMPDB_LINK)


def generate_compdb(ctx: AppContext, *, quiet: bool = False) -> int:
    BUILD_ROOT.mkdir(parents=True, exist_ok=True)
    entries = _native_compdb_from_make()
    if not entries:
        raise CliError("no compiler invocations were found for compile_commands.json")
    source = "Python build plan"
    COMPDB_PATH.write_text(json.dumps(entries, indent=2) + "\n", encoding="utf-8")
    _publish_compdb(COMPDB_PATH)
    if not quiet:
        UI.ok(f"compilation database ready · {len(entries)} translation units · {source}")
        UI.hint(f"clangd discovery: {rel(COMPDB_LINK)} -> {rel(COMPDB_PATH)}")
    return 0


def command_compdb(args: list[str], ctx: AppContext) -> int:
    if args:
        die("compdb takes no arguments")
    return generate_compdb(ctx)


def refresh_compdb(ctx: AppContext) -> None:
    if not env_flag("MAKE_COMPDB", True):
        return
    try:
        generate_compdb(ctx, quiet=True)
        if UI.verbose:
            UI.log("COMPDB", f"refreshed {rel(COMPDB_PATH)}", "accent_soft")
    except Exception as exc:
        UI.warn(f"could not refresh compile_commands.json: {exc}")


def command_all(args: list[str], ctx: AppContext) -> int:
    started = time.perf_counter()
    if args:
        die("all: use global -j/--jobs for build parallelism")
    build_project("debug", jobs=ctx.jobs)
    refresh_compdb(ctx)
    UI.ok(f"build completed in {format_elapsed(time.perf_counter() - started)}")
    return 0


def command_build_mode(mode: str) -> CommandFunc:
    def handler(args: list[str], ctx: AppContext) -> int:
        if args:
            die(f"{mode}: use global -j/--jobs for build parallelism")
        started = time.perf_counter()
        build_project(mode, jobs=ctx.jobs)
        if mode in {"release", "debug"}:
            refresh_compdb(ctx)
        UI.ok(f"{mode} completed in {format_elapsed(time.perf_counter() - started)}")
        return 0

    return handler


def command_install(args: list[str], ctx: AppContext) -> int:
    if args:
        die("install: use environment variables PREFIX, BINDIR, LIBDIR, or INCDIR")
    started = time.perf_counter()
    target = build_project("debug", jobs=ctx.jobs)
    prefix = Path(os.environ.get("PREFIX", "/usr/local")).expanduser()
    bindir = Path(os.environ.get("BINDIR", str(prefix / "bin")))
    libdir = Path(os.environ.get("LIBDIR", str(prefix / "lib")))
    incdir = Path(os.environ.get("INCDIR", str(prefix / "include" / "monad")))
    core_dir = Path(os.environ.get("COREDIR", str(prefix / "lib" / "monad" / "core")))
    for directory in (bindir, libdir, incdir):
        directory.mkdir(parents=True, exist_ok=True)
    shutil.copy2(target, bindir / target.name)
    libraries = [
        "libmonad.a", "libmonad-embed.a", "libmonad-embed.so",
        "libmonad-compiler.a", "libmonad-compiler.so",
    ]
    for name in libraries:
        source = BUILD_ROOT / "lib" / name
        if source.exists():
            shutil.copy2(source, libdir / name)
    for header in [ROOT / "src" / "runtime.h", *(ROOT / "src" / "embed" / "include" / "monad").glob("*.h")]:
        shutil.copy2(header, incdir / ("runtime.h" if header.name == "runtime.h" else header.name))
    if core_dir.exists():
        shutil.rmtree(core_dir)
    for source in (ROOT / "core").rglob("*.mon"):
        if source.name.startswith(".#"):
            continue
        destination = core_dir / source.relative_to(ROOT / "core")
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    if env_flag("PREWARM_REPL_CACHE", True):
        cache_dir = Path(os.environ.get("CORE_CACHE_DIR", str(Path.home() / ".cache" / "monad" / "core")))
        if cache_dir.exists():
            shutil.rmtree(cache_dir)
        cache_dir.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(bindir / target.name)], input=b"", stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, check=False)
    UI.ok(f"install completed in {format_elapsed(time.perf_counter() - started)}")
    return 0


def command_uninstall(args: list[str], _ctx: AppContext) -> int:
    if args:
        die("uninstall: use environment variables PREFIX, BINDIR, LIBDIR, or INCDIR")
    prefix = Path(os.environ.get("PREFIX", "/usr/local")).expanduser()
    bindir = Path(os.environ.get("BINDIR", str(prefix / "bin")))
    libdir = Path(os.environ.get("LIBDIR", str(prefix / "lib")))
    incdir = Path(os.environ.get("INCDIR", str(prefix / "include" / "monad")))
    core_dir = Path(os.environ.get("COREDIR", str(prefix / "lib" / "monad" / "core")))
    for path in [bindir / "monad", *[libdir / name for name in (
        "libmonad.a", "libmonad-embed.a", "libmonad-embed.so",
        "libmonad-compiler.a", "libmonad-compiler.so",
    )]]:
        path.unlink(missing_ok=True)
    for directory in (incdir, core_dir):
        if directory.exists():
            shutil.rmtree(directory)
    UI.ok("uninstall completed")
    return 0


def command_help(args: list[str], _ctx: AppContext) -> int:
    if args:
        return 0 if print_command_help(args[0]) else 2
    print_help()
    return 0


def command_bin_static(args: list[str], ctx: AppContext) -> int:
    return run_static(["bin", *args], ctx)


def command_asan(args: list[str], ctx: AppContext) -> int:
    return command_sanitizer("asan", args, ctx)


def command_ubsan(args: list[str], ctx: AppContext) -> int:
    return command_sanitizer("ubsan", args, ctx)


COMMAND_SPECS: tuple[CommandSpec, ...] = (
    CommandSpec(
        "all", "Build", "./make [--jobs N] all",
        "Build the project's default target with CPU/RAM-aware parallelism.",
        command_all,
        (("-j, --jobs N", "Override automatic parallelism."),),
    ),
    CommandSpec(
        "release", "Build", "./make [--jobs N] release",
        "Run the release target, falling back to the default build target.",
        command_build_mode("release"),
    ),
    CommandSpec(
        "debug", "Build", "./make [--jobs N] debug",
        "Run the debug target, falling back to the default build target.",
        command_build_mode("debug"),
    ),
    CommandSpec(
        "perf", "Build", "./make perf",
        "Build an optimized profiling binary and libraries.", command_build_mode("perf"),
    ),
    CommandSpec(
        "compdb", "Build", "./make compdb",
        "Generate compile_commands.json for clangd from the canonical Python build plan.",
        command_compdb, no_args=True,
    ),
    CommandSpec(
        "run", "Build", "./make run [--] [program args]",
        "Build the inferred project binary if needed, then execute it.",
        command_run,
        (("--", "Treat every following token literally as a program argument."),),
        present=False,
    ),
    CommandSpec(
        "install", "Build", "./make install",
        "Install the compiler, libraries, headers, and core library.", command_install,
    ),
    CommandSpec(
        "uninstall", "Build", "./make uninstall",
        "Remove the installed compiler, libraries, headers, and core library.", command_uninstall,
    ),
    CommandSpec(
        "clean", "Build", "./make clean [--check]",
        "Deep-clean generated repository state or verify that none is present.", command_clean,
        (
            ("--check", "Fail without mutating the checkout when generated state is present; intended for Git hooks."),
            ("--verbose", "Show every generated path removed or rejected."),
        ),
    ),
    CommandSpec(
        "check", "Quality", "./make check [--quick|--no-build|--no-tests]",
        "Run the fail-closed project quality gate.", command_check,
        (
            ("--quick", "Run hygiene and configured quality targets, but skip tests in the composed fallback gate."),
            ("--no-build", "Skip the build stage in the composed fallback gate."),
            ("--no-tests", "Skip the test stage in the composed fallback gate."),
            ("MAKE_CHECK_TARGETS=a,b", "Use an explicit ordered set of ./make commands for the gate."),
        ),
    ),
    CommandSpec(
        "test", "Quality", "./make test [filters] [options]",
        "Run the canonical .mon verification suite with the shared terminal UI.", command_test,
        (
            ("filter ...", "Select a recursive test menu, for example: codegen errors reader."),
            ("--name REGEX", "Select tests by TEST-ID/name; repeatable."),
            ("--tier NAME", "Select regression, known-fail, future, or generated tiers."),
            ("--all-tiers", "Include every test tier instead of the regression gate."),
            ("--list", "List selected .mon fixtures without executing them."),
            ("--list-targets", "List recursive test-menu targets."),
            ("--only-failed", "Rerun failures recorded by the previous suite execution."),
            ("--rerun-first-failure", "Rerun only the previous first failure."),
            ("--fail-fast", "Stop after the first failing fixture."),
            ("--max-failures N", "Stop after N failures."),
            ("--validate-metadata", "Validate required TEST-* metadata and exit."),
            ("--validate-context-links", "Validate TEST-CONTEXT references and exit."),
            ("--no-build", "Use an existing compiler instead of building first."),
        ),
        present=False,
    ),
    CommandSpec(
        "test-core", "Quality", "./make test-core",
        "Run the core module verification suite.", command_core_tests,
    ),
    CommandSpec(
        "core", "Quality", "./make core",
        "Alias for the core module verification suite.", command_core_tests,
    ),
    CommandSpec(
        "test-embedding", "Quality", "./make test-embedding",
        "Run the explicit native embedding contract suite.", command_embedding_tests,
    ),
    CommandSpec(
        "repl", "Quality", "./make repl",
        "Run the focused REPL contract suite.", command_repl_tests,
    ),
    CommandSpec(
        "bytecode", "Quality", "./make bytecode",
        "Run the bytecode contract suite with visual output enabled.", command_bytecode_tests,
    ),
    CommandSpec(
        "test-bytecode", "Quality", "./make test-bytecode",
        "Alias for the bytecode contract suite.", command_bytecode_tests,
    ),
    CommandSpec(
        "test-runner", "Quality", "./make test-runner",
        "Run the compiler-facing runner contract.", command_runner_contract,
    ),
    CommandSpec(
        "test-how-to", "Quality", "./make test-how-to",
        "Compile and execute the how-to examples contract.", command_how_to_tests,
    ),
    CommandSpec(
        "test-context-visualizer", "Quality", "./make test-context-visualizer",
        "Run context visualizer contract tests.", command_context_tests("src.testing.contracts.test_context_visualizer"),
    ),
    CommandSpec(
        "test-context-lint", "Quality", "./make test-context-lint",
        "Run context linter contract tests.", command_context_tests("src.testing.contracts.test_context_lint"),
    ),
    CommandSpec(
        "test-context-refs", "Quality", "./make test-context-refs",
        "Run context reference contract tests.", command_context_tests("src.testing.contracts.test_context_refs"),
    ),
    CommandSpec(
        "test-context-graph", "Quality", "./make test-context-graph",
        "Run context graph contract tests.", command_context_tests("src.testing.contracts.test_context_graph"),
    ),
    CommandSpec(
        "verify-context", "Quality", "./make verify-context",
        "Validate context IDs, references, and test links.", command_verify_context,
    ),
    CommandSpec(
        "verify-context-strict", "Quality", "./make verify-context-strict",
        "Run advisory strict context quality metrics.", command_verify_context_strict,
    ),
    CommandSpec(
        "test-fuzzing", "Quality", "./make test-fuzzing",
        "Run deterministic code-generation fuzz contracts.", command_fuzzing,
    ),
    CommandSpec(
        "fuzzing", "Quality", "./make fuzzing",
        "Alias for deterministic code-generation fuzz contracts.", command_fuzzing,
    ),
    CommandSpec(
        "generate-asm-tests", "Quality", "./make generate-asm-tests",
        "Generate the inline-assembly regression fixtures.", command_generate_asm("generate-asm-tests"),
    ),
    CommandSpec(
        "generate-asm-tests-extra", "Quality", "./make generate-asm-tests-extra",
        "Generate the extended inline-assembly regression fixtures.", command_generate_asm("generate-asm-tests-extra"),
    ),
    CommandSpec(
        "asan", "Quality", "./make asan [--build-only]",
        "Run AddressSanitizer through a native target or a generic compiler-flag fallback.",
        command_asan,
        (("--build-only", "Build with sanitizer instrumentation without automatically running the test target."),),
    ),
    CommandSpec(
        "ubsan", "Quality", "./make ubsan [--build-only]",
        "Run UndefinedBehaviorSanitizer through a native target or a generic compiler-flag fallback.",
        command_ubsan,
        (("--build-only", "Build with sanitizer instrumentation without automatically running the test target."),),
    ),
    CommandSpec(
        "hooks", "Quality", "./make hooks",
        "Activate repository-managed .githooks for this checkout.", command_hooks,
        no_args=True,
    ),
    CommandSpec(
        "install-git-hooks", "Quality", "./make install-git-hooks",
        "Alias for installing repository-managed Git hooks.", command_hooks,
        no_args=True,
    ),
    CommandSpec(
        "verify-push", "Quality", "./make verify-push",
        "Run the clean-tree and quality gates used before pushing.", command_verify_push,
        no_args=True,
    ),
    CommandSpec(
        "context-visualizer", "Quality", "./make context-visualizer",
        "Render the context corpus visualization.",
        lambda args, _ctx: subprocess.run(
            [sys.executable, "-B", "src/tooling/context/visualizer.py", *args],
            cwd=str(ROOT), check=False,
        ).returncode,
    ),
    CommandSpec(
        "vendor", "Portable", "./make vendor [command-or-path] [dir]",
        "Create a relocatable runtime/toolchain bundle with manifests and dependency metadata.",
        run_vendor,
        (
            ("build/vendor/bin", "Bundled binary plus tool wrappers."),
            ("build/vendor/lib", "Deduplicated runtime and toolchain libraries."),
            ("build/vendor/include", "C API headers; large C++ headers remain opt-in."),
            ("build/vendor/src", "Dependency source metadata and optional source payloads."),
            ("MAKE_VENDOR_ROOTFS=1", "Additionally create the optional chroot-compatible filesystem view."),
            ("MAKE_VENDOR_NO_SYSTEM_LIBS=1", "Create a thinner same-system bundle without libc/libm-style system libraries."),
            ("MAKE_VENDOR_ALLOW_UNRESOLVED=1", "Permit packaging with unresolved runtime SONAMEs; disabled by default."),
            ("MAKE_VENDOR_LLVM_CPP_HEADERS=1", "Include the large LLVM/Clang C++ header trees."),
            ("MAKE_VENDOR_NO_CLANG_BUILTINS=1", "Omit Clang resource/builtin headers."),
        ),
    ),
    CommandSpec(
        "static", "Portable", "./make static bin | ./make static check <binary>",
        "Create or inspect the portable binary folder.", run_static,
        (
            ("bin", "Build build/static with launcher, runtime closure, and manifest."),
            ("check <binary>", "Classify linkage and report dynamic dependencies or unresolved SONAMEs."),
        ),
        help_on_empty=True,
    ),
    CommandSpec(
        "bin-static", "Portable", "./make bin-static",
        "Alias for ./make static bin.", command_bin_static,
    ),
    CommandSpec(
        "tar", "Portable", "./make tar [--with-binaries] [--with-vendor] [--with-context]",
        "Create the minimal complete source package, with heavy portable assets opt-in.", run_tar,
        (
            ("--with-binaries", "Include build/static in the package and use the -static package name."),
            ("--with-vendor", "Include the self-contained vendor/toolchain bundle."),
            ("--with-context", "Include context/, glyph/, and etc/ design/research material."),
            ("MAKE_TAR_WITH_BINARIES=1", "Environment equivalent of --with-binaries."),
            ("MAKE_TAR_WITH_VENDOR=1", "Environment equivalent of --with-vendor."),
        ),
    ),
    CommandSpec(
        "doctor", "Inspect", "./make doctor",
        "Audit project discovery, host capacity, tools, dependencies, and runtime closure.",
        run_doctor, present=True, no_args=True,
    ),
    CommandSpec(
        "deps", "Inspect", "./make deps [--install]",
        "Inspect required host tools and pkg-config dependencies.", command_deps,
        (("--install", "Install generic host build tools on supported package-manager platforms."),),
    ),
    CommandSpec(
        "env", "Inspect", "./make env [--json]",
        "Emit the resolved build environment for humans or automation.", run_env,
        (("--json", "Emit stable JSON instead of KEY=value lines."),),
        present=False,
    ),
    CommandSpec(
        "targets", "Inspect", "./make targets",
        "List available ./make commands, one per line for shell-friendly use.",
        list_make_targets, present=False, no_args=True,
    ),
    CommandSpec(
        "help", "Inspect", "./make help [command]",
        "Show the command index or focused documentation for one command.",
        command_help, present=False,
    ),
)


def build_command_registry(specs: tuple[CommandSpec, ...]) -> dict[str, CommandSpec]:
    registry: dict[str, CommandSpec] = {}
    valid_groups = set(GROUP_ORDER)
    for spec in specs:
        if spec.name in registry:
            raise RuntimeError(f"duplicate command specification: {spec.name}")
        if spec.group not in valid_groups:
            raise RuntimeError(f"unknown command group {spec.group!r} for {spec.name}")
        if not spec.usage.startswith("./make"):
            raise RuntimeError(f"invalid command usage for {spec.name}: {spec.usage}")
        if not spec.summary.strip():
            raise RuntimeError(f"missing command summary for {spec.name}")
        registry[spec.name] = spec
    return registry


COMMANDS = build_command_registry(COMMAND_SPECS)


def command_summary(name: str) -> str:
    spec = COMMANDS.get(name)
    return spec.summary if spec else "Run a command through the Python build frontend."


def print_command_help(name: str) -> bool:
    spec = COMMANDS.get(name)
    if spec is None:
        if make_has_target(name):
            UI.header(name, "Legacy target exposed through the project build frontend.")
            UI.section("Usage")
            print(f"  {UI.paint(UI.theme.accent, './make ' + name)} {UI.dim('[make args]')}")
            print("")
            UI.section("Global options")
            UI.option_rows(list(GLOBAL_OPTIONS))
            return True
        UI.error(f"unknown help topic '{name}'")
        UI.hint("use ./make help for the command index or ./make targets for available commands")
        return False

    UI.header(spec.name, spec.summary)
    UI.section("Usage")
    print(f"  {UI.paint(UI.theme.accent, spec.usage)}")
    if spec.options:
        print("")
        UI.section("Details")
        UI.option_rows(list(spec.options))
    print("")
    UI.section("Global options")
    UI.option_rows(list(GLOBAL_OPTIONS))
    return True


def print_help() -> None:
    UI.header("build frontend", "One entry point for build, quality, diagnostics, packaging, and portable artifacts.")
    UI.section("Usage")
    print(f"  {UI.paint(UI.theme.accent, './make')} {UI.strong('<command>')} {UI.dim('[options]')}")
    print(f"  {UI.paint(UI.theme.accent, './make help')} {UI.strong('<command>')}")
    print("")
    for group in GROUP_ORDER:
        specs = [spec for spec in COMMAND_SPECS if spec.group == group]
        UI.section(group)
        UI.option_rows([(spec.name, spec.summary) for spec in specs])
        print("")
    UI.section("Global options")
    UI.option_rows(list(GLOBAL_OPTIONS))
    print("")
    UI.section("Environment")
    UI.option_rows(list(ENVIRONMENT_HELP))
    print("")
    UI.hint("run ./make help <command> for focused documentation")


### CLI

def parse(argv: list[str]) -> tuple[argparse.Namespace, list[str]]:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--help", "-h", action="store_true")
    parser.add_argument("--version", action="store_true")
    parser.add_argument("--color", nargs="?", const="always", default=None)
    parser.add_argument("--no-color", action="store_true")
    parser.add_argument("-v", "--verbose", action="store_true")
    parser.add_argument("-j", "--jobs", type=int, default=0)
    global_args: list[str] = []
    rest: list[str] = []
    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg == "--":
            rest = argv[i + 1:]
            break
        if arg in ("--help", "-h", "--version", "--no-color", "-v", "--verbose"):
            global_args.append(arg)
            i += 1
            continue
        if arg == "--color":
            valid_modes = {"auto", "always", "never", "on", "off", "tty", "default", "1", "0", "true", "false", "yes", "no"}
            if i + 1 < len(argv) and argv[i + 1].strip().lower() in valid_modes:
                global_args.extend([arg, argv[i + 1]])
                i += 2
            else:
                global_args.append("--color=always")
                i += 1
            continue
        if arg in ("-j", "--jobs"):
            if i + 1 >= len(argv):
                raise CliError(f"{arg} requires a positive integer")
            try:
                if int(argv[i + 1]) <= 0:
                    raise ValueError
            except ValueError as exc:
                raise CliError(f"{arg} requires a positive integer") from exc
            global_args.extend([arg, argv[i + 1]])
            i += 2
            continue
        if arg.startswith("-j") and arg[2:].isdigit():
            if int(arg[2:]) <= 0:
                raise CliError("-j requires a positive integer")
            global_args.extend(["--jobs", arg[2:]])
            i += 1
            continue
        if arg.startswith("--color="):
            global_args.append(arg)
            i += 1
            continue
        if arg.startswith("--jobs="):
            value = arg.split("=", 1)[1]
            try:
                if int(value) <= 0:
                    raise ValueError
            except ValueError as exc:
                raise CliError("--jobs requires a positive integer") from exc
            global_args.append(arg)
            i += 1
            continue
        rest = argv[i:]
        break
    else:
        rest = []
    return parser.parse_args(global_args), rest


def command_help_requested(spec: CommandSpec, args: list[str]) -> bool:
    if spec.help_on_empty and not args:
        return True
    return bool(args and args[0] in HELP_TOKENS)


def validate_command_args(spec: CommandSpec, args: list[str]) -> None:
    if spec.no_args and args:
        die(f"{spec.name}: no arguments are supported")


def execute_command(spec: CommandSpec, args: list[str], ctx: AppContext) -> int:
    if command_help_requested(spec, args):
        return 0 if print_command_help(spec.name) else 2
    validate_command_args(spec, args)
    if spec.present:
        ctx.ui.header(spec.name, spec.summary)
        if ctx.ui.verbose:
            ctx.ui.kv_rows([("jobs", f"{ctx.jobs} ({ctx.jobs_source})"), ("host", f"{host_os()} / {host_arch()}")])
            print("")
    elif ctx.ui.verbose:
        ctx.ui.log("HOST", f"jobs={ctx.jobs} ({ctx.jobs_source})", "info")
    return spec.handler(args, ctx)


def _main(argv: list[str]) -> int:
    ns, rest = parse(argv)
    mode = "never" if ns.no_color else parse_color_mode(ns.color)
    UI.color = color_enabled(mode)
    UI.verbose = bool(ns.verbose)

    if ns.version:
        print(f"{PROJECT.name} build frontend")
        return 0
    if ns.help:
        if rest:
            return 0 if print_command_help(rest[0]) else 2
        print_help()
        return 0
    if not rest:
        print_help()
        return 0

    jobs, jobs_source = resolve_jobs(ns.jobs)
    ctx = AppContext(PROJECT, jobs, jobs_source, UI)
    cmd, args = rest[0], rest[1:]
    spec = COMMANDS.get(cmd)
    if spec is not None:
        return execute_command(spec, args, ctx)

    die(
        f"unknown command '{cmd}'",
        hint="run ./make help for the canonical command index",
    )


def main(argv: list[str] | None = None) -> int:
    try:
        return _main(list(sys.argv[1:] if argv is None else argv))
    except CliError as exc:
        UI.error(exc.message)
        for detail in exc.details:
            UI.error_detail(detail)
        if exc.hint:
            UI.hint(exc.hint, stream=sys.stderr)
        return exc.code
    except subprocess.CalledProcessError as exc:
        rc = int(exc.returncode or 1)
        UI.error(f"command failed with exit status {rc}")
        return rc
    except KeyboardInterrupt:
        print(file=sys.stderr)
        UI.warn("interrupted")
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
