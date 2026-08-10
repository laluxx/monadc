#include "core.h"
#include "place.h"
#include "../infer.h"

#include <stdlib.h>
#include <string.h>

extern bool qtt_compiler_lookup_callable_authority(
    uint64_t module_id, const char *visible_name,
    const char **portable_contract, uint64_t *contract_fingerprint,
    const char **portable_hm_scheme) __attribute__((weak));
extern bool qtt_compiler_lookup_effect_judgment(
    uint64_t module_id, const char *visible_name,
    uint64_t *row_fingerprint, uint64_t *constraint_fingerprint,
    int *constraint_result, const size_t **predicate_stages,
    const char *const **predicate_names, size_t *predicate_count)
    __attribute__((weak));
extern bool infer_callable_contract_deserialize(
    InferCallableContract *contract,
    const char *text) __attribute__((weak));
extern void infer_callable_contract_free(
    InferCallableContract *contract) __attribute__((weak));

static char *core_copy_text(const char *text) {
    if (!text) return NULL;
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
}

#if defined(__GNUC__) || defined(__clang__)
extern const Type *qtt_bindings_layout_type(const char *name)
    __attribute__((weak));
extern Type *type_from_name(const char *name) __attribute__((weak));
#else
extern const Type *qtt_bindings_layout_type(const char *name);
#endif

static const Type *source_layout_type(const char *name) {
    return qtt_bindings_layout_type
        ? qtt_bindings_layout_type(name) : NULL;
}

typedef struct LoweringTypeScope {
    QttCoreVar var;
    const Type *type;
    const struct LoweringTypeScope *parent;
} LoweringTypeScope;

typedef struct {
    uint64_t module_id;
    uint64_t next_synthetic_binder;
    QttCoreError error;
    const LoweringTypeScope *types;
} Lowering;

typedef struct ValidationScope {
    QttCoreVar var;
    bool loan;
    const struct ValidationScope *parent;
} ValidationScope;

/* Stable primitive witnesses used when legacy inference leaves an otherwise
 * self-describing literal unannotated. */
static const Type core_string_type = {.kind = TYPE_STRING};
static const Type core_path_type = {.kind = TYPE_PATH};
static const Type core_char_type = {.kind = TYPE_CHAR};
static const Type core_keyword_type = {.kind = TYPE_KEYWORD};
static const Type core_ratio_type = {.kind = TYPE_RATIO};

/* FNV-1a gives reproducible nominal identities without pointer or process
 * identity.  Source position makes two uses of one declared capability
 * distinct; module identity prevents accidental cross-module capture. */
static uint64_t capability_id(uint64_t module_id, const char *kind,
                              const char *name, int line, int column) {
    uint64_t hash = UINT64_C(1469598103934665603);
#define HASH_BYTE(value) do { hash ^= (uint8_t)(value); \
    hash *= UINT64_C(1099511628211); } while (0)
    for (unsigned shift = 0; shift < 64; shift += 8)
        HASH_BYTE(module_id >> shift);
    for (const char *text = kind; *text; text++) HASH_BYTE(*text);
    HASH_BYTE(0xff);
    for (const char *text = name; *text; text++) HASH_BYTE(*text);
    HASH_BYTE(0xfe);
    for (unsigned shift = 0; shift < 32; shift += 8)
        HASH_BYTE((uint32_t)line >> shift);
    for (unsigned shift = 0; shift < 32; shift += 8)
        HASH_BYTE((uint32_t)column >> shift);
#undef HASH_BYTE
    return hash ? hash : UINT64_C(1);
}

bool qtt_core_var_equal(QttCoreVar left, QttCoreVar right) {
    return left.module_id == right.module_id &&
           left.binder_id == right.binder_id;
}

const Type *qtt_core_var_type(
    const QttCoreNode *node, QttCoreVar var) {
    if (!node) return NULL;
    if (node->kind == QTT_CORE_VAR &&
        qtt_core_var_equal(node->var, var))
        return node->type;
    const Type *found = NULL;
    switch (node->kind) {
    case QTT_CORE_LAMBDA:
        return qtt_core_var_type(node->lambda.body, var);
    case QTT_CORE_APPLY:
        found = qtt_core_var_type(node->apply.callee, var);
        for (size_t i = 0; !found && i < node->apply.argument_count; i++)
            found = qtt_core_var_type(node->apply.arguments[i], var);
        return found;
    case QTT_CORE_LET:
        found = qtt_core_var_type(node->let.value, var);
        return found ? found : qtt_core_var_type(node->let.body, var);
    case QTT_CORE_IF:
        found = qtt_core_var_type(node->conditional.condition, var);
        if (!found)
            found = qtt_core_var_type(
                node->conditional.then_branch, var);
        return found ? found :
            qtt_core_var_type(node->conditional.else_branch, var);
    case QTT_CORE_SEQUENCE:
        for (size_t i = 0; !found && i < node->sequence.count; i++)
            found = qtt_core_var_type(node->sequence.items[i], var);
        return found;
    case QTT_CORE_WRITE:
        return qtt_core_var_equal(node->write.place.root, var)
            ? node->type
            : qtt_core_var_type(node->write.value, var);
    case QTT_CORE_BORROW:
        if (qtt_core_var_equal(node->borrow.binding, var))
            return node->type;
        return qtt_core_var_type(node->borrow.body, var);
    case QTT_CORE_PERFORM:
        return qtt_core_var_type(node->perform.argument, var);
    case QTT_CORE_HANDLE:
        found = qtt_core_var_type(node->handle.computation, var);
        return found ? found : qtt_core_var_type(node->handle.clause, var);
    case QTT_CORE_PLACE:
        return qtt_core_var_equal(node->place.root, var)
            ? node->type : NULL;
    case QTT_CORE_LITERAL: case QTT_CORE_GLOBAL: case QTT_CORE_VAR:
    case QTT_CORE_QUOTE:
        return NULL;
    }
    return NULL;
}

static bool scope_contains(const ValidationScope *scope, QttCoreVar var) {
    for (; scope; scope = scope->parent)
        if (qtt_core_var_equal(scope->var, var)) return true;
    return false;
}

static bool scope_contains_loan(
    const ValidationScope *scope, QttCoreVar var) {
    for (; scope; scope = scope->parent)
        if (scope->loan && qtt_core_var_equal(scope->var, var)) return true;
    return false;
}

static QttCoreValidation validate_node(const QttCoreNode *node,
                                       uint64_t module_id,
                                       const ValidationScope *scope) {
    if (!node) return QTT_CORE_UNBOUND_VAR;
    switch (node->kind) {
    case QTT_CORE_VAR:
        if (node->var.module_id != module_id)
            return QTT_CORE_WRONG_MODULE;
        return scope_contains(scope, node->var)
            ? QTT_CORE_VALID : QTT_CORE_UNBOUND_VAR;
    case QTT_CORE_LAMBDA: {
        const ValidationScope *inner = scope;
        ValidationScope *entries =
            calloc(node->lambda.param_count, sizeof(*entries));
        if (node->lambda.param_count && !entries)
            return QTT_CORE_VALIDATION_OUT_OF_MEMORY;
        for (size_t i = 0; i < node->lambda.param_count; i++) {
            if (node->lambda.params[i].module_id != module_id) {
                free(entries);
                return QTT_CORE_WRONG_MODULE;
            }
            if (scope_contains(inner, node->lambda.params[i])) {
                free(entries);
                return QTT_CORE_DUPLICATE_BINDER;
            }
            entries[i].var = node->lambda.params[i];
            entries[i].parent = inner;
            inner = &entries[i];
        }
        QttCoreValidation result =
            validate_node(node->lambda.body, module_id, inner);
        free(entries);
        return result;
    }
    case QTT_CORE_APPLY: {
        QttCoreValidation result =
            validate_node(node->apply.callee, module_id, scope);
        for (size_t i = 0;
             result == QTT_CORE_VALID && i < node->apply.argument_count;
             i++)
            result = validate_node(node->apply.arguments[i], module_id, scope);
        return result;
    }
    case QTT_CORE_LET: {
        QttCoreValidation result =
            validate_node(node->let.value, module_id, scope);
        if (result != QTT_CORE_VALID) return result;
        if (node->let.binding.module_id != module_id)
            return QTT_CORE_WRONG_MODULE;
        if (scope_contains(scope, node->let.binding))
            return QTT_CORE_DUPLICATE_BINDER;
        ValidationScope binding = {
            .var = node->let.binding,
            .parent = scope,
        };
        return validate_node(node->let.body, module_id, &binding);
    }
    case QTT_CORE_IF: {
        QttCoreValidation result =
            validate_node(node->conditional.condition, module_id, scope);
        if (result == QTT_CORE_VALID)
            result = validate_node(node->conditional.then_branch,
                                   module_id, scope);
        if (result == QTT_CORE_VALID)
            result = validate_node(node->conditional.else_branch,
                                   module_id, scope);
        return result;
    }
    case QTT_CORE_SEQUENCE:
        for (size_t i = 0; i < node->sequence.count; i++) {
            QttCoreValidation result =
                validate_node(node->sequence.items[i], module_id, scope);
            if (result != QTT_CORE_VALID) return result;
        }
        return QTT_CORE_VALID;
    case QTT_CORE_WRITE:
        if (node->write.place.root.module_id != module_id)
            return QTT_CORE_WRONG_MODULE;
        if (!scope_contains(scope, node->write.place.root))
            return QTT_CORE_UNBOUND_VAR;
        return validate_node(node->write.value, module_id, scope);
    case QTT_CORE_BORROW: {
        if (node->borrow.binding.module_id != module_id ||
            node->borrow.place.root.module_id != module_id ||
            (node->borrow.parent.binder_id &&
             node->borrow.parent.module_id != module_id))
            return QTT_CORE_WRONG_MODULE;
        if (scope_contains(scope, node->borrow.binding))
            return QTT_CORE_DUPLICATE_BINDER;
        if (!scope_contains(scope, node->borrow.place.root) ||
            (node->borrow.parent.binder_id &&
             !scope_contains_loan(scope, node->borrow.parent)))
            return QTT_CORE_UNBOUND_VAR;
        if (node->borrow.loan_kind != QTT_LOAN_SHARED &&
            node->borrow.loan_kind != QTT_LOAN_EXCLUSIVE)
            return QTT_CORE_UNBOUND_VAR;
        ValidationScope loan = {
            .var = node->borrow.binding, .loan = true, .parent = scope};
        return validate_node(node->borrow.body, module_id, &loan);
    }
    case QTT_CORE_PERFORM:
        if (!node->perform.effect_name || !*node->perform.effect_name ||
            !node->perform.capability_id)
            return QTT_CORE_UNBOUND_VAR;
        return validate_node(node->perform.argument, module_id, scope);
    case QTT_CORE_HANDLE: {
        if (!node->handle.profile_name || !*node->handle.profile_name ||
            !node->handle.capability_id)
            return QTT_CORE_UNBOUND_VAR;
        QttCoreValidation result = validate_node(
            node->handle.computation, module_id, scope);
        return result == QTT_CORE_VALID
            ? validate_node(node->handle.clause, module_id, scope) : result;
    }
    case QTT_CORE_PLACE:
        if (node->place.root.module_id != module_id)
            return QTT_CORE_WRONG_MODULE;
        return scope_contains(scope, node->place.root)
            ? QTT_CORE_VALID : QTT_CORE_UNBOUND_VAR;
    default:
        return QTT_CORE_VALID;
    }
}

QttCoreValidation qtt_core_validate(const QttCoreNode *node,
                                    uint64_t module_id) {
    return validate_node(node, module_id, NULL);
}

typedef struct {
    QttCoreVar *items;
    size_t count;
    size_t capacity;
    QttCoreError error;
} CaptureSet;

static bool capture_push(CaptureSet *set, QttCoreVar var) {
    for (size_t i = 0; i < set->count; i++)
        if (qtt_core_var_equal(set->items[i], var)) return true;
    if (set->count == set->capacity) {
        size_t next = set->capacity ? set->capacity * 2 : 8;
        QttCoreVar *grown = realloc(set->items, next * sizeof(*grown));
        if (!grown) {
            set->error = QTT_CORE_OUT_OF_MEMORY;
            return false;
        }
        set->items = grown;
        set->capacity = next;
    }
    set->items[set->count++] = var;
    return true;
}

static void collect_captures(const QttCoreNode *node,
                             const ValidationScope *bound,
                             CaptureSet *captures) {
    if (!node || captures->error != QTT_CORE_OK) return;
    switch (node->kind) {
    case QTT_CORE_VAR:
        if (!scope_contains(bound, node->var))
            capture_push(captures, node->var);
        return;
    case QTT_CORE_LAMBDA: {
        const ValidationScope *inner = bound;
        ValidationScope *entries =
            calloc(node->lambda.param_count, sizeof(*entries));
        if (node->lambda.param_count && !entries) {
            captures->error = QTT_CORE_OUT_OF_MEMORY;
            return;
        }
        for (size_t i = 0; i < node->lambda.param_count; i++) {
            entries[i] = (ValidationScope){
                .var = node->lambda.params[i], .parent = inner};
            inner = &entries[i];
        }
        collect_captures(node->lambda.body, inner, captures);
        free(entries);
        return;
    }
    case QTT_CORE_LET: {
        collect_captures(node->let.value, bound, captures);
        ValidationScope binding = {
            .var = node->let.binding, .parent = bound};
        collect_captures(node->let.body, &binding, captures);
        return;
    }
    case QTT_CORE_APPLY:
        collect_captures(node->apply.callee, bound, captures);
        for (size_t i = 0; i < node->apply.argument_count; i++)
            collect_captures(node->apply.arguments[i], bound, captures);
        return;
    case QTT_CORE_IF:
        collect_captures(node->conditional.condition, bound, captures);
        collect_captures(node->conditional.then_branch, bound, captures);
        collect_captures(node->conditional.else_branch, bound, captures);
        return;
    case QTT_CORE_SEQUENCE:
        for (size_t i = 0; i < node->sequence.count; i++)
            collect_captures(node->sequence.items[i], bound, captures);
        return;
    case QTT_CORE_WRITE:
        if (!scope_contains(bound, node->write.place.root))
            capture_push(captures, node->write.place.root);
        collect_captures(node->write.value, bound, captures);
        return;
    case QTT_CORE_BORROW: {
        if (!scope_contains(bound, node->borrow.place.root))
            capture_push(captures, node->borrow.place.root);
        ValidationScope loan = {
            .var = node->borrow.binding, .loan = true, .parent = bound};
        collect_captures(node->borrow.body, &loan, captures);
        return;
    }
    case QTT_CORE_PERFORM:
        collect_captures(node->perform.argument, bound, captures);
        return;
    case QTT_CORE_HANDLE:
        collect_captures(node->handle.computation, bound, captures);
        collect_captures(node->handle.clause, bound, captures);
        return;
    case QTT_CORE_PLACE:
        if (!scope_contains(bound, node->place.root))
            capture_push(captures, node->place.root);
        return;
    default:
        return;
    }
}

QttCoreError qtt_core_lambda_captures(const QttCoreNode *lambda,
                                      QttCoreVar **captures,
                                      size_t *capture_count) {
    if (!lambda || lambda->kind != QTT_CORE_LAMBDA ||
        !captures || !capture_count)
        return QTT_CORE_MALFORMED_AST;
    CaptureSet set = {.error = QTT_CORE_OK};
    collect_captures(lambda, NULL, &set);
    if (set.error != QTT_CORE_OK) {
        free(set.items);
        return set.error;
    }
    *captures = set.items;
    *capture_count = set.count;
    return QTT_CORE_OK;
}

void qtt_core_free(QttCoreNode *node) {
    if (!node) return;
    switch (node->kind) {
    case QTT_CORE_GLOBAL:
        if (node->global.authority_owned) {
            free(node->global.callable_contract);
            free(node->global.hm_scheme);
            for (size_t i = 0;
                 i < node->global.effect_predicate_count; i++)
                free(node->global.effect_predicate_names[i]);
            free(node->global.effect_predicate_names);
            free(node->global.effect_predicate_stages);
        }
        break;
    case QTT_CORE_LAMBDA:
        free(node->lambda.params);
        free(node->lambda.param_types);
        qtt_core_free(node->lambda.body);
        break;
    case QTT_CORE_APPLY:
        qtt_core_free(node->apply.callee);
        for (size_t i = 0; i < node->apply.argument_count; i++)
            qtt_core_free(node->apply.arguments[i]);
        free(node->apply.arguments);
        break;
    case QTT_CORE_LET:
        qtt_core_free(node->let.value);
        qtt_core_free(node->let.body);
        break;
    case QTT_CORE_IF:
        qtt_core_free(node->conditional.condition);
        qtt_core_free(node->conditional.then_branch);
        qtt_core_free(node->conditional.else_branch);
        break;
    case QTT_CORE_SEQUENCE:
        for (size_t i = 0; i < node->sequence.count; i++)
            qtt_core_free(node->sequence.items[i]);
        free(node->sequence.items);
        break;
    case QTT_CORE_WRITE:
        qtt_core_free(node->write.value);
        break;
    case QTT_CORE_BORROW:
        qtt_core_free(node->borrow.body);
        break;
    case QTT_CORE_PERFORM:
        qtt_core_free(node->perform.argument);
        break;
    case QTT_CORE_HANDLE:
        free(node->handle.portable_proof);
        qtt_core_free(node->handle.computation);
        qtt_core_free(node->handle.clause);
        break;
    default:
        break;
    }
    free(node);
}

static QttCoreNode *lower_ast(Lowering *lowering, const AST *ast);

static const Type *lowering_var_type(const Lowering *lowering,
                                     QttCoreVar var) {
    for (const LoweringTypeScope *scope = lowering->types;
         scope; scope = scope->parent)
        if (qtt_core_var_equal(scope->var, var)) return scope->type;
    return NULL;
}

static const Type *resolve_layout_type(const Type *type) {
    if (!type) return NULL;
    if (type->kind == TYPE_LAYOUT && type->layout_field_count > 0)
        return type;
    const char *name = type->kind == TYPE_APP
        ? type->app_constructor
        : type->kind == TYPE_LAYOUT ? type->layout_name : NULL;
    const Type *resolved = source_layout_type(name);
    return resolved ? resolved : type;
}

static bool lower_structural_place(Lowering *lowering, const AST *ast,
                                   QttPlace *place,
                                   const Type **projected_type) {
    const char *dot = strchr(ast->symbol, '.');
    if (!dot || !ast->resolved_binder_id) return false;
    QttCoreVar root = {
        .module_id = lowering->module_id,
        .binder_id = ast->resolved_binder_id,
    };
    const Type *layout = resolve_layout_type(
        lowering_var_type(lowering, root));
    QttPlace current = qtt_place_root(root);
    while (dot && dot[1]) {
        const char *start = dot + 1;
        const char *next = strchr(start, '.');
        size_t length = next ? (size_t)(next - start) : strlen(start);
        char *field = malloc(length + 1);
        if (!field) {
            lowering->error = QTT_CORE_OUT_OF_MEMORY;
            return false;
        }
        memcpy(field, start, length);
        field[length] = '\0';
        QttPlace projected = {0};
        bool found = qtt_place_layout_field(
            current, layout, field, &projected);
        const Type *field_type = NULL;
        if (found)
            for (int i = 0; i < layout->layout_field_count; i++)
                if (layout->layout_fields[i].name &&
                    strcmp(layout->layout_fields[i].name, field) == 0) {
                    field_type = layout->layout_fields[i].type;
                    break;
                }
        free(field);
        if (!found || !field_type) return false;
        current = projected;
        layout = field_type;
        dot = next;
    }
    if (dot && !dot[1]) return false;
    *place = current;
    if (projected_type) *projected_type = layout;
    return current.projection_depth > 0;
}

static QttCoreNode *new_node(Lowering *lowering, QttCoreKind kind,
                             const AST *source) {
    QttCoreNode *node = calloc(1, sizeof(*node));
    if (!node) {
        lowering->error = QTT_CORE_OUT_OF_MEMORY;
        return NULL;
    }
    node->kind = kind;
    node->type = source ? source->inferred_type : NULL;
    node->source = source;
    node->line = source ? source->line : 0;
    node->column = source ? source->column : 0;
    return node;
}

static bool list_head_is(const AST *ast, const char *name) {
    return ast && ast->type == AST_LIST && ast->list.count &&
           ast->list.items[0] &&
           ast->list.items[0]->type == AST_SYMBOL &&
           strcmp(ast->list.items[0]->symbol, name) == 0;
}

/*
 * A saturated layout constructor whose fields are literal values is a pure,
 * closed allocation.  Keeping it as one Core literal preserves the aggregate
 * allocation/destructor boundary without pretending the constructor is an
 * ordinary callable with an unavailable interprocedural signature.  General
 * constructor arguments must remain applications until aggregate construction
 * has its own effectful Core rule.
 */
static bool is_closed_layout_literal(const AST *ast) {
    const Type *layout = ast ? resolve_layout_type(ast->inferred_type) : NULL;
    if ((!layout || layout->kind != TYPE_LAYOUT) && ast &&
        ast->type == AST_LIST && ast->list.count && ast->list.items[0] &&
        ast->list.items[0]->type == AST_SYMBOL)
        layout = source_layout_type(ast->list.items[0]->symbol);
    if (!ast || ast->type != AST_LIST || !layout ||
        layout->kind != TYPE_LAYOUT || !layout->layout_name ||
        ast->list.count == 0 ||
        !ast->list.items[0] || ast->list.items[0]->type != AST_SYMBOL ||
        strcmp(ast->list.items[0]->symbol,
               layout->layout_name) != 0 ||
        layout->layout_field_count <= 0 ||
        ast->list.count !=
            (size_t)layout->layout_field_count + 1)
        return false;
    for (size_t i = 1; i < ast->list.count; i++) {
        ASTType kind = ast->list.items[i]->type;
        if (kind != AST_NUMBER && kind != AST_STRING &&
            kind != AST_CHAR && kind != AST_PATH &&
            kind != AST_KEYWORD && kind != AST_RATIO)
            return false;
    }
    return true;
}

static QttCoreNode *lower_sequence(Lowering *lowering, AST *const *items,
                                   size_t count, const AST *source) {
    if (count == 1) return lower_ast(lowering, items[0]);
    QttCoreNode *node = new_node(lowering, QTT_CORE_SEQUENCE, source);
    if (!node) return NULL;
    node->sequence.items = calloc(count, sizeof(*node->sequence.items));
    if (!node->sequence.items) {
        lowering->error = QTT_CORE_OUT_OF_MEMORY;
        qtt_core_free(node);
        return NULL;
    }
    node->sequence.count = count;
    for (size_t i = 0; i < count; i++) {
        node->sequence.items[i] = lower_ast(lowering, items[i]);
        if (!node->sequence.items[i]) {
            qtt_core_free(node);
            return NULL;
        }
    }
    /* A sequence has the type of its final expression.  In particular, a
     * lambda body must not inherit the enclosing lambda's arrow type. */
    node->type = node->sequence.items[count - 1]->type;
    return node;
}

static QttCoreNode *lower_local_bindings(Lowering *lowering,
                                         const AST *source,
                                         const AST *bindings,
                                         size_t binding_index) {
    if (binding_index >= bindings->array.element_count)
        return lower_sequence(lowering, source->list.items + 2,
                              source->list.count - 2, source);
    if (binding_index + 1 >= bindings->array.element_count) {
        lowering->error = QTT_CORE_MALFORMED_AST;
        return NULL;
    }
    const AST *name = bindings->array.elements[binding_index];
    if (!name || name->type != AST_SYMBOL ||
        name->resolved_binder_id == 0) {
        lowering->error = QTT_CORE_MALFORMED_AST;
        return NULL;
    }
    QttCoreNode *node = new_node(lowering, QTT_CORE_LET, source);
    if (!node) return NULL;
    node->let.binding = (QttCoreVar){
        .module_id = lowering->module_id,
        .binder_id = name->resolved_binder_id,
    };
    node->let.value =
        lower_ast(lowering, bindings->array.elements[binding_index + 1]);
    const Type *binding_type = resolve_layout_type(name->inferred_type);
    if (!binding_type || binding_type->kind != TYPE_LAYOUT)
        binding_type = node->let.value ? node->let.value->type : NULL;
    LoweringTypeScope binding_scope = {
        .var = node->let.binding,
        .type = binding_type,
        .parent = lowering->types,
    };
    const LoweringTypeScope *outer_types = lowering->types;
    if (node->let.value) {
        lowering->types = &binding_scope;
        node->let.body = lower_local_bindings(lowering, source, bindings,
                                              binding_index + 2);
        lowering->types = outer_types;
    }
    if (!node->let.value || !node->let.body) {
        qtt_core_free(node);
        return NULL;
    }
    if (!node->type) node->type = node->let.body->type;
    return node;
}

static QttCoreNode *lower_list(Lowering *lowering, const AST *ast) {
    if (!ast->list.count) {
        lowering->error = QTT_CORE_MALFORMED_AST;
        return NULL;
    }
    if (is_closed_layout_literal(ast)) {
        QttCoreNode *node = new_node(lowering, QTT_CORE_LITERAL, ast);
        if (node) {
            node->literal.source = ast;
            node->type = resolve_layout_type(ast->inferred_type);
            if (!node->type || node->type->kind != TYPE_LAYOUT)
                node->type = source_layout_type(
                    ast->list.items[0]->symbol);
        }
        return node;
    }
    if (list_head_is(ast, "quote")) {
        QttCoreNode *node = new_node(lowering, QTT_CORE_QUOTE, ast);
        if (node) node->literal.source = ast;
        return node;
    }
    if (list_head_is(ast, "perform")) {
        if (ast->list.count != 3 || !ast->list.items[1] ||
            ast->list.items[1]->type != AST_SYMBOL ||
            !ast->list.items[1]->symbol || !*ast->list.items[1]->symbol) {
            lowering->error = QTT_CORE_MALFORMED_AST;
            return NULL;
        }
        QttCoreNode *node = new_node(lowering, QTT_CORE_PERFORM, ast);
        if (!node) return NULL;
        node->perform.effect_name = ast->list.items[1]->symbol;
        node->perform.capability_id = capability_id(
            lowering->module_id, "perform", node->perform.effect_name,
            ast->line, ast->column);
        node->perform.argument = lower_ast(lowering, ast->list.items[2]);
        if (!node->perform.argument) {
            qtt_core_free(node);
            return NULL;
        }
        return node;
    }
    if (list_head_is(ast, "handle")) {
        if (ast->list.count != 4 || !ast->list.items[1] ||
            ast->list.items[1]->type != AST_SYMBOL ||
            !ast->list.items[1]->symbol || !*ast->list.items[1]->symbol) {
            lowering->error = QTT_CORE_MALFORMED_AST;
            return NULL;
        }
        QttCoreNode *node = new_node(lowering, QTT_CORE_HANDLE, ast);
        if (!node) return NULL;
        node->handle.profile_name = ast->list.items[1]->symbol;
        node->handle.capability_id = capability_id(
            lowering->module_id, "handle", node->handle.profile_name,
            ast->line, ast->column);
        node->handle.computation = lower_ast(lowering, ast->list.items[2]);
        if (node->handle.computation)
            node->handle.clause = lower_ast(lowering, ast->list.items[3]);
        if (!node->handle.computation || !node->handle.clause) {
            qtt_core_free(node);
            return NULL;
        }
        return node;
    }
    if (list_head_is(ast, "begin")) {
        if (ast->list.count < 2) {
            lowering->error = QTT_CORE_MALFORMED_AST;
            return NULL;
        }
        return lower_sequence(
            lowering, ast->list.items + 1, ast->list.count - 1, ast);
    }
    if (list_head_is(ast, "if")) {
        if (ast->list.count != 4) {
            lowering->error = QTT_CORE_MALFORMED_AST;
            return NULL;
        }
        QttCoreNode *node = new_node(lowering, QTT_CORE_IF, ast);
        if (!node) return NULL;
        node->conditional.condition = lower_ast(lowering, ast->list.items[1]);
        if (node->conditional.condition)
            node->conditional.then_branch =
                lower_ast(lowering, ast->list.items[2]);
        if (node->conditional.then_branch)
            node->conditional.else_branch =
                lower_ast(lowering, ast->list.items[3]);
        if (!node->conditional.condition || !node->conditional.then_branch ||
            !node->conditional.else_branch) {
            qtt_core_free(node);
            return NULL;
        }
        return node;
    }
    if (list_head_is(ast, "set!")) {
        if (ast->list.count != 3 || !ast->list.items[1] ||
            ast->list.items[1]->type != AST_SYMBOL ||
            !ast->list.items[1]->resolved_binder_id) {
            lowering->error = QTT_CORE_MALFORMED_AST;
            return NULL;
        }
        QttCoreNode *write = new_node(lowering, QTT_CORE_WRITE, ast);
        if (!write) return NULL;
        write->write.place.root = (QttCoreVar){
            .module_id = lowering->module_id,
            .binder_id = ast->list.items[1]->resolved_binder_id,
        };
        bool projected = strchr(ast->list.items[1]->symbol, '.') != NULL;
        const Type *projected_type = NULL;
        if (projected && !lower_structural_place(
                lowering, ast->list.items[1], &write->write.place,
                &projected_type)) {
            qtt_core_free(write);
            lowering->error = QTT_CORE_MALFORMED_AST;
            return NULL;
        }
        if (projected_type) write->type = projected_type;
        write->write.value = lower_ast(lowering, ast->list.items[2]);
        if (!write->write.value) {
            qtt_core_free(write);
            return NULL;
        }
        if (!projected) return write;
        QttCoreNode *borrow = new_node(lowering, QTT_CORE_BORROW, ast);
        if (!borrow) {
            qtt_core_free(write);
            return NULL;
        }
        borrow->type = write->type;
        borrow->borrow.binding = (QttCoreVar){
            .module_id = lowering->module_id,
            .binder_id = lowering->next_synthetic_binder++,
        };
        borrow->borrow.place = write->write.place;
        borrow->borrow.loan_kind = QTT_LOAN_EXCLUSIVE;
        borrow->borrow.body = write;
        return borrow;
    }
    if ((list_head_is(ast, "with") || list_head_is(ast, "let")) &&
        ast->list.count >= 3 && ast->list.items[1] &&
        ast->list.items[1]->type == AST_ARRAY)
        return lower_local_bindings(lowering, ast, ast->list.items[1], 0);

    QttCoreNode *node = new_node(lowering, QTT_CORE_APPLY, ast);
    if (!node) return NULL;
    node->apply.callee = lower_ast(lowering, ast->list.items[0]);
    node->apply.argument_count = ast->list.count - 1;
    if (node->apply.argument_count)
        node->apply.arguments =
            calloc(node->apply.argument_count, sizeof(*node->apply.arguments));
    if (!node->apply.callee ||
        (node->apply.argument_count && !node->apply.arguments)) {
        if (!node->apply.arguments && node->apply.argument_count)
            lowering->error = QTT_CORE_OUT_OF_MEMORY;
        qtt_core_free(node);
        return NULL;
    }
    for (size_t i = 0; i < node->apply.argument_count; i++) {
        node->apply.arguments[i] =
            lower_ast(lowering, ast->list.items[i + 1]);
        if (!node->apply.arguments[i]) {
            qtt_core_free(node);
            return NULL;
        }
    }
    /* Application result types are determined by the recovered callee
     * contract, not by potentially stale legacy AST metadata.  This matters
     * for lexical callable aliases: their arrow type is reconstructed from
     * the type scope even when inference left the call annotated as code. */
    const Type *application_type = node->apply.callee->type;
    for (size_t i = 0;
         application_type && i < node->apply.argument_count; i++) {
        if (application_type->kind != TYPE_ARROW) {
            application_type = NULL;
            break;
        }
        application_type = application_type->arrow_ret;
    }
    if (application_type)
        node->type = application_type;
    return node;
}

static QttCoreNode *lower_ast(Lowering *lowering, const AST *ast) {
    if (!ast || lowering->error != QTT_CORE_OK) return NULL;
    switch (ast->type) {
    case AST_SYMBOL: {
        if (ast->resolved_binder_id && strchr(ast->symbol, '.')) {
            QttPlace place = {0};
            const Type *projected_type = NULL;
            if (!lower_structural_place(
                    lowering, ast, &place, &projected_type)) {
                if (lowering->error == QTT_CORE_OK)
                    lowering->error = QTT_CORE_MALFORMED_AST;
                return NULL;
            }
            QttCoreNode *node = new_node(
                lowering, QTT_CORE_PLACE, ast);
            if (node) {
                node->place = place;
                node->type = projected_type;
            }
            return node;
        }
        QttCoreKind kind =
            ast->resolved_binder_id ? QTT_CORE_VAR : QTT_CORE_GLOBAL;
        QttCoreNode *node = new_node(lowering, kind, ast);
        if (!node) return NULL;
        if (kind == QTT_CORE_VAR) {
            node->var = (QttCoreVar){
                .module_id = lowering->module_id,
                .binder_id = ast->resolved_binder_id,
            };
            const Type *lexical_type = lowering_var_type(
                lowering, node->var);
            if (lexical_type)
                node->type = lexical_type;
        } else {
            node->global.name = ast->symbol;
            if (qtt_compiler_lookup_callable_authority) {
                const char *contract = NULL;
                const char *hm_scheme = NULL;
                bool found = qtt_compiler_lookup_callable_authority(
                    lowering->module_id, ast->symbol,
                    &contract,
                    &node->global.callable_contract_fingerprint,
                    &hm_scheme);
                if (found) {
                    node->global.callable_contract = contract
                        ? core_copy_text(contract) : NULL;
                    node->global.hm_scheme = hm_scheme
                        ? core_copy_text(hm_scheme) : NULL;
                    if ((contract && !node->global.callable_contract) ||
                        (hm_scheme && !node->global.hm_scheme)) {
                        free(node->global.callable_contract);
                        free(node->global.hm_scheme);
                        free(node);
                        lowering->error = QTT_CORE_OUT_OF_MEMORY;
                        return NULL;
                    }
                    node->global.authority_owned = true;
                    const size_t *stages = NULL;
                    const char *const *names = NULL;
                    size_t structured_count = 0;
                    bool structured = qtt_compiler_lookup_effect_judgment &&
                        qtt_compiler_lookup_effect_judgment(
                            lowering->module_id, ast->symbol,
                            &node->global.effect_row_fingerprint,
                            &node->global.effect_constraint_fingerprint,
                            &node->global.effect_constraint_result,
                            &stages, &names, &structured_count);
                    if (structured) {
                        node->global.effect_predicate_stages = structured_count
                            ? malloc(structured_count * sizeof(size_t)) : NULL;
                        node->global.effect_predicate_names = structured_count
                            ? calloc(structured_count, sizeof(char *)) : NULL;
                        bool copied = !structured_count ||
                            (node->global.effect_predicate_stages &&
                             node->global.effect_predicate_names);
                        for (size_t i = 0; copied && i < structured_count; i++) {
                            node->global.effect_predicate_stages[i] = stages[i];
                            node->global.effect_predicate_names[i] =
                                core_copy_text(names[i]);
                            copied = node->global.effect_predicate_names[i] != NULL;
                            if (copied) node->global.effect_predicate_count++;
                        }
                        if (!copied) {
                            qtt_core_free(node);
                            lowering->error = QTT_CORE_OUT_OF_MEMORY;
                            return NULL;
                        }
                    } else if (contract && infer_callable_contract_deserialize &&
                        infer_callable_contract_free) {
                        InferCallableContract decoded = {0};
                        if (!infer_callable_contract_deserialize(
                                &decoded, contract)) {
                            qtt_core_free(node);
                            lowering->error = QTT_CORE_MALFORMED_AST;
                            return NULL;
                        }
                        size_t count = decoded.effect_trait_predicate_count;
                        node->global.effect_predicate_stages = count
                            ? malloc(count * sizeof(size_t)) : NULL;
                        node->global.effect_predicate_names = count
                            ? calloc(count, sizeof(char *)) : NULL;
                        bool copied = !count ||
                            (node->global.effect_predicate_stages &&
                             node->global.effect_predicate_names);
                        for (size_t i = 0; copied && i < count; i++) {
                            node->global.effect_predicate_stages[i] =
                                decoded.effect_trait_predicate_stages[i];
                            node->global.effect_predicate_names[i] =
                                core_copy_text(
                                    decoded.effect_trait_predicate_names[i]);
                            copied =
                                node->global.effect_predicate_names[i] != NULL;
                            if (copied) node->global.effect_predicate_count++;
                        }
                        infer_callable_contract_free(&decoded);
                        if (!copied) {
                            qtt_core_free(node);
                            lowering->error = QTT_CORE_OUT_OF_MEMORY;
                            return NULL;
                        }
                    }
                }
            }
        }
        return node;
    }
    case AST_NUMBER:
    case AST_STRING:
    case AST_PATH:
    case AST_CHAR:
    case AST_KEYWORD:
    case AST_RATIO:
    case AST_TYPE_SET: {
        QttCoreNode *node = new_node(lowering, QTT_CORE_LITERAL, ast);
        if (node) {
            node->literal.source = ast;
            if (!node->type) {
                switch (ast->type) {
                case AST_STRING: node->type = &core_string_type; break;
                case AST_PATH: node->type = &core_path_type; break;
                case AST_CHAR: node->type = &core_char_type; break;
                case AST_KEYWORD: node->type = &core_keyword_type; break;
                case AST_RATIO: node->type = &core_ratio_type; break;
                default: break;
                }
            }
        }
        return node;
    }
    case AST_LIST:
        return lower_list(lowering, ast);
    case AST_LAMBDA: {
        QttCoreNode *node = new_node(lowering, QTT_CORE_LAMBDA, ast);
        if (!node) return NULL;
        node->lambda.param_count = (size_t)ast->lambda.param_count;
        if (node->lambda.param_count) {
            node->lambda.params =
                calloc(node->lambda.param_count, sizeof(*node->lambda.params));
            node->lambda.param_types = calloc(
                node->lambda.param_count,
                sizeof(*node->lambda.param_types));
        }
        if (node->lambda.param_count &&
            (!node->lambda.params || !node->lambda.param_types)) {
            lowering->error = QTT_CORE_OUT_OF_MEMORY;
            qtt_core_free(node);
            return NULL;
        }
        const Type *function_type = ast->inferred_type;
        LoweringTypeScope *type_scopes = calloc(
            node->lambda.param_count, sizeof(*type_scopes));
        if (node->lambda.param_count && !type_scopes) {
            lowering->error = QTT_CORE_OUT_OF_MEMORY;
            qtt_core_free(node);
            return NULL;
        }
        const LoweringTypeScope *outer_types = lowering->types;
        for (size_t i = 0; i < node->lambda.param_count; i++) {
            if (!ast->lambda.params[i].binder_id) {
                lowering->error = QTT_CORE_MALFORMED_AST;
                qtt_core_free(node);
                return NULL;
            }
            node->lambda.params[i] = (QttCoreVar){
                .module_id = lowering->module_id,
                .binder_id = ast->lambda.params[i].binder_id,
            };
            node->lambda.param_types[i] =
                function_type && function_type->kind == TYPE_ARROW
                    ? function_type->arrow_param
                    : ast->lambda.params[i].type_name
                        && type_from_name ? type_from_name(
                              ast->lambda.params[i].type_name)
                        : NULL;
            if (!node->lambda.param_types[i]) {
                lowering->error = QTT_CORE_MALFORMED_AST;
                qtt_core_free(node);
                free(type_scopes);
                lowering->types = outer_types;
                return NULL;
            }
            type_scopes[i] = (LoweringTypeScope){
                .var = node->lambda.params[i],
                .type = node->lambda.param_types[i],
                .parent = lowering->types,
            };
            lowering->types = &type_scopes[i];
            function_type =
                function_type && function_type->kind == TYPE_ARROW
                    ? function_type->arrow_ret : NULL;
        }
        node->lambda.body =
            lower_sequence(lowering, ast->lambda.body_exprs,
                           (size_t)ast->lambda.body_count, ast);
        lowering->types = outer_types;
        free(type_scopes);
        if (!node->lambda.body) {
            qtt_core_free(node);
            return NULL;
        }
        return node;
    }
    default:
        lowering->error = QTT_CORE_UNSUPPORTED_AST;
        return NULL;
    }
}

QttCoreNode *qtt_core_lower(const AST *ast, uint64_t module_id,
                            QttCoreError *error) {
    Lowering lowering = {
        .module_id = module_id,
        .next_synthetic_binder = UINT64_C(0xa000000000000000),
        .error = QTT_CORE_OK,
    };
    QttCoreNode *node = lower_ast(&lowering, ast);
    if (error) *error = lowering.error;
    return node;
}
