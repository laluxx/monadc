#ifndef MONAD_QTT_INTERFACE_H
#define MONAD_QTT_INTERFACE_H

#include "signature.h"
#include "../effects/effect.h"
#include "../effects/constraints.h"

#include <stdio.h>

#define QTT_INTERFACE_VERSION 12

typedef struct {
    size_t stage;
    char *trait;
} QttInterfaceEffectPredicate;

typedef struct {
    char *premise;
    char *consequence;
    char *provenance;
} QttEffectTraitImplication;

typedef enum {
    QTT_INTERFACE_OK,
    QTT_INTERFACE_IO_ERROR,
    QTT_INTERFACE_OUT_OF_MEMORY,
    QTT_INTERFACE_BAD_MAGIC,
    QTT_INTERFACE_UNSUPPORTED_VERSION,
    QTT_INTERFACE_MALFORMED,
    QTT_INTERFACE_UNSUPPORTED_TYPE,
    QTT_INTERFACE_FINGERPRINT_MISMATCH,
} QttInterfaceError;

typedef struct {
    char *name;
    unsigned interface_version;
    bool has_ownership_signature;
    QttFunctionSignature signature;
    uint64_t stable_fingerprint;
    char *callable_contract;
    uint64_t callable_contract_fingerprint;
    char *hm_scheme;
    bool has_effect_judgment;
    uint64_t effect_row_fingerprint;
    uint64_t effect_constraint_fingerprint;
    QttEffectConstraintResult effect_constraint_result;
    QttInterfaceEffectPredicate *effect_predicates;
    size_t effect_predicate_count;
    uint64_t effect_judgment_fingerprint;
} QttInterfaceContract;

typedef struct QttInterface QttInterface;

QttInterface *qtt_interface_new(const char *module_name);
void qtt_interface_set_artifact_fingerprint(
    QttInterface *interface, uint64_t fingerprint);
uint64_t qtt_interface_artifact_fingerprint(
    const QttInterface *interface);
void qtt_interface_free(QttInterface *interface);
const char *qtt_interface_module(const QttInterface *interface);
size_t qtt_interface_count(const QttInterface *interface);
const QttInterfaceContract *qtt_interface_contract(
    const QttInterface *interface, size_t index);
size_t qtt_interface_effect_declaration_count(const QttInterface *interface);
const QttEffectDeclaration *qtt_interface_effect_declaration(
    const QttInterface *interface, size_t index);
bool qtt_interface_add_effect_declaration(
    QttInterface *interface, const QttEffectDeclaration *declaration);
size_t qtt_interface_handler_profile_count(const QttInterface *interface);
const QttEffectHandlerProfile *qtt_interface_handler_profile(
    const QttInterface *interface, size_t index);
bool qtt_interface_add_handler_profile(
    QttInterface *interface, const QttEffectHandlerProfile *profile);
size_t qtt_interface_trait_implication_count(const QttInterface *interface);
const QttEffectTraitImplication *qtt_interface_trait_implication(
    const QttInterface *interface, size_t index);
bool qtt_interface_add_trait_implication(
    QttInterface *interface, const char *premise,
    const char *consequence, const char *provenance);

/*
 * This fingerprint is stable across compilation-local TypeId allocation.
 * Structural type fingerprints, not arena indices, enter the digest.
 */
uint64_t qtt_interface_signature_fingerprint(
    const QttFunctionSignature *signature);

bool qtt_interface_add(
    QttInterface *interface, const char *name,
    const QttFunctionSignature *signature);
bool qtt_interface_add_metadata(QttInterface *interface, const char *name);
bool qtt_interface_set_callable_contract(
    QttInterface *interface, const char *name, const char *portable_contract,
    uint64_t semantic_fingerprint);
bool qtt_interface_set_hm_scheme(
    QttInterface *interface, const char *name, const char *portable_scheme);
bool qtt_interface_set_effect_judgment(
    QttInterface *interface, const char *name,
    uint64_t row_fingerprint, uint64_t constraint_fingerprint,
    QttEffectConstraintResult result, const size_t *predicate_stages,
    const char *const *predicate_traits, size_t predicate_count);
QttInterfaceError qtt_interface_write(
    const QttInterface *interface, const char *path);
QttInterface *qtt_interface_read(
    const char *path, QttInterfaceError *error);

#endif
