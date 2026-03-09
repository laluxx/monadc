#include "asm.h"
#include "env.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void reg_alloc_init(RegisterAllocator *ra) {
    // System V AMD64 ABI calling convention
    ra->int_regs[0] = "%rdi";
    ra->int_regs[1] = "%rsi";
    ra->int_regs[2] = "%rdx";
    ra->int_regs[3] = "%rcx";
    ra->int_regs[4] = "%r8";
    ra->int_regs[5] = "%r9";

    ra->float_regs[0] = "%xmm0";
    ra->float_regs[1] = "%xmm1";
    ra->float_regs[2] = "%xmm2";
    ra->float_regs[3] = "%xmm3";
    ra->float_regs[4] = "%xmm4";
    ra->float_regs[5] = "%xmm5";
    ra->float_regs[6] = "%xmm6";
    ra->float_regs[7] = "%xmm7";

    ra->int_count = 0;
    ra->float_count = 0;
}

const char *reg_alloc_get(RegisterAllocator *ra, int param_index, Type *param_type) {
    if (!param_type) return NULL;

    if (param_type->kind == TYPE_FLOAT) {
        if (ra->float_count < 8) {
            return ra->float_regs[ra->float_count++];
        }
    } else {
        // Int, Hex, Bin, Oct, Char, String, etc.
        if (ra->int_count < 6) {
            return ra->int_regs[ra->int_count++];
        }
    }

    return NULL; // Stack parameters not yet supported
}

// Helper: check if a symbol is a parameter name
static int find_param_index(const char *name, EnvParam *params, int param_count) {
    for (int i = 0; i < param_count; i++) {
        if (strcmp(name, params[i].name) == 0) {
            return i;
        }
    }
    return -1;
}

// Convert AST symbol to LLVM placeholder or keep as-is
static char *process_operand(AST *operand, AsmContext *ctx, int *placeholder_num) {
    if (operand->type == AST_SYMBOL) {
        int param_idx = find_param_index(operand->symbol, ctx->params, ctx->param_count);
        if (param_idx >= 0) {
            if (ctx->naked) {
                // Naked: substitute real ABI register directly
                // Reset and walk allocator to get the right register for this param
                RegisterAllocator ra;
                reg_alloc_init(&ra);
                const char *reg = NULL;
                for (int i = 0; i <= param_idx; i++) {
                    reg = reg_alloc_get(&ra, i, ctx->params[i].type);
                }
                return strdup(reg ? reg : "%rdi");
            } else {
                // Normal: use LLVM placeholder
                char buf[16];
                snprintf(buf, sizeof(buf), "$%d", param_idx + 1);
                return strdup(buf);
            }
        }
        return strdup(operand->symbol);
    } else if (operand->type == AST_NUMBER) {
        char buf[64];
        if (ctx->naked) {
            snprintf(buf, sizeof(buf), "$%lld", (long long)operand->number);
        } else {
            snprintf(buf, sizeof(buf), "$$%lld", (long long)operand->number);
        }
        return strdup(buf);
    } else if (operand->type == AST_STRING) {
        return strdup(operand->string);
    }
    return strdup("???");
}

// Check if a token is a known x86-64 mnemonic
static bool is_mnemonic(const char *str) {
    static const char *mnemonics[] = {
        // Data movement
        "mov", "movq", "movl", "movw", "movb", "movabs",
        "lea", "movsx", "movsxd", "movzx",
        "movaps", "movups", "movapd", "movupd",
        "movdqa", "movdqu", "movq", "movd",
        "movss", "movsd", "movhps", "movlps",

        // Arithmetic
        "add", "sub", "imul", "idiv", "mul", "div",
        "inc", "dec", "neg", "adc", "sbb",
        "addps", "addpd", "addss", "addsd",
        "subps", "subpd", "subss", "subsd",
        "mulps", "mulpd", "mulss", "mulsd",
        "divps", "divpd", "divss", "divsd",

        // Logical
        "and", "or", "xor", "not",
        "andps", "andpd", "andnps", "andnpd",
        "orps", "orpd", "xorps", "xorpd",
        "pand", "pandn", "por", "pxor",

        // Shifts/rotates
        "shl", "shr", "sal", "sar", "rol", "ror",
        "rcl", "rcr", "shld", "shrd",
        "psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq",
        "psraw", "psrad",

        // Stack
        "push", "pop", "pushf", "popf", "pushfq", "popfq",

        // Control flow
        "call", "ret", "jmp", "loop", "loope", "loopne",
        "je", "jne", "jz", "jnz", "jl", "jle", "jg", "jge",
        "ja", "jae", "jb", "jbe", "js", "jns", "jo", "jno",
        "jp", "jnp", "jpe", "jpo", "jc", "jnc",
        "jecxz", "jrcxz",

        // Comparison
        "cmp", "test", "bt", "bts", "btr", "btc",
        "cmpps", "cmppd", "cmpss", "cmpsd",
        "comiss", "comisd", "ucomiss", "ucomisd",

        // Conditional move
        "cmov", "cmova", "cmovae", "cmovb", "cmovbe",
        "cmove", "cmovne", "cmovl", "cmovle", "cmovg", "cmovge",
        "cmovs", "cmovns", "cmovo", "cmovno", "cmovp", "cmovnp",

        // Set byte on condition
        "setz", "setnz", "sete", "setne",
        "setl", "setle", "setg", "setge",
        "seta", "setae", "setb", "setbe",
        "sets", "setns", "seto", "setno", "setp", "setnp",

        // Conversion
        "cvtsi2sd", "cvtsd2si", "cvtsi2ss", "cvtss2si",
        "cvtss2sd", "cvtsd2ss", "cvttpd2dq", "cvttps2dq",
        "cvttsd2si", "cvttss2si", "cvtdq2pd", "cvtdq2ps",

        // String operations
        "movs", "movsb", "movsw", "movsd", "movsq",
        "cmps", "cmpsb", "cmpsw", "cmpsd", "cmpsq",
        "scas", "scasb", "scasw", "scasd", "scasq",
        "lods", "lodsb", "lodsw", "lodsd", "lodsq",
        "stos", "stosb", "stosw", "stosd", "stosq",

        // Repeat prefixes
        "rep", "repe", "repz", "repne", "repnz",

        // Atomic operations
        "lock", "xchg", "xadd", "cmpxchg", "cmpxchg8b", "cmpxchg16b",

        // Memory barriers
        "mfence", "lfence", "sfence",

        // System/privileged
        "syscall", "sysenter", "sysexit", "sysret",
        "int", "iret", "iretq", "iretd",
        "cpuid", "rdtsc", "rdtscp", "rdpmc",
        "rdmsr", "wrmsr", "rdrand", "rdseed",

        // Cache control
        "clflush", "clflushopt", "clwb",
        "prefetcht0", "prefetcht1", "prefetcht2", "prefetchnta",
        "prefetchw", "prefetchwt1",

        // Non-temporal
        "movnti", "movntq", "movntdq", "movntpd", "movntps",
        "movntdqa",

        // Bit manipulation
        "popcnt", "lzcnt", "tzcnt", "bsf", "bsr",
        "bextr", "blsi", "blsmsk", "blsr",
        "andn", "bzhi", "pdep", "pext",
        "rorx", "sarx", "shlx", "shrx",

        // SIMD packed operations
        "paddb", "paddw", "paddd", "paddq",
        "psubb", "psubw", "psubd", "psubq",
        "pmullw", "pmulld", "pmuludq", "pmuldq",
        "pminub", "pminuw", "pminud", "pminsb", "pminsw", "pminsd",
        "pmaxub", "pmaxuw", "pmaxud", "pmaxsb", "pmaxsw", "pmaxsd",
        "pcmpeqb", "pcmpeqw", "pcmpeqd", "pcmpeqq",
        "pcmpgtb", "pcmpgtw", "pcmpgtd", "pcmpgtq",
        "pabsb", "pabsw", "pabsd",

        // SIMD shuffle/pack/unpack
        "pshufb", "pshufd", "pshufhw", "pshuflw",
        "shufps", "shufpd",
        "punpcklbw", "punpcklwd", "punpckldq", "punpcklqdq",
        "punpckhbw", "punpckhwd", "punpckhdq", "punpckhqdq",
        "unpcklps", "unpcklpd", "unpckhps", "unpckhpd",
        "packsswb", "packssdw", "packuswb", "packusdw",

        // SIMD horizontal operations
        "haddps", "haddpd", "hsubps", "hsubpd",
        "phaddw", "phaddd", "phsubw", "phsubd",

        // SIMD blending
        "blendps", "blendpd", "blendvps", "blendvpd",
        "pblendw", "pblendvb",

        // Cryptographic
        "aesenc", "aesenclast", "aesdec", "aesdeclast",
        "aesimc", "aeskeygenassist",
        "sha1rnds4", "sha1nexte", "sha1msg1", "sha1msg2",
        "sha256rnds2", "sha256msg1", "sha256msg2",
        "crc32", "pclmulqdq", "pclmullqlqdq", "pclmulhqlqdq",

        // FMA (Fused Multiply-Add)
        "vfmadd132ps", "vfmadd213ps", "vfmadd231ps",
        "vfmadd132pd", "vfmadd213pd", "vfmadd231pd",
        "vfmadd132ss", "vfmadd213ss", "vfmadd231ss",
        "vfmadd132sd", "vfmadd213sd", "vfmadd231sd",

        // Transactional memory
        "xbegin", "xend", "xabort", "xtest",

        // Misc
        "nop", "pause", "serialize", "clac", "stac",
        "ud2", "hlt", "wait", "fwait",
        "leave", "enter",
        "cbw", "cwde", "cdqe", "cwd", "cdq", "cqo",
        "xlat", "xlatb",

        NULL
    };

    for (int i = 0; mnemonics[i]; i++) {
        if (strcmp(str, mnemonics[i]) == 0) {
            return true;
        }
    }
    return false;
}

// Split flat token stream into individual instructions
static AsmInstruction *split_instructions(AST *flat_list, AsmContext *ctx, int *out_count) {
    if (flat_list->type != AST_LIST || flat_list->list.count == 0) {
        *out_count = 0;
        return NULL;
    }

    // First pass: count instructions (count mnemonics)
    int inst_count = 0;
    for (size_t i = 0; i < flat_list->list.count; i++) {
        AST *token = flat_list->list.items[i];
        if (token->type == AST_SYMBOL && is_mnemonic(token->symbol)) {
            inst_count++;
        }
    }

    if (inst_count == 0) {
        *out_count = 0;
        return NULL;
    }

    // Allocate instructions
    AsmInstruction *instructions = calloc(inst_count, sizeof(AsmInstruction));
    int current_inst = -1;

    // Second pass: split into instructions
    int placeholder_num = 0;
    for (size_t i = 0; i < flat_list->list.count; i++) {
        AST *token = flat_list->list.items[i];

        if (token->type == AST_SYMBOL && is_mnemonic(token->symbol)) {
            // Start new instruction
            current_inst++;
            instructions[current_inst].mnemonic = strdup(token->symbol);
            instructions[current_inst].operands = NULL;
            instructions[current_inst].operand_count = 0;
        } else if (current_inst >= 0) {
            // Add operand to current instruction
            char *operand = process_operand(token, ctx, &placeholder_num);

            instructions[current_inst].operand_count++;
            instructions[current_inst].operands = realloc(
                instructions[current_inst].operands,
                sizeof(char *) * instructions[current_inst].operand_count
            );
            instructions[current_inst].operands[instructions[current_inst].operand_count - 1] = operand;
        }
    }

    *out_count = inst_count;
    return instructions;
}

AsmInstruction *preprocess_asm(AST *asm_node, AsmContext *ctx, int *out_count) {
    if (asm_node->type != AST_ASM) {
        *out_count = 0;
        return NULL;
    }

    // asm_node->asm_block.instructions[0] contains the flat token list
    if (asm_node->asm_block.instruction_count == 0) {
        *out_count = 0;
        return NULL;
    }

    AST *flat_list = asm_node->asm_block.instructions[0];
    return split_instructions(flat_list, ctx, out_count);
}

void free_asm_instructions(AsmInstruction *instructions, int count) {
    if (!instructions) return;

    for (int i = 0; i < count; i++) {
        free(instructions[i].mnemonic);
        for (int j = 0; j < instructions[i].operand_count; j++) {
            free(instructions[i].operands[j]);
        }
        free(instructions[i].operands);
    }
    free(instructions);
}

// Build the AT&T syntax assembly string
/* static char *build_asm_string(AsmInstruction *instructions, int count, Type *return_type) { */
static char *build_asm_string(AsmInstruction *instructions, int count,
                               Type *return_type, bool naked) {
    // Estimate buffer size
    size_t bufsize = 1024;
    char *asm_str = malloc(bufsize);
    asm_str[0] = '\0';

    for (int i = 0; i < count; i++) {
        // Add mnemonic
        strcat(asm_str, instructions[i].mnemonic);

        // For AT&T two-operand instructions where destination is also source,
        // we need to swap operands because LLVM matching constraint makes
        // the FIRST input parameter the output/destination
        //
        // User writes: (asm add x y)  → add $1, $2
        // But with constraint "=r,0,r": $0=output, $1=first input (=output), $2=second input
        // AT&T syntax: add source, dest
        // We want: add $2, $1 (add second param TO first param which is output)
        // The swap is only needed for non-naked (LLVM placeholder) mode
        // because of the matching constraint "=r,0,r" requiring output==input[0]
        // For naked functions, operands are real registers — no swap needed
        bool is_two_operand = !naked && (
            strcmp(instructions[i].mnemonic, "add" ) == 0  ||
            strcmp(instructions[i].mnemonic, "sub" ) == 0  ||
            strcmp(instructions[i].mnemonic, "imul") == 0  ||
            strcmp(instructions[i].mnemonic, "and" ) == 0  ||
            strcmp(instructions[i].mnemonic, "or"  ) == 0  ||
            strcmp(instructions[i].mnemonic, "xor" ) == 0  );

        if (is_two_operand && instructions[i].operand_count == 2) {
            // Swap operands: user wrote (add x y) → add $1, $2
            // We generate: add $2, $1 (add y to x, result in x)
            strcat(asm_str, " ");
            strcat(asm_str, instructions[i].operands[1]);  // Second operand first (source)
            strcat(asm_str, ", ");
            strcat(asm_str, instructions[i].operands[0]);  // First operand second (dest)
        } else {
            // Normal order for other instructions
            for (int j = 0; j < instructions[i].operand_count; j++) {
                strcat(asm_str, " ");
                strcat(asm_str, instructions[i].operands[j]);
                if (j < instructions[i].operand_count - 1) {
                    strcat(asm_str, ",");
                }
            }
        }

        // Add newline separator between instructions
        if (i < count - 1) {
            strcat(asm_str, "\n");
        }
    }

    return asm_str;
}

LLVMValueRef codegen_inline_asm(LLVMContextRef context,
                                 LLVMBuilderRef builder,
                                 AsmInstruction *instructions,
                                 int instruction_count,
                                 Type *return_type,
                                 LLVMValueRef *params,
                                 int param_count,
                                 bool naked) {          // <-- add
    char *asm_str = build_asm_string(instructions, instruction_count, return_type, naked);

    fprintf(stderr, "=== INLINE ASM ===\n");
    fprintf(stderr, "Assembly: %s\n", asm_str);
    fprintf(stderr, "Naked: %s\n", naked ? "yes" : "no");

    LLVMTypeRef llvm_ret_type;
    if (return_type->kind == TYPE_FLOAT)     llvm_ret_type = LLVMDoubleTypeInContext(context);
    else if (return_type->kind == TYPE_CHAR) llvm_ret_type = LLVMInt8TypeInContext(context);
    else                                     llvm_ret_type = LLVMInt64TypeInContext(context);

    if (naked) {
        // Raw asm: no inputs, no outputs, no constraints
        // Just side-effecting assembly that manages its own registers
        LLVMTypeRef void_fn_type = LLVMFunctionType(
            LLVMVoidTypeInContext(context), NULL, 0, 0);

        LLVMValueRef asm_fn = LLVMGetInlineAsm(
            void_fn_type,
            asm_str, strlen(asm_str),
            "", 0,   // no constraints
            1,       // hasSideEffects
            0,
            LLVMInlineAsmDialectATT,
            0);

        LLVMBuildCall2(builder, void_fn_type, asm_fn, NULL, 0, "");
        free(asm_str);
        // Return dummy — caller will emit unreachable
        return LLVMConstInt(llvm_ret_type, 0, 0);
    }

    // Normal (non-naked) path — unchanged
    char constraints[256];
    if (return_type->kind == TYPE_FLOAT) strcpy(constraints, "=x");
    else                                 strcpy(constraints, "=r");

    if (param_count >= 1) strcat(constraints, ",0");
    for (int i = 1; i < param_count; i++) strcat(constraints, ",r");

    fprintf(stderr, "Constraints: %s\n", constraints);
    fprintf(stderr, "==================\n");

    LLVMTypeRef *param_types = malloc(sizeof(LLVMTypeRef) * param_count);
    for (int i = 0; i < param_count; i++)
        param_types[i] = LLVMTypeOf(params[i]);

    LLVMTypeRef asm_fn_type = LLVMFunctionType(llvm_ret_type, param_types, param_count, 0);
    LLVMValueRef asm_fn = LLVMGetInlineAsm(
        asm_fn_type,
        asm_str, strlen(asm_str),
        constraints, strlen(constraints),
        1, 0, LLVMInlineAsmDialectATT, 0);

    LLVMValueRef result = LLVMBuildCall2(builder, asm_fn_type, asm_fn,
                                         params, param_count, "asm_result");
    free(asm_str);
    free(param_types);
    return result;
}
