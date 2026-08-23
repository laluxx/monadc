import os
import shutil
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

from monad_binary import resolve_monad_binary, resolve_runtime_archive

ROOT = Path(__file__).resolve().parents[1]
MONAD = resolve_monad_binary()
RUNTIME = resolve_runtime_archive(MONAD)


class ModuleTestArtifactIsolationTests(unittest.TestCase):
    def test_orphan_object_cannot_hide_source_interface_metadata(self):
        with tempfile.TemporaryDirectory(prefix="monadc-orphan-object-") as td:
            work = Path(td)
            home = work / "home"
            core = work / "checkout/core"
            installed_core = work / "install/lib/monad/core"
            compiler = work / "install/bin/monad"
            shutil.copytree(ROOT / "core", core)
            shutil.copytree(ROOT / "core", installed_core)
            compiler.parent.mkdir(parents=True)
            shutil.copy2(MONAD, compiler)
            for artifact in core.rglob("*.module.o"):
                artifact.unlink()
            for interface in core.rglob("*.module.mqti"):
                interface.unlink()
            orphan = core / "prelude/Data/Functor.module.o"
            orphan.write_bytes(b"not a certified module object")
            future = time.time() + 60
            os.utime(orphan, (future, future))
            env = os.environ.copy()
            env.pop("MONAD_CORE", None)
            env.update(HOME=str(home), MONAD_RUNTIME_LIB=str(RUNTIME))
            home.mkdir()
            output = work / "Sequence"
            built = subprocess.run(
                [str(compiler), "Sequence.mon",
                 "-o", str(output)],
                cwd=core / "prelude", env=env,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(built.returncode, 0, built.stdout[-3000:])
            self.assertEqual(orphan.read_bytes(), b"not a certified module object")
            self.assertTrue(list(
                (home / ".cache/monad/core").glob("*Functor.module.mqti")))

    def test_test_objects_are_not_reused_by_normal_builds(self):
        with tempfile.TemporaryDirectory(prefix="monadc-mode-cache-") as td:
            work = Path(td)
            home = work / "home"
            core = work / "core"
            shutil.copytree(ROOT / "core", core)
            angle = core / "Math" / "Angle.mon"
            if not angle.exists():
                angle.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(Path.home() / ".local/lib/monad/core/Math/Angle.mon", angle)
            env = os.environ.copy()
            env.update(HOME=str(home), MONAD_CORE=str(core), MONAD_RUNTIME_LIB=str(RUNTIME))
            home.mkdir()
            tested = subprocess.run([str(MONAD), "test", str(angle)], env=env,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(tested.returncode, 0, tested.stdout[-2000:])
            probe = work / "Probe.mon"
            probe.write_text("import Math.Angle\nmodule Main\nshow (sin-radians 0.0)\n")
            output = work / "Probe"
            built = subprocess.run([str(MONAD), str(probe), "-o", str(output)], env=env,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(built.returncode, 0, built.stdout[-2000:])
            run = subprocess.run([str(output)], env=env, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True)
            self.assertEqual(run.stdout, "0\n")
            cache = home / ".cache/monad/core"
            self.assertTrue(list(cache.glob("*.test.module.o")))
            self.assertTrue(list(cache.glob("*.module.o")))


if __name__ == "__main__":
    unittest.main()
