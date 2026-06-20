#include "bytecode.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

// We should probably take advantage that the language is a lisp somehow.

#if defined(__x86_64__) && defined(__unix__)
#include <sys/mman.h>
#include <unistd.h>
#define BC_HAS_X64_JIT 1
#else
#define BC_HAS_X64_JIT 0
#endif

#define BC_CLR_RESET    "\033[0m"
#define BC_CLR_DIM      "\033[2m"
#define BC_CLR_GRAY     "\033[90m"
#define BC_CLR_RED      "\033[31m"
#define BC_CLR_GREEN    "\033[32m"
#define BC_CLR_YELLOW   "\033[33m"
#define BC_CLR_BLUE     "\033[34m"
#define BC_CLR_MAGENTA  "\033[35m"
#define BC_CLR_CYAN     "\033[36m"
#define BC_CLR_BOLD_CYAN "\033[1;36m"

typedef enum {
    FRAME_BLOCK,
    FRAME_IF,
    FRAME_ELSE,
    FRAME_LOOP,
} FrameKind;

typedef struct {
    FrameKind kind;
    size_t start;
} VerifyFrame;

typedef struct {
    uint8_t *data;
    size_t count;
    size_t capacity;
} CodeBuf;

typedef struct {
    bool known;
    BcValue value;
} ConstFact;

static char *bc_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (!p) {
        fprintf(stderr, "bytecode: out of memory copying string\n");
        abort();
    }
    memcpy(p, s, n);
    return p;
}

static void bc_die_oom(size_t bytes) {
    fprintf(stderr, "bytecode: out of memory requesting %zu bytes\n", bytes);
    abort();
}

static void *bc_realloc_array(void *ptr, size_t count, size_t elem_size) {
    if (elem_size && count > SIZE_MAX / elem_size) bc_die_oom(count * elem_size);
    void *p = realloc(ptr, count * elem_size);
    if (!p && count) bc_die_oom(count * elem_size);
    return p;
}

static void cb_emit_u8(CodeBuf *cb, uint8_t byte) {
    if (cb->count == cb->capacity) {
        size_t cap = cb->capacity ? cb->capacity * 2 : 256;
        cb->data = bc_realloc_array(cb->data, cap, sizeof(cb->data[0]));
        cb->capacity = cap;
    }
    cb->data[cb->count++] = byte;
}

static void cb_emit_i32(CodeBuf *cb, int32_t value) {
    for (int i = 0; i < 4; i++) cb_emit_u8(cb, (uint8_t)(((uint32_t)value >> (8u * (uint32_t)i)) & 0xffu));
}

static void cb_emit_i64(CodeBuf *cb, int64_t value) {
    for (int i = 0; i < 8; i++) cb_emit_u8(cb, (uint8_t)(((uint64_t)value >> (8u * (uint32_t)i)) & 0xffu));
}

static int32_t reg_slot(uint16_t reg) {
    return -(int32_t)(((uint32_t)reg + 1u) * 8u);
}

static void x64_mov_rax_imm64(CodeBuf *cb, int64_t imm) {
    cb_emit_u8(cb, 0x48); cb_emit_u8(cb, 0xb8);
    cb_emit_i64(cb, imm);
}

static void x64_mov_rax_slot(CodeBuf *cb, uint16_t reg) {
    cb_emit_u8(cb, 0x48); cb_emit_u8(cb, 0x8b); cb_emit_u8(cb, 0x85);
    cb_emit_i32(cb, reg_slot(reg));
}

static void x64_mov_slot_rax(CodeBuf *cb, uint16_t reg) {
    cb_emit_u8(cb, 0x48); cb_emit_u8(cb, 0x89); cb_emit_u8(cb, 0x85);
    cb_emit_i32(cb, reg_slot(reg));
}

static void x64_binop_slot(CodeBuf *cb, uint8_t op_ext, uint16_t reg) {
    cb_emit_u8(cb, 0x48);
    cb_emit_u8(cb, op_ext);
    cb_emit_u8(cb, 0x85);
    cb_emit_i32(cb, reg_slot(reg));
}

#define BC_JIT_PROBE_STRIDE 4096

static void x64_emit_probed_stack_alloc(CodeBuf *cb, size_t frame_bytes) {
    if (frame_bytes < BC_JIT_PROBE_STRIDE) {
        cb_emit_u8(cb, 0x48); cb_emit_u8(cb, 0x81); cb_emit_u8(cb, 0xec);
        cb_emit_i32(cb, (int32_t)frame_bytes); /* sub rsp, frame_bytes */
        return;
    }

    cb_emit_u8(cb, 0x48); cb_emit_u8(cb, 0xb9); /* mov rcx, frame_bytes */
    cb_emit_i64(cb, (int64_t)frame_bytes);

    size_t loop_start = cb->count;
    cb_emit_u8(cb, 0x48); cb_emit_u8(cb, 0x81); cb_emit_u8(cb, 0xf9);
    cb_emit_i32(cb, BC_JIT_PROBE_STRIDE); /* cmp rcx, BC_JIT_PROBE_STRIDE */

    cb_emit_u8(cb, 0x0f); cb_emit_u8(cb, 0x86); /* jbe rel32 (forward, patched below) */
    size_t jbe_patch = cb->count;
    cb_emit_i32(cb, 0);

    cb_emit_u8(cb, 0x48); cb_emit_u8(cb, 0x81); cb_emit_u8(cb, 0xec);
    cb_emit_i32(cb, BC_JIT_PROBE_STRIDE); /* sub rsp, BC_JIT_PROBE_STRIDE */

    cb_emit_u8(cb, 0x48); cb_emit_u8(cb, 0xc7); cb_emit_u8(cb, 0x04); cb_emit_u8(cb, 0x24);
    cb_emit_i32(cb, 0); /* mov qword [rsp], 0 -- touch the probed page */

    cb_emit_u8(cb, 0x48); cb_emit_u8(cb, 0x81); cb_emit_u8(cb, 0xe9);
    cb_emit_i32(cb, BC_JIT_PROBE_STRIDE); /* sub rcx, BC_JIT_PROBE_STRIDE */

    cb_emit_u8(cb, 0xe9); /* jmp rel32 back to loop_start */
    int32_t back = (int32_t)((int64_t)loop_start - (int64_t)(cb->count + 4));
    cb_emit_i32(cb, back);

    int32_t fwd = (int32_t)((int64_t)cb->count - (int64_t)(jbe_patch + 4));
    cb->data[jbe_patch + 0] = (uint8_t)((uint32_t)fwd & 0xffu);
    cb->data[jbe_patch + 1] = (uint8_t)(((uint32_t)fwd >> 8) & 0xffu);
    cb->data[jbe_patch + 2] = (uint8_t)(((uint32_t)fwd >> 16) & 0xffu);
    cb->data[jbe_patch + 3] = (uint8_t)(((uint32_t)fwd >> 24) & 0xffu);

    cb_emit_u8(cb, 0x48); cb_emit_u8(cb, 0x81); cb_emit_u8(cb, 0xec);
    cb_emit_i32(cb, BC_JIT_PROBE_STRIDE); /* sub rsp, BC_JIT_PROBE_STRIDE (remainder, always < stride) */
}

static BcSourceSpan instr_span(const BcProgram *program, size_t index) {
    if (program && program->debug_mode == BC_DEBUG_FULL_SPANS && program->spans && index < program->code_count)
        return program->spans[index];
    if (program && program->debug_mode == BC_DEBUG_LINES && program->lines && index < program->code_count)
        return bc_span(NULL, program->lines[index], 0);
    return bc_span(NULL, 0, 0);
}

static void print_padding(FILE *out, size_t width) {
    for (size_t i = 0; i < width; i++) fputc(' ', out);
}

static size_t location_text_width(BcSourceSpan span) {
    char buf[160];
    if (span.file && span.line)
        return (size_t)snprintf(buf, sizeof(buf), "%s:%u:%u: note:", span.file, span.line, span.column);
    if (span.line)
        return (size_t)snprintf(buf, sizeof(buf), "<bytecode>:%u:0: note:", span.line);
    return 0;
}

static size_t program_location_column_width(const BcProgram *program) {
    size_t width = 0;
    for (size_t i = 0; i < program->code_count; i++) {
        size_t w = location_text_width(instr_span(program, i));
        if (w > width) width = w;
    }
    return width;
}

static void print_location_field(FILE *out, BcSourceSpan span, size_t column_width) {
    char buf[160];
    size_t w;
    if (span.file && span.line)
        w = (size_t)snprintf(buf, sizeof(buf), "%s:%u:%u: note:", span.file, span.line, span.column);
    else if (span.line)
        w = (size_t)snprintf(buf, sizeof(buf), "<bytecode>:%u:0: note:", span.line);
    else {
        print_padding(out, column_width + 1);
        return;
    }
    fprintf(out, "%s ", buf);
    print_padding(out, column_width > w ? column_width - w : 0);
}

static void print_location_blank(FILE *out, size_t column_width) {
    print_padding(out, column_width + 1);
}

static const char *op_color(BcOp op) {
    if (op == BC_OP_IF || op == BC_OP_ELSE || op == BC_OP_LOOP || op == BC_OP_BLOCK || op == BC_OP_END)
        return BC_CLR_YELLOW;
    if (op == BC_OP_RETURN || op == BC_OP_HALT)
        return BC_CLR_MAGENTA;
    if (op == BC_OP_CALL_NATIVE)
        return BC_CLR_BLUE;
    return BC_CLR_CYAN;
}

static void print_colored_op(FILE *out, BcOp op) {
    fprintf(out, "%s%s%s", op_color(op), bc_op_name(op), BC_CLR_RESET);
}

static void print_padded_op(FILE *out, BcOp op, size_t field_width) {
    const char *name = bc_op_name(op);
    size_t len = strlen(name);
    print_colored_op(out, op);
    print_padding(out, field_width > len ? field_width - len : 1);
}

static void print_status(FILE *out, bool ok) {
    fprintf(out, "%s%s%s", ok ? BC_CLR_GREEN : BC_CLR_RED, ok ? "OK" : "FAIL", BC_CLR_RESET);
}

static void print_dim(FILE *out, const char *fmt, ...) {
    fprintf(out, "%s", BC_CLR_DIM);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fprintf(out, "%s", BC_CLR_RESET);
}

static void bc_errorf(BcError *error,
                      const BcProgram *program,
                      size_t instr,
                      uint8_t opcode,
                      const char *fmt,
                      ...) {
    if (!error) return;
    error->instr = instr;
    error->opcode = opcode;
    if (program && instr < program->code_count && program->debug_mode == BC_DEBUG_FULL_SPANS && program->spans) {
        error->span = program->spans[instr];
    } else if (program && instr < program->code_count && program->debug_mode == BC_DEBUG_LINES && program->lines) {
        error->span = bc_span(NULL, program->lines[instr], 0);
    } else {
        error->span = bc_span(NULL, 0, 0);
    }

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(error->message, sizeof(error->message), fmt, ap);
    va_end(ap);
}

static void ensure_code(BcProgram *program) {
    if (program->code_count < program->code_capacity) return;
    size_t cap = program->code_capacity ? program->code_capacity * 2 : 64;
    program->code = bc_realloc_array(program->code, cap, sizeof(program->code[0]));
    if (program->debug_mode == BC_DEBUG_FULL_SPANS)
        program->spans = bc_realloc_array(program->spans, cap, sizeof(program->spans[0]));
    if (program->debug_mode == BC_DEBUG_LINES)
        program->lines = bc_realloc_array(program->lines, cap, sizeof(program->lines[0]));
    program->code_capacity = cap;
}

static bool reserve_array(void **ptr, size_t *capacity, size_t requested, size_t elem_size) {
    if (requested <= *capacity) return true;
    *ptr = bc_realloc_array(*ptr, requested, elem_size);
    *capacity = requested;
    return true;
}

static void ensure_constants(BcProgram *program) {
    if (program->const_count < program->const_capacity) return;
    size_t cap = program->const_capacity ? program->const_capacity * 2 : 16;
    program->constants = bc_realloc_array(program->constants, cap, sizeof(program->constants[0]));
    program->const_capacity = cap;
}

static void ensure_registers(BcProgram *program, uint32_t reg) {
    if ((size_t)reg < program->register_count) return;
    size_t need = (size_t)reg + 1;
    size_t cap = program->register_capacity ? program->register_capacity : 32;
    while (cap < need) cap *= 2;
    program->register_types = bc_realloc_array(program->register_types, cap, sizeof(program->register_types[0]));
    for (size_t i = program->register_count; i < cap; i++) program->register_types[i] = BC_TYPE_UNKNOWN;
    program->register_count = need;
    program->register_capacity = cap;
}

static void ensure_natives(BcProgram *program) {
    if (program->native_count < program->native_capacity) return;
    size_t cap = program->native_capacity ? program->native_capacity * 2 : 8;
    program->natives = bc_realloc_array(program->natives, cap, sizeof(program->natives[0]));
    program->native_capacity = cap;
}

static void ensure_inline_caches(BcProgram *program) {
    if (program->inline_cache_count < program->inline_cache_capacity) return;
    size_t cap = program->inline_cache_capacity ? program->inline_cache_capacity * 2 : 16;
    program->inline_caches = bc_realloc_array(program->inline_caches, cap, sizeof(program->inline_caches[0]));
    program->inline_cache_capacity = cap;
}

static void ensure_deopts(BcProgram *program) {
    if (program->deopt_count < program->deopt_capacity) return;
    size_t cap = program->deopt_capacity ? program->deopt_capacity * 2 : 16;
    program->deopts = bc_realloc_array(program->deopts, cap, sizeof(program->deopts[0]));
    program->deopt_capacity = cap;
}

static void write_byte(FILE *out, uint8_t byte, bool *ok) {
    if (*ok && fputc(byte, out) == EOF) *ok = false;
}

static void write_leb_u64(FILE *out, uint64_t value, bool *ok) {
    do {
        uint8_t byte = (uint8_t)(value & 0x7fu);
        value >>= 7u;
        if (value) byte |= 0x80u;
        write_byte(out, byte, ok);
    } while (value);
}

static bool read_leb_u64(FILE *in, uint64_t *out) {
    uint64_t result = 0;
    unsigned shift = 0;
    for (;;) {
        int ch = fgetc(in);
        if (ch == EOF || shift >= 64) return false;
        result |= (uint64_t)(ch & 0x7f) << shift;
        if (!(ch & 0x80)) {
            *out = result;
            return true;
        }
        shift += 7;
    }
}

static bool skip_bytes(FILE *in, uint64_t size) {
    unsigned char buf[4096];
    while (size) {
        size_t chunk = size < sizeof(buf) ? (size_t)size : sizeof(buf);
        if (fread(buf, 1, chunk, in) != chunk) return false;
        size -= chunk;
    }
    return true;
}

static bool section_is_known(uint32_t id) {
    return id == BC_SECTION_CODE ||
        id == BC_SECTION_CONSTANTS ||
        id == BC_SECTION_TYPES ||
        id == BC_SECTION_DEBUG ||
        id == BC_SECTION_DEOPT ||
        id == BC_SECTION_IC;
}

static const char *section_name(uint32_t id) {
    switch (id) {
        case BC_SECTION_CODE: return "code";
        case BC_SECTION_CONSTANTS: return "constants";
        case BC_SECTION_TYPES: return "types";
        case BC_SECTION_DEBUG: return "debug";
        case BC_SECTION_DEOPT: return "deopt";
        case BC_SECTION_IC: return "inline-cache";
        default: return "unknown";
    }
}

static void write_i64(FILE *out, int64_t value, bool *ok) {
    for (int i = 0; i < 8; i++) write_byte(out, (uint8_t)(((uint64_t)value >> (8u * (uint32_t)i)) & 0xffu), ok);
}

static void write_f64(FILE *out, double value, bool *ok) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    for (int i = 0; i < 8; i++) write_byte(out, (uint8_t)((bits >> (8u * (uint32_t)i)) & 0xffu), ok);
}

static bool write_section(const BcProgram *program, FILE *out, uint32_t id, void (*writer)(const BcProgram *, FILE *, bool *), BcError *error) {
    FILE *tmp = tmpfile();
    if (!tmp) {
        bc_errorf(error, program, 0, 0, "could not create temporary section buffer");
        return false;
    }
    bool ok = true;
    writer(program, tmp, &ok);
    long size = ftell(tmp);
    if (size < 0) ok = false;
    rewind(tmp);
    write_leb_u64(out, id, &ok);
    write_leb_u64(out, (uint64_t)size, &ok);
    for (long i = 0; ok && i < size; i++) {
        int ch = fgetc(tmp);
        if (ch == EOF) ok = false;
        else write_byte(out, (uint8_t)ch, &ok);
    }
    fclose(tmp);
    if (!ok) bc_errorf(error, program, 0, 0, "failed writing bytecode section %u", id);
    return ok;
}

static bool same_numeric(BcType a, BcType b) {
    return (a == BC_TYPE_I64 || a == BC_TYPE_F64) && (b == BC_TYPE_I64 || b == BC_TYPE_F64);
}

static BcType numeric_result(BcType a, BcType b) {
    if (a == BC_TYPE_F64 || b == BC_TYPE_F64) return BC_TYPE_F64;
    return BC_TYPE_I64;
}

static bool value_equal(BcValue a, BcValue b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case BC_VALUE_NIL: return true;
        case BC_VALUE_BOOL: return a.as.boolean == b.as.boolean;
        case BC_VALUE_I64: return a.as.i64 == b.as.i64;
        case BC_VALUE_F64: return a.as.f64 == b.as.f64;
        case BC_VALUE_PTR: return a.as.ptr == b.as.ptr;
    }
    return false;
}

static bool value_i64_binary(BcOp op, BcValue a, BcValue b, BcValue *out) {
    if (a.kind != BC_VALUE_I64 || b.kind != BC_VALUE_I64) return false;
    switch (op) {
        case BC_OP_ADD: *out = bc_value_i64(a.as.i64 + b.as.i64); return true;
        case BC_OP_SUB: *out = bc_value_i64(a.as.i64 - b.as.i64); return true;
        case BC_OP_MUL: *out = bc_value_i64(a.as.i64 * b.as.i64); return true;
        case BC_OP_DIV:
            if (b.as.i64 == 0) return false;
            *out = bc_value_i64(a.as.i64 / b.as.i64);
            return true;
        case BC_OP_MOD:
            if (b.as.i64 == 0) return false;
            *out = bc_value_i64(a.as.i64 % b.as.i64);
            return true;
        case BC_OP_EQ: *out = bc_value_bool(a.as.i64 == b.as.i64); return true;
        case BC_OP_LT: *out = bc_value_bool(a.as.i64 < b.as.i64); return true;
        case BC_OP_LE: *out = bc_value_bool(a.as.i64 <= b.as.i64); return true;
        case BC_OP_GT: *out = bc_value_bool(a.as.i64 > b.as.i64); return true;
        case BC_OP_GE: *out = bc_value_bool(a.as.i64 >= b.as.i64); return true;
        default: return false;
    }
}

BcValue bc_value_nil(void) {
    BcValue v;
    v.kind = BC_VALUE_NIL;
    v.as.ptr = NULL;
    return v;
}

BcValue bc_value_bool(bool value) {
    BcValue v;
    v.kind = BC_VALUE_BOOL;
    v.as.boolean = value;
    return v;
}

BcValue bc_value_i64(int64_t value) {
    BcValue v;
    v.kind = BC_VALUE_I64;
    v.as.i64 = value;
    return v;
}

BcValue bc_value_f64(double value) {
    BcValue v;
    v.kind = BC_VALUE_F64;
    v.as.f64 = value;
    return v;
}

BcValue bc_value_ptr(void *value) {
    BcValue v;
    v.kind = BC_VALUE_PTR;
    v.as.ptr = value;
    return v;
}

BcType bc_type_of_value(BcValue value) {
    switch (value.kind) {
        case BC_VALUE_NIL: return BC_TYPE_NIL;
        case BC_VALUE_BOOL: return BC_TYPE_BOOL;
        case BC_VALUE_I64: return BC_TYPE_I64;
        case BC_VALUE_F64: return BC_TYPE_F64;
        case BC_VALUE_PTR: return BC_TYPE_PTR;
    }
    return BC_TYPE_UNKNOWN;
}

bool bc_value_truthy(BcValue value) {
    switch (value.kind) {
        case BC_VALUE_NIL: return false;
        case BC_VALUE_BOOL: return value.as.boolean;
        case BC_VALUE_I64: return value.as.i64 != 0;
        case BC_VALUE_F64: return value.as.f64 != 0.0;
        case BC_VALUE_PTR: return value.as.ptr != NULL;
    }
    return false;
}

BcSourceSpan bc_span(const char *file, uint32_t line, uint32_t column) {
    BcSourceSpan span;
    span.file = file;
    span.line = line;
    span.column = column;
    return span;
}

void bc_error_clear(BcError *error) {
    if (error) memset(error, 0, sizeof(*error));
}

const char *bc_op_name(BcOp op) {
    static const char *names[] = {
        "bc.nop", "bc.const", "bc.nil", "bc.bool", "bc.mov", "bc.add",
        "bc.sub", "bc.mul", "bc.div", "bc.mod", "bc.neg", "bc.eq",
        "bc.lt", "bc.le", "bc.gt", "bc.ge", "bc.not", "bc.call-native",
        "bc.block", "bc.if", "bc.else", "bc.loop", "bc.end", "bc.return",
        "bc.halt",
    };
    if ((unsigned)op >= BC_OP_COUNT) return "bc.<invalid>";
    return names[op];
}

const char *bc_type_name(BcType type) {
    switch (type) {
        case BC_TYPE_UNKNOWN: return "Unknown";
        case BC_TYPE_NIL: return "Nil";
        case BC_TYPE_BOOL: return "Bool";
        case BC_TYPE_I64: return "I64";
        case BC_TYPE_F64: return "F64";
        case BC_TYPE_PTR: return "Ptr";
    }
    return "<type>";
}

void bc_program_init(BcProgram *program, const char *name) {
    memset(program, 0, sizeof(*program));
    program->name = bc_strdup(name ? name : "<bytecode>");
    program->debug_mode = BC_DEBUG_FULL_SPANS;
}

void bc_program_free(BcProgram *program) {
    if (!program) return;
    free(program->name);
    free(program->code);
    free(program->spans);
    free(program->lines);
    free(program->constants);
    free(program->register_types);
    for (size_t i = 0; i < program->native_count; i++) free(program->natives[i].name);
    free(program->natives);
    free(program->inline_caches);
    free(program->deopts);
    memset(program, 0, sizeof(*program));
}

void bc_program_set_debug_mode(BcProgram *program, BcDebugMode mode) {
    if (!program) return;
    if (program->code_count != 0 || program->code_capacity != 0) return;
    program->debug_mode = mode;
}

bool bc_program_reserve(BcProgram *program,
                        size_t code_capacity,
                        size_t const_capacity,
                        size_t register_capacity,
                        BcError *error) {
    bc_error_clear(error);
    if (code_capacity > SIZE_MAX / sizeof(BcInstr) ||
        const_capacity > SIZE_MAX / sizeof(BcValue) ||
        register_capacity > SIZE_MAX / sizeof(BcType)) {
        bc_errorf(error, program, 0, 0, "bytecode reserve size overflow");
        return false;
    }

    reserve_array((void **)&program->code, &program->code_capacity, code_capacity, sizeof(program->code[0]));
    if (program->debug_mode == BC_DEBUG_FULL_SPANS)
        program->spans = bc_realloc_array(program->spans, program->code_capacity, sizeof(program->spans[0]));
    else if (program->debug_mode == BC_DEBUG_LINES)
        program->lines = bc_realloc_array(program->lines, program->code_capacity, sizeof(program->lines[0]));

    reserve_array((void **)&program->constants, &program->const_capacity, const_capacity, sizeof(program->constants[0]));

    size_t old_reg_capacity = program->register_capacity;
    reserve_array((void **)&program->register_types, &program->register_capacity, register_capacity, sizeof(program->register_types[0]));
    for (size_t i = old_reg_capacity; i < program->register_capacity; i++)
        program->register_types[i] = BC_TYPE_UNKNOWN;
    return true;
}

void bc_program_shrink_to_fit(BcProgram *program) {
    if (!program) return;
    size_t code_count = program->code_count;
    if (code_count != program->code_capacity) {
        program->code = bc_realloc_array(program->code, code_count, sizeof(program->code[0]));
        if (program->debug_mode == BC_DEBUG_FULL_SPANS)
            program->spans = bc_realloc_array(program->spans, code_count, sizeof(program->spans[0]));
        if (program->debug_mode == BC_DEBUG_LINES)
            program->lines = bc_realloc_array(program->lines, code_count, sizeof(program->lines[0]));
        program->code_capacity = code_count;
    }
    if (program->const_count != program->const_capacity) {
        program->constants = bc_realloc_array(program->constants, program->const_count, sizeof(program->constants[0]));
        program->const_capacity = program->const_count;
    }
    if (program->register_count != program->register_capacity) {
        program->register_types = bc_realloc_array(program->register_types, program->register_count, sizeof(program->register_types[0]));
        program->register_capacity = program->register_count;
    }
}

BcProgramMemoryStats bc_program_memory_stats(const BcProgram *program) {
    BcProgramMemoryStats stats = {0};
    if (!program) return stats;
    stats.instruction_count = program->code_count;
    stats.instruction_capacity = program->code_capacity;
    stats.register_count = program->register_count;
    stats.constant_count = program->const_count;
    stats.code_bytes = program->code_capacity * sizeof(program->code[0]);
    if (program->debug_mode == BC_DEBUG_FULL_SPANS)
        stats.debug_bytes = program->code_capacity * sizeof(program->spans[0]);
    else if (program->debug_mode == BC_DEBUG_LINES)
        stats.debug_bytes = program->code_capacity * sizeof(program->lines[0]);
    stats.register_type_bytes = program->register_capacity * sizeof(program->register_types[0]);
    stats.constant_bytes = program->const_capacity * sizeof(program->constants[0]);
    stats.metadata_bytes =
        program->native_capacity * sizeof(program->natives[0]) +
        program->inline_cache_capacity * sizeof(program->inline_caches[0]) +
        program->deopt_capacity * sizeof(program->deopts[0]);
    stats.total_bytes = stats.code_bytes + stats.debug_bytes +
        stats.register_type_bytes + stats.constant_bytes + stats.metadata_bytes;
    return stats;
}

uint32_t bc_program_add_const(BcProgram *program, BcValue value) {
    ensure_constants(program);
    if (program->const_count > UINT32_MAX) abort();
    uint32_t index = (uint32_t)program->const_count;
    program->constants[program->const_count++] = value;
    return index;
}

uint32_t bc_program_add_native(BcProgram *program,
                               const char *name,
                               BcNativeFn fn,
                               void *userdata,
                               uint8_t min_arity,
                               uint8_t max_arity) {
    return bc_program_add_native_typed(program, name, fn, userdata,
                                       BC_TYPE_UNKNOWN, min_arity, max_arity);
}

uint32_t bc_program_add_native_typed(BcProgram *program,
                                     const char *name,
                                     BcNativeFn fn,
                                     void *userdata,
                                     BcType return_type,
                                     uint8_t min_arity,
                                     uint8_t max_arity) {
    ensure_natives(program);
    if (program->native_count > UINT32_MAX) abort();
    uint32_t index = (uint32_t)program->native_count;
    BcNative *native = &program->natives[program->native_count++];
    native->name = bc_strdup(name ? name : "<native>");
    native->fn = fn;
    native->userdata = userdata;
    native->return_type = return_type;
    native->min_arity = min_arity;
    native->max_arity = max_arity;
    return index;
}

uint32_t bc_program_add_inline_cache(BcProgram *program, uint32_t callsite) {
    ensure_inline_caches(program);
    uint32_t index = (uint32_t)program->inline_cache_count;
    program->inline_caches[program->inline_cache_count++] = (BcInlineCache){callsite, BC_TYPE_UNKNOWN, 0, 0};
    return index;
}

uint32_t bc_program_add_deopt_point(BcProgram *program,
                                    uint32_t safepoint,
                                    uint32_t first_register,
                                    uint32_t register_count,
                                    uint32_t source_id) {
    ensure_deopts(program);
    uint32_t index = (uint32_t)program->deopt_count;
    program->deopts[program->deopt_count++] = (BcDeoptPoint){safepoint, first_register, register_count, source_id};
    return index;
}

size_t bc_emit(BcProgram *program, BcInstr instr, BcSourceSpan span) {
    ensure_code(program);
    size_t index = program->code_count;
    program->code[program->code_count] = instr;
    if (program->debug_mode == BC_DEBUG_FULL_SPANS && program->spans)
        program->spans[program->code_count] = span;
    else if (program->debug_mode == BC_DEBUG_LINES && program->lines)
        program->lines[program->code_count] = span.line;
    program->code_count++;
    if (instr.a < BC_MAX_REGISTERS) ensure_registers(program, instr.a);
    if (instr.b < BC_MAX_REGISTERS && instr.op != BC_OP_BLOCK && instr.op != BC_OP_ELSE && instr.op != BC_OP_END) ensure_registers(program, instr.b);
    if (instr.c < BC_MAX_REGISTERS && instr.op != BC_OP_CALL_NATIVE) ensure_registers(program, instr.c);
    return index;
}

size_t bc_emit_abc(BcProgram *program, BcOp op, uint16_t a, uint16_t b, uint16_t c, BcSourceSpan span) {
    return bc_emit(program, (BcInstr){(uint8_t)op, a, b, c, 0}, span);
}

size_t bc_emit_const(BcProgram *program, uint16_t dst, uint32_t constant, BcSourceSpan span) {
    return bc_emit(program, (BcInstr){BC_OP_CONST, dst, 0, 0, constant}, span);
}

size_t bc_emit_return(BcProgram *program, uint16_t src, BcSourceSpan span) {
    return bc_emit(program, (BcInstr){BC_OP_RETURN, src, 0, 0, 0}, span);
}

bool bc_verify(const BcProgram *program, BcError *error) {
    bc_error_clear(error);
    BcType *types = calloc(program->register_count ? program->register_count : 1, sizeof(BcType));
    if (!types) bc_die_oom(program->register_count * sizeof(BcType));
    VerifyFrame frames[BC_STRUCTURED_DEPTH_MAX];
    size_t depth = 0;
    bool terminated = false;

    for (size_t i = 0; i < program->code_count; i++) {
        BcInstr in = program->code[i];
        if (in.op >= BC_OP_COUNT) {
            bc_errorf(error, program, i, in.op, "invalid opcode %u", in.op);
            free(types);
            return false;
        }

        BcOp op = (BcOp)in.op;
        if (terminated && op != BC_OP_END && op != BC_OP_ELSE) {
            bc_errorf(error, program, i, in.op, "unreachable instruction after return/halt");
            free(types);
            return false;
        }

        switch (op) {
            case BC_OP_NOP:
                break;
            case BC_OP_CONST:
                if (in.imm >= program->const_count) {
                    bc_errorf(error, program, i, in.op, "constant index %u out of range", in.imm);
                    free(types);
                    return false;
                }
                types[in.a] = bc_type_of_value(program->constants[in.imm]);
                break;
            case BC_OP_NIL:
                types[in.a] = BC_TYPE_NIL;
                break;
            case BC_OP_BOOL:
                types[in.a] = BC_TYPE_BOOL;
                break;
            case BC_OP_MOV:
                if (types[in.b] == BC_TYPE_UNKNOWN) goto unknown_operand;
                types[in.a] = types[in.b];
                break;
            case BC_OP_ADD:
            case BC_OP_SUB:
            case BC_OP_MUL:
            case BC_OP_DIV:
            case BC_OP_MOD:
                if (!same_numeric(types[in.b], types[in.c])) goto type_error;
                if (op == BC_OP_MOD && (types[in.b] != BC_TYPE_I64 || types[in.c] != BC_TYPE_I64)) goto type_error;
                types[in.a] = numeric_result(types[in.b], types[in.c]);
                break;
            case BC_OP_NEG:
                if (types[in.b] != BC_TYPE_I64 && types[in.b] != BC_TYPE_F64) goto type_error;
                types[in.a] = types[in.b];
                break;
            case BC_OP_EQ:
                if (types[in.b] == BC_TYPE_UNKNOWN || types[in.c] == BC_TYPE_UNKNOWN) goto unknown_operand;
                types[in.a] = BC_TYPE_BOOL;
                break;
            case BC_OP_LT:
            case BC_OP_LE:
            case BC_OP_GT:
            case BC_OP_GE:
                if (!same_numeric(types[in.b], types[in.c])) goto type_error;
                types[in.a] = BC_TYPE_BOOL;
                break;
            case BC_OP_NOT:
                if (types[in.b] == BC_TYPE_UNKNOWN) goto unknown_operand;
                types[in.a] = BC_TYPE_BOOL;
                break;
            case BC_OP_CALL_NATIVE:
                if (in.imm >= program->native_count) {
                    bc_errorf(error, program, i, in.op, "native index %u out of range", in.imm);
                    free(types);
                    return false;
                }
                if (in.c < program->natives[in.imm].min_arity || in.c > program->natives[in.imm].max_arity) {
                    bc_errorf(error, program, i, in.op, "native arity %u outside accepted range", in.c);
                    free(types);
                    return false;
                }
                for (uint16_t r = 0; r < in.c; r++) {
                    if ((size_t)in.b + r >= program->register_count || types[in.b + r] == BC_TYPE_UNKNOWN) goto unknown_operand;
                }
                types[in.a] = program->natives[in.imm].return_type;
                break;
            case BC_OP_BLOCK:
                if (depth == BC_STRUCTURED_DEPTH_MAX) {
                    bc_errorf(error, program, i, in.op, "structured block nesting too deep");
                    free(types);
                    return false;
                }
                frames[depth++] = (VerifyFrame){FRAME_BLOCK, i};
                break;
            case BC_OP_IF:
                if (types[in.a] != BC_TYPE_BOOL) {
                    bc_errorf(error, program, i, in.op, "if condition register r%u has type %s", in.a, bc_type_name(types[in.a]));
                    free(types);
                    return false;
                }
                if (depth == BC_STRUCTURED_DEPTH_MAX) {
                    bc_errorf(error, program, i, in.op, "structured block nesting too deep");
                    free(types);
                    return false;
                }
                frames[depth++] = (VerifyFrame){FRAME_IF, i};
                terminated = false;
                break;
            case BC_OP_ELSE:
                if (!depth || frames[depth - 1].kind != FRAME_IF) {
                    bc_errorf(error, program, i, in.op, "else without matching if");
                    free(types);
                    return false;
                }
                frames[depth - 1].kind = FRAME_ELSE;
                terminated = false;
                break;
            case BC_OP_LOOP:
                if (types[in.a] != BC_TYPE_I64) {
                    bc_errorf(error, program, i, in.op, "loop count register r%u has type %s", in.a, bc_type_name(types[in.a]));
                    free(types);
                    return false;
                }
                if (depth == BC_STRUCTURED_DEPTH_MAX) {
                    bc_errorf(error, program, i, in.op, "structured block nesting too deep");
                    free(types);
                    return false;
                }
                frames[depth++] = (VerifyFrame){FRAME_LOOP, i};
                terminated = false;
                break;
            case BC_OP_END:
                if (!depth) {
                    bc_errorf(error, program, i, in.op, "end without matching block");
                    free(types);
                    return false;
                }
                depth--;
                terminated = false;
                break;
            case BC_OP_RETURN:
                if (types[in.a] == BC_TYPE_UNKNOWN) goto unknown_operand;
                terminated = true;
                break;
            case BC_OP_HALT:
                terminated = true;
                break;
            case BC_OP_COUNT:
                break;
        }
        continue;

unknown_operand:
        bc_errorf(error, program, i, in.op, "instruction %s reads an unknown register type", bc_op_name(op));
        free(types);
        return false;
type_error:
        bc_errorf(error, program, i, in.op, "type mismatch in %s: r%u=%s r%u=%s",
                  bc_op_name(op), in.b, bc_type_name(types[in.b]), in.c, bc_type_name(types[in.c]));
        free(types);
        return false;
    }

    if (depth) {
        bc_errorf(error, program, frames[depth - 1].start, program->code[frames[depth - 1].start].op, "unclosed structured block");
        free(types);
        return false;
    }
    if (program->code_count && !terminated) {
        size_t last = program->code_count - 1;
        bc_errorf(error, program, last, program->code[last].op, "program does not end in return or halt");
        free(types);
        return false;
    }
    free(types);
    return true;
}

static void trace_indent(FILE *out, size_t depth) {
    for (size_t i = 0; i < depth; i++) fprintf(out, "│   ");
}

static void trace_reg_type(FILE *out, size_t depth, size_t loc_col, uint16_t reg, BcType type) {
    print_location_blank(out, loc_col);
    trace_indent(out, depth);
    fprintf(out, "╰─ r%u : %s\n", reg, bc_type_name(type));
}

bool bc_verify_trace(const BcProgram *program, FILE *out, BcError *error) {
    if (!out) out = stderr;
    bc_error_clear(error);
    fprintf(out, "Bytecode verify %s\n",
            program->name ? program->name : "<bytecode>");

    size_t loc_col = program_location_column_width(program);
    BcType *types = calloc(program->register_count ? program->register_count : 1, sizeof(BcType));
    if (!types) bc_die_oom(program->register_count * sizeof(BcType));
    VerifyFrame frames[BC_STRUCTURED_DEPTH_MAX];
    size_t depth = 0;
    bool terminated = false;
    bool ok = true;

    for (size_t i = 0; i < program->code_count && ok; i++) {
        BcInstr in = program->code[i];
        if (in.op >= BC_OP_COUNT) {
            bc_errorf(error, program, i, in.op, "invalid opcode %u", in.op);
            ok = false;
            break;
        }

        BcOp op = (BcOp)in.op;
        BcSourceSpan span = instr_span(program, i);
        print_location_field(out, span, loc_col);

        trace_indent(out, depth);
        fprintf(out, "├─ ");
        print_dim(out, "%04zu ", i);
        print_padded_op(out, op, 14);
        if (op == BC_OP_CONST) fprintf(out, "r%u const[%u]", in.a, in.imm);
        else if (op == BC_OP_IF) fprintf(out, "r%u", in.a);
        else if (op == BC_OP_RETURN) fprintf(out, "r%u", in.a);
        else if (op == BC_OP_MOV || op == BC_OP_NEG || op == BC_OP_NOT) fprintf(out, "r%u <- r%u", in.a, in.b);
        else if (op == BC_OP_ADD || op == BC_OP_SUB || op == BC_OP_MUL ||
                 op == BC_OP_DIV || op == BC_OP_MOD || op == BC_OP_EQ ||
                 op == BC_OP_LT || op == BC_OP_LE || op == BC_OP_GT || op == BC_OP_GE)
            fprintf(out, "r%u <- r%u r%u", in.a, in.b, in.c);
        fputc('\n', out);

        if (terminated && op != BC_OP_END && op != BC_OP_ELSE) {
            bc_errorf(error, program, i, in.op, "unreachable instruction after return/halt");
            ok = false;
            break;
        }

        switch (op) {
            case BC_OP_NOP:
                break;
            case BC_OP_CONST:
                if (in.imm >= program->const_count) {
                    bc_errorf(error, program, i, in.op, "constant index %u out of range", in.imm);
                    ok = false;
                    break;
                }
                types[in.a] = bc_type_of_value(program->constants[in.imm]);
                trace_reg_type(out, depth + 1, loc_col, in.a, types[in.a]);
                break;
            case BC_OP_NIL:
                types[in.a] = BC_TYPE_NIL;
                trace_reg_type(out, depth + 1, loc_col, in.a, types[in.a]);
                break;
            case BC_OP_BOOL:
                types[in.a] = BC_TYPE_BOOL;
                trace_reg_type(out, depth + 1, loc_col, in.a, types[in.a]);
                break;
            case BC_OP_MOV:
                if (types[in.b] == BC_TYPE_UNKNOWN) goto trace_unknown_operand;
                types[in.a] = types[in.b];
                trace_reg_type(out, depth + 1, loc_col, in.a, types[in.a]);
                break;
            case BC_OP_ADD:
            case BC_OP_SUB:
            case BC_OP_MUL:
            case BC_OP_DIV:
            case BC_OP_MOD:
                if (!same_numeric(types[in.b], types[in.c])) goto trace_type_error;
                if (op == BC_OP_MOD && (types[in.b] != BC_TYPE_I64 || types[in.c] != BC_TYPE_I64)) goto trace_type_error;
                types[in.a] = numeric_result(types[in.b], types[in.c]);
                trace_reg_type(out, depth + 1, loc_col, in.a, types[in.a]);
                break;
            case BC_OP_NEG:
                if (types[in.b] != BC_TYPE_I64 && types[in.b] != BC_TYPE_F64) goto trace_type_error;
                types[in.a] = types[in.b];
                trace_reg_type(out, depth + 1, loc_col, in.a, types[in.a]);
                break;
            case BC_OP_EQ:
                if (types[in.b] == BC_TYPE_UNKNOWN || types[in.c] == BC_TYPE_UNKNOWN) goto trace_unknown_operand;
                types[in.a] = BC_TYPE_BOOL;
                trace_reg_type(out, depth + 1, loc_col, in.a, types[in.a]);
                break;
            case BC_OP_LT:
            case BC_OP_LE:
            case BC_OP_GT:
            case BC_OP_GE:
                if (!same_numeric(types[in.b], types[in.c])) goto trace_type_error;
                types[in.a] = BC_TYPE_BOOL;
                trace_reg_type(out, depth + 1, loc_col, in.a, types[in.a]);
                break;
            case BC_OP_NOT:
                if (types[in.b] == BC_TYPE_UNKNOWN) goto trace_unknown_operand;
                types[in.a] = BC_TYPE_BOOL;
                trace_reg_type(out, depth + 1, loc_col, in.a, types[in.a]);
                break;
            case BC_OP_CALL_NATIVE:
                if (in.imm >= program->native_count) {
                    bc_errorf(error, program, i, in.op, "native index %u out of range", in.imm);
                    ok = false;
                    break;
                }
                if (in.c < program->natives[in.imm].min_arity || in.c > program->natives[in.imm].max_arity) {
                    bc_errorf(error, program, i, in.op, "native arity %u outside accepted range", in.c);
                    ok = false;
                    break;
                }
                for (uint16_t r = 0; r < in.c; r++)
                    if ((size_t)in.b + r >= program->register_count || types[in.b + r] == BC_TYPE_UNKNOWN)
                        goto trace_unknown_operand;
                types[in.a] = program->natives[in.imm].return_type;
                trace_reg_type(out, depth + 1, loc_col, in.a, types[in.a]);
                break;

            case BC_OP_BLOCK:
                if (depth == BC_STRUCTURED_DEPTH_MAX) {
                    bc_errorf(error, program, i, in.op, "structured block nesting too deep");
                    ok = false;
                    break;
                }
                frames[depth++] = (VerifyFrame){FRAME_BLOCK, i};
                break;
            case BC_OP_IF:
                if (types[in.a] != BC_TYPE_BOOL) {
                    bc_errorf(error, program, i, in.op, "if condition register r%u has type %s", in.a, bc_type_name(types[in.a]));
                    ok = false;
                    break;
                }
                if (depth == BC_STRUCTURED_DEPTH_MAX) {
                    bc_errorf(error, program, i, in.op, "structured block nesting too deep");
                    ok = false;
                    break;
                }
                frames[depth++] = (VerifyFrame){FRAME_IF, i};
                terminated = false;
                break;
            case BC_OP_ELSE:
                if (!depth || frames[depth - 1].kind != FRAME_IF) {
                    bc_errorf(error, program, i, in.op, "else without matching if");
                    ok = false;
                    break;
                }
                frames[depth - 1].kind = FRAME_ELSE;
                terminated = false;
                break;
            case BC_OP_LOOP:
                if (types[in.a] != BC_TYPE_I64) {
                    bc_errorf(error, program, i, in.op, "loop count register r%u has type %s", in.a, bc_type_name(types[in.a]));
                    ok = false;
                    break;
                }
                if (depth == BC_STRUCTURED_DEPTH_MAX) {
                    bc_errorf(error, program, i, in.op, "structured block nesting too deep");
                    ok = false;
                    break;
                }
                frames[depth++] = (VerifyFrame){FRAME_LOOP, i};
                terminated = false;
                break;
            case BC_OP_END:
                if (!depth) {
                    bc_errorf(error, program, i, in.op, "end without matching block");
                    ok = false;
                    break;
                }
                depth--;
                terminated = false;
                break;
            case BC_OP_RETURN:
                if (types[in.a] == BC_TYPE_UNKNOWN) goto trace_unknown_operand;
                terminated = true;
                break;
            case BC_OP_HALT:
                terminated = true;
                break;
            case BC_OP_COUNT:
                break;
        }
        continue;

trace_unknown_operand:
        bc_errorf(error, program, i, in.op, "instruction %s reads an unknown register type", bc_op_name(op));
        ok = false;
        break;
trace_type_error:
        bc_errorf(error, program, i, in.op, "type mismatch in %s: r%u=%s r%u=%s",
                  bc_op_name(op), in.b, bc_type_name(types[in.b]), in.c, bc_type_name(types[in.c]));
        ok = false;
        break;
    }

    if (ok && depth) {
        bc_errorf(error, program, frames[depth - 1].start, program->code[frames[depth - 1].start].op, "unclosed structured block");
        ok = false;
    }
    if (ok && program->code_count && !terminated) {
        size_t last = program->code_count - 1;
        bc_errorf(error, program, last, program->code[last].op, "program does not end in return or halt");
        ok = false;
    }

    print_location_blank(out, loc_col);
    fprintf(out, "└─ ");
    print_status(out, ok);
    fputc('\n', out);
    free(types);
    return ok;
}

static void write_code_section(const BcProgram *program, FILE *out, bool *ok) {
    write_leb_u64(out, program->code_count, ok);
    for (size_t i = 0; i < program->code_count; i++) {
        BcInstr in = program->code[i];
        write_leb_u64(out, in.op, ok);
        write_leb_u64(out, in.a, ok);
        write_leb_u64(out, in.b, ok);
        write_leb_u64(out, in.c, ok);
        write_leb_u64(out, in.imm, ok);
    }
}

static void write_constants_section(const BcProgram *program, FILE *out, bool *ok) {
    write_leb_u64(out, program->const_count, ok);
    for (size_t i = 0; i < program->const_count; i++) {
        BcValue v = program->constants[i];
        write_leb_u64(out, v.kind, ok);
        switch (v.kind) {
            case BC_VALUE_NIL: break;
            case BC_VALUE_BOOL: write_leb_u64(out, v.as.boolean ? 1 : 0, ok); break;
            case BC_VALUE_I64: write_i64(out, v.as.i64, ok); break;
            case BC_VALUE_F64: write_f64(out, v.as.f64, ok); break;
            case BC_VALUE_PTR: write_leb_u64(out, 0, ok); break;
        }
    }
}

static void write_types_section(const BcProgram *program, FILE *out, bool *ok) {
    write_leb_u64(out, program->register_count, ok);
    for (size_t i = 0; i < program->register_count; i++) write_leb_u64(out, program->register_types[i], ok);
}

static void write_debug_section(const BcProgram *program, FILE *out, bool *ok) {
    write_leb_u64(out, program->code_count, ok);
    for (size_t i = 0; i < program->code_count; i++) {
        BcSourceSpan span = bc_span("", 0, 0);
        if (program->debug_mode == BC_DEBUG_FULL_SPANS && program->spans)
            span = program->spans[i];
        else if (program->debug_mode == BC_DEBUG_LINES && program->lines)
            span.line = program->lines[i];
        const char *file = span.file ? span.file : "";
        size_t len = strlen(file);
        write_leb_u64(out, len, ok);
        for (size_t j = 0; j < len; j++) write_byte(out, (uint8_t)file[j], ok);
        write_leb_u64(out, span.line, ok);
        write_leb_u64(out, span.column, ok);
    }
}

static void write_deopt_section(const BcProgram *program, FILE *out, bool *ok) {
    write_leb_u64(out, program->deopt_count, ok);
    for (size_t i = 0; i < program->deopt_count; i++) {
        BcDeoptPoint d = program->deopts[i];
        write_leb_u64(out, d.safepoint, ok);
        write_leb_u64(out, d.first_register, ok);
        write_leb_u64(out, d.register_count, ok);
        write_leb_u64(out, d.source_id, ok);
    }
}

static void write_ic_section(const BcProgram *program, FILE *out, bool *ok) {
    write_leb_u64(out, program->inline_cache_count, ok);
    for (size_t i = 0; i < program->inline_cache_count; i++) {
        BcInlineCache ic = program->inline_caches[i];
        write_leb_u64(out, ic.callsite, ok);
        write_leb_u64(out, ic.receiver_type, ok);
        write_leb_u64(out, ic.target, ok);
        write_leb_u64(out, ic.misses, ok);
    }
}

bool bc_write_binary(const BcProgram *program, FILE *out, BcError *error) {
    bc_error_clear(error);
    BcError verify_error;
    if (!bc_verify(program, &verify_error)) {
        if (error) *error = verify_error;
        return false;
    }

    bool ok = true;
    write_leb_u64(out, BC_MAGIC, &ok);
    write_leb_u64(out, BC_VERSION_MAJOR, &ok);
    write_leb_u64(out, BC_VERSION_MINOR, &ok);
    write_leb_u64(out, 6, &ok);
    if (!write_section(program, out, BC_SECTION_CODE, write_code_section, error)) return false;
    if (!write_section(program, out, BC_SECTION_CONSTANTS, write_constants_section, error)) return false;
    if (!write_section(program, out, BC_SECTION_TYPES, write_types_section, error)) return false;
    if (!write_section(program, out, BC_SECTION_DEBUG, write_debug_section, error)) return false;
    if (!write_section(program, out, BC_SECTION_DEOPT, write_deopt_section, error)) return false;
    if (!write_section(program, out, BC_SECTION_IC, write_ic_section, error)) return false;
    if (!ok) {
        bc_errorf(error, program, 0, 0, "failed writing bytecode header");
        return false;
    }
    return true;
}

static void bc_binary_error(BcError *error, const char *message) {
    if (!error) return;
    bc_error_clear(error);
    snprintf(error->message, sizeof(error->message), "%s", message);
}

bool bc_read_binary_info(FILE *in, BcBinaryInfo *info, BcError *error) {
    bc_error_clear(error);
    if (!in || !info) {
        bc_binary_error(error, "bytecode binary info requires input and output");
        return false;
    }
    memset(info, 0, sizeof(*info));

    uint64_t magic = 0, major = 0, minor = 0, sections = 0;
    if (!read_leb_u64(in, &magic) ||
        !read_leb_u64(in, &major) ||
        !read_leb_u64(in, &minor) ||
        !read_leb_u64(in, &sections)) {
        bc_binary_error(error, "truncated bytecode binary header");
        return false;
    }
    if (magic != BC_MAGIC) {
        bc_binary_error(error, "invalid bytecode magic");
        return false;
    }

    info->major = (uint32_t)major;
    info->minor = (uint32_t)minor;
    info->section_count = sections;

    for (uint64_t i = 0; i < sections; i++) {
        uint64_t id = 0, size = 0;
        if (!read_leb_u64(in, &id) || !read_leb_u64(in, &size)) {
            bc_binary_error(error, "truncated bytecode section header");
            return false;
        }
        bool known = id <= UINT32_MAX && section_is_known((uint32_t)id);
        if (!known) info->unknown_section_count++;
        info->total_payload_bytes += size;
        if (info->stored_section_count < sizeof(info->sections) / sizeof(info->sections[0])) {
            BcSectionInfo *slot = &info->sections[info->stored_section_count++];
            slot->id = id <= UINT32_MAX ? (uint32_t)id : BC_SECTION_UNKNOWN;
            slot->size = size;
            slot->known = known;
        }
        if (!skip_bytes(in, size)) {
            bc_binary_error(error, "truncated bytecode section payload");
            return false;
        }
    }
    return true;
}

bool bc_dump_binary_sections(FILE *in, FILE *out, BcError *error) {
    if (!out) out = stdout;
    BcBinaryInfo info;
    if (!bc_read_binary_info(in, &info, error)) return false;
    fprintf(out, "== bytecode sections v%u.%u ==\n", info.major, info.minor);
    fprintf(out, "sections=%" PRIu64 " payload=%" PRIu64 " unknown=%" PRIu64 "\n",
            info.section_count, info.total_payload_bytes, info.unknown_section_count);
    for (size_t i = 0; i < info.stored_section_count; i++) {
        BcSectionInfo section = info.sections[i];
        fprintf(out, "section %u %-12s size=%" PRIu64 "%s\n",
                section.id,
                section_name(section.id),
                section.size,
                section.known ? "" : " skipped");
    }
    if (info.stored_section_count < info.section_count)
        fprintf(out, "... %" PRIu64 " more section(s) not shown\n",
                info.section_count - (uint64_t)info.stored_section_count);
    return true;
}

BcOptimizeOptions bc_optimize_options_default(void) {
    BcOptimizeOptions options;
    options.fold_constants = true;
    options.remove_self_moves = true;
    options.compact_nops = true;
    options.verify_after = true;
    return options;
}

static void clear_const_facts(ConstFact *facts, size_t count) {
    for (size_t i = 0; i < count; i++) facts[i].known = false;
}

static void set_const_fact(ConstFact *facts, size_t count, uint16_t reg, BcValue value) {
    if ((size_t)reg >= count) return;
    facts[reg].known = true;
    facts[reg].value = value;
}

static void clear_const_fact(ConstFact *facts, size_t count, uint16_t reg) {
    if ((size_t)reg < count) facts[reg].known = false;
}

static bool get_const_fact(const ConstFact *facts, size_t count, uint16_t reg, BcValue *value) {
    if ((size_t)reg >= count || !facts[reg].known) return false;
    *value = facts[reg].value;
    return true;
}

bool bc_optimize_program(BcProgram *program,
                         const BcOptimizeOptions *options,
                         BcOptimizeReport *report,
                         BcError *error) {
    bc_error_clear(error);
    if (!program) {
        bc_errorf(error, program, 0, 0, "bytecode optimizer requires a program");
        return false;
    }
    BcOptimizeOptions local = options ? *options : bc_optimize_options_default();
    BcOptimizeReport r = {0};
    r.before_instructions = program->code_count;

    BcError verify_error;
    if (!bc_verify(program, &verify_error)) {
        if (error) *error = verify_error;
        if (report) *report = r;
        return false;
    }

    ConstFact *facts = calloc(program->register_count ? program->register_count : 1, sizeof(facts[0]));
    if (!facts) bc_die_oom(program->register_count * sizeof(facts[0]));

    for (size_t i = 0; i < program->code_count; i++) {
        BcInstr *in = &program->code[i];
        BcValue left, right, folded;
        switch ((BcOp)in->op) {
            case BC_OP_CONST:
                if (in->imm < program->const_count) {
                    if (local.fold_constants && get_const_fact(facts, program->register_count, in->a, &left) &&
                        value_equal(left, program->constants[in->imm])) {
                        in->op = BC_OP_NOP;
                        r.nops_removed++;
                    } else {
                        set_const_fact(facts, program->register_count, in->a, program->constants[in->imm]);
                    }
                }
                break;
            case BC_OP_NIL:
                set_const_fact(facts, program->register_count, in->a, bc_value_nil());
                break;
            case BC_OP_BOOL:
                set_const_fact(facts, program->register_count, in->a, bc_value_bool(in->imm != 0));
                break;
            case BC_OP_MOV:
                if (local.remove_self_moves && in->a == in->b) {
                    in->op = BC_OP_NOP;
                    r.moves_eliminated++;
                    r.nops_removed++;
                    break;
                }
                if (get_const_fact(facts, program->register_count, in->b, &left))
                    set_const_fact(facts, program->register_count, in->a, left);
                else
                    clear_const_fact(facts, program->register_count, in->a);
                break;
            case BC_OP_ADD:
            case BC_OP_SUB:
            case BC_OP_MUL:
            case BC_OP_DIV:
            case BC_OP_MOD:
            case BC_OP_EQ:
            case BC_OP_LT:
            case BC_OP_LE:
            case BC_OP_GT:
            case BC_OP_GE:
                if (local.fold_constants &&
                    get_const_fact(facts, program->register_count, in->b, &left) &&
                    get_const_fact(facts, program->register_count, in->c, &right) &&
                    value_i64_binary((BcOp)in->op, left, right, &folded)) {
                    uint32_t constant = bc_program_add_const(program, folded);
                    r.constants_added++;
                    in->op = BC_OP_CONST;
                    in->b = 0;
                    in->c = 0;
                    in->imm = constant;
                    set_const_fact(facts, program->register_count, in->a, folded);
                    r.constants_folded++;
                } else {
                    clear_const_fact(facts, program->register_count, in->a);
                }
                break;
            case BC_OP_NEG:
                if (local.fold_constants &&
                    get_const_fact(facts, program->register_count, in->b, &left) &&
                    left.kind == BC_VALUE_I64) {
                    folded = bc_value_i64(-left.as.i64);
                    uint32_t constant = bc_program_add_const(program, folded);
                    r.constants_added++;
                    in->op = BC_OP_CONST;
                    in->b = 0;
                    in->c = 0;
                    in->imm = constant;
                    set_const_fact(facts, program->register_count, in->a, folded);
                    r.constants_folded++;
                } else {
                    clear_const_fact(facts, program->register_count, in->a);
                }
                break;
            case BC_OP_NOT:
                if (local.fold_constants &&
                    get_const_fact(facts, program->register_count, in->b, &left)) {
                    folded = bc_value_bool(!bc_value_truthy(left));
                    uint32_t constant = bc_program_add_const(program, folded);
                    r.constants_added++;
                    in->op = BC_OP_CONST;
                    in->b = 0;
                    in->c = 0;
                    in->imm = constant;
                    set_const_fact(facts, program->register_count, in->a, folded);
                    r.constants_folded++;
                } else {
                    clear_const_fact(facts, program->register_count, in->a);
                }
                break;
            case BC_OP_CALL_NATIVE:
                clear_const_fact(facts, program->register_count, in->a);
                break;
            case BC_OP_BLOCK:
            case BC_OP_IF:
            case BC_OP_ELSE:
            case BC_OP_LOOP:
            case BC_OP_END:
                clear_const_facts(facts, program->register_count);
                break;
            case BC_OP_RETURN:
            case BC_OP_HALT:
            case BC_OP_NOP:
            case BC_OP_COUNT:
                break;
        }
    }
    free(facts);

    if (local.compact_nops && r.nops_removed) {
        size_t out = 0;
        for (size_t i = 0; i < program->code_count; i++) {
            if (program->code[i].op == BC_OP_NOP) continue;
            if (out != i) {
                program->code[out] = program->code[i];
                if (program->debug_mode == BC_DEBUG_FULL_SPANS && program->spans)
                    program->spans[out] = program->spans[i];
                else if (program->debug_mode == BC_DEBUG_LINES && program->lines)
                    program->lines[out] = program->lines[i];
            }
            out++;
        }
        program->code_count = out;
    }

    r.after_instructions = program->code_count;
    if (local.verify_after && !bc_verify(program, &verify_error)) {
        if (error) *error = verify_error;
        if (report) *report = r;
        return false;
    }
    if (report) *report = r;
    return true;
}

static void print_optimize_instr_row(FILE *out, size_t i, BcInstr in, BcSourceSpan span) {
    fprintf(out, "%s│   ├─ %s", BC_CLR_GRAY, BC_CLR_RESET);
    print_dim(out, "%04zu ", i);
    print_padded_op(out, (BcOp)in.op, 14);
    fprintf(out, "r%u r%u r%u %simm=%u%s", in.a, in.b, in.c, BC_CLR_MAGENTA, in.imm, BC_CLR_RESET);
    if (span.line) fprintf(out, "%s @ %u:%u%s", BC_CLR_GREEN, span.line, span.column, BC_CLR_RESET);
    fputc('\n', out);
}

bool bc_optimize_trace(BcProgram *program,
                       const BcOptimizeOptions *options,
                       BcOptimizeReport *report,
                       FILE *out,
                       BcError *error) {
    if (!out) out = stderr;
    const char *name = program && program->name ? program->name : "<bytecode>";
    fprintf(out, "%sBytecode optimize %s%s\n", BC_CLR_BOLD_CYAN, name, BC_CLR_RESET);
    if (!program) {
        bc_errorf(error, program, 0, 0, "bytecode optimizer requires a program");
        fprintf(out, "%s└─ %s", BC_CLR_GRAY, BC_CLR_RESET);
        print_status(out, false);
        fprintf(out, " null program\n");
        return false;
    }

    BcOptimizeOptions local = options ? *options : bc_optimize_options_default();
    BcOptimizeReport r = {0};
    size_t before = program->code_count;
    fprintf(out, "%s├─ %sbefore\n", BC_CLR_GRAY, BC_CLR_RESET);
    for (size_t i = 0; i < program->code_count; i++) {
        print_optimize_instr_row(out, i, program->code[i], instr_span(program, i));
    }

    bool ok = bc_optimize_program(program, &local, &r, error);
    fprintf(out, "%s├─ %spasses\n", BC_CLR_GRAY, BC_CLR_RESET);
    fprintf(out, "%s│   ├─ %sconstant folds: %zu\n", BC_CLR_GRAY, BC_CLR_RESET, r.constants_folded);
    fprintf(out, "%s│   ├─ %sdead/self moves: %zu\n", BC_CLR_GRAY, BC_CLR_RESET, r.moves_eliminated);
    fprintf(out, "%s│   ├─ %snops removed: %zu\n", BC_CLR_GRAY, BC_CLR_RESET, r.nops_removed);
    fprintf(out, "%s│   └─ %sconstants added: %zu\n", BC_CLR_GRAY, BC_CLR_RESET, r.constants_added);
    fprintf(out, "%s├─ %sinstructions %zu -> %zu\n", BC_CLR_GRAY, BC_CLR_RESET, before, program->code_count);
    fprintf(out, "%s├─ %safter\n", BC_CLR_GRAY, BC_CLR_RESET);
    for (size_t i = 0; i < program->code_count; i++) {
        print_optimize_instr_row(out, i, program->code[i], bc_span(NULL, 0, 0));
    }

    fprintf(out, "%s└─ %s", BC_CLR_GRAY, BC_CLR_RESET);
    print_status(out, ok);
    fputc('\n', out);
    if (report) *report = r;
    return ok;
}

BcJitOptions bc_jit_options_default(void) {
    BcJitOptions options;
    options.tier = BC_JIT_BASELINE_TEMPLATE;
    options.enable_inline_caches = true;
    options.require_deopt_maps = true;
    options.trace = false;
    return options;
}

bool bc_jit_compile_baseline(const BcProgram *program,
                             const BcJitOptions *options,
                             BcJitArtifact *artifact,
                             BcError *error) {
    if (artifact) memset(artifact, 0, sizeof(*artifact));
    if (artifact && options) artifact->tier = options->tier;

    BcError verify_error;
    if (!bc_verify(program, &verify_error)) {
        if (error) *error = verify_error;
        return false;
    }

#if !BC_HAS_X64_JIT
    bc_errorf(error, program, 0, 0, "baseline JIT is not available on this host architecture");
    return false;
#else
    if (!artifact) {
        bc_errorf(error, program, 0, 0, "baseline JIT requires an output artifact");
        return false;
    }

    BcType *types = calloc(program->register_count ? program->register_count : 1, sizeof(BcType));
    if (!types) bc_die_oom(program->register_count * sizeof(BcType));
    CodeBuf cb = {0};
    bool returned = false;

    size_t frame_bytes = program->register_count ? program->register_count * 8 : 8;
    frame_bytes = (frame_bytes + 15u) & ~(size_t)15u;

    cb_emit_u8(&cb, 0x55);                         /* push rbp */
    cb_emit_u8(&cb, 0x48); cb_emit_u8(&cb, 0x89); cb_emit_u8(&cb, 0xe5); /* mov rbp,rsp */
    x64_emit_probed_stack_alloc(&cb, frame_bytes);

    for (size_t i = 0; i < program->code_count; i++) {
        BcInstr in = program->code[i];
        switch ((BcOp)in.op) {
            case BC_OP_NOP:
            case BC_OP_BLOCK:
            case BC_OP_END:
                break;
            case BC_OP_CONST:
                if (program->constants[in.imm].kind != BC_VALUE_I64 &&
                    program->constants[in.imm].kind != BC_VALUE_BOOL &&
                    program->constants[in.imm].kind != BC_VALUE_NIL) {
                    bc_errorf(error, program, i, in.op, "baseline JIT supports only I64/Bool/Nil constants");
                    free(types); free(cb.data); return false;
                }
                types[in.a] = bc_type_of_value(program->constants[in.imm]);
                if (program->constants[in.imm].kind == BC_VALUE_BOOL)
                    x64_mov_rax_imm64(&cb, program->constants[in.imm].as.boolean ? 1 : 0);
                else if (program->constants[in.imm].kind == BC_VALUE_NIL)
                    x64_mov_rax_imm64(&cb, 0);
                else
                    x64_mov_rax_imm64(&cb, program->constants[in.imm].as.i64);
                x64_mov_slot_rax(&cb, in.a);
                break;
            case BC_OP_NIL:
                types[in.a] = BC_TYPE_NIL;
                x64_mov_rax_imm64(&cb, 0);
                x64_mov_slot_rax(&cb, in.a);
                break;
            case BC_OP_BOOL:
                types[in.a] = BC_TYPE_BOOL;
                x64_mov_rax_imm64(&cb, in.imm ? 1 : 0);
                x64_mov_slot_rax(&cb, in.a);
                break;
            case BC_OP_MOV:
                types[in.a] = types[in.b];
                x64_mov_rax_slot(&cb, in.b);
                x64_mov_slot_rax(&cb, in.a);
                break;
            case BC_OP_ADD:
                types[in.a] = numeric_result(types[in.b], types[in.c]);
                x64_mov_rax_slot(&cb, in.b);
                x64_binop_slot(&cb, 0x03, in.c); /* add rax, mem */
                x64_mov_slot_rax(&cb, in.a);
                break;
            case BC_OP_SUB:
                types[in.a] = numeric_result(types[in.b], types[in.c]);
                x64_mov_rax_slot(&cb, in.b);
                x64_binop_slot(&cb, 0x2b, in.c); /* sub rax, mem */
                x64_mov_slot_rax(&cb, in.a);
                break;
            case BC_OP_MUL:
                types[in.a] = numeric_result(types[in.b], types[in.c]);
                x64_mov_rax_slot(&cb, in.b);
                cb_emit_u8(&cb, 0x48); cb_emit_u8(&cb, 0x0f); cb_emit_u8(&cb, 0xaf); cb_emit_u8(&cb, 0x85);
                cb_emit_i32(&cb, reg_slot(in.c)); /* imul rax, mem */
                x64_mov_slot_rax(&cb, in.a);
                break;
            case BC_OP_DIV:
            case BC_OP_MOD:
                types[in.a] = BC_TYPE_I64;
                x64_mov_rax_slot(&cb, in.b);
                cb_emit_u8(&cb, 0x48); cb_emit_u8(&cb, 0x99); /* cqo */
                cb_emit_u8(&cb, 0x48); cb_emit_u8(&cb, 0xf7); cb_emit_u8(&cb, 0xbd);
                cb_emit_i32(&cb, reg_slot(in.c)); /* idiv qword [rbp+disp] */
                if (in.op == BC_OP_MOD) {
                    cb_emit_u8(&cb, 0x48); cb_emit_u8(&cb, 0x89); cb_emit_u8(&cb, 0xd0); /* mov rax,rdx */
                }
                x64_mov_slot_rax(&cb, in.a);
                break;
            case BC_OP_NEG:
                types[in.a] = types[in.b];
                x64_mov_rax_slot(&cb, in.b);
                cb_emit_u8(&cb, 0x48); cb_emit_u8(&cb, 0xf7); cb_emit_u8(&cb, 0xd8); /* neg rax */
                x64_mov_slot_rax(&cb, in.a);
                break;
            case BC_OP_EQ:
            case BC_OP_LT:
            case BC_OP_LE:
            case BC_OP_GT:
            case BC_OP_GE: {
                types[in.a] = BC_TYPE_BOOL;
                x64_mov_rax_slot(&cb, in.b);
                cb_emit_u8(&cb, 0x48); cb_emit_u8(&cb, 0x3b); cb_emit_u8(&cb, 0x85);
                cb_emit_i32(&cb, reg_slot(in.c)); /* cmp rax, mem */
                uint8_t cc = in.op == BC_OP_EQ ? 0x94 : in.op == BC_OP_LT ? 0x9c :
                    in.op == BC_OP_LE ? 0x9e : in.op == BC_OP_GT ? 0x9f : 0x9d;
                cb_emit_u8(&cb, 0x0f); cb_emit_u8(&cb, cc); cb_emit_u8(&cb, 0xc0); /* setcc al */
                cb_emit_u8(&cb, 0x48); cb_emit_u8(&cb, 0x0f); cb_emit_u8(&cb, 0xb6); cb_emit_u8(&cb, 0xc0); /* movzx rax,al */
                x64_mov_slot_rax(&cb, in.a);
                break;
            }
            case BC_OP_NOT:
                types[in.a] = BC_TYPE_BOOL;
                x64_mov_rax_slot(&cb, in.b);
                cb_emit_u8(&cb, 0x48); cb_emit_u8(&cb, 0x83); cb_emit_u8(&cb, 0xf8); cb_emit_u8(&cb, 0x00); /* cmp rax,0 */
                cb_emit_u8(&cb, 0x0f); cb_emit_u8(&cb, 0x94); cb_emit_u8(&cb, 0xc0); /* sete al */
                cb_emit_u8(&cb, 0x48); cb_emit_u8(&cb, 0x0f); cb_emit_u8(&cb, 0xb6); cb_emit_u8(&cb, 0xc0);
                x64_mov_slot_rax(&cb, in.a);
                break;
            case BC_OP_RETURN:
                if (types[in.a] != BC_TYPE_I64 && types[in.a] != BC_TYPE_BOOL && types[in.a] != BC_TYPE_NIL) {
                    bc_errorf(error, program, i, in.op, "baseline JIT can return only I64/Bool/Nil values");
                    free(types); free(cb.data); return false;
                }
                x64_mov_rax_slot(&cb, in.a);
                cb_emit_u8(&cb, 0xc9); /* leave */
                cb_emit_u8(&cb, 0xc3); /* ret */
                returned = true;
                break;
            case BC_OP_IF:
            case BC_OP_ELSE:
            case BC_OP_LOOP:
            case BC_OP_CALL_NATIVE:
            case BC_OP_HALT:
            case BC_OP_COUNT:
                bc_errorf(error, program, i, in.op, "baseline JIT supports straight-line I64 bytecode only; unsupported %s", bc_op_name((BcOp)in.op));
                free(types); free(cb.data); return false;
        }
    }

    if (!returned) {
        bc_errorf(error, program, 0, 0, "baseline JIT program has no return");
        free(types); free(cb.data); return false;
    }

    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t alloc_size = (cb.count + page - 1u) & ~(page - 1u);
    void *mem = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        bc_errorf(error, program, 0, 0, "baseline JIT mmap failed");
        free(types); free(cb.data); return false;
    }
    memcpy(mem, cb.data, cb.count);
    if (mprotect(mem, alloc_size, PROT_READ | PROT_EXEC) != 0) {
        munmap(mem, alloc_size);
        bc_errorf(error, program, 0, 0, "baseline JIT mprotect failed");
        free(types); free(cb.data); return false;
    }

    artifact->entry = mem;
    artifact->code = mem;
    artifact->code_size = alloc_size;
    artifact->tier = options ? options->tier : BC_JIT_BASELINE_TEMPLATE;
    free(types);
    free(cb.data);
    return true;
#endif
}

void bc_jit_artifact_free(BcJitArtifact *artifact) {
    if (!artifact) return;
#if BC_HAS_X64_JIT
    if (artifact->code && artifact->code_size) {
        munmap(artifact->code, artifact->code_size);
        memset(artifact, 0, sizeof(*artifact));
        return;
    }
#endif
    free(artifact->code);
    memset(artifact, 0, sizeof(*artifact));
}

BcTierPlan bc_tier_plan_default(void) {
    BcTierPlan plan;
    plan.call_hot_threshold = 100;
    plan.loop_hot_threshold = 32;
    plan.enable_baseline_jit = true;
    plan.enable_optimizing_jit = true;
    plan.enable_osr = true;
    return plan;
}

bool bc_tier_plan_trace(const BcProgram *program,
                        const BcTierPlan *plan,
                        FILE *out,
                        BcError *error) {
    if (!out) out = stderr;
    BcTierPlan local = plan ? *plan : bc_tier_plan_default();
    BcError verify_error;
    if (!bc_verify(program, &verify_error)) {
        if (error) *error = verify_error;
        fprintf(out, "%sTier plan %s%s\n%s└─ %s", BC_CLR_BOLD_CYAN,
                program->name ? program->name : "<bytecode>", BC_CLR_RESET, BC_CLR_GRAY, BC_CLR_RESET);
        print_status(out, false);
        fprintf(out, " verify: %s\n", verify_error.message);
        return false;
    }

    fprintf(out, "%sTier plan %s%s\n", BC_CLR_BOLD_CYAN,
            program->name ? program->name : "<bytecode>", BC_CLR_RESET);
    fprintf(out, "%s├─ %sbaseline template JIT: %s\n", BC_CLR_GRAY, BC_CLR_RESET, local.enable_baseline_jit ? "enabled" : "disabled");
    fprintf(out, "%s├─ %soptimizing JIT: %s\n", BC_CLR_GRAY, BC_CLR_RESET, local.enable_optimizing_jit ? "enabled" : "disabled");
    fprintf(out, "%s├─ %shotness call=%u loop=%u\n", BC_CLR_GRAY, BC_CLR_RESET, local.call_hot_threshold, local.loop_hot_threshold);
    fprintf(out, "%s├─ %sOSR: %s\n", BC_CLR_GRAY, BC_CLR_RESET, local.enable_osr ? "enabled" : "disabled");
    for (size_t i = 0; i < program->code_count; i++) {
        if (program->code[i].op == BC_OP_LOOP) {
            fprintf(out, "%s│   ├─ %sOSR safepoint @%zu loop register r%u\n", BC_CLR_GRAY, BC_CLR_RESET, i, program->code[i].a);
            for (size_t d = 0; d < program->deopt_count; d++) {
                BcDeoptPoint dp = program->deopts[d];
                if (dp.safepoint == i && dp.register_count > 0) {
                    uint32_t last = dp.first_register + dp.register_count - 1;
                    fprintf(out, "%s│   │   ├─ %sdeopt registers r%u..r%u source=%u\n",
                            BC_CLR_GRAY, BC_CLR_RESET, dp.first_register, last, dp.source_id);
                }
            }
        }
    }
    fprintf(out, "%s└─ %s", BC_CLR_GRAY, BC_CLR_RESET);
    print_status(out, true);
    fputc('\n', out);
    return true;
}

void bc_vm_init(BcVM *vm) {
    memset(vm, 0, sizeof(*vm));
    vm->trace_out = stderr;
}

void bc_vm_free(BcVM *vm) {
    if (!vm) return;
    free(vm->registers);
    memset(vm, 0, sizeof(*vm));
}

static size_t find_else_or_end(const BcProgram *program, size_t ip) {
    size_t depth = 0;
    for (size_t i = ip; i < program->code_count; i++) {
        BcOp op = (BcOp)program->code[i].op;
        if (op == BC_OP_IF || op == BC_OP_BLOCK || op == BC_OP_LOOP) depth++;
        else if (op == BC_OP_END) {
            if (!depth) return i;
            depth--;
        } else if (op == BC_OP_ELSE && !depth) return i;
    }
    return program->code_count;
}

static size_t find_end(const BcProgram *program, size_t ip) {
    size_t depth = 0;
    for (size_t i = ip; i < program->code_count; i++) {
        BcOp op = (BcOp)program->code[i].op;
        if (op == BC_OP_IF || op == BC_OP_BLOCK || op == BC_OP_LOOP) depth++;
        else if (op == BC_OP_END) {
            if (!depth) return i;
            depth--;
        }
    }
    return program->code_count;
}

bool bc_vm_run(BcVM *vm, const BcProgram *program, BcValue *result, BcError *error) {
    BcError verify_error;
    if (!bc_verify(program, &verify_error)) {
        if (error) *error = verify_error;
        return false;
    }

    if (program->register_count > vm->register_count) {
        vm->registers = bc_realloc_array(vm->registers, program->register_count, sizeof(vm->registers[0]));
        vm->register_count = program->register_count;
    }
    for (size_t i = 0; i < vm->register_count; i++) vm->registers[i] = bc_value_nil();

    for (size_t ip = 0; ip < program->code_count; ip++) {
        BcInstr in = program->code[ip];
        BcValue x, y;
        if (vm->trace) fprintf(vm->trace_out ? vm->trace_out : stderr, "[bc] %04zu %s\n", ip, bc_op_name((BcOp)in.op));
        switch ((BcOp)in.op) {
            case BC_OP_NOP:
            case BC_OP_BLOCK:
            case BC_OP_END:
                break;
            case BC_OP_CONST:
                vm->registers[in.a] = program->constants[in.imm];
                break;
            case BC_OP_NIL:
                vm->registers[in.a] = bc_value_nil();
                break;
            case BC_OP_BOOL:
                vm->registers[in.a] = bc_value_bool(in.imm != 0);
                break;
            case BC_OP_MOV:
                vm->registers[in.a] = vm->registers[in.b];
                break;
            case BC_OP_ADD:
            case BC_OP_SUB:
            case BC_OP_MUL:
            case BC_OP_DIV:
            case BC_OP_MOD:
                x = vm->registers[in.b];
                y = vm->registers[in.c];
                if (x.kind == BC_VALUE_I64 && y.kind == BC_VALUE_I64) {
                    if (in.op == BC_OP_ADD) vm->registers[in.a] = bc_value_i64(x.as.i64 + y.as.i64);
                    else if (in.op == BC_OP_SUB) vm->registers[in.a] = bc_value_i64(x.as.i64 - y.as.i64);
                    else if (in.op == BC_OP_MUL) vm->registers[in.a] = bc_value_i64(x.as.i64 * y.as.i64);
                    else if (in.op == BC_OP_DIV) vm->registers[in.a] = bc_value_i64(x.as.i64 / y.as.i64);
                    else vm->registers[in.a] = bc_value_i64(x.as.i64 % y.as.i64);
                } else {
                    double a = x.kind == BC_VALUE_I64 ? (double)x.as.i64 : x.as.f64;
                    double b = y.kind == BC_VALUE_I64 ? (double)y.as.i64 : y.as.f64;
                    if (in.op == BC_OP_ADD) vm->registers[in.a] = bc_value_f64(a + b);
                    else if (in.op == BC_OP_SUB) vm->registers[in.a] = bc_value_f64(a - b);
                    else if (in.op == BC_OP_MUL) vm->registers[in.a] = bc_value_f64(a * b);
                    else vm->registers[in.a] = bc_value_f64(a / b);
                }
                break;
            case BC_OP_NEG:
                x = vm->registers[in.b];
                vm->registers[in.a] = x.kind == BC_VALUE_I64 ? bc_value_i64(-x.as.i64) : bc_value_f64(-x.as.f64);
                break;
            case BC_OP_EQ:
                vm->registers[in.a] = bc_value_bool(value_equal(vm->registers[in.b], vm->registers[in.c]));
                break;
            case BC_OP_LT:
            case BC_OP_LE:
            case BC_OP_GT:
            case BC_OP_GE: {
                double a = vm->registers[in.b].kind == BC_VALUE_I64 ? (double)vm->registers[in.b].as.i64 : vm->registers[in.b].as.f64;
                double b = vm->registers[in.c].kind == BC_VALUE_I64 ? (double)vm->registers[in.c].as.i64 : vm->registers[in.c].as.f64;
                bool r = in.op == BC_OP_LT ? a < b : in.op == BC_OP_LE ? a <= b : in.op == BC_OP_GT ? a > b : a >= b;
                vm->registers[in.a] = bc_value_bool(r);
                break;
            }
            case BC_OP_NOT:
                vm->registers[in.a] = bc_value_bool(!bc_value_truthy(vm->registers[in.b]));
                break;
            case BC_OP_CALL_NATIVE: {
                BcNative native = program->natives[in.imm];
                BcValue out = bc_value_nil();
                if (!native.fn(vm, &vm->registers[in.b], (uint8_t)in.c, &out, native.userdata)) {
                    bc_errorf(error, program, ip, in.op, "native '%s' failed", native.name);
                    return false;
                }
                vm->registers[in.a] = out;
                break;
            }
            case BC_OP_IF:
                if (!bc_value_truthy(vm->registers[in.a])) ip = find_else_or_end(program, ip + 1);
                break;
            case BC_OP_ELSE:
                ip = find_end(program, ip + 1);
                break;
            case BC_OP_LOOP:
                if (vm->registers[in.a].as.i64 <= 0) ip = find_end(program, ip + 1);
                break;
            case BC_OP_RETURN:
                if (result) *result = vm->registers[in.a];
                return true;
            case BC_OP_HALT:
                if (result) *result = bc_value_nil();
                return true;
            case BC_OP_COUNT:
                break;
        }
    }
    if (result) *result = bc_value_nil();
    return true;
}

static void print_value(FILE *out, BcValue value) {
    switch (value.kind) {
        case BC_VALUE_NIL: fprintf(out, "nil"); break;
        case BC_VALUE_BOOL: fprintf(out, "%s", value.as.boolean ? "true" : "false"); break;
        case BC_VALUE_I64: fprintf(out, "%" PRId64, value.as.i64); break;
        case BC_VALUE_F64: fprintf(out, "%g", value.as.f64); break;
        case BC_VALUE_PTR: fprintf(out, "ptr:%p", value.as.ptr); break;
    }
}

void bc_disassemble(const BcProgram *program, FILE *out) {
    if (!out) out = stdout;
    fprintf(out, "== %s ==\n", program->name ? program->name : "<bytecode>");
    for (size_t i = 0; i < program->code_count; i++) {
        BcInstr in = program->code[i];
        print_dim(out, "%04zu  ", i);
        print_padded_op(out, (BcOp)in.op, 16);
        print_dim(out, "r%u r%u r%u imm=%u", in.a, in.b, in.c, in.imm);
        if (in.op == BC_OP_CONST && in.imm < program->const_count) {
            fprintf(out, " ; ");
            print_value(out, program->constants[in.imm]);
        } else if (in.op == BC_OP_CALL_NATIVE && in.imm < program->native_count) {
            fprintf(out, " ; %s", program->natives[in.imm].name);
        }
        fputc('\n', out);
    }
}

static char *fmt_expr(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return bc_strdup("<format-error>");
    char *buf = malloc((size_t)n + 1);
    if (!buf) bc_die_oom((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(buf, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return buf;
}

static char *value_expr(BcValue value) {
    char buf[96];
    switch (value.kind) {
        case BC_VALUE_NIL: return bc_strdup("nil");
        case BC_VALUE_BOOL: return bc_strdup(value.as.boolean ? "true" : "false");
        case BC_VALUE_I64: snprintf(buf, sizeof(buf), "%" PRId64, value.as.i64); return bc_strdup(buf);
        case BC_VALUE_F64: snprintf(buf, sizeof(buf), "%g", value.as.f64); return bc_strdup(buf);
        case BC_VALUE_PTR: snprintf(buf, sizeof(buf), "<ptr:%p>", value.as.ptr); return bc_strdup(buf);
    }
    return bc_strdup("<unknown>");
}

void bc_decompile_monad(const BcProgram *program, FILE *out) {
    if (!out) out = stdout;
    fprintf(out, ";; register bytecode decompile: %s\n", program->name ? program->name : "<bytecode>");
    char **expr = calloc(program->register_count ? program->register_count : 1, sizeof(char *));
    if (!expr) bc_die_oom(program->register_count * sizeof(char *));
    for (size_t i = 0; i < program->code_count; i++) {
        BcInstr in = program->code[i];
        char *a, *b;
        switch ((BcOp)in.op) {
            case BC_OP_CONST:
                free(expr[in.a]);
                expr[in.a] = in.imm < program->const_count ? value_expr(program->constants[in.imm]) : bc_strdup("<bad-const>");
                fprintf(out, ";; r%u = %s\n", in.a, expr[in.a]);
                break;
            case BC_OP_NIL:
                free(expr[in.a]);
                expr[in.a] = bc_strdup("nil");
                fprintf(out, ";; r%u = nil\n", in.a);
                break;
            case BC_OP_BOOL:
                free(expr[in.a]);
                expr[in.a] = bc_strdup(in.imm ? "true" : "false");
                fprintf(out, ";; r%u = %s\n", in.a, expr[in.a]);
                break;
            case BC_OP_MOV:
                free(expr[in.a]);
                expr[in.a] = bc_strdup(expr[in.b] ? expr[in.b] : "<unknown>");
                fprintf(out, ";; r%u = %s\n", in.a, expr[in.a]);
                break;
            case BC_OP_ADD:
            case BC_OP_SUB:
            case BC_OP_MUL:
            case BC_OP_DIV:
            case BC_OP_MOD:
            case BC_OP_EQ:
            case BC_OP_LT:
            case BC_OP_LE:
            case BC_OP_GT:
            case BC_OP_GE: {
                const char *sym = in.op == BC_OP_ADD ? "+" : in.op == BC_OP_SUB ? "-" :
                    in.op == BC_OP_MUL ? "*" : in.op == BC_OP_DIV ? "/" :
                    in.op == BC_OP_MOD ? "mod" : in.op == BC_OP_EQ ? "=" :
                    in.op == BC_OP_LT ? "<" : in.op == BC_OP_LE ? "<=" :
                    in.op == BC_OP_GT ? ">" : ">=";
                a = expr[in.b] ? expr[in.b] : "<unknown>";
                b = expr[in.c] ? expr[in.c] : "<unknown>";
                free(expr[in.a]);
                expr[in.a] = fmt_expr("(%s %s %s)", sym, a, b);
                fprintf(out, ";; r%u = %s\n", in.a, expr[in.a]);
                break;
            }
            case BC_OP_NEG:
                free(expr[in.a]);
                expr[in.a] = fmt_expr("(- %s)", expr[in.b] ? expr[in.b] : "<unknown>");
                fprintf(out, ";; r%u = %s\n", in.a, expr[in.a]);
                break;
            case BC_OP_NOT:
                free(expr[in.a]);
                expr[in.a] = fmt_expr("(not %s)", expr[in.b] ? expr[in.b] : "<unknown>");
                fprintf(out, ";; r%u = %s\n", in.a, expr[in.a]);
                break;
            case BC_OP_CALL_NATIVE:
                free(expr[in.a]);
                expr[in.a] = fmt_expr("(%s ...)", in.imm < program->native_count ? program->natives[in.imm].name : "<native>");
                fprintf(out, ";; r%u = %s\n", in.a, expr[in.a]);
                break;
            case BC_OP_RETURN:
                fprintf(out, "%s\n", expr[in.a] ? expr[in.a] : "<unknown>");
                break;
            case BC_OP_IF:
                fprintf(out, "(if %s\n", expr[in.a] ? expr[in.a] : "<unknown>");
                break;
            case BC_OP_ELSE:
                fprintf(out, " else\n");
                break;
            case BC_OP_END:
                fprintf(out, ")\n");
                break;
            default:
                break;
        }
    }
    for (size_t i = 0; i < program->register_count; i++) free(expr[i]);
    free(expr);
}
