#ifndef RUNTIME_H
#define RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <llvm-c/Core.h>
#include "codegen.h"

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
    RT_NIL
} RuntimeValueType;

// Runtime value representation
// This is a tagged union that can hold any Monad value
typedef struct RuntimeValue {
    RuntimeValueType type;
    union {
        int64_t int_val;
        double float_val;
        char char_val;
        char *string_val;
        char *symbol_val;   // RT_SYMBOL
        char *keyword_val;
        struct RuntimeList *list_val;

        // Ratio value
        struct {
            int64_t numerator;
            int64_t denominator;
        } ratio_val;

        // Array value
        struct {
            struct RuntimeValue **elements;
            size_t length;
        } array_val;
    } data;
} RuntimeValue;

// Runtime list node (cons cell)
typedef struct RuntimeListNode {
    RuntimeValue *value;
    struct RuntimeListNode *next;
} RuntimeListNode;

// Runtime list
typedef struct RuntimeList {
    RuntimeListNode *head;
    size_t length;
} RuntimeList;

/// Runtime API

// These functions will be called from generated LLVM code

// List construction
RuntimeList *rt_list_create(void);
RuntimeList *rt_list_cons(RuntimeValue *value, RuntimeList *list);
void rt_list_append(RuntimeList *list, RuntimeValue *value);

// List access
RuntimeValue *rt_list_car(RuntimeList *list);    // first element
RuntimeList *rt_list_cdr(RuntimeList *list);     // rest of list
RuntimeValue *rt_list_nth(RuntimeList *list, int64_t index);
int64_t rt_list_length(RuntimeList *list);
int rt_list_is_empty(RuntimeList *list);

// Value construction
RuntimeValue *rt_value_int(int64_t val);
RuntimeValue *rt_value_float(double val);
RuntimeValue *rt_value_char(char val);
RuntimeValue *rt_value_string(const char *val);
RuntimeValue *rt_value_keyword(const char *val);
RuntimeValue *rt_value_list(RuntimeList *val);
RuntimeValue *rt_value_nil(void);

// Value printing
void rt_print_value(RuntimeValue *val);
void rt_print_list(RuntimeList *list);

// Memory management
void rt_value_free(RuntimeValue *val);
void rt_list_free(RuntimeList *list);

/// Arrays and Ratios

RuntimeValue *rt_value_ratio(int64_t numerator, int64_t denominator);
RuntimeValue *rt_value_array(size_t length);
void rt_array_set(RuntimeValue *array, size_t index, RuntimeValue *value);
RuntimeValue *rt_array_get(RuntimeValue *array, size_t index);
int64_t rt_array_length(RuntimeValue *array);

RuntimeValue *rt_ratio_add(RuntimeValue *a, RuntimeValue *b);
RuntimeValue *rt_ratio_sub(RuntimeValue *a, RuntimeValue *b);
RuntimeValue *rt_ratio_mul(RuntimeValue *a, RuntimeValue *b);
RuntimeValue *rt_ratio_div(RuntimeValue *a, RuntimeValue *b);
int64_t rt_ratio_to_int(RuntimeValue *ratio);
double rt_ratio_to_float(RuntimeValue *ratio);


/// Symbols

RuntimeValue *rt_value_symbol(const char *val);


// Helper functions for LLVM
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
LLVMValueRef get_rt_value_symbol(CodegenContext *ctx);


/// LLVM Integration

// Declare all runtime functions in the LLVM module
void declare_runtime_functions(CodegenContext *ctx);

// Get runtime function references
LLVMValueRef get_rt_list_create(CodegenContext *ctx);
LLVMValueRef get_rt_list_cons(CodegenContext *ctx);
LLVMValueRef get_rt_list_append(CodegenContext *ctx);
LLVMValueRef get_rt_list_car(CodegenContext *ctx);
LLVMValueRef get_rt_list_cdr(CodegenContext *ctx);
LLVMValueRef get_rt_list_nth(CodegenContext *ctx);
LLVMValueRef get_rt_list_length(CodegenContext *ctx);
LLVMValueRef get_rt_list_is_empty(CodegenContext *ctx);

LLVMValueRef get_rt_value_int(CodegenContext *ctx);
LLVMValueRef get_rt_value_float(CodegenContext *ctx);
LLVMValueRef get_rt_value_char(CodegenContext *ctx);
LLVMValueRef get_rt_value_string(CodegenContext *ctx);
LLVMValueRef get_rt_value_keyword(CodegenContext *ctx);
LLVMValueRef get_rt_value_list(CodegenContext *ctx);
LLVMValueRef get_rt_value_nil(CodegenContext *ctx);

LLVMValueRef get_rt_print_value(CodegenContext *ctx);
LLVMValueRef get_rt_print_list(CodegenContext *ctx);

// Get runtime type references
LLVMTypeRef get_rt_value_type(CodegenContext *ctx);
LLVMTypeRef get_rt_list_type(CodegenContext *ctx);

#endif // RUNTIME_H
