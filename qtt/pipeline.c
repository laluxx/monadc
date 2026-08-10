#include "pipeline.h"
#include "elaboration.h"
#include "environment.h"
#include "../infer.h"

#include <stdlib.h>
#include <string.h>

static bool source_target(
    uint64_t binder_id, const uint64_t *targets, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (targets[i] == binder_id) return true;
    return false;
}

static const InferGradeApplication *source_application(
    const InferCtx *inference, const AST *ast) {
    size_t count = infer_grade_application_count(inference);
    for (size_t i = 0; i < count; i++) {
        const InferGradeApplication *application =
            infer_grade_application(inference, i);
        if (application && application->application == ast)
            return application;
    }
    return NULL;
}

typedef struct {
    uint64_t module_id;
    uint64_t *module_ids;
    uint64_t *closure_ids;
    uint64_t *binder_ids;
    size_t *slots;
    size_t *parameter_indices;
    QttEnvironmentOriginKind *origin_kinds;
    uint64_t *origin_ids;
    QttGradeExpr **grades;
    uint64_t *domain_module_ids;
    uint64_t *domain_closure_ids;
    size_t *domain_indices;
    QttGradeExpr **domain_grades;
    size_t domain_count;
    size_t domain_capacity;
    uint64_t *invoked_instance_ids;
    size_t invoked_instance_count;
    size_t invoked_instance_capacity;
    uint64_t *alias_binder_ids;
    const InferGradeApplication **alias_applications;
    size_t alias_count;
    size_t alias_capacity;
    size_t *callable_parameter_indices;
    size_t callable_parameter_count;
    size_t callable_parameter_capacity;
    size_t *callable_domain_parameter_indices;
    size_t *callable_domain_indices;
    QttGradeExpr **callable_domain_grades;
    size_t callable_domain_count;
    size_t callable_domain_capacity;
    const uint64_t *root_parameters;
    size_t root_parameter_count;
    size_t count;
    size_t capacity;
} SourceClosures;

static QttGradeExpr *source_callable_domain(
    SourceClosures *closures, QttGradeArena *arena,
    size_t parameter_index, size_t domain_index) {
    if (!closures || !arena ||
        parameter_index >= closures->root_parameter_count)
        return NULL;
    for (size_t i = 0; i < closures->callable_domain_count; i++)
        if (closures->callable_domain_parameter_indices[i] ==
                parameter_index &&
            closures->callable_domain_indices[i] == domain_index)
            return closures->callable_domain_grades[i];
    if (closures->callable_domain_count ==
        closures->callable_domain_capacity) {
        size_t next = closures->callable_domain_capacity
            ? closures->callable_domain_capacity * 2 : 4;
        size_t *parameters = realloc(
            closures->callable_domain_parameter_indices,
            next * sizeof(*parameters));
        size_t *indices = realloc(
            closures->callable_domain_indices,
            next * sizeof(*indices));
        QttGradeExpr **grades = realloc(
            closures->callable_domain_grades,
            next * sizeof(*grades));
        if (!parameters || !indices || !grades) {
            if (parameters)
                closures->callable_domain_parameter_indices = parameters;
            if (indices) closures->callable_domain_indices = indices;
            if (grades) closures->callable_domain_grades = grades;
            return NULL;
        }
        closures->callable_domain_parameter_indices = parameters;
        closures->callable_domain_indices = indices;
        closures->callable_domain_grades = grades;
        closures->callable_domain_capacity = next;
    }
    QttGradeExpr *grade = qtt_grade_fresh(arena);
    if (!grade) return NULL;
    size_t index = closures->callable_domain_count++;
    closures->callable_domain_parameter_indices[index] = parameter_index;
    closures->callable_domain_indices[index] = domain_index;
    closures->callable_domain_grades[index] = grade;
    return grade;
}

static bool source_callable_add(
    SourceClosures *closures, size_t parameter_index) {
    if (!closures || parameter_index >= closures->root_parameter_count)
        return false;
    for (size_t i = 0; i < closures->callable_parameter_count; i++)
        if (closures->callable_parameter_indices[i] == parameter_index)
            return true;
    if (closures->callable_parameter_count ==
        closures->callable_parameter_capacity) {
        size_t next = closures->callable_parameter_capacity
            ? closures->callable_parameter_capacity * 2 : 4;
        size_t *grown = realloc(
            closures->callable_parameter_indices, next * sizeof(*grown));
        if (!grown) return false;
        closures->callable_parameter_indices = grown;
        closures->callable_parameter_capacity = next;
    }
    closures->callable_parameter_indices[
        closures->callable_parameter_count++] = parameter_index;
    return true;
}

static const InferGradeApplication *source_alias_application(
    const SourceClosures *closures, uint64_t binder_id) {
    if (!closures || !binder_id) return NULL;
    for (size_t i = closures->alias_count; i > 0; i--)
        if (closures->alias_binder_ids[i - 1] == binder_id)
            return closures->alias_applications[i - 1];
    return NULL;
}

static bool source_alias_push(
    SourceClosures *closures, uint64_t binder_id,
    const InferGradeApplication *application) {
    if (!closures || !binder_id || !application ||
        !application->result_closure_count)
        return false;
    if (closures->alias_count == closures->alias_capacity) {
        size_t next = closures->alias_capacity
            ? closures->alias_capacity * 2 : 8;
        uint64_t *binders = realloc(
            closures->alias_binder_ids, next * sizeof(*binders));
        const InferGradeApplication **applications = realloc(
            closures->alias_applications, next * sizeof(*applications));
        if (!binders || !applications) {
            if (binders) closures->alias_binder_ids = binders;
            if (applications) closures->alias_applications = applications;
            return false;
        }
        closures->alias_binder_ids = binders;
        closures->alias_applications = applications;
        closures->alias_capacity = next;
    }
    size_t index = closures->alias_count++;
    closures->alias_binder_ids[index] = binder_id;
    closures->alias_applications[index] = application;
    return true;
}

static bool source_invoked_instance_add(
    SourceClosures *closures, uint64_t instance_id) {
    if (!closures || !instance_id) return false;
    for (size_t i = 0; i < closures->invoked_instance_count; i++)
        if (closures->invoked_instance_ids[i] == instance_id)
            return true;
    if (closures->invoked_instance_count ==
        closures->invoked_instance_capacity) {
        size_t next = closures->invoked_instance_capacity
            ? closures->invoked_instance_capacity * 2 : 8;
        uint64_t *grown = realloc(
            closures->invoked_instance_ids, next * sizeof(*grown));
        if (!grown) return false;
        closures->invoked_instance_ids = grown;
        closures->invoked_instance_capacity = next;
    }
    closures->invoked_instance_ids[
        closures->invoked_instance_count++] = instance_id;
    return true;
}

static bool source_closure_domain_append(
    SourceClosures *closures, uint64_t module_id, uint64_t closure_id,
    size_t parameter_index, QttGradeExpr *grade) {
    if (!closures || !module_id || !closure_id || !grade) return false;
    if (closures->domain_count == closures->domain_capacity) {
        size_t next = closures->domain_capacity
            ? closures->domain_capacity * 2 : 8;
        uint64_t *modules = realloc(
            closures->domain_module_ids, next * sizeof(*modules));
        uint64_t *ids = realloc(
            closures->domain_closure_ids, next * sizeof(*ids));
        size_t *indices = realloc(
            closures->domain_indices, next * sizeof(*indices));
        QttGradeExpr **grades = realloc(
            closures->domain_grades, next * sizeof(*grades));
        if (!modules || !ids || !indices || !grades) {
            if (modules) closures->domain_module_ids = modules;
            if (ids) closures->domain_closure_ids = ids;
            if (indices) closures->domain_indices = indices;
            if (grades) closures->domain_grades = grades;
            return false;
        }
        closures->domain_module_ids = modules;
        closures->domain_closure_ids = ids;
        closures->domain_indices = indices;
        closures->domain_grades = grades;
        closures->domain_capacity = next;
    }
    size_t index = closures->domain_count++;
    closures->domain_module_ids[index] = module_id;
    closures->domain_closure_ids[index] = closure_id;
    closures->domain_indices[index] = parameter_index;
    closures->domain_grades[index] = grade;
    return true;
}

static bool source_closure_append(
    SourceClosures *closures, uint64_t module_id, uint64_t closure_id,
    size_t slot, QttEnvironmentOriginKind origin_kind,
    uint64_t origin_id, QttGradeExpr *grade) {
    if (!closures || !module_id || !closure_id ||
        !origin_id || !grade) return false;
    if (closures->count == closures->capacity) {
        size_t next = closures->capacity ? closures->capacity * 2 : 8;
        uint64_t *module_ids =
            realloc(closures->module_ids, next * sizeof(*module_ids));
        uint64_t *closure_ids =
            realloc(closures->closure_ids, next * sizeof(*closure_ids));
        uint64_t *binder_ids =
            realloc(closures->binder_ids, next * sizeof(*binder_ids));
        size_t *slots =
            realloc(closures->slots, next * sizeof(*slots));
        size_t *parameter_indices = realloc(
            closures->parameter_indices, next * sizeof(*parameter_indices));
        QttEnvironmentOriginKind *origin_kinds = realloc(
            closures->origin_kinds, next * sizeof(*origin_kinds));
        uint64_t *origin_ids = realloc(
            closures->origin_ids, next * sizeof(*origin_ids));
        QttGradeExpr **grades =
            realloc(closures->grades, next * sizeof(*grades));
        if (!module_ids || !closure_ids || !binder_ids || !slots ||
            !parameter_indices || !origin_kinds || !origin_ids || !grades) {
            if (module_ids) closures->module_ids = module_ids;
            if (closure_ids) closures->closure_ids = closure_ids;
            if (binder_ids) closures->binder_ids = binder_ids;
            if (slots) closures->slots = slots;
            if (parameter_indices)
                closures->parameter_indices = parameter_indices;
            if (origin_kinds) closures->origin_kinds = origin_kinds;
            if (origin_ids) closures->origin_ids = origin_ids;
            if (grades) closures->grades = grades;
            return false;
        }
        closures->module_ids = module_ids;
        closures->closure_ids = closure_ids;
        closures->binder_ids = binder_ids;
        closures->slots = slots;
        closures->parameter_indices = parameter_indices;
        closures->origin_kinds = origin_kinds;
        closures->origin_ids = origin_ids;
        closures->grades = grades;
        closures->capacity = next;
    }
    size_t index = closures->count++;
    closures->module_ids[index] = module_id;
    closures->closure_ids[index] = closure_id;
    closures->binder_ids[index] = origin_id;
    closures->slots[index] = slot;
    closures->parameter_indices[index] = SIZE_MAX;
    for (size_t i = 0; i < closures->root_parameter_count; i++)
        if (origin_kind == QTT_ENVIRONMENT_LOCAL &&
            closures->root_parameters[i] == origin_id) {
            closures->parameter_indices[index] = i;
            origin_kind = QTT_ENVIRONMENT_PARAMETER;
            origin_id = i;
            break;
        }
    closures->origin_kinds[index] = origin_kind;
    closures->origin_ids[index] = origin_id;
    closures->grades[index] = grade;
    return true;
}

static void source_closures_free(SourceClosures *closures) {
    if (!closures) return;
    free(closures->module_ids);
    free(closures->closure_ids);
    free(closures->binder_ids);
    free(closures->slots);
    free(closures->parameter_indices);
    free(closures->origin_kinds);
    free(closures->origin_ids);
    free(closures->grades);
    free(closures->domain_module_ids);
    free(closures->domain_closure_ids);
    free(closures->domain_indices);
    free(closures->domain_grades);
    free(closures->invoked_instance_ids);
    free(closures->alias_binder_ids);
    free(closures->alias_applications);
    free(closures->callable_parameter_indices);
    free(closures->callable_domain_parameter_indices);
    free(closures->callable_domain_indices);
    free(closures->callable_domain_grades);
    memset(closures, 0, sizeof(*closures));
}

static bool source_head_is(const AST *ast, const char *name);

typedef struct {
    uint64_t *module_ids;
    uint64_t *closure_ids;
    size_t count;
    size_t capacity;
} SourceResults;

static bool source_result_append(
    SourceResults *results, uint64_t module_id, uint64_t closure_id) {
    if (!results || !module_id || !closure_id) return false;
    for (size_t i = 0; i < results->count; i++)
        if (results->module_ids[i] == module_id &&
            results->closure_ids[i] == closure_id)
            return true;
    if (results->count == results->capacity) {
        size_t next = results->capacity ? results->capacity * 2 : 4;
        uint64_t *modules =
            realloc(results->module_ids, next * sizeof(*modules));
        uint64_t *closures =
            realloc(results->closure_ids, next * sizeof(*closures));
        if (!modules || !closures) {
            if (modules) results->module_ids = modules;
            if (closures) results->closure_ids = closures;
            return false;
        }
        results->module_ids = modules;
        results->closure_ids = closures;
        results->capacity = next;
    }
    results->module_ids[results->count] = module_id;
    results->closure_ids[results->count++] = closure_id;
    return true;
}

static bool source_result_closures(
    const AST *ast, InferCtx *inference,
    uint64_t module_id, SourceClosures *summaries,
    SourceResults *results, QttSourceGradeStatus *status) {
    if (!ast) return true;
    if (ast->type == AST_LAMBDA)
        return source_result_append(
            results, module_id, ast->qtt_closure_id);
    if (ast->type != AST_LIST || !ast->list.count) return true;
    if (source_head_is(ast, "begin"))
        return ast->list.count == 1 ||
            source_result_closures(
                ast->list.items[ast->list.count - 1],
                inference, module_id, summaries, results, status);
    if (source_head_is(ast, "if"))
        return ast->list.count == 4 &&
            source_result_closures(
                ast->list.items[2], inference, module_id,
                summaries, results, status) &&
            source_result_closures(
                ast->list.items[3], inference, module_id,
                summaries, results, status);
    if (source_head_is(ast, "with") || source_head_is(ast, "let"))
        return ast->list.count < 3 ||
            source_result_closures(
                ast->list.items[ast->list.count - 1],
                inference, module_id, summaries, results, status);
    const InferGradeApplication *application =
        source_application(inference, ast);
    if (application) {
        for (size_t i = 0; i < application->result_closure_count; i++)
            if (!source_result_append(
                    results,
                    application->result_closure_module_ids[i],
                    application->result_closure_ids[i]))
                return false;
        for (size_t i = 0; i < application->closure_grade_count; i++) {
            size_t result_index = SIZE_MAX;
            for (size_t j = 0;
                 j < application->result_closure_count; j++)
                if (application->closure_module_ids[i] ==
                        application->result_closure_module_ids[j] &&
                    application->closure_ids[i] ==
                        application->result_closure_ids[j]) {
                    result_index = j;
                    break;
                }
            if (result_index == SIZE_MAX) continue;
            const QttClosureEnvironment *environment =
                application->result_closure_environments
                    ? application->result_closure_environments[result_index]
                    : NULL;
            size_t slot = application->closure_slots[i];
            if (!environment ||
                !application->result_closure_instance_ids ||
                environment->instance_id !=
                    application->result_closure_instance_ids[result_index] ||
                environment->module_id !=
                    application->result_closure_module_ids[result_index] ||
                environment->closure_id !=
                    application->result_closure_ids[result_index] ||
                !qtt_environment_validate(environment) ||
                slot >= environment->slot_count ||
                environment->origins[slot].kind ==
                    QTT_ENVIRONMENT_PARAMETER) {
                *status = QTT_SOURCE_GRADES_UNSUPPORTED;
                return false;
            }
            /*
             * Forward the existential origin itself, not the producer AST.
             * This preserves the local/expression distinction needed by
             * ownership lowering while root parameters remain substitutable.
             */
            uint64_t origin_identity =
                environment->origins[slot].identity;
            if (!source_closure_append(
                    summaries, application->closure_module_ids[i],
                    application->closure_ids[i],
                    slot, environment->origins[slot].kind, origin_identity,
                    application->closure_grades[i])) {
                *status = QTT_SOURCE_GRADES_OUT_OF_MEMORY;
                return false;
            }
        }
        for (size_t i = 0;
             i < application->closure_domain_grade_count; i++) {
            bool returned = false;
            for (size_t j = 0;
                 j < application->result_closure_count; j++)
                if (application->closure_domain_module_ids[i] ==
                        application->result_closure_module_ids[j] &&
                    application->closure_domain_ids[i] ==
                        application->result_closure_ids[j]) {
                    returned = true;
                    break;
                }
            if (returned && !source_closure_domain_append(
                    summaries, application->closure_domain_module_ids[i],
                    application->closure_domain_ids[i],
                    application->closure_domain_indices[i],
                    application->closure_domain_grades[i])) {
                *status = QTT_SOURCE_GRADES_OUT_OF_MEMORY;
                return false;
            }
        }
    }
    return true;
}

static void source_results_free(SourceResults *results) {
    if (!results) return;
    free(results->module_ids);
    free(results->closure_ids);
    memset(results, 0, sizeof(*results));
}

static QttUsageContext *source_usage(
    const AST *ast,
    InferCtx *inference,
    const uint64_t *targets,
    size_t target_count,
    SourceClosures *closures,
    QttSourceGradeStatus *status);

static QttUsageContext *source_sequence(
    AST *const *items,
    size_t count,
    InferCtx *inference,
    const uint64_t *targets,
    size_t target_count,
    SourceClosures *closures,
    QttSourceGradeStatus *status) {
    QttUsageContext *result = qtt_usage_empty(inference->grade_arena);
    if (!result) {
        *status = QTT_SOURCE_GRADES_OUT_OF_MEMORY;
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        QttUsageContext *item = source_usage(
            items[i], inference, targets, target_count, closures, status);
        QttUsageContext *next = item
            ? qtt_usage_sequence(
                  inference->grade_arena, result, item) : NULL;
        qtt_usage_context_free(item);
        qtt_usage_context_free(result);
        if (!next) {
            if (*status == QTT_SOURCE_GRADES_OK)
                *status = QTT_SOURCE_GRADES_OUT_OF_MEMORY;
            return NULL;
        }
        result = next;
    }
    return result;
}

static bool source_head_is(const AST *ast, const char *name) {
    return ast && ast->type == AST_LIST && ast->list.count &&
        ast->list.items[0] &&
        ast->list.items[0]->type == AST_SYMBOL &&
        strcmp(ast->list.items[0]->symbol, name) == 0;
}

static bool source_linear_primitive(const AST *head) {
    if (!head || head->type != AST_SYMBOL || !head->symbol) return false;
    static const char *const names[] = {
        "+", "-", "*", "/", "%", "=", "!=", "<", "<=", ">", ">=",
        "and", "or", "not", "show",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(head->symbol, names[i]) == 0) return true;
    return false;
}

typedef enum {
    SOURCE_PURE_TOTAL,
    SOURCE_PARTIAL,
    SOURCE_EFFECTFUL,
    SOURCE_EFFECT_UNKNOWN,
} SourceEffect;

static SourceEffect source_effect_join(SourceEffect left, SourceEffect right) {
    return left > right ? left : right;
}

static SourceEffect source_effect(const AST *ast) {
    if (!ast) return SOURCE_EFFECT_UNKNOWN;
    if (ast->type == AST_LAMBDA) return SOURCE_PURE_TOTAL;
    if (ast->type == AST_ARRAY) {
        SourceEffect result = SOURCE_PURE_TOTAL;
        for (size_t i = 0; i < ast->array.element_count; i++)
            result = source_effect_join(
                result, source_effect(ast->array.elements[i]));
        return result;
    }
    if (ast->type == AST_SET) {
        SourceEffect result = SOURCE_PURE_TOTAL;
        for (size_t i = 0; i < ast->set.element_count; i++)
            result = source_effect_join(
                result, source_effect(ast->set.elements[i]));
        return result;
    }
    if (ast->type != AST_LIST) return SOURCE_PURE_TOTAL;
    if (!ast->list.count) return SOURCE_EFFECT_UNKNOWN;
    if (source_head_is(ast, "quote")) return SOURCE_PURE_TOTAL;
    if (source_head_is(ast, "begin")) {
        SourceEffect result = SOURCE_PURE_TOTAL;
        for (size_t i = 1; i < ast->list.count; i++)
            result = source_effect_join(
                result, source_effect(ast->list.items[i]));
        return result;
    }
    if (source_head_is(ast, "if")) {
        if (ast->list.count != 4) return SOURCE_EFFECT_UNKNOWN;
        SourceEffect result = source_effect(ast->list.items[1]);
        result = source_effect_join(
            result, source_effect(ast->list.items[2]));
        return source_effect_join(
            result, source_effect(ast->list.items[3]));
    }
    if (source_head_is(ast, "with") || source_head_is(ast, "let")) {
        if (ast->list.count < 3 || !ast->list.items[1] ||
            ast->list.items[1]->type != AST_ARRAY)
            return SOURCE_EFFECT_UNKNOWN;
        SourceEffect result = SOURCE_PURE_TOTAL;
        const AST *bindings = ast->list.items[1];
        for (size_t i = 1; i < bindings->array.element_count; i += 2)
            result = source_effect_join(
                result, source_effect(bindings->array.elements[i]));
        for (size_t i = 2; i < ast->list.count; i++)
            result = source_effect_join(
                result, source_effect(ast->list.items[i]));
        return result;
    }
    const AST *head = ast->list.items[0];
    if (!head || head->type != AST_SYMBOL || !head->symbol)
        return SOURCE_EFFECT_UNKNOWN;
    if (strcmp(head->symbol, "show") == 0 ||
        strcmp(head->symbol, "set!") == 0)
        return SOURCE_EFFECTFUL;
    if (strcmp(head->symbol, "/") == 0 ||
        strcmp(head->symbol, "%") == 0)
        return SOURCE_PARTIAL;
    if (!source_linear_primitive(head)) return SOURCE_EFFECT_UNKNOWN;
    SourceEffect result = SOURCE_PURE_TOTAL;
    for (size_t i = 1; i < ast->list.count; i++)
        result = source_effect_join(
            result, source_effect(ast->list.items[i]));
    return result;
}

static QttUsageContext *source_force_callable_environment(
    const InferGradeApplication *producer,
    InferCtx *inference,
    const uint64_t *targets,
    size_t target_count,
    SourceClosures *closures,
    QttSourceGradeStatus *status) {
    if (!producer || !producer->result_closure_count ||
        !producer->result_closure_instance_ids ||
        !producer->result_closure_environments) {
        *status = QTT_SOURCE_GRADES_UNSUPPORTED;
        return NULL;
    }
    QttUsageContext *alternatives = NULL;
    for (size_t r = 0; r < producer->result_closure_count; r++) {
        uint64_t module_id = producer->result_closure_module_ids[r];
        uint64_t closure_id = producer->result_closure_ids[r];
        const QttClosureEnvironment *environment =
            producer->result_closure_environments[r];
        if (!environment ||
            environment->instance_id !=
                producer->result_closure_instance_ids[r] ||
            environment->module_id != module_id ||
            environment->closure_id != closure_id ||
            !qtt_environment_validate(environment) ||
            !source_invoked_instance_add(
                closures, environment->instance_id)) {
            *status = QTT_SOURCE_GRADES_UNSUPPORTED;
            qtt_usage_context_free(alternatives);
            return NULL;
        }
        size_t count = 0;
        for (size_t i = 0; i < producer->closure_grade_count; i++)
            if (producer->closure_module_ids[i] == module_id &&
                producer->closure_ids[i] == closure_id)
                count++;
        QttUsageContext **terms = count
            ? calloc(count, sizeof(*terms)) : NULL;
        QttGradeExpr **grades = count
            ? calloc(count, sizeof(*grades)) : NULL;
        QttUsageContext *empty =
            qtt_usage_empty(inference->grade_arena);
        if (!empty || (count && (!terms || !grades))) {
            qtt_usage_context_free(empty);
            free(terms);
            free(grades);
            qtt_usage_context_free(alternatives);
            *status = QTT_SOURCE_GRADES_OUT_OF_MEMORY;
            return NULL;
        }
        size_t write = 0;
        bool complete = true;
        for (size_t i = 0;
             complete && i < producer->closure_grade_count; i++) {
            if (producer->closure_module_ids[i] != module_id ||
                producer->closure_ids[i] != closure_id)
                continue;
            size_t slot = producer->closure_slots[i];
            if (slot >= environment->slot_count) {
                complete = false;
                break;
            }
            QttEnvironmentOrigin origin = environment->origins[slot];
            if (origin.kind == QTT_ENVIRONMENT_LOCAL)
                terms[write] = source_target(
                        origin.identity, targets, target_count)
                    ? qtt_usage_singleton(
                          inference->grade_arena, origin.identity)
                    : qtt_usage_empty(inference->grade_arena);
            else if (origin.kind == QTT_ENVIRONMENT_EXPRESSION)
                terms[write] = qtt_usage_empty(inference->grade_arena);
            else
                complete = false;
            if (complete) grades[write++] = producer->closure_grades[i];
            if (complete && !terms[write - 1]) complete = false;
        }
        QttUsageContext *alternative = complete
            ? qtt_usage_application(
                  inference->grade_arena, empty,
                  (const QttUsageContext *const *)terms, grades, write)
            : NULL;
        for (size_t i = 0; i < write; i++)
            qtt_usage_context_free(terms[i]);
        qtt_usage_context_free(empty);
        free(terms);
        free(grades);
        if (!alternative) {
            qtt_usage_context_free(alternatives);
            *status = complete
                ? QTT_SOURCE_GRADES_OUT_OF_MEMORY
                : QTT_SOURCE_GRADES_UNSUPPORTED;
            return NULL;
        }
        if (!alternatives)
            alternatives = alternative;
        else {
            QttUsageContext *joined = qtt_usage_choice(
                inference->grade_arena, alternatives, alternative);
            qtt_usage_context_free(alternatives);
            qtt_usage_context_free(alternative);
            alternatives = joined;
            if (!alternatives) {
                *status = QTT_SOURCE_GRADES_OUT_OF_MEMORY;
                return NULL;
            }
        }
    }
    return alternatives;
}

static QttGradeExpr *source_callable_actual_domain(
    const InferGradeApplication *producer, size_t domain_index,
    QttGradeArena *arena) {
    if (!producer || !producer->result_closure_count || !arena)
        return NULL;
    QttGradeExpr *result = NULL;
    for (size_t r = 0; r < producer->result_closure_count; r++) {
        QttGradeExpr *alternative = NULL;
        for (size_t i = 0;
             i < producer->closure_domain_grade_count; i++)
            if (producer->closure_domain_module_ids[i] ==
                    producer->result_closure_module_ids[r] &&
                producer->closure_domain_ids[i] ==
                    producer->result_closure_ids[r] &&
                producer->closure_domain_indices[i] == domain_index) {
                alternative = producer->closure_domain_grades[i];
                break;
            }
        if (!alternative) return NULL;
        result = result
            ? qtt_grade_maximum(arena, result, alternative)
            : alternative;
        if (!result) return NULL;
    }
    return result;
}

static QttUsageContext *source_list_usage(
    const AST *ast,
    InferCtx *inference,
    const uint64_t *targets,
    size_t target_count,
    SourceClosures *closures,
    QttSourceGradeStatus *status) {
    if (!ast->list.count) {
        *status = QTT_SOURCE_GRADES_MALFORMED;
        return NULL;
    }
    if (source_head_is(ast, "quote"))
        return qtt_usage_empty(inference->grade_arena);
    if (source_head_is(ast, "begin"))
        return source_sequence(
            ast->list.items + 1, ast->list.count - 1, inference,
            targets, target_count, closures, status);
    if ((source_head_is(ast, "with") || source_head_is(ast, "let")) &&
        ast->list.count >= 3 && ast->list.items[1] &&
        ast->list.items[1]->type == AST_ARRAY) {
        const AST *bindings = ast->list.items[1];
        if (bindings->array.element_count % 2 != 0) {
            *status = QTT_SOURCE_GRADES_MALFORMED;
            return NULL;
        }
        size_t binding_count = bindings->array.element_count / 2;
        uint64_t *extended = calloc(
            target_count + binding_count, sizeof(*extended));
        QttUsageContext **values = binding_count
            ? calloc(binding_count, sizeof(*values)) : NULL;
        if (!extended || (binding_count && !values)) {
            free(extended);
            free(values);
            *status = QTT_SOURCE_GRADES_OUT_OF_MEMORY;
            return NULL;
        }
        memcpy(extended, targets, target_count * sizeof(*extended));
        bool complete = true;
        size_t alias_scope = closures->alias_count;
        for (size_t i = 0; i < binding_count; i++) {
            const AST *name = bindings->array.elements[i * 2];
            if (!name || name->type != AST_SYMBOL ||
                !name->resolved_binder_id) {
                *status = QTT_SOURCE_GRADES_MALFORMED;
                complete = false;
                break;
            }
            extended[target_count + i] = name->resolved_binder_id;
        }
        for (size_t i = 0; complete && i < binding_count; i++) {
            const AST *value = bindings->array.elements[i * 2 + 1];
            const InferGradeApplication *alias =
                value && value->type == AST_LIST
                    ? source_application(inference, value)
                    : value && value->type == AST_SYMBOL
                        ? source_alias_application(
                              closures, value->resolved_binder_id)
                        : NULL;
            if (source_effect(value) != SOURCE_PURE_TOTAL &&
                !(alias && alias->result_closure_count)) {
                *status = QTT_SOURCE_GRADES_UNSUPPORTED;
                complete = false;
                break;
            }
            values[i] = source_usage(
                value, inference,
                extended, target_count + binding_count, closures, status);
            if (!values[i]) complete = false;
            if (complete && alias &&
                !source_alias_push(
                    closures, extended[target_count + i], alias)) {
                *status = QTT_SOURCE_GRADES_OUT_OF_MEMORY;
                complete = false;
            }
        }
        QttUsageContext *body = complete ? source_sequence(
            ast->list.items + 2, ast->list.count - 2, inference,
            extended, target_count + binding_count, closures, status) : NULL;
        closures->alias_count = alias_scope;
        for (size_t i = binding_count; body && i > 0; i--) {
            QttUsageContext *next = qtt_usage_let(
                inference->grade_arena,
                extended[target_count + i - 1],
                values[i - 1], body);
            qtt_usage_context_free(body);
            body = next;
            if (!body && *status == QTT_SOURCE_GRADES_OK)
                *status = QTT_SOURCE_GRADES_OUT_OF_MEMORY;
        }
        for (size_t i = 0; i < binding_count; i++)
            qtt_usage_context_free(values[i]);
        free(values);
        free(extended);
        return body;
    }
    if (source_head_is(ast, "if")) {
        if (ast->list.count != 4) {
            *status = QTT_SOURCE_GRADES_MALFORMED;
            return NULL;
        }
        QttUsageContext *condition = source_usage(
            ast->list.items[1], inference, targets, target_count,
            closures, status);
        QttUsageContext *then_usage = condition ? source_usage(
            ast->list.items[2], inference, targets, target_count,
            closures, status) : NULL;
        QttUsageContext *else_usage = then_usage ? source_usage(
            ast->list.items[3], inference, targets, target_count,
            closures, status) : NULL;
        QttUsageContext *branches = else_usage
            ? qtt_usage_choice(
                  inference->grade_arena, then_usage, else_usage) : NULL;
        QttUsageContext *result = branches
            ? qtt_usage_sequence(
                  inference->grade_arena, condition, branches) : NULL;
        qtt_usage_context_free(branches);
        qtt_usage_context_free(else_usage);
        qtt_usage_context_free(then_usage);
        qtt_usage_context_free(condition);
        if (!result && *status == QTT_SOURCE_GRADES_OK)
            *status = QTT_SOURCE_GRADES_OUT_OF_MEMORY;
        return result;
    }

    /*
     * A call through a root function parameter introduces one principal
     * symbolic grade per callable domain.  Open arguments are scaled by these
     * grades; a known actual closure specializes them at the outer call site.
     */
    const AST *head = ast->list.items[0];
    size_t callable_parameter = SIZE_MAX;
    if (head && head->type == AST_SYMBOL && head->resolved_binder_id)
        for (size_t i = 0; i < closures->root_parameter_count; i++)
            if (closures->root_parameters[i] ==
                head->resolved_binder_id) {
                callable_parameter = i;
                break;
            }
    if (callable_parameter != SIZE_MAX &&
        !source_alias_application(closures, head->resolved_binder_id)) {
        if (!source_callable_add(closures, callable_parameter)) {
            *status = QTT_SOURCE_GRADES_UNSUPPORTED;
            return NULL;
        }
        QttUsageContext *callee = qtt_usage_singleton(
            inference->grade_arena, head->resolved_binder_id);
        size_t argument_count = ast->list.count - 1;
        QttUsageContext **arguments = argument_count
            ? calloc(argument_count, sizeof(*arguments)) : NULL;
        QttGradeExpr **grades = argument_count
            ? calloc(argument_count, sizeof(*grades)) : NULL;
        bool complete = callee &&
            (!argument_count || (arguments && grades));
        for (size_t i = 0; complete && i < argument_count; i++) {
            arguments[i] = source_usage(
                ast->list.items[i + 1], inference,
                targets, target_count, closures, status);
            grades[i] = source_callable_domain(
                closures, inference->grade_arena,
                callable_parameter, i);
            if (!arguments[i] || !grades[i]) complete = false;
        }
        QttUsageContext *result = complete
            ? qtt_usage_application(
                  inference->grade_arena, callee,
                  (const QttUsageContext *const *)arguments,
                  grades, argument_count)
            : NULL;
        for (size_t i = 0; i < argument_count; i++)
            qtt_usage_context_free(arguments ? arguments[i] : NULL);
        qtt_usage_context_free(callee);
        free(arguments);
        free(grades);
        if (!result) {
            if (*status == QTT_SOURCE_GRADES_OK)
                *status = QTT_SOURCE_GRADES_OUT_OF_MEMORY;
            return NULL;
        }
        /*
         * Nullary calls have no domain evidence but still carry the
         * path-sensitive invocation grade through the callee occurrence.
         */
        return result;
    }

    /*
     * Higher-order invocation of a closure returned by a fully applied,
     * quantitatively-known producer.  Each possible result closure supplies
     * its own domain vector and capture-slot latent vector.  Alternative
     * closures are joined by maximum because exactly one value is returned.
     */
    const AST *producer_ast = ast->list.items[0];
    const InferGradeApplication *producer =
        producer_ast && producer_ast->type == AST_LIST
            ? source_application(inference, producer_ast)
            : producer_ast && producer_ast->type == AST_SYMBOL
                ? source_alias_application(
                      closures, producer_ast->resolved_binder_id)
                : NULL;
    if (producer && producer->result_closure_count) {
        size_t invocation_arity = ast->list.count - 1;
        QttUsageContext *alternatives = NULL;
        bool complete = true;
        for (size_t r = 0;
             complete && r < producer->result_closure_count; r++) {
            if (!producer->result_closure_instance_ids ||
                !source_invoked_instance_add(
                    closures,
                    producer->result_closure_instance_ids[r])) {
                *status = producer->result_closure_instance_ids
                    ? QTT_SOURCE_GRADES_OUT_OF_MEMORY
                    : QTT_SOURCE_GRADES_UNSUPPORTED;
                complete = false;
                break;
            }
            uint64_t module_id =
                producer->result_closure_module_ids[r];
            uint64_t closure_id = producer->result_closure_ids[r];
            const QttClosureEnvironment *environment =
                producer->result_closure_environments
                    ? producer->result_closure_environments[r] : NULL;
            if (!environment ||
                environment->instance_id !=
                    producer->result_closure_instance_ids[r] ||
                environment->module_id != module_id ||
                environment->closure_id != closure_id ||
                !qtt_environment_validate(environment)) {
                *status = QTT_SOURCE_GRADES_UNSUPPORTED;
                complete = false;
                break;
            }
            size_t capture_count = 0;
            for (size_t i = 0; i < producer->closure_grade_count; i++)
                if (producer->closure_module_ids[i] == module_id &&
                    producer->closure_ids[i] == closure_id)
                    capture_count++;
            size_t term_count = invocation_arity + capture_count;
            QttUsageContext **terms = term_count
                ? calloc(term_count, sizeof(*terms)) : NULL;
            QttGradeExpr **term_grades = term_count
                ? calloc(term_count, sizeof(*term_grades)) : NULL;
            QttUsageContext *empty =
                qtt_usage_empty(inference->grade_arena);
            if (!empty || (term_count && (!terms || !term_grades))) {
                qtt_usage_context_free(empty);
                free(terms);
                free(term_grades);
                *status = QTT_SOURCE_GRADES_OUT_OF_MEMORY;
                complete = false;
                break;
            }
            size_t write = 0;
            for (size_t i = 0; i < invocation_arity; i++) {
                QttGradeExpr *domain = NULL;
                for (size_t j = 0;
                     j < producer->closure_domain_grade_count; j++)
                    if (producer->closure_domain_module_ids[j] ==
                            module_id &&
                        producer->closure_domain_ids[j] == closure_id &&
                        producer->closure_domain_indices[j] == i) {
                        domain = producer->closure_domain_grades[j];
                        break;
                    }
                if (!domain) {
                    *status = QTT_SOURCE_GRADES_UNSUPPORTED;
                    complete = false;
                    break;
                }
                terms[write] = source_usage(
                    ast->list.items[i + 1], inference,
                    targets, target_count, closures, status);
                term_grades[write++] = domain;
                if (!terms[write - 1]) complete = false;
            }
            for (size_t i = 0;
                 complete && i < producer->closure_grade_count; i++) {
                if (producer->closure_module_ids[i] != module_id ||
                    producer->closure_ids[i] != closure_id)
                    continue;
                size_t slot = producer->closure_slots[i];
                if (slot >= environment->slot_count) {
                    *status = QTT_SOURCE_GRADES_UNSUPPORTED;
                    complete = false;
                    break;
                }
                QttEnvironmentOrigin origin = environment->origins[slot];
                if (origin.kind == QTT_ENVIRONMENT_LOCAL) {
                    terms[write] = source_target(
                            origin.identity, targets, target_count)
                        ? qtt_usage_singleton(
                              inference->grade_arena, origin.identity)
                        : qtt_usage_empty(inference->grade_arena);
                } else if (origin.kind == QTT_ENVIRONMENT_EXPRESSION) {
                    /*
                     * Construction already accounts for evaluating the
                     * argument expression. Invocation consumes its captured
                     * result and must not replay that expression's demand.
                     */
                    terms[write] =
                        qtt_usage_empty(inference->grade_arena);
                } else {
                    *status = QTT_SOURCE_GRADES_UNSUPPORTED;
                    complete = false;
                    break;
                }
                term_grades[write++] = producer->closure_grades[i];
                if (!terms[write - 1]) complete = false;
            }
            QttUsageContext *alternative = complete
                ? qtt_usage_application(
                      inference->grade_arena, empty,
                      (const QttUsageContext *const *)terms,
                      term_grades, write)
                : NULL;
            for (size_t i = 0; i < write; i++)
                qtt_usage_context_free(terms[i]);
            qtt_usage_context_free(empty);
            free(terms);
            free(term_grades);
            if (complete && !alternative) {
                *status = QTT_SOURCE_GRADES_OUT_OF_MEMORY;
                complete = false;
            }
            if (alternative && !alternatives)
                alternatives = alternative;
            else if (alternative) {
                QttUsageContext *joined = qtt_usage_choice(
                    inference->grade_arena, alternatives, alternative);
                qtt_usage_context_free(alternatives);
                qtt_usage_context_free(alternative);
                alternatives = joined;
                if (!alternatives) {
                    *status = QTT_SOURCE_GRADES_OUT_OF_MEMORY;
                    complete = false;
                }
            }
        }
        QttUsageContext *callee = complete ? source_usage(
            producer_ast, inference, targets, target_count,
            closures, status) : NULL;
        QttUsageContext *result =
            callee && alternatives ? qtt_usage_sequence(
                inference->grade_arena, callee, alternatives) : NULL;
        qtt_usage_context_free(callee);
        qtt_usage_context_free(alternatives);
        if (!result && *status == QTT_SOURCE_GRADES_OK)
            *status = QTT_SOURCE_GRADES_OUT_OF_MEMORY;
        return result;
    }

    size_t argument_count = ast->list.count - 1;
    QttUsageContext *callee = source_usage(
        ast->list.items[0], inference, targets, target_count, closures, status);
    QttUsageContext **arguments = argument_count
        ? calloc(argument_count, sizeof(*arguments)) : NULL;
    QttGradeExpr **grades = argument_count
        ? calloc(argument_count, sizeof(*grades)) : NULL;
    if (!callee || (argument_count && (!arguments || !grades))) {
        if (*status == QTT_SOURCE_GRADES_OK)
            *status = QTT_SOURCE_GRADES_OUT_OF_MEMORY;
        qtt_usage_context_free(callee);
        free(arguments);
        free(grades);
        return NULL;
    }
    const InferGradeApplication *application =
        source_application(inference, ast);
    bool primitive = source_linear_primitive(ast->list.items[0]);
    if (argument_count && !application && !primitive) {
        *status = QTT_SOURCE_GRADES_UNSUPPORTED;
        qtt_usage_context_free(callee);
        free(arguments);
        free(grades);
        return NULL;
    }
    bool complete = true;
    for (size_t i = 0; i < argument_count; i++) {
        arguments[i] = source_usage(
            ast->list.items[i + 1], inference,
            targets, target_count, closures, status);
        grades[i] = application && i < application->applied_count
            ? application->domain_grades[i]
            : qtt_grade_constant(
                  inference->grade_arena, qtt_quantity_finite(1));
        if (!arguments[i] || !grades[i]) complete = false;
    }
    /*
     * A callable value is constructed once under call-by-value.  Its
     * invocation grade scales the latent environment separately below; using
     * that grade here as well would count closure construction q times.
     */
    if (application)
        for (size_t i = 0;
             complete && i < application->callable_parameter_count; i++) {
            size_t parameter =
                application->callable_parameter_indices[i];
            if (parameter >= argument_count) {
                complete = false;
                *status = QTT_SOURCE_GRADES_UNSUPPORTED;
                break;
            }
            grades[parameter] = qtt_grade_constant(
                inference->grade_arena, qtt_quantity_finite(1));
            if (!grades[parameter]) {
                complete = false;
                *status = QTT_SOURCE_GRADES_OUT_OF_MEMORY;
            }
        }
    if (application)
        for (size_t i = 0;
             complete && i < application->callable_domain_count; i++) {
            size_t parameter =
                application->callable_domain_parameter_indices[i];
            if (parameter >= argument_count ||
                !application->callable_domain_grades ||
                qtt_grade_expr_kind(
                    application->callable_domain_grades[i]) !=
                    QTT_GRADE_VARIABLE) {
                complete = false;
                *status = QTT_SOURCE_GRADES_UNSUPPORTED;
                break;
            }
            const AST *actual = ast->list.items[parameter + 1];
            const InferGradeApplication *producer =
                actual && actual->type == AST_LIST
                    ? source_application(inference, actual)
                    : actual && actual->type == AST_SYMBOL
                        ? source_alias_application(
                              closures, actual->resolved_binder_id)
                        : NULL;
            QttGradeExpr *replacement =
                source_callable_actual_domain(
                    producer,
                    application->callable_domain_indices[i],
                    inference->grade_arena);
            if (!replacement) {
                complete = false;
                *status = QTT_SOURCE_GRADES_UNSUPPORTED;
                break;
            }
            for (size_t j = 0; j < argument_count; j++) {
                grades[j] = qtt_grade_substitute(
                    inference->grade_arena, grades[j],
                    application->callable_domain_grades[i],
                    replacement);
                if (!grades[j]) {
                    complete = false;
                    *status = QTT_SOURCE_GRADES_OUT_OF_MEMORY;
                    break;
                }
            }
        }
    QttUsageContext *result = complete
        ? qtt_usage_application(
              inference->grade_arena, callee,
              (const QttUsageContext *const *)arguments,
              grades, argument_count)
        : NULL;
    for (size_t i = 0;
         result && application &&
         i < application->callable_parameter_count; i++) {
        size_t parameter =
            application->callable_parameter_indices[i];
        if (parameter >= argument_count) {
            *status = QTT_SOURCE_GRADES_UNSUPPORTED;
            qtt_usage_context_free(result);
            result = NULL;
            break;
        }
        const AST *actual = ast->list.items[parameter + 1];
        const InferGradeApplication *producer =
            actual && actual->type == AST_LIST
                ? source_application(inference, actual)
                : actual && actual->type == AST_SYMBOL
                    ? source_alias_application(
                          closures, actual->resolved_binder_id)
                    : NULL;
        QttUsageContext *latent = producer
            ? source_force_callable_environment(
                  producer, inference, targets, target_count,
                  closures, status)
            : NULL;
        QttGradeExpr *invocations =
            application->callable_invocation_grades
                ? application->callable_invocation_grades[i] : NULL;
        QttUsageContext *forced = latent && invocations
            ? qtt_usage_scale(
                  inference->grade_arena, invocations, latent)
            : NULL;
        QttUsageContext *next = forced
            ? qtt_usage_sequence(
                  inference->grade_arena, result, forced)
            : NULL;
        qtt_usage_context_free(forced);
        qtt_usage_context_free(latent);
        qtt_usage_context_free(result);
        result = next;
        if (!result && *status == QTT_SOURCE_GRADES_OK)
            *status = producer
                ? QTT_SOURCE_GRADES_OUT_OF_MEMORY
                : QTT_SOURCE_GRADES_UNSUPPORTED;
    }
    for (size_t i = 0; i < argument_count; i++)
        qtt_usage_context_free(arguments[i]);
    qtt_usage_context_free(callee);
    free(arguments);
    free(grades);
    if (!result && *status == QTT_SOURCE_GRADES_OK)
        *status = QTT_SOURCE_GRADES_OUT_OF_MEMORY;
    return result;
}

static QttUsageContext *source_usage(
    const AST *ast,
    InferCtx *inference,
    const uint64_t *targets,
    size_t target_count,
    SourceClosures *closures,
    QttSourceGradeStatus *status) {
    if (!ast || !inference || *status != QTT_SOURCE_GRADES_OK)
        return NULL;
    switch (ast->type) {
    case AST_SYMBOL:
        return source_target(
                   ast->resolved_binder_id, targets, target_count)
            ? qtt_usage_singleton(
                  inference->grade_arena, ast->resolved_binder_id)
            : qtt_usage_empty(inference->grade_arena);
    case AST_LIST:
        return source_list_usage(
            ast, inference, targets, target_count, closures, status);
    case AST_ARRAY:
        return source_sequence(
            ast->array.elements, ast->array.element_count, inference,
            targets, target_count, closures, status);
    case AST_SET:
        return source_sequence(
            ast->set.elements, ast->set.element_count, inference,
            targets, target_count, closures, status);
    case AST_LAMBDA: {
        size_t parameter_count = (size_t)ast->lambda.param_count;
        uint64_t *extended = calloc(
            target_count + parameter_count, sizeof(*extended));
        if (!extended) {
            *status = QTT_SOURCE_GRADES_OUT_OF_MEMORY;
            return NULL;
        }
        memcpy(extended, targets, target_count * sizeof(*extended));
        bool complete = true;
        for (size_t i = 0; i < parameter_count; i++) {
            extended[target_count + i] =
                ast->lambda.params[i].binder_id;
            if (!extended[target_count + i]) {
                *status = QTT_SOURCE_GRADES_MALFORMED;
                complete = false;
            }
        }
        QttUsageContext *body = complete ? source_sequence(
            ast->lambda.body_exprs, (size_t)ast->lambda.body_count,
            inference, extended, target_count + parameter_count,
            closures, status) : NULL;
        QttUsageClosure *closure = body ? qtt_usage_closure(
            inference->grade_arena, extended + target_count,
            parameter_count, body) : NULL;
        for (size_t i = 0; body && i < parameter_count; i++) {
            QttGradeExpr *grade = qtt_usage_grade(
                body, extended[target_count + i]);
            if (!grade)
                grade = qtt_grade_constant(
                    inference->grade_arena, qtt_quantity_finite(0));
            if (!grade || !source_closure_domain_append(
                    closures, closures->module_id,
                    ast->qtt_closure_id, i, grade)) {
                *status = QTT_SOURCE_GRADES_OUT_OF_MEMORY;
                qtt_usage_closure_free(closure);
                closure = NULL;
                break;
            }
        }
        QttUsageContext *empty = closure
            ? qtt_usage_empty(inference->grade_arena) : NULL;
        QttUsageContext *captures = empty ? qtt_usage_sequence(
            inference->grade_arena,
            qtt_usage_closure_captures(closure), empty) : NULL;
        const QttUsageContext *latent = closure
            ? qtt_usage_closure_latent(closure) : NULL;
        for (size_t i = 0;
             closure && i < qtt_usage_binding_count(latent);
             i++) {
            if (!source_closure_append(
                    closures, closures->module_id, ast->qtt_closure_id,
                    i, QTT_ENVIRONMENT_LOCAL,
                    qtt_usage_binding_id(latent, i),
                    qtt_usage_binding_grade(latent, i))) {
                *status = QTT_SOURCE_GRADES_OUT_OF_MEMORY;
                qtt_usage_context_free(captures);
                captures = NULL;
                break;
            }
        }
        qtt_usage_context_free(empty);
        qtt_usage_closure_free(closure);
        qtt_usage_context_free(body);
        free(extended);
        if (!captures && *status == QTT_SOURCE_GRADES_OK)
            *status = QTT_SOURCE_GRADES_OUT_OF_MEMORY;
        return captures;
    }
    default:
        return qtt_usage_empty(inference->grade_arena);
    }
}

QttSourceGradeResult qtt_source_grade_scheme(
    const AST *lambda, InferCtx *inference, uint64_t module_id) {
    QttSourceGradeResult result = {
        .status = QTT_SOURCE_GRADES_MALFORMED,
    };
    if (!lambda || lambda->type != AST_LAMBDA || !inference || !module_id)
        return result;
    result.count = (size_t)lambda->lambda.param_count;
    uint64_t *targets = result.count
        ? calloc(result.count, sizeof(*targets)) : NULL;
    QttGradeExpr **expressions = result.count
        ? calloc(result.count, sizeof(*expressions)) : NULL;
    result.solved = result.count
        ? calloc(result.count, sizeof(*result.solved)) : NULL;
    if (result.count && (!targets || !expressions || !result.solved)) {
        result.status = QTT_SOURCE_GRADES_OUT_OF_MEMORY;
        free(targets);
        free(expressions);
        qtt_source_grade_result_free(&result);
        return result;
    }
    for (size_t i = 0; i < result.count; i++) {
        targets[i] = lambda->lambda.params[i].binder_id;
        if (!targets[i]) {
            result.status = QTT_SOURCE_GRADES_MALFORMED;
            free(targets);
            free(expressions);
            qtt_source_grade_result_free(&result);
            return result;
        }
    }
    result.status = QTT_SOURCE_GRADES_OK;
    SourceClosures closures = {
        .module_id = module_id,
        .root_parameters = targets,
        .root_parameter_count = result.count,
    };
    SourceResults result_closures = {0};
    QttUsageContext *body = source_sequence(
        lambda->lambda.body_exprs, (size_t)lambda->lambda.body_count,
        inference, targets, result.count, &closures, &result.status);
    if (!body) {
        free(targets);
        free(expressions);
        source_closures_free(&closures);
        source_results_free(&result_closures);
        qtt_source_grade_result_free(&result);
        return result;
    }
    if (lambda->lambda.body_count > 0 &&
        !source_result_closures(
            lambda->lambda.body_exprs[lambda->lambda.body_count - 1],
            inference, module_id, &closures, &result_closures,
            &result.status) &&
        result.status == QTT_SOURCE_GRADES_OK)
        result.status = QTT_SOURCE_GRADES_OUT_OF_MEMORY;
    for (size_t i = 0; i < result.count; i++) {
        expressions[i] = qtt_usage_grade(body, targets[i]);
        if (!expressions[i])
            expressions[i] = qtt_grade_constant(
                inference->grade_arena, qtt_quantity_finite(0));
        if (!expressions[i]) result.status = QTT_SOURCE_GRADES_OUT_OF_MEMORY;
    }
    if (result.status == QTT_SOURCE_GRADES_OK) {
        QttGradeExpr **callable_invocations =
            closures.callable_parameter_count
                ? calloc(closures.callable_parameter_count,
                         sizeof(*callable_invocations))
                : NULL;
        if (closures.callable_parameter_count && !callable_invocations)
            result.status = QTT_SOURCE_GRADES_OUT_OF_MEMORY;
        for (size_t i = 0;
             result.status == QTT_SOURCE_GRADES_OK &&
             i < closures.callable_parameter_count; i++)
            callable_invocations[i] =
                expressions[closures.callable_parameter_indices[i]];
        QttGradeSignature signature = {
            .domain_expressions = expressions,
            .domain_count = result.count,
            .closure_module_ids = closures.module_ids,
            .closure_ids = closures.closure_ids,
            .closure_binder_ids = closures.binder_ids,
            .closure_slots = closures.slots,
            .closure_parameter_indices = closures.parameter_indices,
            .closure_origin_kinds = closures.origin_kinds,
            .closure_origin_ids = closures.origin_ids,
            .closure_expressions = closures.grades,
            .closure_count = closures.count,
            .closure_domain_module_ids = closures.domain_module_ids,
            .closure_domain_ids = closures.domain_closure_ids,
            .closure_domain_indices = closures.domain_indices,
            .closure_domain_expressions = closures.domain_grades,
            .closure_domain_count = closures.domain_count,
            .result_closure_module_ids = result_closures.module_ids,
            .result_closure_ids = result_closures.closure_ids,
            .result_closure_count = result_closures.count,
            .callable_parameter_indices =
                closures.callable_parameter_indices,
            .callable_invocation_expressions = callable_invocations,
            .callable_parameter_count =
                closures.callable_parameter_count,
            .callable_domain_parameter_indices =
                closures.callable_domain_parameter_indices,
            .callable_domain_indices =
                closures.callable_domain_indices,
            .callable_domain_expressions =
                closures.callable_domain_grades,
            .callable_domain_count =
                closures.callable_domain_count,
        };
        if (result.status == QTT_SOURCE_GRADES_OK)
            result.scheme = qtt_grade_generalize_signature(
                inference->grade_arena, &signature);
        free(callable_invocations);
    }
    result.closure_count = closures.count;
    result.closure_domain_count = closures.domain_count;
    result.invoked_closure_instance_count =
        closures.invoked_instance_count;
    for (size_t i = 0; i < closures.count; i++)
        if (closures.parameter_indices[i] != SIZE_MAX)
            result.substitutable_closure_count++;
    result.result_closure_count = result_closures.count;
    result.callable_parameter_count =
        closures.callable_parameter_count;
    result.callable_domain_count = closures.callable_domain_count;
    if (result.status == QTT_SOURCE_GRADES_OK && closures.count) {
        result.closure_solved =
            calloc(closures.count, sizeof(*result.closure_solved));
        if (!result.closure_solved)
            result.status = QTT_SOURCE_GRADES_OUT_OF_MEMORY;
    }
    if (result.status == QTT_SOURCE_GRADES_OK && !result.scheme)
        result.status = QTT_SOURCE_GRADES_OUT_OF_MEMORY;
    if (result.status == QTT_SOURCE_GRADES_OK) {
        QttGradeSolver *solver =
            qtt_grade_solver_new(inference->grade_arena);
        if (!solver || qtt_grade_solve(solver) != QTT_GRADE_SOLVED)
            result.status = QTT_SOURCE_GRADES_OUT_OF_MEMORY;
        else
            for (size_t i = 0; i < result.count; i++)
                result.solved[i] =
                    qtt_grade_solution(solver, expressions[i]);
        if (solver && result.status == QTT_SOURCE_GRADES_OK)
            for (size_t i = 0; i < closures.count; i++)
                result.closure_solved[i] =
                    qtt_grade_solution(solver, closures.grades[i]);
        qtt_grade_solver_free(solver);
    }
    qtt_usage_context_free(body);
    free(targets);
    free(expressions);
    source_closures_free(&closures);
    source_results_free(&result_closures);
    if (result.status != QTT_SOURCE_GRADES_OK)
        qtt_source_grade_result_free(&result);
    return result;
}

void qtt_source_grade_result_free(QttSourceGradeResult *result) {
    if (!result) return;
    qtt_grade_scheme_free(result->scheme);
    free(result->solved);
    free(result->closure_solved);
    result->scheme = NULL;
    result->solved = NULL;
    result->closure_solved = NULL;
    result->count = 0;
    result->closure_count = 0;
    result->closure_domain_count = 0;
    result->invoked_closure_instance_count = 0;
    result->result_closure_count = 0;
}

QttGradeScheme *qtt_shadow_grade_scheme(
    const AST *lambda, uint64_t module_id) {
    if (!lambda || lambda->type != AST_LAMBDA ||
        lambda->lambda.body_count != 1)
        return NULL;
    QttCoreError core_error = QTT_CORE_OK;
    QttCoreNode *core =
        qtt_core_lower(lambda, module_id, &core_error);
    if (!core) return NULL;
    QttSignatureError signature_error = QTT_SIGNATURE_OK;
    QttFunctionSignature *signature =
        qtt_signature_derive(core, &signature_error);
    if (!signature) {
        qtt_core_free(core);
        return NULL;
    }
    QttGradeArena *arena = qtt_grade_arena_new();
    QttGradeExpr **domains = signature->parameter_count
        ? calloc(signature->parameter_count, sizeof(*domains)) : NULL;
    if (!arena || (signature->parameter_count && !domains)) {
        free(domains);
        qtt_grade_arena_free(arena);
        qtt_signature_free(signature);
        qtt_core_free(core);
        return NULL;
    }
    bool complete = true;
    for (size_t i = 0; i < signature->parameter_count; i++) {
        domains[i] = qtt_grade_constant(
            arena, signature->parameters[i].quantity);
        if (!domains[i]) complete = false;
    }
    QttGradeScheme *scheme = complete
        ? qtt_grade_generalize(
              arena, domains, signature->parameter_count)
        : NULL;
    free(domains);
    qtt_grade_arena_free(arena);
    qtt_signature_free(signature);
    qtt_core_free(core);
    return scheme;
}

static void shadow_demand_free(QttShadowDemandEvidence *demand) {
    if (!demand) return;
    for (size_t i = 0; i < demand->premise_count; i++)
        shadow_demand_free(demand->premises[i]);
    free(demand->premises);
    free(demand);
}

static uint64_t shadow_demand_path_id(
    uint64_t parent, size_t index, uint64_t structural) {
    uint64_t hash = parent ? parent : UINT64_C(1469598103934665603);
    hash = (hash ^ (uint64_t)(index + 1)) * UINT64_C(1099511628211);
    hash = (hash ^ structural) * UINT64_C(1099511628211);
    return hash ? hash : 1;
}

static bool shadow_subject_is_parameter(
    const QttCoreNode *source, QttCoreVar subject) {
    if (!source) return false;
    const QttCoreNode *lambda = source->kind == QTT_CORE_LAMBDA
        ? source
        : source->kind == QTT_CORE_APPLY ? source->apply.callee : NULL;
    if (!lambda || lambda->kind != QTT_CORE_LAMBDA) return false;
    for (size_t i = 0; i < lambda->lambda.param_count; i++)
        if (qtt_core_var_equal(lambda->lambda.params[i], subject))
            return true;
    return false;
}

static QttShadowDemandEvidence *shadow_demand_snapshot_at(
    const QttDemandDerivation *derivation,
    uint64_t parent_id, size_t child_index, bool root) {
    if (!derivation) return NULL;
    QttShadowDemandEvidence *snapshot =
        calloc(1, sizeof(*snapshot));
    if (!snapshot) return NULL;
    uint64_t structural =
        qtt_demand_derivation_node_fingerprint(derivation);
    snapshot->node_id = root
        ? structural
        : shadow_demand_path_id(parent_id, child_index, structural);
    snapshot->rule = derivation->rule;
    snapshot->syntactic = derivation->syntactic;
    snapshot->runtime = derivation->runtime;
    const QttCoreNode *source = derivation->source;
    snapshot->syntactic_premise_count =
        source && source->kind == QTT_CORE_LET ? 2
        : source && source->kind == QTT_CORE_APPLY
            ? 1 + source->apply.argument_count
        : derivation->premise_count;
    snapshot->local_use =
        source &&
        ((source->kind == QTT_CORE_VAR &&
          qtt_core_var_equal(source->var, derivation->subject)) ||
         (source->kind == QTT_CORE_PLACE &&
          qtt_core_var_equal(source->place.root,
                             derivation->subject)));
    if (source && source->kind == QTT_CORE_WRITE &&
        qtt_core_var_equal(source->write.place.root, derivation->subject))
        snapshot->local_use = true;
    if (source && source->kind == QTT_CORE_BORROW &&
        qtt_core_var_equal(source->borrow.place.root,
                           derivation->subject))
        snapshot->local_use = true;
    snapshot->subject_is_binding =
        source && source->kind == QTT_CORE_LET &&
        qtt_core_var_equal(source->let.binding, derivation->subject);
    snapshot->subject_is_parameter =
        shadow_subject_is_parameter(source, derivation->subject);
    snapshot->direct_application =
        source && source->kind == QTT_CORE_APPLY &&
        source->apply.callee &&
        source->apply.callee->kind == QTT_CORE_LAMBDA &&
        source->apply.argument_count ==
            source->apply.callee->lambda.param_count;
    snapshot->application_argument_count =
        source && source->kind == QTT_CORE_APPLY
            ? source->apply.argument_count : 0;
    snapshot->premise_count = derivation->premise_count;
    snapshot->premises = derivation->premise_count
        ? calloc(derivation->premise_count,
                 sizeof(*snapshot->premises))
        : NULL;
    if (derivation->premise_count && !snapshot->premises) {
        shadow_demand_free(snapshot);
        return NULL;
    }
    for (size_t i = 0; i < derivation->premise_count; i++) {
        snapshot->premises[i] =
            shadow_demand_snapshot_at(
                derivation->premises[i], snapshot->node_id, i, false);
        if (!snapshot->premises[i]) {
            shadow_demand_free(snapshot);
            return NULL;
        }
    }
    return snapshot;
}

static QttShadowDemandEvidence *shadow_demand_snapshot(
    const QttDemandDerivation *derivation) {
    return shadow_demand_snapshot_at(derivation, 0, 0, true);
}

static QttShadowResult shadow_verify_lambda(
    const AST *lambda, uint64_t module_id, QttShadowEvidence *evidence,
    const QttSignatureEnv *signatures) {
    QttShadowResult result = {
        .status = QTT_SHADOW_UNSUPPORTED,
        .core_error = QTT_CORE_OK,
        .core_validation = QTT_CORE_VALID,
        .lower_error = QTT_ANF_LOWER_OK,
        .verification_error = QTT_ANF_VALID,
    };
    if (lambda && lambda->type == AST_LAMBDA) {
        result.parameter_count = (size_t)lambda->lambda.param_count;
        result.body_count = (size_t)lambda->lambda.body_count;
    }
    if (!lambda || lambda->type != AST_LAMBDA ||
        lambda->lambda.body_count != 1)
        return result;

    bool closed_body = false;
    QttCoreNode *core = qtt_core_lower(
        closed_body ? lambda->lambda.body_exprs[0] : lambda,
        module_id, &result.core_error);
    if (!core) {
        result.status =
            result.core_error == QTT_CORE_UNSUPPORTED_AST
                ? QTT_SHADOW_UNSUPPORTED : QTT_SHADOW_INTERNAL_ERROR;
        return result;
    }
    result.core_lowered = true;
    result.core_validation = qtt_core_validate(core, module_id);
    if (result.core_validation != QTT_CORE_VALID) {
        result.status = QTT_SHADOW_INTERNAL_ERROR;
        qtt_core_free(core);
        return result;
    }
    result.core_validated = true;
    QttTypeArena *type_arena = qtt_type_arena_new();
    if (!type_arena) {
        result.status = QTT_SHADOW_INTERNAL_ERROR;
        qtt_core_free(core);
        return result;
    }

    QttFunctionSignature *signature = NULL;
    QttAnfProgram *anf = NULL;
    if (closed_body) {
        QttTypeIdentityError identity_error = QTT_TYPE_IDENTITY_OK;
        if (!qtt_type_intern(type_arena, core->type, &identity_error).value) {
            result.status = QTT_SHADOW_INTERNAL_ERROR;
            qtt_type_arena_free(type_arena);
            qtt_core_free(core);
            return result;
        }
        anf = qtt_anf_lower_core(core, &result.lower_error);
        result.signature_validated = true;
    } else {
        QttSignatureError signature_error = QTT_SIGNATURE_OK;
        signature = qtt_signature_derive(core, &signature_error);
        if (!signature) {
            result.status = signature_error == QTT_SIGNATURE_OUT_OF_MEMORY
                ? QTT_SHADOW_INTERNAL_ERROR : QTT_SHADOW_UNSUPPORTED;
            qtt_type_arena_free(type_arena);
            qtt_core_free(core);
            return result;
        }
        QttSignatureCanonicalization canonical =
            qtt_signature_canonicalize(signature, type_arena);
        if (canonical != QTT_SIGNATURE_CANONICAL) {
            if (getenv("MONAD_QTT_DEBUG"))
                fprintf(stderr, "[qtt-debug] signature canonical=%d\n",
                        canonical);
            result.status =
                canonical == QTT_SIGNATURE_CANONICAL_OUT_OF_MEMORY
                    ? QTT_SHADOW_INTERNAL_ERROR : QTT_SHADOW_UNSUPPORTED;
            qtt_signature_free(signature);
            qtt_type_arena_free(type_arena);
            qtt_core_free(core);
            return result;
        }
        if (qtt_signature_validate_grades(signature, type_arena) !=
            QTT_GRADED_PROOF_VALID) {
            if (getenv("MONAD_QTT_DEBUG"))
                fprintf(stderr, "[qtt-debug] graded proof invalid\n");
            result.status = QTT_SHADOW_INTERNAL_ERROR;
            qtt_signature_free(signature);
            qtt_type_arena_free(type_arena);
            qtt_core_free(core);
            return result;
        }
        result.signature_validated = true;
        result.graded_parameter_count = signature->parameter_count;
        for (size_t i = 0; i < signature->parameter_count; i++)
            if (qtt_graded_is_erased(
                    signature->graded, signature->parameters[i].var))
                result.erased_parameter_count++;
        if (evidence) {
            evidence->parameter_count = signature->parameter_count;
            evidence->result_mode = signature->result.mode;
            evidence->result_origin = signature->result.origin;
            evidence->result_representation =
                signature->result.representation;
            evidence->result_type_id = signature->result.type_id;
            evidence->contract_fingerprint =
                signature->contract_fingerprint;
            evidence->parameters = signature->parameter_count
                ? calloc(signature->parameter_count,
                         sizeof(*evidence->parameters))
                : NULL;
            if (signature->parameter_count && !evidence->parameters) {
                evidence->details_complete = false;
                evidence->parameter_count = 0;
            } else {
                for (size_t i = 0; i < signature->parameter_count; i++) {
                    QttParameterContract *source =
                        &signature->parameters[i];
                    QttShadowParameterEvidence *target =
                        &evidence->parameters[i];
                    target->var = source->var;
                    target->allowance = source->quantity;
                    target->observed = source->observed;
                    target->mode = source->mode;
                    target->representation = source->representation;
                    target->type_id = source->type_id;
                    QttDemandError demand_error = QTT_DEMAND_OK;
                    QttDemandDerivation *derivation =
                        qtt_demand_prove(
                            signature->demand,
                            core->lambda.body,
                            source->var,
                            &demand_error);
                    if (derivation)
                        target->demand =
                            shadow_demand_snapshot(derivation);
                    qtt_demand_derivation_free(derivation);
                    if (!target->demand)
                        evidence->details_complete = false;
                }
            }
        }
        anf = qtt_anf_lower_function_in_env(
            core, signature, signatures, module_id,
            &result.lower_error);
    }
    result.canonical_type_count = qtt_type_arena_count(type_arena);
    if (!anf) {
        result.status =
            result.lower_error == QTT_ANF_LOWER_UNSUPPORTED_CORE ||
            result.lower_error == QTT_ANF_LOWER_MALFORMED_LITERAL
                ? QTT_SHADOW_UNSUPPORTED : QTT_SHADOW_INTERNAL_ERROR;
        qtt_signature_free(signature);
        qtt_type_arena_free(type_arena);
        qtt_core_free(core);
        return result;
    }
    result.anf_lowered = true;
    QttAnfVerification verification = qtt_anf_infer_loan_regions(anf);
    result.verification_ran = true;
    result.verification_error = verification.error;
    result.block_count = anf->block_count;
    for (size_t i = 0; i < anf->block_count; i++)
        result.instruction_count += anf->blocks[i].instruction_count;
    if (evidence) {
        evidence->block_count = anf->block_count;
        evidence->blocks = anf->block_count
            ? calloc(anf->block_count, sizeof(*evidence->blocks))
            : NULL;
        if (anf->block_count && !evidence->blocks) {
            evidence->details_complete = false;
            evidence->block_count = 0;
        } else {
            for (size_t i = 0; i < anf->block_count; i++) {
                QttAnfBlock *source = &anf->blocks[i];
                QttShadowBlockEvidence *target = &evidence->blocks[i];
                target->instruction_count = source->instruction_count;
                target->parameter_count = source->parameter_count;
                target->terminator = source->terminator.kind;
                target->terminator_value = source->terminator.value;
                target->target = source->terminator.target;
                target->else_target = source->terminator.else_target;
                target->argument_count =
                    source->terminator.argument_count;
                target->else_argument_count =
                    source->terminator.else_argument_count;
                target->successor_count =
                    source->terminator.kind == QTT_ANF_BRANCH ? 2
                    : source->terminator.kind == QTT_ANF_JUMP ? 1 : 0;
                target->parameters = source->parameter_count
                    ? malloc(source->parameter_count *
                             sizeof(*target->parameters))
                    : NULL;
                target->instructions = source->instruction_count
                    ? malloc(source->instruction_count *
                             sizeof(*target->instructions))
                    : NULL;
                target->arguments = source->terminator.argument_count
                    ? malloc(source->terminator.argument_count *
                             sizeof(*target->arguments))
                    : NULL;
                target->else_arguments =
                    source->terminator.else_argument_count
                    ? malloc(source->terminator.else_argument_count *
                             sizeof(*target->else_arguments))
                    : NULL;
                if ((source->parameter_count && !target->parameters) ||
                    (source->instruction_count &&
                     !target->instructions) ||
                    (source->terminator.argument_count &&
                     !target->arguments) ||
                    (source->terminator.else_argument_count &&
                     !target->else_arguments)) {
                    evidence->details_complete = false;
                    target->instruction_count = 0;
                    target->parameter_count = 0;
                    target->argument_count = 0;
                    target->else_argument_count = 0;
                    continue;
                }
                if (source->parameter_count)
                    memcpy(target->parameters, source->parameters,
                           source->parameter_count *
                           sizeof(*target->parameters));
                if (source->terminator.argument_count)
                    memcpy(target->arguments,
                           source->terminator.arguments,
                           source->terminator.argument_count *
                           sizeof(*target->arguments));
                if (source->terminator.else_argument_count)
                    memcpy(target->else_arguments,
                           source->terminator.else_arguments,
                           source->terminator.else_argument_count *
                           sizeof(*target->else_arguments));
                for (size_t j = 0; j < source->instruction_count; j++) {
                    QttAnfInstruction *instruction =
                        &source->instructions[j];
                    QttShadowInstructionEvidence *detail =
                        &target->instructions[j];
                    detail->kind = instruction->kind;
                    detail->result = instruction->result;
                    detail->operand = instruction->operand;
                    detail->resource = instruction->resource;
                    detail->loan_id = instruction->loan_id;
                    detail->parent_loan_id =
                        instruction->parent_loan_id;
                    detail->loan_kind = instruction->loan_kind;
                    detail->result_resource =
                        instruction->result_resource;
                    detail->representation =
                        instruction->representation;
                    detail->callable = instruction->callable;
                    detail->call_argument_count =
                        instruction->call_argument_count;
                    detail->place = instruction->place;
                }
            }
        }
    }
    result.status = verification.error == QTT_ANF_VALID
        ? QTT_SHADOW_VERIFIED : QTT_SHADOW_INTERNAL_ERROR;
    qtt_anf_program_free(anf);
    qtt_signature_free(signature);
    qtt_type_arena_free(type_arena);
    qtt_core_free(core);
    return result;
}

QttShadowResult qtt_shadow_verify_lambda(const AST *lambda,
                                         uint64_t module_id) {
    return shadow_verify_lambda(lambda, module_id, NULL, NULL);
}

QttShadowEvidence qtt_shadow_collect_lambda(
    const AST *lambda, uint64_t module_id) {
    QttShadowEvidence evidence = {.details_complete = true};
    evidence.summary =
        shadow_verify_lambda(lambda, module_id, &evidence, NULL);
    return evidence;
}

QttShadowEvidence qtt_shadow_collect_lambda_in_env(
    const AST *lambda, uint64_t module_id,
    const QttSignatureEnv *signatures) {
    QttShadowEvidence evidence = {.details_complete = true};
    evidence.summary = shadow_verify_lambda(
        lambda, module_id, &evidence, signatures);
    return evidence;
}

void qtt_shadow_evidence_free(QttShadowEvidence *evidence) {
    if (!evidence) return;
    for (size_t i = 0; i < evidence->parameter_count; i++)
        shadow_demand_free(evidence->parameters[i].demand);
    for (size_t i = 0; i < evidence->block_count; i++)
        free(evidence->blocks[i].parameters);
    for (size_t i = 0; i < evidence->block_count; i++)
        free(evidence->blocks[i].instructions);
    for (size_t i = 0; i < evidence->block_count; i++) {
        free(evidence->blocks[i].arguments);
        free(evidence->blocks[i].else_arguments);
    }
    free(evidence->blocks);
    free(evidence->parameters);
    memset(evidence, 0, sizeof(*evidence));
}

const char *qtt_shadow_status_name(QttShadowStatus status) {
    if (status == QTT_SHADOW_VERIFIED) return "verified";
    if (status == QTT_SHADOW_UNSUPPORTED) return "unsupported";
    return "internal-error";
}
