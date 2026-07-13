from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(name: str) -> str:
    return (ROOT / name).read_text(encoding="utf-8")


class CliFlagDualityTests(unittest.TestCase):
    def test_cli_eval_short_flag_and_word_command_share_eval_code_storage(self):
        cli_c = read("cli.c")
        cli_h = read("cli.h")
        main_c = read("main.c")

        self.assertIn("CMD_EVAL", cli_h)
        self.assertIn("char *eval_code;", cli_h)
        self.assertIn('strcmp(argv[1], "-e") == 0', cli_c)
        self.assertIn('strcmp(argv[1], "eval") == 0', cli_c)
        self.assertIn("flags.eval_code = argv[2];", cli_c)
        self.assertIn("cmd_eval(flags.eval_code);", main_c)

    def test_cli_option_words_exist_for_the_advertised_flag_forms(self):
        cli_c = read("cli.c")
        config_c = read("config.c")

        dual_forms = {
            "--emit-ir": "emit-ir",
            "--emit-bc": "emit-bc",
            "--emit-asm": "emit-asm",
            "--emit-obj": "emit-obj",
            "--emit-json": "emit-json",
            "--emit-typst": "emit-typst",
            "--emit-bytecode": "emit-bytecode",
            "--bytecode-verify": "bytecode-verify",
            "--bytecode-disassemble": "bytecode-disassemble",
            "--bytecode-decompile": "bytecode-decompile",
            "--bytecode-sections": "bytecode-sections",
            "--bytecode-trace": "bytecode-trace",
            "--bytecode-baseline-jit": "bytecode-baseline-jit",
            "-jit": "jit",
            "-S": "asm",
            "-c": "obj",
            "-o": "output",
            "-O": "optimize",
            "-v": "verbose",
            "--quiet": "quiet",
            "--trace=": "trace",
            "--test": "test",
            "-i": "repl",
            "-e": "eval",
        }

        for flag, word in dual_forms.items():
            self.assertIn(flag, cli_c)
            self.assertIn(word, cli_c)
            if flag not in {"-e", "--trace=", "-o"}:
                self.assertIn(flag, config_c)
                self.assertIn(word, config_c)

    def test_cli_common_options_can_prefix_real_subcommands(self):
        cli_c = read("cli.c")
        main_c = read("main.c")

        self.assertIn("is_dispatch_word", cli_c)
        self.assertIn("monad verbose test file.mon", cli_c)
        self.assertIn("normalized_argv[0] = argv[0];", cli_c)
        self.assertIn('strcmp(argv[1], "test") == 0', cli_c)
        self.assertIn("Unknown test flag", cli_c)
        self.assertIn("is_test_suite_word", cli_c)
        self.assertIn("flags.test_suite", cli_c)
        self.assertIn("python3 tests/main.py %s", cli_c)
        self.assertIn("cmd_test(&flags);", main_c)

    def test_usage_menu_lists_flag_word_duality_and_eval_help(self):
        completion_c = read("completion.c")

        self.assertIn("eval", completion_c)
        self.assertIn("-e <code>, eval <code>", completion_c)
        self.assertIn("--emit-ir, emit-ir", completion_c)
        self.assertIn("--emit-bytecode, emit-bytecode", completion_c)
        self.assertIn("--bytecode-disassemble, bytecode-disassemble", completion_c)
        self.assertIn("-jit, jit", completion_c)
        self.assertIn("monad jit file.mon", completion_c)
        self.assertIn("monad -jit file.mon", completion_c)
        self.assertIn("monad test [list|runner|windows|how-to|file.mon]", completion_c)
        self.assertIn("Suites: list, runner, windows, how-to, cmake, readme, bytecode, core, all", completion_c)
        self.assertIn("-S, asm", completion_c)
        self.assertIn("-c, obj", completion_c)
        self.assertIn("-O, optimize", completion_c)


if __name__ == "__main__":
    unittest.main()
