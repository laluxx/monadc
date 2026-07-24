import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from monad_binary import resolve_monad_binary


MONAD = resolve_monad_binary()


class ReplCacheTests(unittest.TestCase):
    def run_repl(self, home: Path, extra_env=None):
        env = os.environ.copy()
        env["HOME"] = str(home)
        env["MONAD_NO_PROMPT"] = "1"
        env["MONAD_CACHE_TRACE"] = "1"
        if extra_env:
            env.update(extra_env)
        return subprocess.run(
            [str(MONAD), "repl"],
            input="id 3\n",
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
            timeout=20,
        )

    def test_content_addressed_repl_cache_hits_and_recovers_from_corruption(self):
        with tempfile.TemporaryDirectory(prefix="monadc-repl-cache-") as td:
            home = Path(td)

            cold = self.run_repl(home)
            self.assertEqual(cold.returncode, 0, cold.stdout)
            self.assertIn("[monad-cache] store Function", cold.stdout)
            self.assertTrue(cold.stdout.rstrip().endswith("3"), cold.stdout)

            artifacts = list(
                (home / ".cache" / "monad" / "repl" / "v1").glob(
                    "Function-*.so"
                )
            )
            self.assertEqual(len(artifacts), 1)

            warm = self.run_repl(home)
            self.assertEqual(warm.returncode, 0, warm.stdout)
            self.assertIn("[monad-cache] hit Function", warm.stdout)
            self.assertTrue(warm.stdout.rstrip().endswith("3"), warm.stdout)

            artifacts[0].write_bytes(b"corrupt")
            repaired = self.run_repl(home)
            self.assertEqual(repaired.returncode, 0, repaired.stdout)
            self.assertIn("[monad-cache] repair Function", repaired.stdout)
            self.assertIn("[monad-cache] store Function", repaired.stdout)
            self.assertTrue(repaired.stdout.rstrip().endswith("3"), repaired.stdout)
            self.assertGreater(artifacts[0].stat().st_size, len(b"corrupt"))

    def test_concurrent_cold_repls_publish_one_complete_artifact(self):
        with tempfile.TemporaryDirectory(prefix="monadc-repl-cache-race-") as td:
            home = Path(td)
            env = os.environ.copy()
            env["HOME"] = str(home)
            env["MONAD_NO_PROMPT"] = "1"
            env["MONAD_CACHE_TRACE"] = "1"

            processes = [
                subprocess.Popen(
                    [str(MONAD), "repl"],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    env=env,
                    text=True,
                )
                for _ in range(6)
            ]
            results = [
                process.communicate("id 11\n", timeout=30)[0]
                for process in processes
            ]

            for process, output in zip(processes, results):
                self.assertEqual(process.returncode, 0, output)
                self.assertTrue(output.rstrip().endswith("11"), output)

            artifacts = list(
                (home / ".cache" / "monad" / "repl" / "v1").glob(
                    "Function-*.so"
                )
            )
            self.assertEqual(len(artifacts), 1)
            self.assertGreater(artifacts[0].stat().st_size, 4096)

    def test_xdg_location_and_explicit_opt_out_are_respected(self):
        with tempfile.TemporaryDirectory(prefix="monadc-repl-cache-config-") as td:
            root = Path(td)
            xdg = root / "xdg"
            cached = self.run_repl(
                root / "home", {"XDG_CACHE_HOME": str(xdg)}
            )
            self.assertEqual(cached.returncode, 0, cached.stdout)
            self.assertEqual(
                len(list((xdg / "monad" / "repl" / "v1").glob("Function-*.so"))),
                1,
            )

            disabled_home = root / "disabled-home"
            disabled = self.run_repl(
                disabled_home,
                {"MONAD_CACHE": "off", "XDG_CACHE_HOME": str(root / "disabled-xdg")},
            )
            self.assertEqual(disabled.returncode, 0, disabled.stdout)
            self.assertNotIn("[monad-cache]", disabled.stdout)
            self.assertFalse((root / "disabled-xdg" / "monad" / "repl").exists())


if __name__ == "__main__":
    unittest.main()
