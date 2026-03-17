#ifndef RUNTIME_H
#define RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <llvm-c/Core.h>
#include <gmp.h>
#include "arena.h"
#include "codegen.h"

/// Arena

extern volatile int rt_interrupted;
extern Arena g_eval_arena;

/// Value Types

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
    RT_SET,
    RT_MAP,
    RT_CLOSURE,
} RuntimeValueType;


/// Forward Declarations

struct RuntimeValue;
struct ConsCell;
struct RuntimeList;
struct RuntimeSet;
struct RuntimeMap;

typedef struct RuntimeValue *(*ThunkFn)(void *env);


/// RuntimeThunk

typedef struct RuntimeThunk {
    ThunkFn              fn;
    void                *env;
    struct RuntimeValue *value;
    int                  forced;
} RuntimeThunk;


/// RuntimeValue

typedef struct {
    void  *fn_ptr;    // LLVM-compiled function pointer
    void **env;       // heap array of captured RuntimeValue* pointers
    int    env_size;  // number of captured variables
    int    arity;     // number of declared (non-captured) parameters
} RuntimeClosure;

typedef struct RuntimeValue {
    RuntimeValueType type;
    union {
        int64_t       int_val;
        double        float_val;
        char          char_val;
        char         *string_val;
        char         *symbol_val;
        char         *keyword_val;
        mpz_t         bignum_val;
        struct RuntimeList  *list_val;
        struct RuntimeSet   *set_val;
        struct RuntimeMap   *map_val;
        RuntimeThunk        *thunk_val;
        RuntimeClosure      *closure_val;

        struct {
            int64_t numerator;
            int64_t denominator;
        } ratio_val;

        struct {
            struct RuntimeValue **elements;
            size_t                length;
        } array_val;
    } data;
} RuntimeValue;


/// Closure

RuntimeValue *rt_value_closure(void *fn_ptr, void **env, int env_size, int arity);
RuntimeValue *rt_closure_calln(RuntimeValue *closure, int n, RuntimeValue **args);

/// HOF Callback Types

/* typedef RuntimeValue *(*RT_UnaryFn)(RuntimeValue *); */
/* typedef RuntimeValue *(*RT_BinaryFn)(RuntimeValue *, RuntimeValue *); */
typedef RuntimeValue *(*RT_UnaryFn)(void *env, int n, RuntimeValue **args);
typedef RuntimeValue *(*RT_BinaryFn)(void *env, int n, RuntimeValue **args);
typedef int           (*RT_PredFn)(RuntimeValue *);

/// ConsCell
//
//  One arena allocation per cons node.
//  Head and tail thunks are inlined directly into the cell.
//  A NULL tail_val with tail_forced=1 signals end-of-list.

typedef struct ConsCell {
    ThunkFn       head_fn;
    void         *head_env;
    RuntimeValue *head_val;
    int           head_forced;

    ThunkFn       tail_fn;
    void         *tail_env;
    RuntimeValue *tail_val;
    int           tail_forced;
} ConsCell;


/// RuntimeList
//
//  Thin wrapper around ConsCell*.  NULL cell == empty list.

typedef struct RuntimeList {
    ConsCell *cell;
} RuntimeList;


/// RuntimeSet
//
//  Heap-allocated hash set using open addressing with linear probing.
//  Tombstones mark deleted slots so probe chains remain intact after disj.
//  Capacity is always a power of two.  Load factor threshold: 0.7.

typedef struct RuntimeSet {
    RuntimeValue **buckets;
    size_t         capacity;
    size_t         count;
    size_t         tombstones;
} RuntimeSet;


/// RuntimeSet
//
// TODO Write description

typedef struct RuntimeMapEntry {
    RuntimeValue *key;
    RuntimeValue *val;
} RuntimeMapEntry;

typedef struct RuntimeMap {
    RuntimeMapEntry *buckets;
    size_t           capacity;
    size_t           count;
    size_t           tombstones;
} RuntimeMap;


/// Thunks

RuntimeThunk *rt_thunk_of_value(RuntimeValue *val);
RuntimeThunk *rt_thunk_create(ThunkFn fn, void *env);
RuntimeValue *rt_force(RuntimeThunk *thunk);


/// List

RuntimeList *rt_list_new(void);
RuntimeList *rt_list_empty(void);
int          rt_list_is_empty_list(RuntimeList *list);

RuntimeList  *rt_list_lazy_cons(RuntimeThunk *head_thunk, RuntimeThunk *tail_thunk);
RuntimeList  *rt_list_cons(RuntimeValue *head_val, RuntimeList *tail_list);
RuntimeValue *rt_list_car(RuntimeList *list);
RuntimeList  *rt_list_cdr(RuntimeList *list);
RuntimeValue *rt_list_nth(RuntimeList *list, int64_t index);
int64_t       rt_list_length(RuntimeList *list);

void         rt_list_append(RuntimeList *list, RuntimeValue *value);
RuntimeList *rt_list_append_lists(RuntimeList *a, RuntimeList *b);
RuntimeList *rt_list_copy(RuntimeList *src);
RuntimeList *rt_make_list(int64_t n, RuntimeValue *fill_val);

RuntimeList *rt_list_range(int64_t lo, int64_t hi);
RuntimeList *rt_list_from(int64_t lo);
RuntimeList *rt_list_from_step(int64_t lo, int64_t step);
RuntimeList *rt_list_take(RuntimeList *list, int64_t n);
RuntimeList *rt_list_drop(RuntimeList *list, int64_t n);

RuntimeList  *rt_list_map(RuntimeList *list, void *env, RT_UnaryFn fn);
RuntimeValue *rt_list_foldl(RuntimeList *list, RuntimeValue *init, void *env, RT_BinaryFn fn);
RuntimeValue *rt_list_foldr(RuntimeList *list, RuntimeValue *init, void *env, RT_BinaryFn fn);
RuntimeList  *rt_list_filter(RuntimeList *list, void *env, RT_UnaryFn pred);
RuntimeList  *rt_list_zipwith(RuntimeList *a, RuntimeList *b, void *env, RT_BinaryFn fn);



/// Set

static uint64_t rt_hash_value(RuntimeValue *v);

RuntimeSet   *rt_set_new(void);
RuntimeSet   *rt_set_of(RuntimeValue **vals, size_t n);
RuntimeSet   *rt_set_from_list(RuntimeList *list);
RuntimeSet   *rt_set_from_array(RuntimeValue *array_rv);
RuntimeSet   *rt_set_conj(RuntimeSet *s, RuntimeValue *val);
RuntimeSet   *rt_set_disj(RuntimeSet *s, RuntimeValue *val);
RuntimeSet   *rt_set_conj_mut(RuntimeSet *s, RuntimeValue *val);
RuntimeSet   *rt_set_disj_mut(RuntimeSet *s, RuntimeValue *val);
int           rt_set_contains(RuntimeSet *s, RuntimeValue *val);
RuntimeValue *rt_set_get(RuntimeSet *s, RuntimeValue *key);
int64_t       rt_set_count(RuntimeSet *s);
RuntimeList  *rt_set_seq(RuntimeSet *s);
int           rt_set_equal(RuntimeSet *a, RuntimeSet *b);
void          rt_set_free(RuntimeSet *s);

RuntimeValue *rt_set_foldl(RuntimeSet *s, RuntimeValue *init, void *env, RT_BinaryFn fn);
RuntimeList  *rt_set_map(RuntimeSet *s, void *env, RT_UnaryFn fn);
RuntimeSet   *rt_set_filter(RuntimeSet *s, void *env, RT_UnaryFn pred);


/// Map

RuntimeMap   *rt_map_new(void);
RuntimeMap   *rt_map_assoc(RuntimeMap *m, RuntimeValue *key, RuntimeValue *val);
RuntimeMap   *rt_map_assoc_mut(RuntimeMap *m, RuntimeValue *key, RuntimeValue *val);
RuntimeMap   *rt_map_dissoc(RuntimeMap *m, RuntimeValue *key);
RuntimeMap   *rt_map_dissoc_mut(RuntimeMap *m, RuntimeValue *key);
RuntimeValue *rt_map_get(RuntimeMap *m, RuntimeValue *key, RuntimeValue *default_val);
int           rt_map_contains(RuntimeMap *m, RuntimeValue *key);
RuntimeValue *rt_map_find(RuntimeMap *m, RuntimeValue *key);
int64_t       rt_map_count(RuntimeMap *m);
RuntimeList  *rt_map_keys(RuntimeMap *m);
RuntimeList  *rt_map_vals(RuntimeMap *m);
RuntimeMap   *rt_map_merge(RuntimeMap *a, RuntimeMap *b);
RuntimeMap   *rt_map_merge_with(RuntimeMap *a, RuntimeMap *b, RuntimeValue *(*fn)(RuntimeValue *, RuntimeValue *));
int           rt_map_equal(RuntimeMap *a, RuntimeMap *b);
void          rt_map_free(RuntimeMap *m);



/// Equality

int rt_equal_p(RuntimeValue *a, RuntimeValue *b);


/// Value Construction
//
//  Hot path (arena):   int, float, char, list, nil, thunk
//  Long-lived (heap):  string, symbol, keyword, ratio, array, set

RuntimeValue *rt_value_int(int64_t val);
RuntimeValue *rt_value_float(double val);
RuntimeValue *rt_value_char(char val);
RuntimeValue *rt_value_string(const char *val);
RuntimeValue *rt_value_symbol(const char *val);
RuntimeValue *rt_value_keyword(const char *val);
RuntimeValue *rt_value_list(RuntimeList *val);
RuntimeValue *rt_value_nil(void);
RuntimeValue *rt_value_thunk(RuntimeThunk *thunk);
RuntimeValue *rt_value_ratio(int64_t numerator, int64_t denominator);
RuntimeValue *rt_value_array(size_t length);
RuntimeValue *rt_value_set(RuntimeSet *s);
RuntimeValue *rt_value_map(RuntimeMap *m);


/// Unboxing

int64_t      rt_unbox_int(RuntimeValue *v);
double       rt_unbox_float(RuntimeValue *v);
char         rt_unbox_char(RuntimeValue *v);
char        *rt_unbox_string(RuntimeValue *v);
RuntimeList *rt_unbox_list(RuntimeValue *v);
RuntimeSet  *rt_unbox_set(RuntimeValue *v);
RuntimeMap  *rt_unbox_map(RuntimeValue *v);
int          rt_value_is_nil(RuntimeValue *v);


/// Array

void          rt_array_set(RuntimeValue *array, size_t index, RuntimeValue *value);
RuntimeValue *rt_array_get(RuntimeValue *array, size_t index);
int64_t       rt_array_length(RuntimeValue *array);


/// Ratio

RuntimeValue *rt_ratio_add(RuntimeValue *a, RuntimeValue *b);
RuntimeValue *rt_ratio_sub(RuntimeValue *a, RuntimeValue *b);
RuntimeValue *rt_ratio_mul(RuntimeValue *a, RuntimeValue *b);
RuntimeValue *rt_ratio_div(RuntimeValue *a, RuntimeValue *b);
int64_t       rt_ratio_to_int(RuntimeValue *ratio);
double        rt_ratio_to_float(RuntimeValue *ratio);


/// Bignum

RuntimeValue *rt_value_bignum_from_str(const char *s);
RuntimeValue *rt_value_bignum_from_i64(int64_t n);
RuntimeValue *rt_promote_to_bignum(RuntimeValue *v);
int           rt_value_is_bignum(RuntimeValue *v);
RuntimeValue *rt_bignum_add(RuntimeValue *a, RuntimeValue *b);
RuntimeValue *rt_bignum_sub(RuntimeValue *a, RuntimeValue *b);
RuntimeValue *rt_bignum_mul(RuntimeValue *a, RuntimeValue *b);
RuntimeValue *rt_bignum_div(RuntimeValue *a, RuntimeValue *b);


/// Printing

void rt_print_value(RuntimeValue *val);
void rt_print_value_newline(RuntimeValue *val);
void rt_print_list(RuntimeList *list);
void rt_print_list_unbounded(RuntimeList *list);
void __print_i128(__int128 v);
void __print_u128(unsigned __int128 v);

/// Memory Management

void rt_value_free(RuntimeValue *val);
void rt_list_free(RuntimeList *list);
void rt_thunk_free(RuntimeThunk *thunk);
void rt_set_free(RuntimeSet *s);

/// Assert

void __monad_assert_fail(const char *label);


/// AST Conversion

RuntimeValue *rt_ast_to_runtime_value(AST *ast);
char         *rt_string_take(const char *s, int64_t n);

/// LLVM

//// Runtime Declaration

void declare_runtime_functions(CodegenContext *ctx);
LLVMTypeRef get_rt_value_type(CodegenContext *ctx);
LLVMTypeRef get_rt_list_type(CodegenContext *ctx);

LLVMValueRef get___print_i128(CodegenContext *ctx);
LLVMValueRef get___print_u128(CodegenContext *ctx);


//// Closure

LLVMValueRef get_rt_value_closure(CodegenContext *ctx);
LLVMValueRef get_rt_closure_calln(CodegenContext *ctx);

//// Thunks

LLVMValueRef get_rt_thunk_of_value(CodegenContext *ctx);
LLVMValueRef get_rt_thunk_create(CodegenContext *ctx);
LLVMValueRef get_rt_force(CodegenContext *ctx);

//// List

LLVMValueRef get_rt_list_new(CodegenContext *ctx);
LLVMValueRef get_rt_list_empty(CodegenContext *ctx);
LLVMValueRef get_rt_list_lazy_cons(CodegenContext *ctx);
LLVMValueRef get_rt_list_cons(CodegenContext *ctx);
LLVMValueRef get_rt_list_is_empty_list(CodegenContext *ctx);
LLVMValueRef get_rt_list_is_empty(CodegenContext *ctx);
LLVMValueRef get_rt_list_car(CodegenContext *ctx);
LLVMValueRef get_rt_list_cdr(CodegenContext *ctx);
LLVMValueRef get_rt_list_nth(CodegenContext *ctx);
LLVMValueRef get_rt_list_length(CodegenContext *ctx);
LLVMValueRef get_rt_list_append(CodegenContext *ctx);
LLVMValueRef get_rt_list_append_lists(CodegenContext *ctx);
LLVMValueRef get_rt_list_copy(CodegenContext *ctx);
LLVMValueRef get_rt_make_list(CodegenContext *ctx);
LLVMValueRef get_rt_list_range(CodegenContext *ctx);
LLVMValueRef get_rt_list_from(CodegenContext *ctx);
LLVMValueRef get_rt_list_from_step(CodegenContext *ctx);
LLVMValueRef get_rt_list_take(CodegenContext *ctx);
LLVMValueRef get_rt_list_drop(CodegenContext *ctx);
LLVMValueRef get_rt_list_map(CodegenContext *ctx);
LLVMValueRef get_rt_list_foldl(CodegenContext *ctx);
LLVMValueRef get_rt_list_foldr(CodegenContext *ctx);
LLVMValueRef get_rt_list_filter(CodegenContext *ctx);
LLVMValueRef get_rt_list_zip(CodegenContext *ctx);
LLVMValueRef get_rt_list_zipwith(CodegenContext *ctx);

////  Set

LLVMValueRef get_rt_set_new(CodegenContext *ctx);
LLVMValueRef get_rt_set_of(CodegenContext *ctx);
LLVMValueRef get_rt_set_from_list(CodegenContext *ctx);
LLVMValueRef get_rt_set_from_array(CodegenContext *ctx);
LLVMValueRef get_rt_set_contains(CodegenContext *ctx);
LLVMValueRef get_rt_set_conj(CodegenContext *ctx);
LLVMValueRef get_rt_set_disj(CodegenContext *ctx);
LLVMValueRef get_rt_set_conj_mut(CodegenContext *ctx);
LLVMValueRef get_rt_set_disj_mut(CodegenContext *ctx);
LLVMValueRef get_rt_set_get(CodegenContext *ctx);
LLVMValueRef get_rt_set_count(CodegenContext *ctx);
LLVMValueRef get_rt_set_seq(CodegenContext *ctx);
LLVMValueRef get_rt_value_set(CodegenContext *ctx);
LLVMValueRef get_rt_unbox_set(CodegenContext *ctx);

LLVMValueRef get_rt_set_foldl(CodegenContext *ctx);
LLVMValueRef get_rt_set_map(CodegenContext *ctx);
LLVMValueRef get_rt_set_filter(CodegenContext *ctx);

//// Map

LLVMValueRef get_rt_map_new(CodegenContext *ctx);
LLVMValueRef get_rt_map_assoc(CodegenContext *ctx);
LLVMValueRef get_rt_map_assoc_mut(CodegenContext *ctx);
LLVMValueRef get_rt_map_dissoc(CodegenContext *ctx);
LLVMValueRef get_rt_map_dissoc_mut(CodegenContext *ctx);
LLVMValueRef get_rt_map_get(CodegenContext *ctx);
LLVMValueRef get_rt_map_contains(CodegenContext *ctx);
LLVMValueRef get_rt_map_find(CodegenContext *ctx);
LLVMValueRef get_rt_map_count(CodegenContext *ctx);
LLVMValueRef get_rt_map_keys(CodegenContext *ctx);
LLVMValueRef get_rt_map_vals(CodegenContext *ctx);
LLVMValueRef get_rt_map_merge(CodegenContext *ctx);
LLVMValueRef get_rt_value_map(CodegenContext *ctx);
LLVMValueRef get_rt_unbox_map(CodegenContext *ctx);

//// Equality

LLVMValueRef get_rt_equal_p(CodegenContext *ctx);

//// Unboxing

LLVMValueRef get_rt_unbox_int(CodegenContext *ctx);
LLVMValueRef get_rt_unbox_float(CodegenContext *ctx);
LLVMValueRef get_rt_unbox_char(CodegenContext *ctx);
LLVMValueRef get_rt_unbox_string(CodegenContext *ctx);
LLVMValueRef get_rt_unbox_list(CodegenContext *ctx);
LLVMValueRef get_rt_value_is_nil(CodegenContext *ctx);
LLVMValueRef get_rt_print_value_newline(CodegenContext *ctx);

//// Value Construction

LLVMValueRef get_rt_value_int(CodegenContext *ctx);
LLVMValueRef get_rt_value_float(CodegenContext *ctx);
LLVMValueRef get_rt_value_char(CodegenContext *ctx);
LLVMValueRef get_rt_value_string(CodegenContext *ctx);
LLVMValueRef get_rt_value_symbol(CodegenContext *ctx);
LLVMValueRef get_rt_value_keyword(CodegenContext *ctx);
LLVMValueRef get_rt_value_list(CodegenContext *ctx);
LLVMValueRef get_rt_value_nil(CodegenContext *ctx);
LLVMValueRef get_rt_value_thunk(CodegenContext *ctx);
LLVMValueRef get_rt_value_ratio(CodegenContext *ctx);
LLVMValueRef get_rt_value_array(CodegenContext *ctx);

//// Array

LLVMValueRef get_rt_array_set(CodegenContext *ctx);
LLVMValueRef get_rt_array_get(CodegenContext *ctx);
LLVMValueRef get_rt_array_length(CodegenContext *ctx);

//// Ratio

LLVMValueRef get_rt_ratio_add(CodegenContext *ctx);
LLVMValueRef get_rt_ratio_sub(CodegenContext *ctx);
LLVMValueRef get_rt_ratio_mul(CodegenContext *ctx);
LLVMValueRef get_rt_ratio_div(CodegenContext *ctx);
LLVMValueRef get_rt_ratio_to_int(CodegenContext *ctx);
LLVMValueRef get_rt_ratio_to_float(CodegenContext *ctx);

//// Print

LLVMValueRef get_rt_print_value(CodegenContext *ctx);
LLVMValueRef get_rt_print_list(CodegenContext *ctx);

//// String

LLVMValueRef get_rt_string_take(CodegenContext *ctx);
LLVMValueRef get_rt_ast_to_runtime_value(CodegenContext *ctx);

#endif // RUNTIME_H
