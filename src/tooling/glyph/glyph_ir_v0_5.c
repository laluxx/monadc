#include "glyph_ir_v0_5.h"

#include <stdlib.h>

static GlyphVerifyResult fail(GlyphVerifyCode code, GlyphId object, size_t detail) {
    GlyphVerifyResult result = { code, object, detail };
    return result;
}

static bool span_ok(size_t offset, size_t count, size_t total) {
    return offset <= total && count <= total - offset;
}

static GlyphId port_region(const GlyphGraph *g, GlyphId port_id) {
    const GlyphPort *p = &g->ports[port_id];
    if (p->owner_kind == GLYPH_OWNER_REGION)
        return p->owner;
    if (p->owner >= g->node_count)
        return GLYPH_ID_NONE;
    return g->nodes[p->owner].region;
}

GlyphVerifyResult glyph_verify_graph_kernel(const GlyphGraph *g) {
    size_t *incoming = NULL;
    size_t *fanout = NULL;
    size_t i;

    if (!g || (!g->ports && g->port_count) || (!g->edges && g->edge_count) ||
        (!g->nodes && g->node_count) || (!g->regions && g->region_count) ||
        (!g->port_ids && g->port_id_count) ||
        (!g->edge_targets && g->edge_target_count))
        return fail(GLYPH_VERIFY_NULL, GLYPH_ID_NONE, 0);
    if (g->root_region >= g->region_count ||
        g->regions[g->root_region].parent_node != GLYPH_ID_NONE)
        return fail(GLYPH_VERIFY_ROOT, g->root_region, 0);

    incoming = calloc(g->port_count ? g->port_count : 1, sizeof(*incoming));
    fanout = calloc(g->port_count ? g->port_count : 1, sizeof(*fanout));
    if (!incoming || !fanout) {
        free(incoming);
        free(fanout);
        return fail(GLYPH_VERIFY_NULL, GLYPH_ID_NONE, 1);
    }

    for (i = 0; i < g->region_count; ++i) {
        const GlyphRegion *r = &g->regions[i];
        if (!span_ok(r->input_offset, r->input_count, g->port_id_count) ||
            !span_ok(r->exit_offset, r->exit_count, g->port_id_count) ||
            !span_ok(r->node_offset, r->node_count, g->node_count) ||
            !span_ok(r->edge_offset, r->edge_count, g->edge_count)) {
            free(incoming); free(fanout);
            return fail(GLYPH_VERIFY_RANGE, (GlyphId)i, 0);
        }
    }

    for (i = 0; i < g->node_count; ++i) {
        const GlyphNode *n = &g->nodes[i];
        if (n->region >= g->region_count ||
            !span_ok(n->input_offset, n->input_count, g->port_id_count) ||
            !span_ok(n->output_offset, n->output_count, g->port_id_count)) {
            free(incoming); free(fanout);
            return fail(GLYPH_VERIFY_RANGE, (GlyphId)i, 1);
        }
    }

    for (i = 0; i < g->port_count; ++i) {
        const GlyphPort *p = &g->ports[i];
        if ((p->owner_kind == GLYPH_OWNER_NODE && p->owner >= g->node_count) ||
            (p->owner_kind == GLYPH_OWNER_REGION && p->owner >= g->region_count)) {
            free(incoming); free(fanout);
            return fail(GLYPH_VERIFY_OWNER, (GlyphId)i, p->owner);
        }
    }

    for (i = 0; i < g->edge_count; ++i) {
        const GlyphEdge *e = &g->edges[i];
        size_t j;
        const GlyphPort *src;
        if (e->region >= g->region_count || e->source_port >= g->port_count ||
            !span_ok(e->target_offset, e->target_count, g->edge_target_count) ||
            e->target_count == 0) {
            free(incoming); free(fanout);
            return fail(GLYPH_VERIFY_RANGE, (GlyphId)i, 2);
        }
        src = &g->ports[e->source_port];
        if (src->direction != GLYPH_PORT_OUT) {
            free(incoming); free(fanout);
            return fail(GLYPH_VERIFY_DIRECTION, e->source_port, 0);
        }
        if (port_region(g, e->source_port) != e->region) {
            free(incoming); free(fanout);
            return fail(GLYPH_VERIFY_REGION_CROSSING, (GlyphId)i, e->source_port);
        }
        for (j = 0; j < e->target_count; ++j) {
            GlyphId tid = g->edge_targets[e->target_offset + j];
            const GlyphPort *dst;
            if (tid >= g->port_count) {
                free(incoming); free(fanout);
                return fail(GLYPH_VERIFY_RANGE, (GlyphId)i, tid);
            }
            dst = &g->ports[tid];
            if (dst->direction != GLYPH_PORT_IN) {
                free(incoming); free(fanout);
                return fail(GLYPH_VERIFY_DIRECTION, tid, 1);
            }
            if (port_region(g, tid) != e->region) {
                free(incoming); free(fanout);
                return fail(GLYPH_VERIFY_REGION_CROSSING, (GlyphId)i, tid);
            }
            if (src->kind != dst->kind || src->linearity != dst->linearity ||
                src->type_hash != dst->type_hash) {
                free(incoming); free(fanout);
                return fail(GLYPH_VERIFY_TYPE, (GlyphId)i, tid);
            }
            if (src->erased && !dst->erased) {
                free(incoming); free(fanout);
                return fail(GLYPH_VERIFY_ERASURE, (GlyphId)i, tid);
            }
            ++incoming[tid];
            ++fanout[e->source_port];
        }
    }

    for (i = 0; i < g->port_count; ++i) {
        const GlyphPort *p = &g->ports[i];
        if (p->direction == GLYPH_PORT_IN && incoming[i] != 1) {
            size_t count = incoming[i];
            free(incoming); free(fanout);
            return fail(GLYPH_VERIFY_INPUT_PRODUCER, (GlyphId)i, count);
        }
        if ((p->linearity == GLYPH_LINEAR && p->direction == GLYPH_PORT_OUT && fanout[i] != 1) ||
            (p->linearity == GLYPH_AFFINE && p->direction == GLYPH_PORT_OUT && fanout[i] > 1)) {
            size_t count = fanout[i];
            free(incoming); free(fanout);
            return fail(GLYPH_VERIFY_LINEARITY, (GlyphId)i, count);
        }
    }

    free(incoming);
    free(fanout);
    return fail(GLYPH_VERIFY_OK, GLYPH_ID_NONE, 0);
}

const char *glyph_verify_code_name(GlyphVerifyCode code) {
    switch (code) {
    case GLYPH_VERIFY_OK: return "ok";
    case GLYPH_VERIFY_NULL: return "null";
    case GLYPH_VERIFY_RANGE: return "range";
    case GLYPH_VERIFY_ROOT: return "root";
    case GLYPH_VERIFY_OWNER: return "owner";
    case GLYPH_VERIFY_DIRECTION: return "direction";
    case GLYPH_VERIFY_REGION_CROSSING: return "region-crossing";
    case GLYPH_VERIFY_TYPE: return "type";
    case GLYPH_VERIFY_INPUT_PRODUCER: return "input-producer";
    case GLYPH_VERIFY_LINEARITY: return "linearity";
    case GLYPH_VERIFY_ERASURE: return "erasure";
    }
    return "unknown";
}
