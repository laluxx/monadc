#ifndef BYTECODE_H
#define BYTECODE_H

/*
 * bytecode.h - Monad register bytecode substrate
 *
 * Core contracts:
 *   - register based, not stack based
 *   - fixed-size in-memory instructions for fast decode and validation
 *   - structured control operators only: block, if, else, loop, end
 *   - single-pass linear verification
 *   - sectioned binary serialization with skippable unknown sections
 *   - explicit debug/deopt metadata hooks from the first version
 *
 * The VM execution path is intentionally separated from the bytecode format.
 * This file defines the portable IR and validation/debug substrate that a
 * baseline JIT can lower from without changing bytecode producers.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BC_MAGIC 0x31434442u /* "BDC1" little endian */
#define BC_VERSION_MAJOR 1u
#define BC_VERSION_MINOR 0u
#define BC_MAX_REGISTERS 65535u
#define BC_STRUCTURED_DEPTH_MAX 1024u
#define BC_SECTION_UNKNOWN UINT32_MAX

typedef struct BcProgram BcProgram;
typedef struct BcVM BcVM;
typedef struct BcJitArtifact BcJitArtifact;

typedef enum {
    BC_VALUE_NIL,
    BC_VALUE_BOOL,
    BC_VALUE_I64,
    BC_VALUE_F64,
    BC_VALUE_PTR,
} BcValueKind;

typedef struct {
    BcValueKind kind;
    union {
        bool    boolean;
        int64_t i64;
        double  f64;
        void   *ptr;
    } as;
} BcValue;

typedef enum {
    BC_TYPE_UNKNOWN,
    BC_TYPE_NIL,
    BC_TYPE_BOOL,
    BC_TYPE_I64,
    BC_TYPE_F64,
    BC_TYPE_PTR,
} BcType;

typedef struct {
    const char *file;
    uint32_t    line;
    uint32_t    column;
} BcSourceSpan;

typedef enum {
    BC_DEBUG_NONE,
    BC_DEBUG_LINES,
    BC_DEBUG_FULL_SPANS,
} BcDebugMode;

typedef struct {
    char     message[256];
    size_t   instr;
    uint8_t  opcode;
    BcSourceSpan span;
} BcError;

typedef enum {
    BC_OP_NOP = 0,
    BC_OP_CONST,       /* r[a] = constants[imm] */
    BC_OP_NIL,         /* r[a] = nil */
    BC_OP_BOOL,        /* r[a] = imm != 0 */
    BC_OP_MOV,         /* r[a] = r[b] */
    BC_OP_ADD,         /* r[a] = r[b] + r[c] */
    BC_OP_SUB,
    BC_OP_MUL,
    BC_OP_DIV,
    BC_OP_MOD,
    BC_OP_NEG,         /* r[a] = -r[b] */
    BC_OP_EQ,          /* r[a] = r[b] == r[c] */
    BC_OP_LT,
    BC_OP_LE,
    BC_OP_GT,
    BC_OP_GE,
    BC_OP_NOT,         /* r[a] = !r[b] */
    BC_OP_CALL_NATIVE, /* r[a] = natives[imm](r[b..b+c)) */
    BC_OP_BLOCK,
    BC_OP_IF,          /* if r[a] then ... optional else ... end */
    BC_OP_ELSE,
    BC_OP_LOOP,        /* bounded loop body; r[a] is remaining count */
    BC_OP_END,
    BC_OP_RETURN,      /* return r[a] */
    BC_OP_HALT,
    BC_OP_COUNT,
} BcOp;

typedef enum {
    BC_SECTION_CODE = 1,
    BC_SECTION_CONSTANTS = 2,
    BC_SECTION_TYPES = 3,
    BC_SECTION_DEBUG = 4,
    BC_SECTION_DEOPT = 5,
    BC_SECTION_IC = 6,
} BcSectionId;

typedef struct {
    uint32_t id;
    uint64_t size;
    bool     known;
} BcSectionInfo;

typedef struct {
    uint8_t  op;
    uint16_t a;
    uint16_t b;
    uint16_t c;
    uint32_t imm;
} BcInstr;

typedef bool (*BcNativeFn)(BcVM *vm,
                          const BcValue *args,
                          uint8_t argc,
                          BcValue *result,
                          void *userdata);

typedef struct {
    char       *name;
    BcNativeFn fn;
    void       *userdata;
    BcType      return_type;
    uint8_t     min_arity;
    uint8_t     max_arity;
} BcNative;

typedef struct {
    uint32_t callsite;
    BcType   receiver_type;
    uint32_t target;
    uint32_t misses;
} BcInlineCache;

typedef struct {
    uint32_t safepoint;
    uint32_t first_register;
    uint32_t register_count;
    uint32_t source_id;
} BcDeoptPoint;

typedef enum {
    BC_JIT_BASELINE_TEMPLATE,
    BC_JIT_BASELINE_NATIVE,
} BcJitTier;

typedef struct {
    BcJitTier tier;
    bool      enable_inline_caches;
    bool      require_deopt_maps;
    bool      trace;
} BcJitOptions;

typedef struct {
    bool fold_constants;
    bool remove_self_moves;
    bool compact_nops;
    bool verify_after;
} BcOptimizeOptions;

typedef struct {
    size_t before_instructions;
    size_t after_instructions;
    size_t constants_folded;
    size_t moves_eliminated;
    size_t nops_removed;
    size_t constants_added;
} BcOptimizeReport;

typedef struct {
    uint32_t call_hot_threshold;
    uint32_t loop_hot_threshold;
    bool     enable_baseline_jit;
    bool     enable_optimizing_jit;
    bool     enable_osr;
} BcTierPlan;

struct BcJitArtifact {
    void    *entry;
    void    *code;
    size_t   code_size;
    BcJitTier tier;
};

typedef struct {
    size_t instruction_count;
    size_t instruction_capacity;
    size_t register_count;
    size_t constant_count;
    size_t code_bytes;
    size_t debug_bytes;
    size_t register_type_bytes;
    size_t constant_bytes;
    size_t metadata_bytes;
    size_t total_bytes;
} BcProgramMemoryStats;

typedef struct {
    uint32_t major;
    uint32_t minor;
    uint64_t section_count;
    uint64_t unknown_section_count;
    uint64_t total_payload_bytes;
    BcSectionInfo sections[16];
    size_t stored_section_count;
} BcBinaryInfo;

struct BcProgram {
    char *name;

    BcInstr      *code;
    BcSourceSpan *spans;
    uint32_t     *lines;
    size_t        code_count;
    size_t        code_capacity;
    BcDebugMode   debug_mode;

    BcValue *constants;
    size_t   const_count;
    size_t   const_capacity;

    BcType  *register_types;
    size_t   register_count;
    size_t   register_capacity;

    BcNative *natives;
    size_t    native_count;
    size_t    native_capacity;

    BcInlineCache *inline_caches;
    size_t         inline_cache_count;
    size_t         inline_cache_capacity;

    BcDeoptPoint *deopts;
    size_t        deopt_count;
    size_t        deopt_capacity;
};

struct BcVM {
    BcValue *registers;
    size_t   register_count;
    bool     trace;
    FILE    *trace_out;
};

BcValue bc_value_nil(void);
BcValue bc_value_bool(bool value);
BcValue bc_value_i64(int64_t value);
BcValue bc_value_f64(double value);
BcValue bc_value_ptr(void *value);
BcType  bc_type_of_value(BcValue value);
bool    bc_value_truthy(BcValue value);

BcSourceSpan bc_span(const char *file, uint32_t line, uint32_t column);
void         bc_error_clear(BcError *error);
const char  *bc_op_name(BcOp op);
const char  *bc_type_name(BcType type);

void     bc_program_init(BcProgram *program, const char *name);
void     bc_program_free(BcProgram *program);
void     bc_program_set_debug_mode(BcProgram *program, BcDebugMode mode);
bool     bc_program_reserve(BcProgram *program,
                            size_t code_capacity,
                            size_t const_capacity,
                            size_t register_capacity,
                            BcError *error);
void     bc_program_shrink_to_fit(BcProgram *program);
BcProgramMemoryStats bc_program_memory_stats(const BcProgram *program);
uint32_t bc_program_add_const(BcProgram *program, BcValue value);
uint32_t bc_program_add_native(BcProgram *program,
                               const char *name,
                               BcNativeFn fn,
                               void *userdata,
                               uint8_t min_arity,
                               uint8_t max_arity);
uint32_t bc_program_add_native_typed(BcProgram *program,
                                     const char *name,
                                     BcNativeFn fn,
                                     void *userdata,
                                     BcType return_type,
                                     uint8_t min_arity,
                                     uint8_t max_arity);
uint32_t bc_program_add_inline_cache(BcProgram *program, uint32_t callsite);
uint32_t bc_program_add_deopt_point(BcProgram *program,
                                    uint32_t safepoint,
                                    uint32_t first_register,
                                    uint32_t register_count,
                                    uint32_t source_id);

size_t bc_emit(BcProgram *program, BcInstr instr, BcSourceSpan span);
size_t bc_emit_abc(BcProgram *program, BcOp op, uint16_t a, uint16_t b, uint16_t c, BcSourceSpan span);
size_t bc_emit_const(BcProgram *program, uint16_t dst, uint32_t constant, BcSourceSpan span);
size_t bc_emit_return(BcProgram *program, uint16_t src, BcSourceSpan span);

bool bc_verify(const BcProgram *program, BcError *error);
bool bc_verify_trace(const BcProgram *program, FILE *out, BcError *error);
bool bc_write_binary(const BcProgram *program, FILE *out, BcError *error);
bool bc_read_binary_info(FILE *in, BcBinaryInfo *info, BcError *error);
bool bc_dump_binary_sections(FILE *in, FILE *out, BcError *error);

BcOptimizeOptions bc_optimize_options_default(void);
bool              bc_optimize_program(BcProgram *program,
                                      const BcOptimizeOptions *options,
                                      BcOptimizeReport *report,
                                      BcError *error);
bool              bc_optimize_trace(BcProgram *program,
                                    const BcOptimizeOptions *options,
                                    BcOptimizeReport *report,
                                    FILE *out,
                                    BcError *error);
BcJitOptions bc_jit_options_default(void);
bool         bc_jit_compile_baseline(const BcProgram *program,
                                     const BcJitOptions *options,
                                     BcJitArtifact *artifact,
                                     BcError *error);
void         bc_jit_artifact_free(BcJitArtifact *artifact);
BcTierPlan   bc_tier_plan_default(void);
bool         bc_tier_plan_trace(const BcProgram *program,
                                const BcTierPlan *plan,
                                FILE *out,
                                BcError *error);

void bc_vm_init(BcVM *vm);
void bc_vm_free(BcVM *vm);
bool bc_vm_run(BcVM *vm, const BcProgram *program, BcValue *result, BcError *error);

void bc_disassemble(const BcProgram *program, FILE *out);
void bc_decompile_monad(const BcProgram *program, FILE *out);

#ifdef __cplusplus
}
#endif

#endif
