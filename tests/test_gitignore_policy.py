import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class GitignorePolicyTests(unittest.TestCase):
    def ignored(self, path):
        result = subprocess.run(
            ["git", "check-ignore", "--no-index", "--quiet", path],
            cwd=ROOT,
            check=False,
        )
        return result.returncode == 0

    def test_generated_compiler_artifacts_are_ignored(self):
        generated = [
            ".dryc/cache/anchors-v1/example.anc",
            "core/prelude/Sequence.json",
            "core/prelude/Data/Char.json",
            "how_to/TcpServer",
            "libmonad-compiler.so",
            "core_text_parser_combinators",
        ]
        for path in generated:
            with self.subTest(path=path):
                self.assertTrue(self.ignored(path))

    def test_sources_remain_visible(self):
        for path in ["how_to/TcpServer.mon", "core/Text/Parser.mon"]:
            with self.subTest(path=path):
                self.assertFalse(self.ignored(path))


if __name__ == "__main__":
    unittest.main()
