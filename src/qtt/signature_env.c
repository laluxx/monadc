#include "signature_env.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    QttCallableId id;
    QttFunctionSignature *signature;
} QttSignatureEntry;

struct QttSignatureEnv {
    QttTypeArena *types;
    QttSignatureEntry *entries;
    size_t count;
    size_t capacity;
};

static char *copy_text(const char *text) {
    if (!text) return NULL;
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
}

static uint64_t name_fingerprint(const char *name) {
    uint64_t hash = UINT64_C(1469598103934665603);
    if (!name) return 0;
    for (; *name; name++) {
        hash ^= (unsigned char)*name;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

bool qtt_callable_id_equal(QttCallableId left, QttCallableId right) {
    return left.module_id == right.module_id &&
           left.name_hash == right.name_hash &&
           left.name && right.name &&
           strcmp(left.name, right.name) == 0;
}

QttSignatureEnv *qtt_signature_env_new(QttTypeArena *types) {
    if (!types) return NULL;
    QttSignatureEnv *env = calloc(1, sizeof(*env));
    if (env) env->types = types;
    return env;
}

void qtt_signature_env_free(QttSignatureEnv *env) {
    if (!env) return;
    for (size_t i = 0; i < env->count; i++)
        free((char *)env->entries[i].id.name);
    free(env->entries);
    free(env);
}

size_t qtt_signature_env_count(const QttSignatureEnv *env) {
    return env ? env->count : 0;
}

static bool canonical_signature(const QttSignatureEnv *env,
                                const QttFunctionSignature *signature) {
    if (!signature || !signature->result.type_id.value ||
        !qtt_type_lookup(env->types, signature->result.type_id))
        return false;
    for (size_t i = 0; i < signature->parameter_count; i++)
        if (!signature->parameters[i].type_id.value ||
            !qtt_type_lookup(env->types, signature->parameters[i].type_id))
            return false;
    if (signature->source &&
        qtt_signature_validate_grades(signature, env->types) !=
            QTT_GRADED_PROOF_VALID)
        return false;
    return true;
}

QttSignatureEnvError qtt_signature_env_register_with_hash(
    QttSignatureEnv *env,
    uint64_t module_id,
    const char *name,
    uint64_t name_hash,
    QttFunctionSignature *signature,
    QttCallableId *id) {
    if (!env || !name || !signature)
        return QTT_SIGNATURE_ENV_NON_CANONICAL;
    if (signature->source &&
        qtt_signature_validate(signature, signature->source) !=
            QTT_SIGNATURE_VALID)
        return QTT_SIGNATURE_ENV_STALE_SIGNATURE;
    if (!canonical_signature(env, signature))
        return QTT_SIGNATURE_ENV_NON_CANONICAL;
    if (qtt_signature_env_lookup(env, module_id, name))
        return QTT_SIGNATURE_ENV_DUPLICATE;
    if (env->count == env->capacity) {
        size_t next = env->capacity ? env->capacity * 2 : 16;
        QttSignatureEntry *grown =
            realloc(env->entries, next * sizeof(*grown));
        if (!grown) return QTT_SIGNATURE_ENV_OUT_OF_MEMORY;
        env->entries = grown;
        env->capacity = next;
    }
    char *owned_name = copy_text(name);
    if (!owned_name) return QTT_SIGNATURE_ENV_OUT_OF_MEMORY;
    QttCallableId stored = {
        .module_id = module_id,
        .name_hash = name_hash,
        .name = owned_name,
    };
    env->entries[env->count++] =
        (QttSignatureEntry){stored, signature};
    if (id) *id = stored;
    return QTT_SIGNATURE_ENV_OK;
}

QttSignatureEnvError qtt_signature_env_register(
    QttSignatureEnv *env,
    uint64_t module_id,
    const char *name,
    QttFunctionSignature *signature,
    QttCallableId *id) {
    return qtt_signature_env_register_with_hash(
        env, module_id, name, name_fingerprint(name), signature, id);
}

QttFunctionSignature *qtt_signature_env_lookup(
    const QttSignatureEnv *env,
    uint64_t module_id,
    const char *name) {
    if (!env || !name) return NULL;
    for (size_t i = 0; i < env->count; i++)
        if (env->entries[i].id.module_id == module_id &&
            strcmp(env->entries[i].id.name, name) == 0)
            return env->entries[i].signature;
    return NULL;
}

QttFunctionSignature *qtt_signature_env_lookup_id(
    const QttSignatureEnv *env,
    QttCallableId id) {
    if (!env) return NULL;
    for (size_t i = 0; i < env->count; i++)
        if (qtt_callable_id_equal(env->entries[i].id, id))
            return env->entries[i].signature;
    return NULL;
}

bool qtt_signature_env_resolve(
    const QttSignatureEnv *env, uint64_t module_id, const char *name,
    QttCallableId *id, QttFunctionSignature **signature) {
    if (!env || !name) return false;
    for (size_t i = 0; i < env->count; i++) {
        if (env->entries[i].id.module_id != module_id ||
            strcmp(env->entries[i].id.name, name) != 0)
            continue;
        if (id) *id = env->entries[i].id;
        if (signature) *signature = env->entries[i].signature;
        return true;
    }
    return false;
}
