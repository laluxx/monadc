#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"

/// Runtime List Implementation

RuntimeList *rt_list_create(void) {
    RuntimeList *list = malloc(sizeof(RuntimeList));
    list->head = NULL;
    list->length = 0;
    return list;
}

RuntimeList *rt_list_cons(RuntimeValue *value, RuntimeList *list) {
    RuntimeListNode *new_node = malloc(sizeof(RuntimeListNode));
    new_node->value = value;
    new_node->next = list->head;

    RuntimeList *new_list = malloc(sizeof(RuntimeList));
    new_list->head = new_node;
    new_list->length = list->length + 1;

    return new_list;
}

void rt_list_append(RuntimeList *list, RuntimeValue *value) {
    RuntimeListNode *new_node = malloc(sizeof(RuntimeListNode));
    new_node->value = value;
    new_node->next = NULL;

    if (list->head == NULL) {
        list->head = new_node;
    } else {
        RuntimeListNode *current = list->head;
        while (current->next != NULL) {
            current = current->next;
        }
        current->next = new_node;
    }

    list->length++;
}

RuntimeValue *rt_list_car(RuntimeList *list) {
    if (list == NULL || list->head == NULL) {
        return rt_value_nil();
    }
    return list->head->value;
}

RuntimeList *rt_list_cdr(RuntimeList *list) {
    if (list == NULL || list->head == NULL) {
        return rt_list_create();
    }

    RuntimeList *new_list = malloc(sizeof(RuntimeList));
    new_list->head = list->head->next;
    new_list->length = list->length > 0 ? list->length - 1 : 0;

    return new_list;
}

RuntimeValue *rt_list_nth(RuntimeList *list, int64_t index) {
    if (list == NULL || list->head == NULL || index < 0) {
        return rt_value_nil();
    }

    RuntimeListNode *current = list->head;
    int64_t i = 0;

    while (current != NULL && i < index) {
        current = current->next;
        i++;
    }

    if (current == NULL) {
        return rt_value_nil();
    }

    return current->value;
}

int64_t rt_list_length(RuntimeList *list) {
    if (list == NULL) {
        return 0;
    }
    return list->length;
}

int rt_list_is_empty(RuntimeList *list) {
    return (list == NULL || list->head == NULL) ? 1 : 0;
}

/// Runtime Value Construction

RuntimeValue *rt_value_int(int64_t val) {
    RuntimeValue *v = malloc(sizeof(RuntimeValue));
    v->type = RT_INT;
    v->data.int_val = val;
    return v;
}

RuntimeValue *rt_value_float(double val) {
    RuntimeValue *v = malloc(sizeof(RuntimeValue));
    v->type = RT_FLOAT;
    v->data.float_val = val;
    return v;
}

RuntimeValue *rt_value_char(char val) {
    RuntimeValue *v = malloc(sizeof(RuntimeValue));
    v->type = RT_CHAR;
    v->data.char_val = val;
    return v;
}

RuntimeValue *rt_value_string(const char *val) {
    RuntimeValue *v = malloc(sizeof(RuntimeValue));
    v->type = RT_STRING;
    v->data.string_val = strdup(val);
    return v;
}

RuntimeValue *rt_value_keyword(const char *val) {
    RuntimeValue *v = malloc(sizeof(RuntimeValue));
    v->type = RT_KEYWORD;
    v->data.keyword_val = strdup(val);
    return v;
}

RuntimeValue *rt_value_list(RuntimeList *val) {
    RuntimeValue *v = malloc(sizeof(RuntimeValue));
    v->type = RT_LIST;
    v->data.list_val = val;
    return v;
}

RuntimeValue *rt_value_nil(void) {
    RuntimeValue *v = malloc(sizeof(RuntimeValue));
    v->type = RT_NIL;
    return v;
}

/// Runtime Printing

void rt_print_value(RuntimeValue *val) {
    if (val == NULL) {
        printf("nil");
        return;
    }

    switch (val->type) {
        case RT_INT:
            printf("%ld", val->data.int_val);
            break;
        case RT_FLOAT:
            printf("%g", val->data.float_val);
            break;
        case RT_CHAR:
            printf("'%c'", val->data.char_val);
            break;
        case RT_STRING:
            printf("\"%s\"", val->data.string_val);
            break;
        case RT_KEYWORD:
            printf(":%s", val->data.keyword_val);
            break;
        case RT_LIST:
            rt_print_list(val->data.list_val);
            break;
        case RT_NIL:
            printf("nil");
            break;
    }
}

void rt_print_list(RuntimeList *list) {
    printf("(");

    if (list != NULL && list->head != NULL) {
        RuntimeListNode *current = list->head;
        int first = 1;

        while (current != NULL) {
            if (!first) {
                printf(" ");
            }
            rt_print_value(current->value);
            current = current->next;
            first = 0;
        }
    }

    printf(")");
}

/// Memory Management

void rt_value_free(RuntimeValue *val) {
    if (val == NULL) return;

    switch (val->type) {
        case RT_STRING:
            free(val->data.string_val);
            break;
        case RT_KEYWORD:
            free(val->data.keyword_val);
            break;
        case RT_LIST:
            rt_list_free(val->data.list_val);
            break;
        default:
            break;
    }

    free(val);
}

void rt_list_free(RuntimeList *list) {
    if (list == NULL) return;

    RuntimeListNode *current = list->head;
    while (current != NULL) {
        RuntimeListNode *next = current->next;
        rt_value_free(current->value);
        free(current);
        current = next;
    }

    free(list);
}

/// LLVM Integration

void declare_runtime_functions(CodegenContext *ctx) {
    LLVMTypeRef ptr_type    = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    LLVMTypeRef i64_type    = LLVMInt64TypeInContext(ctx->context);
    LLVMTypeRef i32_type    = LLVMInt32TypeInContext(ctx->context);
    LLVMTypeRef i8_type     = LLVMInt8TypeInContext(ctx->context);
    LLVMTypeRef double_type = LLVMDoubleTypeInContext(ctx->context);

    // RuntimeList *rt_list_create(void)
    LLVMTypeRef rt_list_create_type = LLVMFunctionType(ptr_type, NULL, 0, 0);
    LLVMAddFunction(ctx->module, "rt_list_create", rt_list_create_type);

    // RuntimeList *rt_list_cons(RuntimeValue *value, RuntimeList *list)
    LLVMTypeRef rt_list_cons_params[] = {ptr_type, ptr_type};
    LLVMTypeRef rt_list_cons_type = LLVMFunctionType(ptr_type, rt_list_cons_params, 2, 0);
    LLVMAddFunction(ctx->module, "rt_list_cons", rt_list_cons_type);

    // void rt_list_append(RuntimeList *list, RuntimeValue *value)
    LLVMTypeRef rt_list_append_params[] = {ptr_type, ptr_type};
    LLVMTypeRef rt_list_append_type = LLVMFunctionType(LLVMVoidTypeInContext(ctx->context), rt_list_append_params, 2, 0);
    LLVMAddFunction(ctx->module, "rt_list_append", rt_list_append_type);

    // RuntimeValue *rt_list_car(RuntimeList *list)
    LLVMTypeRef rt_list_car_params[] = {ptr_type};
    LLVMTypeRef rt_list_car_type = LLVMFunctionType(ptr_type, rt_list_car_params, 1, 0);
    LLVMAddFunction(ctx->module, "rt_list_car", rt_list_car_type);

    // RuntimeList *rt_list_cdr(RuntimeList *list)
    LLVMTypeRef rt_list_cdr_params[] = {ptr_type};
    LLVMTypeRef rt_list_cdr_type = LLVMFunctionType(ptr_type, rt_list_cdr_params, 1, 0);
    LLVMAddFunction(ctx->module, "rt_list_cdr", rt_list_cdr_type);

    // RuntimeValue *rt_list_nth(RuntimeList *list, int64_t index)
    LLVMTypeRef rt_list_nth_params[] = {ptr_type, i64_type};
    LLVMTypeRef rt_list_nth_type = LLVMFunctionType(ptr_type, rt_list_nth_params, 2, 0);
    LLVMAddFunction(ctx->module, "rt_list_nth", rt_list_nth_type);

    // int64_t rt_list_length(RuntimeList *list)
    LLVMTypeRef rt_list_length_params[] = {ptr_type};
    LLVMTypeRef rt_list_length_type = LLVMFunctionType(i64_type, rt_list_length_params, 1, 0);
    LLVMAddFunction(ctx->module, "rt_list_length", rt_list_length_type);

    // int rt_list_is_empty(RuntimeList *list)
    LLVMTypeRef rt_list_is_empty_params[] = {ptr_type};
    LLVMTypeRef rt_list_is_empty_type = LLVMFunctionType(i32_type, rt_list_is_empty_params, 1, 0);
    LLVMAddFunction(ctx->module, "rt_list_is_empty", rt_list_is_empty_type);

    // RuntimeValue *rt_value_int(int64_t val)
    LLVMTypeRef rt_value_int_params[] = {i64_type};
    LLVMTypeRef rt_value_int_type = LLVMFunctionType(ptr_type, rt_value_int_params, 1, 0);
    LLVMAddFunction(ctx->module, "rt_value_int", rt_value_int_type);

    // RuntimeValue *rt_value_float(double val)
    LLVMTypeRef rt_value_float_params[] = {double_type};
    LLVMTypeRef rt_value_float_type = LLVMFunctionType(ptr_type, rt_value_float_params, 1, 0);
    LLVMAddFunction(ctx->module, "rt_value_float", rt_value_float_type);

    // RuntimeValue *rt_value_char(char val)
    LLVMTypeRef rt_value_char_params[] = {i8_type};
    LLVMTypeRef rt_value_char_type = LLVMFunctionType(ptr_type, rt_value_char_params, 1, 0);
    LLVMAddFunction(ctx->module, "rt_value_char", rt_value_char_type);

    // RuntimeValue *rt_value_string(const char *val)
    LLVMTypeRef rt_value_string_params[] = {ptr_type};
    LLVMTypeRef rt_value_string_type = LLVMFunctionType(ptr_type, rt_value_string_params, 1, 0);
    LLVMAddFunction(ctx->module, "rt_value_string", rt_value_string_type);

    // RuntimeValue *rt_value_keyword(const char *val)
    LLVMTypeRef rt_value_keyword_params[] = {ptr_type};
    LLVMTypeRef rt_value_keyword_type = LLVMFunctionType(ptr_type, rt_value_keyword_params, 1, 0);
    LLVMAddFunction(ctx->module, "rt_value_keyword", rt_value_keyword_type);

    // RuntimeValue *rt_value_list(RuntimeList *val)
    LLVMTypeRef rt_value_list_params[] = {ptr_type};
    LLVMTypeRef rt_value_list_type = LLVMFunctionType(ptr_type, rt_value_list_params, 1, 0);
    LLVMAddFunction(ctx->module, "rt_value_list", rt_value_list_type);

    // RuntimeValue *rt_value_nil(void)
    LLVMTypeRef rt_value_nil_type = LLVMFunctionType(ptr_type, NULL, 0, 0);
    LLVMAddFunction(ctx->module, "rt_value_nil", rt_value_nil_type);

    // void rt_print_value(RuntimeValue *val)
    LLVMTypeRef rt_print_value_params[] = {ptr_type};
    LLVMTypeRef rt_print_value_type = LLVMFunctionType(LLVMVoidTypeInContext(ctx->context), rt_print_value_params, 1, 0);
    LLVMAddFunction(ctx->module, "rt_print_value", rt_print_value_type);

    // void rt_print_list(RuntimeList *list)
    LLVMTypeRef rt_print_list_params[] = {ptr_type};
    LLVMTypeRef rt_print_list_type = LLVMFunctionType(LLVMVoidTypeInContext(ctx->context), rt_print_list_params, 1, 0);
    LLVMAddFunction(ctx->module, "rt_print_list", rt_print_list_type);
}

// Helper functions to get runtime function references
#define GET_RUNTIME_FUNCTION(name) \
    LLVMValueRef get_##name(CodegenContext *ctx) { \
        LLVMValueRef fn = LLVMGetNamedFunction(ctx->module, #name); \
        if (!fn) { \
            fprintf(stderr, "Runtime function " #name " not found\n"); \
            exit(1); \
        } \
        return fn; \
    }

GET_RUNTIME_FUNCTION(rt_list_create)
GET_RUNTIME_FUNCTION(rt_list_cons)
GET_RUNTIME_FUNCTION(rt_list_append)
GET_RUNTIME_FUNCTION(rt_list_car)
GET_RUNTIME_FUNCTION(rt_list_cdr)
GET_RUNTIME_FUNCTION(rt_list_nth)
GET_RUNTIME_FUNCTION(rt_list_length)
GET_RUNTIME_FUNCTION(rt_list_is_empty)
GET_RUNTIME_FUNCTION(rt_value_int)
GET_RUNTIME_FUNCTION(rt_value_float)
GET_RUNTIME_FUNCTION(rt_value_char)
GET_RUNTIME_FUNCTION(rt_value_string)
GET_RUNTIME_FUNCTION(rt_value_keyword)
GET_RUNTIME_FUNCTION(rt_value_list)
GET_RUNTIME_FUNCTION(rt_value_nil)
GET_RUNTIME_FUNCTION(rt_print_value)
GET_RUNTIME_FUNCTION(rt_print_list)

LLVMTypeRef get_rt_value_type(CodegenContext *ctx) {
    // RuntimeValue* is represented as i8*
    return LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
}

LLVMTypeRef get_rt_list_type(CodegenContext *ctx) {
    // RuntimeList* is represented as i8*
    return LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
}
