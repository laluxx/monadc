#include "pipeline.h"
#include "rules.h"

static QttShadowDemandValidation validate_node(
    const QttShadowDemandEvidence *demand) {
    if (!demand || !demand->node_id)
        return QTT_SHADOW_DEMAND_INVALID_IDENTITY;
    if ((demand->premise_count && !demand->premises) ||
        demand->syntactic_premise_count > demand->premise_count)
        return QTT_SHADOW_DEMAND_INVALID_SHAPE;

    size_t expected = demand->premise_count;
    switch (demand->rule) {
    case QTT_DEMAND_RULE_ZERO:
    case QTT_DEMAND_RULE_VAR: expected = 0; break;
    case QTT_DEMAND_RULE_LET:
    case QTT_DEMAND_RULE_IF: expected = 3; break;
    case QTT_DEMAND_RULE_LAMBDA:
    case QTT_DEMAND_RULE_WRITE:
    case QTT_DEMAND_RULE_BORROW: expected = 1; break;
    case QTT_DEMAND_RULE_APPLY:
        expected = demand->direct_application
            ? 2 + 2 * demand->application_argument_count
            : 1 + demand->application_argument_count;
        break;
    case QTT_DEMAND_RULE_SEQUENCE: break;
    }
    if (demand->premise_count != expected)
        return QTT_SHADOW_DEMAND_INVALID_SHAPE;
    for (size_t i = 0; i < demand->premise_count; i++) {
        QttShadowDemandValidation child =
            validate_node(demand->premises[i]);
        if (child != QTT_SHADOW_DEMAND_VALID) return child;
    }

    QttQuantity syntactic =
        qtt_quantity_finite(demand->local_use ? 1 : 0);
    for (size_t i = 0; i < demand->syntactic_premise_count; i++)
        syntactic = qtt_quantity_add(
            syntactic, demand->premises[i]->syntactic);
    QttQuantity runtime = qtt_quantity_finite(0);
    switch (demand->rule) {
    case QTT_DEMAND_RULE_ZERO:
        syntactic = qtt_quantity_finite(0);
        break;
    case QTT_DEMAND_RULE_VAR:
        runtime = syntactic;
        break;
    case QTT_DEMAND_RULE_WRITE:
    case QTT_DEMAND_RULE_BORROW:
        runtime = demand->premises[0]->runtime;
        if (demand->local_use)
            runtime = qtt_quantity_add(
                runtime, qtt_quantity_finite(1));
        break;
    case QTT_DEMAND_RULE_LET:
        runtime = qtt_rule_let(
            demand->premises[0]->runtime,
            demand->premises[1]->runtime,
            demand->premises[2]->runtime,
            demand->subject_is_binding);
        break;
    case QTT_DEMAND_RULE_LAMBDA:
        runtime = qtt_rule_capture(
            demand->premises[0]->runtime,
            demand->subject_is_parameter);
        break;
    case QTT_DEMAND_RULE_IF:
        runtime = qtt_quantity_add(
            demand->premises[0]->runtime,
            qtt_rule_choice(
                demand->premises[1]->runtime,
                demand->premises[2]->runtime));
        break;
    case QTT_DEMAND_RULE_APPLY:
        if (demand->direct_application) {
            size_t n = demand->application_argument_count;
            size_t latent = 1 + n;
            runtime = demand->premises[0]->runtime;
            if (!demand->subject_is_parameter)
                runtime = qtt_rule_sequence(
                    runtime, demand->premises[latent]->runtime);
            for (size_t i = 0; i < n; i++)
                runtime = qtt_rule_sequence(
                    runtime,
                    qtt_quantity_multiply(
                        demand->premises[latent + 1 + i]->runtime,
                        demand->premises[1 + i]->runtime));
            break;
        }
        for (size_t i = 0; i < demand->premise_count; i++)
            runtime = qtt_rule_sequence(
                runtime, demand->premises[i]->runtime);
        break;
    case QTT_DEMAND_RULE_SEQUENCE:
        for (size_t i = 0; i < demand->premise_count; i++)
            runtime = qtt_rule_sequence(
                runtime, demand->premises[i]->runtime);
        break;
    }
    if (!qtt_quantity_equal(syntactic, demand->syntactic) ||
        !qtt_quantity_equal(runtime, demand->runtime))
        return QTT_SHADOW_DEMAND_INVALID_QUANTITY;
    return QTT_SHADOW_DEMAND_VALID;
}

QttShadowDemandValidation qtt_shadow_demand_validate(
    const QttShadowDemandEvidence *demand) {
    return validate_node(demand);
}
