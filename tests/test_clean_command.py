"""Standalone `monad clean` removes only compiler-owned local artifacts."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class CleanCommandTests(unittest.TestCase):
    def test_standalone_clean_preserves_sources_and_removes_generated_files(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            preserved = {
                "Chip8.mon": "module Chip8 []\n",
                "Display.mon": "module Display []\n",
                "README.org": "#+TITLE: Chip8\n",
                "notes.txt": "not a compiler artifact\n",
            }
            for name, contents in preserved.items():
                (work / name).write_text(contents)

            generated = [
                "Chip8", "Chip8.mqti", "Display.module.mqti",
                "Display.module.o", "Chip8.o", "Chip8.ll", "Chip8.s",
            ]
            for name in generated:
                path = work / name
                path.write_text("generated\n")
            os.chmod(work / "Chip8", 0o755)
            unrelated_executable = work / "rom-tool"
            unrelated_executable.write_text("#!/bin/sh\n")
            os.chmod(unrelated_executable, 0o755)

            cleaned = subprocess.run(
                [str(ROOT / "monad"), "clean"], cwd=work,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )

            self.assertEqual(cleaned.returncode, 0, cleaned.stderr)
            self.assertIn("standalone", cleaned.stdout.lower())
            self.assertEqual(
                sorted(path.name for path in work.iterdir()),
                sorted([*preserved, "rom-tool"]),
            )


if __name__ == "__main__":
    unittest.main()
