#ifndef MONAD_QTT_TYPE_IDENTITY_H
#define MONAD_QTT_TYPE_IDENTITY_H

#include "../types.h"

typedef struct {
    uint64_t value;
} QttTypeId;

typedef struct QttTypeArena QttTypeArena;

typedef enum {
    QTT_TYPE_IDENTITY_OK,
    QTT_TYPE_IDENTITY_OUT_OF_MEMORY,
    QTT_TYPE_IDENTITY_UNSUPPORTED_RECURSION,
} QttTypeIdentityError;

QttTypeArena *qtt_type_arena_new(void);
void qtt_type_arena_free(QttTypeArena *arena);
size_t qtt_type_arena_count(const QttTypeArena *arena);

QttTypeId qtt_type_intern(
    QttTypeArena *arena,
    const Type *type,
    QttTypeIdentityError *error);

/*
 * Used when checking serialized certificates that already carry a digest.
 * The digest is only an index hint: structural equality still decides identity.
 */
QttTypeId qtt_type_intern_with_fingerprint(
    QttTypeArena *arena,
    const Type *type,
    uint64_t fingerprint,
    QttTypeIdentityError *error);

const Type *qtt_type_lookup(const QttTypeArena *arena, QttTypeId id);
bool qtt_type_id_equal(QttTypeId left, QttTypeId right);
uint64_t qtt_type_fingerprint(const Type *type);
char *qtt_type_serialize(const Type *type);
Type *qtt_type_deserialize(const char *text);
void qtt_type_free_owned(Type *type);

#endif
