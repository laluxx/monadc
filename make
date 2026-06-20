#!/usr/bin/env -S python3 -B
from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
import platform
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
from dataclasses import dataclass
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
LOADED_CONFIGS: list[Path] = []
COLOR = True

### Console

def supports_glyph(glyph: str) -> bool:
    enc = sys.stdout.encoding or "utf-8"
    try:
        glyph.encode(enc)
        return True
    except Exception:
        return False


def parse_color_mode(raw: str | None) -> str:
    value = (raw or "always").strip().lower()
    if value in ("always", "on", "1", "true", "yes"):
        return "always"
    if value in ("never", "off", "0", "false", "no"):
        return "never"
    if value in ("auto", "tty", "default", ""):
        return "auto"
    raise SystemExit(f"make: invalid color mode '{raw}' (expected auto|always|never)")


def color_enabled(mode: str) -> bool:
    if os.environ.get("NO_COLOR"):
        return False
    if os.environ.get("FORCE_COLOR") or os.environ.get("CLICOLOR_FORCE"):
        return True
    if mode == "always":
        return True
    if mode == "never":
        return False
    return (sys.stdout.isatty() or sys.stderr.isatty()) and os.environ.get("TERM", "") != "dumb"


def c(code: str, text: str) -> str:
    if not COLOR:
        return text
    return f"\033[{code}m{text}\033[0m"


def ok_mark() -> str:
    return "OK" if not supports_glyph("✓") else "✓"


def fail_mark() -> str:
    return "FAIL" if not supports_glyph("✗") else "✗"


def tag(name: str, color: str = "1;35") -> str:
    return c(color, name)


def log(name: str, msg: str, color: str = "1;35") -> None:
    print(f"{tag(name, color)} {msg}", flush=True)


def step(msg: str) -> None:
    arrow = "->" if not supports_glyph("→") else "→"
    print(f"{c('1;36', arrow)} {c('36', msg)}", flush=True)


def ok(msg: str) -> None:
    print(f"{c('1;32', ok_mark())} {c('32', msg)}", flush=True)


def warn(msg: str) -> None:
    print(f"{c('1;33', 'WARN')} {c('33', msg)}", file=sys.stderr, flush=True)


def err(msg: str) -> None:
    print(f"{c('1;31', 'ERROR')} {msg}", file=sys.stderr, flush=True)


def die(msg: str, code: int = 2) -> None:
    err(msg)
    raise SystemExit(code)


def status_word(good: bool) -> str:
    return c("1;32", "ok") if good else c("1;31", "missing")


def print_section(title: str) -> None:
    print(c("1;36", title))


def print_row(name: str, state: str | None, value: str, *, good: bool | None = None) -> None:
    state_s = "" if state is None else state
    if good is not None:
        state_s = status_word(good)
    print(f"  {state_s:<18} {c('1', name):<24} {value}")


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


def run_capture(cmd: list[str] | str, *, cwd: Path | None = None, shell: bool = False, timeout: int | None = None) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(cmd, cwd=str(cwd or ROOT), shell=shell, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    except (FileNotFoundError, subprocess.TimeoutExpired) as exc:
        return subprocess.CompletedProcess(cmd, 127, "", str(exc))


def color_build_line(line: str) -> str:
    if not COLOR or not line:
        return line
    low = line.lower()
    stripped = line.lstrip()
    if "error:" in low or stripped.startswith("FAILED:") or " failed" in low:
        return c("1;31", line)
    if "warning:" in low or " warning" in low:
        return c("1;33", line)
    if stripped.startswith("[") and "]" in stripped[:16]:
        end = stripped.find("]") + 1
        lead = line[: len(line) - len(stripped)]
        return lead + c("1;36", stripped[:end]) + stripped[end:]
    if stripped.startswith("--"):
        return c("36", line)
    return line


def run(cmd: list[str] | str, *, cwd: Path | None = None, shell: bool = False, env: dict[str, str] | None = None, quiet: bool = False) -> None:
    merged = os.environ.copy()
    if env:
        merged.update(env)
    if COLOR:
        merged.setdefault("FORCE_COLOR", "1")
        merged.setdefault("CLICOLOR_FORCE", "1")
    if quiet:
        res = subprocess.run(cmd, cwd=str(cwd or ROOT), shell=shell, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=merged)
        if res.returncode != 0:
            if res.stdout:
                sys.stderr.write(res.stdout)
            raise subprocess.CalledProcessError(res.returncode, cmd)
        return
    proc = subprocess.Popen(cmd, cwd=str(cwd or ROOT), shell=shell, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=merged, bufsize=1)
    assert proc.stdout is not None
    for line in proc.stdout:
        print(color_build_line(line.rstrip("\n")), file=sys.stderr, flush=True)
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


def parse_make_var(name: str, default: str = "") -> str:
    text = makefile_text()
    m = re.search(rf"^\s*{re.escape(name)}\s*(?::=|\+=|=)\s*(.*?)\s*$", text, re.M)
    if not m:
        return default
    return m.group(1).strip().strip('"\'')


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


def system_make() -> str:
    for name in ("gmake", "make"):
        p = which(name)
        if p and Path(p).resolve() != Path(__file__).resolve():
            return p
    die("system make was not found")


def discover_project() -> Project:
    target = os.environ.get("MAKE_TARGET", "").strip() or parse_make_var("TARGET")
    name = os.environ.get("MAKE_PROJECT_NAME", "").strip() or target or ROOT.name
    runtime_lib = os.environ.get("MAKE_RUNTIME_LIB", "").strip() or parse_make_var("RUNTIME_LIB")
    source_dirs = [p for p in ("src", "include", "core", "lib", "tests", "context", "docs", "scripts") if (ROOT / p).exists()]
    source_files = [p for p in ("Makefile", "makefile", "GNUmakefile", "make", "main.c", "runtime.c", "runtime.h") if (ROOT / p).exists()]
    package_files = [p for p in ("README", "README.md", "LICENSE", "LICENSE.txt", "COPYING", "CHANGELOG.md", "pyproject.toml", "CMakeLists.txt") if (ROOT / p).exists()]
    build_target = os.environ.get("MAKE_BUILD_TARGET", "").strip() or ("all" if make_has_target("all") else "")
    release_target = os.environ.get("MAKE_RELEASE_TARGET", "").strip() or ("release" if make_has_target("release") else build_target)
    return Project(name=name, target=target, runtime_lib=runtime_lib, build_target=build_target, release_target=release_target, source_dirs=source_dirs, source_files=source_files, package_files=package_files)


PROJECT = discover_project()
load_config(PROJECT.name)
PROJECT = discover_project()


def rel(path: Path) -> str:
    try:
        return str(path.resolve().relative_to(ROOT.resolve()))
    except Exception:
        return str(path)


### File helpers

def copy_file(src: Path, dst: Path) -> None:
    if not src.exists() or not src.is_file():
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


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


def write_manifest(root: Path, extra: dict[str, object] | None = None) -> None:
    files: list[dict[str, object]] = []
    for p in sorted(root.rglob("*")):
        if p.name == "MANIFEST.json":
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
    (root / "MANIFEST.json").write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


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


### Makefile bridge

def run_make(targets: list[str] | None = None, *, jobs: int = 0, extra: list[str] | None = None) -> None:
    cmd = [system_make()]
    if jobs > 0:
        cmd.append(f"-j{jobs}")
    if targets:
        cmd.extend(targets)
    if extra:
        cmd.extend(extra)
    log("MAKE", " ".join(cmd), "1;34")
    run(cmd)


def possible_binary_paths() -> list[Path]:
    exe = ".exe" if host_os() == "windows" else ""
    out: list[Path] = []
    if PROJECT.target:
        for prefix in (ROOT, BUILD_ROOT, BUILD_ROOT / "release", BUILD_ROOT / "debug", ROOT / "bin", BUILD_ROOT / "bin"):
            out.append(prefix / (PROJECT.target + exe))
    return out


def find_binary(build_if_missing: bool = False, jobs: int = 0) -> Path:
    for p in possible_binary_paths():
        if p.exists() and p.is_file():
            return p
    if build_if_missing:
        target = PROJECT.release_target if PROJECT.release_target and make_has_target(PROJECT.release_target) else PROJECT.build_target
        step(f"build: running make {target or 'all'}")
        run_make([target] if target else None, jobs=jobs)
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
    names.extend(env_list("MAKE_VENDOR_LIBS"))
    return sorted(dict.fromkeys(names))


def detected_pkg_config_packages() -> list[str]:
    pkgs: list[str] = []
    for m in re.finditer(r"pkg-config\s+([^\n\r`$()]*)", makefile_text()):
        tail = m.group(1)
        for token in re.findall(r"[A-Za-z0-9_.+-]+", tail):
            if token.startswith("-") or token in {"pkg-config", "cflags", "libs", "exists", "echo", "2", "dev", "null"}:
                continue
            pkgs.append(token)
    pkgs.extend(env_list("MAKE_VENDOR_PKGS"))
    return sorted(dict.fromkeys(pkgs))


def llvm_config_path() -> str:
    raw = os.environ.get("LLVM_CONFIG", "").strip()
    for name in [raw, "llvm-config", "llvm-config-22", "llvm-config-21", "llvm-config-20", "llvm-config-19", "llvm-config-18", "llvm-config-17", "llvm-config-16"]:
        if not name:
            continue
        if os.sep in name and Path(name).exists():
            return name
        found = which(name)
        if found:
            return found
    return ""


def dependency_report() -> dict[str, object]:
    return {"link_libraries": detected_link_libraries(), "pkg_config_packages": detected_pkg_config_packages(), "llvm_config": llvm_config_path()}


### Runtime closure

def ldd_paths(binary: Path) -> list[Path]:
    if host_os() != "linux" or not which("ldd"):
        return []
    res = run_capture(["ldd", str(binary)], timeout=10)
    text = (res.stdout or "") + (res.stderr or "")
    out: list[Path] = []
    for line in text.splitlines():
        s = line.strip()
        if not s or "linux-vdso" in s or "not a dynamic executable" in s.lower():
            continue
        path = ""
        m = re.search(r"=>\s+(/\S+)", s)
        if m:
            path = m.group(1)
        elif s.startswith("/"):
            path = s.split()[0]
        if path and Path(path).exists():
            out.append(Path(path))
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
    return list(dict.fromkeys(out))


def dependency_paths(binary: Path) -> list[Path]:
    return ldd_paths(binary) or otool_paths(binary)


def copy_binary_rootfs(binary: Path, rootfs: Path, *, include_system: bool = True) -> list[Path]:
    copied: list[Path] = []
    rootfs.mkdir(parents=True, exist_ok=True)
    for dst in (rootfs / "bin" / binary.name, rootfs / "usr" / "bin" / binary.name):
        copy_file(binary, dst)
        chmod_executable(dst)
        copied.append(dst)
        log("VENDOR", f"  copied {binary} -> {rel(dst)}")
    queue = dependency_paths(binary)
    seen: set[str] = set()
    while queue:
        dep = queue.pop(0)
        try:
            real = dep.resolve()
        except Exception:
            real = dep
        key = str(real)
        if key in seen:
            continue
        seen.add(key)
        if not include_system and re.search(r"/(libc|libm|libdl|libpthread|librt|ld-linux|libSystem)", real.name):
            continue
        if not real.exists() or not real.is_file():
            continue
        dst = rootfs / str(dep).lstrip("/")
        copy_file(real, dst)
        copied.append(dst)
        log("VENDOR", f"  copied {real} -> {rel(dst)}")
        for more in dependency_paths(real):
            try:
                more_key = str(more.resolve())
            except Exception:
                more_key = str(more)
            if more_key not in seen:
                queue.append(more)
    usr_lib = rootfs / "usr" / "lib"
    if usr_lib.exists() and not (rootfs / "lib").exists():
        copytree_replace(usr_lib, rootfs / "lib")
    usr_lib64 = rootfs / "usr" / "lib64"
    if usr_lib64.exists() and not (rootfs / "lib64").exists():
        copytree_replace(usr_lib64, rootfs / "lib64")
    return copied


def flatten_host_libs(rootfs: Path, lib_host: Path) -> list[str]:
    lib_host.mkdir(parents=True, exist_ok=True)
    names: list[str] = []
    for top in ("lib", "lib64", "usr/lib", "usr/lib64"):
        d = rootfs / top
        if not d.exists():
            continue
        for p in d.rglob("*"):
            if p.is_file() and (".so" in p.name or p.suffix in (".dylib", ".dll")):
                dst = lib_host / p.name
                if not dst.exists():
                    copy_file(p, dst)
                    names.append(p.name)
    return sorted(dict.fromkeys(names))


### LLVM vendor

def bundle_llvm(vendor_dir: Path) -> None:
    cfg = llvm_config_path()
    if not cfg:
        log("VENDOR", "no llvm-config found; skipping LLVM/Clang headers", "1;33")
        return
    version = run_capture([cfg, "--version"]).stdout.strip() or "unknown"
    inc_s = run_capture([cfg, "--includedir"]).stdout.strip().splitlines()
    lib_s = run_capture([cfg, "--libdir"]).stdout.strip().splitlines()
    cflags = run_capture([cfg, "--cflags"]).stdout.strip()
    libs = run_capture([cfg, "--libs", "all", "--system-libs"]).stdout.strip()
    ldflags = run_capture([cfg, "--ldflags"]).stdout.strip()
    if not inc_s:
        log("VENDOR", "llvm-config did not report includedir", "1;33")
        return
    inc = Path(inc_s[0])
    libdir = Path(lib_s[0]) if lib_s else Path()
    vendor_include = vendor_dir / "include"
    vendor_lib = vendor_dir / "lib" / "host"
    vendor_bin = vendor_dir / "bin"
    for sub in ("llvm-c", "clang-c", "llvm", "clang"):
        src = inc / sub
        if src.exists():
            copytree_replace(src, vendor_include / sub)
    if libdir.exists():
        for pattern in ("libLLVM*.so*", "libclang*.so*", "libLLVM*.dylib", "libclang*.dylib"):
            for p in libdir.glob(pattern):
                if p.is_file() or p.is_symlink():
                    try:
                        src = p.resolve()
                    except Exception:
                        src = p
                    copy_file(src, vendor_lib / src.name)
    major = version.split(".", 1)[0]
    for cand in (inc.parent / "lib" / "clang" / major / "include", libdir.parent / "lib" / "clang" / major / "include", libdir / "clang" / major / "include", Path("/usr/lib") / "clang" / major / "include"):
        if cand.exists() and any(cand.iterdir()):
            copytree_replace(cand, vendor_include / "clang-builtins")
            break
    vendor_bin.mkdir(parents=True, exist_ok=True)
    script = f'''#!/usr/bin/env sh
### Generated by ./make vendor
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/..
case "${{1:-}}" in
  --version) echo "{version}" ;;
  --includedir) echo "$here/include" ;;
  --libdir) echo "$here/lib/host" ;;
  --cflags) echo "{cflags.replace('"', '\\"')}" | sed "s|-I[^ ]*|-I$here/include|g" ;;
  --ldflags) echo "-L$here/lib/host {ldflags.replace('"', '\\"')}" ;;
  --libs) echo "{libs.replace('"', '\\"')}" ;;
  --system-libs) echo "" ;;
  *) exit 1 ;;
esac
'''
    path = vendor_bin / "llvm-config"
    path.write_text(script, encoding="utf-8")
    chmod_executable(path)
    log("VENDOR", f"vendored llvm-config ({version}) + LLVM/Clang headers")


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
        warn("MAKE_VENDOR_FETCH_SOURCES=1 but apt-get is unavailable")
        return []
    dst.mkdir(parents=True, exist_ok=True)
    fetched: list[str] = []
    for name in apt_source_names():
        step(f"vendor: fetching source package {name}")
        res = run_capture(["apt-get", "source", "--download-only", name], cwd=dst)
        if res.returncode != 0:
            warn(f"apt-get source failed for {name}")
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
    info = {"local_source_dirs": copied_dirs, "source_archives": copied_archives, "fetched_system_sources": fetched, "detected_dependencies": dependency_report()}
    (source_root / "DEPENDENCIES.json").write_text(json.dumps(info, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if copied_dirs:
        log("VENDOR", f"local dependency source trees: {', '.join(copied_dirs)}")
    if copied_archives:
        log("VENDOR", f"dependency source archives: {', '.join(copied_archives)}")
    if fetched:
        log("VENDOR", f"downloaded system dependency sources: {', '.join(fetched)}")
    if not copied_dirs and not copied_archives and not fetched:
        msg = "no dependency source trees found; set MAKE_VENDOR_SOURCE_DIRS or MAKE_VENDOR_FETCH_SOURCES=1"
        if env_flag("MAKE_VENDOR_REQUIRE_SOURCES", False):
            die("vendor: " + msg)
        log("VENDOR", msg, "1;33")
    return info


### Vendor

def write_vendor_env(vendor_dir: Path, binary_name: str) -> None:
    text = f'''#!/usr/bin/env sh
### Source this to use vendored compiler dependencies
here=$(CDPATH= cd -- "$(dirname -- "${{BASH_SOURCE:-$0}}")" && pwd)
export VENDOR_ROOTFS="$here/rootfs"
export PATH="$here/bin:$here/rootfs/bin:$here/rootfs/usr/bin${{PATH:+:$PATH}}"
export LD_LIBRARY_PATH="$here/lib/host:$here/rootfs/lib:$here/rootfs/lib64:$here/rootfs/usr/lib:$here/rootfs/usr/lib64${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}}"
export DYLD_LIBRARY_PATH="$here/lib/host${{DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}}"
export LIBRARY_PATH="$here/lib/host${{LIBRARY_PATH:+:$LIBRARY_PATH}}"
export CPATH="$here/include${{CPATH:+:$CPATH}}"
if [ -x "$here/bin/llvm-config" ]; then export LLVM_CONFIG="$here/bin/llvm-config"; fi
echo "{PROJECT.name} vendor environment loaded"
'''
    path = vendor_dir / "env.sh"
    path.write_text(text, encoding="utf-8")
    chmod_executable(path)
    run_path = vendor_dir / "run"
    run_path.write_text(f'''#!/usr/bin/env sh
### Generated by ./make vendor
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export LD_LIBRARY_PATH="$here/lib/host:$here/rootfs/lib:$here/rootfs/lib64:$here/rootfs/usr/lib:$here/rootfs/usr/lib64${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}}"
exec "$here/rootfs/bin/{binary_name}" "$@"
''', encoding="utf-8")
    chmod_executable(run_path)


def run_vendor(args: list[str], jobs: int) -> int:
    if args and args[0] in ("-h", "--help", "help"):
        print(c("1;36", f"{PROJECT.name} vendor"))
        print("Usage:")
        print("  ./make vendor                         build/find project binary and vendor it")
        print("  ./make vendor <command-or-path> [dir] copy arbitrary binary + ldd closure")
        print("")
        print("Output:")
        print("  build/vendor/rootfs                   chroot-like binary/dependency tree")
        print("  build/vendor/lib/host                 flat host library path for LD_LIBRARY_PATH")
        print("  build/vendor/include                  LLVM/Clang headers when detected")
        print("  build/vendor/bin/llvm-config          wrapper pointing at vendored paths")
        print("  build/vendor/src/DEPENDENCIES.json    source dependency manifest")
        return 0
    if args:
        binary = resolve_program(args[0])
        vendor_dir = Path(args[1]).expanduser() if len(args) > 1 else VENDOR_DIR
    else:
        binary = find_binary(build_if_missing=True, jobs=jobs)
        vendor_dir = VENDOR_DIR
    shutil.rmtree(vendor_dir, ignore_errors=True)
    rootfs = vendor_dir / "rootfs"
    lib_host = vendor_dir / "lib" / "host"
    bin_dir = vendor_dir / "bin"
    bin_dir.mkdir(parents=True, exist_ok=True)
    log("VENDOR", f"using {rel(vendor_dir)}")
    log("VENDOR", f"binary {binary}")
    copied = copy_binary_rootfs(binary, rootfs, include_system=not env_flag("MAKE_VENDOR_NO_SYSTEM_LIBS", False))
    copy_file(binary, bin_dir / binary.name)
    chmod_executable(bin_dir / binary.name)
    flat = flatten_host_libs(rootfs, lib_host)
    bundle_llvm(vendor_dir)
    source_info = bundle_dependency_sources(vendor_dir)
    write_vendor_env(vendor_dir, binary.name)
    write_manifest(vendor_dir, {"binary": str(binary), "rootfs_files": [str(p.relative_to(vendor_dir)) for p in copied if p.exists()], "flat_host_libs": flat, "dependency_sources": source_info})
    log("VENDOR", f"copied {len(copied)} rootfs files")
    log("VENDOR", f"flat host libs: {len(flat)}")
    ok(f"vendor ready: {rel(vendor_dir)}")
    print(c("90", f"You can inspect it with: find {rel(rootfs)} -maxdepth 3 -type f"))
    print(c("90", f"Chroot-style run idea: sudo chroot {rel(rootfs)} /bin/{binary.name}"))
    return 0


### Static

def run_static(args: list[str], jobs: int) -> int:
    if not args or args[0] in ("-h", "--help", "help"):
        print(c("1;36", f"{PROJECT.name} static"))
        print("Usage:")
        print("  ./make static bin")
        print("  ./make bin-static")
        print("  ./make static check <binary>")
        return 0
    cmd = args[0]
    if cmd == "bin":
        binary = find_binary(build_if_missing=True, jobs=jobs)
        shutil.rmtree(STATIC_DIR, ignore_errors=True)
        copy_file(binary, STATIC_DIR / "bin" / binary.name)
        chmod_executable(STATIC_DIR / "bin" / binary.name)
        copied = copy_binary_rootfs(binary, STATIC_DIR / "rootfs", include_system=not env_flag("MAKE_STATIC_NO_SYSTEM_LIBS", False))
        flat = flatten_host_libs(STATIC_DIR / "rootfs", STATIC_DIR / "lib" / "host")
        run_script = STATIC_DIR / "run"
        run_script.write_text(f'''#!/usr/bin/env sh
### Generated by ./make static bin
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export LD_LIBRARY_PATH="$here/lib/host:$here/rootfs/lib:$here/rootfs/lib64:$here/rootfs/usr/lib:$here/rootfs/usr/lib64${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}}"
exec "$here/bin/{binary.name}" "$@"
''', encoding="utf-8")
        chmod_executable(run_script)
        write_manifest(STATIC_DIR, {"binary": str(binary), "rootfs_files": [str(p.relative_to(STATIC_DIR)) for p in copied if p.exists()], "flat_host_libs": flat})
        log("STATIC", f"binary {rel(binary)}")
        log("STATIC", f"bundled {len(flat)} shared libs")
        ok(f"static ready: {rel(STATIC_DIR)}")
        return 0
    if cmd == "check":
        if len(args) < 2:
            die("usage: ./make static check <binary>")
        binary = Path(args[1]).expanduser()
        deps = dependency_paths(binary)
        log("STATIC", f"{binary}: {'dynamic' if deps else 'static or unknown'}")
        for dep in deps:
            print(f"  {dep}")
        return 1 if deps else 0
    die(f"unknown static command: {cmd}")


### Tar

def tar_source_ignore(_dir: str, names: list[str]) -> set[str]:
    ignored_names = {".git", ".hg", ".svn", ".cache", ".pytest_cache", "__pycache__", "build", "dist", "tmp", "temp", "CMakeFiles", "CMakeCache.txt", "compile_commands.json"}
    ignored_suffixes = (".o", ".a", ".so", ".dylib", ".dll", ".exe", ".pyc", ".pyo", ".gcda", ".gcno", ".profraw", ".profdata")
    return {n for n in names if n in ignored_names or n.endswith(ignored_suffixes)}


def copy_source_tree(package_dir: Path) -> None:
    copied: set[str] = set()
    for name in PROJECT.source_dirs:
        src = ROOT / name
        if src.exists():
            copytree_replace(src, package_dir / name, ignore=tar_source_ignore)
            copied.add(name)
    for name in PROJECT.source_files + PROJECT.package_files:
        src = ROOT / name
        if src.exists():
            copy_file(src, package_dir / name)
            copied.add(name)
    for p in ROOT.iterdir():
        if p.name in copied or p.name.startswith(".") or p.name == "build":
            continue
        if p.is_file() and p.suffix in (".md", ".txt", ".toml", ".json", ".yaml", ".yml", ".ini"):
            copy_file(p, package_dir / p.name)


def write_agent_build(package_dir: Path, with_binaries: bool) -> None:
    target = PROJECT.target or "target"
    text = f'''# Agent build notes for {PROJECT.name}

This package was produced by ./make tar.

## Included

- Project source tree.
- build/vendor/rootfs with the compiler/tool binary and its ldd dependency closure.
- build/vendor/lib/host with a flat runtime library path.
- build/vendor/include and build/vendor/bin/llvm-config when LLVM/Clang was detected.
- build/vendor/src/DEPENDENCIES.json with dependency source metadata.
- build/static when built with --with-binaries.
- MANIFEST.json with hashes.

## Build from source

```sh
. build/vendor/env.sh 2>/dev/null || true
./make doctor
./make all
```

Or directly:

```sh
. build/vendor/env.sh 2>/dev/null || true
make all
```

## Run vendored compiler/tool

```sh
. build/vendor/env.sh 2>/dev/null || true
build/vendor/run --help 2>/dev/null || build/vendor/run
```

## Chroot-style experiment

```sh
sudo chroot build/vendor/rootfs /bin/{target}
```
'''
    if with_binaries:
        text += "\n## Static folder\n\n```sh\nbuild/static/run --help 2>/dev/null || build/static/run\n```\n"
    (package_dir / "AGENT_BUILD.md").write_text(text, encoding="utf-8")


def run_tar(args: list[str], jobs: int) -> int:
    if args and args[0] in ("-h", "--help", "help"):
        print(c("1;36", f"{PROJECT.name} tar"))
        print("Usage:")
        print("  ./make tar")
        print("  ./make tar --with-binaries")
        return 0
    with_binaries = "--with-binaries" in args or env_flag("MAKE_TAR_WITH_BINARIES", False)
    DIST_DIR.mkdir(parents=True, exist_ok=True)
    package_name = f"{PROJECT.name}-static" if with_binaries else f"{PROJECT.name}-source"
    package_dir = DIST_DIR / package_name
    shutil.rmtree(package_dir, ignore_errors=True)
    package_dir.mkdir(parents=True, exist_ok=True)
    log("TAR", f"package {package_name}", "1;36")
    if not (VENDOR_DIR / "env.sh").exists():
        step("tar: building vendor first")
        run_vendor([], jobs)
    log("VENDOR", f"using {rel(VENDOR_DIR)}")
    copytree_replace(VENDOR_DIR, package_dir / "build" / "vendor")
    if with_binaries:
        log("STATIC", "building portable binary folder")
        run_static(["bin"], jobs)
        copytree_replace(STATIC_DIR, package_dir / "build" / "static")
    else:
        log("STATIC", "source package only; pass --with-binaries to include build/static")
    copy_source_tree(package_dir)
    write_agent_build(package_dir, with_binaries)
    write_manifest(package_dir, {"with_binaries": with_binaries, "dependency_report": dependency_report()})
    archive = make_tar_gz(DIST_DIR / package_name, DIST_DIR, package_name)
    ok(f"tar ready: {rel(archive)}")
    return 0


### Doctor and env

def run_doctor(_args: list[str], _jobs: int) -> int:
    dep = dependency_report()
    print(c("1;36", f"{PROJECT.name} build doctor"))
    print_section("Layout")
    print_row("root", None, str(ROOT), good=True)
    print_row("Makefile", None, rel(makefile_path()), good=makefile_path().exists())
    print_row("build dir", None, rel(BUILD_ROOT), good=True)
    print("")
    print_section("Required tools")
    checks = [
        ("python3", sys.executable, True),
        ("make", system_make(), bool(system_make())),
        ("cc", os.environ.get("CC") or which("cc") or which("gcc") or which("clang"), bool(os.environ.get("CC") or which("cc") or which("gcc") or which("clang"))),
        ("target", PROJECT.target or "-", bool(PROJECT.target)),
    ]
    bad = False
    for name, value, good in checks:
        print_row(name, None, value, good=good)
        bad = bad or not good
    print("")
    print_section("Common optional tools")
    for name in ("git", "tar", "gzip", "ldd", "readelf", "pkg-config", "llvm-config"):
        value = which(name) if name != "llvm-config" else str(dep["llvm_config"] or "")
        print_row(name, None, value or "-", good=bool(value))
    print("")
    print_section("Detected project")
    print_row("name", None, PROJECT.name)
    print_row("target", None, PROJECT.target or "-")
    print_row("runtime lib", None, PROJECT.runtime_lib or "-")
    print_row("link libs", None, ", ".join(dep["link_libraries"]) or "-")
    print_row("pkg-config", None, ", ".join(dep["pkg_config_packages"]) or "-")
    print_row("vendor", None, rel(VENDOR_DIR))
    print_row("static", None, rel(STATIC_DIR))
    if LOADED_CONFIGS:
        print_row("config", None, "; ".join(rel(p) for p in LOADED_CONFIGS))
    print("")
    if bad:
        err("doctor found missing required pieces")
        return 1
    ok("doctor passed")
    return 0


def run_env(args: list[str], _jobs: int) -> int:
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
        "SYSTEM_MAKE": system_make(),
        "DEPENDENCIES": dependency_report(),
    }
    if args and args[0] == "--json":
        print(json.dumps(data, indent=2, sort_keys=True))
    else:
        for k, v in data.items():
            print(f"{k}={json.dumps(v, sort_keys=True) if isinstance(v, (dict, list)) else v}")
    return 0


### Help

def print_help() -> None:
    print(c("1;36", f"{PROJECT.name} build tool"))
    print(f"{c('1', 'Usage:')} {c('1;32', './make')} {c('36', '<command>')} {c('32', '[options]')}")
    print("")
    groups = (
        ("Build", (("all", "run Makefile all target"), ("release", "run Makefile release target"), ("debug", "run Makefile debug target or all"), ("asan", "run Makefile asan target"), ("test", "run Makefile test target"), ("clean", "run Makefile clean target"))),
        ("Portable", (("vendor", "copy compiler binary + ldd closure into build/vendor"), ("vendor <cmd> [dir]", "copy arbitrary command like the bash concept"), ("static bin", "build build/static portable folder"), ("bin-static", "alias for static bin"), ("tar", f"create build/dist/{PROJECT.name}-source.tar.gz"), ("tar --with-binaries", f"create build/dist/{PROJECT.name}-static.tar.gz"))),
        ("Info", (("doctor", "check tools and detected dependencies"), ("env", "print resolved environment"), ("targets", "list Makefile targets"), ("help", "show help"))),
    )
    for title, rows in groups:
        print(c("1", title + ":"))
        width = max(len(k) for k, _ in rows) + 2
        for name, desc in rows:
            print(f"  {name:<{width}} {desc}")
        print("")
    print(c("1", "Env:"))
    print("  MAKE_TARGET=name                    override binary target")
    print("  MAKE_PROJECT_NAME=name              override package name")
    print("  MAKE_VENDOR_SOURCE_DIRS=a:b         dependency source trees to include")
    print("  MAKE_VENDOR_SOURCE_ARCHIVES=a:b     dependency source archives to include")
    print("  MAKE_VENDOR_FETCH_SOURCES=1         try apt-get source for detected deps")
    print("  MAKE_VENDOR_NO_SYSTEM_LIBS=1        omit libc/libm/etc from rootfs copy")


def list_make_targets(_args: list[str], _jobs: int) -> int:
    for t in sorted(make_targets()):
        print(t)
    return 0


### CLI

CommandFunc = Callable[[list[str], int], int]


def command_all(args: list[str], jobs: int) -> int:
    run_make(["all"] if make_has_target("all") else None, jobs=jobs, extra=args)
    return 0


def command_make_target(target: str) -> CommandFunc:
    def inner(args: list[str], jobs: int) -> int:
        t = target
        if target == "release" and not make_has_target("release"):
            t = PROJECT.build_target or "all"
        if target == "debug" and not make_has_target("debug"):
            t = PROJECT.build_target or "all"
        if target in ("asan", "test") and not make_has_target(target):
            die(f"Makefile has no {target} target")
        run_make([t], jobs=jobs, extra=args)
        return 0
    return inner


def command_help(_args: list[str], _jobs: int) -> int:
    print_help()
    return 0


def command_bin_static(args: list[str], jobs: int) -> int:
    return run_static(["bin", *args], jobs)


COMMANDS: dict[str, CommandFunc] = {
    "help": command_help,
    "doctor": run_doctor,
    "env": run_env,
    "targets": list_make_targets,
    "vendor": run_vendor,
    "static": run_static,
    "bin-static": command_bin_static,
    "tar": run_tar,
    "all": command_all,
    "release": command_make_target("release"),
    "debug": command_make_target("debug"),
    "asan": command_make_target("asan"),
    "test": command_make_target("test"),
    "clean": command_make_target("clean"),
    "install": command_make_target("install"),
    "uninstall": command_make_target("uninstall"),
}


def parse(argv: list[str]) -> tuple[argparse.Namespace, list[str]]:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--help", "-h", action="store_true")
    parser.add_argument("--version", action="store_true")
    parser.add_argument("--color", nargs="?", const="always", default=None)
    parser.add_argument("--no-color", action="store_true")
    parser.add_argument("-j", "--jobs", type=int, default=int(os.environ.get("MAKE_JOBS", "0") or "0"))
    global_args: list[str] = []
    rest: list[str] = []
    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg == "--":
            rest = argv[i + 1:]
            break
        if arg in ("--help", "-h", "--version", "--no-color"):
            global_args.append(arg)
            i += 1
            continue
        if arg in ("--color", "-j", "--jobs"):
            global_args.append(arg)
            if i + 1 < len(argv):
                global_args.append(argv[i + 1])
                i += 2
            else:
                i += 1
            continue
        if arg.startswith("--color=") or arg.startswith("--jobs="):
            global_args.append(arg)
            i += 1
            continue
        rest = argv[i:]
        break
    else:
        rest = []
    ns = parser.parse_args(global_args)
    return ns, rest


def main(argv: list[str] | None = None) -> int:
    global COLOR
    ns, rest = parse(list(sys.argv[1:] if argv is None else argv))
    mode = "never" if ns.no_color else parse_color_mode(ns.color)
    COLOR = color_enabled(mode)
    if ns.version:
        print(f"{PROJECT.name} build tool")
        return 0
    if ns.help or not rest:
        print_help()
        return 0
    jobs = ns.jobs if ns.jobs > 0 else (os.cpu_count() or 1)
    cmd, args = rest[0], rest[1:]
    try:
        if cmd in COMMANDS:
            return COMMANDS[cmd](args, jobs)
        if make_has_target(cmd):
            run_make([cmd], jobs=jobs, extra=args)
            return 0
        die(f"unknown command '{cmd}'. Run ./make help or ./make targets.")
    except subprocess.CalledProcessError as exc:
        return int(exc.returncode or 1)


if __name__ == "__main__":
    raise SystemExit(main())
