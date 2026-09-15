#include "resource.h"
#include "signature_env.h"
#include "callable_env.h"

#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
/*
 * Keep the lexical resource calculus usable as a small standalone proof
 * library. Environment-aware call elaboration becomes available when the
 * signature registry is linked by the compiler.
 */
extern bool qtt_signature_env_resolve(
    const QttSignatureEnv *, uint64_t, const char *,
    QttCallableId *, QttFunctionSignature **) __attribute__((weak));
#endif

typedef struct {
    QttCoreVar alias;
    QttPlace place;
    QttLoanKind kind;
    QttCoreVar parent;
    bool suspended;
    bool suspends_parent;
} ResourceAlias;

typedef struct {
    QttCoreVar *live;
    size_t live_count;
    size_t live_capacity;
    QttCoreVar *consumed;
    size_t consumed_count;
    size_t consumed_capacity;
    ResourceAlias *aliases;
    size_t alias_count;
    size_t alias_capacity;
    /* Fields moved out of otherwise-live aggregate capabilities. */
    QttPlace *evacuated;
    size_t evacuated_count;
    size_t evacuated_capacity;
} ResourceState;

typedef struct {
    QttResourceOp *ops;
    size_t count;
    size_t capacity;
    QttResourceElaborationError error;
    uint64_t next_synthetic_binder;
} Builder;

typedef struct {
    QttCoreVar var;
    QttCoreVar root;
    QttRepresentation representation;
} HeapEntry;

typedef struct {
    HeapEntry *entries;
    size_t count;
    size_t capacity;
    const bool *choices;
    size_t choice_count;
    size_t choice_index;
    QttHeapExecution result;
} HeapState;

typedef struct ResourceBinding {
    QttCoreVar var;
    QttCoreVar capability;
    QttRepresentation representation;
    /* Exact, capture-free code value known at this lexical BinderId. */
    const QttCoreNode *callable;
    bool tracks_capability;
    bool is_loan;
    QttPlace loan_place;
    QttLoanKind loan_kind;
    const struct ResourceBinding *parent;
} ResourceBinding;

static const ResourceBinding *find_covering_loan(
    const ResourceBinding *bindings, QttPlace place) {
    for (const ResourceBinding *binding = bindings;
         binding; binding = binding->parent)
        if (binding->is_loan &&
            qtt_place_contains(binding->loan_place, place))
            return binding;
    return NULL;
}

static bool capture_free_lambda(const QttCoreNode *core) {
    return qtt_callable_capture_free(core);
}

/* The ownership part of a local lambda contract: an argument transfers
 * exactly when the corresponding parameter can become the call result. */
static bool lambda_tail_escapes(
    const QttCoreNode *core, QttCoreVar var) {
    if (!core) return false;
    switch (core->kind) {
    case QTT_CORE_VAR:
        return qtt_core_var_equal(core->var, var);
    case QTT_CORE_LET:
        return lambda_tail_escapes(core->let.body, var);
    case QTT_CORE_BORROW:
        return lambda_tail_escapes(core->borrow.body, var);
    case QTT_CORE_SEQUENCE:
        return core->sequence.count && lambda_tail_escapes(
            core->sequence.items[core->sequence.count - 1], var);
    case QTT_CORE_IF:
        return lambda_tail_escapes(
                   core->conditional.then_branch, var) ||
               lambda_tail_escapes(
                   core->conditional.else_branch, var);
    default:
        return false;
    }
}

QttRepresentation qtt_resource_classify_type(const Type *type) {
    if (!type) return QTT_REP_UNKNOWN;
    switch (type->kind) {
    case TYPE_INT: case TYPE_FLOAT: case TYPE_CHAR: case TYPE_BYTE:
    case TYPE_BOOL: case TYPE_HEX: case TYPE_BIN: case TYPE_OCT:
    case TYPE_KEYWORD: case TYPE_RATIO: case TYPE_F32:
    case TYPE_I8: case TYPE_U8: case TYPE_I16: case TYPE_U16:
    case TYPE_I32: case TYPE_U32: case TYPE_I64: case TYPE_U64:
    case TYPE_I128: case TYPE_U128: case TYPE_INT_ARBITRARY:
    case TYPE_F80: case TYPE_NIL: case TYPE_UNIT: case TYPE_FINITE_SET:
        return QTT_REP_IMMEDIATE;
    case TYPE_ARR:
        return type->arr_is_heap ? QTT_REP_OWNED_HEAP : QTT_REP_INLINE;
    case TYPE_LAYOUT:
        return (type->layout_is_scalar || type->layout_is_inline)
            ? QTT_REP_INLINE : QTT_REP_OWNED_HEAP;
    case TYPE_STRING: case TYPE_SYMBOL: case TYPE_LIST: case TYPE_SET:
    case TYPE_MAP: case TYPE_COLL: case TYPE_PATH: case TYPE_ESCAPE:
    case TYPE_OPTIONAL: case TYPE_APP: case TYPE_FN: case TYPE_ARROW:
        return QTT_REP_OWNED_HEAP;
    case TYPE_PTR:
        return QTT_REP_FOREIGN;
    case TYPE_UNKNOWN: case TYPE_VAR: case TYPE_VARIADIC:
        return QTT_REP_UNKNOWN;
    }
    return QTT_REP_UNKNOWN;
}

static QttResourceOp resource_op(QttResourceOpKind kind, QttCoreVar var) {
    QttResourceOp op = {0};
    op.kind = kind;
    op.var = var;
    return op;
}

QttResourceOp qtt_resource_alloc(QttCoreVar var) {
    return qtt_resource_alloc_typed(var, QTT_REP_OWNED_HEAP);
}

QttResourceOp qtt_resource_alloc_typed(
    QttCoreVar var, QttRepresentation representation) {
    QttResourceOp op = resource_op(QTT_RESOURCE_ALLOC, var);
    op.representation = representation;
    return op;
}

QttResourceOp qtt_resource_borrow(QttCoreVar var) {
    return resource_op(QTT_RESOURCE_BORROW, var);
}

QttResourceOp qtt_resource_alias(
    QttCoreVar alias, QttCoreVar capability) {
    return qtt_resource_alias_place(
        alias, qtt_place_root(capability), QTT_LOAN_SHARED);
}

QttResourceOp qtt_resource_alias_place(
    QttCoreVar alias, QttPlace place, QttLoanKind kind) {
    QttResourceOp op = resource_op(QTT_RESOURCE_ALIAS, alias);
    op.target = place.root;
    op.place = place;
    op.loan_kind = kind;
    return op;
}

QttResourceOp qtt_resource_reborrow_place(
    QttCoreVar alias, QttCoreVar parent_alias,
    QttPlace place, QttLoanKind kind) {
    QttResourceOp op = qtt_resource_alias_place(alias, place, kind);
    op.parent_alias = parent_alias;
    return op;
}

QttResourceOp qtt_resource_alias_exclusive(
    QttCoreVar alias, QttCoreVar capability) {
    return qtt_resource_alias_place(
        alias, qtt_place_root(capability), QTT_LOAN_EXCLUSIVE);
}

QttResourceOp qtt_resource_end_alias(
    QttCoreVar alias, QttCoreVar capability) {
    return qtt_resource_end_alias_place(
        alias, qtt_place_root(capability), QTT_LOAN_SHARED);
}

QttResourceOp qtt_resource_end_alias_place(
    QttCoreVar alias, QttPlace place, QttLoanKind kind) {
    QttResourceOp op = resource_op(QTT_RESOURCE_END_ALIAS, alias);
    op.target = place.root;
    op.place = place;
    op.loan_kind = kind;
    return op;
}

QttResourceOp qtt_resource_end_alias_exclusive(
    QttCoreVar alias, QttCoreVar capability) {
    return qtt_resource_end_alias_place(
        alias, qtt_place_root(capability), QTT_LOAN_EXCLUSIVE);
}

QttResourceOp qtt_resource_move(QttCoreVar var) {
    return resource_op(QTT_RESOURCE_MOVE, var);
}

QttResourceOp qtt_resource_move_place(QttPlace place) {
    QttResourceOp op = resource_op(
        QTT_RESOURCE_MOVE_PLACE, place.root);
    op.target = place.root;
    op.place = place;
    return op;
}

QttResourceOp qtt_resource_move_alias(
    QttCoreVar alias, QttCoreVar capability) {
    return qtt_resource_move_alias_place(
        alias, qtt_place_root(capability));
}

QttResourceOp qtt_resource_move_alias_place(
    QttCoreVar alias, QttPlace place) {
    QttResourceOp op =
        resource_op(QTT_RESOURCE_MOVE_ALIAS, alias);
    op.target = place.root;
    op.place = place;
    return op;
}

QttResourceOp qtt_resource_write_alias(
    QttCoreVar alias, QttCoreVar capability) {
    return qtt_resource_write_alias_place(
        alias, qtt_place_root(capability));
}

QttResourceOp qtt_resource_write_alias_place(
    QttCoreVar alias, QttPlace place) {
    QttResourceOp op =
        resource_op(QTT_RESOURCE_WRITE_ALIAS, alias);
    op.target = place.root;
    op.place = place;
    op.loan_kind = QTT_LOAN_EXCLUSIVE;
    return op;
}

QttResourceOp qtt_resource_drop(QttCoreVar var) {
    return resource_op(QTT_RESOURCE_DROP, var);
}

QttResourceOp qtt_resource_dup(QttCoreVar source, QttCoreVar target) {
    QttResourceOp op = resource_op(QTT_RESOURCE_DUP, source);
    op.target = target;
    return op;
}

QttResourceOp qtt_resource_rehome(QttCoreVar source, QttCoreVar target) {
    QttResourceOp op = resource_op(QTT_RESOURCE_REHOME, source);
    op.target = target;
    return op;
}

QttResourceOp qtt_resource_replace(
    QttCoreVar var, QttRepresentation representation) {
    QttResourceOp op = resource_op(QTT_RESOURCE_REPLACE, var);
    op.representation = representation;
    op.place = qtt_place_root(var);
    return op;
}

QttResourceOp qtt_resource_branch(QttResourceBlock *then_block,
                                  QttResourceBlock *else_block) {
    QttResourceOp op = {0};
    op.kind = QTT_RESOURCE_BRANCH;
    op.branch.then_block = then_block;
    op.branch.else_block = else_block;
    return op;
}

bool qtt_resource_op_effect_label(
    const QttResourceOp *op, char *buffer, size_t capacity) {
    if (!op) return false;
    if (op->kind == QTT_RESOURCE_BORROW)
        return qtt_place_effect_label(
            buffer, capacity, "read", qtt_place_root(op->var));
    if (op->kind == QTT_RESOURCE_ALIAS &&
        op->loan_kind == QTT_LOAN_SHARED)
        return qtt_place_effect_label(
            buffer, capacity, "read", op->place);
    if (op->kind == QTT_RESOURCE_WRITE_ALIAS)
        return qtt_place_effect_label(
            buffer, capacity, "write", op->place);
    if ((op->kind == QTT_RESOURCE_MOVE_ALIAS ||
         op->kind == QTT_RESOURCE_MOVE_PLACE) &&
        op->place.projection_id != 0)
        return qtt_place_effect_label(
            buffer, capacity, "write", op->place);
    return false;
}

static ptrdiff_t state_find(const QttCoreVar *items, size_t count,
                            QttCoreVar var) {
    for (size_t i = 0; i < count; i++)
        if (qtt_core_var_equal(items[i], var)) return (ptrdiff_t)i;
    return -1;
}

static bool state_push(QttCoreVar **items, size_t *count, size_t *capacity,
                       QttCoreVar var) {
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity * 2 : 8;
        QttCoreVar *grown = realloc(*items, next * sizeof(*grown));
        if (!grown) return false;
        *items = grown;
        *capacity = next;
    }
    (*items)[(*count)++] = var;
    return true;
}

static ptrdiff_t alias_find(
    const ResourceAlias *items, size_t count, QttCoreVar alias) {
    for (size_t i = 0; i < count; i++)
        if (qtt_core_var_equal(items[i].alias, alias))
            return (ptrdiff_t)i;
    return -1;
}

static bool alias_push(
    ResourceState *state, QttCoreVar alias, QttPlace place,
    QttLoanKind kind, QttCoreVar parent, bool suspends_parent) {
    if (state->alias_count == state->alias_capacity) {
        size_t next = state->alias_capacity
            ? state->alias_capacity * 2 : 8;
        ResourceAlias *grown =
            realloc(state->aliases, next * sizeof(*grown));
        if (!grown) return false;
        state->aliases = grown;
        state->alias_capacity = next;
    }
    state->aliases[state->alias_count++] =
        (ResourceAlias){
            .alias = alias, .place = place, .kind = kind,
            .parent = parent, .suspends_parent = suspends_parent};
    return true;
}

static bool alias_is_ancestor(
    const ResourceState *state, QttCoreVar candidate,
    QttCoreVar alias) {
    while (alias.module_id || alias.binder_id) {
        ptrdiff_t found = alias_find(
            state->aliases, state->alias_count, alias);
        if (found < 0) return false;
        alias = state->aliases[(size_t)found].parent;
        if (qtt_core_var_equal(alias, candidate)) return true;
    }
    return false;
}

static bool place_has_loan(
    const ResourceState *state, QttPlace place) {
    for (size_t i = 0; i < state->alias_count; i++)
        if (qtt_place_overlaps(state->aliases[i].place, place))
            return true;
    return false;
}

static bool place_has_exclusive_loan(
    const ResourceState *state, QttPlace place) {
    for (size_t i = 0; i < state->alias_count; i++)
        if (state->aliases[i].kind == QTT_LOAN_EXCLUSIVE &&
            qtt_place_overlaps(state->aliases[i].place, place))
            return true;
    return false;
}

static bool place_is_evacuated(
    const ResourceState *state, QttPlace place) {
    for (size_t i = 0; i < state->evacuated_count; i++)
        if (qtt_place_overlaps(state->evacuated[i], place))
            return true;
    return false;
}

static bool root_is_partial(
    const ResourceState *state, QttCoreVar root) {
    for (size_t i = 0; i < state->evacuated_count; i++)
        if (qtt_core_var_equal(state->evacuated[i].root, root))
            return true;
    return false;
}

static bool evacuate_place(ResourceState *state, QttPlace place) {
    if (state->evacuated_count == state->evacuated_capacity) {
        size_t next = state->evacuated_capacity
            ? state->evacuated_capacity * 2 : 8;
        QttPlace *grown = realloc(
            state->evacuated, next * sizeof(*grown));
        if (!grown) return false;
        state->evacuated = grown;
        state->evacuated_capacity = next;
    }
    state->evacuated[state->evacuated_count++] = place;
    return true;
}

static void clear_evacuated_root(
    ResourceState *state, QttCoreVar root) {
    size_t output = 0;
    for (size_t i = 0; i < state->evacuated_count; i++)
        if (!qtt_core_var_equal(state->evacuated[i].root, root))
            state->evacuated[output++] = state->evacuated[i];
    state->evacuated_count = output;
}

static void state_free(ResourceState *state) {
    free(state->live);
    free(state->consumed);
    free(state->aliases);
    free(state->evacuated);
    memset(state, 0, sizeof(*state));
}

static bool state_clone(ResourceState *destination,
                        const ResourceState *source) {
    memset(destination, 0, sizeof(*destination));
    if (source->live_count) {
        destination->live = malloc(source->live_count *
                                   sizeof(*destination->live));
        if (!destination->live) return false;
        memcpy(destination->live, source->live,
               source->live_count * sizeof(*destination->live));
        destination->live_count = source->live_count;
        destination->live_capacity = source->live_count;
    }
    if (source->consumed_count) {
        destination->consumed = malloc(source->consumed_count *
                                       sizeof(*destination->consumed));
        if (!destination->consumed) {
            state_free(destination);
            return false;
        }
        memcpy(destination->consumed, source->consumed,
               source->consumed_count * sizeof(*destination->consumed));
        destination->consumed_count = source->consumed_count;
        destination->consumed_capacity = source->consumed_count;
    }
    if (source->alias_count) {
        destination->aliases = malloc(
            source->alias_count * sizeof(*destination->aliases));
        if (!destination->aliases) {
            state_free(destination);
            return false;
        }
        memcpy(
            destination->aliases, source->aliases,
            source->alias_count * sizeof(*destination->aliases));
        destination->alias_count = source->alias_count;
        destination->alias_capacity = source->alias_count;
    }
    if (source->evacuated_count) {
        destination->evacuated = malloc(
            source->evacuated_count * sizeof(*destination->evacuated));
        if (!destination->evacuated) {
            state_free(destination);
            return false;
        }
        memcpy(destination->evacuated, source->evacuated,
               source->evacuated_count * sizeof(*destination->evacuated));
        destination->evacuated_count = source->evacuated_count;
        destination->evacuated_capacity = source->evacuated_count;
    }
    return true;
}

static bool state_set_equal(const QttCoreVar *left, size_t left_count,
                            const QttCoreVar *right, size_t right_count) {
    if (left_count != right_count) return false;
    for (size_t i = 0; i < left_count; i++)
        if (state_find(right, right_count, left[i]) < 0) return false;
    return true;
}

static bool state_equal(const ResourceState *left,
                        const ResourceState *right) {
    if (!state_set_equal(
            left->live, left->live_count,
            right->live, right->live_count))
        return false;
    if (!state_set_equal(
            left->consumed, left->consumed_count,
            right->consumed, right->consumed_count) ||
        left->alias_count != right->alias_count ||
        left->evacuated_count != right->evacuated_count)
        return false;
    for (size_t i = 0; i < left->evacuated_count; i++) {
        bool found = false;
        for (size_t j = 0; j < right->evacuated_count; j++)
            if (qtt_place_equal(
                    left->evacuated[i], right->evacuated[j])) {
                found = true;
                break;
            }
        if (!found) return false;
    }
    for (size_t i = 0; i < left->alias_count; i++) {
        ptrdiff_t found =
            alias_find(
                right->aliases, right->alias_count,
                left->aliases[i].alias);
        if (found < 0 ||
            !qtt_place_equal(
                left->aliases[i].place,
                right->aliases[(size_t)found].place) ||
            left->aliases[i].kind !=
                right->aliases[(size_t)found].kind)
            return false;
        if (!qtt_core_var_equal(
                left->aliases[i].parent,
                right->aliases[(size_t)found].parent) ||
            left->aliases[i].suspended !=
                right->aliases[(size_t)found].suspended ||
            left->aliases[i].suspends_parent !=
                right->aliases[(size_t)found].suspends_parent)
            return false;
    }
    return true;
}

static QttResourceVerification verification(QttResourceError error,
                                            size_t index,
                                            QttCoreVar var,
                                            size_t live_count) {
    return (QttResourceVerification){
        .error = error,
        .operation_index = index,
        .var = var,
        .live_count = live_count,
    };
}

static QttResourceVerification verify_block(const QttResourceBlock *block,
                                            ResourceState *state) {
    if (!block)
        return verification(QTT_RESOURCE_UNKNOWN_VAR, 0,
                            (QttCoreVar){0}, state->live_count);
    for (size_t i = 0; i < block->count; i++) {
        const QttResourceOp *op = &block->ops[i];
        ptrdiff_t live = state_find(state->live, state->live_count, op->var);
        ptrdiff_t consumed =
            state_find(state->consumed, state->consumed_count, op->var);
        switch (op->kind) {
        case QTT_RESOURCE_ALLOC:
            if (op->representation != QTT_REP_OWNED_HEAP &&
                op->representation != QTT_REP_FOREIGN)
                return verification(
                    QTT_RESOURCE_INVALID_CAPABILITY, i, op->var,
                    state->live_count);
            if (live >= 0 || consumed >= 0)
                return verification(QTT_RESOURCE_DUPLICATE_ALLOC, i, op->var,
                                    state->live_count);
            if (!state_push(&state->live, &state->live_count,
                            &state->live_capacity, op->var))
                return verification(QTT_RESOURCE_OUT_OF_MEMORY, i, op->var,
                                    state->live_count);
            break;
        case QTT_RESOURCE_BORROW:
            if (live < 0)
                return verification(
                    consumed >= 0 ? QTT_RESOURCE_USE_AFTER_CONSUME
                                  : QTT_RESOURCE_UNKNOWN_VAR,
                    i, op->var, state->live_count);
            if (place_has_exclusive_loan(
                    state, qtt_place_root(op->var)))
                return verification(
                    QTT_RESOURCE_ACCESS_CONFLICT, i, op->var,
                    state->live_count);
            if (root_is_partial(state, op->var))
                return verification(
                    QTT_RESOURCE_USE_AFTER_CONSUME, i, op->var,
                    state->live_count);
            break;
        case QTT_RESOURCE_ALIAS: {
            if (!qtt_core_var_equal(op->target, op->place.root))
                return verification(
                    QTT_RESOURCE_INVALID_CAPABILITY, i, op->var,
                    state->live_count);
            ptrdiff_t target_live =
                state_find(state->live, state->live_count, op->target);
            ptrdiff_t target_consumed =
                state_find(
                    state->consumed, state->consumed_count, op->target);
            if (target_live < 0)
                return verification(
                    target_consumed >= 0
                        ? QTT_RESOURCE_USE_AFTER_CONSUME
                        : QTT_RESOURCE_UNKNOWN_VAR,
                    i, op->target, state->live_count);
            if (qtt_core_var_equal(op->var, op->target))
                return verification(
                    QTT_RESOURCE_INVALID_CAPABILITY, i, op->var,
                    state->live_count);
            if (alias_find(
                    state->aliases, state->alias_count,
                    op->var) >= 0)
                return verification(
                    QTT_RESOURCE_DUPLICATE_ALIAS, i, op->var,
                    state->live_count);
            if (place_is_evacuated(state, op->place))
                return verification(
                    QTT_RESOURCE_USE_AFTER_CONSUME, i, op->target,
                    state->live_count);
            ptrdiff_t parent =
                (op->parent_alias.module_id || op->parent_alias.binder_id)
                    ? alias_find(
                        state->aliases, state->alias_count,
                        op->parent_alias)
                    : -1;
            bool has_parent =
                op->parent_alias.module_id || op->parent_alias.binder_id;
            bool invalid_parent = has_parent &&
                (parent < 0 || state->aliases[(size_t)parent].suspended ||
                 !qtt_place_contains(
                     state->aliases[(size_t)parent].place, op->place) ||
                 (op->loan_kind == QTT_LOAN_EXCLUSIVE &&
                  state->aliases[(size_t)parent].kind !=
                      QTT_LOAN_EXCLUSIVE));
            bool conflict = false;
            for (size_t loan = 0; loan < state->alias_count; loan++) {
                if (has_parent &&
                    (qtt_core_var_equal(
                         state->aliases[loan].alias, op->parent_alias) ||
                     alias_is_ancestor(
                         state, state->aliases[loan].alias,
                         op->parent_alias)))
                    continue;
                if (qtt_place_overlaps(
                        state->aliases[loan].place, op->place) &&
                    (op->loan_kind == QTT_LOAN_EXCLUSIVE ||
                     state->aliases[loan].kind == QTT_LOAN_EXCLUSIVE))
                    conflict = true;
            }
            if (invalid_parent || conflict)
                return verification(
                    QTT_RESOURCE_LOAN_CONFLICT, i, op->target,
                    state->live_count);
            bool suspends_parent = has_parent && parent >= 0 &&
                state->aliases[(size_t)parent].kind == QTT_LOAN_EXCLUSIVE;
            if (!alias_push(
                    state, op->var, op->place,
                    op->loan_kind, op->parent_alias,
                    suspends_parent))
                return verification(
                    QTT_RESOURCE_OUT_OF_MEMORY, i, op->var,
                    state->live_count);
            if (suspends_parent)
                state->aliases[(size_t)parent].suspended = true;
            break;
        }
        case QTT_RESOURCE_END_ALIAS: {
            ptrdiff_t found =
                alias_find(
                    state->aliases, state->alias_count,
                    op->var);
            bool has_child = false;
            for (size_t child = 0; child < state->alias_count; child++)
                if (qtt_core_var_equal(
                        state->aliases[child].parent, op->var))
                    has_child = true;
            if (found < 0 || has_child)
                return verification(
                    found < 0 ? QTT_RESOURCE_UNKNOWN_ALIAS
                              : QTT_RESOURCE_LOAN_CONFLICT,
                    i, op->var,
                    state->live_count);
            if (!qtt_core_var_equal(op->target, op->place.root) ||
                !qtt_place_equal(
                    state->aliases[(size_t)found].place,
                    op->place))
                return verification(
                    QTT_RESOURCE_ALIAS_TARGET_MISMATCH, i, op->var,
                    state->live_count);
            if (state->aliases[(size_t)found].kind !=
                op->loan_kind)
                return verification(
                    QTT_RESOURCE_LOAN_KIND_MISMATCH, i, op->var,
                    state->live_count);
            QttCoreVar parent_alias =
                state->aliases[(size_t)found].parent;
            bool restore_parent =
                state->aliases[(size_t)found].suspends_parent;
            state->aliases[(size_t)found] =
                state->aliases[--state->alias_count];
            if (restore_parent) {
                ptrdiff_t parent = alias_find(
                    state->aliases, state->alias_count, parent_alias);
                if (parent < 0)
                    return verification(
                        QTT_RESOURCE_LOAN_CONFLICT, i, op->var,
                        state->live_count);
                state->aliases[(size_t)parent].suspended = false;
            }
            break;
        }
        case QTT_RESOURCE_DUP: {
            ptrdiff_t target_live =
                state_find(state->live, state->live_count, op->target);
            ptrdiff_t target_consumed =
                state_find(state->consumed, state->consumed_count, op->target);
            if (live < 0)
                return verification(
                    consumed >= 0 ? QTT_RESOURCE_USE_AFTER_CONSUME
                                  : QTT_RESOURCE_UNKNOWN_VAR,
                    i, op->var, state->live_count);
            if (place_has_loan(
                    state, qtt_place_root(op->var)))
                return verification(
                    QTT_RESOURCE_ACCESS_CONFLICT, i, op->var,
                    state->live_count);
            if (root_is_partial(state, op->var))
                return verification(
                    QTT_RESOURCE_INVALID_CAPABILITY, i, op->var,
                    state->live_count);
            if (target_live >= 0 || target_consumed >= 0)
                return verification(QTT_RESOURCE_DUPLICATE_ALLOC, i,
                                    op->target, state->live_count);
            if (!state_push(&state->live, &state->live_count,
                            &state->live_capacity, op->target))
                return verification(QTT_RESOURCE_OUT_OF_MEMORY, i,
                                    op->target, state->live_count);
            break;
        }
        case QTT_RESOURCE_REHOME: {
            ptrdiff_t target_live =
                state_find(state->live, state->live_count, op->target);
            ptrdiff_t target_consumed =
                state_find(state->consumed, state->consumed_count, op->target);
            if (live < 0)
                return verification(
                    consumed >= 0 ? QTT_RESOURCE_DOUBLE_CONSUME
                                  : QTT_RESOURCE_UNKNOWN_VAR,
                    i, op->var, state->live_count);
            if (place_has_loan(
                    state, qtt_place_root(op->var)))
                return verification(
                    QTT_RESOURCE_ACCESS_CONFLICT, i, op->var,
                    state->live_count);
            if (root_is_partial(state, op->var))
                return verification(
                    QTT_RESOURCE_INVALID_CAPABILITY, i, op->var,
                    state->live_count);
            if (target_live >= 0 || target_consumed >= 0)
                return verification(QTT_RESOURCE_DUPLICATE_ALLOC, i,
                                    op->target, state->live_count);
            state->live[(size_t)live] = op->target;
            if (!state_push(&state->consumed, &state->consumed_count,
                            &state->consumed_capacity, op->var))
                return verification(QTT_RESOURCE_OUT_OF_MEMORY, i, op->var,
                                    state->live_count);
            break;
        }
        case QTT_RESOURCE_REPLACE:
            if (live < 0)
                return verification(
                    consumed >= 0 ? QTT_RESOURCE_USE_AFTER_CONSUME
                                  : QTT_RESOURCE_UNKNOWN_VAR,
                    i, op->var, state->live_count);
            if (op->representation != QTT_REP_OWNED_HEAP ||
                !qtt_place_equal(op->place, qtt_place_root(op->var)))
                return verification(
                    QTT_RESOURCE_INVALID_CAPABILITY, i, op->var,
                    state->live_count);
            if (place_has_loan(state, qtt_place_root(op->var)))
                return verification(
                    QTT_RESOURCE_ACCESS_CONFLICT, i, op->var,
                    state->live_count);
            if (root_is_partial(state, op->var))
                return verification(
                    QTT_RESOURCE_INVALID_CAPABILITY, i, op->var,
                    state->live_count);
            break;
        case QTT_RESOURCE_MOVE_ALIAS: {
            ptrdiff_t alias =
                alias_find(
                    state->aliases, state->alias_count,
                    op->var);
            ptrdiff_t target_live =
                state_find(
                    state->live, state->live_count,
                    op->target);
            ptrdiff_t target_consumed =
                state_find(
                    state->consumed, state->consumed_count,
                    op->target);
            if (alias < 0)
                return verification(
                    QTT_RESOURCE_UNKNOWN_ALIAS, i, op->var,
                    state->live_count);
            if (!qtt_core_var_equal(op->target, op->place.root) ||
                !qtt_place_equal(
                    state->aliases[(size_t)alias].place,
                    op->place))
                return verification(
                    QTT_RESOURCE_ALIAS_TARGET_MISMATCH, i,
                    op->var, state->live_count);
            ResourceAlias *loan = &state->aliases[(size_t)alias];
            if (loan->suspended ||
                (op->place.projection_id != 0 &&
                 loan->kind != QTT_LOAN_EXCLUSIVE))
                return verification(
                    QTT_RESOURCE_ACCESS_CONFLICT, i,
                    op->var, state->live_count);
            if (target_live < 0)
                return verification(
                    target_consumed >= 0
                        ? QTT_RESOURCE_DOUBLE_CONSUME
                        : QTT_RESOURCE_UNKNOWN_VAR,
                    i, op->target, state->live_count);
            if (op->place.projection_id != 0) {
                if (place_is_evacuated(state, op->place))
                    return verification(
                        QTT_RESOURCE_USE_AFTER_CONSUME, i,
                        op->target, state->live_count);
                if (!evacuate_place(state, op->place))
                    return verification(
                        QTT_RESOURCE_OUT_OF_MEMORY, i,
                        op->target, state->live_count);
                break;
            }
            if (root_is_partial(state, op->target))
                return verification(
                    QTT_RESOURCE_INVALID_CAPABILITY, i,
                    op->target, state->live_count);
            state->live[(size_t)target_live] =
                state->live[--state->live_count];
            if (!state_push(
                    &state->consumed, &state->consumed_count,
                    &state->consumed_capacity, op->target))
                return verification(
                    QTT_RESOURCE_OUT_OF_MEMORY, i, op->target,
                    state->live_count);
            break;
        }
        case QTT_RESOURCE_WRITE_ALIAS: {
            ptrdiff_t alias =
                alias_find(
                    state->aliases, state->alias_count,
                    op->var);
            if (alias < 0)
                return verification(
                    QTT_RESOURCE_UNKNOWN_ALIAS, i, op->var,
                    state->live_count);
            ResourceAlias *loan =
                &state->aliases[(size_t)alias];
            if (!qtt_core_var_equal(op->target, op->place.root) ||
                !qtt_place_equal(loan->place, op->place))
                return verification(
                    QTT_RESOURCE_ALIAS_TARGET_MISMATCH, i,
                    op->var, state->live_count);
            if (loan->suspended || loan->kind != QTT_LOAN_EXCLUSIVE)
                return verification(
                    QTT_RESOURCE_ACCESS_CONFLICT, i,
                    op->var, state->live_count);
            if (place_is_evacuated(state, op->place))
                return verification(
                    QTT_RESOURCE_USE_AFTER_CONSUME, i,
                    op->target, state->live_count);
            if (state_find(
                    state->live, state->live_count,
                    op->target) < 0)
                return verification(
                    QTT_RESOURCE_USE_AFTER_CONSUME, i,
                    op->target, state->live_count);
            break;
        }
        case QTT_RESOURCE_MOVE:
        case QTT_RESOURCE_DROP:
            if (live < 0)
                return verification(
                    consumed >= 0 ? QTT_RESOURCE_DOUBLE_CONSUME
                                  : QTT_RESOURCE_UNKNOWN_VAR,
                    i, op->var, state->live_count);
            if (place_has_loan(
                    state, qtt_place_root(op->var)))
                return verification(
                    QTT_RESOURCE_ACCESS_CONFLICT, i, op->var,
                    state->live_count);
            if (op->kind == QTT_RESOURCE_MOVE &&
                root_is_partial(state, op->var))
                return verification(
                    QTT_RESOURCE_INVALID_CAPABILITY, i, op->var,
                    state->live_count);
            state->live[(size_t)live] = state->live[--state->live_count];
            if (!state_push(&state->consumed, &state->consumed_count,
                            &state->consumed_capacity, op->var))
                return verification(QTT_RESOURCE_OUT_OF_MEMORY, i, op->var,
                                    state->live_count);
            clear_evacuated_root(state, op->var);
            break;
        case QTT_RESOURCE_MOVE_PLACE:
            if (live < 0)
                return verification(
                    consumed >= 0 ? QTT_RESOURCE_USE_AFTER_CONSUME
                                  : QTT_RESOURCE_UNKNOWN_VAR,
                    i, op->var, state->live_count);
            if (!qtt_core_var_equal(op->var, op->place.root) ||
                op->place.projection_depth == 0)
                return verification(
                    QTT_RESOURCE_INVALID_CAPABILITY, i, op->var,
                    state->live_count);
            if (place_has_loan(state, op->place))
                return verification(
                    QTT_RESOURCE_ACCESS_CONFLICT, i, op->var,
                    state->live_count);
            if (place_is_evacuated(state, op->place))
                return verification(
                    QTT_RESOURCE_USE_AFTER_CONSUME, i, op->var,
                    state->live_count);
            if (!evacuate_place(state, op->place))
                return verification(
                    QTT_RESOURCE_OUT_OF_MEMORY, i, op->var,
                    state->live_count);
            break;
        case QTT_RESOURCE_BRANCH: {
            ResourceState then_state = {0};
            ResourceState else_state = {0};
            if (!state_clone(&then_state, state) ||
                !state_clone(&else_state, state)) {
                state_free(&then_state);
                state_free(&else_state);
                return verification(QTT_RESOURCE_OUT_OF_MEMORY, i, op->var,
                                    state->live_count);
            }
            QttResourceVerification then_result =
                verify_block(op->branch.then_block, &then_state);
            QttResourceVerification else_result =
                verify_block(op->branch.else_block, &else_state);
            if (then_result.error != QTT_RESOURCE_VALID) {
                state_free(&then_state);
                state_free(&else_state);
                return then_result;
            }
            if (else_result.error != QTT_RESOURCE_VALID) {
                state_free(&then_state);
                state_free(&else_state);
                return else_result;
            }
            if (!state_equal(&then_state, &else_state)) {
                state_free(&then_state);
                state_free(&else_state);
                return verification(QTT_RESOURCE_BRANCH_MISMATCH, i, op->var,
                                    state->live_count);
            }
            state_free(state);
            *state = then_state;
            state_free(&else_state);
            break;
        }
        }
    }
    return verification(QTT_RESOURCE_VALID, block->count,
                        (QttCoreVar){0}, state->live_count);
}

QttResourceVerification qtt_resource_verify(
    const QttResourceBlock *block,
    const QttCoreVar *initially_owned,
    size_t initially_owned_count) {
    ResourceState state = {0};
    for (size_t i = 0; i < initially_owned_count; i++) {
        if (!state_push(&state.live, &state.live_count, &state.live_capacity,
                        initially_owned[i])) {
            state_free(&state);
            return verification(QTT_RESOURCE_OUT_OF_MEMORY, 0,
                                initially_owned[i], 0);
        }
    }
    QttResourceVerification result = verify_block(block, &state);
    if (result.error == QTT_RESOURCE_VALID && state.live_count)
        result = verification(QTT_RESOURCE_LEAK,
                              block ? block->count : 0,
                              state.live[0], state.live_count);
    if (result.error == QTT_RESOURCE_VALID && state.alias_count)
        result = verification(
            QTT_RESOURCE_UNKNOWN_ALIAS,
            block ? block->count : 0,
            state.aliases[0].alias, state.live_count);
    state_free(&state);
    return result;
}

static ptrdiff_t heap_find(const HeapState *heap, QttCoreVar var) {
    for (size_t i = 0; i < heap->count; i++)
        if (qtt_core_var_equal(heap->entries[i].var, var))
            return (ptrdiff_t)i;
    return -1;
}

static bool heap_add(HeapState *heap, QttCoreVar var, QttCoreVar root,
                     QttRepresentation representation) {
    if (heap->count == heap->capacity) {
        size_t next = heap->capacity ? heap->capacity * 2 : 8;
        HeapEntry *grown = realloc(heap->entries, next * sizeof(*grown));
        if (!grown) return false;
        heap->entries = grown;
        heap->capacity = next;
    }
    heap->entries[heap->count++] = (HeapEntry){var, root, representation};
    return true;
}

static bool heap_has_root(const HeapState *heap, QttCoreVar root) {
    for (size_t i = 0; i < heap->count; i++)
        if (qtt_core_var_equal(heap->entries[i].root, root)) return true;
    return false;
}

static void execute_block(const QttResourceBlock *block, HeapState *heap) {
    for (size_t i = 0;
         heap->result.error == QTT_RESOURCE_VALID && i < block->count;
         i++) {
        const QttResourceOp *op = &block->ops[i];
        ptrdiff_t found = heap_find(heap, op->var);
        switch (op->kind) {
        case QTT_RESOURCE_ALLOC:
            if (!heap_add(heap, op->var, op->var, op->representation)) {
                heap->result.error = QTT_RESOURCE_OUT_OF_MEMORY;
                return;
            }
            if (op->representation == QTT_REP_OWNED_HEAP ||
                op->representation == QTT_REP_FOREIGN) {
                heap->result.allocated++;
                heap->result.live_owned++;
                if (heap->result.live_owned > heap->result.peak_live_owned)
                    heap->result.peak_live_owned = heap->result.live_owned;
            }
            break;
        case QTT_RESOURCE_BORROW:
            heap->result.borrows++;
            break;
        case QTT_RESOURCE_ALIAS:
            /* Proof operation: aliases do not create physical ownership. */
            break;
        case QTT_RESOURCE_END_ALIAS:
            break;
        case QTT_RESOURCE_MOVE_ALIAS:
            found = heap_find(heap, op->target);
            if (found < 0) {
                heap->result.error = QTT_RESOURCE_UNKNOWN_VAR;
                return;
            }
            if (op->place.projection_id != 0) {
                heap->result.projected_moves++;
                break;
            }
            {
                HeapEntry entry = heap->entries[(size_t)found];
                heap->entries[(size_t)found] =
                    heap->entries[--heap->count];
                if (entry.representation == QTT_REP_OWNED_HEAP) {
                    heap->result.moved_out++;
                    if (!heap_has_root(heap, entry.root))
                        heap->result.live_owned--;
                }
            }
            break;
        case QTT_RESOURCE_MOVE_PLACE:
            if (!heap_has_root(heap, op->var)) {
                heap->result.error = QTT_RESOURCE_USE_AFTER_CONSUME;
                break;
            }
            heap->result.projected_moves++;
            break;
        case QTT_RESOURCE_WRITE_ALIAS:
            break;
        case QTT_RESOURCE_DUP: {
            HeapEntry source = heap->entries[(size_t)found];
            if (!heap_add(heap, op->target, source.root,
                          source.representation)) {
                heap->result.error = QTT_RESOURCE_OUT_OF_MEMORY;
                return;
            }
            if (source.representation == QTT_REP_OWNED_HEAP ||
                source.representation == QTT_REP_FOREIGN)
                heap->result.retains++;
            break;
        }
        case QTT_RESOURCE_REHOME:
            heap->entries[(size_t)found].var = op->target;
            heap->result.rehomes++;
            break;
        case QTT_RESOURCE_REPLACE:
            if (found < 0 ||
                op->representation != QTT_REP_OWNED_HEAP) {
                heap->result.error = QTT_RESOURCE_UNKNOWN_VAR;
                return;
            }
            /*
             * Replacement has peak one: the old payload is destroyed before
             * the fresh payload is installed under the same capability.
             */
            heap->result.dropped++;
            heap->result.allocated++;
            heap->entries[(size_t)found].root = op->var;
            heap->entries[(size_t)found].representation =
                op->representation;
            break;
        case QTT_RESOURCE_MOVE:
        case QTT_RESOURCE_DROP: {
            HeapEntry entry = heap->entries[(size_t)found];
            heap->entries[(size_t)found] = heap->entries[--heap->count];
            if (entry.representation == QTT_REP_OWNED_HEAP ||
                entry.representation == QTT_REP_FOREIGN) {
                bool last_local = !heap_has_root(heap, entry.root);
                if (op->kind == QTT_RESOURCE_MOVE) {
                    heap->result.moved_out++;
                    if (last_local) heap->result.live_owned--;
                } else if (entry.representation == QTT_REP_FOREIGN) {
                    if (last_local) heap->result.live_owned--;
                    heap->result.releases++;
                    heap->result.dropped++;
                } else if (last_local) {
                    heap->result.live_owned--;
                    heap->result.dropped++;
                } else {
                    heap->result.releases++;
                }
            }
            break;
        }
        case QTT_RESOURCE_BRANCH:
            if (heap->choice_index >= heap->choice_count) {
                heap->result.error = QTT_RESOURCE_BRANCH_CHOICE_MISSING;
                return;
            }
            execute_block(heap->choices[heap->choice_index++]
                              ? op->branch.then_block
                              : op->branch.else_block,
                          heap);
            break;
        }
    }
}

QttHeapExecution qtt_resource_execute(
    const QttResourceBlock *block,
    const bool *branch_choices,
    size_t branch_choice_count) {
    QttResourceVerification verified =
        qtt_resource_verify(block, NULL, 0);
    if (verified.error != QTT_RESOURCE_VALID)
        return (QttHeapExecution){.error = verified.error};
    HeapState heap = {
        .choices = branch_choices,
        .choice_count = branch_choice_count,
        .result = {.error = QTT_RESOURCE_VALID},
    };
    execute_block(block, &heap);
    free(heap.entries);
    return heap.result;
}

static bool builder_push(Builder *builder, QttResourceOp op) {
    if (builder->count == builder->capacity) {
        size_t next = builder->capacity ? builder->capacity * 2 : 8;
        QttResourceOp *grown =
            realloc(builder->ops, next * sizeof(*grown));
        if (!grown) {
            builder->error = QTT_RESOURCE_ELABORATE_OUT_OF_MEMORY;
            return false;
        }
        builder->ops = grown;
        builder->capacity = next;
    }
    builder->ops[builder->count++] = op;
    return true;
}

static bool block_consumes(const QttResourceBlock *block, QttCoreVar var);

static bool op_consumes(const QttResourceOp *op, QttCoreVar var) {
    if (op->kind == QTT_RESOURCE_MOVE_ALIAS &&
        qtt_core_var_equal(op->target, var))
        return true;
    if ((op->kind == QTT_RESOURCE_MOVE ||
         op->kind == QTT_RESOURCE_REHOME ||
         op->kind == QTT_RESOURCE_DROP) &&
        qtt_core_var_equal(op->var, var))
        return true;
    return op->kind == QTT_RESOURCE_BRANCH &&
           block_consumes(op->branch.then_block, var) &&
           block_consumes(op->branch.else_block, var);
}

static bool block_consumes(const QttResourceBlock *block, QttCoreVar var) {
    if (!block) return false;
    for (size_t i = 0; i < block->count; i++)
        if (op_consumes(&block->ops[i], var)) return true;
    return false;
}

static bool builder_has_consumed(const Builder *builder, QttCoreVar var,
                                 size_t start) {
    for (size_t i = start; i < builder->count; i++)
        if (op_consumes(&builder->ops[i], var)) return true;
    return false;
}

static QttResourceBlock *builder_finish(Builder *builder) {
    QttResourceBlock *block = malloc(sizeof(*block));
    if (!block) {
        builder->error = QTT_RESOURCE_ELABORATE_OUT_OF_MEMORY;
        return NULL;
    }
    block->ops = builder->ops;
    block->count = builder->count;
    builder->ops = NULL;
    builder->count = 0;
    builder->capacity = 0;
    return block;
}

static const ResourceBinding *find_resource_binding(
    const ResourceBinding *bindings, QttCoreVar var,
    QttRepresentation *representation) {
    for (; bindings; bindings = bindings->parent)
        if (qtt_core_var_equal(bindings->var, var)) {
            if (representation)
                *representation = bindings->representation;
            return bindings;
        }
    return NULL;
}

static const QttCoreNode *known_callable_value(
    const QttCoreNode *value, const ResourceBinding *bindings) {
    if (capture_free_lambda(value)) return value;
    if (!value || value->kind != QTT_CORE_VAR) return NULL;
    const ResourceBinding *source = find_resource_binding(
        bindings, value->var, NULL);
    return source ? source->callable : NULL;
}

static bool representation_has_capability(
    QttRepresentation representation) {
    return representation == QTT_REP_OWNED_HEAP;
}

static QttRepresentation expression_representation(
    const QttCoreNode *core, const QttSignatureEnv *signatures,
    uint64_t module_id) {
    QttRepresentation representation =
        core && core->type
            ? qtt_resource_classify_type(core->type)
            : QTT_REP_UNKNOWN;
    if (representation != QTT_REP_UNKNOWN ||
        !core || core->kind != QTT_CORE_APPLY ||
        !core->apply.callee ||
        core->apply.callee->kind != QTT_CORE_GLOBAL ||
        !signatures || !module_id)
        return representation;
    QttFunctionSignature *signature = NULL;
    return qtt_signature_env_resolve(
               signatures, module_id,
               core->apply.callee->global.name,
               NULL, &signature) &&
           signature
        ? signature->result.representation
        : QTT_REP_UNKNOWN;
}

/*
 * Physical representation and authority are deliberately separate.
 * A variable of heap representation aliases an existing capability; it does
 * not allocate another one.  Fresh calls and heap literals introduce roots.
 */
static bool expression_allocates_capability(
    const QttCoreNode *core, const QttSignatureEnv *signatures,
    uint64_t module_id) {
    if (!core ||
        !representation_has_capability(
            expression_representation(core, signatures, module_id)))
        return false;
    if (core->kind == QTT_CORE_LITERAL ||
        core->kind == QTT_CORE_QUOTE ||
        core->kind == QTT_CORE_LAMBDA)
        return true;
    if (core->kind != QTT_CORE_APPLY ||
        !core->apply.callee ||
        core->apply.callee->kind != QTT_CORE_GLOBAL ||
        !signatures || !module_id)
        return false;
    QttFunctionSignature *signature = NULL;
    return qtt_signature_env_resolve(
               signatures, module_id,
               core->apply.callee->global.name, NULL, &signature) &&
           signature &&
           signature->result.mode == QTT_RESULT_OWNED &&
           signature->result.origin == QTT_RESULT_ORIGIN_FRESH;
}

static bool elaborate(Builder *builder, const QttCoreNode *core,
                      bool tail_position,
                      const ResourceBinding *bindings,
                      const QttSignatureEnv *signatures,
                      uint64_t module_id) {
    if (!core || builder->error != QTT_RESOURCE_ELABORATE_OK) return false;
    switch (core->kind) {
    case QTT_CORE_LITERAL:
    case QTT_CORE_GLOBAL:
    case QTT_CORE_QUOTE:
        return true;
    case QTT_CORE_LAMBDA:
        if (capture_free_lambda(core)) return true;
        builder->error = QTT_RESOURCE_ELABORATE_UNSUPPORTED_CORE;
        return false;
    case QTT_CORE_VAR: {
        QttRepresentation representation = QTT_REP_UNKNOWN;
        const ResourceBinding *binding =
            find_resource_binding(
                bindings, core->var, &representation);
        if (!binding && core->type) {
            representation =
                qtt_resource_classify_type(core->type);
            if (representation == QTT_REP_IMMEDIATE ||
                representation == QTT_REP_INLINE)
                return true;
        }
        if (binding && !binding->tracks_capability)
            return true;
        QttCoreVar capability =
            binding ? binding->capability : core->var;
        return builder_push(
            builder,
            tail_position && binding &&
                    !qtt_core_var_equal(
                        binding->var, binding->capability)
                ? qtt_resource_move_alias(
                      binding->var, binding->capability)
                : tail_position ? qtt_resource_move(capability)
                          : qtt_resource_borrow(capability));
    }
    case QTT_CORE_PLACE: {
        const ResourceBinding *binding = find_resource_binding(
            bindings, core->place.root, NULL);
        if (!binding || !binding->tracks_capability) {
            builder->error =
                QTT_RESOURCE_ELABORATE_INVALID_RESOURCE_STATE;
            return false;
        }
        QttPlace place = core->place;
        place.root = binding->capability;
        QttRepresentation representation =
            qtt_resource_classify_type(core->type);
        if (tail_position && representation == QTT_REP_OWNED_HEAP)
            return builder_push(
                builder, qtt_resource_move_place(place));
        return builder_push(
            builder, qtt_resource_borrow(binding->capability));
    }
    case QTT_CORE_LET: {
        if (!elaborate(
                builder, core->let.value, false, bindings,
                signatures, module_id))
            return false;
        const QttCoreNode *callable = known_callable_value(
            core->let.value, bindings);
        QttRepresentation representation = callable
            ? QTT_REP_INLINE
            : expression_representation(
                  core->let.value, signatures, module_id);
        if (representation == QTT_REP_UNKNOWN) {
            builder->error =
                QTT_RESOURCE_ELABORATE_INVALID_RESOURCE_STATE;
            return false;
        }
        bool allocates_capability = !callable &&
            expression_allocates_capability(
                core->let.value, signatures, module_id);
        const ResourceBinding *aliased = NULL;
        if (!allocates_capability &&
            core->let.value->kind == QTT_CORE_VAR)
            aliased = find_resource_binding(
                bindings, core->let.value->var, NULL);
        if (allocates_capability &&
            !builder_push(builder, qtt_resource_alloc_typed(
                core->let.binding, representation)))
            return false;
        ResourceBinding binding = {
            .var = core->let.binding,
            .capability = allocates_capability
                ? core->let.binding
                : aliased && aliased->tracks_capability
                    ? aliased->capability : (QttCoreVar){0},
            .representation = representation,
            .callable = callable,
            .tracks_capability =
                allocates_capability ||
                (aliased && aliased->tracks_capability),
            .parent = bindings,
        };
        if (!allocates_capability && binding.tracks_capability &&
            !builder_push(
                builder,
                qtt_resource_alias(
                    binding.var, binding.capability)))
            return false;
        size_t body_start = builder->count;
        if (!elaborate(
                builder, core->let.body, tail_position, &binding,
                signatures, module_id))
            return false;
        if (!allocates_capability && binding.tracks_capability &&
            !builder_push(
                builder,
                qtt_resource_end_alias(
                    binding.var, binding.capability)))
            return false;
        if (allocates_capability &&
            !builder_has_consumed(
                builder, core->let.binding, body_start))
            return builder_push(builder,
                                qtt_resource_drop(core->let.binding));
        return true;
    }
    case QTT_CORE_BORROW: {
        QttRepresentation representation = QTT_REP_UNKNOWN;
        const ResourceBinding *owner = find_resource_binding(
            bindings, core->borrow.place.root, &representation);
        if (!owner || !owner->tracks_capability ||
            representation != QTT_REP_OWNED_HEAP) {
            builder->error = QTT_RESOURCE_ELABORATE_UNSUPPORTED_CORE;
            return false;
        }
        QttPlace place = core->borrow.place;
        place.root = owner->capability;
        QttResourceOp begin = core->borrow.parent.binder_id
            ? qtt_resource_reborrow_place(
                core->borrow.binding, core->borrow.parent,
                place, core->borrow.loan_kind)
            : qtt_resource_alias_place(
                core->borrow.binding, place, core->borrow.loan_kind);
        ResourceBinding loan = {
            .var = core->borrow.binding,
            .capability = owner->capability,
            .representation = representation,
            .tracks_capability = true,
            .is_loan = true,
            .loan_place = place,
            .loan_kind = core->borrow.loan_kind,
            .parent = bindings,
        };
        return builder_push(builder, begin) &&
            elaborate(builder, core->borrow.body, tail_position,
                      &loan, signatures, module_id) &&
            builder_push(builder, qtt_resource_end_alias_place(
                core->borrow.binding, place, core->borrow.loan_kind));
    }
    case QTT_CORE_SEQUENCE:
        for (size_t i = 0; i < core->sequence.count; i++)
            if (!elaborate(
                    builder, core->sequence.items[i],
                    tail_position && i + 1 == core->sequence.count,
                    bindings, signatures, module_id))
                return false;
        return true;
    case QTT_CORE_WRITE: {
        if (!elaborate(
                builder, core->write.value, false, bindings,
                signatures, module_id))
            return false;
        QttRepresentation representation = QTT_REP_UNKNOWN;
        const ResourceBinding *binding = find_resource_binding(
            bindings, core->write.place.root, &representation);
        if (!binding) {
            builder->error =
                QTT_RESOURCE_ELABORATE_UNSUPPORTED_CORE;
            return false;
        }
        if (!binding->tracks_capability)
            return representation == QTT_REP_IMMEDIATE ||
                   representation == QTT_REP_INLINE;
        const ResourceBinding *loan = find_covering_loan(
            bindings, core->write.place);
        if (loan) {
            if (loan->loan_kind != QTT_LOAN_EXCLUSIVE) {
                builder->error =
                    QTT_RESOURCE_ELABORATE_INVALID_RESOURCE_STATE;
                return false;
            }
            return builder_push(builder, qtt_resource_write_alias_place(
                loan->var, core->write.place));
        }
        if (core->write.place.projection_id != 0 ||
            core->write.place.projection_depth != 0) {
            if (!qtt_core_var_equal(
                    binding->var, binding->capability) ||
                representation != QTT_REP_OWNED_HEAP) {
                builder->error =
                    QTT_RESOURCE_ELABORATE_UNSUPPORTED_CORE;
                return false;
            }
            QttCoreVar alias = {
                .module_id = core->write.place.root.module_id,
                .binder_id = builder->next_synthetic_binder++,
            };
            return builder_push(builder, qtt_resource_alias_place(
                       alias, core->write.place,
                       QTT_LOAN_EXCLUSIVE)) &&
                   builder_push(builder, qtt_resource_write_alias_place(
                       alias, core->write.place)) &&
                   builder_push(builder, qtt_resource_end_alias_place(
                       alias, core->write.place,
                       QTT_LOAN_EXCLUSIVE));
        }
        QttRepresentation replacement =
            expression_representation(
                core->write.value, signatures, module_id);
        if (!qtt_core_var_equal(binding->var, binding->capability) ||
            representation != QTT_REP_OWNED_HEAP ||
            replacement != QTT_REP_OWNED_HEAP ||
            !expression_allocates_capability(
                core->write.value, signatures, module_id)) {
            builder->error =
                QTT_RESOURCE_ELABORATE_UNSUPPORTED_CORE;
            return false;
        }
        return builder_push(
            builder,
            qtt_resource_replace(
                binding->capability, QTT_REP_OWNED_HEAP));
    }
    case QTT_CORE_APPLY: {
        const QttCoreNode *known_lambda = NULL;
        if (core->apply.callee &&
            core->apply.callee->kind == QTT_CORE_VAR) {
            const ResourceBinding *binding = find_resource_binding(
                bindings, core->apply.callee->var, NULL);
            known_lambda = binding ? binding->callable : NULL;
        }
        QttFunctionSignature *signature = NULL;
        if (!known_lambda && (!core->apply.callee ||
                   core->apply.callee->kind != QTT_CORE_GLOBAL ||
                   !signatures || !module_id ||
                   !qtt_signature_env_resolve)) {
            builder->error = QTT_RESOURCE_ELABORATE_UNSUPPORTED_CORE;
            return false;
        } else if (!known_lambda && (!qtt_signature_env_resolve(
                signatures, module_id,
                core->apply.callee->global.name,
                NULL, &signature) ||
                   !signature)) {
            builder->error = QTT_RESOURCE_ELABORATE_UNSUPPORTED_CORE;
            return false;
        }
        size_t parameter_count = known_lambda
            ? known_lambda->lambda.param_count
            : signature ? signature->parameter_count : 0;
        if (parameter_count != core->apply.argument_count) {
            builder->error = QTT_RESOURCE_ELABORATE_UNSUPPORTED_CORE;
            return false;
        }
        for (size_t i = 0; i < core->apply.argument_count; i++) {
            bool transfers = known_lambda
                ? lambda_tail_escapes(
                      known_lambda->lambda.body,
                      known_lambda->lambda.params[i])
                : signature->parameters[i].mode ==
                      QTT_OWNERSHIP_CONSUMED;
            if (!elaborate(
                    builder, core->apply.arguments[i], transfers,
                    bindings, signatures, module_id))
                return false;
        }
        return true;
    }
    case QTT_CORE_IF: {
        if (!elaborate(
                builder, core->conditional.condition, false, bindings,
                signatures, module_id))
            return false;
        Builder then_builder = {.error = QTT_RESOURCE_ELABORATE_OK};
        Builder else_builder = {.error = QTT_RESOURCE_ELABORATE_OK};
        then_builder.next_synthetic_binder =
            builder->next_synthetic_binder;
        else_builder.next_synthetic_binder =
            builder->next_synthetic_binder;
        if (!elaborate(
                &then_builder, core->conditional.then_branch,
                tail_position, bindings, signatures, module_id) ||
            !elaborate(
                &else_builder, core->conditional.else_branch,
                tail_position, bindings, signatures, module_id)) {
            builder->error =
                then_builder.error != QTT_RESOURCE_ELABORATE_OK
                    ? then_builder.error : else_builder.error;
            free(then_builder.ops);
            free(else_builder.ops);
            return false;
        }
        QttResourceBlock *then_block = builder_finish(&then_builder);
        QttResourceBlock *else_block = builder_finish(&else_builder);
        if (!then_block || !else_block) {
            builder->error = QTT_RESOURCE_ELABORATE_OUT_OF_MEMORY;
            qtt_resource_block_free(then_block);
            qtt_resource_block_free(else_block);
            free(then_builder.ops);
            free(else_builder.ops);
            return false;
        }
        if (!builder_push(builder,
                          qtt_resource_branch(then_block, else_block))) {
            qtt_resource_block_free(then_block);
            qtt_resource_block_free(else_block);
            return false;
        }
        builder->next_synthetic_binder =
            then_builder.next_synthetic_binder >
                else_builder.next_synthetic_binder
            ? then_builder.next_synthetic_binder
            : else_builder.next_synthetic_binder;
        return true;
    }
    default:
        builder->error = QTT_RESOURCE_ELABORATE_UNSUPPORTED_CORE;
        return false;
    }
}

QttResourceBlock *qtt_resource_elaborate_lexical(
    const QttCoreNode *core,
    QttResourceElaborationError *error) {
    Builder builder = {0};
    builder.error = QTT_RESOURCE_ELABORATE_OK;
    builder.next_synthetic_binder = UINT64_C(0x9000000000000000);
    if (!elaborate(&builder, core, true, NULL, NULL, 0)) {
        free(builder.ops);
        if (error) *error = builder.error;
        return NULL;
    }
    QttResourceBlock *block = builder_finish(&builder);
    if (!block) {
        free(builder.ops);
        if (error) *error = QTT_RESOURCE_ELABORATE_OUT_OF_MEMORY;
        return NULL;
    }
    QttResourceVerification verified =
        qtt_resource_verify(block, NULL, 0);
    if (verified.error != QTT_RESOURCE_VALID) {
        qtt_resource_block_free(block);
        if (error)
            *error = verified.error == QTT_RESOURCE_BRANCH_MISMATCH
                ? QTT_RESOURCE_ELABORATE_UNBALANCED_CONTROL_FLOW
                : QTT_RESOURCE_ELABORATE_INVALID_RESOURCE_STATE;
        return NULL;
    }
    if (error) *error = QTT_RESOURCE_ELABORATE_OK;
    return block;
}

QttResourceBlock *qtt_resource_elaborate_lexical_in_env(
    const QttCoreNode *core,
    const QttSignatureEnv *signatures, uint64_t module_id,
    QttResourceElaborationError *error) {
    Builder builder = {
        .error = QTT_RESOURCE_ELABORATE_OK,
        .next_synthetic_binder = UINT64_C(0x9000000000000000),
    };
    if (!signatures || !module_id ||
        !elaborate(
            &builder, core, true, NULL, signatures, module_id)) {
        free(builder.ops);
        if (error) *error = builder.error;
        return NULL;
    }
    QttResourceBlock *block = builder_finish(&builder);
    if (!block) {
        free(builder.ops);
        if (error) *error = QTT_RESOURCE_ELABORATE_OUT_OF_MEMORY;
        return NULL;
    }
    QttResourceVerification verified =
        qtt_resource_verify(block, NULL, 0);
    if (verified.error != QTT_RESOURCE_VALID) {
        qtt_resource_block_free(block);
        if (error)
            *error = verified.error == QTT_RESOURCE_BRANCH_MISMATCH
                ? QTT_RESOURCE_ELABORATE_UNBALANCED_CONTROL_FLOW
                : QTT_RESOURCE_ELABORATE_INVALID_RESOURCE_STATE;
        return NULL;
    }
    if (error) *error = QTT_RESOURCE_ELABORATE_OK;
    return block;
}

typedef struct {
    const QttResourceBlock *block;
    size_t index;
    uint64_t next_synthetic_binder;
} PlanCursor;

static bool plan_match_node(
    PlanCursor *cursor, const QttCoreNode *core,
    bool tail_position, const ResourceBinding *bindings,
    const QttSignatureEnv *signatures, uint64_t module_id);

static bool plan_match_op(
    PlanCursor *cursor, QttResourceOpKind kind,
    QttCoreVar var, QttRepresentation representation) {
    if (!cursor->block || cursor->index >= cursor->block->count)
        return false;
    const QttResourceOp *op =
        &cursor->block->ops[cursor->index];
    if (op->kind != kind || !qtt_core_var_equal(op->var, var) ||
        (kind == QTT_RESOURCE_ALLOC &&
         op->representation != representation))
        return false;
    cursor->index++;
    return true;
}

static bool plan_range_consumes(
    const QttResourceBlock *block, size_t start, size_t end,
    QttCoreVar var) {
    if (!block || end > block->count) return false;
    for (size_t i = start; i < end; i++)
        if (op_consumes(&block->ops[i], var)) return true;
    return false;
}

static bool plan_match_node(
    PlanCursor *cursor, const QttCoreNode *core,
    bool tail_position, const ResourceBinding *bindings,
    const QttSignatureEnv *signatures, uint64_t module_id) {
    if (!cursor || !core) return false;
    switch (core->kind) {
    case QTT_CORE_LITERAL:
    case QTT_CORE_GLOBAL:
    case QTT_CORE_QUOTE:
        return true;
    case QTT_CORE_LAMBDA:
        return capture_free_lambda(core);
    case QTT_CORE_VAR: {
        QttRepresentation representation = QTT_REP_UNKNOWN;
        const ResourceBinding *binding =
            find_resource_binding(
                bindings, core->var, &representation);
        if (!binding && core->type) {
            representation =
                qtt_resource_classify_type(core->type);
            if (representation == QTT_REP_IMMEDIATE ||
                representation == QTT_REP_INLINE)
                return true;
        }
        if (binding && !binding->tracks_capability)
            return true;
        if (tail_position && binding &&
            !qtt_core_var_equal(
                binding->var, binding->capability)) {
            if (!plan_match_op(
                    cursor, QTT_RESOURCE_MOVE_ALIAS,
                    binding->var, QTT_REP_UNKNOWN))
                return false;
            return qtt_core_var_equal(
                cursor->block->ops[cursor->index - 1].target,
                binding->capability);
        }
        return plan_match_op(
            cursor,
            tail_position ? QTT_RESOURCE_MOVE
                          : QTT_RESOURCE_BORROW,
            binding ? binding->capability : core->var,
            QTT_REP_UNKNOWN);
    }
    case QTT_CORE_PLACE: {
        const ResourceBinding *binding = find_resource_binding(
            bindings, core->place.root, NULL);
        if (!binding || !binding->tracks_capability)
            return false;
        QttRepresentation representation =
            qtt_resource_classify_type(core->type);
        QttResourceOpKind kind =
            tail_position && representation == QTT_REP_OWNED_HEAP
                ? QTT_RESOURCE_MOVE_PLACE : QTT_RESOURCE_BORROW;
        if (!plan_match_op(cursor, kind, binding->capability,
                           QTT_REP_UNKNOWN))
            return false;
        if (kind == QTT_RESOURCE_MOVE_PLACE) {
            QttPlace expected = core->place;
            expected.root = binding->capability;
            return qtt_place_equal(
                cursor->block->ops[cursor->index - 1].place,
                expected);
        }
        return true;
    }
    case QTT_CORE_LET: {
        if (!plan_match_node(
                cursor, core->let.value, false, bindings,
                signatures, module_id))
            return false;
        const QttCoreNode *callable = known_callable_value(
            core->let.value, bindings);
        QttRepresentation representation = callable
            ? QTT_REP_INLINE
            : expression_representation(
                  core->let.value, signatures, module_id);
        if (representation == QTT_REP_UNKNOWN) return false;
        bool allocates_capability = !callable &&
            expression_allocates_capability(
                core->let.value, signatures, module_id);
        const ResourceBinding *aliased = NULL;
        if (!allocates_capability &&
            core->let.value->kind == QTT_CORE_VAR)
            aliased = find_resource_binding(
                bindings, core->let.value->var, NULL);
        if (allocates_capability &&
            !plan_match_op(
                cursor, QTT_RESOURCE_ALLOC,
                core->let.binding, representation))
            return false;
        ResourceBinding binding = {
            .var = core->let.binding,
            .capability = allocates_capability
                ? core->let.binding
                : aliased && aliased->tracks_capability
                    ? aliased->capability : (QttCoreVar){0},
            .representation = representation,
            .callable = callable,
            .tracks_capability =
                allocates_capability ||
                (aliased && aliased->tracks_capability),
            .parent = bindings,
        };
        if (!allocates_capability && binding.tracks_capability &&
            !plan_match_op(
                cursor, QTT_RESOURCE_ALIAS,
                binding.var, QTT_REP_UNKNOWN))
            return false;
        if (!allocates_capability && binding.tracks_capability) {
            const QttResourceOp *alias =
                &cursor->block->ops[cursor->index - 1];
            if (!qtt_core_var_equal(
                    alias->target, binding.capability))
                return false;
        }
        size_t body_start = cursor->index;
        if (!plan_match_node(
                cursor, core->let.body, tail_position, &binding,
                signatures, module_id))
            return false;
        if (!allocates_capability && binding.tracks_capability) {
            if (!plan_match_op(
                    cursor, QTT_RESOURCE_END_ALIAS,
                    binding.var, QTT_REP_UNKNOWN))
                return false;
            const QttResourceOp *end =
                &cursor->block->ops[cursor->index - 1];
            if (!qtt_core_var_equal(
                    end->target, binding.capability))
                return false;
        }
        if (allocates_capability &&
            !plan_range_consumes(
                cursor->block, body_start, cursor->index,
                core->let.binding) &&
            !plan_match_op(
                cursor, QTT_RESOURCE_DROP,
                core->let.binding, QTT_REP_UNKNOWN))
            return false;
        return true;
    }
    case QTT_CORE_BORROW: {
        QttRepresentation representation = QTT_REP_UNKNOWN;
        const ResourceBinding *owner = find_resource_binding(
            bindings, core->borrow.place.root, &representation);
        if (!owner || !owner->tracks_capability ||
            representation != QTT_REP_OWNED_HEAP ||
            cursor->index >= cursor->block->count)
            return false;
        QttPlace place = core->borrow.place;
        place.root = owner->capability;
        const QttResourceOp *begin = &cursor->block->ops[cursor->index++];
        if (begin->kind != QTT_RESOURCE_ALIAS ||
            !qtt_core_var_equal(begin->var, core->borrow.binding) ||
            !qtt_core_var_equal(begin->parent_alias, core->borrow.parent) ||
            !qtt_place_equal(begin->place, place) ||
            begin->loan_kind != core->borrow.loan_kind ||
            cursor->index >= cursor->block->count)
            return false;
        ResourceBinding loan = {
            .var = core->borrow.binding,
            .capability = owner->capability,
            .representation = representation,
            .tracks_capability = true,
            .is_loan = true,
            .loan_place = place,
            .loan_kind = core->borrow.loan_kind,
            .parent = bindings,
        };
        if (!plan_match_node(cursor, core->borrow.body, tail_position,
                             &loan, signatures, module_id) ||
            cursor->index >= cursor->block->count)
            return false;
        const QttResourceOp *end = &cursor->block->ops[cursor->index++];
        return end->kind == QTT_RESOURCE_END_ALIAS &&
            qtt_core_var_equal(end->var, core->borrow.binding) &&
            qtt_core_var_equal(end->target, place.root) &&
            qtt_place_equal(end->place, place) &&
            end->loan_kind == core->borrow.loan_kind;
    }
    case QTT_CORE_SEQUENCE:
        for (size_t i = 0; i < core->sequence.count; i++)
            if (!plan_match_node(
                    cursor, core->sequence.items[i],
                    tail_position &&
                        i + 1 == core->sequence.count,
                    bindings, signatures, module_id))
                return false;
        return true;
    case QTT_CORE_WRITE: {
        if (!plan_match_node(
                cursor, core->write.value, false, bindings,
                signatures, module_id))
            return false;
        QttRepresentation representation = QTT_REP_UNKNOWN;
        const ResourceBinding *binding = find_resource_binding(
            bindings, core->write.place.root, &representation);
        if (!binding) return false;
        if (!binding->tracks_capability)
            return representation == QTT_REP_IMMEDIATE ||
                   representation == QTT_REP_INLINE;
        const ResourceBinding *loan = find_covering_loan(
            bindings, core->write.place);
        if (loan) {
            if (loan->loan_kind != QTT_LOAN_EXCLUSIVE ||
                cursor->index >= cursor->block->count)
                return false;
            const QttResourceOp *write =
                &cursor->block->ops[cursor->index++];
            return write->kind == QTT_RESOURCE_WRITE_ALIAS &&
                qtt_core_var_equal(write->var, loan->var) &&
                qtt_place_equal(write->place, core->write.place);
        }
        if (core->write.place.projection_id != 0 ||
            core->write.place.projection_depth != 0) {
            QttCoreVar alias = {
                .module_id = core->write.place.root.module_id,
                .binder_id = cursor->next_synthetic_binder++,
            };
            if (!qtt_core_var_equal(
                    binding->var, binding->capability) ||
                representation != QTT_REP_OWNED_HEAP ||
                cursor->index + 3 > cursor->block->count)
                return false;
            const QttResourceOp *begin =
                &cursor->block->ops[cursor->index++];
            const QttResourceOp *write =
                &cursor->block->ops[cursor->index++];
            const QttResourceOp *end =
                &cursor->block->ops[cursor->index++];
            return begin->kind == QTT_RESOURCE_ALIAS &&
                write->kind == QTT_RESOURCE_WRITE_ALIAS &&
                end->kind == QTT_RESOURCE_END_ALIAS &&
                qtt_core_var_equal(begin->var, alias) &&
                qtt_core_var_equal(write->var, alias) &&
                qtt_core_var_equal(end->var, alias) &&
                qtt_place_equal(begin->place, core->write.place) &&
                qtt_place_equal(write->place, core->write.place) &&
                qtt_place_equal(end->place, core->write.place) &&
                begin->loan_kind == QTT_LOAN_EXCLUSIVE &&
                end->loan_kind == QTT_LOAN_EXCLUSIVE;
        }
        QttRepresentation replacement =
            expression_representation(
                core->write.value, signatures, module_id);
        return qtt_core_var_equal(binding->var, binding->capability) &&
            representation == QTT_REP_OWNED_HEAP &&
            replacement == QTT_REP_OWNED_HEAP &&
            expression_allocates_capability(
                core->write.value, signatures, module_id) &&
            plan_match_op(
                cursor, QTT_RESOURCE_REPLACE,
                binding->capability, QTT_REP_OWNED_HEAP);
    }
    case QTT_CORE_APPLY: {
        const QttCoreNode *known_lambda = NULL;
        if (core->apply.callee &&
            core->apply.callee->kind == QTT_CORE_VAR) {
            const ResourceBinding *binding = find_resource_binding(
                bindings, core->apply.callee->var, NULL);
            known_lambda = binding ? binding->callable : NULL;
        }
        QttFunctionSignature *signature = NULL;
        if (!known_lambda && (!core->apply.callee ||
                   core->apply.callee->kind != QTT_CORE_GLOBAL ||
                   !signatures || !module_id ||
                   !qtt_signature_env_resolve))
            return false;
        else if (!known_lambda && (!qtt_signature_env_resolve(
                signatures, module_id,
                core->apply.callee->global.name,
                NULL, &signature) ||
                 !signature))
            return false;
        size_t parameter_count = known_lambda
            ? known_lambda->lambda.param_count
            : signature ? signature->parameter_count : 0;
        if (parameter_count != core->apply.argument_count)
            return false;
        for (size_t i = 0; i < core->apply.argument_count; i++) {
            if (!plan_match_node(
                    cursor, core->apply.arguments[i],
                    known_lambda
                        ? lambda_tail_escapes(
                              known_lambda->lambda.body,
                              known_lambda->lambda.params[i])
                        : signature->parameters[i].mode ==
                              QTT_OWNERSHIP_CONSUMED,
                    bindings, signatures, module_id)) {
                return false;
            }
        }
        return true;
    }
    case QTT_CORE_IF: {
        if (!plan_match_node(
                cursor, core->conditional.condition,
                false, bindings, signatures, module_id) ||
            !cursor->block ||
            cursor->index >= cursor->block->count)
            return false;
        const QttResourceOp *branch =
            &cursor->block->ops[cursor->index++];
        if (branch->kind != QTT_RESOURCE_BRANCH) return false;
        PlanCursor then_cursor = {
            .block = branch->branch.then_block,
            .next_synthetic_binder = cursor->next_synthetic_binder};
        PlanCursor else_cursor = {
            .block = branch->branch.else_block,
            .next_synthetic_binder = cursor->next_synthetic_binder};
        bool matches = plan_match_node(
                &then_cursor, core->conditional.then_branch,
                tail_position, bindings, signatures, module_id) &&
            then_cursor.index == then_cursor.block->count &&
            plan_match_node(
                &else_cursor, core->conditional.else_branch,
                tail_position, bindings, signatures, module_id) &&
            else_cursor.index == else_cursor.block->count;
        if (matches)
            cursor->next_synthetic_binder =
                then_cursor.next_synthetic_binder >
                    else_cursor.next_synthetic_binder
                ? then_cursor.next_synthetic_binder
                : else_cursor.next_synthetic_binder;
        return matches;
    }
    default:
        return false;
    }
}

QttResourceVerification qtt_resource_verify_elaboration(
    const QttResourceBlock *block, const QttCoreNode *core) {
    QttResourceVerification linear =
        qtt_resource_verify(block, NULL, 0);
    if (linear.error != QTT_RESOURCE_VALID) return linear;
    PlanCursor cursor = {
        .block = block,
        .next_synthetic_binder = UINT64_C(0x9000000000000000)};
    bool matches =
        plan_match_node(&cursor, core, true, NULL, NULL, 0) &&
        cursor.index == block->count;
    return matches
        ? linear
        : verification(
              QTT_RESOURCE_PLAN_MISMATCH, 0,
              (QttCoreVar){0}, linear.live_count);
}

QttResourceVerification qtt_resource_verify_elaboration_in_env(
    const QttResourceBlock *block, const QttCoreNode *core,
    const QttSignatureEnv *signatures, uint64_t module_id) {
    QttResourceVerification linear =
        qtt_resource_verify(block, NULL, 0);
    if (linear.error != QTT_RESOURCE_VALID) return linear;
    PlanCursor cursor = {
        .block = block,
        .next_synthetic_binder = UINT64_C(0x9000000000000000)};
    bool matches = signatures && module_id &&
        plan_match_node(
            &cursor, core, true, NULL, signatures, module_id) &&
        cursor.index == block->count;
    return matches
        ? linear
        : verification(
              QTT_RESOURCE_PLAN_MISMATCH, 0,
              (QttCoreVar){0}, linear.live_count);
}

void qtt_resource_block_free(QttResourceBlock *block) {
    if (!block) return;
    for (size_t i = 0; i < block->count; i++) {
        if (block->ops[i].kind != QTT_RESOURCE_BRANCH) continue;
        qtt_resource_block_free(block->ops[i].branch.then_block);
        qtt_resource_block_free(block->ops[i].branch.else_block);
    }
    free(block->ops);
    free(block);
}
