#include "interface.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

struct QttInterface {
    char *module_name;
    uint64_t artifact_fingerprint;
    QttInterfaceContract *contracts;
    size_t count;
    size_t capacity;
    QttEffectDeclaration *effect_declarations;
    size_t effect_declaration_count;
    size_t effect_declaration_capacity;
    QttEffectHandlerProfile *handler_profiles;
    size_t handler_profile_count;
    size_t handler_profile_capacity;
    QttEffectTraitImplication *trait_implications;
    size_t trait_implication_count;
    size_t trait_implication_capacity;
};

static char *copy_text(const char *text) {
    if (!text) return NULL;
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
}
static bool portable_token(const char *text);

void qtt_interface_set_artifact_fingerprint(
    QttInterface *interface, uint64_t fingerprint) {
    if (interface) interface->artifact_fingerprint = fingerprint;
}

uint64_t qtt_interface_artifact_fingerprint(
    const QttInterface *interface) {
    return interface ? interface->artifact_fingerprint : 0;
}

static uint64_t mix(uint64_t hash, uint64_t value) {
    hash ^= value;
    return hash * UINT64_C(1099511628211);
}

static uint64_t text_fingerprint(const char *text) {
    uint64_t hash = UINT64_C(1469598103934665603);
    if (!text) return 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        hash ^= *p;
        hash *= UINT64_C(1099511628211);
    }
    return hash ? hash : 1;
}

static uint64_t effect_judgment_fingerprint(
    uint64_t row, uint64_t constraints, QttEffectConstraintResult result,
    const QttInterfaceEffectPredicate *predicates, size_t count) {
    uint64_t hash = UINT64_C(0x6566666563746a75);
    hash = mix(hash, row);
    hash = mix(hash, constraints);
    hash = mix(hash, (uint64_t)result);
    hash = mix(hash, count);
    for (size_t i = 0; i < count; i++) {
        hash = mix(hash, predicates[i].stage);
        hash = mix(hash, text_fingerprint(predicates[i].trait));
    }
    return hash ? hash : UINT64_C(1);
}

static uint64_t effect_declarations_fingerprint(
    const QttInterface *interface, unsigned version) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, interface ? interface->effect_declaration_count : 0);
    for (size_t i = 0; interface &&
         i < interface->effect_declaration_count; i++) {
        const QttEffectDeclaration *declaration =
            &interface->effect_declarations[i];
        hash = mix(hash, text_fingerprint(declaration->name));
        if (version >= 6)
            hash = mix(hash, text_fingerprint(declaration->traits));
        hash = mix(hash, (uint64_t)declaration->kind);
        hash = mix(hash, declaration->constructor_id);
        hash = mix(hash, text_fingerprint(declaration->operation));
        if (version >= 9) {
            hash = mix(hash, text_fingerprint(declaration->payload_type));
            hash = mix(hash, text_fingerprint(declaration->result_type));
        }
        if (version >= 10)
            hash = mix(hash, text_fingerprint(declaration->operation_scheme));
        hash = mix(hash, declaration->resumption.is_omega ? 1 : 0);
        hash = mix(hash, declaration->resumption.finite);
        hash = mix(hash, declaration->scoped ? 1 : 0);
    }
    return hash ? hash : 1;
}

static uint64_t trait_implications_fingerprint(const QttInterface *interface) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, interface ? interface->trait_implication_count : 0);
    for (size_t i = 0; interface && i < interface->trait_implication_count; i++) {
        const QttEffectTraitImplication *edge = &interface->trait_implications[i];
        hash = mix(hash, text_fingerprint(edge->premise));
        hash = mix(hash, text_fingerprint(edge->consequence));
        hash = mix(hash, text_fingerprint(edge->provenance));
    }
    return hash ? hash : 1;
}

static uint64_t handler_profiles_fingerprint(const QttInterface *interface) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, interface ? interface->handler_profile_count : 0);
    for (size_t i = 0; interface && i < interface->handler_profile_count; i++) {
        const QttEffectHandlerProfile *profile = &interface->handler_profiles[i];
        hash = mix(hash, text_fingerprint(profile->name));
        hash = mix(hash, text_fingerprint(profile->effect_name));
        hash = mix(hash, profile->continuation_usage.is_omega ? 1 : 0);
        hash = mix(hash, profile->continuation_usage.finite);
        hash = mix(hash, profile->deep ? 1 : 0);
    }
    return hash ? hash : 1;
}

static uint64_t signature_fingerprint_version(
    const QttFunctionSignature *signature, uint64_t version) {
    if (!signature || !signature->result.type) return 0;
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = mix(hash, version);
    hash = mix(hash, signature->parameter_count);
    for (size_t i = 0; i < signature->parameter_count; i++) {
        const QttParameterContract *parameter =
            &signature->parameters[i];
        if (!parameter->type) return 0;
        hash = mix(hash, qtt_type_fingerprint(parameter->type));
        hash = mix(hash, parameter->representation);
        hash = mix(hash, parameter->mode);
        hash = mix(hash, parameter->quantity.is_omega);
        hash = mix(hash, parameter->quantity.finite);
        hash = mix(hash, parameter->observed.is_omega);
        hash = mix(hash, parameter->observed.finite);
    }
    hash = mix(hash, qtt_type_fingerprint(signature->result.type));
    hash = mix(hash, signature->result.representation);
    hash = mix(hash, signature->result.mode);
    hash = mix(hash, signature->result.origin);
    return hash ? hash : 1;
}

uint64_t qtt_interface_signature_fingerprint(
    const QttFunctionSignature *signature) {
    return signature_fingerprint_version(signature, QTT_INTERFACE_VERSION);
}

static bool flat_type_kind(TypeKind kind) {
    switch (kind) {
    case TYPE_INT: case TYPE_FLOAT: case TYPE_CHAR: case TYPE_BYTE:
    case TYPE_STRING: case TYPE_SYMBOL: case TYPE_BOOL: case TYPE_HEX:
    case TYPE_BIN: case TYPE_OCT: case TYPE_KEYWORD: case TYPE_RATIO:
    case TYPE_F32: case TYPE_I8: case TYPE_U8: case TYPE_I16:
    case TYPE_U16: case TYPE_I32: case TYPE_U32: case TYPE_I64:
    case TYPE_U64: case TYPE_I128: case TYPE_U128: case TYPE_F80:
    case TYPE_NIL: case TYPE_PATH: case TYPE_ESCAPE: case TYPE_UNIT:
        return true;
    default:
        return false;
    }
}

static Type *copy_interface_type(const Type *type) {
    char *encoded = qtt_type_serialize(type);
    Type *copy = encoded ? qtt_type_deserialize(encoded) : NULL;
    free(encoded);
    return copy;
}

static void contract_clear(QttInterfaceContract *contract) {
    if (!contract) return;
    free(contract->name);
    free(contract->callable_contract);
    free(contract->hm_scheme);
    for (size_t i = 0; i < contract->effect_predicate_count; i++)
        free(contract->effect_predicates[i].trait);
    free(contract->effect_predicates);
    for (size_t i = 0; i < contract->signature.parameter_count; i++)
        qtt_type_free_owned((Type *)contract->signature.parameters[i].type);
    free(contract->signature.parameters);
    qtt_type_free_owned((Type *)contract->signature.result.type);
    memset(contract, 0, sizeof(*contract));
}

QttInterface *qtt_interface_new(const char *module_name) {
    if (!module_name || !module_name[0]) return NULL;
    QttInterface *interface = calloc(1, sizeof(*interface));
    if (!interface) return NULL;
    interface->module_name = copy_text(module_name);
    if (!interface->module_name) {
        free(interface);
        return NULL;
    }
    return interface;
}

void qtt_interface_free(QttInterface *interface) {
    if (!interface) return;
    for (size_t i = 0; i < interface->count; i++)
        contract_clear(&interface->contracts[i]);
    free(interface->contracts);
    for (size_t i = 0; i < interface->effect_declaration_count; i++) {
        free((char *)interface->effect_declarations[i].name);
        free((char *)interface->effect_declarations[i].traits);
        free((char *)interface->effect_declarations[i].operation);
        free((char *)interface->effect_declarations[i].payload_type);
        free((char *)interface->effect_declarations[i].result_type);
        free((char *)interface->effect_declarations[i].operation_scheme);
    }
    free(interface->effect_declarations);
    for (size_t i = 0; i < interface->handler_profile_count; i++) {
        free((char *)interface->handler_profiles[i].name);
        free((char *)interface->handler_profiles[i].effect_name);
    }
    free(interface->handler_profiles);
    for (size_t i = 0; i < interface->trait_implication_count; i++) {
        free(interface->trait_implications[i].premise);
        free(interface->trait_implications[i].consequence);
        free(interface->trait_implications[i].provenance);
    }
    free(interface->trait_implications);
    free(interface->module_name);
    free(interface);
}

size_t qtt_interface_trait_implication_count(const QttInterface *interface) {
    return interface ? interface->trait_implication_count : 0;
}

const QttEffectTraitImplication *qtt_interface_trait_implication(
    const QttInterface *interface, size_t index) {
    return interface && index < interface->trait_implication_count
        ? &interface->trait_implications[index] : NULL;
}

bool qtt_interface_add_trait_implication(
    QttInterface *interface, const char *premise,
    const char *consequence, const char *provenance) {
    if (!interface || !portable_token(premise) ||
        !portable_token(consequence) || !portable_token(provenance) ||
        strchr(premise, ',') || strchr(consequence, ',')) return false;
    size_t position = 0;
    while (position < interface->trait_implication_count) {
        QttEffectTraitImplication *edge = &interface->trait_implications[position];
        int order = strcmp(edge->premise, premise);
        if (!order) order = strcmp(edge->consequence, consequence);
        if (!order) return strcmp(edge->provenance, provenance) == 0;
        if (order > 0) break;
        position++;
    }
    if (interface->trait_implication_count == interface->trait_implication_capacity) {
        size_t next = interface->trait_implication_capacity
            ? interface->trait_implication_capacity * 2 : 8;
        QttEffectTraitImplication *grown = realloc(
            interface->trait_implications, next * sizeof(*grown));
        if (!grown) return false;
        interface->trait_implications = grown;
        interface->trait_implication_capacity = next;
    }
    memmove(&interface->trait_implications[position + 1],
        &interface->trait_implications[position],
        (interface->trait_implication_count - position) *
            sizeof(*interface->trait_implications));
    QttEffectTraitImplication edge = {
        .premise = copy_text(premise), .consequence = copy_text(consequence),
        .provenance = copy_text(provenance)};
    if (!edge.premise || !edge.consequence || !edge.provenance) {
        free(edge.premise); free(edge.consequence); free(edge.provenance);
        return false;
    }
    interface->trait_implications[position] = edge;
    interface->trait_implication_count++;
    return true;
}

size_t qtt_interface_effect_declaration_count(const QttInterface *interface) {
    return interface ? interface->effect_declaration_count : 0;
}

const QttEffectDeclaration *qtt_interface_effect_declaration(
    const QttInterface *interface, size_t index) {
    return interface && index < interface->effect_declaration_count
        ? &interface->effect_declarations[index] : NULL;
}

static bool portable_token(const char *text) {
    return text && *text && strlen(text) < 256 &&
        !strchr(text, ' ') && !strchr(text, '\n') && !strchr(text, '\r');
}

static bool portable_operation_scheme(const char *text) {
    static const char prefix[] = "monad-hm-scheme-v1|";
    if (!text) return true;
    if (strncmp(text, prefix, sizeof(prefix) - 1)) return false;
    const char *fingerprint_text = text + sizeof(prefix) - 1;
    char *fingerprint_end = NULL;
    errno = 0;
    uint64_t fingerprint = strtoull(
        fingerprint_text, &fingerprint_end, 16);
    if (errno || !fingerprint_end ||
        fingerprint_end != fingerprint_text + 16 ||
        *fingerprint_end != '|') return false;
    const char *count_text = fingerprint_end + 1;
    char *count_end = NULL;
    errno = 0;
    long quantified = strtol(count_text, &count_end, 10);
    if (errno || !count_end || count_end == count_text ||
        *count_end != '|' || quantified < 0 || quantified > 4096)
        return false;
    const char *body = count_end + 1;
    if (!*body || text_fingerprint(body) != fingerprint) return false;
    Type *type = qtt_type_deserialize(body);
    bool valid = type && type->kind == TYPE_ARROW &&
        type->arrow_param && type->arrow_ret;
    qtt_type_free_owned(type);
    return valid;
}

bool qtt_interface_add_effect_declaration(
    QttInterface *interface, const QttEffectDeclaration *declaration) {
    if (!interface || !declaration || !portable_token(declaration->name) ||
        !portable_token(declaration->traits) ||
        !portable_token(declaration->operation) ||
        (declaration->payload_type &&
         !portable_token(declaration->payload_type)) ||
        (declaration->result_type &&
         !portable_token(declaration->result_type)) ||
        (declaration->operation_scheme &&
         (strlen(declaration->operation_scheme) >= 4096 ||
          strchr(declaration->operation_scheme, ' ') ||
          strchr(declaration->operation_scheme, '\n') ||
          strchr(declaration->operation_scheme, '\r'))) ||
        !portable_operation_scheme(declaration->operation_scheme) ||
        (!!declaration->payload_type != !!declaration->result_type) ||
        declaration->kind < QTT_EFFECT_CUSTOM ||
        declaration->kind > QTT_EFFECT_CONTROL ||
        !declaration->constructor_id)
        return false;
    char *normal_traits = qtt_effect_traits_normalize(declaration->traits);
    if (!normal_traits) return false;
    for (size_t i = 0; i < interface->effect_declaration_count; i++) {
        QttEffectDeclaration *existing = &interface->effect_declarations[i];
        if (strcmp(existing->name, declaration->name)) continue;
        bool canonical_scheme = existing->operation_scheme &&
            declaration->operation_scheme &&
            strcmp(existing->operation_scheme,
                   declaration->operation_scheme) == 0;
        bool equal = existing->kind == declaration->kind &&
            strcmp(existing->traits, normal_traits) == 0 &&
            existing->constructor_id == declaration->constructor_id &&
            strcmp(existing->operation, declaration->operation) == 0 &&
            (canonical_scheme || ((existing->payload_type == NULL &&
              declaration->payload_type == NULL) ||
             (existing->payload_type && declaration->payload_type &&
              strcmp(existing->payload_type,
                     declaration->payload_type) == 0))) &&
            (canonical_scheme || ((existing->result_type == NULL &&
              declaration->result_type == NULL) ||
             (existing->result_type && declaration->result_type &&
              strcmp(existing->result_type,
                     declaration->result_type) == 0))) &&
            ((existing->operation_scheme == NULL &&
              declaration->operation_scheme == NULL) ||
             (existing->operation_scheme && declaration->operation_scheme &&
              strcmp(existing->operation_scheme,
                     declaration->operation_scheme) == 0)) &&
            qtt_quantity_equal(existing->resumption, declaration->resumption) &&
            existing->scoped == declaration->scoped;
        free(normal_traits);
        return equal;
    }
    if (interface->effect_declaration_count ==
            interface->effect_declaration_capacity) {
        size_t next = interface->effect_declaration_capacity
            ? interface->effect_declaration_capacity * 2 : 8;
        QttEffectDeclaration *grown = realloc(
            interface->effect_declarations, next * sizeof(*grown));
        if (!grown) { free(normal_traits); return false; }
        interface->effect_declarations = grown;
        interface->effect_declaration_capacity = next;
    }
    QttEffectDeclaration copy = *declaration;
    copy.name = copy_text(declaration->name);
    copy.traits = normal_traits;
    copy.operation = copy_text(declaration->operation);
    copy.payload_type = copy_text(declaration->payload_type);
    copy.result_type = copy_text(declaration->result_type);
    copy.operation_scheme = copy_text(declaration->operation_scheme);
    if (!copy.name || !copy.traits || !copy.operation ||
        (declaration->payload_type && !copy.payload_type) ||
        (declaration->result_type && !copy.result_type) ||
        (declaration->operation_scheme && !copy.operation_scheme)) {
        free((char *)copy.name);
        free((char *)copy.traits);
        free((char *)copy.operation);
        free((char *)copy.payload_type);
        free((char *)copy.result_type);
        free((char *)copy.operation_scheme);
        return false;
    }
    interface->effect_declarations[interface->effect_declaration_count++] = copy;
    return true;
}

size_t qtt_interface_handler_profile_count(const QttInterface *interface) {
    return interface ? interface->handler_profile_count : 0;
}

const QttEffectHandlerProfile *qtt_interface_handler_profile(
    const QttInterface *interface, size_t index) {
    return interface && index < interface->handler_profile_count
        ? &interface->handler_profiles[index] : NULL;
}

bool qtt_interface_add_handler_profile(
    QttInterface *interface, const QttEffectHandlerProfile *profile) {
    if (!interface || !profile || !portable_token(profile->name) ||
        !portable_token(profile->effect_name)) return false;
    const QttEffectDeclaration *effect = NULL;
    for (size_t i = 0; i < interface->effect_declaration_count; i++)
        if (!strcmp(interface->effect_declarations[i].name,
                    profile->effect_name)) {
            effect = &interface->effect_declarations[i];
            break;
        }
    if (!effect || !qtt_quantity_leq(
            profile->continuation_usage, effect->resumption)) return false;
    size_t position = 0;
    while (position < interface->handler_profile_count) {
        QttEffectHandlerProfile *existing = &interface->handler_profiles[position];
        int order = strcmp(existing->name, profile->name);
        if (!order)
            return !strcmp(existing->effect_name, profile->effect_name) &&
                qtt_quantity_equal(existing->continuation_usage,
                                   profile->continuation_usage) &&
                existing->deep == profile->deep;
        if (order > 0) break;
        position++;
    }
    if (interface->handler_profile_count == interface->handler_profile_capacity) {
        size_t next = interface->handler_profile_capacity
            ? interface->handler_profile_capacity * 2 : 8;
        QttEffectHandlerProfile *grown = realloc(
            interface->handler_profiles, next * sizeof(*grown));
        if (!grown) return false;
        interface->handler_profiles = grown;
        interface->handler_profile_capacity = next;
    }
    memmove(&interface->handler_profiles[position + 1],
            &interface->handler_profiles[position],
            (interface->handler_profile_count - position) *
                sizeof(*interface->handler_profiles));
    QttEffectHandlerProfile copy = *profile;
    copy.name = copy_text(profile->name);
    copy.effect_name = copy_text(profile->effect_name);
    if (!copy.name || !copy.effect_name) {
        free((char *)copy.name); free((char *)copy.effect_name); return false;
    }
    interface->handler_profiles[position] = copy;
    interface->handler_profile_count++;
    return true;
}

const char *qtt_interface_module(const QttInterface *interface) {
    return interface ? interface->module_name : NULL;
}

size_t qtt_interface_count(const QttInterface *interface) {
    return interface ? interface->count : 0;
}

const QttInterfaceContract *qtt_interface_contract(
    const QttInterface *interface, size_t index) {
    return interface && index < interface->count
        ? &interface->contracts[index] : NULL;
}

static bool reserve_contract(QttInterface *interface) {
    if (interface->count < interface->capacity) return true;
    size_t next = interface->capacity ? interface->capacity * 2 : 8;
    QttInterfaceContract *grown = realloc(
        interface->contracts, next * sizeof(*grown));
    if (!grown) return false;
    interface->contracts = grown;
    interface->capacity = next;
    return true;
}

bool qtt_interface_add(
    QttInterface *interface, const char *name,
    const QttFunctionSignature *signature) {
    if (!interface || !name || !name[0] || !signature ||
        !signature->result.type || !reserve_contract(interface))
        return false;
    QttInterfaceContract contract = {
        .interface_version = QTT_INTERFACE_VERSION};
    contract.name = copy_text(name);
    contract.has_ownership_signature = true;
    contract.signature.parameter_count = signature->parameter_count;
    if (signature->parameter_count)
        contract.signature.parameters = calloc(
            signature->parameter_count,
            sizeof(*contract.signature.parameters));
    if (!contract.name ||
        (signature->parameter_count && !contract.signature.parameters))
        goto fail;
    for (size_t i = 0; i < signature->parameter_count; i++) {
        contract.signature.parameters[i] = signature->parameters[i];
        contract.signature.parameters[i].type =
            copy_interface_type(signature->parameters[i].type);
        contract.signature.parameters[i].type_id = (QttTypeId){0};
        if (!contract.signature.parameters[i].type) goto fail;
    }
    contract.signature.result = signature->result;
    contract.signature.result.type = copy_interface_type(signature->result.type);
    contract.signature.result.type_id = (QttTypeId){0};
    contract.signature.result_type = contract.signature.result.type;
    if (!contract.signature.result.type) goto fail;
    contract.stable_fingerprint =
        qtt_interface_signature_fingerprint(&contract.signature);
    if (!contract.stable_fingerprint) goto fail;
    interface->contracts[interface->count++] = contract;
    return true;
fail:
    contract_clear(&contract);
    return false;
}

bool qtt_interface_add_metadata(QttInterface *interface, const char *name) {
    if (!interface || !name || !*name || !reserve_contract(interface))
        return false;
    QttInterfaceContract contract = {
        .name = copy_text(name),
        .interface_version = QTT_INTERFACE_VERSION};
    if (!contract.name) return false;
    interface->contracts[interface->count++] = contract;
    return true;
}

bool qtt_interface_set_callable_contract(
    QttInterface *interface, const char *name, const char *portable_contract,
    uint64_t semantic_fingerprint) {
    if (!interface || !name || !portable_contract || !*portable_contract ||
        !semantic_fingerprint || strlen(portable_contract) >= 15000 ||
        strchr(portable_contract, '\n') ||
        strchr(portable_contract, '\r') || strchr(portable_contract, ' '))
        return false;
    for (size_t i = 0; i < interface->count; i++) {
        QttInterfaceContract *contract = &interface->contracts[i];
        if (strcmp(contract->name, name)) continue;
        char *copy = copy_text(portable_contract);
        if (!copy) return false;
        free(contract->callable_contract);
        contract->callable_contract = copy;
        contract->callable_contract_fingerprint = semantic_fingerprint;
        return true;
    }
    return false;
}

bool qtt_interface_set_hm_scheme(
    QttInterface *interface, const char *name, const char *portable_scheme) {
    if (!interface || !name || !portable_scheme || !*portable_scheme ||
        strlen(portable_scheme) >= 15000 || strchr(portable_scheme, '\n') ||
        strchr(portable_scheme, '\r') || strchr(portable_scheme, ' '))
        return false;
    for (size_t i = 0; i < interface->count; i++) {
        QttInterfaceContract *contract = &interface->contracts[i];
        if (strcmp(contract->name, name)) continue;
        char *copy = copy_text(portable_scheme);
        if (!copy) return false;
        free(contract->hm_scheme);
        contract->hm_scheme = copy;
        return true;
    }
    return false;
}

static bool valid_effect_trait(const char *trait) {
    return trait && *trait && !strpbrk(trait, " ,:;\r\n");
}

bool qtt_interface_set_effect_judgment(
    QttInterface *interface, const char *name,
    uint64_t row_fingerprint, uint64_t constraint_fingerprint,
    QttEffectConstraintResult result, const size_t *predicate_stages,
    const char *const *predicate_traits, size_t predicate_count) {
    if (!interface || !name || !*name || !row_fingerprint ||
        !constraint_fingerprint ||
        (result != QTT_EFFECT_CONSTRAINT_SOLVED &&
         result != QTT_EFFECT_CONSTRAINT_RESIDUAL) ||
        (predicate_count && (!predicate_stages || !predicate_traits)))
        return false;
    QttInterfaceEffectPredicate *predicates = predicate_count
        ? calloc(predicate_count, sizeof(*predicates)) : NULL;
    if (predicate_count && !predicates) return false;
    for (size_t i = 0; i < predicate_count; i++) {
        if (!valid_effect_trait(predicate_traits[i])) goto fail;
        predicates[i].stage = predicate_stages[i];
        predicates[i].trait = copy_text(predicate_traits[i]);
        if (!predicates[i].trait) goto fail;
    }
    for (size_t i = 0; i < interface->count; i++) {
        QttInterfaceContract *contract = &interface->contracts[i];
        if (strcmp(contract->name, name)) continue;
        for (size_t j = 0; j < contract->effect_predicate_count; j++)
            free(contract->effect_predicates[j].trait);
        free(contract->effect_predicates);
        contract->has_effect_judgment = true;
        contract->effect_row_fingerprint = row_fingerprint;
        contract->effect_constraint_fingerprint = constraint_fingerprint;
        contract->effect_constraint_result = result;
        contract->effect_predicates = predicates;
        contract->effect_predicate_count = predicate_count;
        contract->effect_judgment_fingerprint =
            effect_judgment_fingerprint(
                row_fingerprint, constraint_fingerprint, result,
                predicates, predicate_count);
        return true;
    }
fail:
    for (size_t i = 0; i < predicate_count; i++)
        free(predicates[i].trait);
    free(predicates);
    return false;
}

QttInterfaceError qtt_interface_write(
    const QttInterface *interface, const char *path) {
    if (!interface || !path) return QTT_INTERFACE_IO_ERROR;
    FILE *file = fopen(path, "w");
    if (!file) return QTT_INTERFACE_IO_ERROR;
    bool ok = fprintf(
        file,
        "MONAD-QTT-INTERFACE %d\nMODULE %s\nARTIFACT %016" PRIx64 "\n",
        QTT_INTERFACE_VERSION, interface->module_name,
        interface->artifact_fingerprint) > 0;
    if (ok) ok = fprintf(
        file, "DECLARATIONS %zu fingerprint=%016" PRIx64 "\n",
        interface->effect_declaration_count,
        effect_declarations_fingerprint(interface, QTT_INTERFACE_VERSION)) > 0;
    for (size_t i = 0; ok && i < interface->effect_declaration_count; i++) {
        const QttEffectDeclaration *declaration =
            &interface->effect_declarations[i];
        ok = fprintf(
            file, "EFFECTDECL name=%s traits=%s kind=%d constructor=%016" PRIx64
            " operation=%s payload=%s result=%s scheme=%s resumption=%d:%" PRIu64
            " scoped=%u\n",
            declaration->name, declaration->traits, (int)declaration->kind,
            declaration->constructor_id, declaration->operation,
            declaration->payload_type ? declaration->payload_type : "-",
            declaration->result_type ? declaration->result_type : "-",
            declaration->operation_scheme ? declaration->operation_scheme : "-",
            declaration->resumption.is_omega ? 1 : 0,
            declaration->resumption.finite,
            declaration->scoped ? 1u : 0u) > 0;
    }
    if (ok) ok = fprintf(file,
        "HANDLERS %zu fingerprint=%016" PRIx64 "\n",
        interface->handler_profile_count,
        handler_profiles_fingerprint(interface)) > 0;
    for (size_t i = 0; ok && i < interface->handler_profile_count; i++) {
        const QttEffectHandlerProfile *profile = &interface->handler_profiles[i];
        ok = fprintf(file,
            "HANDLERPROFILE name=%s effect=%s usage=%d:%" PRIu64
            " deep=%u\n",
            profile->name, profile->effect_name,
            profile->continuation_usage.is_omega ? 1 : 0,
            profile->continuation_usage.finite,
            profile->deep ? 1u : 0u) > 0;
    }
    if (ok) ok = fprintf(file,
        "IMPLICATIONS %zu fingerprint=%016" PRIx64 "\n",
        interface->trait_implication_count,
        trait_implications_fingerprint(interface)) > 0;
    for (size_t i = 0; ok && i < interface->trait_implication_count; i++) {
        const QttEffectTraitImplication *edge = &interface->trait_implications[i];
        ok = fprintf(file,
            "TRAITIMPL premise=%s consequence=%s provenance=%s\n",
            edge->premise, edge->consequence, edge->provenance) > 0;
    }
    for (size_t i = 0; ok && i < interface->count; i++) {
        const QttInterfaceContract *contract = &interface->contracts[i];
        ok = fprintf(
            file, "CONTRACT %s parameters=%zu fingerprint=%016" PRIx64
            " ownership=%u\n",
            contract->name, contract->signature.parameter_count,
            contract->stable_fingerprint,
            contract->has_ownership_signature ? 1u : 0u) > 0;
        for (size_t j = 0;
             ok && j < contract->signature.parameter_count; j++) {
            const QttParameterContract *parameter =
                &contract->signature.parameters[j];
            char *type_text = qtt_type_serialize(parameter->type);
            ok = type_text && fprintf(
                file,
                "PARAM type=%s representation=%d mode=%d "
                "quantity=%d:%" PRIu64 " observed=%d:%" PRIu64 "\n",
                type_text,
                (int)parameter->representation, (int)parameter->mode,
                parameter->quantity.is_omega,
                parameter->quantity.finite,
                parameter->observed.is_omega,
                parameter->observed.finite) > 0;
            free(type_text);
        }
        const QttResultContract *result = &contract->signature.result;
        char *result_type_text = ok
            && contract->has_ownership_signature
            ? qtt_type_serialize(result->type) : NULL;
        if (ok && contract->has_ownership_signature) {
            ok = result_type_text && fprintf(
                file,
                "RESULT type=%s representation=%d mode=%d origin=%d\n",
                result_type_text, (int)result->representation,
                (int)result->mode, (int)result->origin) > 0;
            free(result_type_text);
        }
        if (ok && contract->hm_scheme)
            ok = fprintf(
                file, "HM checksum=%016" PRIx64 " payload=%s\n",
                text_fingerprint(contract->hm_scheme),
                contract->hm_scheme) > 0;
        else if (ok)
            ok = fprintf(file, "HM none\n") > 0;
        if (ok && contract->callable_contract)
            ok = fprintf(
                file, "EFFECT fingerprint=%016" PRIx64
                " checksum=%016" PRIx64 " payload=%s\n",
                contract->callable_contract_fingerprint,
                text_fingerprint(contract->callable_contract),
                contract->callable_contract) > 0;
        else if (ok)
            ok = fprintf(file, "EFFECT none\n") > 0;
        if (ok && contract->has_effect_judgment) {
            ok = fprintf(file,
                "EFFECTJUDGMENT row=%016" PRIx64
                " constraints=%016" PRIx64 " result=%d count=%zu"
                " commitment=%016" PRIx64 " predicates=",
                contract->effect_row_fingerprint,
                contract->effect_constraint_fingerprint,
                (int)contract->effect_constraint_result,
                contract->effect_predicate_count,
                contract->effect_judgment_fingerprint) > 0;
            if (ok && !contract->effect_predicate_count)
                ok = fputc('-', file) != EOF;
            for (size_t j = 0;
                 ok && j < contract->effect_predicate_count; j++) {
                ok = fprintf(file, "%s%zu:%s", j ? "," : "",
                    contract->effect_predicates[j].stage,
                    contract->effect_predicates[j].trait) > 0;
            }
            if (ok) ok = fputc('\n', file) != EOF;
        } else if (ok) {
            ok = fprintf(file, "EFFECTJUDGMENT none\n") > 0;
        }
        if (ok) ok = fprintf(file, "END\n") > 0;
    }
    if (fclose(file) != 0) ok = false;
    return ok ? QTT_INTERFACE_OK : QTT_INTERFACE_IO_ERROR;
}

static bool enum_range(int value, int end) {
    return value >= 0 && value < end;
}

static Type *read_flat_type(int kind) {
    if (kind < 0 || kind > TYPE_FINITE_SET ||
        !flat_type_kind((TypeKind)kind))
        return NULL;
    Type *type = calloc(1, sizeof(*type));
    if (type) type->kind = (TypeKind)kind;
    return type;
}

static QttInterface *read_fail(
    FILE *file, QttInterface *interface,
    QttInterfaceError value, QttInterfaceError *error) {
    if (file) fclose(file);
    qtt_interface_free(interface);
    if (error) *error = value;
    return NULL;
}

static bool parse_effect_predicates(
    QttInterfaceContract *contract, char *payload, size_t count) {
    if (!count) return strcmp(payload, "-") == 0;
    contract->effect_predicates =
        calloc(count, sizeof(*contract->effect_predicates));
    if (!contract->effect_predicates) return false;
    contract->effect_predicate_count = count;
    char *cursor = payload;
    for (size_t i = 0; i < count; i++) {
        errno = 0;
        char *stage_end = NULL;
        uint64_t stage = strtoull(cursor, &stage_end, 10);
        if (errno || !stage_end || stage_end == cursor ||
            *stage_end != ':' || stage > SIZE_MAX)
            return false;
        char *trait = stage_end + 1;
        char *end = strchr(trait, ',');
        if ((i + 1 < count) != (end != NULL)) return false;
        size_t length = end ? (size_t)(end - trait) : strlen(trait);
        if (!length) return false;
        char saved = trait[length];
        trait[length] = '\0';
        bool valid = valid_effect_trait(trait);
        contract->effect_predicates[i].trait = valid
            ? copy_text(trait) : NULL;
        trait[length] = saved;
        if (!contract->effect_predicates[i].trait) return false;
        contract->effect_predicates[i].stage = (size_t)stage;
        cursor = end ? end + 1 : trait + length;
    }
    return *cursor == '\0';
}

QttInterface *qtt_interface_read(
    const char *path, QttInterfaceError *error) {
    if (error) *error = QTT_INTERFACE_OK;
    FILE *file = path ? fopen(path, "r") : NULL;
    if (!file) return read_fail(
        NULL, NULL, QTT_INTERFACE_IO_ERROR, error);
    char line[16384];
    int version = 0;
    if (!fgets(line, sizeof(line), file) ||
        sscanf(line, "MONAD-QTT-INTERFACE %d", &version) != 1)
        return read_fail(file, NULL, QTT_INTERFACE_BAD_MAGIC, error);
    if (version < 1 || version > QTT_INTERFACE_VERSION)
        return read_fail(
            file, NULL, QTT_INTERFACE_UNSUPPORTED_VERSION, error);
    char module[256], extra;
    if (!fgets(line, sizeof(line), file) ||
        sscanf(line, "MODULE %255s %c", module, &extra) != 1)
        return read_fail(file, NULL, QTT_INTERFACE_MALFORMED, error);
    QttInterface *interface = qtt_interface_new(module);
    if (!interface)
        return read_fail(file, NULL, QTT_INTERFACE_OUT_OF_MEMORY, error);
    char artifact_text[32];
    if (!fgets(line, sizeof(line), file) ||
        sscanf(line, "ARTIFACT %31s %c", artifact_text, &extra) != 1)
        return read_fail(
            file, interface, QTT_INTERFACE_MALFORMED, error);
    errno = 0;
    char *artifact_end = NULL;
    interface->artifact_fingerprint =
        strtoull(artifact_text, &artifact_end, 16);
    if (errno || !artifact_end || *artifact_end)
        return read_fail(
            file, interface, QTT_INTERFACE_MALFORMED, error);
    if (version >= 5) {
        size_t declaration_count = 0;
        char declaration_fingerprint_text[32];
        if (!fgets(line, sizeof(line), file) ||
            sscanf(line, "DECLARATIONS %zu fingerprint=%31s %c",
                &declaration_count, declaration_fingerprint_text, &extra) != 2 ||
            declaration_count > 4096)
            return read_fail(file, interface, QTT_INTERFACE_MALFORMED, error);
        for (size_t i = 0; i < declaration_count; i++) {
            char name[256], traits[256], operation[256], payload[256],
                result_type[256], operation_scheme[4096], constructor_text[32];
            int kind, omega, scoped;
            uint64_t finite;
            if (!fgets(line, sizeof(line), file))
                return read_fail(file, interface, QTT_INTERFACE_MALFORMED, error);
            int declaration_fields = version >= 10 ? sscanf(line,
                    "EFFECTDECL name=%255s traits=%255s kind=%d constructor=%31s "
                    "operation=%255s payload=%255s result=%255s scheme=%4095s "
                    "resumption=%d:%" SCNu64 " scoped=%d %c",
                    name, traits, &kind, constructor_text, operation,
                    payload, result_type, operation_scheme, &omega, &finite,
                    &scoped, &extra)
                : version >= 9 ? sscanf(line,
                    "EFFECTDECL name=%255s traits=%255s kind=%d constructor=%31s "
                    "operation=%255s payload=%255s result=%255s "
                    "resumption=%d:%" SCNu64 " scoped=%d %c",
                    name, traits, &kind, constructor_text, operation,
                    payload, result_type, &omega, &finite, &scoped, &extra)
                : version >= 6 ? sscanf(line,
                    "EFFECTDECL name=%255s traits=%255s kind=%d constructor=%31s "
                    "operation=%255s resumption=%d:%" SCNu64
                    " scoped=%d %c",
                    name, traits, &kind, constructor_text, operation, &omega,
                    &finite, &scoped, &extra) : sscanf(line,
                    "EFFECTDECL name=%255s kind=%d constructor=%31s "
                    "operation=%255s resumption=%d:%" SCNu64
                    " scoped=%d %c",
                    name, &kind, constructor_text, operation, &omega,
                    &finite, &scoped, &extra);
            if (version < 6) strcpy(traits, "legacy");
            if (declaration_fields != (version >= 10 ? 11 : version >= 9 ? 10 : version >= 6 ? 8 : 7) ||
                kind < QTT_EFFECT_CUSTOM || kind > QTT_EFFECT_CONTROL ||
                (omega != 0 && omega != 1) ||
                (scoped != 0 && scoped != 1))
                return read_fail(file, interface, QTT_INTERFACE_MALFORMED, error);
            errno = 0;
            char *constructor_end = NULL;
            uint64_t constructor = strtoull(
                constructor_text, &constructor_end, 16);
            QttEffectDeclaration declaration = {
                .name = name,
                .traits = traits,
                .kind = (QttEffectKind)kind,
                .constructor_id = constructor,
                .operation = operation,
                .payload_type = version >= 9 && strcmp(payload, "-")
                    ? payload : NULL,
                .result_type = version >= 9 && strcmp(result_type, "-")
                    ? result_type : NULL,
                .operation_scheme = version >= 10 &&
                    strcmp(operation_scheme, "-") ? operation_scheme : NULL,
                .resumption = {finite, omega != 0},
                .scoped = scoped != 0,
            };
            if (errno || !constructor_end || *constructor_end || !constructor ||
                !qtt_interface_add_effect_declaration(interface, &declaration))
                return read_fail(
                    file, interface, QTT_INTERFACE_FINGERPRINT_MISMATCH, error);
        }
        errno = 0;
        char *declaration_fingerprint_end = NULL;
        uint64_t declaration_fingerprint = strtoull(
            declaration_fingerprint_text,
            &declaration_fingerprint_end, 16);
        if (errno || !declaration_fingerprint_end ||
            *declaration_fingerprint_end || !declaration_fingerprint ||
            declaration_fingerprint !=
                effect_declarations_fingerprint(interface, version))
            return read_fail(
                file, interface, QTT_INTERFACE_FINGERPRINT_MISMATCH, error);
    }
    if (version >= 8) {
        size_t profile_count = 0;
        char fingerprint_text[32];
        if (!fgets(line, sizeof(line), file) || sscanf(line,
            "HANDLERS %zu fingerprint=%31s %c",
            &profile_count, fingerprint_text, &extra) != 2 ||
            profile_count > 4096)
            return read_fail(file, interface, QTT_INTERFACE_MALFORMED, error);
        for (size_t i = 0; i < profile_count; i++) {
            char name[256], effect_name[256];
            int omega, deep;
            uint64_t finite;
            if (!fgets(line, sizeof(line), file) || sscanf(line,
                "HANDLERPROFILE name=%255s effect=%255s usage=%d:%" SCNu64
                " deep=%d %c",
                name, effect_name, &omega, &finite, &deep, &extra) != 5 ||
                (omega != 0 && omega != 1) ||
                (deep != 0 && deep != 1))
                return read_fail(file, interface, QTT_INTERFACE_MALFORMED, error);
            QttEffectHandlerProfile profile = {
                .name = name,
                .effect_name = effect_name,
                .continuation_usage = {finite, omega != 0},
                .deep = deep != 0,
            };
            if (!qtt_interface_add_handler_profile(interface, &profile))
                return read_fail(file, interface,
                    QTT_INTERFACE_FINGERPRINT_MISMATCH, error);
        }
        errno = 0; char *end = NULL;
        uint64_t expected = strtoull(fingerprint_text, &end, 16);
        if (errno || !end || *end || !expected ||
            expected != handler_profiles_fingerprint(interface))
            return read_fail(file, interface,
                QTT_INTERFACE_FINGERPRINT_MISMATCH, error);
    }
    if (version >= 7) {
        size_t implication_count = 0;
        char fingerprint_text[32];
        if (!fgets(line, sizeof(line), file) || sscanf(line,
            "IMPLICATIONS %zu fingerprint=%31s %c",
            &implication_count, fingerprint_text, &extra) != 2 ||
            implication_count > 4096)
            return read_fail(file, interface, QTT_INTERFACE_MALFORMED, error);
        for (size_t i = 0; i < implication_count; i++) {
            char premise[256], consequence[256], provenance[256];
            if (!fgets(line, sizeof(line), file) || sscanf(line,
                "TRAITIMPL premise=%255s consequence=%255s provenance=%255s %c",
                premise, consequence, provenance, &extra) != 3 ||
                !qtt_interface_add_trait_implication(
                    interface, premise, consequence, provenance))
                return read_fail(file, interface, QTT_INTERFACE_MALFORMED, error);
        }
        errno = 0; char *end = NULL;
        uint64_t expected = strtoull(fingerprint_text, &end, 16);
        if (errno || !end || *end || !expected ||
            expected != trait_implications_fingerprint(interface))
            return read_fail(file, interface,
                QTT_INTERFACE_FINGERPRINT_MISMATCH, error);
    }
    while (fgets(line, sizeof(line), file)) {
        char name[256], fingerprint_text[32];
        size_t parameter_count = 0;
        int has_ownership = 1;
        int contract_fields = version >= 4
            ? sscanf(
                line,
                "CONTRACT %255s parameters=%zu fingerprint=%31s "
                "ownership=%d %c",
                name, &parameter_count, fingerprint_text,
                &has_ownership, &extra)
            : sscanf(
                line,
                "CONTRACT %255s parameters=%zu fingerprint=%31s %c",
                name, &parameter_count, fingerprint_text, &extra);
        if (contract_fields != (version >= 4 ? 4 : 3) ||
            (has_ownership != 0 && has_ownership != 1) ||
            (!has_ownership && parameter_count != 0))
            return read_fail(
                file, interface, QTT_INTERFACE_MALFORMED, error);
        if (!reserve_contract(interface))
            return read_fail(
                file, interface, QTT_INTERFACE_OUT_OF_MEMORY, error);
        QttInterfaceContract contract = {
            .interface_version = (unsigned)version};
        contract.name = copy_text(name);
        contract.has_ownership_signature = has_ownership != 0;
        contract.signature.parameter_count = parameter_count;
        if (parameter_count)
            contract.signature.parameters = calloc(
                parameter_count,
                sizeof(*contract.signature.parameters));
        if (!contract.name ||
            (parameter_count && !contract.signature.parameters)) {
            contract_clear(&contract);
            return read_fail(
                file, interface, QTT_INTERFACE_OUT_OF_MEMORY, error);
        }
        for (size_t i = 0; i < parameter_count; i++) {
            int kind = -1, representation, mode, qomega, oomega;
            uint64_t quantity, observed;
            char type_text[12000];
            if (!fgets(line, sizeof(line), file) || (version >= 3
                ? sscanf(
                      line,
                      "PARAM type=%11999s representation=%d mode=%d "
                      "quantity=%d:%" SCNu64 " observed=%d:%" SCNu64 " %c",
                      type_text, &representation, &mode,
                      &qomega, &quantity, &oomega, &observed, &extra) != 7
                : sscanf(
                      line,
                      "PARAM type=%d representation=%d mode=%d "
                      "quantity=%d:%" SCNu64 " observed=%d:%" SCNu64 " %c",
                      &kind, &representation, &mode,
                      &qomega, &quantity, &oomega, &observed, &extra) != 7) ||
                !enum_range(representation, QTT_REP_UNKNOWN + 1) ||
                !enum_range(mode, QTT_OWNERSHIP_SHARED + 1) ||
                (qomega != 0 && qomega != 1) ||
                (oomega != 0 && oomega != 1)) {
                contract_clear(&contract);
                return read_fail(
                    file, interface, QTT_INTERFACE_MALFORMED, error);
            }
            Type *type = version >= 3
                ? qtt_type_deserialize(type_text) : read_flat_type(kind);
            if (!type) {
                contract_clear(&contract);
                return read_fail(
                    file, interface,
                    QTT_INTERFACE_UNSUPPORTED_TYPE, error);
            }
            contract.signature.parameters[i] =
                (QttParameterContract){
                    .quantity = {quantity, qomega != 0},
                    .observed = {observed, oomega != 0},
                    .mode = (QttOwnershipMode)mode,
                    .type = type,
                    .representation =
                        (QttRepresentation)representation,
                };
        }
        int kind = -1, representation, mode, origin;
        char result_type_text[12000];
        if (contract.has_ownership_signature &&
            (!fgets(line, sizeof(line), file) || (version >= 3
            ? sscanf(
                  line,
                  "RESULT type=%11999s representation=%d mode=%d origin=%d %c",
                  result_type_text, &representation, &mode, &origin, &extra) != 4
            : sscanf(
                  line,
                  "RESULT type=%d representation=%d mode=%d origin=%d %c",
                  &kind, &representation, &mode, &origin, &extra) != 4) ||
            !enum_range(representation, QTT_REP_UNKNOWN + 1) ||
            !enum_range(mode, QTT_RESULT_UNKNOWN + 1) ||
            !enum_range(origin, QTT_RESULT_ORIGIN_UNKNOWN + 1))) {
            contract_clear(&contract);
            return read_fail(
                file, interface, QTT_INTERFACE_MALFORMED, error);
        }
        Type *result_type = contract.has_ownership_signature
            ? (version >= 3
                ? qtt_type_deserialize(result_type_text) : read_flat_type(kind))
            : NULL;
        if (contract.has_ownership_signature && !result_type) {
            contract_clear(&contract);
            return read_fail(
                file, interface, QTT_INTERFACE_UNSUPPORTED_TYPE, error);
        }
        if (contract.has_ownership_signature) {
            contract.signature.result = (QttResultContract){
                .mode = (QttResultMode)mode,
                .origin = (QttResultOrigin)origin,
                .type = result_type,
                .representation = (QttRepresentation)representation,
            };
            contract.signature.result_type = result_type;
        }
        if (!fgets(line, sizeof(line), file)) {
            contract_clear(&contract);
            return read_fail(
                file, interface, QTT_INTERFACE_MALFORMED, error);
        }
        if (version >= 4) {
            if (strcmp(line, "HM none\n") != 0) {
                char checksum_text[32], payload[15000];
                if (sscanf(
                        line, "HM checksum=%31s payload=%14999s %c",
                        checksum_text, payload, &extra) != 2) {
                    contract_clear(&contract);
                    return read_fail(
                        file, interface, QTT_INTERFACE_MALFORMED, error);
                }
                errno = 0;
                char *checksum_end = NULL;
                uint64_t checksum = strtoull(
                    checksum_text, &checksum_end, 16);
                contract.hm_scheme = copy_text(payload);
                if (errno || !checksum_end || *checksum_end ||
                    !contract.hm_scheme || checksum != text_fingerprint(payload)) {
                    contract_clear(&contract);
                    return read_fail(
                        file, interface,
                        QTT_INTERFACE_FINGERPRINT_MISMATCH, error);
                }
            }
            if (!fgets(line, sizeof(line), file)) {
                contract_clear(&contract);
                return read_fail(
                    file, interface, QTT_INTERFACE_MALFORMED, error);
            }
        }
        if (version >= 2) {
            if (strcmp(line, "EFFECT none\n") != 0) {
                char semantic_text[32], checksum_text[32], payload[15000];
                if (sscanf(
                        line,
                        "EFFECT fingerprint=%31s checksum=%31s payload=%14999s %c",
                        semantic_text, checksum_text, payload, &extra) != 3) {
                    contract_clear(&contract);
                    return read_fail(
                        file, interface, QTT_INTERFACE_MALFORMED, error);
                }
                errno = 0;
                char *semantic_end = NULL;
                contract.callable_contract_fingerprint =
                    strtoull(semantic_text, &semantic_end, 16);
                char *checksum_end = NULL;
                uint64_t checksum = strtoull(
                    checksum_text, &checksum_end, 16);
                contract.callable_contract = copy_text(payload);
                if (errno || !semantic_end || *semantic_end ||
                    !checksum_end || *checksum_end ||
                    !contract.callable_contract_fingerprint ||
                    !contract.callable_contract ||
                    checksum != text_fingerprint(payload)) {
                    contract_clear(&contract);
                    return read_fail(
                        file, interface,
                        QTT_INTERFACE_FINGERPRINT_MISMATCH, error);
                }
            }
            if (!fgets(line, sizeof(line), file)) {
                contract_clear(&contract);
                return read_fail(
                    file, interface, QTT_INTERFACE_MALFORMED, error);
            }
        }
        if (version >= 11) {
            if (strcmp(line, "EFFECTJUDGMENT none\n") != 0) {
                char row_text[32], constraint_text[32], commitment_text[32];
                char predicates[15000];
                int result = -1;
                size_t predicate_count = 0;
                if (sscanf(line,
                        "EFFECTJUDGMENT row=%31s constraints=%31s"
                        " result=%d count=%zu commitment=%31s"
                        " predicates=%14999s %c",
                        row_text, constraint_text, &result,
                        &predicate_count, commitment_text,
                        predicates, &extra) != 6 ||
                    (result != QTT_EFFECT_CONSTRAINT_SOLVED &&
                     result != QTT_EFFECT_CONSTRAINT_RESIDUAL)) {
                    contract_clear(&contract);
                    return read_fail(file, interface,
                        QTT_INTERFACE_MALFORMED, error);
                }
                errno = 0;
                char *row_end = NULL;
                uint64_t row = strtoull(row_text, &row_end, 16);
                char *constraint_end = NULL;
                uint64_t constraint = strtoull(
                    constraint_text, &constraint_end, 16);
                char *commitment_end = NULL;
                uint64_t commitment = strtoull(
                    commitment_text, &commitment_end, 16);
                contract.has_effect_judgment = true;
                contract.effect_row_fingerprint = row;
                contract.effect_constraint_fingerprint = constraint;
                contract.effect_constraint_result =
                    (QttEffectConstraintResult)result;
                if (errno || !row || !constraint || !commitment ||
                    !row_end || *row_end ||
                    !constraint_end || *constraint_end ||
                    !commitment_end || *commitment_end ||
                    !parse_effect_predicates(
                        &contract, predicates, predicate_count)) {
                    contract_clear(&contract);
                    return read_fail(file, interface,
                        QTT_INTERFACE_MALFORMED, error);
                }
                contract.effect_judgment_fingerprint =
                    effect_judgment_fingerprint(
                        row, constraint,
                        (QttEffectConstraintResult)result,
                        contract.effect_predicates,
                        contract.effect_predicate_count);
                if (contract.effect_judgment_fingerprint != commitment) {
                    contract_clear(&contract);
                    return read_fail(file, interface,
                        QTT_INTERFACE_FINGERPRINT_MISMATCH, error);
                }
            }
            if (version >= 12 && contract.callable_contract &&
                !contract.has_effect_judgment) {
                contract_clear(&contract);
                return read_fail(file, interface,
                    QTT_INTERFACE_MALFORMED, error);
            }
            if (!fgets(line, sizeof(line), file)) {
                contract_clear(&contract);
                return read_fail(file, interface,
                    QTT_INTERFACE_MALFORMED, error);
            }
        }
        if (strcmp(line, "END\n") != 0) {
            contract_clear(&contract);
            return read_fail(
                file, interface, QTT_INTERFACE_MALFORMED, error);
        }
        errno = 0;
        char *end = NULL;
        contract.stable_fingerprint = strtoull(fingerprint_text, &end, 16);
        if (errno || !end || *end ||
            (contract.has_ownership_signature &&
             contract.stable_fingerprint !=
                signature_fingerprint_version(
                    &contract.signature, (uint64_t)version)) ||
            (!contract.has_ownership_signature &&
             contract.stable_fingerprint != 0)) {
            contract_clear(&contract);
            return read_fail(
                file, interface,
                QTT_INTERFACE_FINGERPRINT_MISMATCH, error);
        }
        interface->contracts[interface->count++] = contract;
    }
    if (ferror(file))
        return read_fail(
            file, interface, QTT_INTERFACE_IO_ERROR, error);
    fclose(file);
    return interface;
}
