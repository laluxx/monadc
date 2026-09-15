#include "demand.h"
#include "rules.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct QttDemandCertificate {
    const QttCoreNode *root;
    uint64_t fingerprint;
    char formatted[512];
};

static uint64_t hash_mix(uint64_t hash, uint64_t value) {
    hash ^= value;
    return hash * UINT64_C(1099511628211);
}

static uint64_t hash_text(uint64_t hash, const char *text) {
    if (!text) return hash_mix(hash, 0);
    for (; *text; text++) hash = hash_mix(hash, (unsigned char)*text);
    return hash_mix(hash, 0xff);
}

static uint64_t core_fingerprint(const QttCoreNode *core) {
    if (!core) return UINT64_C(0xcbf29ce484222325);
    uint64_t hash = hash_mix(UINT64_C(1469598103934665603), core->kind);
    switch (core->kind) {
    case QTT_CORE_VAR:
        hash = hash_mix(hash, core->var.module_id);
        return hash_mix(hash, core->var.binder_id);
    case QTT_CORE_PLACE:
        hash = hash_mix(hash, core->place.root.module_id);
        hash = hash_mix(hash, core->place.root.binder_id);
        hash = hash_mix(hash, core->place.projection_depth);
        for (uint8_t i = 0; i < core->place.projection_depth; i++)
            hash = hash_mix(hash, core->place.projection_path[i]);
        return hash;
    case QTT_CORE_GLOBAL:
        return hash_text(hash, core->global.name);
    case QTT_CORE_LITERAL:
    case QTT_CORE_QUOTE:
        if (!core->literal.source) return hash_mix(hash, 0);
        hash = hash_mix(hash, core->literal.source->type);
        if (core->literal.source->type == AST_NUMBER) {
            uint64_t bits = 0;
            memcpy(&bits, &core->literal.source->number, sizeof(bits));
            return hash_mix(hash, bits);
        }
        if (core->literal.source->type == AST_STRING)
            return hash_text(hash, core->literal.source->string);
        return hash;
    case QTT_CORE_LET:
        hash = hash_mix(hash, core->let.binding.module_id);
        hash = hash_mix(hash, core->let.binding.binder_id);
        hash = hash_mix(hash, core_fingerprint(core->let.value));
        return hash_mix(hash, core_fingerprint(core->let.body));
    case QTT_CORE_IF:
        hash = hash_mix(
            hash, core_fingerprint(core->conditional.condition));
        hash = hash_mix(
            hash, core_fingerprint(core->conditional.then_branch));
        return hash_mix(
            hash, core_fingerprint(core->conditional.else_branch));
    case QTT_CORE_SEQUENCE:
        hash = hash_mix(hash, core->sequence.count);
        for (size_t i = 0; i < core->sequence.count; i++)
            hash = hash_mix(
                hash, core_fingerprint(core->sequence.items[i]));
        return hash;
    case QTT_CORE_WRITE:
        hash = hash_mix(hash, core->write.place.root.module_id);
        hash = hash_mix(hash, core->write.place.root.binder_id);
        hash = hash_mix(hash, core->write.place.projection_id);
        return hash_mix(hash, core_fingerprint(core->write.value));
    case QTT_CORE_BORROW:
        hash = hash_mix(hash, core->borrow.binding.module_id);
        hash = hash_mix(hash, core->borrow.binding.binder_id);
        hash = hash_mix(hash, core->borrow.parent.module_id);
        hash = hash_mix(hash, core->borrow.parent.binder_id);
        hash = hash_mix(hash, core->borrow.loan_kind);
        hash = hash_mix(hash, core->borrow.place.root.binder_id);
        hash = hash_mix(hash, core->borrow.place.projection_id);
        return hash_mix(hash, core_fingerprint(core->borrow.body));
    case QTT_CORE_PERFORM:
        hash = hash_text(hash, core->perform.effect_name);
        hash = hash_mix(hash, core->perform.capability_id);
        hash = hash_mix(hash, core->perform.constructor_id);
        hash = hash_mix(hash, core->perform.resumption.is_omega);
        hash = hash_mix(hash, core->perform.resumption.finite);
        hash = hash_mix(hash, core->perform.scoped);
        return hash_mix(hash, core_fingerprint(core->perform.argument));
    case QTT_CORE_HANDLE:
        hash = hash_text(hash, core->handle.profile_name);
        hash = hash_mix(hash, core->handle.capability_id);
        hash = hash_mix(hash, core->handle.resumption_class);
        hash = hash_mix(hash, core->handle.deep);
        hash = hash_text(hash, core->handle.portable_proof);
        hash = hash_mix(hash, core_fingerprint(core->handle.computation));
        return hash_mix(hash, core_fingerprint(core->handle.clause));
    case QTT_CORE_APPLY:
        hash = hash_mix(hash, core_fingerprint(core->apply.callee));
        hash = hash_mix(hash, core->apply.argument_count);
        for (size_t i = 0; i < core->apply.argument_count; i++)
            hash = hash_mix(
                hash, core_fingerprint(core->apply.arguments[i]));
        return hash;
    case QTT_CORE_LAMBDA:
        hash = hash_mix(hash, core->lambda.param_count);
        for (size_t i = 0; i < core->lambda.param_count; i++) {
            hash = hash_mix(hash, core->lambda.params[i].module_id);
            hash = hash_mix(hash, core->lambda.params[i].binder_id);
        }
        return hash_mix(hash, core_fingerprint(core->lambda.body));
    }
    return hash;
}

static size_t saturating_add(size_t left, size_t right) {
    return SIZE_MAX - left < right ? SIZE_MAX : left + right;
}

static bool contains_node(const QttCoreNode *root,
                          const QttCoreNode *wanted) {
    if (!root) return false;
    if (root == wanted) return true;
    switch (root->kind) {
    case QTT_CORE_LET:
        return contains_node(root->let.value, wanted) ||
               contains_node(root->let.body, wanted);
    case QTT_CORE_IF:
        return contains_node(root->conditional.condition, wanted) ||
               contains_node(root->conditional.then_branch, wanted) ||
               contains_node(root->conditional.else_branch, wanted);
    case QTT_CORE_SEQUENCE:
        for (size_t i = 0; i < root->sequence.count; i++)
            if (contains_node(root->sequence.items[i], wanted)) return true;
        return false;
    case QTT_CORE_APPLY:
        if (contains_node(root->apply.callee, wanted)) return true;
        for (size_t i = 0; i < root->apply.argument_count; i++)
            if (contains_node(root->apply.arguments[i], wanted)) return true;
        return false;
    case QTT_CORE_LAMBDA:
        return contains_node(root->lambda.body, wanted);
    case QTT_CORE_WRITE:
        return contains_node(root->write.value, wanted);
    case QTT_CORE_BORROW:
        return contains_node(root->borrow.body, wanted);
    default:
        return false;
    }
}

static size_t occurrences(const QttCoreNode *core, QttCoreVar var) {
    if (!core) return 0;
    switch (core->kind) {
    case QTT_CORE_VAR:
        return qtt_core_var_equal(core->var, var) ? 1 : 0;
    case QTT_CORE_PLACE:
        return qtt_core_var_equal(core->place.root, var) ? 1 : 0;
    case QTT_CORE_LET:
        return saturating_add(occurrences(core->let.value, var),
                              occurrences(core->let.body, var));
    case QTT_CORE_IF:
        return saturating_add(
            occurrences(core->conditional.condition, var),
            saturating_add(
                occurrences(core->conditional.then_branch, var),
                occurrences(core->conditional.else_branch, var)));
    case QTT_CORE_SEQUENCE: {
        size_t count = 0;
        for (size_t i = 0; i < core->sequence.count; i++)
            count = saturating_add(
                count, occurrences(core->sequence.items[i], var));
        return count;
    }
    case QTT_CORE_APPLY: {
        size_t count = occurrences(core->apply.callee, var);
        for (size_t i = 0; i < core->apply.argument_count; i++)
            count = saturating_add(
                count, occurrences(core->apply.arguments[i], var));
        return count;
    }
    case QTT_CORE_LAMBDA:
        return occurrences(core->lambda.body, var);
    case QTT_CORE_WRITE:
        return saturating_add(
            qtt_core_var_equal(core->write.place.root, var) ? 1 : 0,
            occurrences(core->write.value, var));
    case QTT_CORE_BORROW:
        return saturating_add(
            qtt_core_var_equal(core->borrow.place.root, var) ? 1 : 0,
            occurrences(core->borrow.body, var));
    default:
        return 0;
    }
}

static QttDemandRule rule_for_core(const QttCoreNode *core) {
    switch (core->kind) {
    case QTT_CORE_VAR:
    case QTT_CORE_PLACE: return QTT_DEMAND_RULE_VAR;
    case QTT_CORE_LET: return QTT_DEMAND_RULE_LET;
    case QTT_CORE_IF: return QTT_DEMAND_RULE_IF;
    case QTT_CORE_SEQUENCE: return QTT_DEMAND_RULE_SEQUENCE;
    case QTT_CORE_APPLY: return QTT_DEMAND_RULE_APPLY;
    case QTT_CORE_LAMBDA: return QTT_DEMAND_RULE_LAMBDA;
    case QTT_CORE_WRITE: return QTT_DEMAND_RULE_WRITE;
    case QTT_CORE_BORROW: return QTT_DEMAND_RULE_BORROW;
    default: return QTT_DEMAND_RULE_ZERO;
    }
}

static size_t premise_count_for_core(const QttCoreNode *core) {
    switch (core->kind) {
    /* value, body demand for the subject, body demand for the binder */
    case QTT_CORE_LET: return 3;
    case QTT_CORE_IF: return 3;
    case QTT_CORE_SEQUENCE: return core->sequence.count;
    case QTT_CORE_APPLY:
        if (core->apply.callee &&
            core->apply.callee->kind == QTT_CORE_LAMBDA &&
            core->apply.argument_count ==
                core->apply.callee->lambda.param_count)
            /* callee, args, latent subject, one domain proof per arg */
            return 2 + 2 * core->apply.argument_count;
        return 1 + core->apply.argument_count;
    case QTT_CORE_LAMBDA: return 1;
    case QTT_CORE_WRITE: return 1;
    case QTT_CORE_BORROW: return 1;
    default: return 0;
    }
}

static const QttCoreNode *premise_source(const QttCoreNode *core,
                                         size_t index) {
    switch (core->kind) {
    case QTT_CORE_LET:
        return index ? core->let.body : core->let.value;
    case QTT_CORE_IF:
        if (!index) return core->conditional.condition;
        return index == 1 ? core->conditional.then_branch
                          : core->conditional.else_branch;
    case QTT_CORE_SEQUENCE:
        return core->sequence.items[index];
    case QTT_CORE_APPLY:
        if (!index) return core->apply.callee;
        if (index <= core->apply.argument_count)
            return core->apply.arguments[index - 1];
        return core->apply.callee->lambda.body;
    case QTT_CORE_LAMBDA:
        return core->lambda.body;
    case QTT_CORE_WRITE:
        return core->write.value;
    case QTT_CORE_BORROW:
        return core->borrow.body;
    default:
        return NULL;
    }
}

static QttCoreVar premise_subject(
    const QttCoreNode *core, size_t index, QttCoreVar subject) {
    if (core->kind == QTT_CORE_LET && index == 2)
        return core->let.binding;
    if (core->kind == QTT_CORE_APPLY && core->apply.callee &&
        core->apply.callee->kind == QTT_CORE_LAMBDA) {
        size_t base = 2 + core->apply.argument_count;
        if (index >= base)
            return core->apply.callee->lambda.params[index - base];
    }
    return subject;
}

static size_t syntactic_premise_count(const QttCoreNode *core) {
    if (core->kind == QTT_CORE_LET) return 2;
    if (core->kind == QTT_CORE_APPLY)
        return 1 + core->apply.argument_count;
    return premise_count_for_core(core);
}

static void derivation_grades(QttDemandDerivation *derivation) {
    if (derivation->rule == QTT_DEMAND_RULE_VAR) {
        bool used = qtt_core_var_equal(
            derivation->source->var, derivation->subject);
        derivation->syntactic = qtt_quantity_finite(used ? 1 : 0);
        derivation->runtime = derivation->syntactic;
        return;
    }
    derivation->syntactic = qtt_quantity_finite(0);
    derivation->runtime = qtt_quantity_finite(0);
    if (derivation->rule == QTT_DEMAND_RULE_ZERO) return;
    size_t syntactic_count =
        syntactic_premise_count(derivation->source);
    for (size_t i = 0; i < syntactic_count; i++)
        derivation->syntactic = qtt_quantity_add(
            derivation->syntactic,
            derivation->premises[i]->syntactic);
    if (derivation->rule == QTT_DEMAND_RULE_WRITE) {
        bool targets_subject = qtt_core_var_equal(
            derivation->source->write.place.root,
            derivation->subject);
        if (targets_subject)
            derivation->syntactic = qtt_quantity_add(
                derivation->syntactic, qtt_quantity_finite(1));
        derivation->runtime = derivation->premises[0]->runtime;
        if (targets_subject)
            derivation->runtime = qtt_quantity_add(
                derivation->runtime, qtt_quantity_finite(1));
        return;
    }
    if (derivation->rule == QTT_DEMAND_RULE_BORROW) {
        bool targets_subject = qtt_core_var_equal(
            derivation->source->borrow.place.root,
            derivation->subject);
        derivation->syntactic = derivation->premises[0]->syntactic;
        derivation->runtime = derivation->premises[0]->runtime;
        if (targets_subject) {
            QttQuantity one = qtt_quantity_finite(1);
            derivation->syntactic = qtt_quantity_add(
                derivation->syntactic, one);
            derivation->runtime = qtt_quantity_add(
                derivation->runtime, one);
        }
        return;
    }
    if (derivation->rule == QTT_DEMAND_RULE_LET) {
        derivation->runtime = qtt_rule_let(
            derivation->premises[0]->runtime,
            derivation->premises[1]->runtime,
            derivation->premises[2]->runtime,
            qtt_core_var_equal(
                derivation->source->let.binding,
                derivation->subject));
        return;
    }
    if (derivation->rule == QTT_DEMAND_RULE_LAMBDA) {
        for (size_t i = 0;
             i < derivation->source->lambda.param_count; i++)
            if (qtt_core_var_equal(
                    derivation->source->lambda.params[i],
                    derivation->subject))
                return;
        derivation->runtime = qtt_rule_capture(
            derivation->premises[0]->runtime, false);
        return;
    }
    if (derivation->rule == QTT_DEMAND_RULE_APPLY &&
        derivation->source->apply.callee &&
        derivation->source->apply.callee->kind == QTT_CORE_LAMBDA &&
        derivation->source->apply.argument_count ==
            derivation->source->apply.callee->lambda.param_count) {
        size_t argument_count =
            derivation->source->apply.argument_count;
        size_t latent_index = 1 + argument_count;
        derivation->runtime = qtt_rule_direct_application(
            derivation->premises[0]->runtime,
            qtt_quantity_finite(0), NULL, NULL, 0);
        bool subject_is_parameter = false;
        for (size_t i = 0; i < argument_count; i++)
            if (qtt_core_var_equal(
                    derivation->source->apply.callee->lambda.params[i],
                    derivation->subject))
                subject_is_parameter = true;
        if (!subject_is_parameter)
            derivation->runtime = qtt_rule_sequence(
                derivation->runtime,
                derivation->premises[latent_index]->runtime);
        for (size_t i = 0; i < argument_count; i++)
            derivation->runtime = qtt_rule_sequence(
                derivation->runtime,
                qtt_quantity_multiply(
                    derivation->premises[
                        latent_index + 1 + i]->runtime,
                    derivation->premises[1 + i]->runtime));
        return;
    }
    if (derivation->rule == QTT_DEMAND_RULE_IF) {
        QttQuantity choice = qtt_rule_choice(
            derivation->premises[1]->runtime,
            derivation->premises[2]->runtime);
        derivation->runtime = qtt_quantity_add(
            derivation->premises[0]->runtime, choice);
        return;
    }
    for (size_t i = 0; i < derivation->premise_count; i++)
        derivation->runtime = qtt_quantity_add(
            derivation->runtime, derivation->premises[i]->runtime);
}

static QttDemandDerivation *prove_node(const QttCoreNode *core,
                                       QttCoreVar var,
                                       QttDemandError *error) {
    QttDemandDerivation *proof = calloc(1, sizeof(*proof));
    if (!proof) {
        *error = QTT_DEMAND_OUT_OF_MEMORY;
        return NULL;
    }
    proof->rule = rule_for_core(core);
    proof->source = core;
    proof->subject = var;
    proof->premise_count = premise_count_for_core(core);
    if (proof->premise_count)
        proof->premises = calloc(
            proof->premise_count, sizeof(*proof->premises));
    if (proof->premise_count && !proof->premises) {
        free(proof);
        *error = QTT_DEMAND_OUT_OF_MEMORY;
        return NULL;
    }
    for (size_t i = 0; i < proof->premise_count; i++) {
        proof->premises[i] =
            prove_node(
                premise_source(core, i),
                premise_subject(core, i, var), error);
        if (!proof->premises[i]) {
            qtt_demand_derivation_free(proof);
            return NULL;
        }
    }
    derivation_grades(proof);
    return proof;
}

QttDemandDerivation *qtt_demand_prove(
    const QttDemandCertificate *certificate,
    const QttCoreNode *subtree,
    QttCoreVar var,
    QttDemandError *error) {
    QttDemandError local = QTT_DEMAND_OK;
    if (!certificate || !subtree ||
        qtt_demand_validate(certificate, certificate->root) !=
            QTT_DEMAND_VALID ||
        !contains_node(certificate->root, subtree)) {
        local = QTT_DEMAND_UNSUPPORTED_CORE;
        if (error) *error = local;
        return NULL;
    }
    QttDemandDerivation *proof = prove_node(subtree, var, &local);
    if (error) *error = local;
    return proof;
}

static QttDemandProofValidation check_node(
    const QttDemandDerivation *proof,
    const QttCoreNode *core,
    QttCoreVar var) {
    if (!proof || proof->source != core ||
        !qtt_core_var_equal(proof->subject, var))
        return QTT_DEMAND_PROOF_SOURCE_MISMATCH;
    if (proof->rule != rule_for_core(core))
        return QTT_DEMAND_PROOF_RULE_MISMATCH;
    size_t expected_count = premise_count_for_core(core);
    if (proof->premise_count != expected_count ||
        (expected_count && !proof->premises))
        return QTT_DEMAND_PROOF_PREMISE_MISMATCH;
    for (size_t i = 0; i < expected_count; i++) {
        QttDemandProofValidation checked = check_node(
            proof->premises[i], premise_source(core, i),
            premise_subject(core, i, var));
        if (checked != QTT_DEMAND_PROOF_VALID) return checked;
    }
    QttDemandDerivation expected = {
        .rule = proof->rule,
        .source = core,
        .subject = var,
        .premises = proof->premises,
        .premise_count = proof->premise_count,
    };
    derivation_grades(&expected);
    if (!qtt_quantity_equal(proof->syntactic, expected.syntactic) ||
        !qtt_quantity_equal(proof->runtime, expected.runtime))
        return QTT_DEMAND_PROOF_GRADE_MISMATCH;
    return QTT_DEMAND_PROOF_VALID;
}

QttDemandProofValidation qtt_demand_check(
    const QttDemandDerivation *derivation,
    const QttCoreNode *subtree,
    QttCoreVar var) {
    if (!subtree) return QTT_DEMAND_PROOF_SOURCE_MISMATCH;
    return check_node(derivation, subtree, var);
}

uint64_t qtt_demand_derivation_node_fingerprint(
    const QttDemandDerivation *derivation) {
    if (!derivation || !derivation->source) return 0;
    uint64_t hash = core_fingerprint(derivation->source);
    hash = hash_mix(hash, derivation->subject.module_id);
    hash = hash_mix(hash, derivation->subject.binder_id);
    return hash ? hash : 1;
}

void qtt_demand_derivation_free(QttDemandDerivation *derivation) {
    if (!derivation) return;
    for (size_t i = 0; i < derivation->premise_count; i++)
        qtt_demand_derivation_free(derivation->premises[i]);
    free(derivation->premises);
    free(derivation);
}

QttDemandCertificate *qtt_demand_derive(const QttCoreNode *core,
                                        QttDemandError *error) {
    if (!core) {
        if (error) *error = QTT_DEMAND_UNSUPPORTED_CORE;
        return NULL;
    }
    QttDemandCertificate *certificate = calloc(1, sizeof(*certificate));
    if (!certificate) {
        if (error) *error = QTT_DEMAND_OUT_OF_MEMORY;
        return NULL;
    }
    certificate->root = core;
    certificate->fingerprint = core_fingerprint(core);
    if (error) *error = QTT_DEMAND_OK;
    return certificate;
}

QttDemandValidation qtt_demand_validate(
    const QttDemandCertificate *certificate,
    const QttCoreNode *core) {
    if (!certificate || !core || certificate->root != core ||
        certificate->fingerprint != core_fingerprint(core))
        return QTT_DEMAND_SOURCE_MISMATCH;
    return QTT_DEMAND_VALID;
}

size_t qtt_demand_occurrences(const QttDemandCertificate *certificate,
                              const QttCoreNode *subtree,
                              QttCoreVar var) {
    QttDemandError error = QTT_DEMAND_OK;
    QttDemandDerivation *proof =
        qtt_demand_prove(certificate, subtree, var, &error);
    if (!proof ||
        qtt_demand_check(proof, subtree, var) !=
            QTT_DEMAND_PROOF_VALID) {
        qtt_demand_derivation_free(proof);
        return 0;
    }
    size_t result = proof->syntactic.is_omega ||
                    proof->syntactic.finite > SIZE_MAX
        ? SIZE_MAX : (size_t)proof->syntactic.finite;
    qtt_demand_derivation_free(proof);
    return result;
}

QttQuantity qtt_demand_runtime(
    const QttDemandCertificate *certificate,
    const QttCoreNode *subtree,
    QttCoreVar var) {
    QttDemandError error = QTT_DEMAND_OK;
    QttDemandDerivation *proof =
        qtt_demand_prove(certificate, subtree, var, &error);
    if (!proof ||
        qtt_demand_check(proof, subtree, var) !=
            QTT_DEMAND_PROOF_VALID) {
        qtt_demand_derivation_free(proof);
        return qtt_quantity_omega();
    }
    QttQuantity result = proof->runtime;
    qtt_demand_derivation_free(proof);
    return result;
}

static size_t append_text(char *buffer, size_t capacity, size_t used,
                          const char *text) {
    if (used >= capacity) return used;
    int wrote = snprintf(buffer + used, capacity - used, "%s", text);
    return wrote < 0 ? used : used + (size_t)wrote;
}

static size_t format_demand(char *buffer, size_t capacity, size_t used,
                            const QttCoreNode *core, QttCoreVar var) {
    if (!core) return append_text(buffer, capacity, used, "0");
    if (core->kind == QTT_CORE_IF) {
        size_t condition = occurrences(core->conditional.condition, var);
        size_t then_count =
            occurrences(core->conditional.then_branch, var);
        size_t else_count =
            occurrences(core->conditional.else_branch, var);
        char choice[128];
        snprintf(choice, sizeof(choice), "choice(%zu,%zu)",
                 then_count, else_count);
        if (!condition)
            return append_text(buffer, capacity, used, choice);
        char combined[192];
        snprintf(combined, sizeof(combined), "add(%zu,%s)",
                 condition, choice);
        return append_text(buffer, capacity, used, combined);
    }
    size_t count = occurrences(core, var);
    char finite[64];
    snprintf(finite, sizeof(finite), "%zu", count);
    return append_text(buffer, capacity, used, finite);
}

const char *qtt_demand_format(QttDemandCertificate *certificate,
                              const QttCoreNode *subtree,
                              QttCoreVar var) {
    if (!certificate) return NULL;
    certificate->formatted[0] = '\0';
    format_demand(certificate->formatted,
                  sizeof(certificate->formatted), 0, subtree, var);
    return certificate->formatted;
}

void qtt_demand_certificate_free(QttDemandCertificate *certificate) {
    free(certificate);
}
