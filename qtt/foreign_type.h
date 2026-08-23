#ifndef MONAD_QTT_FOREIGN_TYPE_H
#define MONAD_QTT_FOREIGN_TYPE_H

#include "signature.h"

typedef struct {
    const char *name;
    QttNominalAuthority authority;
    const Type *type;
    QttTypeId type_id;
    QttRepresentation representation;
    QttOwnershipMode ownership;
} QttForeignType;

typedef struct QttForeignTypeEnv QttForeignTypeEnv;

typedef enum {
    QTT_FOREIGN_TYPE_OK,
    QTT_FOREIGN_TYPE_INVALID,
    QTT_FOREIGN_TYPE_OUT_OF_MEMORY,
    QTT_FOREIGN_TYPE_DUPLICATE_NAME,
    QTT_FOREIGN_TYPE_DUPLICATE_AUTHORITY,
    QTT_FOREIGN_TYPE_UNSAFE_BORROW,
} QttForeignTypeError;

QttForeignTypeEnv *qtt_foreign_type_env_new(QttTypeArena *types);
void qtt_foreign_type_env_free(QttForeignTypeEnv *env);
size_t qtt_foreign_type_env_count(const QttForeignTypeEnv *env);
const QttForeignType *qtt_foreign_type_at(
    const QttForeignTypeEnv *env, size_t index);

QttForeignTypeError qtt_foreign_type_import(
    QttForeignTypeEnv *env, const char *name,
    QttNominalAuthority authority, const Type *representation_type,
    QttOwnershipMode ownership, const QttForeignType **foreign_type);
const QttForeignType *qtt_foreign_type_lookup_name(
    const QttForeignTypeEnv *env, const char *name);
const QttForeignType *qtt_foreign_type_lookup_authority(
    const QttForeignTypeEnv *env, QttNominalAuthority authority);

#endif
