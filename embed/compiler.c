#include "include/monad/compiler.h"
#include "include/monad/qtt.h"
#include "frontend_transaction.h"
#include "compiler_internal.h"
#include "../infer.h"
#include "../qtt/foreign_type.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The stable facade follows the same opaque-session boundary used by LLVM's C
 * APIs, while keeping LLVM and compiler arena layouts out of public headers:
 * https://llvm.org/docs/ORCv2.html
 */

struct monad_compiler_session {
    pthread_t owner;
    MonadFrontendState *frontend;
    QttTypeArena *types;
    QttForeignTypeEnv *foreign_types;
    monad_compiler_foreign_type_info_t *infos;
    size_t count;
    Type byte_type;
    Type handle_type;
};

struct monad_compiler_source {
    size_t byte_count;
    uint64_t version;
    char *bytes;
    char name[];
};

struct monad_compiler_diagnostic_set {
    size_t count;
    monad_compiler_diagnostic_t item;
    char message[512];
    char source_name[];
};

struct monad_compiler_environment {
    size_t count;
    monad_compiler_definition_t *definitions;
    char *source_name;
    size_t index_capacity;
    size_t *name_index;
    size_t *id_index;
    struct monad_qtt_report *qtt_report;
};

struct monad_qtt_report {
    size_t count;
    monad_qtt_definition_evidence_t *definitions;
};

/* Immutable open-addressed indexes keep editor lookup independent of the
 * number of definitions. The design follows LLVM DenseMap's dense lookup
 * boundary, while retaining exact string comparison after hashing:
 * https://llvm.org/doxygen/classllvm_1_1DenseMap.html
 */

/* Source units follow the owned-copy boundary used by LLVM memory buffers:
 * https://llvm.org/doxygen/classllvm_1_1MemoryBuffer.html
 * The stable facade owns bytes instead of exposing the parser's mutable global
 * source context, following Clang's source-manager ownership separation:
 * https://clang.llvm.org/doxygen/classclang_1_1SourceManager.html
 */

static monad_status_t compiler_fail(
    monad_error_t *error, monad_status_t status, const char *message) {
    if (error) {
        error->status = status;
        error->message = message;
    }
    return status;
}

static void compiler_clear_error(monad_error_t *error) {
    if (error) {
        error->status = MONAD_OK;
        error->message = NULL;
    }
}

uint32_t monad_compiler_abi_version(void) {
    return MONAD_COMPILER_ABI_VERSION;
}

void monad_compiler_source_descriptor_init(
    monad_compiler_source_descriptor_t *descriptor) {
    if (!descriptor) return;
    *descriptor = (monad_compiler_source_descriptor_t){
        .struct_size = sizeof(*descriptor),
        .abi_version = MONAD_COMPILER_SOURCE_ABI_VERSION,
    };
}

monad_status_t monad_compiler_source_create(
    const monad_compiler_source_descriptor_t *descriptor,
    monad_compiler_source_t **source, monad_error_t *error) {
    compiler_clear_error(error);
    if (!source)
        return compiler_fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                             "source output is required");
    *source = NULL;
    if (!descriptor ||
        descriptor->struct_size < sizeof(monad_compiler_source_descriptor_t))
        return compiler_fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                             "source descriptor is smaller than required");
    if (descriptor->abi_version != MONAD_COMPILER_SOURCE_ABI_VERSION)
        return compiler_fail(error, MONAD_ERROR_ABI_MISMATCH,
                             "source descriptor ABI is not supported");
    if (!descriptor->name || !descriptor->name[0] ||
        (!descriptor->bytes && descriptor->byte_count != 0))
        return compiler_fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                             "source name and bytes are invalid");
    if (descriptor->byte_count &&
        memchr(descriptor->bytes, '\0', descriptor->byte_count))
        return compiler_fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                             "source bytes cannot contain an embedded NUL");

    size_t name_size = strlen(descriptor->name) + 1;
    if (name_size == 0 || name_size > SIZE_MAX - sizeof(monad_compiler_source_t))
        return compiler_fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                             "source name is too large");
    monad_compiler_source_t *created =
        malloc(sizeof(*created) + name_size);
    if (!created)
        return compiler_fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                             "could not allocate source unit");
    if (descriptor->byte_count == SIZE_MAX) {
        free(created);
        return compiler_fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                             "source buffer is too large");
    }
    created->bytes = malloc(descriptor->byte_count + 1);
    if (!created->bytes) {
        free(created);
        return compiler_fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                             "could not copy source bytes");
    }
    memcpy(created->name, descriptor->name, name_size);
    if (descriptor->byte_count)
        memcpy(created->bytes, descriptor->bytes, descriptor->byte_count);
    created->bytes[descriptor->byte_count] = '\0';
    created->byte_count = descriptor->byte_count;
    created->version = descriptor->version;
    *source = created;
    return MONAD_OK;
}

void monad_compiler_source_destroy(monad_compiler_source_t *source) {
    if (!source) return;
    free(source->bytes);
    free(source);
}

const char *monad_compiler_source_name(const monad_compiler_source_t *source) {
    return source ? source->name : NULL;
}

const char *monad_compiler_source_bytes(const monad_compiler_source_t *source) {
    return source ? source->bytes : NULL;
}

size_t monad_compiler_source_byte_count(const monad_compiler_source_t *source) {
    return source ? source->byte_count : 0;
}

uint64_t monad_compiler_source_version(const monad_compiler_source_t *source) {
    return source ? source->version : 0;
}

/* Diagnostics are stable data rather than preformatted terminal output, as in
 * Clang's diagnostic/source-location separation:
 * https://clang.llvm.org/docs/InternalsManual.html#the-diagnostics-subsystem
 * UTF-8 validity follows RFC 3629's shortest-form, scalar-value range:
 * https://www.rfc-editor.org/rfc/rfc3629#section-4
 */
static int compiler_utf8_invalid_offset(
    const unsigned char *bytes, size_t count, size_t *offset) {
    size_t i = 0;
    while (i < count) {
        unsigned char a = bytes[i];
        size_t width = 0;
        if (a <= 0x7f) { i++; continue; }
        if (a >= 0xc2 && a <= 0xdf) width = 2;
        else if (a >= 0xe0 && a <= 0xef) width = 3;
        else if (a >= 0xf0 && a <= 0xf4) width = 4;
        else { *offset = i; return 1; }
        if (width > count - i) { *offset = i; return 1; }
        for (size_t j = 1; j < width; j++)
            if ((bytes[i + j] & 0xc0) != 0x80) {
                *offset = i;
                return 1;
            }
        if ((a == 0xe0 && bytes[i + 1] < 0xa0) ||
            (a == 0xed && bytes[i + 1] >= 0xa0) ||
            (a == 0xf0 && bytes[i + 1] < 0x90) ||
            (a == 0xf4 && bytes[i + 1] >= 0x90)) {
            *offset = i;
            return 1;
        }
        i += width;
    }
    return 0;
}

static size_t compiler_source_offset(
    const monad_compiler_source_t *source, int line, int column) {
    size_t offset = 0;
    int current_line = 1;
    while (offset < source->byte_count && current_line < line) {
        if (source->bytes[offset++] == '\n') current_line++;
    }
    int current_column = 1;
    while (offset < source->byte_count && source->bytes[offset] != '\n' &&
           current_column < column) {
        offset++;
        current_column++;
    }
    return offset;
}

static uint64_t compiler_definition_id(
    const char *source_name, const char *name) {
    /* FNV-1a is a deterministic compact index key, never type authority or a
     * substitute for exact source/name comparison:
     * https://www.ietf.org/archive/id/draft-eastlake-fnv-25.html
     */
    uint64_t hash = UINT64_C(14695981039346656037);
    const char *parts[] = {source_name, "\xff", name};
    for (size_t i = 0; i < 3; i++)
        for (const unsigned char *p = (const unsigned char *)parts[i]; *p; p++) {
            hash ^= *p;
            hash *= UINT64_C(1099511628211);
        }
    return hash ? hash : 1;
}

static uint64_t compiler_name_hash(const char *name) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        hash ^= *p;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static size_t compiler_id_slot(uint64_t id, size_t mask) {
    id ^= id >> 33;
    id *= UINT64_C(0xff51afd7ed558ccd);
    id ^= id >> 33;
    return (size_t)id & mask;
}

static bool compiler_environment_build_indexes(
    monad_compiler_environment_t *environment) {
    size_t capacity = 8;
    if (environment->count > SIZE_MAX / 2) return false;
    size_t minimum_capacity = environment->count * 2;
    while (capacity < minimum_capacity) {
        if (capacity > SIZE_MAX / 2) return false;
        capacity *= 2;
    }
    environment->name_index = calloc(capacity, sizeof(size_t));
    environment->id_index = calloc(capacity, sizeof(size_t));
    if (!environment->name_index || !environment->id_index) return false;
    environment->index_capacity = capacity;
    size_t mask = capacity - 1;
    for (size_t i = 0; i < environment->count; i++) {
        const monad_compiler_definition_t *definition =
            &environment->definitions[i];
        size_t slot = (size_t)compiler_name_hash(definition->name) & mask;
        while (environment->name_index[slot] &&
               strcmp(environment->definitions[
                          environment->name_index[slot] - 1].name,
                      definition->name) != 0)
            slot = (slot + 1) & mask;
        environment->name_index[slot] = i + 1;
        slot = compiler_id_slot(definition->id, mask);
        while (environment->id_index[slot] &&
               environment->definitions[environment->id_index[slot] - 1].id !=
                   definition->id)
            slot = (slot + 1) & mask;
        environment->id_index[slot] = i + 1;
    }
    return true;
}

static monad_compiler_environment_t *compiler_environment_create(
    const MonadCompilationUnit *unit,
    const monad_compiler_source_t *source) {
    size_t count = monad_compilation_unit_definition_count(unit);
    monad_compiler_environment_t *environment = calloc(1, sizeof(*environment));
    if (!environment) return NULL;
    environment->source_name = malloc(strlen(source->name) + 1);
    environment->definitions = count
        ? calloc(count, sizeof(*environment->definitions)) : NULL;
    environment->qtt_report = calloc(1, sizeof(*environment->qtt_report));
    if (environment->qtt_report)
        environment->qtt_report->definitions = count
            ? calloc(count, sizeof(*environment->qtt_report->definitions))
            : NULL;
    if (!environment->source_name || !environment->qtt_report ||
        (count && (!environment->definitions ||
                   !environment->qtt_report->definitions))) {
        monad_compiler_environment_destroy(environment);
        return NULL;
    }
    strcpy(environment->source_name, source->name);
    environment->count = count;
    for (size_t i = 0; i < count; i++) {
        const char *name = NULL;
        const TypeScheme *scheme = NULL;
        const AST *ast = NULL;
        const char *docstring = NULL;
        if (!monad_compilation_unit_definition_at(
                unit, i, &name, &scheme, &ast, &docstring)) {
            monad_compiler_environment_destroy(environment);
            return NULL;
        }
        char *owned_name = malloc(strlen(name) + 1);
        char *owned_scheme = infer_type_scheme_serialize(scheme);
        char *owned_docstring = docstring ? malloc(strlen(docstring) + 1) : NULL;
        if (!owned_name || !owned_scheme || (docstring && !owned_docstring)) {
            free(owned_name);
            free(owned_scheme);
            free(owned_docstring);
            monad_compiler_environment_destroy(environment);
            return NULL;
        }
        strcpy(owned_name, name);
        if (owned_docstring) strcpy(owned_docstring, docstring);
        size_t start = compiler_source_offset(source, ast->line, ast->column);
        Type *type = scheme ? scheme->type : NULL;
        environment->definitions[i] = (monad_compiler_definition_t){
            .struct_size = sizeof(monad_compiler_definition_t),
            .id = compiler_definition_id(source->name, name),
            .kind = type && type->kind == TYPE_ARROW
                ? MONAD_COMPILER_DEFINITION_FUNCTION
                : MONAD_COMPILER_DEFINITION_VALUE,
            .name = owned_name,
            .principal_scheme = owned_scheme,
            .source_name = environment->source_name,
            .source_version = source->version,
            .byte_start = start,
            .byte_end = source->byte_count,
            .line = ast->line > 0 ? (uint32_t)ast->line : 1,
            .column = ast->column > 0 ? (uint32_t)ast->column : 1,
            .docstring = owned_docstring,
        };
        InferCallableContract contract = {0};
        char *portable_contract = NULL;
        if (infer_callable_contract_from_scheme(&contract, scheme)) {
            portable_contract = infer_callable_contract_serialize(&contract);
            infer_callable_contract_free(&contract);
        }
        const QttQuantity *parameter_quantities = NULL;
        const QttQuantity *closure_quantities = NULL;
        size_t parameter_quantity_count = 0;
        size_t closure_quantity_count = 0;
        monad_compilation_unit_definition_quantities(
            unit, i, &parameter_quantities, &parameter_quantity_count,
            &closure_quantities, &closure_quantity_count);
        monad_qtt_quantity_t *owned_parameters = parameter_quantity_count
            ? calloc(parameter_quantity_count, sizeof(*owned_parameters))
            : NULL;
        monad_qtt_quantity_t *owned_closures = closure_quantity_count
            ? calloc(closure_quantity_count, sizeof(*owned_closures))
            : NULL;
        if ((parameter_quantity_count && !owned_parameters) ||
            (closure_quantity_count && !owned_closures)) {
            free(portable_contract);
            free(owned_parameters);
            free(owned_closures);
            monad_compiler_environment_destroy(environment);
            return NULL;
        }
        for (size_t q = 0; q < parameter_quantity_count; q++)
            owned_parameters[q] = (monad_qtt_quantity_t){
                .finite = parameter_quantities[q].finite,
                .is_omega = parameter_quantities[q].is_omega,
            };
        for (size_t q = 0; q < closure_quantity_count; q++)
            owned_closures[q] = (monad_qtt_quantity_t){
                .finite = closure_quantities[q].finite,
                .is_omega = closure_quantities[q].is_omega,
            };
        MonadQttResourceSummary resource = {0};
        monad_compilation_unit_definition_resource(unit, i, &resource);
        monad_qtt_representation_t result_representation =
            resource.result_representation >= 0 &&
            resource.result_representation <= 4
                ? (monad_qtt_representation_t)
                      (resource.result_representation + 1)
                : MONAD_QTT_REP_UNKNOWN;
        environment->qtt_report->definitions[i] =
            (monad_qtt_definition_evidence_t){
                .struct_size = sizeof(monad_qtt_definition_evidence_t),
                .definition_id = environment->definitions[i].id,
                .name = owned_name,
                .stage = resource.verified
                    ? MONAD_QTT_EVIDENCE_RESOURCE_VERIFIED
                    : MONAD_QTT_EVIDENCE_INFERRED,
                .quantitative_usage_available =
                    parameter_quantity_count != 0,
                .parameter_usage_count = parameter_quantity_count,
                .closure_usage_count = closure_quantity_count,
                .effects_complete = scheme ? scheme->effects_complete : 0,
                .effect_fingerprint = scheme_effect_fingerprint(scheme),
                .portable_effect_contract = portable_contract,
                .resource_verified = resource.verified,
                .destructor_plan_available =
                    resource.destructor_plan_available,
                .parameter_usages = owned_parameters,
                .closure_usages = owned_closures,
                .result_representation = result_representation,
                .resource_block_count = resource.block_count,
                .resource_instruction_count = resource.instruction_count,
                .resource_certificate_fingerprint =
                    resource.certificate_fingerprint,
            };
    }
    environment->qtt_report->count = count;
    for (size_t i = 0; i + 1 < count; i++)
        environment->definitions[i].byte_end =
            environment->definitions[i + 1].byte_start;
    if (!compiler_environment_build_indexes(environment)) {
        monad_compiler_environment_destroy(environment);
        return NULL;
    }
    return environment;
}

static monad_status_t compiler_analyze_source_impl(
    monad_compiler_session_t *session, const monad_compiler_source_t *source,
    monad_compiler_diagnostic_set_t **diagnostics,
    monad_compiler_environment_t **environment,
    monad_compiler_unit_hook_t hook, void **product,
    monad_error_t *error) {
    compiler_clear_error(error);
    if (!diagnostics || !environment)
        return compiler_fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                             "diagnostic and environment outputs are required");
    *diagnostics = NULL;
    *environment = NULL;
    if (product) *product = NULL;
    if (!session || !source)
        return compiler_fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                             "compiler session and source are required");
    if (!pthread_equal(session->owner, pthread_self()))
        return compiler_fail(error, MONAD_ERROR_WRONG_THREAD,
                             "compiler session is thread-affine");

    size_t name_size = strlen(source->name) + 1;
    if (name_size == 0 || name_size > SIZE_MAX - sizeof(monad_compiler_diagnostic_set_t))
        return compiler_fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                             "diagnostic source name is too large");
    monad_compiler_diagnostic_set_t *created =
        calloc(1, sizeof(*created) + name_size);
    if (!created)
        return compiler_fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                             "could not allocate diagnostic set");
    memcpy(created->source_name, source->name, name_size);

    size_t invalid = 0;
    if (compiler_utf8_invalid_offset(
            (const unsigned char *)source->bytes, source->byte_count, &invalid)) {
        uint32_t line = 1, column = 1;
        for (size_t i = 0; i < invalid; i++) {
            if (source->bytes[i] == '\n') { line++; column = 1; }
            else column++;
        }
        created->count = 1;
        created->item = (monad_compiler_diagnostic_t){
            .struct_size = sizeof(monad_compiler_diagnostic_t),
            .severity = MONAD_COMPILER_DIAGNOSTIC_ERROR,
            .phase = MONAD_COMPILER_PHASE_INPUT,
            .code = "MONAD-C0001",
            .source_name = created->source_name,
            .source_version = source->version,
            .byte_start = invalid,
            .byte_end = invalid + 1,
            .line = line,
            .column = column,
            .message = "source is not valid UTF-8",
        };
    } else {
        MonadCompilationUnit *unit =
            monad_compilation_unit_begin(session->frontend);
        if (!unit) {
            free(created);
            return compiler_fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                                 "could not create compilation unit");
        }
        MonadFrontendResult parsed;
        bool accepted = monad_compilation_unit_parse(
            unit, source->bytes, source->name, &parsed);
        monad_compiler_phase_t phase = MONAD_COMPILER_PHASE_PARSE;
        const char *code = "MONAD-C0002";
        if (accepted) {
            accepted = monad_compilation_unit_infer(unit, &parsed);
            phase = MONAD_COMPILER_PHASE_TYPE;
            code = "MONAD-C0003";
        }
        if (!accepted) {
            if (!parsed.has_diagnostic) {
                monad_compilation_unit_destroy(unit);
                free(created);
                return compiler_fail(error, MONAD_ERROR_INTERNAL,
                                     "compilation phase failed without a diagnostic");
            }
            snprintf(created->message, sizeof(created->message), "%s",
                     parsed.diagnostic.message);
            size_t start = compiler_source_offset(
                source, parsed.diagnostic.line, parsed.diagnostic.column);
            size_t end = compiler_source_offset(
                source, parsed.diagnostic.line, parsed.diagnostic.end_column);
            if (end < start) end = start;
            if (end == start && end < source->byte_count) end++;
            created->count = 1;
            created->item = (monad_compiler_diagnostic_t){
                .struct_size = sizeof(monad_compiler_diagnostic_t),
                .severity = MONAD_COMPILER_DIAGNOSTIC_ERROR,
                .phase = phase,
                .code = code,
                .source_name = created->source_name,
                .source_version = source->version,
                .byte_start = start,
                .byte_end = end,
                .line = parsed.diagnostic.line > 0
                    ? (uint32_t)parsed.diagnostic.line : 1,
                .column = parsed.diagnostic.column > 0
                    ? (uint32_t)parsed.diagnostic.column : 1,
                .message = created->message,
            };
        } else {
            *environment = compiler_environment_create(unit, source);
            if (!*environment) {
                monad_compilation_unit_destroy(unit);
                free(created);
                return compiler_fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                                     "could not create environment snapshot");
            }
            if (hook) {
                const char *failed_definition = NULL;
                if (!product || !hook(unit, product, &failed_definition)) {
                    monad_compiler_environment_destroy(*environment);
                    *environment = NULL;
                    monad_compilation_unit_destroy(unit);
                    free(created);
                    return compiler_fail(
                        error, MONAD_ERROR_TYPE_MISMATCH,
                        failed_definition
                            ? "definition is not yet in the native embedding ABI subset"
                            : "could not build native source image");
                }
            }
        }
        monad_compilation_unit_destroy(unit);
    }
    *diagnostics = created;
    return MONAD_OK;
}

monad_status_t monad_compiler_analyze_source(
    monad_compiler_session_t *session, const monad_compiler_source_t *source,
    monad_compiler_diagnostic_set_t **diagnostics,
    monad_compiler_environment_t **environment, monad_error_t *error) {
    return compiler_analyze_source_impl(
        session, source, diagnostics, environment, NULL, NULL, error);
}

monad_status_t monad_compiler_analyze_source_hook(
    monad_compiler_session_t *session, const monad_compiler_source_t *source,
    monad_compiler_diagnostic_set_t **diagnostics,
    monad_compiler_environment_t **environment,
    monad_compiler_unit_hook_t hook, void **product,
    monad_error_t *error) {
    if (!hook || !product)
        return compiler_fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                             "compiler hook and product output are required");
    return compiler_analyze_source_impl(
        session, source, diagnostics, environment, hook, product, error);
}

monad_status_t monad_compiler_check_source(
    monad_compiler_session_t *session, const monad_compiler_source_t *source,
    monad_compiler_diagnostic_set_t **diagnostics, monad_error_t *error) {
    monad_compiler_environment_t *environment = NULL;
    monad_status_t status = monad_compiler_analyze_source(
        session, source, diagnostics, &environment, error);
    monad_compiler_environment_destroy(environment);
    return status;
}

void monad_compiler_diagnostic_set_destroy(
    monad_compiler_diagnostic_set_t *diagnostics) {
    free(diagnostics);
}

size_t monad_compiler_diagnostic_count(
    const monad_compiler_diagnostic_set_t *diagnostics) {
    return diagnostics ? diagnostics->count : 0;
}

const monad_compiler_diagnostic_t *monad_compiler_diagnostic_at(
    const monad_compiler_diagnostic_set_t *diagnostics, size_t index) {
    if (!diagnostics || index >= diagnostics->count) return NULL;
    return &diagnostics->item;
}

void monad_compiler_environment_destroy(
    monad_compiler_environment_t *environment) {
    if (!environment) return;
    for (size_t i = 0; i < environment->count; i++) {
        free((char *)environment->definitions[i].name);
        free((char *)environment->definitions[i].principal_scheme);
        free((char *)environment->definitions[i].docstring);
        if (environment->qtt_report)
            free((char *)environment->qtt_report->definitions[i]
                     .portable_effect_contract);
        if (environment->qtt_report) {
            free((void *)environment->qtt_report->definitions[i]
                     .parameter_usages);
            free((void *)environment->qtt_report->definitions[i]
                     .closure_usages);
        }
    }
    if (environment->qtt_report) {
        free(environment->qtt_report->definitions);
        free(environment->qtt_report);
    }
    free(environment->name_index);
    free(environment->id_index);
    free(environment->definitions);
    free(environment->source_name);
    free(environment);
}

size_t monad_compiler_environment_count(
    const monad_compiler_environment_t *environment) {
    return environment ? environment->count : 0;
}

const monad_compiler_definition_t *monad_compiler_environment_at(
    const monad_compiler_environment_t *environment, size_t index) {
    if (!environment || index >= environment->count) return NULL;
    return &environment->definitions[index];
}

const monad_qtt_report_t *monad_compiler_environment_qtt_report(
    const monad_compiler_environment_t *environment) {
    return environment ? environment->qtt_report : NULL;
}

size_t monad_qtt_report_count(const monad_qtt_report_t *report) {
    return report ? report->count : 0;
}

const monad_qtt_definition_evidence_t *monad_qtt_report_at(
    const monad_qtt_report_t *report, size_t index) {
    if (!report || index >= report->count) return NULL;
    return &report->definitions[index];
}

const monad_qtt_definition_evidence_t *monad_qtt_report_find_id(
    const monad_qtt_report_t *report, uint64_t definition_id) {
    if (!report || !definition_id) return NULL;
    for (size_t i = 0; i < report->count; i++)
        if (report->definitions[i].definition_id == definition_id)
            return &report->definitions[i];
    return NULL;
}

const monad_qtt_quantity_t *monad_qtt_definition_parameter_usage(
    const monad_qtt_definition_evidence_t *evidence, size_t index) {
    if (!evidence || index >= evidence->parameter_usage_count) return NULL;
    return &evidence->parameter_usages[index];
}

const monad_qtt_quantity_t *monad_qtt_definition_closure_usage(
    const monad_qtt_definition_evidence_t *evidence, size_t index) {
    if (!evidence || index >= evidence->closure_usage_count) return NULL;
    return &evidence->closure_usages[index];
}

const monad_compiler_definition_t *monad_compiler_environment_find_name(
    const monad_compiler_environment_t *environment, const char *name) {
    if (!environment || !name || !environment->index_capacity) return NULL;
    size_t mask = environment->index_capacity - 1;
    size_t slot = (size_t)compiler_name_hash(name) & mask;
    while (environment->name_index[slot]) {
        size_t index = environment->name_index[slot] - 1;
        if (strcmp(environment->definitions[index].name, name) == 0)
            return &environment->definitions[index];
        slot = (slot + 1) & mask;
    }
    return NULL;
}

const monad_compiler_definition_t *monad_compiler_environment_find_id(
    const monad_compiler_environment_t *environment, uint64_t id) {
    if (!environment || !id || !environment->index_capacity) return NULL;
    size_t mask = environment->index_capacity - 1;
    size_t slot = compiler_id_slot(id, mask);
    while (environment->id_index[slot]) {
        size_t index = environment->id_index[slot] - 1;
        if (environment->definitions[index].id == id)
            return &environment->definitions[index];
        slot = (slot + 1) & mask;
    }
    return NULL;
}

void monad_compiler_definition_filter_init(
    monad_compiler_definition_filter_t *filter) {
    if (!filter) return;
    *filter = (monad_compiler_definition_filter_t){
        .struct_size = sizeof(*filter),
    };
}

static bool compiler_definition_matches(
    const monad_compiler_definition_t *definition,
    const monad_compiler_definition_filter_t *filter) {
    if (!filter) return true;
    if (filter->struct_size < sizeof(*filter)) return false;
    if (filter->kind && definition->kind != filter->kind) return false;
    if (filter->name_prefix &&
        strncmp(definition->name, filter->name_prefix,
                strlen(filter->name_prefix)) != 0)
        return false;
    if (filter->principal_scheme &&
        strcmp(definition->principal_scheme, filter->principal_scheme) != 0)
        return false;
    return true;
}

size_t monad_compiler_environment_matching_count(
    const monad_compiler_environment_t *environment,
    const monad_compiler_definition_filter_t *filter) {
    if (!environment) return 0;
    size_t count = 0;
    for (size_t i = 0; i < environment->count; i++)
        if (compiler_definition_matches(&environment->definitions[i], filter))
            count++;
    return count;
}

const monad_compiler_definition_t *monad_compiler_environment_matching_at(
    const monad_compiler_environment_t *environment,
    const monad_compiler_definition_filter_t *filter, size_t index) {
    if (!environment) return NULL;
    for (size_t i = 0; i < environment->count; i++) {
        if (!compiler_definition_matches(&environment->definitions[i], filter))
            continue;
        if (!index) return &environment->definitions[i];
        index--;
    }
    return NULL;
}

monad_status_t monad_compiler_session_create(
    monad_compiler_session_t **session, monad_error_t *error) {
    compiler_clear_error(error);
    if (!session)
        return compiler_fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                             "compiler session output is required");
    *session = NULL;
    monad_compiler_session_t *created = calloc(1, sizeof(*created));
    if (!created)
        return compiler_fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                             "could not allocate compiler session");
    created->owner = pthread_self();
    created->frontend = monad_frontend_state_create();
    created->byte_type.kind = TYPE_BYTE;
    created->handle_type.kind = TYPE_PTR;
    created->handle_type.element_type = &created->byte_type;
    created->types = qtt_type_arena_new();
    created->foreign_types = qtt_foreign_type_env_new(created->types);
    if (!created->frontend || !created->types || !created->foreign_types) {
        monad_frontend_state_destroy(created->frontend);
        qtt_foreign_type_env_free(created->foreign_types);
        qtt_type_arena_free(created->types);
        free(created);
        return compiler_fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                             "could not initialize compiler type environment");
    }
    *session = created;
    return MONAD_OK;
}

void monad_compiler_session_destroy(monad_compiler_session_t *session) {
    if (!session) return;
    monad_frontend_state_destroy(session->frontend);
    qtt_foreign_type_env_free(session->foreign_types);
    qtt_type_arena_free(session->types);
    free(session->infos);
    free(session);
}

monad_status_t monad_compiler_import_foreign_types(
    monad_compiler_session_t *session,
    const monad_binding_snapshot_t *snapshot, monad_error_t *error) {
    compiler_clear_error(error);
    if (!session || !snapshot)
        return compiler_fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                             "compiler session and snapshot are required");
    if (!pthread_equal(session->owner, pthread_self()))
        return compiler_fail(error, MONAD_ERROR_WRONG_THREAD,
                             "compiler session is thread-affine");

    QttTypeArena *next_types = qtt_type_arena_new();
    QttForeignTypeEnv *next_foreign = qtt_foreign_type_env_new(next_types);
    size_t capacity = monad_binding_snapshot_count(snapshot);
    monad_compiler_foreign_type_info_t *next_infos =
        capacity ? calloc(capacity, sizeof(*next_infos)) : NULL;
    if (!next_types || !next_foreign || (capacity && !next_infos)) {
        qtt_foreign_type_env_free(next_foreign);
        qtt_type_arena_free(next_types);
        free(next_infos);
        return compiler_fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                             "could not stage foreign type transaction");
    }

    size_t count = 0;
    for (size_t i = 0; i < capacity; i++) {
        const monad_binding_info_t *binding =
            monad_binding_snapshot_at(snapshot, i);
        if (!binding || binding->kind != MONAD_BINDING_FOREIGN_TYPE) continue;
        QttOwnershipMode ownership =
            binding->foreign_ownership == MONAD_FOREIGN_UNIQUE
                ? QTT_OWNERSHIP_CONSUMED
                : binding->foreign_ownership == MONAD_FOREIGN_SHARED
                    ? QTT_OWNERSHIP_SHARED : QTT_OWNERSHIP_ERASED;
        QttNominalAuthority authority = {
            binding->foreign_type_identity.runtime,
            binding->foreign_type_identity.local};
        const QttForeignType *imported = NULL;
        QttForeignTypeError imported_status = qtt_foreign_type_import(
            next_foreign, binding->name, authority, &session->handle_type,
            ownership, &imported);
        if (imported_status != QTT_FOREIGN_TYPE_OK) {
            qtt_foreign_type_env_free(next_foreign);
            qtt_type_arena_free(next_types);
            free(next_infos);
            return compiler_fail(
                error,
                imported_status == QTT_FOREIGN_TYPE_OUT_OF_MEMORY
                    ? MONAD_ERROR_OUT_OF_MEMORY : MONAD_ERROR_TYPE_MISMATCH,
                "foreign type snapshot failed compiler validation");
        }
        next_infos[count++] = (monad_compiler_foreign_type_info_t){
            .struct_size = sizeof(monad_compiler_foreign_type_info_t),
            .name = imported->name,
            .nominal_identity = binding->foreign_type_identity,
            .canonical_type_id = imported->type_id.value,
            .representation = MONAD_COMPILER_REP_FOREIGN,
            .ownership = ownership == QTT_OWNERSHIP_CONSUMED
                ? MONAD_COMPILER_OWNERSHIP_CONSUMED
                : MONAD_COMPILER_OWNERSHIP_SHARED,
        };
    }

    QttForeignTypeEnv *old_foreign = session->foreign_types;
    QttTypeArena *old_types = session->types;
    monad_compiler_foreign_type_info_t *old_infos = session->infos;
    session->foreign_types = next_foreign;
    session->types = next_types;
    session->infos = next_infos;
    session->count = count;
    qtt_foreign_type_env_free(old_foreign);
    qtt_type_arena_free(old_types);
    free(old_infos);
    return MONAD_OK;
}

size_t monad_compiler_foreign_type_count(
    const monad_compiler_session_t *session) {
    if (!session || !pthread_equal(session->owner, pthread_self())) return 0;
    return session->count;
}

const monad_compiler_foreign_type_info_t *monad_compiler_foreign_type_at(
    const monad_compiler_session_t *session, size_t index) {
    if (!session || !pthread_equal(session->owner, pthread_self()) ||
        index >= session->count)
        return NULL;
    return &session->infos[index];
}
