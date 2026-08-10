#include "anf.h"
#include "callable_env.h"
#include "call.h"

#include <stdlib.h>
#include <string.h>

static const QttFunctionSignature *anf_effect_lookup(
    void *context, uint64_t module_id, const char *name) {
    return qtt_signature_env_lookup(
        context, module_id, name);
}

typedef struct {
    QttAnfLoanId id;
    QttPlace place;
    QttLoanKind kind;
    QttAnfLoanId parent;
    bool suspended;
    bool suspends_parent;
} ActiveLoan;

typedef struct {
    QttCoreVar *items;
    size_t count;
    size_t capacity;
    QttPlace *evacuated;
    size_t evacuated_count;
    size_t evacuated_capacity;
    ActiveLoan *loans;
    size_t loan_count;
    size_t loan_capacity;
} LiveSet;

typedef struct {
    bool initialized;
    bool processed;
    LiveSet incoming;
} BlockState;

typedef struct {
    QttCoreVar var;
    QttAnfType type;
    QttRepresentation representation;
    bool owned;
} ResourceDeclaration;

typedef struct {
    const QttAnfProgram *program;
    BlockState *states;
    ResourceDeclaration *declarations;
    size_t declaration_count;
    size_t declaration_capacity;
    QttAnfVerification result;
} Verifier;

static QttAnfInstruction instruction(QttAnfInstructionKind kind) {
    QttAnfInstruction value = {0};
    value.kind = kind;
    return value;
}

QttAnfInstruction qtt_anf_const_number(QttAnfValue result, double value) {
    QttAnfInstruction op = instruction(QTT_ANF_CONST_NUMBER);
    op.result = result; op.result_type = QTT_ANF_TYPE_NUMBER;
    op.number = value; return op;
}
QttAnfInstruction qtt_anf_const_bool(QttAnfValue result, bool value) {
    QttAnfInstruction op = instruction(QTT_ANF_CONST_BOOL);
    op.result = result; op.result_type = QTT_ANF_TYPE_BOOL;
    op.boolean = value; return op;
}
QttAnfInstruction qtt_anf_const_string(QttAnfValue result,
                                       const char *value) {
    QttAnfInstruction op = instruction(QTT_ANF_CONST_STRING);
    op.result = result; op.result_type = QTT_ANF_TYPE_STRING;
    op.string = value; return op;
}
QttAnfInstruction qtt_anf_const_aggregate(QttAnfValue result) {
    QttAnfInstruction op = instruction(QTT_ANF_CONST_AGGREGATE);
    op.result = result;
    op.result_type = QTT_ANF_TYPE_AGGREGATE;
    return op;
}
QttAnfInstruction qtt_anf_alias(QttAnfValue result, QttAnfValue operand) {
    QttAnfInstruction op = instruction(QTT_ANF_ALIAS);
    op.result = result; op.operand = operand; return op;
}
QttAnfInstruction qtt_anf_write_place(
    QttAnfValue result, QttPlace place, QttAnfValue operand) {
    QttAnfInstruction op = instruction(QTT_ANF_WRITE_PLACE);
    op.result = result;
    op.place = place;
    op.operand = operand;
    return op;
}
QttAnfInstruction qtt_anf_write_place_with_loan(
    QttAnfValue result, QttPlace place, QttAnfValue operand,
    QttAnfLoanId loan_id) {
    QttAnfInstruction op = qtt_anf_write_place(result, place, operand);
    op.loan_id = loan_id;
    return op;
}
QttAnfInstruction qtt_anf_replace_value(
    QttAnfValue result, QttCoreVar resource,
    QttRepresentation representation, QttAnfValue operand) {
    QttAnfInstruction op = instruction(QTT_ANF_REPLACE);
    op.result = result;
    op.resource = resource;
    op.place = qtt_place_root(resource);
    op.representation = representation;
    op.operand = operand;
    return op;
}
QttAnfInstruction qtt_anf_alloc(QttCoreVar resource,
                                QttRepresentation representation) {
    QttAnfInstruction op = instruction(QTT_ANF_ALLOC);
    op.resource = resource; op.representation = representation; return op;
}
QttAnfInstruction qtt_anf_bind(QttCoreVar resource,
                               QttRepresentation representation,
                               QttAnfValue operand) {
    QttAnfInstruction op = qtt_anf_alloc(resource, representation);
    op.operand = operand;
    return op;
}
QttAnfInstruction qtt_anf_borrow(QttCoreVar resource) {
    QttAnfInstruction op = instruction(QTT_ANF_BORROW);
    op.resource = resource;
    op.place = qtt_place_root(resource);
    return op;
}
QttAnfInstruction qtt_anf_borrow_value(QttAnfValue result,
                                       QttCoreVar resource) {
    QttAnfInstruction op = qtt_anf_borrow(resource);
    op.result = result;
    return op;
}
QttAnfInstruction qtt_anf_borrow_loan(
    QttAnfValue result, QttCoreVar resource,
    QttAnfLoanId loan_id, QttLoanKind kind) {
    return qtt_anf_borrow_place(
        result, qtt_place_root(resource), loan_id, kind);
}
QttAnfInstruction qtt_anf_borrow_place(
    QttAnfValue result, QttPlace place,
    QttAnfLoanId loan_id, QttLoanKind kind) {
    QttAnfInstruction op = qtt_anf_borrow_value(result, place.root);
    op.place = place;
    op.loan_id = loan_id;
    op.loan_kind = kind;
    return op;
}
QttAnfInstruction qtt_anf_reborrow_place(
    QttAnfValue result, QttPlace place, QttAnfLoanId loan_id,
    QttAnfLoanId parent_loan_id, QttLoanKind kind) {
    QttAnfInstruction op = qtt_anf_borrow_place(
        result, place, loan_id, kind);
    op.parent_loan_id = parent_loan_id;
    return op;
}
QttAnfInstruction qtt_anf_end_borrow(QttAnfLoanId loan_id) {
    QttAnfInstruction op = instruction(QTT_ANF_END_BORROW);
    op.loan_id = loan_id;
    return op;
}
QttAnfInstruction qtt_anf_move(QttCoreVar resource) {
    QttAnfInstruction op = instruction(QTT_ANF_MOVE);
    op.resource = resource; return op;
}
QttAnfInstruction qtt_anf_move_value(QttAnfValue result,
                                     QttCoreVar resource) {
    QttAnfInstruction op = qtt_anf_move(resource);
    op.result = result;
    return op;
}
QttAnfInstruction qtt_anf_move_place(
    QttAnfValue result, QttAnfType result_type, QttPlace place) {
    QttAnfInstruction op = instruction(QTT_ANF_MOVE_PLACE);
    op.result = result;
    op.result_type = result_type;
    op.resource = place.root;
    op.place = place;
    return op;
}
QttAnfInstruction qtt_anf_drop(QttCoreVar resource) {
    QttAnfInstruction op = instruction(QTT_ANF_DROP);
    op.resource = resource; return op;
}
QttAnfInstruction qtt_anf_project_environment(
    QttAnfValue result, QttAnfType result_type,
    uint64_t closure_id, size_t ordinal,
    QttCoreVar resource, QttAnfEnvironmentAction action) {
    QttAnfInstruction op = instruction(QTT_ANF_ENV_PROJECT);
    op.result = result;
    op.result_type = result_type;
    op.resource = resource;
    op.environment_closure_id = closure_id;
    op.environment_ordinal = ordinal;
    op.environment_action = action;
    return op;
}
QttAnfInstruction qtt_anf_call(
    QttAnfValue result, QttCoreVar result_resource, QttCallableId callable,
    QttAnfCallOperand *arguments, size_t argument_count,
    QttAnfType result_type, QttTypeId canonical_result_type,
    QttResultMode result_mode, QttRepresentation result_representation) {
    QttAnfInstruction op = instruction(QTT_ANF_CALL);
    op.result = result;
    op.result_resource = result_resource;
    op.callable = callable;
    op.call_arguments = arguments;
    op.call_argument_count = argument_count;
    op.result_type = result_type;
    op.canonical_result_type = canonical_result_type;
    op.result_mode = result_mode;
    op.representation = result_representation;
    return op;
}
QttAnfTerminator qtt_anf_return(QttAnfValue value) {
    return (QttAnfTerminator){.kind = QTT_ANF_RETURN, .value = value};
}
QttAnfTerminator qtt_anf_jump(size_t target, QttAnfValue *arguments,
                              size_t argument_count) {
    return (QttAnfTerminator){.kind = QTT_ANF_JUMP, .target = target,
        .arguments = arguments, .argument_count = argument_count};
}
QttAnfTerminator qtt_anf_branch(QttAnfValue condition, size_t then_target,
                                size_t else_target) {
    return (QttAnfTerminator){.kind = QTT_ANF_BRANCH, .value = condition,
        .target = then_target, .else_target = else_target};
}

static ptrdiff_t live_find(const LiveSet *set, QttCoreVar var) {
    for (size_t i = 0; i < set->count; i++)
        if (qtt_core_var_equal(set->items[i], var)) return (ptrdiff_t)i;
    return -1;
}
static bool live_push(LiveSet *set, QttCoreVar var) {
    if (set->count == set->capacity) {
        size_t next = set->capacity ? set->capacity * 2 : 8;
        QttCoreVar *grown = realloc(set->items, next * sizeof(*grown));
        if (!grown) return false;
        set->items = grown; set->capacity = next;
    }
    set->items[set->count++] = var; return true;
}
static bool live_copy(LiveSet *to, const LiveSet *from) {
    memset(to, 0, sizeof(*to));
    if (from->count) {
        to->items = malloc(from->count * sizeof(*to->items));
        if (!to->items) return false;
        memcpy(to->items, from->items, from->count * sizeof(*to->items));
        to->count = to->capacity = from->count;
    }
    if (from->evacuated_count) {
        to->evacuated = malloc(
            from->evacuated_count * sizeof(*to->evacuated));
        if (!to->evacuated) {
            free(to->items);
            memset(to, 0, sizeof(*to));
            return false;
        }
        memcpy(to->evacuated, from->evacuated,
               from->evacuated_count * sizeof(*to->evacuated));
        to->evacuated_count = to->evacuated_capacity =
            from->evacuated_count;
    }
    if (from->loan_count) {
        to->loans = malloc(from->loan_count * sizeof(*to->loans));
        if (!to->loans) {
            free(to->items);
            free(to->evacuated);
            memset(to, 0, sizeof(*to));
            return false;
        }
        memcpy(to->loans, from->loans,
               from->loan_count * sizeof(*to->loans));
        to->loan_count = to->loan_capacity = from->loan_count;
    }
    return true;
}
static ptrdiff_t loan_find(const LiveSet *set, QttAnfLoanId id) {
    for (size_t i = 0; i < set->loan_count; i++)
        if (set->loans[i].id == id) return (ptrdiff_t)i;
    return -1;
}
static bool place_has_active_loan(const LiveSet *set, QttPlace place) {
    for (size_t i = 0; i < set->loan_count; i++)
        if (qtt_place_overlaps(set->loans[i].place, place)) return true;
    return false;
}
static bool loan_authorizes(
    const LiveSet *set, QttAnfLoanId id,
    QttPlace place, QttLoanKind kind) {
    ptrdiff_t found = loan_find(set, id);
    return found >= 0 && !set->loans[(size_t)found].suspended &&
        set->loans[(size_t)found].kind == kind &&
        qtt_place_equal(set->loans[(size_t)found].place, place);
}
static bool loan_is_ancestor(
    const LiveSet *set, QttAnfLoanId candidate, QttAnfLoanId loan) {
    while (loan) {
        ptrdiff_t found = loan_find(set, loan);
        if (found < 0) return false;
        loan = set->loans[(size_t)found].parent;
        if (loan == candidate) return true;
    }
    return false;
}
static bool loan_push(LiveSet *set, ActiveLoan loan) {
    if (set->loan_count == set->loan_capacity) {
        size_t next = set->loan_capacity ? set->loan_capacity * 2 : 4;
        ActiveLoan *grown = realloc(
            set->loans, next * sizeof(*grown));
        if (!grown) return false;
        set->loans = grown;
        set->loan_capacity = next;
    }
    set->loans[set->loan_count++] = loan;
    return true;
}
static bool live_place_evacuated(const LiveSet *set, QttPlace place) {
    for (size_t i = 0; i < set->evacuated_count; i++)
        if (qtt_place_overlaps(set->evacuated[i], place)) return true;
    return false;
}
static bool live_root_partial(const LiveSet *set, QttCoreVar root) {
    for (size_t i = 0; i < set->evacuated_count; i++)
        if (qtt_core_var_equal(set->evacuated[i].root, root)) return true;
    return false;
}
static bool live_evacuate(LiveSet *set, QttPlace place) {
    if (set->evacuated_count == set->evacuated_capacity) {
        size_t next = set->evacuated_capacity
            ? set->evacuated_capacity * 2 : 4;
        QttPlace *grown = realloc(
            set->evacuated, next * sizeof(*grown));
        if (!grown) return false;
        set->evacuated = grown;
        set->evacuated_capacity = next;
    }
    set->evacuated[set->evacuated_count++] = place;
    return true;
}
static void live_clear_root(LiveSet *set, QttCoreVar root) {
    size_t output = 0;
    for (size_t i = 0; i < set->evacuated_count; i++)
        if (!qtt_core_var_equal(set->evacuated[i].root, root))
            set->evacuated[output++] = set->evacuated[i];
    set->evacuated_count = output;
}
static bool live_equal(const LiveSet *a, const LiveSet *b) {
    if (a->count != b->count ||
        a->evacuated_count != b->evacuated_count ||
        a->loan_count != b->loan_count) return false;
    for (size_t i = 0; i < a->count; i++)
        if (live_find(b, a->items[i]) < 0) return false;
    for (size_t i = 0; i < a->evacuated_count; i++) {
        bool found = false;
        for (size_t j = 0; j < b->evacuated_count; j++)
            if (qtt_place_equal(a->evacuated[i], b->evacuated[j])) {
                found = true;
                break;
            }
        if (!found) return false;
    }
    for (size_t i = 0; i < a->loan_count; i++) {
        ptrdiff_t found = loan_find(b, a->loans[i].id);
        if (found < 0 ||
            !qtt_place_equal(
                a->loans[i].place, b->loans[(size_t)found].place) ||
            a->loans[i].kind != b->loans[(size_t)found].kind)
            return false;
        if (a->loans[i].parent != b->loans[(size_t)found].parent ||
            a->loans[i].suspended != b->loans[(size_t)found].suspended ||
            a->loans[i].suspends_parent !=
                b->loans[(size_t)found].suspends_parent)
            return false;
    }
    return true;
}
static bool value_defined(const QttAnfValue *values, size_t count,
                          QttAnfValue value) {
    if (!value) return false;
    for (size_t i = 0; i < count; i++) if (values[i] == value) return true;
    return false;
}
static QttAnfType value_type(const QttAnfValue *values,
                             const QttAnfType *types, size_t count,
                             QttAnfValue value) {
    for (size_t i = 0; i < count; i++)
        if (values[i] == value) return types[i];
    return QTT_ANF_TYPE_UNKNOWN;
}
static ptrdiff_t value_index(const QttAnfValue *values, size_t count,
                             QttAnfValue value) {
    for (size_t i = 0; i < count; i++)
        if (values[i] == value) return (ptrdiff_t)i;
    return -1;
}
static QttRepresentation anf_type_representation(QttAnfType type) {
    if (type == QTT_ANF_TYPE_STRING ||
        type == QTT_ANF_TYPE_AGGREGATE) return QTT_REP_OWNED_HEAP;
    if (type == QTT_ANF_TYPE_NUMBER || type == QTT_ANF_TYPE_BOOL ||
        type == QTT_ANF_TYPE_UNIT)
        return QTT_REP_IMMEDIATE;
    return QTT_REP_UNKNOWN;
}
static ptrdiff_t declaration_find(const Verifier *verifier,
                                  QttCoreVar var) {
    for (size_t i = 0; i < verifier->declaration_count; i++)
        if (qtt_core_var_equal(verifier->declarations[i].var, var))
            return (ptrdiff_t)i;
    return -1;
}

static const QttAnfEnvironmentSlot *environment_slot_find(
    const QttAnfProgram *program, uint64_t closure_id,
    size_t ordinal) {
    for (size_t i = 0; i < program->environment_slot_count; i++)
        if (program->environment_slots[i].closure_id == closure_id &&
            program->environment_slots[i].ordinal == ordinal)
            return &program->environment_slots[i];
    return NULL;
}

static const QttAnfPlaceDeclaration *mutable_place_find(
    const QttAnfProgram *program, QttPlace place) {
    for (size_t i = 0; i < program->mutable_place_count; i++)
        if (qtt_place_equal(
                program->mutable_places[i].place, place))
            return &program->mutable_places[i];
    return NULL;
}
static bool declaration_add(Verifier *verifier, QttCoreVar var,
                            QttAnfType type,
                            QttRepresentation representation,
                            bool owned) {
    if (declaration_find(verifier, var) >= 0) return true;
    if (verifier->declaration_count == verifier->declaration_capacity) {
        size_t next = verifier->declaration_capacity
            ? verifier->declaration_capacity * 2 : 8;
        ResourceDeclaration *grown = realloc(
            verifier->declarations, next * sizeof(*grown));
        if (!grown) return false;
        verifier->declarations = grown;
        verifier->declaration_capacity = next;
    }
    verifier->declarations[verifier->declaration_count++] =
        (ResourceDeclaration){var, type, representation, owned};
    return true;
}

static void verify_block(Verifier *verifier, size_t index, const LiveSet *input);

static void propagate(Verifier *v, size_t from, size_t target,
                      const QttAnfValue *defined, size_t defined_count,
                      const QttAnfType *defined_types,
                      const QttAnfLoanId *defined_loans,
                      QttAnfValue *arguments, size_t argument_count,
                      const LiveSet *live) {
    if (target >= v->program->block_count) {
        v->result = (QttAnfVerification){QTT_ANF_INVALID_BLOCK, from, 0};
        return;
    }
    QttAnfBlock *destination = &v->program->blocks[target];
    if (argument_count != destination->parameter_count) {
        v->result = (QttAnfVerification){
            QTT_ANF_BLOCK_ARGUMENT_MISMATCH, from, 0};
        return;
    }
    for (size_t i = 0; i < argument_count; i++)
        if (!value_defined(defined, defined_count, arguments[i])) {
            v->result = (QttAnfVerification){QTT_ANF_UNDEFINED_VALUE, from, i};
            return;
        } else {
            ptrdiff_t argument = value_index(
                defined, defined_count, arguments[i]);
            QttAnfLoanId actual_loan = argument >= 0
                ? defined_loans[(size_t)argument] : 0;
            QttAnfLoanId expected_loan = destination->parameter_loans
                ? destination->parameter_loans[i] : 0;
            if (!destination->parameter_types ||
                value_type(defined, defined_types, defined_count,
                           arguments[i]) !=
                    destination->parameter_types[i]) {
                v->result = (QttAnfVerification){
                    QTT_ANF_TYPE_MISMATCH, from, i};
                return;
            }
            if (actual_loan != expected_loan ||
                (expected_loan && loan_find(live, expected_loan) < 0)) {
                v->result = (QttAnfVerification){
                    QTT_ANF_INVALID_LOAN, from, i};
                return;
            }
        }
    verify_block(v, target, live);
}

static void verify_block(Verifier *v, size_t index, const LiveSet *input) {
    if (v->result.error != QTT_ANF_VALID) return;
    BlockState *state = &v->states[index];
    if (state->initialized) {
        if (!live_equal(&state->incoming, input))
            v->result = (QttAnfVerification){
                QTT_ANF_OWNERSHIP_JOIN_MISMATCH, index, 0};
        return;
    }
    state->initialized = true;
    if (!live_copy(&state->incoming, input)) {
        v->result.error = QTT_ANF_OUT_OF_MEMORY; return;
    }
    QttAnfBlock *block = &v->program->blocks[index];
    size_t max_values = block->parameter_count + block->instruction_count;
    QttAnfValue *defined = calloc(max_values ? max_values : 1, sizeof(*defined));
    QttAnfType *defined_types =
        calloc(max_values ? max_values : 1, sizeof(*defined_types));
    int *defined_by = malloc((max_values ? max_values : 1) *
                             sizeof(*defined_by));
    QttCoreVar *defined_resources = calloc(
        max_values ? max_values : 1, sizeof(*defined_resources));
    QttAnfLoanId *defined_loans = calloc(
        max_values ? max_values : 1, sizeof(*defined_loans));
    if (!defined || !defined_types || !defined_by ||
        !defined_resources || !defined_loans) {
        free(defined); free(defined_types); free(defined_by);
        free(defined_resources);
        free(defined_loans);
        v->result.error = QTT_ANF_OUT_OF_MEMORY; return;
    }
    for (size_t i = 0; i < (max_values ? max_values : 1); i++)
        defined_by[i] = -1;
    size_t defined_count = 0;
    for (size_t i = 0; i < block->parameter_count; i++) {
        if (!block->parameters[i] ||
            value_defined(defined, defined_count, block->parameters[i])) {
            v->result = (QttAnfVerification){
                QTT_ANF_DUPLICATE_VALUE, index, i};
            free(defined);
            free(defined_types);
            free(defined_by);
            free(defined_resources);
            free(defined_loans);
            return;
        }
        if (!block->parameter_types ||
            block->parameter_types[i] == QTT_ANF_TYPE_UNKNOWN) {
            v->result = (QttAnfVerification){
                QTT_ANF_TYPE_MISMATCH, index, i};
            free(defined);
            free(defined_types);
            free(defined_by);
            free(defined_resources);
            free(defined_loans);
            return;
        }
        defined_types[defined_count] = block->parameter_types[i];
        defined_loans[defined_count] = block->parameter_loans
            ? block->parameter_loans[i] : 0;
        if (defined_loans[defined_count] &&
            loan_find(input, defined_loans[defined_count]) < 0) {
            v->result = (QttAnfVerification){
                QTT_ANF_INVALID_LOAN, index, i};
            free(defined);
            free(defined_types);
            free(defined_by);
            free(defined_resources);
            free(defined_loans);
            return;
        }
        defined[defined_count++] = block->parameters[i];
    }
    LiveSet live;
    if (!live_copy(&live, input)) {
        free(defined); free(defined_types); free(defined_by);
        free(defined_resources);
        free(defined_loans);
        v->result.error = QTT_ANF_OUT_OF_MEMORY; return;
    }
    for (size_t i = 0; i < block->instruction_count; i++) {
        QttAnfInstruction *op = &block->instructions[i];
        if ((op->kind == QTT_ANF_ALIAS ||
             op->kind == QTT_ANF_WRITE_PLACE ||
             op->kind == QTT_ANF_REPLACE ||
             (op->kind == QTT_ANF_ALLOC && op->operand)) &&
            !value_defined(defined, defined_count, op->operand)) {
            v->result = (QttAnfVerification){
                QTT_ANF_UNDEFINED_VALUE, index, i};
            break;
        }
        if (op->operand) {
            ptrdiff_t operand_index = value_index(
                defined, defined_count, op->operand);
            if (operand_index >= 0 &&
                defined_loans[(size_t)operand_index] &&
                (loan_find(&live,
                     defined_loans[(size_t)operand_index]) < 0 ||
                 live.loans[(size_t)loan_find(
                     &live, defined_loans[(size_t)operand_index])].suspended)) {
                v->result = (QttAnfVerification){
                    QTT_ANF_INVALID_LOAN, index, i};
                break;
            }
        }
        if (op->kind == QTT_ANF_WRITE_PLACE) {
            const QttAnfPlaceDeclaration *place =
                mutable_place_find(v->program, op->place);
            QttAnfType operand_type = value_type(
                defined, defined_types, defined_count,
                op->operand);
            if (!place) {
                v->result = (QttAnfVerification){
                    QTT_ANF_INVALID_PLACE, index, i};
                break;
            }
            ptrdiff_t root_declaration =
                declaration_find(v, op->place.root);
            if (root_declaration >= 0 &&
                v->declarations[(size_t)root_declaration].representation ==
                    QTT_REP_OWNED_HEAP &&
                !loan_authorizes(
                    &live, op->loan_id, op->place,
                    QTT_LOAN_EXCLUSIVE)) {
                v->result = (QttAnfVerification){
                    QTT_ANF_INVALID_LOAN, index, i};
                break;
            }
            if (place->type == QTT_ANF_TYPE_UNKNOWN ||
                operand_type != place->type) {
                v->result = (QttAnfVerification){
                    QTT_ANF_TYPE_MISMATCH, index, i};
                break;
            }
        }
        if (op->kind == QTT_ANF_ENV_PROJECT) {
            const QttAnfEnvironmentSlot *slot =
                environment_slot_find(
                    v->program, op->environment_closure_id,
                    op->environment_ordinal);
            if (!slot ||
                !qtt_core_var_equal(slot->source, op->resource) ||
                ((op->environment_action == QTT_ANF_ENV_RELEASE) !=
                 (op->result == 0)) ||
                (op->environment_action != QTT_ANF_ENV_RELEASE &&
                 (op->result_type == QTT_ANF_TYPE_UNKNOWN ||
                  anf_type_representation(op->result_type) !=
                      slot->representation)) ||
                (op->environment_action == QTT_ANF_ENV_MOVE &&
                 slot->capability != QTT_SEMANTIC_CAPABILITY_OWNED) ||
                (op->environment_action == QTT_ANF_ENV_RELEASE &&
                 (slot->capability != QTT_SEMANTIC_CAPABILITY_OWNED ||
                  slot->exit != QTT_CLOSURE_FIELD_RELEASE)) ||
                (op->environment_action != QTT_ANF_ENV_MOVE &&
                 op->environment_action != QTT_ANF_ENV_BORROW &&
                 op->environment_action != QTT_ANF_ENV_RELEASE)) {
                v->result = (QttAnfVerification){
                    QTT_ANF_INVALID_RESOURCE, index, i};
                break;
            }
        }
        const QttFunctionSignature *call_signature = NULL;
        if (op->kind == QTT_ANF_CALL) {
            call_signature = v->program->signatures
                ? qtt_signature_env_lookup_id(
                      v->program->signatures, op->callable)
                : NULL;
            if (!call_signature) {
                v->result = (QttAnfVerification){
                    QTT_ANF_UNKNOWN_CALLABLE, index, i};
                break;
            }
            uint64_t contract_fingerprint =
                qtt_signature_contract_fingerprint(
                    call_signature);
            if (!contract_fingerprint ||
                call_signature->contract_fingerprint !=
                    contract_fingerprint ||
                op->contract_fingerprint !=
                    contract_fingerprint) {
                v->result = (QttAnfVerification){
                    QTT_ANF_CALL_CONTRACT_MISMATCH, index, i};
                break;
            }
            if (op->call_argument_count !=
                call_signature->parameter_count) {
                v->result = (QttAnfVerification){
                    QTT_ANF_CALL_ARITY_MISMATCH, index, i};
                break;
            }
            if (op->call_argument_count && !op->call_arguments) {
                v->result = (QttAnfVerification){
                    QTT_ANF_CALL_TYPE_MISMATCH, index, i};
                break;
            }
            if (!op->result ||
                op->result_type == QTT_ANF_TYPE_UNKNOWN ||
                (op->result_mode == QTT_RESULT_OWNED &&
                 (!op->result_resource.module_id ||
                  !op->result_resource.binder_id)) ||
                (op->result_mode != QTT_RESULT_OWNED &&
                 (op->result_resource.module_id ||
                  op->result_resource.binder_id))) {
                v->result = (QttAnfVerification){
                    QTT_ANF_CALL_RESULT_MISMATCH, index, i};
                break;
            }
            for (size_t argument = 0;
                 argument < op->call_argument_count; argument++) {
                const QttAnfCallOperand *actual =
                    &op->call_arguments[argument];
                const QttParameterContract *expected =
                    &call_signature->parameters[argument];
                ptrdiff_t actual_index =
                    value_index(defined, defined_count, actual->value);
                if (actual_index < 0) {
                    v->result = (QttAnfVerification){
                        QTT_ANF_UNDEFINED_VALUE, index, i};
                    break;
                }
                if (actual->type !=
                        defined_types[(size_t)actual_index] ||
                    anf_type_representation(actual->type) !=
                        actual->representation ||
                    !qtt_type_id_equal(actual->type_id,
                                       expected->type_id) ||
                    actual->representation !=
                        expected->representation) {
                    v->result = (QttAnfVerification){
                        QTT_ANF_CALL_TYPE_MISMATCH, index, i};
                    break;
                }
                if (actual->transfer !=
                    qtt_call_transfer_for_parameter(expected)) {
                    v->result = (QttAnfVerification){
                        QTT_ANF_CALL_TRANSFER_MISMATCH, index, i};
                    break;
                }
                if ((actual->transfer == QTT_CALL_MOVE &&
                     defined_by[(size_t)actual_index] != QTT_ANF_MOVE) ||
                    (actual->transfer == QTT_CALL_BORROW &&
                     defined_by[(size_t)actual_index] != QTT_ANF_BORROW) ||
                    ((actual->transfer == QTT_CALL_MOVE ||
                      actual->transfer == QTT_CALL_BORROW) &&
                     !qtt_core_var_equal(
                         actual->source_resource,
                         defined_resources[(size_t)actual_index])) ||
                    (actual->transfer == QTT_CALL_VALUE &&
                     (actual->source_resource.module_id ||
                      actual->source_resource.binder_id)) ||
                    /* A borrow is a call-scoped loan.  Its owner must still
                     * be live when the consuming CALL executes; moving or
                     * dropping it between BORROW and CALL invalidates the
                     * provenance proof. */
                    (actual->transfer == QTT_CALL_BORROW &&
                     live_find(&live, actual->source_resource) < 0)) {
                    v->result = (QttAnfVerification){
                        QTT_ANF_CALL_PROVENANCE_MISMATCH, index, i};
                    break;
                }
                if (actual->transfer == QTT_CALL_BORROW &&
                    actual->loan_id &&
                    (defined_loans[(size_t)actual_index] != actual->loan_id ||
                     !loan_authorizes(
                         &live, actual->loan_id,
                         qtt_place_root(actual->source_resource),
                         QTT_LOAN_SHARED))) {
                    v->result = (QttAnfVerification){
                        QTT_ANF_CALL_PROVENANCE_MISMATCH, index, i};
                    break;
                }
            }
            if (v->result.error != QTT_ANF_VALID) break;
            if (op->result_mode != call_signature->result.mode ||
                anf_type_representation(op->result_type) !=
                    op->representation ||
                op->representation !=
                    call_signature->result.representation ||
                !qtt_type_id_equal(
                    op->canonical_result_type,
                    call_signature->result.type_id)) {
                v->result = (QttAnfVerification){
                    QTT_ANF_CALL_RESULT_MISMATCH, index, i};
                break;
            }
        }
        if (op->result) {
            if (value_defined(defined, defined_count, op->result)) {
                v->result = (QttAnfVerification){
                    QTT_ANF_DUPLICATE_VALUE, index, i}; break;
            }
            QttAnfType result_type = op->result_type;
            if (op->kind == QTT_ANF_ALIAS ||
                op->kind == QTT_ANF_WRITE_PLACE ||
                op->kind == QTT_ANF_REPLACE)
                result_type = value_type(
                    defined, defined_types, defined_count, op->operand);
            if (result_type == QTT_ANF_TYPE_UNKNOWN) {
                v->result = (QttAnfVerification){
                    QTT_ANF_TYPE_MISMATCH, index, i};
                break;
            }
            defined_types[defined_count] = result_type;
            defined_by[defined_count] =
                op->kind == QTT_ANF_ENV_PROJECT
                ? (op->environment_action == QTT_ANF_ENV_MOVE
                       ? QTT_ANF_MOVE : QTT_ANF_BORROW)
                : (int)op->kind;
            if (op->kind == QTT_ANF_MOVE ||
                op->kind == QTT_ANF_BORROW ||
                op->kind == QTT_ANF_ENV_PROJECT)
                defined_resources[defined_count] = op->resource;
            if (op->kind == QTT_ANF_BORROW)
                defined_loans[defined_count] = op->loan_id;
            else if (op->kind == QTT_ANF_ALIAS && op->operand) {
                ptrdiff_t source = value_index(
                    defined, defined_count, op->operand);
                if (source >= 0)
                    defined_loans[defined_count] =
                        defined_loans[(size_t)source];
            }
            defined[defined_count++] = op->result;
        }
        ptrdiff_t found = live_find(&live, op->resource);
        if (op->kind == QTT_ANF_ALLOC) {
            QttAnfType operand_type = op->operand
                ? value_type(defined, defined_types, defined_count,
                             op->operand)
                : QTT_ANF_TYPE_UNKNOWN;
            QttRepresentation expected =
                anf_type_representation(operand_type);
            if (op->operand && expected != QTT_REP_UNKNOWN &&
                expected != op->representation) {
                v->result = (QttAnfVerification){
                    QTT_ANF_REPRESENTATION_MISMATCH, index, i};
                break;
            }
            if (!declaration_add(v, op->resource, operand_type,
                                 op->representation, true)) {
                v->result = (QttAnfVerification){
                    QTT_ANF_OUT_OF_MEMORY, index, i};
                break;
            }
            if (found >= 0 || !live_push(&live, op->resource)) {
                v->result = (QttAnfVerification){
                    found >= 0 ? QTT_ANF_INVALID_RESOURCE
                               : QTT_ANF_OUT_OF_MEMORY, index, i};
                break;
            }
        } else if (op->kind == QTT_ANF_REPLACE) {
            ptrdiff_t declaration =
                declaration_find(v, op->resource);
            QttAnfType operand_type = value_type(
                defined, defined_types, defined_count, op->operand);
            if (found < 0 || declaration < 0 ||
                live_root_partial(&live, op->resource) ||
                place_has_active_loan(
                    &live, qtt_place_root(op->resource)) ||
                op->representation != QTT_REP_OWNED_HEAP ||
                !qtt_place_equal(
                    op->place, qtt_place_root(op->resource)) ||
                operand_type !=
                    v->declarations[(size_t)declaration].type ||
                v->declarations[(size_t)declaration].representation !=
                    QTT_REP_OWNED_HEAP) {
                v->result = (QttAnfVerification){
                    QTT_ANF_INVALID_RESOURCE, index, i};
                break;
            }
        } else if (op->kind == QTT_ANF_MOVE_PLACE) {
            ptrdiff_t declaration = declaration_find(v, op->resource);
            if (found < 0 || declaration < 0 ||
                place_has_active_loan(&live, op->place) ||
                v->declarations[(size_t)declaration].representation !=
                    QTT_REP_OWNED_HEAP ||
                !qtt_core_var_equal(op->resource, op->place.root) ||
                op->place.projection_depth == 0 ||
                live_place_evacuated(&live, op->place) ||
                !live_evacuate(&live, op->place)) {
                v->result = (QttAnfVerification){
                    QTT_ANF_INVALID_RESOURCE, index, i};
                break;
            }
        } else if (op->kind == QTT_ANF_BORROW ||
                   (op->kind == QTT_ANF_ENV_PROJECT &&
                    op->environment_action == QTT_ANF_ENV_BORROW)) {
            if (found < 0 || live_place_evacuated(&live, op->place)) {
                v->result = (QttAnfVerification){
                    QTT_ANF_INVALID_RESOURCE,index,i};
                break;
            }
            if (op->kind == QTT_ANF_BORROW) {
                ptrdiff_t parent = op->parent_loan_id
                    ? loan_find(&live, op->parent_loan_id) : -1;
                bool invalid_parent = op->parent_loan_id &&
                    (parent < 0 || live.loans[(size_t)parent].suspended ||
                     !qtt_place_contains(
                         live.loans[(size_t)parent].place, op->place) ||
                     (op->loan_kind == QTT_LOAN_EXCLUSIVE &&
                      live.loans[(size_t)parent].kind != QTT_LOAN_EXCLUSIVE));
                bool conflict = op->loan_id &&
                    loan_find(&live, op->loan_id) >= 0;
                for (size_t loan = 0;
                     loan < live.loan_count && !conflict; loan++)
                    if (op->parent_loan_id &&
                        (live.loans[loan].id == op->parent_loan_id ||
                         loan_is_ancestor(
                             &live, live.loans[loan].id,
                             op->parent_loan_id)))
                        continue;
                    else
                    if (qtt_place_overlaps(
                            live.loans[loan].place, op->place) &&
                        (op->loan_kind == QTT_LOAN_EXCLUSIVE ||
                         live.loans[loan].kind == QTT_LOAN_EXCLUSIVE))
                        conflict = true;
                bool invalid_kind =
                    op->loan_kind != QTT_LOAN_SHARED &&
                    op->loan_kind != QTT_LOAN_EXCLUSIVE;
                bool suspends_parent = op->parent_loan_id &&
                    parent >= 0 &&
                    live.loans[(size_t)parent].kind == QTT_LOAN_EXCLUSIVE;
                if (invalid_kind || invalid_parent || conflict ||
                    (op->loan_id && !loan_push(&live, (ActiveLoan){
                        .id = op->loan_id,
                        .place = op->place,
                        .kind = op->loan_kind,
                        .parent = op->parent_loan_id,
                        .suspends_parent = suspends_parent,
                    }))) {
                    v->result = (QttAnfVerification){
                        (invalid_kind || invalid_parent || conflict)
                                 ? QTT_ANF_INVALID_LOAN
                                 : QTT_ANF_OUT_OF_MEMORY,
                        index, i};
                    break;
                }
                if (suspends_parent)
                    live.loans[(size_t)parent].suspended = true;
            }
            ptrdiff_t declaration = declaration_find(v, op->resource);
            bool root_loan = qtt_place_equal(
                op->place, qtt_place_root(op->resource));
            if (op->result && root_loan && declaration >= 0 &&
                v->declarations[(size_t)declaration].type !=
                    QTT_ANF_TYPE_UNKNOWN &&
                op->result_type !=
                    v->declarations[(size_t)declaration].type) {
                v->result = (QttAnfVerification){
                    QTT_ANF_TYPE_MISMATCH,index,i};
                break;
            }
        } else if (op->kind == QTT_ANF_DROP ||
                   op->kind == QTT_ANF_MOVE ||
                   (op->kind == QTT_ANF_ENV_PROJECT &&
                    (op->environment_action == QTT_ANF_ENV_MOVE ||
                     op->environment_action == QTT_ANF_ENV_RELEASE))) {
            ptrdiff_t declaration = declaration_find(v, op->resource);
            if (found < 0 || declaration < 0 ||
                !v->declarations[(size_t)declaration].owned ||
                place_has_active_loan(
                    &live, qtt_place_root(op->resource))) {
                v->result = (QttAnfVerification){
                    QTT_ANF_INVALID_RESOURCE,index,i}; break;
            }
            if (op->kind == QTT_ANF_MOVE &&
                live_root_partial(&live, op->resource)) {
                v->result = (QttAnfVerification){
                    QTT_ANF_INVALID_RESOURCE,index,i};
                break;
            }
            if ((op->kind == QTT_ANF_MOVE ||
                 op->kind == QTT_ANF_ENV_PROJECT) && op->result &&
                declaration >= 0 &&
                v->declarations[(size_t)declaration].type !=
                    QTT_ANF_TYPE_UNKNOWN &&
                op->result_type !=
                    v->declarations[(size_t)declaration].type) {
                v->result = (QttAnfVerification){
                    QTT_ANF_TYPE_MISMATCH,index,i};
                break;
            }
            live.items[(size_t)found] = live.items[--live.count];
            live_clear_root(&live, op->resource);
        } else if (op->kind == QTT_ANF_END_BORROW) {
            ptrdiff_t loan = loan_find(&live, op->loan_id);
            bool has_child = false;
            for (size_t child = 0; child < live.loan_count; child++)
                if (live.loans[child].parent == op->loan_id)
                    has_child = true;
            if (!op->loan_id || loan < 0 || has_child) {
                v->result = (QttAnfVerification){
                    QTT_ANF_INVALID_LOAN, index, i};
                break;
            }
            QttAnfLoanId parent_id = live.loans[(size_t)loan].parent;
            bool restore_parent =
                live.loans[(size_t)loan].suspends_parent;
            live.loans[(size_t)loan] =
                live.loans[--live.loan_count];
            if (restore_parent) {
                ptrdiff_t parent = loan_find(&live, parent_id);
                if (parent < 0) {
                    v->result = (QttAnfVerification){
                        QTT_ANF_INVALID_LOAN, index, i};
                    break;
                }
                live.loans[(size_t)parent].suspended = false;
            }
        } else if (op->kind == QTT_ANF_CALL &&
                   op->result_mode == QTT_RESULT_OWNED) {
            if (live_find(&live, op->result_resource) >= 0 ||
                !declaration_add(v, op->result_resource,
                                 op->result_type,
                                 op->representation, true) ||
                !live_push(&live, op->result_resource)) {
                v->result = (QttAnfVerification){
                    QTT_ANF_INVALID_RESOURCE, index, i};
                break;
            }
        }
    }
    if (v->result.error == QTT_ANF_VALID) {
        QttAnfTerminator *term = &block->terminator;
        if (!value_defined(defined, defined_count, term->value) &&
            term->kind != QTT_ANF_JUMP)
            v->result = (QttAnfVerification){
                QTT_ANF_UNDEFINED_VALUE,index,block->instruction_count};
        else if (term->kind == QTT_ANF_RETURN) {
            ptrdiff_t returned = value_index(
                defined, defined_count, term->value);
            if (returned >= 0 && defined_loans[(size_t)returned] &&
                loan_find(&live, defined_loans[(size_t)returned]) < 0)
                v->result = (QttAnfVerification){
                    QTT_ANF_INVALID_LOAN, index,
                    block->instruction_count};
            else if (live.loan_count)
                v->result = (QttAnfVerification){
                    QTT_ANF_INVALID_LOAN, index,
                    block->instruction_count};
            for (size_t resource = 0;
                 v->result.error == QTT_ANF_VALID &&
                 resource < live.count; resource++) {
                ptrdiff_t declaration =
                    declaration_find(v, live.items[resource]);
                if (declaration >= 0 &&
                    v->declarations[(size_t)declaration].owned) {
                    v->result = (QttAnfVerification){
                        QTT_ANF_RESOURCE_LEAK, index,
                        block->instruction_count};
                    break;
                }
            }
        }
        if (v->result.error == QTT_ANF_VALID &&
            term->kind == QTT_ANF_JUMP)
            propagate(v,index,term->target,defined,defined_count,defined_types,
                      defined_loans,
                      term->arguments,term->argument_count,&live);
        else if (v->result.error == QTT_ANF_VALID &&
                 term->kind == QTT_ANF_BRANCH) {
            if (value_type(defined, defined_types, defined_count,
                           term->value) != QTT_ANF_TYPE_BOOL)
                v->result = (QttAnfVerification){
                    QTT_ANF_NON_BOOLEAN_BRANCH, index,
                    block->instruction_count};
            else {
                propagate(v,index,term->target,defined,defined_count,
                          defined_types,defined_loans,term->arguments,
                          term->argument_count,&live);
                propagate(v,index,term->else_target,defined,defined_count,
                          defined_types,defined_loans,term->else_arguments,
                          term->else_argument_count,&live);
            }
        }
    }
    free(live.items);
    free(live.evacuated);
    free(live.loans);
    free(defined_by);
    free(defined_resources);
    free(defined_loans);
    free(defined_types);
    free(defined);
}

QttAnfVerification qtt_anf_verify(const QttAnfProgram *program) {
    QttAnfVerification invalid = {QTT_ANF_INVALID_BLOCK, 0, 0};
    if (!program || !program->blocks || !program->block_count ||
        program->entry >= program->block_count) return invalid;
    for (size_t i = 0; i < program->mutable_place_count; i++) {
        const QttAnfPlaceDeclaration *place =
            &program->mutable_places[i];
        if (!place->place.root.module_id ||
            !place->place.root.binder_id ||
            place->type == QTT_ANF_TYPE_UNKNOWN)
            return (QttAnfVerification){
                QTT_ANF_INVALID_PLACE, 0, i};
        for (size_t j = 0; j < i; j++)
            if (qtt_place_equal(
                    program->mutable_places[j].place,
                    place->place))
                return (QttAnfVerification){
                    QTT_ANF_INVALID_PLACE, 0, i};
    }
    Verifier verifier = {.program = program,
        .result = {.error = QTT_ANF_VALID}};
    verifier.states = calloc(program->block_count, sizeof(*verifier.states));
    if (!verifier.states)
        return (QttAnfVerification){QTT_ANF_OUT_OF_MEMORY,0,0};
    LiveSet empty = {0};
    for (size_t i = 0; i < program->initial_resource_count; i++) {
        if (!live_push(&empty, program->initial_resources[i])) {
            free(verifier.states);
            return (QttAnfVerification){QTT_ANF_OUT_OF_MEMORY,0,0};
        }
        declaration_add(
            &verifier, program->initial_resources[i],
            QTT_ANF_TYPE_UNKNOWN,
            program->initial_representations
                ? program->initial_representations[i]
                : QTT_REP_UNKNOWN,
            program->initial_resource_owned
                ? program->initial_resource_owned[i] : true);
    }
    verify_block(&verifier, program->entry, &empty);
    free(empty.items);
    free(empty.evacuated);
    free(empty.loans);
    for (size_t i = 0; i < program->block_count; i++) {
        free(verifier.states[i].incoming.items);
        free(verifier.states[i].incoming.evacuated);
        free(verifier.states[i].incoming.loans);
    }
    free(verifier.declarations);
    free(verifier.states);
    return verifier.result;
}

typedef struct {
    QttAnfValue *values;
    QttAnfLoanId *loans;
    size_t count;
} LoanValueMap;

static ptrdiff_t loan_value_find(const LoanValueMap *map, QttAnfValue value) {
    for (size_t i = 0; i < map->count; i++)
        if (map->values[i] == value) return (ptrdiff_t)i;
    return -1;
}

static bool loan_value_map_build(
    const QttAnfBlock *block, LoanValueMap *map) {
    size_t capacity = block->parameter_count + block->instruction_count;
    map->values = calloc(capacity ? capacity : 1, sizeof(*map->values));
    map->loans = calloc(capacity ? capacity : 1, sizeof(*map->loans));
    if (!map->values || !map->loans) {
        free(map->values);
        free(map->loans);
        *map = (LoanValueMap){0};
        return false;
    }
    for (size_t i = 0; i < block->parameter_count; i++) {
        map->values[map->count] = block->parameters[i];
        map->loans[map->count++] = block->parameter_loans
            ? block->parameter_loans[i] : 0;
    }
    for (size_t i = 0; i < block->instruction_count; i++) {
        const QttAnfInstruction *op = &block->instructions[i];
        if (!op->result) continue;
        QttAnfLoanId provenance = 0;
        if (op->kind == QTT_ANF_BORROW)
            provenance = op->loan_id;
        else if (op->kind == QTT_ANF_ALIAS) {
            ptrdiff_t source = loan_value_find(map, op->operand);
            if (source >= 0) provenance = map->loans[(size_t)source];
        }
        map->values[map->count] = op->result;
        map->loans[map->count++] = provenance;
    }
    return true;
}

static void loan_value_map_free(LoanValueMap *map) {
    free(map->values);
    free(map->loans);
    *map = (LoanValueMap){0};
}

static bool infer_edge_provenance(
    QttAnfProgram *program, bool **known, const LoanValueMap *source,
    size_t target, const QttAnfValue *arguments, size_t count,
    bool *changed) {
    if (target >= program->block_count ||
        count != program->blocks[target].parameter_count)
        return false;
    QttAnfBlock *destination = &program->blocks[target];
    for (size_t i = 0; i < count; i++) {
        ptrdiff_t value = loan_value_find(source, arguments[i]);
        if (value < 0) return false;
        QttAnfLoanId loan = source->loans[(size_t)value];
        if (!known[target][i]) {
            destination->parameter_loans[i] = loan;
            known[target][i] = true;
            *changed = true;
        } else if (destination->parameter_loans[i] != loan)
            return false;
    }
    return true;
}

static bool cfg_mark_reachable(
    const QttAnfProgram *program, size_t source, bool reverse,
    bool *reachable) {
    size_t *work = malloc(program->block_count * sizeof(*work));
    if (!work) return false;
    size_t count = 0;
    reachable[source] = true;
    work[count++] = source;
    while (count) {
        size_t current = work[--count];
        for (size_t block = 0; block < program->block_count; block++) {
            const QttAnfTerminator *term = &program->blocks[block].terminator;
            bool edge = term->kind == QTT_ANF_JUMP
                ? term->target == current
                : term->kind == QTT_ANF_BRANCH &&
                    (term->target == current || term->else_target == current);
            size_t next = block;
            if (!reverse) {
                term = &program->blocks[current].terminator;
                if (block == 0) {
                    /* handled below */
                }
                edge = false;
                if (term->kind == QTT_ANF_JUMP && term->target == block)
                    edge = true;
                else if (term->kind == QTT_ANF_BRANCH &&
                         (term->target == block || term->else_target == block))
                    edge = true;
                next = block;
            }
            if (edge && next < program->block_count && !reachable[next]) {
                reachable[next] = true;
                work[count++] = next;
            }
        }
    }
    free(work);
    return true;
}

static size_t cfg_successors(
    const QttAnfProgram *program, size_t block, size_t successors[2]) {
    const QttAnfTerminator *term = &program->blocks[block].terminator;
    if (term->kind == QTT_ANF_JUMP) {
        successors[0] = term->target;
        return 1;
    }
    if (term->kind == QTT_ANF_BRANCH) {
        successors[0] = term->target;
        successors[1] = term->else_target;
        return 2;
    }
    return 0;
}

static bool insert_loan_end(
    QttAnfBlock *block, size_t index, QttAnfLoanId loan) {
    QttAnfInstruction *grown = realloc(
        block->instructions,
        (block->instruction_count + 1) * sizeof(*grown));
    if (!grown) return false;
    block->instructions = grown;
    memmove(&grown[index + 1], &grown[index],
            (block->instruction_count - index) * sizeof(*grown));
    grown[index] = qtt_anf_end_borrow(loan);
    block->instruction_count++;
    return true;
}

static QttAnfLoanId program_loan_parent(
    const QttAnfProgram *program, QttAnfLoanId loan) {
    for (size_t block = 0; block < program->block_count; block++)
        for (size_t i = 0;
             i < program->blocks[block].instruction_count; i++) {
            const QttAnfInstruction *op =
                &program->blocks[block].instructions[i];
            if (op->kind == QTT_ANF_BORROW && op->loan_id == loan)
                return op->parent_loan_id;
        }
    return 0;
}

static bool program_has_loan_end(
    const QttAnfProgram *program, QttAnfLoanId loan) {
    for (size_t block = 0; block < program->block_count; block++)
        for (size_t i = 0;
             i < program->blocks[block].instruction_count; i++)
            if (program->blocks[block].instructions[i].kind ==
                    QTT_ANF_END_BORROW &&
                program->blocks[block].instructions[i].loan_id == loan)
                return true;
    return false;
}

static bool program_has_unended_child(
    const QttAnfProgram *program, QttAnfLoanId parent) {
    for (size_t block = 0; block < program->block_count; block++)
        for (size_t i = 0;
             i < program->blocks[block].instruction_count; i++) {
            const QttAnfInstruction *op =
                &program->blocks[block].instructions[i];
            if (op->kind == QTT_ANF_BORROW &&
                op->parent_loan_id == parent &&
                !program_has_loan_end(program, op->loan_id))
                return true;
        }
    return false;
}

QttAnfVerification qtt_anf_infer_loan_regions(QttAnfProgram *program) {
    QttAnfVerification invalid = {QTT_ANF_INVALID_LOAN, 0, 0};
    if (!program || !program->blocks || !program->block_count ||
        program->entry >= program->block_count)
        return (QttAnfVerification){QTT_ANF_INVALID_BLOCK, 0, 0};
    bool **known = calloc(program->block_count, sizeof(*known));
    bool *reachable = calloc(program->block_count, sizeof(*reachable));
    if (!known || !reachable) {
        free(known); free(reachable);
        return (QttAnfVerification){QTT_ANF_OUT_OF_MEMORY, 0, 0};
    }
    for (size_t block = 0; block < program->block_count; block++) {
        QttAnfBlock *item = &program->blocks[block];
        known[block] = calloc(
            item->parameter_count ? item->parameter_count : 1,
            sizeof(**known));
        if (!known[block]) {
            invalid.error = QTT_ANF_OUT_OF_MEMORY;
            goto cleanup;
        }
        if (item->parameter_count && !item->parameter_loans) {
            item->parameter_loans = calloc(
                item->parameter_count, sizeof(*item->parameter_loans));
            if (!item->parameter_loans) {
                invalid.error = QTT_ANF_OUT_OF_MEMORY;
                goto cleanup;
            }
        } else if (item->parameter_loans)
            for (size_t i = 0; i < item->parameter_count; i++)
                known[block][i] = true;
    }
    if (!cfg_mark_reachable(program, program->entry, false, reachable)) {
        invalid.error = QTT_ANF_OUT_OF_MEMORY;
        goto cleanup;
    }
    for (size_t i = 0; i < program->blocks[program->entry].parameter_count; i++)
        known[program->entry][i] = true;
    for (size_t iteration = 0; iteration <= program->block_count; iteration++) {
        bool changed = false;
        for (size_t block = 0; block < program->block_count; block++) {
            if (!reachable[block]) continue;
            bool inputs_known = true;
            for (size_t i = 0;
                 i < program->blocks[block].parameter_count; i++)
                inputs_known = inputs_known && known[block][i];
            if (!inputs_known) continue;
            LoanValueMap map = {0};
            if (!loan_value_map_build(&program->blocks[block], &map)) {
                invalid.error = QTT_ANF_OUT_OF_MEMORY;
                goto cleanup;
            }
            const QttAnfTerminator *term = &program->blocks[block].terminator;
            bool valid = true;
            if (term->kind == QTT_ANF_JUMP)
                valid = infer_edge_provenance(
                    program, known, &map, term->target,
                    term->arguments, term->argument_count, &changed);
            else if (term->kind == QTT_ANF_BRANCH)
                valid = infer_edge_provenance(
                    program, known, &map, term->target,
                    term->arguments, term->argument_count, &changed) &&
                    infer_edge_provenance(
                    program, known, &map, term->else_target,
                    term->else_arguments, term->else_argument_count, &changed);
            loan_value_map_free(&map);
            if (!valid) goto cleanup;
        }
        if (!changed) break;
    }
    for (size_t block = 0; block < program->block_count; block++)
        if (reachable[block])
            for (size_t i = 0; i < program->blocks[block].parameter_count; i++)
                if (!known[block][i]) goto cleanup;

    size_t n = program->block_count;
    bool *can_exit = calloc(n, sizeof(*can_exit));
    bool *postdom = calloc(n * n, sizeof(*postdom));
    if (!can_exit || !postdom) {
        free(can_exit); free(postdom);
        invalid.error = QTT_ANF_OUT_OF_MEMORY;
        goto cleanup;
    }
    for (size_t block = 0; block < n; block++)
        if (program->blocks[block].terminator.kind == QTT_ANF_RETURN &&
            !cfg_mark_reachable(program, block, true, can_exit)) {
            free(can_exit); free(postdom);
            invalid.error = QTT_ANF_OUT_OF_MEMORY;
            goto cleanup;
        }
    for (size_t block = 0; block < n; block++)
        for (size_t candidate = 0; candidate < n; candidate++)
            postdom[block * n + candidate] =
                program->blocks[block].terminator.kind == QTT_ANF_RETURN
                    ? block == candidate : can_exit[block];
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t block = 0; block < n; block++) {
            size_t successors[2];
            size_t count = cfg_successors(program, block, successors);
            if (!count || !can_exit[block]) continue;
            for (size_t candidate = 0; candidate < n; candidate++) {
                bool value = candidate == block;
                if (!value) {
                    value = true;
                    for (size_t edge = 0; edge < count; edge++)
                        value = value && successors[edge] < n &&
                            postdom[successors[edge] * n + candidate];
                }
                if (postdom[block * n + candidate] != value) {
                    postdom[block * n + candidate] = value;
                    changed = true;
                }
            }
        }
    }

    size_t loan_budget = 1;
    for (size_t block = 0; block < n; block++)
        loan_budget += program->blocks[block].instruction_count;
    for (size_t dependency_pass = 0;
         dependency_pass <= loan_budget; dependency_pass++) {
      bool endpoint_progress = false;
      bool endpoint_pending = false;
      for (size_t definition_block = 0; definition_block < n; definition_block++)
        for (size_t definition_index = 0;
             definition_index < program->blocks[definition_block].instruction_count;
             definition_index++) {
            QttAnfInstruction *definition =
                &program->blocks[definition_block].instructions[definition_index];
            if (definition->kind != QTT_ANF_BORROW || !definition->loan_id)
                continue;
            QttAnfLoanId loan = definition->loan_id;
            bool has_end = false;
            bool *use_blocks = calloc(n, sizeof(*use_blocks));
            size_t *last_use = calloc(n, sizeof(*last_use));
            if (!use_blocks || !last_use) {
                free(use_blocks); free(last_use);
                free(can_exit); free(postdom);
                invalid.error = QTT_ANF_OUT_OF_MEMORY;
                goto cleanup;
            }
            for (size_t block = 0; block < n; block++) {
                LoanValueMap map = {0};
                if (!loan_value_map_build(&program->blocks[block], &map)) {
                    free(use_blocks); free(last_use);
                    free(can_exit); free(postdom);
                    invalid.error = QTT_ANF_OUT_OF_MEMORY;
                    goto cleanup;
                }
                for (size_t i = 0;
                     i < program->blocks[block].instruction_count; i++) {
                    const QttAnfInstruction *op =
                        &program->blocks[block].instructions[i];
                    if (op->kind == QTT_ANF_END_BORROW &&
                        op->loan_id == loan)
                        has_end = true;
                    ptrdiff_t operand = loan_value_find(&map, op->operand);
                    bool uses = operand >= 0 &&
                        map.loans[(size_t)operand] == loan;
                    if (op->kind == QTT_ANF_WRITE_PLACE && op->loan_id == loan)
                        uses = true;
                    if (op->kind == QTT_ANF_BORROW &&
                        op->parent_loan_id == loan)
                        uses = true;
                    if (op->kind == QTT_ANF_END_BORROW &&
                        program_loan_parent(program, op->loan_id) == loan)
                        uses = true;
                    for (size_t argument = 0;
                         op->kind == QTT_ANF_CALL &&
                         argument < op->call_argument_count; argument++)
                        if (op->call_arguments[argument].loan_id == loan)
                            uses = true;
                    if (uses) {
                        use_blocks[block] = true;
                        last_use[block] = i + 1;
                    }
                }
                loan_value_map_free(&map);
            }
            if (!has_end) {
                endpoint_pending = true;
                if (program_has_unended_child(program, loan)) {
                    free(use_blocks);
                    free(last_use);
                    continue;
                }
                bool any_use = false;
                size_t candidate = n;
                size_t best = 0;
                for (size_t block = 0; block < n; block++)
                    any_use = any_use || use_blocks[block];
                if (!any_use) {
                    candidate = definition_block;
                    last_use[candidate] = definition_index + 1;
                } else for (size_t possible = 0; possible < n; possible++) {
                    bool common = can_exit[possible];
                    for (size_t use = 0; use < n && common; use++)
                        if (use_blocks[use])
                            common = postdom[use * n + possible];
                    if (!common) continue;
                    size_t score = 0;
                    for (size_t p = 0; p < n; p++)
                        score += postdom[possible * n + p] ? 1 : 0;
                    if (candidate == n || score > best) {
                        candidate = possible;
                        best = score;
                    }
                }
                size_t insertion = candidate < n ? last_use[candidate] : 0;
                if (candidate >= n ||
                    !insert_loan_end(
                        &program->blocks[candidate], insertion, loan)) {
                    free(use_blocks); free(last_use);
                    free(can_exit); free(postdom);
                    if (candidate < n) invalid.error = QTT_ANF_OUT_OF_MEMORY;
                    goto cleanup;
                }
                endpoint_progress = true;
            }
            free(use_blocks);
            free(last_use);
        }
      if (!endpoint_pending) break;
      if (!endpoint_progress) {
          free(can_exit);
          free(postdom);
          goto cleanup;
      }
    }
    free(can_exit);
    free(postdom);
    invalid = qtt_anf_verify(program);

cleanup:
    for (size_t block = 0; block < program->block_count; block++)
        free(known[block]);
    free(known);
    free(reachable);
    return invalid;
}

typedef struct {
    QttAnfValue name;
    QttAnfRuntimeValue value;
} RuntimeBinding;

typedef struct {
    QttCoreVar var;
    QttRepresentation representation;
    QttAnfRuntimeValue payload;
} RuntimeResource;

static bool runtime_lookup(const RuntimeBinding *bindings, size_t count,
                           QttAnfValue name, QttAnfRuntimeValue *value) {
    for (size_t i = count; i > 0; i--) {
        if (bindings[i - 1].name == name) {
            *value = bindings[i - 1].value;
            return true;
        }
    }
    return false;
}

static ptrdiff_t runtime_resource_find(const RuntimeResource *resources,
                                       size_t count, QttCoreVar var) {
    for (size_t i = 0; i < count; i++)
        if (qtt_core_var_equal(resources[i].var, var)) return (ptrdiff_t)i;
    return -1;
}

QttAnfEvaluation qtt_anf_evaluate(const QttAnfProgram *program) {
    QttAnfEvaluation out = {0};
    out.verification = qtt_anf_verify(program);
    if (out.verification.error != QTT_ANF_VALID) {
        out.error = QTT_ANF_EVAL_INVALID_PROGRAM;
        return out;
    }

    size_t value_capacity = 1;
    size_t resource_capacity = 1;
    for (size_t i = 0; i < program->block_count; i++) {
        size_t values = program->blocks[i].parameter_count +
                        program->blocks[i].instruction_count;
        if (values > value_capacity) value_capacity = values;
        resource_capacity += program->blocks[i].instruction_count;
    }
    RuntimeBinding *bindings = calloc(value_capacity, sizeof(*bindings));
    RuntimeResource *resources =
        calloc(resource_capacity, sizeof(*resources));
    QttAnfRuntimeValue *arguments =
        calloc(value_capacity, sizeof(*arguments));
    if (!bindings || !resources || !arguments) {
        free(bindings); free(resources); free(arguments);
        out.error = QTT_ANF_EVAL_OUT_OF_MEMORY;
        return out;
    }

    size_t block_index = program->entry;
    size_t binding_count = 0;
    size_t resource_count = 0;
    size_t steps = 0;
    const size_t step_limit = 1000000;
    while (steps++ < step_limit) {
        const QttAnfBlock *block = &program->blocks[block_index];
        for (size_t i = 0; i < block->instruction_count; i++) {
            const QttAnfInstruction *op = &block->instructions[i];
            if (op->kind == QTT_ANF_CONST_NUMBER) {
                bindings[binding_count++] = (RuntimeBinding){
                    op->result, {.kind = QTT_ANF_VALUE_NUMBER,
                                 .number = op->number}};
            } else if (op->kind == QTT_ANF_CONST_BOOL) {
                bindings[binding_count++] = (RuntimeBinding){
                    op->result, {.kind = QTT_ANF_VALUE_BOOL,
                                 .boolean = op->boolean}};
            } else if (op->kind == QTT_ANF_CONST_STRING) {
                bindings[binding_count++] = (RuntimeBinding){
                    op->result, {.kind = QTT_ANF_VALUE_STRING,
                                 .string = op->string}};
            } else if (op->kind == QTT_ANF_CONST_AGGREGATE) {
                bindings[binding_count++] = (RuntimeBinding){
                    op->result, {.kind = QTT_ANF_VALUE_UNIT}};
            } else if (op->kind == QTT_ANF_ALIAS) {
                QttAnfRuntimeValue value;
                runtime_lookup(bindings, binding_count, op->operand, &value);
                bindings[binding_count++] =
                    (RuntimeBinding){op->result, value};
            } else if (op->kind == QTT_ANF_WRITE_PLACE) {
                QttAnfRuntimeValue value;
                runtime_lookup(
                    bindings, binding_count, op->operand, &value);
                bindings[binding_count++] =
                    (RuntimeBinding){op->result, value};
            } else if (op->kind == QTT_ANF_REPLACE) {
                QttAnfRuntimeValue value;
                runtime_lookup(
                    bindings, binding_count, op->operand, &value);
                ptrdiff_t found = runtime_resource_find(
                    resources, resource_count, op->resource);
                if (found < 0) {
                    out.error = QTT_ANF_EVAL_INVALID_PROGRAM;
                    goto evaluation_done;
                }
                resources[(size_t)found].payload = value;
                out.heap.dropped++;
                out.heap.allocated++;
                bindings[binding_count++] =
                    (RuntimeBinding){op->result, value};
            } else if (op->kind == QTT_ANF_ALLOC) {
                QttAnfRuntimeValue payload = {.kind = QTT_ANF_VALUE_UNIT};
                if (op->operand)
                    runtime_lookup(bindings, binding_count,
                                   op->operand, &payload);
                resources[resource_count++] = (RuntimeResource){
                    op->resource, op->representation, payload};
                if (op->representation == QTT_REP_OWNED_HEAP) {
                    out.heap.allocated++;
                    out.heap.live_owned++;
                    if (out.heap.live_owned > out.heap.peak_live_owned)
                        out.heap.peak_live_owned = out.heap.live_owned;
                }
            } else if (op->kind == QTT_ANF_BORROW) {
                out.heap.borrows++;
                if (op->result) {
                    ptrdiff_t found = runtime_resource_find(
                        resources, resource_count, op->resource);
                    bindings[binding_count++] = (RuntimeBinding){
                        op->result, resources[(size_t)found].payload};
                }
            } else if (op->kind == QTT_ANF_MOVE_PLACE) {
                ptrdiff_t found = runtime_resource_find(
                    resources, resource_count, op->resource);
                if (found < 0) {
                    out.error = QTT_ANF_EVAL_INVALID_PROGRAM;
                    goto evaluation_done;
                }
                bindings[binding_count++] = (RuntimeBinding){
                    op->result, resources[(size_t)found].payload};
                out.heap.projected_moves++;
            } else if (op->kind == QTT_ANF_DROP ||
                       op->kind == QTT_ANF_MOVE) {
                ptrdiff_t found = runtime_resource_find(
                    resources, resource_count, op->resource);
                RuntimeResource removed = resources[(size_t)found];
                if (op->result)
                    bindings[binding_count++] = (RuntimeBinding){
                        op->result, removed.payload};
                resources[(size_t)found] = resources[--resource_count];
                if (removed.representation == QTT_REP_OWNED_HEAP) {
                    if (op->kind == QTT_ANF_MOVE)
                        out.heap.moved_out++;
                    else
                        out.heap.dropped++;
                    out.heap.live_owned--;
                }
            } else if (op->kind == QTT_ANF_ENV_PROJECT) {
                /*
                 * Standalone evaluation has no closure payload.  Reject
                 * explicitly instead of manufacturing a value while the
                 * verifier correctly accepts the ownership schedule.
                 */
                out.error = QTT_ANF_EVAL_UNSUPPORTED_ENVIRONMENT;
                goto evaluation_done;
            } else if (op->kind == QTT_ANF_CALL) {
                out.error = QTT_ANF_EVAL_UNSUPPORTED_CALL;
                goto evaluation_done;
            }
        }

        const QttAnfTerminator *term = &block->terminator;
        if (term->kind == QTT_ANF_RETURN) {
            runtime_lookup(bindings, binding_count, term->value, &out.value);
            out.error = QTT_ANF_EVAL_OK;
            break;
        }
        if (term->kind == QTT_ANF_BRANCH) {
            QttAnfRuntimeValue condition;
            runtime_lookup(bindings, binding_count, term->value, &condition);
            if (condition.kind != QTT_ANF_VALUE_BOOL) {
                out.error = QTT_ANF_EVAL_NON_BOOLEAN_CONDITION;
                break;
            }
            QttAnfValue *selected_arguments = condition.boolean
                ? term->arguments : term->else_arguments;
            size_t selected_count = condition.boolean
                ? term->argument_count : term->else_argument_count;
            for (size_t i = 0; i < selected_count; i++)
                runtime_lookup(bindings, binding_count,
                               selected_arguments[i], &arguments[i]);
            block_index =
                condition.boolean ? term->target : term->else_target;
            const QttAnfBlock *destination =
                &program->blocks[block_index];
            binding_count = 0;
            for (size_t i = 0; i < destination->parameter_count; i++)
                bindings[binding_count++] = (RuntimeBinding){
                    destination->parameters[i], arguments[i]};
            continue;
        }

        for (size_t i = 0; i < term->argument_count; i++)
            runtime_lookup(bindings, binding_count, term->arguments[i],
                           &arguments[i]);
        block_index = term->target;
        const QttAnfBlock *destination = &program->blocks[block_index];
        binding_count = 0;
        for (size_t i = 0; i < destination->parameter_count; i++)
            bindings[binding_count++] = (RuntimeBinding){
                destination->parameters[i], arguments[i]};
    }
    if (steps > step_limit) out.error = QTT_ANF_EVAL_STEP_LIMIT;
evaluation_done:
    free(arguments);
    free(resources);
    free(bindings);
    return out;
}

typedef struct AnfLowerBinding {
    QttCoreVar var;
    QttAnfValue value;
    QttAnfType type;
    size_t remaining;
    QttRepresentation representation;
    bool owns_resource;
    /* Statically known capture-free code behind a lexical callable alias. */
    const QttCoreNode *callable;
    struct AnfLowerBinding *parent;
} AnfLowerBinding;

typedef struct AnfLowerLoan {
    QttCoreVar binding;
    QttPlace place;
    QttLoanKind kind;
    QttAnfLoanId id;
    struct AnfLowerLoan *parent;
} AnfLowerLoan;

typedef struct {
    QttAnfProgram *program;
    size_t current;
    QttAnfValue next_value;
    QttAnfLowerError error;
    AnfLowerBinding *bindings;
    AnfLowerLoan *loans;
    const QttDemandCertificate *demand;
    const QttSignatureEnv *signatures;
    uint64_t module_id;
    uint64_t next_synthetic_binder;
    QttAnfLoanId next_loan_id;
    size_t next_branch_ordinal;
    size_t next_transition_ordinal;
    size_t next_call_ordinal;
    size_t next_loan_ordinal;
    QttControlPathStep *control_path;
    size_t control_path_count;
    size_t control_path_capacity;
} AnfLowering;

static AnfLowerLoan *lower_find_loan(
    AnfLowering *lowering, QttCoreVar binding) {
    for (AnfLowerLoan *loan = lowering->loans; loan; loan = loan->parent)
        if (qtt_core_var_equal(loan->binding, binding)) return loan;
    return NULL;
}

static AnfLowerLoan *lower_find_covering_loan(
    AnfLowering *lowering, QttPlace place) {
    for (AnfLowerLoan *loan = lowering->loans; loan; loan = loan->parent)
        if (qtt_place_contains(loan->place, place)) return loan;
    return NULL;
}

static AnfLowerBinding *lower_find_binding(AnfLowering *lowering,
                                           QttCoreVar var) {
    for (AnfLowerBinding *binding = lowering->bindings;
         binding; binding = binding->parent)
        if (qtt_core_var_equal(binding->var, var)) return binding;
    return NULL;
}

static bool lower_declare_mutable_place(
    AnfLowering *lowering, QttPlace place, QttAnfType type) {
    const QttAnfPlaceDeclaration *existing =
        mutable_place_find(lowering->program, place);
    if (existing) return existing->type == type;
    size_t count = lowering->program->mutable_place_count + 1;
    QttAnfPlaceDeclaration *grown = realloc(
        lowering->program->mutable_places,
        count * sizeof(*grown));
    if (!grown) {
        lowering->error = QTT_ANF_LOWER_OUT_OF_MEMORY;
        return false;
    }
    lowering->program->mutable_places = grown;
    grown[count - 1] = (QttAnfPlaceDeclaration){
        .place = place, .type = type};
    lowering->program->mutable_place_count = count;
    return true;
}

typedef struct {
    AnfLowerBinding **bindings;
    size_t *then_remaining;
    size_t *else_remaining;
    size_t *join_remaining;
    QttAnfValue *entry_values;
    QttAnfValue *then_values;
    QttAnfValue *else_values;
} LowerBranchState;

static void lower_branch_state_free(LowerBranchState *state) {
    if (!state) return;
    free(state->bindings);
    free(state->then_remaining);
    free(state->else_remaining);
    free(state->join_remaining);
    free(state->entry_values);
    free(state->then_values);
    free(state->else_values);
    memset(state, 0, sizeof(*state));
}

static QttAnfType lower_core_type(const QttCoreNode *core) {
    if (core && core->type && core->type->kind == TYPE_LAYOUT)
        return QTT_ANF_TYPE_AGGREGATE;
    if (!core) return QTT_ANF_TYPE_UNKNOWN;
    if (core->type) {
        switch (core->type->kind) {
        case TYPE_BOOL: return QTT_ANF_TYPE_BOOL;
        case TYPE_FINITE_SET:
            /*
             * The source type checker represents the prelude Bool data type
             * as the closed finite set {False, True}.  Ownership ANF has a
             * dedicated control type for exactly that set; other finite
             * enums still require a general immediate-scalar ANF type.
             */
            if (core->type->finite_name &&
                strcmp(core->type->finite_name, "Bool") == 0)
                return QTT_ANF_TYPE_BOOL;
            break;
        case TYPE_STRING: return QTT_ANF_TYPE_STRING;
        case TYPE_UNIT: case TYPE_NIL: return QTT_ANF_TYPE_UNIT;
        case TYPE_INT: case TYPE_FLOAT: case TYPE_F32:
        case TYPE_I8: case TYPE_U8: case TYPE_I16: case TYPE_U16:
        case TYPE_I32: case TYPE_U32: case TYPE_I64: case TYPE_U64:
        case TYPE_I128: case TYPE_U128: case TYPE_INT_ARBITRARY:
        case TYPE_F80:
            return QTT_ANF_TYPE_NUMBER;
        default: break;
        }
    }
    if (core->kind == QTT_CORE_LITERAL && core->literal.source) {
        if (core->literal.source->type == AST_NUMBER)
            return QTT_ANF_TYPE_NUMBER;
        if (core->literal.source->type == AST_STRING)
            return QTT_ANF_TYPE_STRING;
    }
    if (core->kind == QTT_CORE_GLOBAL && core->global.name &&
        (!strcmp(core->global.name, "True") ||
         !strcmp(core->global.name, "False")))
        return QTT_ANF_TYPE_BOOL;
    /*
     * The legacy source normalizer may leave the set! wrapper unannotated
     * even though its checked replacement value is typed.  Core WRITE
     * evaluates to that value, so retain the operational type at the QTT
     * boundary instead of rejecting branch-local source mutation.
     */
    if (core->kind == QTT_CORE_WRITE)
        return lower_core_type(core->write.value);
    if (core->kind == QTT_CORE_BORROW)
        return lower_core_type(core->borrow.body);
    if (core->kind == QTT_CORE_IF)
        return lower_core_type(core->conditional.then_branch);
    if (core->kind == QTT_CORE_LET)
        return lower_core_type(core->let.body);
    if (core->kind == QTT_CORE_SEQUENCE && core->sequence.count)
        return lower_core_type(
            core->sequence.items[core->sequence.count - 1]);
    return QTT_ANF_TYPE_UNKNOWN;
}

static size_t lower_add_block(AnfLowering *lowering) {
    size_t index = lowering->program->block_count;
    QttAnfBlock *blocks = realloc(
        lowering->program->blocks, (index + 1) * sizeof(*blocks));
    if (!blocks) {
        lowering->error = QTT_ANF_LOWER_OUT_OF_MEMORY;
        return 0;
    }
    lowering->program->blocks = blocks;
    blocks[index] = (QttAnfBlock){0};
    lowering->program->block_count++;
    return index;
}

static bool lower_ownership_instruction(
    const QttAnfInstruction *instruction) {
    QttAnfInstructionKind kind = instruction->kind;
    return kind == QTT_ANF_ALLOC ||
           (kind == QTT_ANF_BORROW && !instruction->loan_id) ||
           kind == QTT_ANF_MOVE || kind == QTT_ANF_MOVE_PLACE ||
           kind == QTT_ANF_DROP || kind == QTT_ANF_REPLACE;
}

static bool lower_emit(AnfLowering *lowering, QttAnfInstruction instruction) {
    bool ownership = lower_ownership_instruction(&instruction);
    bool call = instruction.kind == QTT_ANF_CALL;
    if (ownership) {
        instruction.has_semantic_transition_identity = true;
        instruction.semantic_transition_ordinal =
            lowering->next_transition_ordinal++;
    }
    if (call) {
        instruction.has_semantic_call_identity = true;
        instruction.semantic_call_ordinal =
            lowering->next_call_ordinal++;
    }
    if (ownership || call) {
        if (lowering->control_path_count) {
            instruction.control_path = malloc(
                lowering->control_path_count *
                sizeof(*instruction.control_path));
            if (!instruction.control_path) {
                lowering->error = QTT_ANF_LOWER_OUT_OF_MEMORY;
                return false;
            }
            memcpy(
                instruction.control_path, lowering->control_path,
                lowering->control_path_count *
                sizeof(*instruction.control_path));
            instruction.control_path_count =
                lowering->control_path_count;
        }
    }
    QttAnfBlock *block = &lowering->program->blocks[lowering->current];
    QttAnfInstruction *instructions = realloc(
        block->instructions,
        (block->instruction_count + 1) * sizeof(*instructions));
    if (!instructions) {
        free(instruction.control_path);
        lowering->error = QTT_ANF_LOWER_OUT_OF_MEMORY;
        return false;
    }
    block->instructions = instructions;
    instructions[block->instruction_count++] = instruction;
    return true;
}

static bool lower_control_push(
    AnfLowering *lowering, size_t branch_ordinal, bool else_arm) {
    if (lowering->control_path_count ==
        lowering->control_path_capacity) {
        size_t next = lowering->control_path_capacity
            ? lowering->control_path_capacity * 2 : 4;
        QttControlPathStep *grown = realloc(
            lowering->control_path,
            next * sizeof(*grown));
        if (!grown) {
            lowering->error = QTT_ANF_LOWER_OUT_OF_MEMORY;
            return false;
        }
        lowering->control_path = grown;
        lowering->control_path_capacity = next;
    }
    lowering->control_path[
        lowering->control_path_count++] =
        (QttControlPathStep){
            .branch_ordinal = branch_ordinal,
            .else_arm = else_arm,
        };
    return true;
}

static void lower_control_pop(AnfLowering *lowering) {
    if (lowering->control_path_count)
        lowering->control_path_count--;
}

static QttAnfValue lower_node(AnfLowering *lowering,
                              const QttCoreNode *core,
                              bool result_escapes);

static QttAnfValue lower_direct_call(AnfLowering *lowering,
                                     const QttCoreNode *core,
                                     bool result_escapes) {
    const QttCoreNode *callee = core->apply.callee;
    QttSignatureError signature_error = QTT_SIGNATURE_OK;
    QttFunctionSignature *signature =
        qtt_signature_derive(callee, &signature_error);
    if (!signature) {
        lowering->error = signature_error == QTT_SIGNATURE_OUT_OF_MEMORY
            ? QTT_ANF_LOWER_OUT_OF_MEMORY
            : QTT_ANF_LOWER_UNSUPPORTED_CORE;
        return 0;
    }
    if (signature->parameter_count != core->apply.argument_count) {
        qtt_signature_free(signature);
        lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
        return 0;
    }
    for (size_t i = 0; i < signature->parameter_count; i++) {
        QttOwnershipMode mode = signature->parameters[i].mode;
        if (signature->parameters[i].representation ==
                QTT_REP_OWNED_HEAP &&
            (mode == QTT_OWNERSHIP_BORROWED ||
             mode == QTT_OWNERSHIP_SHARED)) {
            qtt_signature_free(signature);
            lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
            return 0;
        }
    }

    size_t count = signature->parameter_count;
    QttCallArgument *arguments =
        calloc(count ? count : 1, sizeof(*arguments));
    QttAnfValue *values = calloc(count ? count : 1, sizeof(*values));
    AnfLowerBinding *bindings =
        calloc(count ? count : 1, sizeof(*bindings));
    if (!arguments || !values || !bindings) {
        free(arguments); free(values); free(bindings);
        qtt_signature_free(signature);
        lowering->error = QTT_ANF_LOWER_OUT_OF_MEMORY;
        return 0;
    }

    size_t active = 0;
    for (size_t i = 0; i < count; i++) {
        const QttParameterContract *parameter = &signature->parameters[i];
        bool consumed = parameter->mode == QTT_OWNERSHIP_CONSUMED;
        values[i] = lower_node(
            lowering, core->apply.arguments[i], consumed);
        if (!values[i]) break;
        arguments[i] = (QttCallArgument){
            /* Signature inference has already zonked and interned the
             * parameter.  Validate against that canonical identity, exactly
             * as named-call lowering does, instead of manufacturing an
             * identity-less argument contract from the syntax node. */
            .type = parameter->type,
            .type_id = parameter->type_id,
            .representation = parameter->representation,
            .transfer = consumed &&
                    parameter->representation == QTT_REP_OWNED_HEAP
                ? QTT_CALL_MOVE : QTT_CALL_VALUE,
        };
        /*
         * Quantitative consumption is not itself an ownership capability.
         * Immediate, inline, and foreign values are transferred as SSA
         * values; only an owned heap representation enters the resource
         * state tracked by bind/move/drop.
         */
        if (consumed && parameter->representation == QTT_REP_OWNED_HEAP &&
            !lower_emit(lowering, qtt_anf_bind(
                parameter->var, parameter->representation, values[i])))
            break;
        bindings[active] = (AnfLowerBinding){
            .var = parameter->var,
            .value = values[i],
            .type = lower_core_type(core->apply.arguments[i]),
            .remaining = qtt_demand_occurrences(
                signature->demand, callee->lambda.body, parameter->var),
            .representation = parameter->representation,
            .owns_resource = consumed &&
                parameter->representation == QTT_REP_OWNED_HEAP,
            .parent = active ? &bindings[active - 1] : lowering->bindings,
        };
        active++;
    }

    QttCallPlan plan = {0};
    QttCallValidation validation = lowering->error == QTT_ANF_LOWER_OK
        ? qtt_call_plan(signature, arguments, count, &plan)
        : QTT_CALL_OUT_OF_MEMORY;
    QttAnfValue result = 0;
    if (validation == QTT_CALL_VALID) {
        AnfLowerBinding *outer = lowering->bindings;
        lowering->bindings = active ? &bindings[active - 1] : outer;
        result = lower_node(lowering, callee->lambda.body, result_escapes);
        lowering->bindings = outer;
    } else if (lowering->error == QTT_ANF_LOWER_OK) {
        lowering->error = validation == QTT_CALL_OUT_OF_MEMORY
            ? QTT_ANF_LOWER_OUT_OF_MEMORY
            : QTT_ANF_LOWER_UNSUPPORTED_CORE;
    }
    qtt_call_plan_free(&plan);
    free(arguments);
    free(values);
    free(bindings);
    qtt_signature_free(signature);
    return result;
}

static QttCoreVar lower_fresh_resource(AnfLowering *lowering) {
    return (QttCoreVar){
        .module_id = lowering->module_id,
        .binder_id = lowering->next_synthetic_binder++,
    };
}

static const QttAnfInstruction *lower_value_producer(
    const AnfLowering *lowering, QttAnfValue value) {
    if (!lowering || !lowering->program || !value ||
        lowering->current >= lowering->program->block_count)
        return NULL;
    const QttAnfBlock *block =
        &lowering->program->blocks[lowering->current];
    for (size_t i = block->instruction_count; i > 0; i--)
        if (block->instructions[i - 1].result == value)
            return &block->instructions[i - 1];
    return NULL;
}

static QttAnfValue lower_named_call(AnfLowering *lowering,
                                    const QttCoreNode *core,
                                    bool result_escapes) {
    QttCallableId callable = {0};
    QttFunctionSignature *signature = NULL;
    if (!lowering->signatures || !core->apply.callee->global.name ||
        !qtt_signature_env_resolve(
            lowering->signatures, lowering->module_id,
            core->apply.callee->global.name, &callable, &signature)) {
        lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
        return 0;
    }
    size_t count = core->apply.argument_count;
    if (count != signature->parameter_count) {
        lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
        return 0;
    }
    QttAnfCallOperand *operands =
        calloc(count ? count : 1, sizeof(*operands));
    QttCallArgument *arguments =
        calloc(count ? count : 1, sizeof(*arguments));
    QttCoreVar *drop_after_call =
        calloc(count ? count : 1, sizeof(*drop_after_call));
    QttAnfLoanId *end_after_call =
        calloc(count ? count : 1, sizeof(*end_after_call));
    size_t drop_count = 0;
    size_t end_count = 0;
    if (!operands || !arguments || !drop_after_call || !end_after_call) {
        free(operands);
        free(arguments);
        free(drop_after_call);
        free(end_after_call);
        lowering->error = QTT_ANF_LOWER_OUT_OF_MEMORY;
        return 0;
    }
    for (size_t i = 0; i < count; i++) {
        const QttParameterContract *parameter = &signature->parameters[i];
        QttCallTransfer transfer =
            qtt_call_transfer_for_parameter(parameter);
        QttAnfValue value = 0;
        QttCoreVar source_resource = {0};
        QttAnfLoanId loan_id = 0;
        if (transfer == QTT_CALL_BORROW) {
            loan_id = lowering->next_loan_id++;
            const QttCoreNode *argument = core->apply.arguments[i];
            AnfLowerBinding *binding = argument->kind == QTT_CORE_VAR
                ? lower_find_binding(lowering, argument->var) : NULL;
            if (binding && binding->representation == QTT_REP_OWNED_HEAP &&
                binding->remaining) {
                binding->remaining--;
                source_resource = binding->var;
                value = lowering->next_value++;
                QttAnfInstruction borrow = qtt_anf_borrow_loan(
                    value, source_resource, loan_id, QTT_LOAN_SHARED);
                borrow.result_type = lower_core_type(argument);
                if (borrow.result_type == QTT_ANF_TYPE_UNKNOWN)
                    borrow.result_type = binding->type;
                if (borrow.result_type == QTT_ANF_TYPE_UNKNOWN ||
                    !lower_emit(lowering, borrow))
                    break;
                if (!binding->remaining && binding->owns_resource)
                    drop_after_call[drop_count++] = binding->var;
            } else {
                QttAnfValue owned = lower_node(lowering, argument, true);
                if (!owned) break;
                source_resource = lower_fresh_resource(lowering);
                if (!lower_emit(lowering, qtt_anf_bind(
                        source_resource, QTT_REP_OWNED_HEAP, owned)))
                    break;
                value = lowering->next_value++;
                QttAnfInstruction borrow = qtt_anf_borrow_loan(
                    value, source_resource, loan_id, QTT_LOAN_SHARED);
                borrow.result_type = lower_core_type(argument);
                if (borrow.result_type == QTT_ANF_TYPE_UNKNOWN ||
                    !lower_emit(lowering, borrow))
                    break;
                drop_after_call[drop_count++] = source_resource;
            }
            end_after_call[end_count++] = loan_id;
        } else {
            value = lower_node(
                lowering, core->apply.arguments[i], true);
            if (!value) break;
        }
        if (transfer == QTT_CALL_MOVE) {
            QttAnfBlock *current =
                &lowering->program->blocks[lowering->current];
            QttAnfInstruction *producer =
                current->instruction_count
                    ? &current->instructions[
                          current->instruction_count - 1]
                    : NULL;
            if (producer && producer->kind == QTT_ANF_MOVE &&
                producer->result == value) {
                source_resource = producer->resource;
            } else if (producer && producer->kind == QTT_ANF_BORROW &&
                       producer->result == value) {
                /* A non-owning capability cannot satisfy a consuming call.
                 * ALLOC here would forge ownership of the same payload. */
                lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
                break;
            } else {
                QttCoreVar resource =
                    lower_fresh_resource(lowering);
                source_resource = resource;
                if (!lower_emit(lowering, qtt_anf_bind(
                        resource, parameter->representation, value)))
                    break;
                QttAnfValue moved = lowering->next_value++;
                QttAnfInstruction move =
                    qtt_anf_move_value(moved, resource);
                move.result_type =
                    lower_core_type(core->apply.arguments[i]);
                if (move.result_type == QTT_ANF_TYPE_UNKNOWN ||
                    !lower_emit(lowering, move))
                    break;
                value = moved;
            }
        }
        arguments[i] = (QttCallArgument){
            /* Elaboration has already selected this canonical parameter. */
            .type = parameter->type,
            .type_id = parameter->type_id,
            .representation = parameter->representation,
            .transfer = transfer,
        };
        operands[i] = (QttAnfCallOperand){
            .value = value,
            .type = lower_core_type(core->apply.arguments[i]),
            .type_id = parameter->type_id,
            .representation = parameter->representation,
            .transfer = transfer,
            .source_resource = source_resource,
            .loan_id = loan_id,
        };
    }
    QttCallPlan plan = {0};
    QttCallValidation validation =
        lowering->error == QTT_ANF_LOWER_OK
            ? qtt_call_plan(signature, arguments, count, &plan)
            : QTT_CALL_OUT_OF_MEMORY;
    if (validation != QTT_CALL_VALID) {
        free(arguments);
        free(operands);
        free(drop_after_call);
        free(end_after_call);
        if (lowering->error == QTT_ANF_LOWER_OK)
            lowering->error =
                validation == QTT_CALL_OUT_OF_MEMORY
                    ? QTT_ANF_LOWER_OUT_OF_MEMORY
                    : QTT_ANF_LOWER_UNSUPPORTED_CORE;
        return 0;
    }
    free(arguments);
    qtt_call_plan_free(&plan);
    QttAnfType result_type = lower_core_type(core);
    if (result_type == QTT_ANF_TYPE_UNKNOWN && signature->result.type) {
        QttCoreNode type_probe = {.type = signature->result.type};
        result_type = lower_core_type(&type_probe);
    }
    if (result_type == QTT_ANF_TYPE_UNKNOWN) {
        free(operands);
        free(drop_after_call);
        free(end_after_call);
        lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
        return 0;
    }
    QttAnfValue call_value = lowering->next_value++;
    QttCoreVar result_resource = {0};
    if (signature->result.mode == QTT_RESULT_OWNED)
        result_resource = lower_fresh_resource(lowering);
    QttAnfInstruction call = qtt_anf_call(
        call_value, result_resource, callable, operands, count,
        result_type, signature->result.type_id, signature->result.mode,
        signature->result.representation);
    call.contract_fingerprint =
        signature->contract_fingerprint;
    call.owns_call_arguments = true;
    if (!lower_emit(lowering, call)) {
        free(operands);
        free(drop_after_call);
        free(end_after_call);
        return 0;
    }
    for (size_t i = end_count; i > 0; i--)
        if (!lower_emit(lowering, qtt_anf_end_borrow(
                end_after_call[i - 1]))) {
            free(drop_after_call);
            free(end_after_call);
            return 0;
        }
    free(end_after_call);
    for (size_t i = 0; i < drop_count; i++)
        if (!lower_emit(lowering, qtt_anf_drop(drop_after_call[i]))) {
            free(drop_after_call);
            return 0;
        }
    free(drop_after_call);
    if (signature->result.mode != QTT_RESULT_OWNED)
        return call_value;
    if (!result_escapes)
        return lower_emit(lowering, qtt_anf_drop(result_resource))
            ? call_value : 0;
    QttAnfValue moved = lowering->next_value++;
    QttAnfInstruction move =
        qtt_anf_move_value(moved, result_resource);
    move.result_type = result_type;
    move.representation = signature->result.representation;
    return lower_emit(lowering, move) ? moved : 0;
}

static QttAnfValue lower_node(AnfLowering *lowering,
                              const QttCoreNode *core,
                              bool result_escapes) {
    if (!core || lowering->error != QTT_ANF_LOWER_OK) {
        lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
        return 0;
    }
    if (core->kind == QTT_CORE_LITERAL) {
        if (!core->literal.source) {
            lowering->error = QTT_ANF_LOWER_MALFORMED_LITERAL;
            return 0;
        }
        QttAnfValue result = lowering->next_value++;
        if (core->literal.source->type == AST_NUMBER)
            return lower_emit(lowering, qtt_anf_const_number(
                result, core->literal.source->number)) ? result : 0;
        if (core->literal.source->type == AST_STRING)
            return lower_emit(lowering, qtt_anf_const_string(
                result, core->literal.source->string)) ? result : 0;
        if (core->type && core->type->kind == TYPE_LAYOUT &&
            core->literal.source->type == AST_LIST)
            return lower_emit(lowering, qtt_anf_const_aggregate(result))
                ? result : 0;
        lowering->error = QTT_ANF_LOWER_MALFORMED_LITERAL;
        return 0;
    }
    if (core->kind == QTT_CORE_GLOBAL) {
        bool value;
        if (core->global.name && strcmp(core->global.name, "True") == 0)
            value = true;
        else if (core->global.name &&
                 strcmp(core->global.name, "False") == 0)
            value = false;
        else {
            lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
            return 0;
        }
        QttAnfValue result = lowering->next_value++;
        return lower_emit(lowering, qtt_anf_const_bool(result, value))
            ? result : 0;
    }
    if (core->kind == QTT_CORE_SEQUENCE) {
        if (!core->sequence.count) {
            lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
            return 0;
        }
        QttAnfValue result = 0;
        for (size_t i = 0; i < core->sequence.count; i++)
            result = lower_node(
                lowering, core->sequence.items[i],
                result_escapes && i + 1 == core->sequence.count);
        return result;
    }
    if (core->kind == QTT_CORE_VAR) {
        AnfLowerBinding *binding =
            lower_find_binding(lowering, core->var);
        if (!binding || !binding->remaining) {
            lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
            return 0;
        }
        QttAnfValue result = lowering->next_value++;
        binding->remaining--;
        if (binding->representation != QTT_REP_OWNED_HEAP) {
            QttAnfInstruction alias =
                qtt_anf_alias(result, binding->value);
            alias.result_type = lower_core_type(core);
            if (alias.result_type == QTT_ANF_TYPE_UNKNOWN)
                alias.result_type = binding->type;
            if (alias.result_type == QTT_ANF_TYPE_UNKNOWN) {
                lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
                return 0;
            }
            return lower_emit(lowering, alias) ? result : 0;
        }
        QttAnfInstruction ownership;
        if (!binding->owns_resource)
            ownership = qtt_anf_borrow_value(result, binding->var);
        else if (binding->remaining)
            ownership = qtt_anf_borrow_value(result, binding->var);
        else if (result_escapes)
            ownership = qtt_anf_move_value(result, binding->var);
        else
            ownership = qtt_anf_borrow_value(result, binding->var);
        ownership.result_type = lower_core_type(core);
        if (ownership.result_type == QTT_ANF_TYPE_UNKNOWN)
            ownership.result_type = binding->type;
        if (ownership.result_type == QTT_ANF_TYPE_UNKNOWN) {
            lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
            return 0;
        }
        if (!lower_emit(lowering, ownership))
            return 0;
        if (binding->owns_resource && !binding->remaining &&
            !result_escapes &&
            !lower_emit(lowering, qtt_anf_drop(binding->var)))
            return 0;
        return result;
    }
    if (core->kind == QTT_CORE_PLACE) {
        AnfLowerBinding *binding =
            lower_find_binding(lowering, core->place.root);
        if (!binding || !binding->remaining ||
            binding->representation != QTT_REP_OWNED_HEAP ||
            !binding->owns_resource ||
            !result_escapes) {
            lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
            return 0;
        }
        QttAnfType result_type = lower_core_type(core);
        if (result_type == QTT_ANF_TYPE_UNKNOWN) {
            lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
            return 0;
        }
        QttAnfValue result = lowering->next_value++;
        binding->remaining--;
        if (!lower_emit(lowering, qtt_anf_move_place(
                result, result_type, core->place)) ||
            !lower_emit(lowering, qtt_anf_drop(binding->var)))
            return 0;
        return result;
    }
    if (core->kind == QTT_CORE_BORROW) {
        AnfLowerBinding *owner = lower_find_binding(
            lowering, core->borrow.place.root);
        AnfLowerLoan *parent = core->borrow.parent.binder_id
            ? lower_find_loan(lowering, core->borrow.parent) : NULL;
        if (!owner || !owner->owns_resource ||
            owner->representation != QTT_REP_OWNED_HEAP ||
            !owner->remaining ||
            (core->borrow.parent.binder_id && !parent)) {
            lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
            return 0;
        }
        owner->remaining--;
        QttAnfLoanId loan_id = lowering->next_loan_id++;
        QttAnfValue authority = lowering->next_value++;
        QttAnfInstruction borrow = parent
            ? qtt_anf_reborrow_place(
                authority, core->borrow.place, loan_id, parent->id,
                core->borrow.loan_kind)
            : qtt_anf_borrow_place(
                authority, core->borrow.place, loan_id,
                core->borrow.loan_kind);
        borrow.result_type = owner->type;
        borrow.has_semantic_loan_identity = true;
        borrow.semantic_loan_ordinal = lowering->next_loan_ordinal++;
        AnfLowerLoan loan = {
            .binding = core->borrow.binding,
            .place = core->borrow.place,
            .kind = core->borrow.loan_kind,
            .id = loan_id,
            .parent = lowering->loans,
        };
        if (!lower_emit(lowering, borrow)) return 0;
        lowering->loans = &loan;
        QttAnfValue result = lower_node(
            lowering, core->borrow.body, result_escapes);
        lowering->loans = loan.parent;
        if (!result || !lower_emit(
                lowering, qtt_anf_end_borrow(loan_id)))
            return 0;
        if (!loan.parent && owner->remaining == 0 &&
            !lower_emit(lowering, qtt_anf_drop(owner->var)))
            return 0;
        return result;
    }
    if (core->kind == QTT_CORE_WRITE) {
        AnfLowerBinding *binding =
            lower_find_binding(
                lowering, core->write.place.root);
        if (!binding || !binding->remaining) {
            lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
            return 0;
        }
        QttAnfValue value =
            lower_node(lowering, core->write.value, true);
        if (!value) return 0;
        QttAnfType type = lower_core_type(core->write.value);
        if (binding->representation == QTT_REP_OWNED_HEAP) {
            AnfLowerLoan *loan = lower_find_covering_loan(
                lowering, core->write.place);
            if (loan) {
                if (loan->kind != QTT_LOAN_EXCLUSIVE ||
                    type == QTT_ANF_TYPE_UNKNOWN ||
                    !lower_declare_mutable_place(
                        lowering, core->write.place, type)) {
                    if (lowering->error == QTT_ANF_LOWER_OK)
                        lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
                    return 0;
                }
                QttAnfValue result = lowering->next_value++;
                QttAnfInstruction write = qtt_anf_write_place_with_loan(
                    result, core->write.place, value, loan->id);
                write.result_type = type;
                binding->remaining--;
                return lower_emit(lowering, write) ? result : 0;
            }
            if (core->write.place.projection_id != 0 ||
                core->write.place.projection_depth != 0) {
                if (!binding->owns_resource ||
                    type == QTT_ANF_TYPE_UNKNOWN ||
                    !lower_declare_mutable_place(
                        lowering, core->write.place, type)) {
                    if (lowering->error == QTT_ANF_LOWER_OK)
                        lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
                    return 0;
                }
                QttAnfLoanId loan_id = lowering->next_loan_id++;
                QttAnfValue authority = lowering->next_value++;
                QttAnfInstruction borrow = qtt_anf_borrow_place(
                    authority, core->write.place, loan_id,
                    QTT_LOAN_EXCLUSIVE);
                borrow.result_type = type;
                QttAnfValue result = lowering->next_value++;
                QttAnfInstruction write = qtt_anf_write_place_with_loan(
                    result, core->write.place, value, loan_id);
                write.result_type = type;
                if (!lower_emit(lowering, borrow) ||
                    !lower_emit(lowering, write) ||
                    !lower_emit(lowering, qtt_anf_end_borrow(loan_id)))
                    return 0;
                binding->remaining--;
                return result;
            }
            if (!binding->owns_resource ||
                type != binding->type ||
                qtt_resource_classify_type(core->write.value->type) !=
                    QTT_REP_OWNED_HEAP ||
                core->write.value->kind != QTT_CORE_LITERAL) {
                lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
                return 0;
            }
            QttAnfValue result = lowering->next_value++;
            QttAnfInstruction replace = qtt_anf_replace_value(
                result, binding->var, QTT_REP_OWNED_HEAP, value);
            replace.result_type = type;
            if (!lower_emit(lowering, replace)) return 0;
            binding->remaining--;
            binding->value = result;
            return result;
        }
        if (type == QTT_ANF_TYPE_UNKNOWN ||
            !lower_declare_mutable_place(
                lowering, core->write.place, type)) {
            if (lowering->error == QTT_ANF_LOWER_OK)
                lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
            return 0;
        }
        QttAnfValue result = lowering->next_value++;
        QttAnfInstruction write = qtt_anf_write_place(
            result, core->write.place, value);
        write.result_type = type;
        if (!lower_emit(lowering, write)) return 0;
        binding->remaining--;
        binding->value = result;
        return result;
    }
    if (core->kind == QTT_CORE_APPLY &&
        core->apply.callee &&
        core->apply.callee->kind == QTT_CORE_LAMBDA)
        return lower_direct_call(lowering, core, result_escapes);
    if (core->kind == QTT_CORE_APPLY &&
        core->apply.callee &&
        core->apply.callee->kind == QTT_CORE_GLOBAL)
        return lower_named_call(lowering, core, result_escapes);
    if (core->kind == QTT_CORE_APPLY &&
        core->apply.callee &&
        core->apply.callee->kind == QTT_CORE_VAR) {
        AnfLowerBinding *callable = lower_find_binding(
            lowering, core->apply.callee->var);
        if (!callable || !callable->callable) {
            lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
            return 0;
        }
        QttCoreNode resolved = *core;
        resolved.apply.callee = (QttCoreNode *)callable->callable;
        return lower_direct_call(lowering, &resolved, result_escapes);
    }
    if (core->kind == QTT_CORE_LET) {
        const QttCoreNode *known_callable = NULL;
        if (core->let.value &&
            core->let.value->kind == QTT_CORE_LAMBDA)
            known_callable = core->let.value;
        else if (core->let.value &&
                 core->let.value->kind == QTT_CORE_VAR) {
            AnfLowerBinding *source = lower_find_binding(
                lowering, core->let.value->var);
            known_callable = source ? source->callable : NULL;
        }
        if (known_callable) {
            AnfLowerBinding binding = {
                .var = core->let.binding,
                .remaining = qtt_demand_occurrences(
                    lowering->demand, core->let.body,
                    core->let.binding),
                .representation = QTT_REP_INLINE,
                .callable = known_callable,
                .parent = lowering->bindings,
            };
            lowering->bindings = &binding;
            QttAnfValue result = lower_node(
                lowering, core->let.body, result_escapes);
            lowering->bindings = binding.parent;
            return result;
        }
        QttAnfValue value = lower_node(lowering, core->let.value, true);
        if (!value) return 0;
        const QttAnfInstruction *producer =
            lower_value_producer(lowering, value);
        QttAnfType produced_type = producer
            ? producer->result_type : QTT_ANF_TYPE_UNKNOWN;
        QttRepresentation produced_representation = producer
            ? producer->representation : QTT_REP_UNKNOWN;
        QttRepresentation representation =
            qtt_resource_classify_type(core->let.value->type);
        /* A lexical callable's checked lambda is more authoritative than
         * legacy source annotations on the application node.  In particular
         * an identity alias can return an owned String while the old AST
         * still describes the call as an inline function value. */
        const QttCoreNode *result_callable = NULL;
        if (core->let.value->kind == QTT_CORE_APPLY &&
            core->let.value->apply.callee) {
            if (core->let.value->apply.callee->kind == QTT_CORE_LAMBDA)
                result_callable = core->let.value->apply.callee;
            else if (core->let.value->apply.callee->kind == QTT_CORE_VAR) {
                AnfLowerBinding *callee = lower_find_binding(
                    lowering, core->let.value->apply.callee->var);
                result_callable = callee ? callee->callable : NULL;
            }
        }
        if (result_callable) {
            QttSignatureError result_error = QTT_SIGNATURE_OK;
            QttFunctionSignature *result_signature =
                qtt_signature_derive(result_callable, &result_error);
            if (!result_signature) {
                lowering->error = result_error == QTT_SIGNATURE_OUT_OF_MEMORY
                    ? QTT_ANF_LOWER_OUT_OF_MEMORY
                    : QTT_ANF_LOWER_UNSUPPORTED_CORE;
                return 0;
            }
            representation = result_signature->result.representation;
            QttAnfType contract_type =
                lower_core_type(result_callable->lambda.body);
            if (contract_type != QTT_ANF_TYPE_UNKNOWN)
                produced_type = contract_type;
            qtt_signature_free(result_signature);
        }
        if (representation == QTT_REP_UNKNOWN)
            representation = produced_representation;
        if (representation == QTT_REP_UNKNOWN) {
            lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
            return 0;
        }
        /* A BORROW result is an alias, not a fresh owning capability.  Until
         * lexical bindings carry a distinct source-resource identity, do not
         * silently turn that alias into an owner with ALLOC. */
        if (representation == QTT_REP_OWNED_HEAP && producer &&
            producer->kind == QTT_ANF_BORROW) {
            lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
            return 0;
        }
        if (representation == QTT_REP_OWNED_HEAP &&
            !lower_emit(lowering, qtt_anf_bind(
                core->let.binding, representation, value)))
            return 0;
        AnfLowerBinding binding = {
            .var = core->let.binding,
            .value = value,
            .type = produced_type != QTT_ANF_TYPE_UNKNOWN
                ? produced_type
                : lower_core_type(core->let.value),
            .remaining = qtt_demand_occurrences(
                lowering->demand, core->let.body, core->let.binding),
            .representation = representation,
            .owns_resource = representation == QTT_REP_OWNED_HEAP,
            .parent = lowering->bindings,
        };
        lowering->bindings = &binding;
        QttAnfValue result =
            lower_node(lowering, core->let.body, result_escapes);
        lowering->bindings = binding.parent;
        if (!result) return 0;
        if (representation == QTT_REP_OWNED_HEAP &&
            binding.remaining == 0 &&
            qtt_demand_occurrences(
                lowering->demand, core->let.body,
                core->let.binding) == 0 &&
            !lower_emit(lowering, qtt_anf_drop(core->let.binding)))
            return 0;
        return result;
    }
    if (core->kind != QTT_CORE_IF) {
        lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
        return 0;
    }

    QttAnfValue condition =
        lower_node(lowering, core->conditional.condition, false);
    if (!condition) return 0;

    size_t binding_count = 0;
    for (AnfLowerBinding *binding = lowering->bindings;
         binding; binding = binding->parent)
        binding_count++;
    size_t branch_capacity = binding_count ? binding_count : 1;
    LowerBranchState state = {
        .bindings = calloc(branch_capacity, sizeof(*state.bindings)),
        .then_remaining =
            calloc(branch_capacity, sizeof(*state.then_remaining)),
        .else_remaining =
            calloc(branch_capacity, sizeof(*state.else_remaining)),
        .join_remaining =
            calloc(branch_capacity, sizeof(*state.join_remaining)),
        .entry_values =
            calloc(branch_capacity, sizeof(*state.entry_values)),
        .then_values =
            calloc(branch_capacity, sizeof(*state.then_values)),
        .else_values =
            calloc(branch_capacity, sizeof(*state.else_values)),
    };
    if (!state.bindings || !state.then_remaining ||
        !state.else_remaining || !state.join_remaining ||
        !state.entry_values || !state.then_values ||
        !state.else_values) {
        lower_branch_state_free(&state);
        lowering->error = QTT_ANF_LOWER_OUT_OF_MEMORY;
        return 0;
    }
    size_t binding_index = 0;
    for (AnfLowerBinding *binding = lowering->bindings;
         binding; binding = binding->parent, binding_index++) {
        size_t then_uses = qtt_demand_occurrences(
            lowering->demand, core->conditional.then_branch, binding->var);
        size_t else_uses = qtt_demand_occurrences(
            lowering->demand, core->conditional.else_branch, binding->var);
        if (then_uses + else_uses > binding->remaining) {
            lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
            lower_branch_state_free(&state);
            return 0;
        }
        state.bindings[binding_index] = binding;
        state.entry_values[binding_index] = binding->value;
        state.join_remaining[binding_index] =
            binding->remaining - then_uses - else_uses;
        state.then_remaining[binding_index] =
            then_uses + state.join_remaining[binding_index];
        state.else_remaining[binding_index] =
            else_uses + state.join_remaining[binding_index];
    }

    size_t branch_block = lowering->current;
    size_t branch_ordinal = lowering->next_branch_ordinal++;
    size_t then_block = lower_add_block(lowering);
    size_t else_block = lower_add_block(lowering);
    size_t join_block = lower_add_block(lowering);
    if (lowering->error != QTT_ANF_LOWER_OK) {
        lower_branch_state_free(&state);
        return 0;
    }
    QttAnfValue *then_parameters =
        calloc(branch_capacity, sizeof(*then_parameters));
    QttAnfValue *else_parameters =
        calloc(branch_capacity, sizeof(*else_parameters));
    QttAnfType *then_parameter_types =
        calloc(branch_capacity, sizeof(*then_parameter_types));
    QttAnfType *else_parameter_types =
        calloc(branch_capacity, sizeof(*else_parameter_types));
    QttAnfValue *then_entry_arguments =
        calloc(branch_capacity, sizeof(*then_entry_arguments));
    QttAnfValue *else_entry_arguments =
        calloc(branch_capacity, sizeof(*else_entry_arguments));
    if (!then_parameters || !else_parameters ||
        !then_parameter_types || !else_parameter_types ||
        !then_entry_arguments || !else_entry_arguments) {
        free(then_parameters);
        free(else_parameters);
        free(then_parameter_types);
        free(else_parameter_types);
        free(then_entry_arguments);
        free(else_entry_arguments);
        lower_branch_state_free(&state);
        lowering->error = QTT_ANF_LOWER_OUT_OF_MEMORY;
        return 0;
    }
    for (size_t i = 0; i < binding_count; i++) {
        if (state.bindings[i]->type == QTT_ANF_TYPE_UNKNOWN) {
            free(then_parameters);
            free(else_parameters);
            free(then_parameter_types);
            free(else_parameter_types);
            free(then_entry_arguments);
            free(else_entry_arguments);
            lower_branch_state_free(&state);
            lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
            return 0;
        }
        then_parameters[i] = lowering->next_value++;
        else_parameters[i] = lowering->next_value++;
        then_parameter_types[i] = state.bindings[i]->type;
        else_parameter_types[i] = state.bindings[i]->type;
        then_entry_arguments[i] = state.entry_values[i];
        else_entry_arguments[i] = state.entry_values[i];
    }
    lowering->program->blocks[then_block].parameters = then_parameters;
    lowering->program->blocks[then_block].parameter_types =
        then_parameter_types;
    lowering->program->blocks[then_block].parameter_count = binding_count;
    lowering->program->blocks[else_block].parameters = else_parameters;
    lowering->program->blocks[else_block].parameter_types =
        else_parameter_types;
    lowering->program->blocks[else_block].parameter_count = binding_count;
    lowering->program->blocks[branch_block].terminator =
        qtt_anf_branch(condition, then_block, else_block);
    lowering->program->blocks[branch_block].terminator.arguments =
        then_entry_arguments;
    lowering->program->blocks[branch_block].terminator.argument_count =
        binding_count;
    lowering->program->blocks[branch_block].terminator.else_arguments =
        else_entry_arguments;
    lowering->program->blocks[branch_block].terminator.else_argument_count =
        binding_count;
    lowering->program->blocks[branch_block].terminator.has_branch_identity =
        true;
    lowering->program->blocks[branch_block].terminator.branch_ordinal =
        branch_ordinal;

    for (size_t i = 0; i < binding_count; i++) {
        state.bindings[i]->remaining = state.then_remaining[i];
        state.bindings[i]->value = then_parameters[i];
    }
    lowering->current = then_block;
    QttAnfValue then_value = 0;
    if (lower_control_push(lowering, branch_ordinal, false)) {
        then_value = lower_node(
            lowering, core->conditional.then_branch, result_escapes);
        lower_control_pop(lowering);
    }
    if (!then_value) {
        lower_branch_state_free(&state);
        return 0;
    }
    size_t then_exit_block = lowering->current;
    for (size_t i = 0; i < binding_count; i++)
        state.then_values[i] = state.bindings[i]->value;

    for (size_t i = 0; i < binding_count; i++) {
        state.bindings[i]->remaining = state.else_remaining[i];
        state.bindings[i]->value = else_parameters[i];
    }
    lowering->current = else_block;
    QttAnfValue else_value = 0;
    if (lower_control_push(lowering, branch_ordinal, true)) {
        else_value = lower_node(
            lowering, core->conditional.else_branch, result_escapes);
        lower_control_pop(lowering);
    }
    if (!else_value) {
        lower_branch_state_free(&state);
        return 0;
    }
    size_t else_exit_block = lowering->current;
    for (size_t i = 0; i < binding_count; i++) {
        state.else_values[i] = state.bindings[i]->value;
    }

    lowering->current = join_block;
    QttAnfValue result = lowering->next_value++;
    size_t join_count = 1 + binding_count;
    QttAnfValue *parameters =
        calloc(join_count, sizeof(*parameters));
    QttAnfType *parameter_types =
        calloc(join_count, sizeof(*parameter_types));
    QttAnfValue *then_arguments =
        calloc(join_count, sizeof(*then_arguments));
    QttAnfValue *else_arguments =
        calloc(join_count, sizeof(*else_arguments));
    if (!parameters || !parameter_types ||
        !then_arguments || !else_arguments) {
        free(parameters);
        free(parameter_types);
        free(then_arguments);
        free(else_arguments);
        lower_branch_state_free(&state);
        lowering->error = QTT_ANF_LOWER_OUT_OF_MEMORY;
        return 0;
    }
    parameters[0] = result;
    parameter_types[0] = lower_core_type(core);
    then_arguments[0] = then_value;
    else_arguments[0] = else_value;
    if (parameter_types[0] == QTT_ANF_TYPE_UNKNOWN) {
        free(parameters);
        free(parameter_types);
        free(then_arguments);
        free(else_arguments);
        lower_branch_state_free(&state);
        lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
        return 0;
    }
    size_t joined = 1;
    for (size_t i = 0; i < binding_count; i++) {
        state.bindings[i]->remaining = state.join_remaining[i];
        QttAnfType binding_type = state.bindings[i]->type;
        if (binding_type == QTT_ANF_TYPE_UNKNOWN) {
            free(parameters);
            free(parameter_types);
            free(then_arguments);
            free(else_arguments);
            lower_branch_state_free(&state);
            lowering->error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
            return 0;
        }
        parameters[joined] = lowering->next_value++;
        parameter_types[joined] = binding_type;
        then_arguments[joined] = state.then_values[i];
        else_arguments[joined] = state.else_values[i];
        state.bindings[i]->value = parameters[joined];
        joined++;
    }
    lowering->program->blocks[then_exit_block].terminator =
        qtt_anf_jump(join_block, then_arguments, join_count);
    lowering->program->blocks[else_exit_block].terminator =
        qtt_anf_jump(join_block, else_arguments, join_count);
    lowering->program->blocks[join_block].parameters = parameters;
    lowering->program->blocks[join_block].parameter_types =
        parameter_types;
    lowering->program->blocks[join_block].parameter_count = join_count;
    lower_branch_state_free(&state);
    return result;
}

static QttAnfProgram *lower_core_certified_in_env(
    const QttCoreNode *core,
    const QttDemandCertificate *demand,
    const QttSignatureEnv *signatures,
    uint64_t module_id,
    QttAnfLowerError *error) {
    if (qtt_demand_validate(demand, core) != QTT_DEMAND_VALID) {
        if (error) *error = QTT_ANF_LOWER_INVALID_DEMAND;
        return NULL;
    }
    QttAnfProgram *program = calloc(1, sizeof(*program));
    if (!program) {
        if (error) *error = QTT_ANF_LOWER_OUT_OF_MEMORY;
        return NULL;
    }
    AnfLowering lowering = {
        .program = program,
        .next_value = 1,
        .error = QTT_ANF_LOWER_OK,
        .demand = demand,
        .signatures = signatures,
        .module_id = module_id,
        .next_synthetic_binder = UINT64_C(0x8000000000000000),
        .next_loan_id = 1,
    };
    program->signatures = signatures;
    lower_add_block(&lowering);
    QttAnfValue result = lower_node(&lowering, core, true);
    if (lowering.error == QTT_ANF_LOWER_OK)
        program->blocks[lowering.current].terminator = qtt_anf_return(result);
    free(lowering.control_path);
    if (lowering.error != QTT_ANF_LOWER_OK) {
        qtt_anf_program_free(program);
        program = NULL;
    }
    if (error) *error = lowering.error;
    return program;
}

QttAnfProgram *qtt_anf_lower_core_certified(
    const QttCoreNode *core,
    const QttDemandCertificate *demand,
    QttAnfLowerError *error) {
    return lower_core_certified_in_env(core, demand, NULL, 0, error);
}

QttAnfProgram *qtt_anf_lower_core_in_env(
    const QttCoreNode *core,
    const QttSignatureEnv *signatures,
    uint64_t module_id,
    QttAnfLowerError *error) {
    QttDemandError demand_error = QTT_DEMAND_OK;
    QttDemandCertificate *demand =
        qtt_demand_derive(core, &demand_error);
    if (!demand) {
        if (error)
            *error = demand_error == QTT_DEMAND_OUT_OF_MEMORY
                ? QTT_ANF_LOWER_OUT_OF_MEMORY
                : QTT_ANF_LOWER_UNSUPPORTED_CORE;
        return NULL;
    }
    QttAnfProgram *program =
        lower_core_certified_in_env(
            core, demand, signatures, module_id, error);
    qtt_demand_certificate_free(demand);
    return program;
}

QttAnfProgram *qtt_anf_lower_core(const QttCoreNode *core,
                                  QttAnfLowerError *error) {
    return qtt_anf_lower_core_in_env(core, NULL, 0, error);
}

QttAnfProgram *qtt_anf_lower_function_in_env(
    const QttCoreNode *lambda,
    const QttFunctionSignature *signature,
    const QttSignatureEnv *signatures,
    uint64_t module_id,
    QttAnfLowerError *error) {
    if (!lambda || lambda->kind != QTT_CORE_LAMBDA ||
        (signatures && signature &&
             signature->effects_environment_solved
            ? qtt_signature_validate_in_effect_env(
                  signature, lambda, module_id,
                  anf_effect_lookup, (void *)signatures)
            : qtt_signature_validate(signature, lambda)) !=
                QTT_SIGNATURE_VALID) {
        if (error) *error = QTT_ANF_LOWER_INVALID_DEMAND;
        return NULL;
    }
    QttAnfProgram *program = calloc(1, sizeof(*program));
    if (!program) {
        if (error) *error = QTT_ANF_LOWER_OUT_OF_MEMORY;
        return NULL;
    }
    AnfLowering lowering = {
        .program = program,
        .next_value = 1,
        .error = QTT_ANF_LOWER_OK,
        .demand = signature->demand,
        .signatures = signatures,
        .module_id = module_id,
        .next_synthetic_binder = UINT64_C(0x8000000000000000),
        .next_loan_id = 1,
    };
    program->signatures = signatures;
    lower_add_block(&lowering);
    size_t active_count = 0;
    for (size_t i = 0; i < signature->parameter_count; i++)
        if (signature->parameters[i].mode != QTT_OWNERSHIP_ERASED)
            active_count++;
    QttAnfBlock *entry = &program->blocks[0];
    if (active_count) {
        entry->parameters = calloc(active_count, sizeof(*entry->parameters));
        entry->parameter_types =
            calloc(active_count, sizeof(*entry->parameter_types));
        program->initial_resources = calloc(
            active_count, sizeof(*program->initial_resources));
        program->initial_representations = calloc(
            active_count, sizeof(*program->initial_representations));
        program->initial_resource_owned = calloc(
            active_count, sizeof(*program->initial_resource_owned));
    }
    AnfLowerBinding *bindings =
        calloc(active_count ? active_count : 1, sizeof(*bindings));
    if (active_count &&
        (!entry->parameters || !entry->parameter_types ||
         !program->initial_resources ||
         !program->initial_representations ||
         !program->initial_resource_owned || !bindings)) {
        free(bindings);
        qtt_anf_program_free(program);
        if (error) *error = QTT_ANF_LOWER_OUT_OF_MEMORY;
        return NULL;
    }
    size_t active = 0;
    size_t resource_count = 0;
    for (size_t i = 0; i < signature->parameter_count; i++) {
        const QttParameterContract *parameter = &signature->parameters[i];
        if (parameter->mode == QTT_OWNERSHIP_ERASED) continue;
        QttAnfType type = QTT_ANF_TYPE_UNKNOWN;
        if (parameter->type) {
            QttCoreNode type_probe = {.type = parameter->type};
            type = lower_core_type(&type_probe);
        }
        if (type == QTT_ANF_TYPE_UNKNOWN) {
            lowering.error = QTT_ANF_LOWER_UNSUPPORTED_CORE;
            break;
        }
        QttAnfValue value = lowering.next_value++;
        entry->parameters[active] = value;
        entry->parameter_types[active] = type;
        if (parameter->representation == QTT_REP_OWNED_HEAP) {
            program->initial_resources[resource_count] =
                parameter->var;
            program->initial_representations[resource_count] =
                parameter->representation;
            program->initial_resource_owned[resource_count] =
                parameter->mode == QTT_OWNERSHIP_CONSUMED;
            resource_count++;
        }
        bindings[active] = (AnfLowerBinding){
            .var = parameter->var,
            .value = value,
            .type = type,
            /*
             * The contract grade is path-sensitive (choice uses max), while
             * structural lowering visits and emits both branch bodies.
             * Its cursor therefore tracks checked syntactic occurrences.
             */
            .remaining = qtt_demand_occurrences(
                signature->demand, lambda->lambda.body, parameter->var),
            .representation = parameter->representation,
            .owns_resource =
                parameter->representation == QTT_REP_OWNED_HEAP &&
                parameter->mode == QTT_OWNERSHIP_CONSUMED,
            .parent = active ? &bindings[active - 1] : NULL,
        };
        active++;
    }
    entry->parameter_count = active;
    program->initial_resource_count = resource_count;
    lowering.bindings = active ? &bindings[active - 1] : NULL;
    QttAnfValue result = lowering.error == QTT_ANF_LOWER_OK
        ? lower_node(&lowering, lambda->lambda.body, true) : 0;
    if (lowering.error == QTT_ANF_LOWER_OK)
        program->blocks[lowering.current].terminator =
            qtt_anf_return(result);
    free(lowering.control_path);
    free(bindings);
    if (lowering.error != QTT_ANF_LOWER_OK) {
        qtt_anf_program_free(program);
        program = NULL;
    }
    if (error) *error = lowering.error;
    return program;
}

QttAnfProgram *qtt_anf_lower_function(
    const QttCoreNode *lambda,
    const QttFunctionSignature *signature,
    QttAnfLowerError *error) {
    return qtt_anf_lower_function_in_env(
        lambda, signature, NULL, 0, error);
}

void qtt_anf_program_free(QttAnfProgram *program) {
    if (!program) return;
    for (size_t i = 0; i < program->block_count; i++) {
        free(program->blocks[i].parameters);
        free(program->blocks[i].parameter_types);
        free(program->blocks[i].parameter_loans);
        for (size_t j = 0;
             j < program->blocks[i].instruction_count; j++) {
            if (program->blocks[i].instructions[j].owns_call_arguments)
                free(program->blocks[i].instructions[j].call_arguments);
            free(program->blocks[i].instructions[j].control_path);
        }
        free(program->blocks[i].instructions);
        free(program->blocks[i].terminator.arguments);
        free(program->blocks[i].terminator.else_arguments);
    }
    free(program->blocks);
    free(program->initial_resources);
    free(program->initial_representations);
    free(program->initial_resource_owned);
    free(program->environment_slots);
    free(program->mutable_places);
    free(program);
}
