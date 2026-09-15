#include "foreign_type.h"

#include <stdlib.h>
#include <string.h>

/* Design references:
 * - Generative abstract type identity:
 *   https://people.mpi-sws.org/~dreyer/courses/modules/dreyer03.pdf
 * - Explicit dup/drop ownership lowering (Perceus):
 *   https://www.microsoft.com/en-us/research/publication/perceus-garbage-free-reference-counting-with-reuse/
 */

struct QttForeignTypeEnv {
    QttTypeArena *types;
    QttForeignType *entries;
    size_t count;
    size_t capacity;
};

static char *copy_name(const char *name) {
    size_t size = strlen(name) + 1;
    char *copy = malloc(size);
    if (copy) memcpy(copy, name, size);
    return copy;
}

QttForeignTypeEnv *qtt_foreign_type_env_new(QttTypeArena *types) {
    if (!types) return NULL;
    QttForeignTypeEnv *env = calloc(1, sizeof(*env));
    if (env) env->types = types;
    return env;
}

void qtt_foreign_type_env_free(QttForeignTypeEnv *env) {
    if (!env) return;
    for (size_t i = 0; i < env->count; i++)
        free((char *)env->entries[i].name);
    free(env->entries);
    free(env);
}

size_t qtt_foreign_type_env_count(const QttForeignTypeEnv *env) {
    return env ? env->count : 0;
}

const QttForeignType *qtt_foreign_type_at(
    const QttForeignTypeEnv *env, size_t index) {
    return env && index < env->count ? &env->entries[index] : NULL;
}

const QttForeignType *qtt_foreign_type_lookup_name(
    const QttForeignTypeEnv *env, const char *name) {
    if (!env || !name) return NULL;
    for (size_t i = 0; i < env->count; i++)
        if (strcmp(env->entries[i].name, name) == 0)
            return &env->entries[i];
    return NULL;
}

const QttForeignType *qtt_foreign_type_lookup_authority(
    const QttForeignTypeEnv *env, QttNominalAuthority authority) {
    if (!env) return NULL;
    for (size_t i = 0; i < env->count; i++)
        if (qtt_nominal_authority_equal(
                env->entries[i].authority, authority))
            return &env->entries[i];
    return NULL;
}

QttForeignTypeError qtt_foreign_type_import(
    QttForeignTypeEnv *env, const char *name,
    QttNominalAuthority authority, const Type *representation_type,
    QttOwnershipMode ownership, const QttForeignType **foreign_type) {
    if (foreign_type) *foreign_type = NULL;
    if (!env || !name || !name[0] || !authority.domain ||
        !authority.identity || !representation_type || !foreign_type ||
        representation_type->kind != TYPE_PTR)
        return QTT_FOREIGN_TYPE_INVALID;
    if (ownership == QTT_OWNERSHIP_BORROWED)
        return QTT_FOREIGN_TYPE_UNSAFE_BORROW;
    if (ownership != QTT_OWNERSHIP_CONSUMED &&
        ownership != QTT_OWNERSHIP_SHARED)
        return QTT_FOREIGN_TYPE_INVALID;
    if (qtt_foreign_type_lookup_name(env, name))
        return QTT_FOREIGN_TYPE_DUPLICATE_NAME;
    if (qtt_foreign_type_lookup_authority(env, authority))
        return QTT_FOREIGN_TYPE_DUPLICATE_AUTHORITY;

    QttTypeIdentityError type_error = QTT_TYPE_IDENTITY_OK;
    QttTypeId type_id = qtt_type_intern(
        env->types, representation_type, &type_error);
    if (!type_id.value)
        return type_error == QTT_TYPE_IDENTITY_OUT_OF_MEMORY
            ? QTT_FOREIGN_TYPE_OUT_OF_MEMORY : QTT_FOREIGN_TYPE_INVALID;
    if (env->count == env->capacity) {
        size_t next = env->capacity ? env->capacity * 2 : 16;
        QttForeignType *grown = realloc(
            env->entries, next * sizeof(*grown));
        if (!grown) return QTT_FOREIGN_TYPE_OUT_OF_MEMORY;
        env->entries = grown;
        env->capacity = next;
    }
    char *owned_name = copy_name(name);
    if (!owned_name) return QTT_FOREIGN_TYPE_OUT_OF_MEMORY;
    env->entries[env->count] = (QttForeignType){
        .name = owned_name,
        .authority = authority,
        .type = representation_type,
        .type_id = type_id,
        .representation = QTT_REP_FOREIGN,
        .ownership = ownership,
    };
    *foreign_type = &env->entries[env->count++];
    return QTT_FOREIGN_TYPE_OK;
}
