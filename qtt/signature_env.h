#ifndef MONAD_QTT_SIGNATURE_ENV_H
#define MONAD_QTT_SIGNATURE_ENV_H

#include "signature.h"

typedef struct {
    uint64_t module_id;
    uint64_t name_hash;
    const char *name;
} QttCallableId;

typedef struct QttSignatureEnv QttSignatureEnv;

typedef enum {
    QTT_SIGNATURE_ENV_OK,
    QTT_SIGNATURE_ENV_OUT_OF_MEMORY,
    QTT_SIGNATURE_ENV_DUPLICATE,
    QTT_SIGNATURE_ENV_STALE_SIGNATURE,
    QTT_SIGNATURE_ENV_NON_CANONICAL,
} QttSignatureEnvError;

QttSignatureEnv *qtt_signature_env_new(QttTypeArena *types);
void qtt_signature_env_free(QttSignatureEnv *env);
size_t qtt_signature_env_count(const QttSignatureEnv *env);

QttSignatureEnvError qtt_signature_env_register(
    QttSignatureEnv *env,
    uint64_t module_id,
    const char *name,
    QttFunctionSignature *signature,
    QttCallableId *id);
QttSignatureEnvError qtt_signature_env_register_with_hash(
    QttSignatureEnv *env,
    uint64_t module_id,
    const char *name,
    uint64_t name_hash,
    QttFunctionSignature *signature,
    QttCallableId *id);

QttFunctionSignature *qtt_signature_env_lookup(
    const QttSignatureEnv *env,
    uint64_t module_id,
    const char *name);
QttFunctionSignature *qtt_signature_env_lookup_id(
    const QttSignatureEnv *env,
    QttCallableId id);
bool qtt_signature_env_resolve(
    const QttSignatureEnv *env,
    uint64_t module_id,
    const char *name,
    QttCallableId *id,
    QttFunctionSignature **signature);
bool qtt_callable_id_equal(QttCallableId left, QttCallableId right);

#endif
