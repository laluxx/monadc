#include "completion.h"
#include <stdio.h>
#include <string.h>

/* ANSI */
#define RESET   "\x1b[0m"
#define BOLD    "\x1b[1m"
#define DIM     "\x1b[2m"
#define CYAN    "\x1b[36m"
#define YELLOW  "\x1b[33m"
#define MAGENTA "\x1b[35m"
#define GREEN   "\x1b[32m"
#define WHITE   "\x1b[97m"

/* ------------------------------------------------------------------ */
/*  Layout constants                                                   */
/* ------------------------------------------------------------------ */

/* Width of the left name column (padded with spaces). */
#define COL_NAME  20
/* Width of the args/hint column between name and description. */
#define COL_ARGS  16

/* ------------------------------------------------------------------ */
/*  Primitive renderers                                                */
/* ------------------------------------------------------------------ */

/* One completion row:  COLOR  name  DIM  desc */
static void row(const char *color, const char *name, const char *desc)
{
    fprintf(stderr, "    %s%-*s" RESET "  " DIM "%s" RESET "\n",
            color, COL_NAME, name, desc);
}

/* One row with a separate args hint column:
   COLOR name   DIM args   RESET  desc  */
static void row3(const char *color, const char *name,
                 const char *args,  const char *desc)
{
    char namebuf[64];
    if (args && args[0]) {
        snprintf(namebuf, sizeof(namebuf), "%s %s", name, args);
    } else {
        snprintf(namebuf, sizeof(namebuf), "%s", name);
    }
    fprintf(stderr, "    %s%-*s" RESET "  " DIM "%s" RESET "\n",
            color, COL_NAME + COL_ARGS, namebuf, desc);
}

/* Section header — uppercase, dim, with a rule underneath. */
static void section(const char *title)
{
    fprintf(stderr, "\n  " DIM "%s" RESET "\n", title);
}

/* Blank line inside the menu. */
static void gap(void) { fprintf(stderr, "\n"); }

/* ------------------------------------------------------------------ */
/*  Top-level usage menu                                               */
/* ------------------------------------------------------------------ */

void print_usage(const char *prog)
{
    /* Header */
    fprintf(stderr,
        "\n" BOLD "  Monad" RESET "  " DIM "v0.1" RESET "\n\n"
        "  " DIM "Usage:" RESET "  %s " CYAN "<subcommand>" RESET
        " " YELLOW "[options]" RESET " " DIM "[file]" RESET "\n",
        prog);

    /* Subcommands */
    section("subcommands");
    row3(CYAN, "new",     "<name>",     "Create a new Monad package");
    row3(CYAN, "build",   "",           "Build the project (reads package.yaml)");
    row3(CYAN, "run",     "",           "Build and run the project");
    row3(CYAN, "clean",   "",           "Remove build artefacts");
    row3(CYAN, "install", "",           "Install binary to ~/.local/bin");
    row3(CYAN, "test",    "<file.mon>", "Run tests (build, run, delete binary)");
    row3(CYAN, "check",   "[file.mon]", "Type-check without producing a binary");
    row3(CYAN, "lsp",     "",           "Start the Language Server (LSP 3.17)");

    /* Flags */
    section("options");
    row(YELLOW, "-i",            "Start interactive REPL");
    row(YELLOW, "-o <file>",     "Output file name");
    row(YELLOW, "--emit-ir",     "Emit LLVM IR (.ll)");
    row(YELLOW, "--emit-bc",     "Emit LLVM bitcode (.bc)");
    row(YELLOW, "--emit-asm",    "Emit assembly (.s)");
    row(YELLOW, "--emit-obj",    "Emit object file (.o)");
    row(YELLOW, "--emit-json",   "Emit AST as JSON (.json)");
    row(YELLOW, "--emit-typst",  "Emit Typst document (.typ) and compile to PDF");
    row(YELLOW, "--test",        "Embed tests in binary (keep binary)");
    row(YELLOW, "-Wall",         "Enable all warnings");
    row(YELLOW, "-Wextra",       "Enable extra warnings");
    row(YELLOW, "-h, --help",    "Show this help");

    /* Hint */
    gap();
    fprintf(stderr,
        "  " DIM "Run " RESET CYAN "monad help <subcommand>" RESET
        DIM " for detailed options per command." RESET "\n\n");
}

/* ------------------------------------------------------------------ */
/*  Per-subcommand menus                                               */
/* ------------------------------------------------------------------ */

static void menu_lsp(void)
{
    fprintf(stderr,
        "\n" BOLD "  monad lsp" RESET "  "
        DIM "v0.1  Language Server Protocol 3.17" RESET "\n"
        "  " DIM "Communicates over stdin/stdout using JSON-RPC 2.0\n"
        "  Launched by your editor, not by hand." RESET "\n");

    section("subcommands");
    row(CYAN, "repl", "Start the interactive LSP test REPL");

    section("editor setup");
    row(MAGENTA, "VS Code",  "serverOptions: { command: \"monad\", args: [\"lsp\"] }");
    row(MAGENTA, "Neovim",   "cmd = { \"monad\", \"lsp\" }");
    row(MAGENTA, "Emacs",    ":command '(\"monad\" \"lsp\")");
    row(MAGENTA, "Helix",    "command = \"monad\"  args = [\"lsp\"]");

    section("features");
    row(GREEN, "diagnostics",     "errors and warnings in real time");
    row(GREEN, "completion",      "symbol and keyword completion");
    row(GREEN, "hover",           "type info on hover");
    row(GREEN, "go-to-def",       "jump to definition");
    row(GREEN, "references",      "find all usages");
    row(GREEN, "rename",          "project-wide rename");
    row(GREEN, "inlay hints",     "inline type annotations");
    row(GREEN, "semantic tokens", "rich syntax highlighting");
    row(GREEN, "call hierarchy",  "callers and callees");
    row(GREEN, "folding ranges",  "code folding support");

    section("test manually");
    fprintf(stderr,
        "    " DIM "printf 'Content-Length: 47\\r\\n\\r\\n"
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}'"
        " | monad lsp" RESET "\n\n");
}

static void menu_new(void)
{
    fprintf(stderr,
        "\n" BOLD "  monad new" RESET "  " MAGENTA "<name>" RESET "\n"
        "  " DIM "Scaffold a new Monad package in the current directory." RESET "\n");

    section("creates");
    row(MAGENTA, "<name>/src/Main.mon", "entry point with (module Main)");
    row(MAGENTA, "<name>/package.yaml", "project manifest");
    row(MAGENTA, "<name>/README.md",    "project readme");
    row(MAGENTA, "<name>/.gitignore",   "ignores build/ *.o *.ll *.s");

    section("reads from git config");
    row(CYAN, "user.name",  "set as author in package.yaml");
    row(CYAN, "user.email", "set as maintainer in package.yaml");

    gap();
    fprintf(stderr,
        "  " DIM "then run:" RESET "  "
        CYAN "cd <name> && monad run" RESET "\n\n");
}

static void menu_build(void)
{
    fprintf(stderr,
        "\n" BOLD "  monad build" RESET "\n"
        "  " DIM "Reads package.yaml, compiles to build/<exe>." RESET "\n");

    section("emit flags");
    row(YELLOW, "--emit-ir",    "LLVM IR (.ll)");
    row(YELLOW, "--emit-bc",    "LLVM bitcode (.bc)");
    row(YELLOW, "--emit-asm",   "assembly (.s)");
    row(YELLOW, "--emit-obj",   "object file (.o)");
    row(YELLOW, "--emit-json",  "AST as JSON (.json)");
    row(YELLOW, "--emit-typst", "Typst document + PDF");
    row(YELLOW, "-o <file>",    "output file name");
    row(YELLOW, "-Wall",        "enable all warnings");
    row(YELLOW, "-Wextra",      "enable extra warnings");

    section("reads from package.yaml");
    row(CYAN, "name",            "package name");
    row(CYAN, "executables",     "exe name, main file, source-dirs");
    row(CYAN, "monad-options",   "flags prepended to every build");
    row(CYAN, "dependencies",    "linked libraries");

    section("output");
    row(DIM,  "build/<exe>", "binary placed in build/ directory");

    gap();
}

static void menu_run(void)
{
    fprintf(stderr,
        "\n" BOLD "  monad run" RESET "\n"
        "  " DIM "Build and immediately run the project binary." RESET "\n");

    section("same flags as build");
    row(YELLOW, "--emit-ir",    "LLVM IR (.ll)");
    row(YELLOW, "--emit-bc",    "LLVM bitcode (.bc)");
    row(YELLOW, "--emit-asm",   "assembly (.s)");
    row(YELLOW, "--emit-obj",   "object file (.o)");
    row(YELLOW, "-o <file>",    "output file name");

    section("sequence");
    row(GREEN, "1. build", "compiles via package.yaml");
    row(GREEN, "2. run",   "executes build/<exe> immediately");

    gap();
}

static void menu_clean(void)
{
    fprintf(stderr,
        "\n" BOLD "  monad clean" RESET "\n"
        "  " DIM "Remove build artefacts from the project." RESET "\n");

    section("removes");
    row(CYAN, "build/",   "entire build output directory");
    row(CYAN, "*.o",      "object files in source-dirs");
    row(CYAN, "*.ll",     "LLVM IR files in source-dirs");
    row(CYAN, "*.s",      "assembly files in source-dirs");

    gap();
}

static void menu_install(void)
{
    fprintf(stderr,
        "\n" BOLD "  monad install" RESET "\n"
        "  " DIM "Install the project binary to ~/.local/bin." RESET "\n");

    section("sequence");
    row(GREEN, "1. build",         "compiles if binary missing");
    row(GREEN, "2. mkdir",         "ensures ~/.local/bin exists");
    row(GREEN, "3. install -m755", "copies binary with correct perms");

    section("notes");
    row(DIM, "PATH", "warns if ~/.local/bin is not in $PATH");

    gap();
}

static void menu_test(void)
{
    fprintf(stderr,
        "\n" BOLD "  monad test" RESET "  " MAGENTA "<file.mon>" RESET "\n"
        "  " DIM "Compile with tests embedded, run, then delete binary." RESET "\n");

    section("sequence");
    row(GREEN, "1. compile",  "builds <base>_test binary with --test");
    row(GREEN, "2. run",      "executes the test binary");
    row(GREEN, "3. cleanup",  "removes _test binary and .o");

    section("flags");
    row(YELLOW, "--test <file>", "embed tests but keep the binary");

    section("exit codes");
    row(DIM, "0", "all tests passed");
    row(DIM, "1", "test binary signalled (assertion failed)");

    gap();
}

static void menu_check(void)
{
    fprintf(stderr,
        "\n" BOLD "  monad check" RESET "  " DIM "[file.mon]" RESET "\n"
        "  " DIM "Type-check without producing a binary. Exit 0 = ok." RESET "\n");

    section("modes");
    row(CYAN, "monad check",          "checks the project main file");
    row(CYAN, "monad check file.mon", "checks a single file only");

    section("editor integration");
    row(GREEN, "flycheck",  "exit code drives pass/fail");
    row(GREEN, "neovim",    "use with null-ls or nvim-lint");
    row(GREEN, "emacs",     "use with flymake or flycheck");

    section("exit codes");
    row(DIM, "0", "no type errors");
    row(DIM, "1", "type errors found");

    gap();
}

static void menu_help(const char *subcmd)
{
    /* monad help <subcmd> — recurse into the right menu */
    if (subcmd) {
        print_subcommand_menu(subcmd);
        return;
    }
    /* monad help with no argument — same as monad --help */
    print_usage("monad");
}

/* ------------------------------------------------------------------ */
/*  Dispatch                                                           */
/* ------------------------------------------------------------------ */

void print_subcommand_menu(const char *subcmd)
{
    if      (!subcmd)                        return;
    else if (strcmp(subcmd, "lsp")     == 0) menu_lsp();
    else if (strcmp(subcmd, "new")     == 0) menu_new();
    else if (strcmp(subcmd, "build")   == 0) menu_build();
    else if (strcmp(subcmd, "run")     == 0) menu_run();
    else if (strcmp(subcmd, "clean")   == 0) menu_clean();
    else if (strcmp(subcmd, "install") == 0) menu_install();
    else if (strcmp(subcmd, "test")    == 0) menu_test();
    else if (strcmp(subcmd, "check")   == 0) menu_check();
    else if (strcmp(subcmd, "help")    == 0) menu_help(NULL);
    else {
        /* Unknown subcommand passed to help */
        fprintf(stderr,
            "\n  " DIM "No detailed help for '" RESET "%s" DIM "'." RESET
            "  Showing top-level menu.\n", subcmd);
        print_usage("monad");
    }
}
