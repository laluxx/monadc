#ifndef RUNTIME_H
#define RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <llvm-c/Core.h>
#include <gmp.h>
#include "arena.h"
#include "codegen.h"

extern volatile int rt_interrupted;

//  Global evaluation arena — defined in runtime.c
//
//  Initialise once at startup:
//    arena_init(&g_eval_arena, 4 * 1024 * 1024);
//
//  Reset after every REPL expression (all call sites in repl_eval_line +
//  repl_sigint_handler):
//    arena_reset(&g_eval_arena);
extern Arena g_eval_arena;

///  Value types

typedef enum {
    RT_INT,
    RT_FLOAT,
    RT_CHAR,
    RT_STRING,
    RT_SYMBOL,
    RT_KEYWORD,
    RT_LIST,
    RT_RATIO,
    RT_ARRAY,
    RT_NIL,
    RT_THUNK,
    RT_BIGNUM,
} RuntimeValueType;

//  Forward declarations

struct RuntimeValue;
struct ConsCell;

typedef struct RuntimeValue *(*ThunkFn)(void *env);

//  Legacy RuntimeThunk — kept for generated code that calls rt_thunk_create
//  rt_force directly.  Internally the hot path uses ConsCell instead.
typedef struct RuntimeThunk {
    ThunkFn  fn;
    void    *env;
    struct RuntimeValue *value;
    int      forced;
} RuntimeThunk;

//  RuntimeValue  (tagged union)
typedef struct RuntimeValue {
    RuntimeValueType type;
    union {
        int64_t      int_val;
        double       float_val;
        char         char_val;
        char        *string_val;
        char        *symbol_val;
        char        *keyword_val;
        struct RuntimeList  *list_val;
        RuntimeThunk        *thunk_val;
        mpz_t bignum_val;  // arbitrary precision integer

        struct {
            int64_t numerator;
            int64_t denominator;
        } ratio_val;

        struct {
            struct RuntimeValue **elements;
            size_t length;
        } array_val;
    } data;
} RuntimeValue;

//  Step 3 — Fused ConsCell
//
//  One arena allocation per cons node instead of 3–5 separate mallocs.
//  Head and tail thunks are inlined directly into the cell.
//
//  Forcing protocol:
//    if head_forced: use head_val directly
//    else:          call head_fn(head_env), store in head_val, set head_forced=1
//  Same for tail.
//
//  A NULL tail_val with tail_forced=1 signals end-of-list.
typedef struct ConsCell {
    // Inlined head thunk
    ThunkFn       head_fn;
    void         *head_env;
    RuntimeValue *head_val;
    int           head_forced;

    // Inlined tail thunk
    ThunkFn       tail_fn;
    void         *tail_env;
    RuntimeValue *tail_val;   // RT_LIST wrapping the next RuntimeList*
    int           tail_forced;
} ConsCell;

//  RuntimeList — thin wrapper around ConsCell*
//  NULL cell pointer == empty list.
typedef struct RuntimeList {
    ConsCell *cell;
} RuntimeList;

///  Thunk construction helpers (legacy API — arena-allocated)

RuntimeThunk *rt_thunk_of_value(RuntimeValue *val);
RuntimeThunk *rt_thunk_create(ThunkFn fn, void *env);
RuntimeValue *rt_force(RuntimeThunk *thunk);

///  Lazy List API

RuntimeList *rt_list_new(void);
LLVMValueRef get_rt_list_new(CodegenContext *ctx);

RuntimeList  *rt_list_empty(void);
int           rt_list_is_empty_list(RuntimeList *list);

RuntimeList  *rt_list_lazy_cons(RuntimeThunk *head_thunk, RuntimeThunk *tail_thunk);
RuntimeList  *rt_list_cons(RuntimeValue *head_val, RuntimeList *tail_list);

RuntimeValue *rt_list_car(RuntimeList *list);
RuntimeList  *rt_list_cdr(RuntimeList *list);
RuntimeValue *rt_list_nth(RuntimeList *list, int64_t index);
int64_t       rt_list_length(RuntimeList *list);

void          rt_list_append(RuntimeList *list, RuntimeValue *value);
RuntimeList  *rt_list_append_lists(RuntimeList *a, RuntimeList *b);
RuntimeList  *rt_list_copy(RuntimeList *src);
RuntimeList  *rt_make_list(int64_t n, RuntimeValue *fill_val);

///  Infinite / range list constructors

RuntimeList  *rt_list_range(int64_t lo, int64_t hi);
RuntimeList  *rt_list_from(int64_t lo);
RuntimeList  *rt_list_from_step(int64_t lo, int64_t step);
RuntimeList  *rt_list_take(RuntimeList *list, int64_t n);
RuntimeList  *rt_list_drop(RuntimeList *list, int64_t n);

char *rt_string_take(const char *s, int64_t n);
LLVMValueRef get_rt_string_take(CodegenContext *ctx);

///  Equality

int rt_equal_p(RuntimeValue *a, RuntimeValue *b);

///  Unboxing

int64_t      rt_unbox_int(RuntimeValue *v);
double       rt_unbox_float(RuntimeValue *v);
char         rt_unbox_char(RuntimeValue *v);
char        *rt_unbox_string(RuntimeValue *v);
RuntimeList *rt_unbox_list(RuntimeValue *v);
int          rt_value_is_nil(RuntimeValue *v);
void         rt_print_value_newline(RuntimeValue *v);

///  Value construction

RuntimeValue *rt_value_int(int64_t val);      // arena + interning cache
RuntimeValue *rt_value_float(double val);     // arena
RuntimeValue *rt_value_char(char val);        // arena
RuntimeValue *rt_value_string(const char *val);   // heap (owns string)
RuntimeValue *rt_value_symbol(const char *val);   // heap (owns string)
RuntimeValue *rt_value_keyword(const char *val);  // heap (owns string)
RuntimeValue *rt_value_list(RuntimeList *val);    // arena
RuntimeValue *rt_value_nil(void);                 // arena
RuntimeValue *rt_value_thunk(RuntimeThunk *thunk);// arena

///  Ratio / Array  (heap-allocated)

RuntimeValue *rt_value_ratio(int64_t numerator, int64_t denominator);
RuntimeValue *rt_value_array(size_t length);
void          rt_array_set(RuntimeValue *array, size_t index, RuntimeValue *value);
RuntimeValue *rt_array_get(RuntimeValue *array, size_t index);
int64_t       rt_array_length(RuntimeValue *array);

RuntimeValue *rt_ratio_add(RuntimeValue *a, RuntimeValue *b);
RuntimeValue *rt_ratio_sub(RuntimeValue *a, RuntimeValue *b);
RuntimeValue *rt_ratio_mul(RuntimeValue *a, RuntimeValue *b);
RuntimeValue *rt_ratio_div(RuntimeValue *a, RuntimeValue *b);
int64_t       rt_ratio_to_int(RuntimeValue *ratio);
double        rt_ratio_to_float(RuntimeValue *ratio);

//  Printing

void rt_print_value(RuntimeValue *val);
void rt_print_list(RuntimeList *list);
void rt_print_list_unbounded(RuntimeList *list);

/// Memory management
//
//  With the arena these are mostly no-ops on the hot path.
//  rt_value_free still frees heap-owned string payloads.
//
void rt_value_free(RuntimeValue *val);
void rt_list_free(RuntimeList *list);
void rt_thunk_free(RuntimeThunk *thunk);

//  Assert failure handler (weak — overridden by repl.c)
void __monad_assert_fail(const char *label);

/// Bignum

RuntimeValue *rt_value_bignum_from_str(const char *s);
RuntimeValue *rt_value_bignum_from_i64(int64_t n);
RuntimeValue *rt_promote_to_bignum(RuntimeValue *v);
int           rt_value_is_bignum(RuntimeValue *v);

RuntimeValue *rt_bignum_add(RuntimeValue *a, RuntimeValue *b);
RuntimeValue *rt_bignum_sub(RuntimeValue *a, RuntimeValue *b);
RuntimeValue *rt_bignum_mul(RuntimeValue *a, RuntimeValue *b);
RuntimeValue *rt_bignum_div(RuntimeValue *a, RuntimeValue *b);


/// Higher-order list operations

// Callback type for unary and binary RuntimeValue* functions
typedef RuntimeValue *(*RT_UnaryFn)(RuntimeValue *);
typedef RuntimeValue *(*RT_BinaryFn)(RuntimeValue *, RuntimeValue *);
typedef int           (*RT_PredFn)(RuntimeValue *);

RuntimeList  *rt_list_map(RuntimeList *list, RT_UnaryFn fn);
RuntimeValue *rt_list_foldl(RuntimeList *list, RuntimeValue *init, RT_BinaryFn fn);
RuntimeValue *rt_list_foldr(RuntimeList *list, RuntimeValue *init, RT_BinaryFn fn);
RuntimeList  *rt_list_filter(RuntimeList *list, RT_UnaryFn pred);
RuntimeList  *rt_list_zip(RuntimeList *a, RuntimeList *b);
RuntimeList  *rt_list_zipwith(RuntimeList *a, RuntimeList *b, RT_BinaryFn fn);



///  LLVM Integration

void declare_runtime_functions(CodegenContext *ctx);

LLVMValueRef get_rt_list_empty(CodegenContext *ctx);
LLVMValueRef get_rt_list_lazy_cons(CodegenContext *ctx);
LLVMValueRef get_rt_list_cons(CodegenContext *ctx);
LLVMValueRef get_rt_list_car(CodegenContext *ctx);
LLVMValueRef get_rt_list_cdr(CodegenContext *ctx);
LLVMValueRef get_rt_list_nth(CodegenContext *ctx);
LLVMValueRef get_rt_list_length(CodegenContext *ctx);
LLVMValueRef get_rt_list_append(CodegenContext *ctx);
LLVMValueRef get_rt_list_append_lists(CodegenContext *ctx);
LLVMValueRef get_rt_list_copy(CodegenContext *ctx);
LLVMValueRef get_rt_list_is_empty(CodegenContext *ctx);
LLVMValueRef get_rt_make_list(CodegenContext *ctx);

LLVMValueRef get_rt_list_range(CodegenContext *ctx);
LLVMValueRef get_rt_list_from(CodegenContext *ctx);
LLVMValueRef get_rt_list_from_step(CodegenContext *ctx);
LLVMValueRef get_rt_list_take(CodegenContext *ctx);
LLVMValueRef get_rt_list_drop(CodegenContext *ctx);

LLVMValueRef get_rt_thunk_of_value(CodegenContext *ctx);
LLVMValueRef get_rt_thunk_create(CodegenContext *ctx);
LLVMValueRef get_rt_force(CodegenContext *ctx);

LLVMValueRef get_rt_equal_p(CodegenContext *ctx);

LLVMValueRef get_rt_unbox_int(CodegenContext *ctx);
LLVMValueRef get_rt_unbox_float(CodegenContext *ctx);
LLVMValueRef get_rt_unbox_char(CodegenContext *ctx);
LLVMValueRef get_rt_unbox_string(CodegenContext *ctx);
LLVMValueRef get_rt_unbox_list(CodegenContext *ctx);
LLVMValueRef get_rt_value_is_nil(CodegenContext *ctx);
LLVMValueRef get_rt_print_value_newline(CodegenContext *ctx);

LLVMValueRef get_rt_value_int(CodegenContext *ctx);
LLVMValueRef get_rt_value_float(CodegenContext *ctx);
LLVMValueRef get_rt_value_char(CodegenContext *ctx);
LLVMValueRef get_rt_value_string(CodegenContext *ctx);
LLVMValueRef get_rt_value_symbol(CodegenContext *ctx);
LLVMValueRef get_rt_value_keyword(CodegenContext *ctx);
LLVMValueRef get_rt_value_list(CodegenContext *ctx);
LLVMValueRef get_rt_value_nil(CodegenContext *ctx);
LLVMValueRef get_rt_value_thunk(CodegenContext *ctx);

LLVMValueRef get_rt_print_value(CodegenContext *ctx);
LLVMValueRef get_rt_print_list(CodegenContext *ctx);

LLVMValueRef get_rt_value_ratio(CodegenContext *ctx);
LLVMValueRef get_rt_value_array(CodegenContext *ctx);
LLVMValueRef get_rt_array_set(CodegenContext *ctx);
LLVMValueRef get_rt_array_get(CodegenContext *ctx);
LLVMValueRef get_rt_array_length(CodegenContext *ctx);
LLVMValueRef get_rt_ratio_add(CodegenContext *ctx);
LLVMValueRef get_rt_ratio_sub(CodegenContext *ctx);
LLVMValueRef get_rt_ratio_mul(CodegenContext *ctx);
LLVMValueRef get_rt_ratio_div(CodegenContext *ctx);
LLVMValueRef get_rt_ratio_to_int(CodegenContext *ctx);
LLVMValueRef get_rt_ratio_to_float(CodegenContext *ctx);

LLVMValueRef get_rt_list_is_empty_list(CodegenContext *ctx);

LLVMTypeRef get_rt_value_type(CodegenContext *ctx);
LLVMTypeRef get_rt_list_type(CodegenContext *ctx);

LLVMValueRef get_rt_list_map(CodegenContext *ctx);
LLVMValueRef get_rt_list_foldl(CodegenContext *ctx);
LLVMValueRef get_rt_list_foldr(CodegenContext *ctx);
LLVMValueRef get_rt_list_filter(CodegenContext *ctx);
LLVMValueRef get_rt_list_zip(CodegenContext *ctx);
LLVMValueRef get_rt_list_zipwith(CodegenContext *ctx);



#endif // RUNTIME_H
