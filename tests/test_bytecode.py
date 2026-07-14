import platform
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
import os
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def host_supports_baseline_jit() -> bool:
    machine = platform.machine().lower()
    return (
        machine in {"x86_64", "amd64"}
        and sys.platform.startswith("linux")
    )


class BytecodeModuleTests(unittest.TestCase):
    def compile_and_run(self, source: str) -> subprocess.CompletedProcess:
        with tempfile.TemporaryDirectory() as td:
            harness = Path(td) / "bytecode_harness.c"
            exe = Path(td) / "bytecode_harness"
            harness.write_text(source, encoding="utf-8")

            subprocess.run(
                [
                    "gcc",
                    "-std=c99",
                    "-Wall",
                    "-Wextra",
                    "-iquote",
                    str(ROOT),
                    str(ROOT / "bytecode.c"),
                    str(harness),
                    "-o",
                    str(exe),
                ],
                check=True,
                cwd=ROOT,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

            return subprocess.run(
                [str(exe)],
                check=False,
                cwd=ROOT,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

    def test_register_verifier_sections_debug_and_metadata(self):
        """TEST-ID: tests.bytecode.register-core-contract
        TEST-CONTEXT: monadc.context.bytecode.core
        TEST-PURPOSE: register bytecode verifies linearly, executes, serializes sections, and exposes debug/deopt/IC hooks.
        TEST-EXPECT: c-unit
        TEST-TIER: regression
        TEST-STATUS: active
        TEST-COVERAGE: bytecode.h, bytecode.c
        """
        harness = textwrap.dedent(
            r'''
            #include "bytecode.h"
            #include <stdint.h>
            #include <stdio.h>
            #include <string.h>

            static uint64_t read_leb(FILE *f) {
                uint64_t result = 0;
                unsigned shift = 0;
                for (;;) {
                    int ch = fgetc(f);
                    if (ch == EOF) return UINT64_MAX;
                    result |= (uint64_t)(ch & 0x7f) << shift;
                    if (!(ch & 0x80)) return result;
                    shift += 7;
                }
            }

            int main(void) {
                BcProgram program;
                bc_program_init(&program, "answer");

                uint32_t c2 = bc_program_add_const(&program, bc_value_i64(2));
                uint32_t c40 = bc_program_add_const(&program, bc_value_i64(40));
                bc_emit_const(&program, 0, c2, bc_span("<test>", 1, 1));
                bc_emit_const(&program, 1, c40, bc_span("<test>", 1, 5));
                bc_emit_abc(&program, BC_OP_ADD, 2, 0, 1, bc_span("<test>", 1, 8));
                bc_emit(&program, (BcInstr){BC_OP_BOOL, 3, 0, 0, 1}, bc_span("<test>", 1, 10));
                bc_emit(&program, (BcInstr){BC_OP_IF, 3, 0, 0, 0}, bc_span("<test>", 1, 11));
                bc_emit_abc(&program, BC_OP_MOV, 4, 2, 0, bc_span("<test>", 1, 12));
                bc_emit(&program, (BcInstr){BC_OP_ELSE, 0, 0, 0, 0}, bc_span("<test>", 1, 13));
                bc_emit_const(&program, 4, c2, bc_span("<test>", 1, 14));
                bc_emit(&program, (BcInstr){BC_OP_END, 0, 0, 0, 0}, bc_span("<test>", 1, 15));
                bc_emit_return(&program, 4, bc_span("<test>", 1, 16));
                bc_program_add_inline_cache(&program, 2);
                bc_program_add_deopt_point(&program, 2, 0, 5, 99);

                BcError error;
                if (!bc_verify(&program, &error)) {
                    fprintf(stderr, "verify failed: %s\n", error.message);
                    return 1;
                }

                BcVM vm;
                bc_vm_init(&vm);
                BcValue result = bc_value_nil();
                if (!bc_vm_run(&vm, &program, &result, &error)) {
                    fprintf(stderr, "run failed: %s\n", error.message);
                    return 2;
                }
                if (result.kind != BC_VALUE_I64 || result.as.i64 != 42) {
                    fprintf(stderr, "wrong result\n");
                    return 3;
                }

                FILE *bin = tmpfile();
                FILE *dump = tmpfile();
                FILE *decompiled = tmpfile();
                if (!bin || !dump || !decompiled) return 4;
                if (!bc_write_binary(&program, bin, &error)) {
                    fprintf(stderr, "write failed: %s\n", error.message);
                    return 5;
                }
                bc_disassemble(&program, dump);
                bc_decompile_monad(&program, decompiled);
                fflush(bin);
                fflush(dump);
                fflush(decompiled);
                rewind(bin);
                rewind(dump);
                rewind(decompiled);

                if (read_leb(bin) != BC_MAGIC) return 6;
                if (read_leb(bin) != BC_VERSION_MAJOR) return 7;
                if (read_leb(bin) != BC_VERSION_MINOR) return 8;
                if (read_leb(bin) != 6) return 9;
                uint64_t saw_code = 0, saw_const = 0, saw_debug = 0, saw_deopt = 0, saw_ic = 0;
                for (int i = 0; i < 6; i++) {
                    uint64_t id = read_leb(bin);
                    uint64_t size = read_leb(bin);
                    if (id == BC_SECTION_CODE) saw_code = 1;
                    if (id == BC_SECTION_CONSTANTS) saw_const = 1;
                    if (id == BC_SECTION_DEBUG) saw_debug = 1;
                    if (id == BC_SECTION_DEOPT) saw_deopt = 1;
                    if (id == BC_SECTION_IC) saw_ic = 1;
                    fseek(bin, (long)size, SEEK_CUR);
                }
                if (!saw_code || !saw_const || !saw_debug || !saw_deopt || !saw_ic) return 10;

                char dump_buf[2048] = {0};
                char dec_buf[2048] = {0};
                fread(dump_buf, 1, sizeof(dump_buf) - 1, dump);
                fread(dec_buf, 1, sizeof(dec_buf) - 1, decompiled);
                if (!strstr(dump_buf, "bc.add") || !strstr(dump_buf, "bc.if")) {
                    fprintf(stderr, "missing disassembly: %s\n", dump_buf);
                    return 11;
                }
                if (!strstr(dec_buf, "(+ 2 40)") || !strstr(dec_buf, "answer")) {
                    fprintf(stderr, "missing decompile: %s\n", dec_buf);
                    return 12;
                }

                bc_vm_free(&vm);
                bc_program_free(&program);
                return 0;
            }
            '''
        )

        result = self.compile_and_run(harness)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_verifier_rejects_bad_programs_and_jit_boundary_is_explicit(self):
        """TEST-ID: tests.bytecode.verifier-and-jit-boundary
        TEST-CONTEXT: monadc.context.bytecode.core
        TEST-PURPOSE: bytecode verifier rejects malformed register/control programs and baseline JIT reports unsupported cleanly.
        TEST-EXPECT: c-unit
        TEST-TIER: regression
        TEST-STATUS: active
        TEST-COVERAGE: bytecode.h, bytecode.c
        """
        harness = textwrap.dedent(
            r'''
            #include "bytecode.h"
            #include <stdio.h>
            #include <string.h>

            static int expect_reject(BcProgram *program, const char *needle) {
                BcError error;
                if (bc_verify(program, &error)) {
                    fprintf(stderr, "expected verifier rejection\n");
                    return 1;
                }
                if (!strstr(error.message, needle)) {
                    fprintf(stderr, "wrong verifier error: %s\n", error.message);
                    return 2;
                }
                return 0;
            }

            int main(void) {
                BcProgram types;
                bc_program_init(&types, "bad-types");
                uint32_t i = bc_program_add_const(&types, bc_value_i64(1));
                uint32_t b = bc_program_add_const(&types, bc_value_bool(1));
                bc_emit_const(&types, 0, i, bc_span("<bad>", 1, 1));
                bc_emit_const(&types, 1, b, bc_span("<bad>", 1, 3));
                bc_emit_abc(&types, BC_OP_ADD, 2, 0, 1, bc_span("<bad>", 1, 5));
                bc_emit_return(&types, 2, bc_span("<bad>", 1, 8));
                int rc = expect_reject(&types, "type mismatch");
                bc_program_free(&types);
                if (rc) return rc;

                BcProgram control;
                bc_program_init(&control, "bad-control");
                uint32_t one = bc_program_add_const(&control, bc_value_i64(1));
                bc_emit_const(&control, 0, one, bc_span("<bad>", 2, 1));
                bc_emit(&control, (BcInstr){BC_OP_IF, 0, 0, 0, 0}, bc_span("<bad>", 2, 3));
                bc_emit_return(&control, 0, bc_span("<bad>", 2, 6));
                rc = expect_reject(&control, "if condition");
                bc_program_free(&control);
                if (rc) return rc;

                BcProgram jit_program;
                bc_program_init(&jit_program, "jit-boundary");
                uint32_t c = bc_program_add_const(&jit_program, bc_value_i64(7));
                bc_emit_const(&jit_program, 0, c, bc_span("<jit>", 1, 1));
                bc_emit(&jit_program, (BcInstr){BC_OP_LOOP, 0, 0, 0, 0}, bc_span("<jit>", 1, 3));
                bc_emit(&jit_program, (BcInstr){BC_OP_END, 0, 0, 0, 0}, bc_span("<jit>", 1, 4));
                bc_emit_return(&jit_program, 0, bc_span("<jit>", 1, 5));

                BcJitOptions options = bc_jit_options_default();
                BcJitArtifact artifact;
                BcError error;
                if (bc_jit_compile_baseline(&jit_program, &options, &artifact, &error)) {
                    fprintf(stderr, "baseline jit unexpectedly succeeded\n");
                    return 3;
                }
                if (!strstr(error.message, "unsupported") &&
                    !strstr(error.message, "not available")) {
                    fprintf(stderr, "wrong jit error: %s\n", error.message);
                    return 4;
                }
                bc_jit_artifact_free(&artifact);
                bc_program_free(&jit_program);
                return 0;
            }
            '''
        )

        result = self.compile_and_run(harness)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_memory_reserve_stress_and_visual_section_dump(self):
        """TEST-ID: tests.bytecode.memory-and-visual-tooling
        TEST-CONTEXT: monadc.context.bytecode.core
        TEST-PURPOSE: bytecode handles 100000 instructions with explicit memory accounting and dumps/skips binary sections visually.
        TEST-EXPECT: c-unit
        TEST-TIER: regression
        TEST-STATUS: active
        TEST-COVERAGE: bytecode.h, bytecode.c
        """
        harness = textwrap.dedent(
            r'''
            #include "bytecode.h"
            #include <stdint.h>
            #include <stdio.h>
            #include <string.h>

            static void write_leb(FILE *f, uint64_t value) {
                do {
                    unsigned char byte = (unsigned char)(value & 0x7fu);
                    value >>= 7u;
                    if (value) byte |= 0x80u;
                    fputc(byte, f);
                } while (value);
            }

            int main(void) {
                BcProgram program;
                BcError error;
                bc_program_init(&program, "stress");
                bc_program_set_debug_mode(&program, BC_DEBUG_LINES);
                if (!bc_program_reserve(&program, 100001, 1, 1, &error)) {
                    fprintf(stderr, "reserve failed: %s\n", error.message);
                    return 1;
                }
                uint32_t c0 = bc_program_add_const(&program, bc_value_i64(0));
                for (size_t i = 0; i < 100000; i++) {
                    bc_emit_const(&program, 0, c0, bc_span("stress.mon", (uint32_t)(i + 1), 1));
                }
                bc_emit_return(&program, 0, bc_span("stress.mon", 100001, 1));

                BcProgramMemoryStats stats = bc_program_memory_stats(&program);
                if (stats.instruction_count != 100001 || stats.register_count != 1) return 2;
                if (stats.total_bytes == 0 || stats.total_bytes > 3000000u) {
                    fprintf(stderr, "unexpected memory use: %zu\n", stats.total_bytes);
                    return 3;
                }
                if (program.code_capacity != 100001) {
                    fprintf(stderr, "reserve did not prevent code overgrowth: %zu\n", program.code_capacity);
                    return 4;
                }
                bc_program_shrink_to_fit(&program);
                if (program.code_capacity != program.code_count) return 5;
                if (!bc_verify(&program, &error)) {
                    fprintf(stderr, "stress verify failed: %s\n", error.message);
                    return 6;
                }
                bc_program_free(&program);

                FILE *unknown = tmpfile();
                FILE *visual = tmpfile();
                if (!unknown || !visual) return 7;
                write_leb(unknown, BC_MAGIC);
                write_leb(unknown, BC_VERSION_MAJOR);
                write_leb(unknown, BC_VERSION_MINOR);
                write_leb(unknown, 1);
                write_leb(unknown, 99);
                write_leb(unknown, 3);
                fputc('x', unknown);
                fputc('y', unknown);
                fputc('z', unknown);
                fflush(unknown);
                rewind(unknown);

                BcBinaryInfo info;
                if (!bc_read_binary_info(unknown, &info, &error)) {
                    fprintf(stderr, "scan failed: %s\n", error.message);
                    return 8;
                }
                if (info.section_count != 1 || info.unknown_section_count != 1 || info.total_payload_bytes != 3) return 9;
                rewind(unknown);
                if (!bc_dump_binary_sections(unknown, visual, &error)) {
                    fprintf(stderr, "dump failed: %s\n", error.message);
                    return 10;
                }
                fflush(visual);
                rewind(visual);
                char buf[512] = {0};
                fread(buf, 1, sizeof(buf) - 1, visual);
                if (!strstr(buf, "section 99") || !strstr(buf, "unknown") || !strstr(buf, "skipped")) {
                    fprintf(stderr, "missing visual section dump: %s\n", buf);
                    return 11;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(harness)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_visual_verifier_tree_shows_register_and_control_flow(self):
        """TEST-ID: tests.bytecode.visual-verifier-tree
        TEST-CONTEXT: monadc.context.bytecode.core
        TEST-PURPOSE: bytecode verifier can render a dep-style tree with structured control and register type transitions.
        TEST-EXPECT: c-unit
        TEST-TIER: regression
        TEST-STATUS: active
        TEST-COVERAGE: bytecode.h, bytecode.c
        """
        harness = textwrap.dedent(
            r'''
            #include "bytecode.h"
            #include <stdio.h>
            #include <string.h>

            int main(void) {
                BcProgram program;
                BcError error;
                bc_program_init(&program, "visual");
                uint32_t c1 = bc_program_add_const(&program, bc_value_i64(1));
                uint32_t c2 = bc_program_add_const(&program, bc_value_i64(2));
                bc_emit_const(&program, 0, c1, bc_span("visual.mon", 1, 1));
                bc_emit_const(&program, 1, c2, bc_span("visual.mon", 1, 3));
                bc_emit_abc(&program, BC_OP_ADD, 2, 0, 1, bc_span("visual.mon", 1, 5));
                bc_emit(&program, (BcInstr){BC_OP_BOOL, 3, 0, 0, 1}, bc_span("visual.mon", 1, 9));
                bc_emit(&program, (BcInstr){BC_OP_IF, 3, 0, 0, 0}, bc_span("visual.mon", 1, 10));
                bc_emit_abc(&program, BC_OP_MOV, 4, 2, 0, bc_span("visual.mon", 1, 12));
                bc_emit(&program, (BcInstr){BC_OP_END, 0, 0, 0, 0}, bc_span("visual.mon", 1, 14));
                bc_emit_return(&program, 4, bc_span("visual.mon", 1, 16));

                FILE *tree = tmpfile();
                if (!tree) return 1;
                if (!bc_verify_trace(&program, tree, &error)) {
                    fprintf(stderr, "trace verify failed: %s\n", error.message);
                    return 2;
                }
                fflush(tree);
                rewind(tree);
                char buf[4096] = {0};
                fread(buf, 1, sizeof(buf) - 1, tree);
                if (!strstr(buf, "Bytecode verify visual") ||
                    !strstr(buf, "├─") ||
                    !strstr(buf, "│   ├─") ||
                    !strstr(buf, "r2 : I64") ||
                    !strstr(buf, "bc.if") ||
                    !strstr(buf, "r3") ||
                    !strstr(buf, "OK")) {
                    fprintf(stderr, "missing visual verifier tree:\n%s\n", buf);
                    return 3;
                }
                bc_program_free(&program);
                return 0;
            }
            '''
        )

        result = self.compile_and_run(harness)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_vm_numeric_comparison_boolean_and_decompile_ops(self):
        """TEST-ID: tests.bytecode.vm-op-spectrum
        TEST-CONTEXT: monadc.context.bytecode.core
        TEST-PURPOSE: VM executes numeric, comparison, boolean, move, negation, and decompile paths.
        TEST-EXPECT: c-unit
        TEST-TIER: regression
        TEST-STATUS: active
        TEST-COVERAGE: bytecode.h, bytecode.c
        """
        harness = textwrap.dedent(
            r'''
            #include "bytecode.h"
            #include <stdio.h>
            #include <string.h>

            static int run_i64(BcProgram *program, long long expected) {
                BcError error;
                BcVM vm;
                BcValue result = bc_value_nil();
                bc_vm_init(&vm);
                if (!bc_verify(program, &error)) {
                    fprintf(stderr, "verify failed: %s\n", error.message);
                    return 1;
                }
                if (!bc_vm_run(&vm, program, &result, &error)) {
                    fprintf(stderr, "run failed: %s\n", error.message);
                    return 2;
                }
                bc_vm_free(&vm);
                if (result.kind != BC_VALUE_I64 || result.as.i64 != expected) {
                    fprintf(stderr, "expected %lld got kind=%d value=%lld\n", expected, result.kind, (long long)result.as.i64);
                    return 3;
                }
                return 0;
            }

            int main(void) {
                BcProgram program;
                bc_program_init(&program, "ops");
                uint32_t c8 = bc_program_add_const(&program, bc_value_i64(8));
                uint32_t c3 = bc_program_add_const(&program, bc_value_i64(3));
                bc_emit_const(&program, 0, c8, bc_span("ops.mon", 1, 1));
                bc_emit_const(&program, 1, c3, bc_span("ops.mon", 1, 3));
                bc_emit_abc(&program, BC_OP_SUB, 2, 0, 1, bc_span("ops.mon", 1, 5));  /* 5 */
                bc_emit_abc(&program, BC_OP_MUL, 3, 2, 1, bc_span("ops.mon", 1, 7));  /* 15 */
                bc_emit_abc(&program, BC_OP_DIV, 4, 3, 1, bc_span("ops.mon", 1, 9));  /* 5 */
                bc_emit_abc(&program, BC_OP_MOD, 5, 0, 1, bc_span("ops.mon", 1, 11)); /* 2 */
                bc_emit_abc(&program, BC_OP_ADD, 6, 4, 5, bc_span("ops.mon", 1, 13)); /* 7 */
                bc_emit_abc(&program, BC_OP_NEG, 7, 1, 0, bc_span("ops.mon", 1, 15)); /* -3 */
                bc_emit_abc(&program, BC_OP_ADD, 8, 6, 7, bc_span("ops.mon", 1, 17)); /* 4 */
                bc_emit_abc(&program, BC_OP_GT, 9, 0, 1, bc_span("ops.mon", 1, 19));  /* true */
                bc_emit_abc(&program, BC_OP_NOT, 10, 9, 0, bc_span("ops.mon", 1, 21)); /* false */
                bc_emit_abc(&program, BC_OP_EQ, 11, 10, 10, bc_span("ops.mon", 1, 23)); /* true */
                bc_emit(&program, (BcInstr){BC_OP_IF, 11, 0, 0, 0}, bc_span("ops.mon", 1, 25));
                bc_emit_abc(&program, BC_OP_MOV, 12, 8, 0, bc_span("ops.mon", 1, 27));
                bc_emit(&program, (BcInstr){BC_OP_END, 0, 0, 0, 0}, bc_span("ops.mon", 1, 29));
                bc_emit_return(&program, 12, bc_span("ops.mon", 1, 31));

                int rc = run_i64(&program, 4);
                if (rc) return rc;

                FILE *dec = tmpfile();
                if (!dec) return 4;
                bc_decompile_monad(&program, dec);
                fflush(dec);
                rewind(dec);
                char buf[4096] = {0};
                fread(buf, 1, sizeof(buf) - 1, dec);
                if (!strstr(buf, "(- 8 3)") || !strstr(buf, "(> 8 3)") || !strstr(buf, "(not (> 8 3))")) {
                    fprintf(stderr, "missing op decompile:\n%s\n", buf);
                    return 5;
                }
                bc_program_free(&program);
                return 0;
            }
            '''
        )

        result = self.compile_and_run(harness)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_native_calls_inline_cache_and_vm_trace(self):
        """TEST-ID: tests.bytecode.native-ic-trace
        TEST-CONTEXT: monadc.context.bytecode.core
        TEST-PURPOSE: native call arity, inline-cache metadata, and VM trace output work together.
        TEST-EXPECT: c-unit
        TEST-TIER: regression
        TEST-STATUS: active
        TEST-COVERAGE: bytecode.h, bytecode.c
        """
        harness = textwrap.dedent(
            r'''
            #include "bytecode.h"
            #include <stdio.h>
            #include <string.h>

            static bool native_sum(BcVM *vm, const BcValue *args, uint8_t argc, BcValue *result, void *userdata) {
                (void)vm;
                (void)userdata;
                if (argc != 2 || args[0].kind != BC_VALUE_I64 || args[1].kind != BC_VALUE_I64) return false;
                *result = bc_value_i64(args[0].as.i64 + args[1].as.i64);
                return true;
            }

            int main(void) {
                BcProgram program;
                bc_program_init(&program, "native");
                uint32_t c5 = bc_program_add_const(&program, bc_value_i64(5));
                uint32_t c6 = bc_program_add_const(&program, bc_value_i64(6));
                uint32_t native = bc_program_add_native_typed(&program, "sum2", native_sum, NULL, BC_TYPE_I64, 2, 2);
                bc_emit_const(&program, 0, c5, bc_span("native.mon", 1, 1));
                bc_emit_const(&program, 1, c6, bc_span("native.mon", 1, 3));
                size_t callsite = bc_emit(&program, (BcInstr){BC_OP_CALL_NATIVE, 2, 0, 2, native}, bc_span("native.mon", 1, 5));
                bc_program_add_inline_cache(&program, (uint32_t)callsite);
                bc_emit_return(&program, 2, bc_span("native.mon", 1, 9));
                if (program.inline_cache_count != 1 || program.inline_caches[0].callsite != callsite) return 1;

                BcError error;
                BcVM vm;
                BcValue result = bc_value_nil();
                FILE *trace = tmpfile();
                if (!trace) return 2;
                bc_vm_init(&vm);
                vm.trace = true;
                vm.trace_out = trace;
                if (!bc_vm_run(&vm, &program, &result, &error)) {
                    fprintf(stderr, "run failed: %s\n", error.message);
                    return 3;
                }
                bc_vm_free(&vm);
                if (result.kind != BC_VALUE_I64 || result.as.i64 != 11) return 4;
                fflush(trace);
                rewind(trace);
                char buf[1024] = {0};
                fread(buf, 1, sizeof(buf) - 1, trace);
                if (!strstr(buf, "[bc]") || !strstr(buf, "bc.call-native")) {
                    fprintf(stderr, "missing vm trace:\n%s\n", buf);
                    return 5;
                }

                BcProgram bad;
                bc_program_init(&bad, "bad-native-arity");
                uint32_t c1 = bc_program_add_const(&bad, bc_value_i64(1));
                uint32_t n = bc_program_add_native_typed(&bad, "sum2", native_sum, NULL, BC_TYPE_I64, 2, 2);
                bc_emit_const(&bad, 0, c1, bc_span("bad.mon", 1, 1));
                bc_emit(&bad, (BcInstr){BC_OP_CALL_NATIVE, 1, 0, 1, n}, bc_span("bad.mon", 1, 3));
                bc_emit_return(&bad, 1, bc_span("bad.mon", 1, 5));
                if (bc_verify(&bad, &error)) {
                    fprintf(stderr, "expected arity rejection\n");
                    return 6;
                }
                if (!strstr(error.message, "arity")) {
                    fprintf(stderr, "wrong arity error: %s\n", error.message);
                    return 7;
                }

                bc_program_free(&bad);
                bc_program_free(&program);
                return 0;
            }
            '''
        )

        result = self.compile_and_run(harness)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_debug_modes_memory_stats_and_span_reporting(self):
        """TEST-ID: tests.bytecode.debug-modes-memory-stats
        TEST-CONTEXT: monadc.context.bytecode.core
        TEST-PURPOSE: debug modes change memory use and verifier errors preserve the expected span granularity.
        TEST-EXPECT: c-unit
        TEST-TIER: regression
        TEST-STATUS: active
        TEST-COVERAGE: bytecode.h, bytecode.c
        """
        harness = textwrap.dedent(
            r'''
            #include "bytecode.h"
            #include <stdio.h>

            static int build_bad(BcProgram *program, BcDebugMode mode) {
                BcError error;
                bc_program_init(program, "debug-mode");
                bc_program_set_debug_mode(program, mode);
                uint32_t one = bc_program_add_const(program, bc_value_i64(1));
                bc_emit_const(program, 0, one, bc_span("debug.mon", 44, 7));
                bc_emit(&*program, (BcInstr){BC_OP_IF, 0, 0, 0, 0}, bc_span("debug.mon", 45, 9));
                bc_emit_return(program, 0, bc_span("debug.mon", 46, 1));
                if (bc_verify(program, &error)) return 1;
                if (mode == BC_DEBUG_FULL_SPANS) {
                    if (!error.span.file || error.span.line != 45 || error.span.column != 9) return 2;
                } else if (mode == BC_DEBUG_LINES) {
                    if (error.span.file || error.span.line != 45 || error.span.column != 0) return 3;
                } else {
                    if (error.span.file || error.span.line != 0 || error.span.column != 0) return 4;
                }
                return 0;
            }

            int main(void) {
                BcProgram none, lines, full;
                int rc = build_bad(&none, BC_DEBUG_NONE);
                if (rc) return rc;
                rc = build_bad(&lines, BC_DEBUG_LINES);
                if (rc) return 10 + rc;
                rc = build_bad(&full, BC_DEBUG_FULL_SPANS);
                if (rc) return 20 + rc;
                BcProgramMemoryStats nstats = bc_program_memory_stats(&none);
                BcProgramMemoryStats lstats = bc_program_memory_stats(&lines);
                BcProgramMemoryStats fstats = bc_program_memory_stats(&full);
                if (!(nstats.debug_bytes < lstats.debug_bytes && lstats.debug_bytes < fstats.debug_bytes)) {
                    fprintf(stderr, "debug bytes not ordered: none=%zu lines=%zu full=%zu\n",
                            nstats.debug_bytes, lstats.debug_bytes, fstats.debug_bytes);
                    return 30;
                }
                bc_program_free(&none);
                bc_program_free(&lines);
                bc_program_free(&full);
                return 0;
            }
            '''
        )

        result = self.compile_and_run(harness)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_binary_reader_rejects_malformed_headers_and_payloads(self):
        """TEST-ID: tests.bytecode.binary-reader-malformed
        TEST-CONTEXT: monadc.context.bytecode.core
        TEST-PURPOSE: bytecode binary scanner reports invalid magic, truncated headers, and truncated payloads.
        TEST-EXPECT: c-unit
        TEST-TIER: regression
        TEST-STATUS: active
        TEST-COVERAGE: bytecode.h, bytecode.c
        """
        harness = textwrap.dedent(
            r'''
            #include "bytecode.h"
            #include <stdint.h>
            #include <stdio.h>
            #include <string.h>

            static void write_leb(FILE *f, uint64_t value) {
                do {
                    unsigned char byte = (unsigned char)(value & 0x7fu);
                    value >>= 7u;
                    if (value) byte |= 0x80u;
                    fputc(byte, f);
                } while (value);
            }

            static int expect_fail(FILE *f, const char *needle) {
                BcBinaryInfo info;
                BcError error;
                rewind(f);
                if (bc_read_binary_info(f, &info, &error)) {
                    fprintf(stderr, "expected binary scan failure\n");
                    return 1;
                }
                if (!strstr(error.message, needle)) {
                    fprintf(stderr, "wrong binary error: %s\n", error.message);
                    return 2;
                }
                return 0;
            }

            int main(void) {
                FILE *bad_magic = tmpfile();
                FILE *truncated_header = tmpfile();
                FILE *truncated_payload = tmpfile();
                if (!bad_magic || !truncated_header || !truncated_payload) return 1;

                write_leb(bad_magic, 123);
                write_leb(bad_magic, BC_VERSION_MAJOR);
                write_leb(bad_magic, BC_VERSION_MINOR);
                write_leb(bad_magic, 0);
                fflush(bad_magic);
                int rc = expect_fail(bad_magic, "magic");
                if (rc) return 10 + rc;

                write_leb(truncated_header, BC_MAGIC);
                write_leb(truncated_header, BC_VERSION_MAJOR);
                fflush(truncated_header);
                rc = expect_fail(truncated_header, "header");
                if (rc) return 20 + rc;

                write_leb(truncated_payload, BC_MAGIC);
                write_leb(truncated_payload, BC_VERSION_MAJOR);
                write_leb(truncated_payload, BC_VERSION_MINOR);
                write_leb(truncated_payload, 1);
                write_leb(truncated_payload, BC_SECTION_CODE);
                write_leb(truncated_payload, 4);
                fputc('x', truncated_payload);
                fflush(truncated_payload);
                rc = expect_fail(truncated_payload, "payload");
                if (rc) return 30 + rc;

                return 0;
            }
            '''
        )

        result = self.compile_and_run(harness)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_visual_trace_failure_and_unreachable_rejection(self):
        """TEST-ID: tests.bytecode.visual-failure-unreachable
        TEST-CONTEXT: monadc.context.bytecode.core
        TEST-PURPOSE: visual verifier marks failures and rejects unreachable code after return.
        TEST-EXPECT: c-unit
        TEST-TIER: regression
        TEST-STATUS: active
        TEST-COVERAGE: bytecode.h, bytecode.c
        """
        harness = textwrap.dedent(
            r'''
            #include "bytecode.h"
            #include <stdio.h>
            #include <string.h>

            int main(void) {
                BcProgram program;
                bc_program_init(&program, "unreachable");
                uint32_t one = bc_program_add_const(&program, bc_value_i64(1));
                bc_emit_const(&program, 0, one, bc_span("unreachable.mon", 1, 1));
                bc_emit_return(&program, 0, bc_span("unreachable.mon", 1, 3));
                bc_emit_const(&program, 1, one, bc_span("unreachable.mon", 1, 5));

                BcError error;
                FILE *tree = tmpfile();
                if (!tree) return 1;
                if (bc_verify_trace(&program, tree, &error)) {
                    fprintf(stderr, "expected trace verifier failure\n");
                    return 2;
                }
                if (!strstr(error.message, "unreachable")) {
                    fprintf(stderr, "wrong unreachable error: %s\n", error.message);
                    return 3;
                }
                fflush(tree);
                rewind(tree);
                char buf[2048] = {0};
                fread(buf, 1, sizeof(buf) - 1, tree);
                if (!strstr(buf, "Bytecode verify unreachable") || !strstr(buf, "FAIL")) {
                    fprintf(stderr, "missing visual failure:\n%s\n", buf);
                    return 4;
                }
                bc_program_free(&program);
                return 0;
            }
            '''
        )

        result = self.compile_and_run(harness)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_baseline_jit_executes_straightline_i64_bytecode(self):
        """TEST-ID: tests.bytecode.baseline-jit-i64
        TEST-CONTEXT: monadc.context.bytecode.core
        TEST-PURPOSE: baseline JIT emits executable native code for straight-line I64 register bytecode.
        TEST-EXPECT: c-unit
        TEST-TIER: regression
        TEST-STATUS: active
        TEST-COVERAGE: bytecode.h, bytecode.c
        """
        if not host_supports_baseline_jit():
            self.skipTest("baseline JIT is available only on Linux x86_64")

        harness = textwrap.dedent(
            r'''
            #include "bytecode.h"
            #include <stdint.h>
            #include <stdio.h>
            #include <string.h>

            typedef int64_t (*EntryFn)(void);

            int main(void) {
                BcProgram program;
                bc_program_init(&program, "jit-i64");
                uint32_t c8 = bc_program_add_const(&program, bc_value_i64(8));
                uint32_t c3 = bc_program_add_const(&program, bc_value_i64(3));
                bc_emit_const(&program, 0, c8, bc_span("jit.mon", 1, 1));
                bc_emit_const(&program, 1, c3, bc_span("jit.mon", 1, 3));
                bc_emit_abc(&program, BC_OP_SUB, 2, 0, 1, bc_span("jit.mon", 1, 5));  /* 5 */
                bc_emit_abc(&program, BC_OP_MUL, 3, 2, 1, bc_span("jit.mon", 1, 7));  /* 15 */
                bc_emit_abc(&program, BC_OP_ADD, 4, 3, 0, bc_span("jit.mon", 1, 9));  /* 23 */
                bc_emit_return(&program, 4, bc_span("jit.mon", 1, 11));

                BcJitOptions options = bc_jit_options_default();
                options.trace = true;
                BcJitArtifact artifact;
                BcError error;
                if (!bc_jit_compile_baseline(&program, &options, &artifact, &error)) {
                    fprintf(stderr, "jit failed: %s\n", error.message);
                    return 1;
                }
                if (!artifact.entry || artifact.code_size == 0 || artifact.tier != BC_JIT_BASELINE_TEMPLATE) return 2;
                EntryFn fn = (EntryFn)artifact.entry;
                if (fn() != 23) return 3;
                bc_jit_artifact_free(&artifact);
                bc_program_free(&program);
                return 0;
            }
            '''
        )

        result = self.compile_and_run(harness)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_tier_osr_visual_plan_marks_loops_and_safepoints(self):
        """TEST-ID: tests.bytecode.tier-osr-visual-plan
        TEST-CONTEXT: monadc.context.bytecode.core
        TEST-PURPOSE: bytecode tiering visualizer shows baseline tier, hotness, OSR loop safepoints, and deopt maps.
        TEST-EXPECT: c-unit
        TEST-TIER: regression
        TEST-STATUS: active
        TEST-COVERAGE: bytecode.h, bytecode.c
        """
        harness = textwrap.dedent(
            r'''
            #include "bytecode.h"
            #include <stdio.h>
            #include <string.h>

            int main(void) {
                BcProgram program;
                bc_program_init(&program, "tier-osr");
                uint32_t c3 = bc_program_add_const(&program, bc_value_i64(3));
                bc_emit_const(&program, 0, c3, bc_span("tier.mon", 1, 1));
                size_t loop = bc_emit(&program, (BcInstr){BC_OP_LOOP, 0, 0, 0, 0}, bc_span("tier.mon", 1, 3));
                bc_emit_abc(&program, BC_OP_MOV, 1, 0, 0, bc_span("tier.mon", 1, 5));
                bc_emit(&program, (BcInstr){BC_OP_END, 0, 0, 0, 0}, bc_span("tier.mon", 1, 7));
                bc_emit_return(&program, 1, bc_span("tier.mon", 1, 9));
                bc_program_add_deopt_point(&program, (uint32_t)loop, 0, 2, 77);

                BcTierPlan plan = bc_tier_plan_default();
                plan.call_hot_threshold = 10;
                plan.loop_hot_threshold = 4;
                plan.enable_osr = true;
                FILE *tree = tmpfile();
                BcError error;
                if (!tree) return 1;
                if (!bc_tier_plan_trace(&program, &plan, tree, &error)) {
                    fprintf(stderr, "tier plan failed: %s\n", error.message);
                    return 2;
                }
                fflush(tree);
                rewind(tree);
                char buf[4096] = {0};
                fread(buf, 1, sizeof(buf) - 1, tree);
                if (!strstr(buf, "Tier plan tier-osr") ||
                    !strstr(buf, "baseline template JIT") ||
                    !strstr(buf, "hotness call=10 loop=4") ||
                    !strstr(buf, "OSR safepoint") ||
                    !strstr(buf, "deopt registers r0..r1") ||
                    !strstr(buf, "OK")) {
                    fprintf(stderr, "missing tier visual plan:\n%s\n", buf);
                    return 3;
                }
                bc_program_free(&program);
                return 0;
            }
            '''
        )

        result = self.compile_and_run(harness)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_optimizer_folds_constants_and_reports_visually(self):
        """TEST-ID: tests.bytecode.optimizer-visual-report
        TEST-CONTEXT: monadc.context.bytecode.core
        TEST-PURPOSE: bytecode optimizer folds constants, removes redundant moves, preserves execution, and renders a visual report.
        TEST-EXPECT: c-unit
        TEST-TIER: regression
        TEST-STATUS: active
        TEST-COVERAGE: bytecode.h, bytecode.c
        """
        harness = textwrap.dedent(
            r'''
            #include "bytecode.h"
            #include <stdio.h>
            #include <string.h>

            static int run_i64(BcProgram *program, long long expected) {
                BcError error;
                BcVM vm;
                BcValue result = bc_value_nil();
                bc_vm_init(&vm);
                if (!bc_vm_run(&vm, program, &result, &error)) {
                    fprintf(stderr, "run failed: %s\n", error.message);
                    return 1;
                }
                bc_vm_free(&vm);
                if (result.kind != BC_VALUE_I64 || result.as.i64 != expected) {
                    fprintf(stderr, "wrong result kind=%d value=%lld\n", result.kind, (long long)result.as.i64);
                    return 2;
                }
                return 0;
            }

            int main(void) {
                BcProgram program;
                bc_program_init(&program, "opt-visual");
                uint32_t c2 = bc_program_add_const(&program, bc_value_i64(2));
                uint32_t c40 = bc_program_add_const(&program, bc_value_i64(40));
                bc_emit_const(&program, 0, c2, bc_span("opt.mon", 1, 1));
                bc_emit_const(&program, 1, c40, bc_span("opt.mon", 1, 3));
                bc_emit_abc(&program, BC_OP_ADD, 2, 0, 1, bc_span("opt.mon", 1, 5));
                bc_emit_abc(&program, BC_OP_MOV, 2, 2, 0, bc_span("opt.mon", 1, 7));
                bc_emit_return(&program, 2, bc_span("opt.mon", 1, 9));
                if (run_i64(&program, 42)) return 1;

                BcOptimizeOptions options = bc_optimize_options_default();
                BcOptimizeReport report;
                BcError error;
                FILE *tree = tmpfile();
                if (!tree) return 2;
                if (!bc_optimize_trace(&program, &options, &report, tree, &error)) {
                    fprintf(stderr, "opt trace failed: %s\n", error.message);
                    return 3;
                }
                if (report.before_instructions != 5 || report.after_instructions != 4) return 4;
                if (report.constants_folded < 1 || report.moves_eliminated < 1 || report.nops_removed < 1) return 5;
                if (run_i64(&program, 42)) return 6;

                fflush(tree);
                rewind(tree);
                char buf[4096] = {0};
                fread(buf, 1, sizeof(buf) - 1, tree);
                if (!strstr(buf, "Bytecode optimize opt-visual") ||
                    !strstr(buf, "constant folds:") ||
                    !strstr(buf, "dead/self moves:") ||
                    !strstr(buf, "instructions 5 -> 4") ||
                    !strstr(buf, "OK")) {
                    fprintf(stderr, "missing optimizer tree:\n%s\n", buf);
                    return 7;
                }
                bc_program_free(&program);
                return 0;
            }
            '''
        )

        result = self.compile_and_run(harness)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_visual_output_is_emacs_location_first_and_colored(self):
        """TEST-ID: tests.bytecode.visual-emacs-color
        TEST-CONTEXT: monadc.context.bytecode.core
        TEST-PURPOSE: visual bytecode output starts instruction rows with file:line:column for Emacs compilation mode and colors status/mnemonics after that prefix.
        TEST-EXPECT: c-unit
        TEST-TIER: regression
        TEST-STATUS: active
        TEST-COVERAGE: bytecode.h, bytecode.c
        """
        harness = textwrap.dedent(
            r'''
            #include "bytecode.h"
            #include <stdio.h>
            #include <string.h>

            static int line_starts_with(const char *haystack, const char *needle) {
                const char *p = haystack;
                size_t n = strlen(needle);
                while (p && *p) {
                    if (strncmp(p, needle, n) == 0) return 1;
                    p = strchr(p, '\n');
                    if (p) p++;
                }
                return 0;
            }

            int main(void) {
                BcProgram program;
                BcError error;
                bc_program_init(&program, "emacs-visual");
                uint32_t c2 = bc_program_add_const(&program, bc_value_i64(2));
                bc_emit_const(&program, 0, c2, bc_span("emacs-bytecode.mon", 7, 3));
                bc_emit_return(&program, 0, bc_span("emacs-bytecode.mon", 8, 1));

                FILE *trace = tmpfile();
                FILE *dis = tmpfile();
                if (!trace || !dis) return 1;
                if (!bc_verify_trace(&program, trace, &error)) return 2;
                bc_disassemble(&program, dis);
                fflush(trace);
                fflush(dis);
                rewind(trace);
                rewind(dis);

                char tbuf[4096] = {0};
                char dbuf[4096] = {0};
                fread(tbuf, 1, sizeof(tbuf) - 1, trace);
                fread(dbuf, 1, sizeof(dbuf) - 1, dis);

                if (!line_starts_with(tbuf, "emacs-bytecode.mon:7:3:")) {
                    fprintf(stderr, "trace lacks leading location:\n%s\n", tbuf);
                    return 3;
                }
                if (!line_starts_with(tbuf, "emacs-bytecode.mon:7:3:")) {
                    fprintf(stderr, "trace lacks leading location:\n%s\n", tbuf);
                    return 3;
                }
                if (!strstr(tbuf, "\033[36mbc.const\033[0m") ||
                    !strstr(tbuf, "\033[32mOK\033[0m") ||
                    !strstr(dbuf, "\033[36mbc.const\033[0m")) {
                    fprintf(stderr, "missing color:\nTRACE:\n%s\nDIS:\n%s\n", tbuf, dbuf);
                    return 5;
                }
                if (strncmp(tbuf, "\033", 1) == 0) {
                    fprintf(stderr, "color before location breaks compilation mode:\n%s\n", tbuf);
                    return 6;
                }


                bc_program_free(&program);
                return 0;
            }
            '''
        )

        result = self.compile_and_run(harness)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


def emit_visual_bytecode_report() -> int:
    harness = textwrap.dedent(
        r'''
        #include "bytecode.h"
        #include <stdio.h>

        int main(void) {
            BcProgram program;
            BcError error;
            bc_program_init(&program, "make-test-bytecode-visual");
            uint32_t c2 = bc_program_add_const(&program, bc_value_i64(2));
            uint32_t c40 = bc_program_add_const(&program, bc_value_i64(40));
            bc_emit_const(&program, 0, c2, bc_span("visual-test.mon", 1, 1));
            bc_emit_const(&program, 1, c40, bc_span("visual-test.mon", 1, 3));
            bc_emit_abc(&program, BC_OP_ADD, 2, 0, 1, bc_span("visual-test.mon", 1, 5));
            bc_emit_abc(&program, BC_OP_MOV, 2, 2, 0, bc_span("visual-test.mon", 1, 7));
            bc_emit(&program, (BcInstr){BC_OP_BOOL, 3, 0, 0, 1}, bc_span("visual-test.mon", 1, 9));
            bc_emit(&program, (BcInstr){BC_OP_IF, 3, 0, 0, 0}, bc_span("visual-test.mon", 1, 10));
            bc_emit_abc(&program, BC_OP_MOV, 4, 2, 0, bc_span("visual-test.mon", 1, 12));
            bc_emit(&program, (BcInstr){BC_OP_END, 0, 0, 0, 0}, bc_span("visual-test.mon", 1, 14));
            bc_emit_return(&program, 4, bc_span("visual-test.mon", 1, 16));

            if (!bc_verify_trace(&program, stdout, &error)) {
                fprintf(stderr, "verify trace failed: %s\n", error.message);
                return 1;
            }

            BcOptimizeOptions opt = bc_optimize_options_default();
            BcOptimizeReport report;
            if (!bc_optimize_trace(&program, &opt, &report, stdout, &error)) {
                fprintf(stderr, "optimize trace failed: %s\n", error.message);
                return 2;
            }

            BcTierPlan plan = bc_tier_plan_default();
            bc_tier_plan_trace(&program, &plan, stdout, &error);

            bc_disassemble(&program, stdout);
            bc_program_free(&program);
            return 0;
        }
        '''
    )

    test = BytecodeModuleTests()
    result = test.compile_and_run(harness)
    print(result.stdout, end="")
    if result.returncode != 0:
        print(result.stderr, end="")
    return result.returncode


if __name__ == "__main__":
    if os.environ.get("BYTECODE_VISUAL") == "1":
        rc = emit_visual_bytecode_report()
        if rc != 0:
            raise SystemExit(rc)

    import io

    loader = unittest.TestLoader()
    suite = loader.loadTestsFromTestCase(BytecodeModuleTests)
    test_cases = list(suite)
    name_width = max(len(t._testMethodName) for t in test_cases) if test_cases else 0

    def run_one(test):
        result_stream = io.StringIO()
        runner = unittest.TextTestRunner(stream=result_stream, verbosity=0)
        single_suite = unittest.TestSuite([test])
        start = time.perf_counter()
        result = runner.run(single_suite)
        elapsed_us = (time.perf_counter() - start) * 1_000_000
        return test._testMethodName, result.wasSuccessful(), result.errors, result.failures, elapsed_us

    results = []
    if os.environ.get("BYTECODE_PARALLEL") == "1":
        import concurrent.futures

        with concurrent.futures.ThreadPoolExecutor(max_workers=len(test_cases) or 1) as pool:
            futures = [pool.submit(run_one, t) for t in test_cases]
            for future in concurrent.futures.as_completed(futures):
                results.append(future.result())
    else:
        for t in test_cases:
            results.append(run_one(t))

    results.sort(key=lambda r: r[0])
    failures_total = 0
    pass_color = "\033[1;32m"
    fail_color = "\033[1;31m"
    cyan = "\033[36m"
    reset = "\033[0m"
    time_width = max(len(f"{elapsed_us:.0f}us") for *_, elapsed_us in results) if results else 0
    for name, ok, errors, failures, elapsed_us in results:
        status = f"{pass_color}PASS{reset}" if ok else f"{fail_color}FAIL{reset}"
        timing = f"{cyan}{elapsed_us:.0f}us{reset}".rjust(time_width + len(cyan) + len(reset))
        print(f"  {name.ljust(name_width)}  {status}  {timing}")
        for _, trace in errors + failures:
            print(trace)
            failures_total += 1

    raise SystemExit(1 if failures_total else 0)
