#include "typeclass.h"
#include "codegen.h"
#include "env.h"
#include "types.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

static void tc_codegen_error(CodegenContext *ctx, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(ctx->error_msg, sizeof(ctx->error_msg), fmt, args);
    va_end(args);
    fprintf(stderr, "%s\n", ctx->error_msg);
    if (ctx->error_jmp_set) {
        ctx->error_jmp_set = false;
        longjmp(ctx->error_jmp, 1);
    }
    exit(1);
}

/// Lifecycle

TypeClassRegistry *tc_registry_create(void) {
    TypeClassRegistry *reg = calloc(1, sizeof(TypeClassRegistry));
    reg->class_cap    = 8;
    reg->instance_cap = 16;
    reg->classes   = malloc(sizeof(TCClass)    * reg->class_cap);
    reg->instances = malloc(sizeof(TCInstance) * reg->instance_cap);
    return reg;
}

void tc_registry_free(TypeClassRegistry *reg) {
    if (!reg) return;

    for (int i = 0; i < reg->class_count; i++) {
        TCClass *c = &reg->classes[i];
        free(c->name);
        free(c->type_var);
        for (int j = 0; j < c->superclass_count; j++) {
            free(c->superclass_names[j]);
            free(c->superclass_type_vars[j]);
        }
        free(c->superclass_names);
        free(c->superclass_type_vars);
        for (int j = 0; j < c->assoc_count; j++) free(c->assoc_types[j]);
        if (c->assoc_types) free(c->assoc_types);
        for (int j = 0; j < c->method_count; j++) {
            free(c->methods[j].name);
            free(c->methods[j].type_str);
        }
        free(c->methods);
        for (int j = 0; j < c->default_count; j++) {
            free(c->default_names[j]);
            ast_free(c->default_bodies[j]);
        }
        free(c->default_names);
        free(c->default_bodies);
    }
    free(reg->classes);

    for (int i = 0; i < reg->instance_count; i++) {
        TCInstance *inst = &reg->instances[i];
        free(inst->class_name);
        free(inst->type_name);
        for (int j = 0; j < inst->assoc_count; j++) {
            free(inst->assoc_names[j]);
            free(inst->assoc_values[j]);
        }
        if (inst->assoc_names) free(inst->assoc_names);
        if (inst->assoc_values) free(inst->assoc_values);
        for (int j = 0; j < inst->method_count; j++)
            free(inst->method_names[j]);
        for (int j = 0; j < inst->method_count; j++)
            free(inst->method_symbols[j]);
        free(inst->method_names);
        free(inst->method_funcs);
        free(inst->method_symbols);
    }
    free(reg->instances);
    free(reg);
}

static void tc_registry_ensure_class_cap(TypeClassRegistry *reg) {
    if (reg->class_count < reg->class_cap) return;
    reg->class_cap *= 2;
    reg->classes = realloc(reg->classes, sizeof(TCClass) * reg->class_cap);
}

static void tc_registry_ensure_instance_cap(TypeClassRegistry *reg) {
    if (reg->instance_count < reg->instance_cap) return;
    reg->instance_cap *= 2;
    reg->instances = realloc(reg->instances,
                             sizeof(TCInstance) * reg->instance_cap);
}

static void tc_copy_class_into(TypeClassRegistry *dst, const TCClass *src) {
    if (tc_find_class(dst, src->name))
        return;

    tc_registry_ensure_class_cap(dst);
    TCClass *c = &dst->classes[dst->class_count++];
    memset(c, 0, sizeof(*c));

    c->name = src->name ? strdup(src->name) : NULL;
    c->type_var = src->type_var ? strdup(src->type_var) : NULL;

    c->superclass_count = src->superclass_count;
    c->superclass_names = malloc(sizeof(char*) * (src->superclass_count ? src->superclass_count : 1));
    c->superclass_type_vars = malloc(sizeof(char*) * (src->superclass_count ? src->superclass_count : 1));
    for (int i = 0; i < src->superclass_count; i++) {
        c->superclass_names[i] = src->superclass_names[i] ? strdup(src->superclass_names[i]) : NULL;
        c->superclass_type_vars[i] = src->superclass_type_vars[i] ? strdup(src->superclass_type_vars[i]) : NULL;
    }

    c->assoc_count = src->assoc_count;
    c->assoc_types = malloc(sizeof(char*) * (src->assoc_count ? src->assoc_count : 1));
    for (int i = 0; i < src->assoc_count; i++)
        c->assoc_types[i] = src->assoc_types[i] ? strdup(src->assoc_types[i]) : NULL;

    c->method_count = src->method_count;
    c->methods = malloc(sizeof(TCMethod) * (src->method_count ? src->method_count : 1));
    for (int i = 0; i < src->method_count; i++) {
        c->methods[i].name = src->methods[i].name ? strdup(src->methods[i].name) : NULL;
        c->methods[i].type_str = src->methods[i].type_str ? strdup(src->methods[i].type_str) : NULL;
    }

    c->default_count = src->default_count;
    c->default_names = malloc(sizeof(char*) * (src->default_count ? src->default_count : 1));
    c->default_bodies = malloc(sizeof(AST*) * (src->default_count ? src->default_count : 1));
    for (int i = 0; i < src->default_count; i++) {
        c->default_names[i] = src->default_names[i] ? strdup(src->default_names[i]) : NULL;
        c->default_bodies[i] = ast_clone(src->default_bodies[i]);
    }
}

static void tc_copy_instance_into(TypeClassRegistry *dst,
                                  const TCInstance *src) {
    if (tc_find_instance(dst, src->class_name, src->type_name))
        return;

    tc_registry_ensure_instance_cap(dst);
    TCInstance *inst = &dst->instances[dst->instance_count++];
    memset(inst, 0, sizeof(*inst));

    inst->class_name = src->class_name ? strdup(src->class_name) : NULL;
    inst->type_name = src->type_name ? strdup(src->type_name) : NULL;
    inst->dict_global = src->dict_global;

    inst->assoc_count = src->assoc_count;
    inst->assoc_names = malloc(sizeof(char*) * (src->assoc_count ? src->assoc_count : 1));
    inst->assoc_values = malloc(sizeof(char*) * (src->assoc_count ? src->assoc_count : 1));
    for (int i = 0; i < src->assoc_count; i++) {
        inst->assoc_names[i] = src->assoc_names[i] ? strdup(src->assoc_names[i]) : NULL;
        inst->assoc_values[i] = src->assoc_values[i] ? strdup(src->assoc_values[i]) : NULL;
    }

    inst->method_count = src->method_count;
    inst->method_names = malloc(sizeof(char*) * (src->method_count ? src->method_count : 1));
    inst->method_funcs = malloc(sizeof(LLVMValueRef) * (src->method_count ? src->method_count : 1));
    inst->method_symbols = malloc(sizeof(char*) * (src->method_count ? src->method_count : 1));
    for (int i = 0; i < src->method_count; i++) {
        inst->method_names[i] = src->method_names[i] ? strdup(src->method_names[i]) : NULL;
        inst->method_funcs[i] = NULL;
        const char *live_name = src->method_funcs[i]
            ? LLVMGetValueName(src->method_funcs[i]) : NULL;
        inst->method_symbols[i] = live_name && live_name[0]
            ? strdup(live_name)
            : (src->method_symbols[i] ? strdup(src->method_symbols[i]) : NULL);
    }
}

TypeClassRegistry *tc_registry_clone(TypeClassRegistry *reg) {
    TypeClassRegistry *copy = tc_registry_create();
    tc_registry_merge(copy, reg);
    return copy;
}

void tc_registry_merge(TypeClassRegistry *dst, TypeClassRegistry *src) {
    if (!dst || !src) return;

    for (int i = 0; i < src->class_count; i++)
        tc_copy_class_into(dst, &src->classes[i]);

    for (int i = 0; i < src->instance_count; i++)
        tc_copy_instance_into(dst, &src->instances[i]);
}

/// Name helpers

void tc_dict_name(const char *class_name, const char *type_name,
                  char *out, size_t out_size) {
    snprintf(out, out_size, "__dict_%s_%s", class_name, type_name);
}

void tc_method_name(const char *class_name, const char *type_name,
                    const char *method_name, char *out, size_t out_size) {
    snprintf(out, out_size, "__impl_%s_%s_%s",
             class_name, type_name, method_name);
}

/// Lookup

TCClass *tc_find_class(TypeClassRegistry *reg, const char *class_name) {
    if (!reg || !class_name) return NULL;

    for (int i = 0; i < reg->class_count; i++)
        if (strcmp(reg->classes[i].name, class_name) == 0)
            return &reg->classes[i];
    return NULL;
}

TCInstance *tc_find_instance(TypeClassRegistry *reg, const char *class_name,
                             const char *type_name) {
    if (!reg || !class_name || !type_name) return NULL;

    TCInstance *subtype_match = NULL;
    for (int i = 0; i < reg->instance_count; i++) {
        TCInstance *inst = &reg->instances[i];
        if (strcmp(inst->class_name, class_name) == 0 &&
            strcmp(inst->type_name,  type_name)  == 0)
            return inst;
        if (!subtype_match &&
            strcmp(inst->class_name, class_name) == 0 &&
            type_name_is_subtype(type_name, inst->type_name))
            subtype_match = inst;
    }
    return subtype_match;
}

bool tc_is_method(TypeClassRegistry *reg, const char *method_name) {
    return reg && method_name && tc_method_class(reg, method_name) != NULL;
}

const char *tc_method_class(TypeClassRegistry *reg, const char *method_name) {
    if (!reg || !method_name) return NULL;

    for (int i = 0; i < reg->class_count; i++) {
        TCClass *c = &reg->classes[i];
        for (int j = 0; j < c->method_count; j++)
            if (strcmp(c->methods[j].name, method_name) == 0)
                return c->name;
    }
    return NULL;
}

/// Dictionary LLVM type
//
// The dictionary for a class is a struct of function pointers, one per method.
// e.g. Eq -> { ptr, ptr }  (one for =, one for !=)
// We cache it as an LLVM named struct "dict.Eq".
//
LLVMTypeRef tc_dict_type(TypeClassRegistry *reg, const char *class_name,
                         CodegenContext *ctx) {
    char struct_name[256];
    snprintf(struct_name, sizeof(struct_name), "dict.%s", class_name);

    LLVMTypeRef existing = LLVMGetTypeByName2(ctx->context, struct_name);
    if (existing) return existing;

    TCClass *c = tc_find_class(reg, class_name);
    if (!c) return NULL;

    /* All method slots are ptr (function pointer, erased type) */
    LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    LLVMTypeRef *fields = malloc(sizeof(LLVMTypeRef) * c->method_count);
    for (int i = 0; i < c->method_count; i++)
        fields[i] = ptr_t;

    LLVMTypeRef dict_t = LLVMStructCreateNamed(ctx->context, struct_name);
    LLVMStructSetBody(dict_t, fields, c->method_count, 0);
    free(fields);
    return dict_t;
}

/// Class registration
//
// Records the class declaration in the registry and registers each method
// in the HM type environment so inference knows about them.
//
//
void tc_register_class(TypeClassRegistry *reg, AST *ast, CodegenContext *ctx) {
    if (!ast || ast->type != AST_CLASS) return;

    for (int si = 0; si < ast->class_decl.superclass_count; si++) {
        const char *super_name = ast->class_decl.superclass_names[si];
        const char *super_var = ast->class_decl.superclass_type_vars[si];
        if (!tc_find_class(reg, super_name)) {
            tc_codegen_error(ctx,
                "%s:%d:%d: error: superclass constraint references unknown class '%s' in class '%s'",
                parser_get_filename(), ast->line, ast->column,
                super_name, ast->class_decl.name);
        }
        if (super_var && ast->class_decl.type_var &&
            strcmp(super_var, ast->class_decl.type_var) != 0) {
            tc_codegen_error(ctx,
                "%s:%d:%d: error: unsupported superclass constraint '%s %s' for class '%s %s'; only constraints over the head type variable are currently supported",
                parser_get_filename(), ast->line, ast->column,
                super_name, super_var, ast->class_decl.name,
                ast->class_decl.type_var);
        }
    }

    /* Grow registry if needed */
    if (reg->class_count >= reg->class_cap) {
        reg->class_cap *= 2;
        reg->classes = realloc(reg->classes,
                               sizeof(TCClass) * reg->class_cap);
    }

    TCClass *c = &reg->classes[reg->class_count++];
    c->name       = strdup(ast->class_decl.name);
    c->type_var   = strdup(ast->class_decl.type_var);

    c->superclass_count = ast->class_decl.superclass_count;
    c->superclass_names = malloc(sizeof(char*) * (c->superclass_count ? c->superclass_count : 1));
    c->superclass_type_vars = malloc(sizeof(char*) * (c->superclass_count ? c->superclass_count : 1));
    for (int i = 0; i < c->superclass_count; i++) {
        c->superclass_names[i] = strdup(ast->class_decl.superclass_names[i]);
        c->superclass_type_vars[i] = strdup(ast->class_decl.superclass_type_vars[i]);
    }

    /* Connect associated types */
    c->assoc_count = ast->class_decl.assoc_count;
    c->assoc_types = malloc(sizeof(char*) * (c->assoc_count ? c->assoc_count : 1));
    for (int i = 0; i < c->assoc_count; i++) {
        c->assoc_types[i] = strdup(ast->class_decl.assoc_types[i]);
    }

    /* Copy method signatures */
    c->method_count = ast->class_decl.method_count;
    c->methods = malloc(sizeof(TCMethod) * (c->method_count ? c->method_count : 1));
    for (int i = 0; i < c->method_count; i++) {
        c->methods[i].name     = strdup(ast->class_decl.method_names[i]);
        c->methods[i].type_str = strdup(ast->class_decl.method_types[i]);
    }

    /* Copy default implementations */
    c->default_count = ast->class_decl.default_count;
    c->default_names  = malloc(sizeof(char*) * (c->default_count ? c->default_count : 1));
    c->default_bodies = malloc(sizeof(AST*)  * (c->default_count ? c->default_count : 1));
    for (int i = 0; i < c->default_count; i++) {
        c->default_names[i]  = strdup(ast->class_decl.default_names[i]);
        c->default_bodies[i] = ast_clone(ast->class_decl.default_bodies[i]);
    }

    if (getenv("MONAD_TYPECLASS_DEBUG")) {
        printf("Class: %s %s (%d methods, %d defaults)\n",
               c->name, c->type_var, c->method_count, c->default_count);
        for (int i = 0; i < c->superclass_count; i++)
            printf("  superclass: %s %s\n",
                   c->superclass_names[i], c->superclass_type_vars[i]);
        for (int i = 0; i < c->method_count; i++)
            printf("  method: %s :: %s\n", c->methods[i].name, c->methods[i].type_str);
        for (int i = 0; i < c->default_count; i++)
            printf("  default: %s\n", c->default_names[i]);
    }
}

static Type *my_type_parse_fn_arrow(const char *sig) {
    while (*sig == ' ') sig++;
    if (strncmp(sig, "Fn :: ", 6) == 0) sig += 6;
    while (*sig == ' ') sig++;

    int depth = 0;
    const char *arrow = NULL;
    for (const char *p = sig; *p; p++) {
        if (*p == '(' || *p == '[') depth++;
        else if (*p == ')' || *p == ']') depth--;
        else if (depth == 0 && *p == '-' && *(p+1) == '>') {
            arrow = p;
            break;
        }
    }

    if (arrow) {
        char lhs_str[256] = {0};
        strncpy(lhs_str, sig, arrow - sig);
        char *lhs_start = lhs_str;
        while (*lhs_start == ' ') lhs_start++;
        for (int i = strlen(lhs_start)-1; i >= 0 && lhs_start[i] == ' '; i--) lhs_start[i] = '\0';

        if (lhs_start[0] == '(' && lhs_start[strlen(lhs_start)-1] == ')') {
            lhs_start[strlen(lhs_start)-1] = '\0';
            lhs_start++;
        }

        Type *param = my_type_parse_fn_arrow(lhs_start);

        const char *rhs_start = arrow + 2;
        Type *ret = my_type_parse_fn_arrow(rhs_start);

        return type_arrow(param, ret);
    }

    char base[256] = {0};
    strncpy(base, sig, 255);
    char *start = base;
    while (*start == ' ') start++;
    for (int i = strlen(start)-1; i >= 0 && start[i] == ' '; i--) start[i] = '\0';

    if (start[0] == '(' && start[strlen(start)-1] == ')') {
        start[strlen(start)-1] = '\0';
        start++;
        return my_type_parse_fn_arrow(start);
    }

    /* Preserve type application arguments (for example `Box a`).
     * type_from_name owns parsing compound types; truncating at whitespace
     * silently collapsed higher-kinded instance methods to bare constructors. */
    Type *t = type_from_name(start);
    /* User ADTs are registered in the compilation environment rather than the
     * builtin type-name table.  A specialized instance signature such as
     * `Dial -> Dial` must retain pointer/layout ABI instead of degrading to
     * TYPE_UNKNOWN (which lowers as an integer). */
    if (!t && start[0] && !strchr(start, ' ') &&
        isupper((unsigned char)start[0]))
        t = type_layout_ref(start);
    if (!t) return type_unknown();
    return t;
}

Type *tc_method_result_type(TypeClassRegistry *reg, const char *class_name,
                            const char *method_name) {
    TCClass *class_decl = tc_find_class(reg, class_name);
    if (!class_decl) return NULL;

    for (int i = 0; i < class_decl->method_count; i++) {
        TCMethod *method = &class_decl->methods[i];
        if (!method->name || !method->type_str ||
            strcmp(method->name, method_name) != 0)
            continue;

        Type *signature = my_type_parse_fn_arrow(method->type_str);
        Type *result = signature;
        while (result && result->kind == TYPE_ARROW)
            result = result->arrow_ret;
        Type *owned_result = result ? type_clone(result) : NULL;
        type_free(signature);
        return owned_result;
    }
    return NULL;
}

/// Instance registration
//
// For each method in the instance:
//   1. Compile the lambda body into an LLVM function named __impl_Class_Type_method
//   2. Fill the default implementations for any missing methods
//   3. Build the dictionary struct global __dict_Class_Type and initialize it
//      with pointers to the method functions
//   4. Register each method in the env so (= x y) can be dispatched
//
void tc_register_instance(TypeClassRegistry *reg, AST *ast,
                          CodegenContext *ctx) {
    if (!ast || ast->type != AST_INSTANCE) return;

    const char *class_name = ast->instance_decl.class_name;
    const char *type_name  = ast->instance_decl.type_name;

    TCClass *c = tc_find_class(reg, class_name);
    if (!c) {
        fprintf(stderr, "instance: unknown class '%s'\n", class_name);
        return;
    }

    for (int si = 0; si < c->superclass_count; si++) {
        const char *super_name = c->superclass_names[si];
        if (!tc_find_instance(reg, super_name, type_name)) {
            tc_codegen_error(ctx,
                "%s:%d:%d: error: superclass constraint requires instance '%s %s' before '%s %s'",
                parser_get_filename(), ast->line, ast->column,
                super_name, type_name, class_name, type_name);
        }
    }

    /* Grow instance registry if needed */
    if (reg->instance_count >= reg->instance_cap) {
        reg->instance_cap *= 2;
        reg->instances = realloc(reg->instances,
                                 sizeof(TCInstance) * reg->instance_cap);
    }

    TCInstance *inst = &reg->instances[reg->instance_count++];
    inst->class_name   = strdup(class_name);
    inst->type_name    = strdup(type_name);

    /* Connect associated values */
    inst->assoc_count  = ast->instance_decl.assoc_count;
    inst->assoc_names  = malloc(sizeof(char*) * (inst->assoc_count ? inst->assoc_count : 1));
    inst->assoc_values = malloc(sizeof(char*) * (inst->assoc_count ? inst->assoc_count : 1));
    for (int i = 0; i < inst->assoc_count; i++) {
        inst->assoc_names[i]  = strdup(ast->instance_decl.assoc_names[i]);
        inst->assoc_values[i] = strdup(ast->instance_decl.assoc_values[i]);
    }

    inst->method_count = c->method_count; /* all methods, including defaults */
    inst->method_names = malloc(sizeof(char*)        * c->method_count);
    inst->method_funcs = malloc(sizeof(LLVMValueRef) * c->method_count);
    inst->method_symbols = malloc(sizeof(char*) * c->method_count);

    LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);

    /* ── Step 0: initialize method name slots only ──────────────────────── */
    for (int mi = 0; mi < c->method_count; mi++) {
        inst->method_names[mi] = strdup(c->methods[mi].name);
        inst->method_funcs[mi] = NULL;
        inst->method_symbols[mi] = NULL;
    }
    /* ── Step 1: compile each explicitly provided method ───────────────── */
    for (int mi = 0; mi < c->method_count; mi++) {
        const char *mname = c->methods[mi].name;

        /* Find this method in the instance declaration */
        AST *body_lam = NULL;
        bool uses_default = false;
        for (int ji = 0; ji < ast->instance_decl.method_count; ji++) {
            if (!ast->instance_decl.method_names[ji]) continue;
            if (strcmp(ast->instance_decl.method_names[ji], mname) == 0) {
                body_lam = ast->instance_decl.method_bodies[ji];
                break;
            }
        }

        /* Fall back to default implementation if not provided */
        if (!body_lam) {
            for (int di = 0; di < c->default_count; di++) {
                if (strcmp(c->default_names[di], mname) == 0) {
                    body_lam = c->default_bodies[di];
                    uses_default = true;
                    break;
                }
            }
        }

        if (!body_lam) {
            fprintf(stderr, "instance %s %s: no implementation for method '%s' "
                    "and no default\n", class_name, type_name, mname);
            continue;
        }

        /* Build LLVM function name */
        char fn_name[256];
        tc_method_name(class_name, type_name, mname, fn_name, sizeof(fn_name));

        /* Clone the lambda and annotate each param with the concrete
         * instance type so codegen uses typed ABI instead of poly stub */
        AST *typed_lam = ast_clone(body_lam);
        if (uses_default) {
            free(typed_lam->lambda.docstring);
            typed_lam->lambda.docstring = strdup("__monad_typeclass_default__");
        }
        char sig_buf[512];
        snprintf(sig_buf, sizeof(sig_buf), "Fn :: %s", c->methods[mi].type_str);

        /* Apply the exact same rewriting to the implementation signature */
        for (int k = 0; k < inst->assoc_count; k++) {
            char target[128];
            snprintf(target, sizeof(target), "%s %s", inst->assoc_names[k], c->type_var);
            char *pos;
            while ((pos = strstr(sig_buf, target)) != NULL) {
                char temp[512];
                int len_before = pos - sig_buf;
                snprintf(temp, sizeof(temp), "%.*s%s%s", len_before, sig_buf, inst->assoc_values[k], pos + strlen(target));
                strcpy(sig_buf, temp);
            }
        }
        {
            char bare_target[64];
            snprintf(bare_target, sizeof(bare_target), "%s", c->type_var);
            char bare_replace[128];
            snprintf(bare_replace, sizeof(bare_replace), "%s", inst->type_name);

            char temp_sig[512];
            strcpy(temp_sig, sig_buf);
            char *pos = temp_sig;
            size_t t_len = strlen(bare_target);

            while ((pos = strstr(pos, bare_target)) != NULL) {
                bool left_bound = (pos == temp_sig) || !((*(pos - 1) >= 'a' && *(pos - 1) <= 'z') || (*(pos - 1) >= 'A' && *(pos - 1) <= 'Z') || (*(pos - 1) >= '0' && *(pos - 1) <= '9') || *(pos - 1) == '_');
                bool right_bound = !((*(pos + t_len) >= 'a' && *(pos + t_len) <= 'z') || (*(pos + t_len) >= 'A' && *(pos + t_len) <= 'Z') || (*(pos + t_len) >= '0' && *(pos + t_len) <= '9') || *(pos + t_len) == '_');

                if (left_bound && right_bound) {
                    char temp[512];
                    int len_before = pos - temp_sig;
                    snprintf(temp, sizeof(temp), "%.*s%s%s", len_before, temp_sig, bare_replace, pos + t_len);
                    strcpy(temp_sig, temp);
                    pos = temp_sig + len_before + strlen(bare_replace);
                } else {
                    pos += t_len;
                }
            }
            strcpy(sig_buf, temp_sig);
        }

        Type *method_sig = my_type_parse_fn_arrow(sig_buf);
        Type *t_iter = method_sig;

        for (int pi = 0; pi < typed_lam->lambda.param_count; pi++) {
            /* The specialized class contract is authoritative. The reader's
             * instance lowering may seed every parameter with the head type
             * (`Box`), which is wrong for higher-order parameters such as the
             * mapping function in `(a -> b) -> Box a -> Box b`. */
            if (t_iter && t_iter->kind == TYPE_ARROW) {
                if (t_iter->arrow_param &&
                    t_iter->arrow_param->kind != TYPE_UNKNOWN) {
                    const char *param_type = type_to_string(t_iter->arrow_param);
                    free(typed_lam->lambda.params[pi].type_name);
                    if (t_iter->arrow_param->kind == TYPE_ARROW) {
                        char grouped[512];
                        snprintf(grouped, sizeof(grouped), "(%s)", param_type);
                        typed_lam->lambda.params[pi].type_name = strdup(grouped);
                    } else {
                        typed_lam->lambda.params[pi].type_name = strdup(param_type);
                    }
                }
            } else if (t_iter && t_iter->kind != TYPE_UNKNOWN) {
                free(typed_lam->lambda.params[pi].type_name);
                typed_lam->lambda.params[pi].type_name = strdup(type_to_string(t_iter));
            }
            if (t_iter && t_iter->kind == TYPE_ARROW) {
                t_iter = t_iter->arrow_ret;
            } else {
                t_iter = NULL;
            }
        }
        if (t_iter && t_iter->kind != TYPE_UNKNOWN) {
            free(typed_lam->lambda.return_type);
            typed_lam->lambda.return_type = strdup(type_to_string(t_iter));
        }

        if (method_sig) type_free(method_sig);

        AST *define_node = ast_new_list();

        ast_list_append(define_node, ast_new_symbol("define"));
        ast_list_append(define_node, ast_new_symbol(fn_name));
        ast_list_append(define_node, typed_lam);

        codegen_expr(ctx, define_node);
        ast_free(define_node);

        /* Look up the compiled function */
        LLVMValueRef fn = LLVMGetNamedFunction(ctx->module, fn_name);
        if (!fn) {
            /* Try env lookup — define may have stored it under original name */
            EnvEntry *e = env_lookup(ctx->env, fn_name);
            if (e) fn = e->func_ref ? e->func_ref : e->value;
        }
        inst->method_funcs[mi] = fn;
        inst->method_symbols[mi] = strdup(fn_name);

        if (getenv("MONAD_TYPECLASS_DEBUG"))
            printf("  compiled method: %s -> %s\n", mname, fn_name);
    }

    /* ── Step 2: build the dictionary global ───────────────────────────── */
    //
    // The dictionary is a global struct of ptr-typed function pointers.
    // Layout matches the order of methods in the class declaration.
    //
    LLVMTypeRef dict_t = tc_dict_type(reg, class_name, ctx);
    if (!dict_t) {
        fprintf(stderr, "instance: could not create dict type for '%s'\n",
                class_name);
        return;
    }

    char dict_gname[256];
    tc_dict_name(class_name, type_name, dict_gname, sizeof(dict_gname));

    LLVMValueRef dict_global = LLVMGetNamedGlobal(ctx->module, dict_gname);
    if (!dict_global) {
        dict_global = LLVMAddGlobal(ctx->module, dict_t, dict_gname);
        LLVMSetLinkage(dict_global, LLVMExternalLinkage);
        LLVMSetInitializer(dict_global, LLVMConstNull(dict_t));
    }
    inst->dict_global = dict_global;

    /* Store each method function pointer into the dict struct */
    LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx->context);
    for (int mi = 0; mi < c->method_count; mi++) {
        if (!inst->method_funcs[mi]) {
            fprintf(stderr, "instance %s %s: method '%s' has no LLVM function, skipping dict slot\n",
                    class_name, type_name,
                    inst->method_names[mi] ? inst->method_names[mi] : "?");
            continue;
        }

        LLVMValueRef fn_ptr = LLVMBuildBitCast(ctx->builder,
                                  inst->method_funcs[mi], ptr_t, "mptr");
        LLVMValueRef zero   = LLVMConstInt(i32, 0, 0);
        LLVMValueRef idx    = LLVMConstInt(i32, mi, 0);
        LLVMValueRef idxs[] = {zero, idx};
        LLVMValueRef slot   = LLVMBuildGEP2(ctx->builder, dict_t,
                                             dict_global, idxs, 2, "dict_slot");
        LLVMBuildStore(ctx->builder, fn_ptr, slot);
    }

    /* ── Step 3: register methods in the env ──────────────────────────── */
    //
    // For each method, register it in the env under its plain name (e.g. "=")
    // pointing to the instance's method function. This enables direct dispatch
    // when the type is known at compile time — the most common case.
    //
    // When the type is not known statically (polymorphic call sites), the
    // dictionary must be passed explicitly — that's the next step.
    //
    for (int mi = 0; mi < c->method_count; mi++) {
        if (!inst->method_funcs[mi]) continue;
        const char *mname = inst->method_names[mi];

        /* Register as ENV_FUNC so direct typed calls work */
        EnvEntry *existing = env_lookup(ctx->env, mname);
        bool is_local_definition = existing && existing->source_ast &&
                                   !existing->module_name;
        bool replace_dispatch_entry = existing && tc_is_method(reg, mname) &&
                                      !is_local_definition;
        if (replace_dispatch_entry) {
            /* Replace the generic inference placeholder (or a previous
             * instance's dispatch trigger) with this implementation's real
             * arity. A user/core definition already occupying the plain name
             * remains lexically authoritative. */
            env_remove(ctx->env, mname);
            existing = NULL;
        }
        if (!existing) {
            int param_count = (int)LLVMCountParams(inst->method_funcs[mi]);
            EnvParam *params = malloc(sizeof(EnvParam) *
                                      (param_count ? param_count : 1));
            for (int pi = 0; pi < param_count; pi++) {
                params[pi].name = strdup("__arg");
                params[pi].type = type_unknown();
            }
            Type *return_type = tc_method_result_type(reg, c->name, mname);
            if (!return_type) return_type = type_unknown();
            env_insert_func(ctx->env, mname, params, param_count,
                            return_type, inst->method_funcs[mi], NULL, NULL);
            EnvEntry *e = env_lookup(ctx->env, mname);
            if (e) {
                e->is_closure_abi = false;
                e->lifted_count   = 0;
            }
        }
    }

    if (getenv("MONAD_TYPECLASS_DEBUG"))
        printf("Instance: %s %s -> dict %s\n",
               class_name, type_name, dict_gname);
}

/// Method dispatch
//
// Returns the LLVM function for a specific method in a specific instance.
// Used when the type is known statically at the call site.
//
LLVMValueRef tc_get_method(TypeClassRegistry *reg, const char *class_name,
                            const char *type_name, const char *method_name,
                            CodegenContext *ctx) {
    TCInstance *inst = tc_find_instance(reg, class_name, type_name);
    if (!inst) return NULL;

    for (int i = 0; i < inst->method_count; i++) {
        if (strcmp(inst->method_names[i], method_name) == 0)
            return inst->method_funcs[i];
    }
    return NULL;
}

const char *tc_type_name_from_llvm(LLVMValueRef val) {
    if (!val) return NULL;
    LLVMTypeRef t = LLVMTypeOf(val);
    LLVMTypeKind k = LLVMGetTypeKind(t);
    if (k == LLVMDoubleTypeKind) return "Float";
    if (k == LLVMFloatTypeKind)  return "Float";
    if (k == LLVMIntegerTypeKind) {
        unsigned w = LLVMGetIntTypeWidth(t);
        if (w == 1)  return "Bool";
        if (w == 64) return "Int";
        if (w == 32) return "Int";
    }
    if (k == LLVMPointerTypeKind) return NULL; /* could be Vec3 or anything heap-allocated */
    return NULL;
}

LLVMValueRef tc_get_dict(TypeClassRegistry *reg, const char *class_name,
                         const char *type_name, CodegenContext *ctx) {
    TCInstance *inst = tc_find_instance(reg, class_name, type_name);
    if (!inst) return NULL;

    /* Re-declare in current module if needed (REPL cross-module) */
    LLVMTypeRef dict_t = tc_dict_type(reg, class_name, ctx);
    if (!dict_t) return NULL;

    char dict_gname[256];
    tc_dict_name(class_name, type_name, dict_gname, sizeof(dict_gname));

    LLVMValueRef gv = LLVMGetNamedGlobal(ctx->module, dict_gname);
    if (!gv) {
        gv = LLVMAddGlobal(ctx->module, dict_t, dict_gname);
        LLVMSetLinkage(gv, LLVMExternalLinkage);
    }
    return gv;
}
