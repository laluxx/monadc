from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(name: str) -> str:
    return (ROOT / name).read_text(encoding="utf-8")


class ReadmeProductTests(unittest.TestCase):
    def test_readme_presents_human_facing_product_structure(self):
        readme = read("README.md")

        required_sections = [
            "## Current Status",
            "## Quick Start",
            "## Windows / MSYS2",
            "## First Program",
            "## Package Builds",
            "## Build Targets",
            "## Verification",
            "## Project Notes",
        ]
        for section in required_sections:
            self.assertIn(section, readme)

        self.assertIn("CMake", readme)
        self.assertIn("MSYS2/UCRT64", readme)
        self.assertIn("monad build", readme)
        self.assertIn("package.yaml", readme)

    def test_readme_does_not_reference_obsolete_windows_workflow(self):
        readme = read("README.md")

        self.assertIn(".github/workflows/ci.yml", readme)
        self.assertNotIn("windows-msys2.yml", readme)
        self.assertNotIn("The root `Makefile` is the primary build path", readme)

    def test_ci_uses_current_actions_and_msys2_recommended_environment(self):
        workflow = read(".github/workflows/ci.yml")

        self.assertIn("actions/checkout@v7", workflow)
        self.assertIn("actions/upload-artifact@v7", workflow)
        self.assertIn("msys2/setup-msys2@v2", workflow)
        self.assertIn("msystem: UCRT64", workflow)
        self.assertIn("pacboy:", workflow)


if __name__ == "__main__":
    unittest.main()
