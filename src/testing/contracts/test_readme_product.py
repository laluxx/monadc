from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]


def read(name: str) -> str:
    return (ROOT / name).read_text(encoding="utf-8")


class ReadmeProductTests(unittest.TestCase):
    def test_readme_documents_the_canonical_repository_contract(self):
        readme = read("README.md")
        required_sections = [
            "## Quick Start",
            "## Repository Layout",
            "## Building",
            "## Editor / clangd Support",
            "## Tests",
            "## Clean-Tree Policy",
            "## Quality Gate",
            "## Source Archives",
            "## Windows / MSYS2",
        ]
        for section in required_sections:
            self.assertIn(section, readme)
        self.assertIn("build/bin/monad", readme)
        self.assertIn("tests/", readme)
        self.assertIn(".mon", readme)
        self.assertIn("src/tooling/", readme)
        self.assertIn("src/testing/", readme)

    def test_readme_exposes_public_frontends_and_no_deleted_test_scripts(self):
        readme = read("README.md")
        self.assertIn("./make test", readme)
        self.assertIn("./make compdb", readme)
        self.assertIn("./make clean --check", readme)
        self.assertIn("./make tar --with-context", readme)
        self.assertIn("monad test list", readme)
        self.assertNotIn("tests/main.py", readme)
        self.assertNotIn("tests/run.py", readme)
        self.assertNotIn("Python test harnesses", readme)

    def test_readme_documents_compilation_database_policy(self):
        readme = read("README.md")
        self.assertIn("build/compile_commands.json", readme)
        self.assertIn("Bear", readme)
        self.assertIn("clangd", readme)
        self.assertIn("MAKE_COMPDB=0", readme)

    def test_ci_uses_current_actions_and_msys2_recommended_environment(self):
        workflow = read(".github/workflows/ci.yml")
        self.assertIn("actions/checkout@v7", workflow)
        self.assertIn("actions/upload-artifact@v7", workflow)
        self.assertIn("msys2/setup-msys2@v2", workflow)
        self.assertIn("msystem: UCRT64", workflow)
        self.assertIn("pacboy:", workflow)


if __name__ == "__main__":
    unittest.main()
