#ifndef MONAD_QTT_DROP_H
#define MONAD_QTT_DROP_H

#include "resource.h"
#include "type_identity.h"

/* A type-directed destructor for the finite unique-tree fragment. */
typedef enum {
    QTT_DROP_NOOP,
    QTT_DROP_OWNED_LEAF,
    QTT_DROP_STRUCT,
    QTT_DROP_SEQUENCE,
    QTT_DROP_OPTIONAL,
    QTT_DROP_BACKREF,
} QttDropKind;

typedef struct QttDropPlan {
    QttDropKind kind;
    struct QttDropPlan **children;
    size_t child_count;
    struct QttDropPlan *backref_target; /* non-owning μ reference */
} QttDropPlan;

typedef struct {
    uint64_t type_fingerprint;
    uint64_t plan_fingerprint;
} QttDestructorId;

typedef struct {
    QttDestructorId id;
    QttDropPlan *plan;
} QttDestructorDescriptor;

/*
 * Evidence that the listed, pairwise-disjoint subobjects have been moved out
 * of an aggregate before its destructor runs.  Paths are canonical structural
 * field ordinals; the certificate is tied to one destructor derivation.
 */
typedef enum {
    QTT_DROP_MASK_VALID,
    QTT_DROP_MASK_OUT_OF_MEMORY,
    QTT_DROP_MASK_INVALID_ROOT,
    QTT_DROP_MASK_INVALID_DESTRUCTOR,
    QTT_DROP_MASK_INVALID_PATH,
    QTT_DROP_MASK_OVERLAP,
} QttDropMaskError;

typedef struct {
    QttDestructorId destructor;
    QttPlace root;
    QttPlace *evacuated;
    size_t evacuated_count;
    uint64_t certificate_fingerprint;
} QttDropMaskCertificate;

typedef enum {
    QTT_DROP_PLAN_OK,
    QTT_DROP_PLAN_OUT_OF_MEMORY,
    QTT_DROP_PLAN_RECURSIVE_TYPE,
    QTT_DROP_PLAN_UNKNOWN_REPRESENTATION,
} QttDropPlanError;

typedef struct QttOwnedObject {
    uint64_t identity;
    bool dropped;
    uint64_t references;
    struct QttOwnedObject **children;
    size_t child_count;
} QttOwnedObject;

typedef enum {
    QTT_DROP_VALID,
    QTT_DROP_DOUBLE,
    QTT_DROP_SHAPE_MISMATCH,
    QTT_DROP_INVALID_MASK,
} QttDropError;

typedef struct {
    QttDropError error;
    size_t visited;
    size_t reclaimed;
    size_t live;
} QttDropExecution;

QttDropPlan *qtt_drop_plan_build(const Type *type, QttDropPlanError *error);
void qtt_drop_plan_free(QttDropPlan *plan);
uint64_t qtt_drop_plan_fingerprint(const QttDropPlan *plan);
QttDestructorDescriptor *qtt_destructor_descriptor_build(
    const Type *type, QttDropPlanError *error);
bool qtt_destructor_descriptor_verify(
    const QttDestructorDescriptor *descriptor, const Type *type);
bool qtt_destructor_id_equal(
    QttDestructorId left, QttDestructorId right);
void qtt_destructor_descriptor_free(
    QttDestructorDescriptor *descriptor);
QttDropMaskCertificate *qtt_drop_mask_build(
    const QttDestructorDescriptor *descriptor, QttPlace root,
    const QttPlace *evacuated, size_t evacuated_count,
    QttDropMaskError *error);
QttDropMaskCertificate *qtt_drop_mask_build_for_resource(
    const QttDestructorDescriptor *descriptor,
    const QttResourceBlock *resources, QttCoreVar root,
    const QttCoreVar *initially_owned, size_t initially_owned_count,
    QttDropMaskError *error);
QttDropMaskError qtt_drop_mask_verify(
    const QttDestructorDescriptor *descriptor,
    const QttDropMaskCertificate *certificate);
void qtt_drop_mask_free(QttDropMaskCertificate *certificate);

QttOwnedObject *qtt_owned_object_new(uint64_t identity, size_t child_count);
bool qtt_owned_object_retain(QttOwnedObject *object);
void qtt_owned_object_free_storage(QttOwnedObject *object);
QttDropExecution qtt_drop_execute(const QttDropPlan *plan,
                                  QttOwnedObject *root);
QttDropExecution qtt_drop_execute_masked(
    const QttDestructorDescriptor *descriptor,
    const QttDropMaskCertificate *certificate,
    QttOwnedObject *root);

#endif
