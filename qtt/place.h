#ifndef MONAD_QTT_PLACE_H
#define MONAD_QTT_PLACE_H

#include "core.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/*
 * projection_id == 0 denotes the whole root. Nonzero IDs are canonical,
 * type-checked projection paths supplied by lowering. Distinct nonzero IDs
 * under one root are conservatively treated as disjoint siblings.
 */
static inline QttPlace qtt_place_root(QttCoreVar root) {
    return (QttPlace){.root = root};
}

static inline QttPlace qtt_place_project(
    QttPlace root, uint64_t projection_id) {
    root.projection_id = projection_id;
    root.projection_depth = 0;
    memset(root.projection_path, 0, sizeof(root.projection_path));
    return root;
}

/*
 * Derive a one-level projection from checked layout metadata.  Field ordinals
 * are stable within the structural layout and become compact nonzero place
 * identities. Nested layout projections append to an explicitly bounded
 * prefix path; exceeding that bound fails closed.
 */
static inline bool qtt_place_layout_field(
    QttPlace root, const Type *layout, const char *field,
    QttPlace *projected) {
    if (!projected || !field || !*field ||
        !layout ||
        layout->kind != TYPE_LAYOUT ||
        !layout->layout_fields || layout->layout_field_count <= 0)
        return false;
    if (root.projection_id != 0 && root.projection_depth == 0)
        return false;
    if (root.projection_depth >= QTT_PLACE_MAX_DEPTH)
        return false;
    size_t found = (size_t)layout->layout_field_count;
    for (int i = 0; i < layout->layout_field_count; i++) {
        const char *candidate = layout->layout_fields[i].name;
        if (!candidate || strcmp(candidate, field) != 0)
            continue;
        if (found != (size_t)layout->layout_field_count)
            return false;
        found = (size_t)i;
    }
    if (found == (size_t)layout->layout_field_count)
        return false;
    *projected = root;
    projected->projection_path[projected->projection_depth++] =
        (uint32_t)found + 1;
    projected->projection_id = projected->projection_path[0];
    return true;
}

static inline bool qtt_place_equal(QttPlace left, QttPlace right) {
    if (left.root.module_id != right.root.module_id ||
        left.root.binder_id != right.root.binder_id)
        return false;
    if (left.projection_depth || right.projection_depth) {
        if (left.projection_depth != right.projection_depth)
            return false;
        for (uint8_t i = 0; i < left.projection_depth; i++)
            if (left.projection_path[i] != right.projection_path[i])
                return false;
        return true;
    }
    return left.projection_id == right.projection_id;
}

static inline bool qtt_place_overlaps(QttPlace left, QttPlace right) {
    if (left.root.module_id != right.root.module_id ||
        left.root.binder_id != right.root.binder_id)
        return false;
    bool left_root = left.projection_id == 0 &&
                     left.projection_depth == 0;
    bool right_root = right.projection_id == 0 &&
                      right.projection_depth == 0;
    if (left_root || right_root) return true;
    if (!left.projection_depth && !right.projection_depth)
        return left.projection_id == right.projection_id;
    /* A legacy flat projection has no prefix proof: overlap conservatively. */
    if (!left.projection_depth || !right.projection_depth)
        return true;
    uint8_t common = left.projection_depth < right.projection_depth
        ? left.projection_depth : right.projection_depth;
    for (uint8_t i = 0; i < common; i++)
        if (left.projection_path[i] != right.projection_path[i])
            return false;
    return true;
}

/* A reborrow may preserve or narrow authority, never widen it.  Legacy flat
 * projections have no prefix evidence, so only equality proves containment. */
static inline bool qtt_place_contains(QttPlace parent, QttPlace child) {
    if (!qtt_core_var_equal(parent.root, child.root)) return false;
    bool parent_root = parent.projection_id == 0 &&
                       parent.projection_depth == 0;
    if (parent_root) return true;
    if (!parent.projection_depth || !child.projection_depth)
        return qtt_place_equal(parent, child);
    if (parent.projection_depth > child.projection_depth) return false;
    for (uint8_t i = 0; i < parent.projection_depth; i++)
        if (parent.projection_path[i] != child.projection_path[i])
            return false;
    return true;
}
static inline bool qtt_place_effect_label(
    char *buffer, size_t capacity, const char *operation,
    QttPlace place) {
    if (!buffer || !capacity || !operation || !*operation ||
        !place.root.module_id || !place.root.binder_id)
        return false;
    int written = snprintf(
        buffer, capacity, "%s:%" PRIu64 ":%" PRIu64 ":%" PRIu64,
        operation, place.root.module_id, place.root.binder_id,
        place.projection_id);
    if (written <= 0 || (size_t)written >= capacity)
        return false;
    if (place.projection_depth <= 1)
        return true;
    size_t used = (size_t)written;
    for (uint8_t i = 1; i < place.projection_depth; i++) {
        int part = snprintf(
            buffer + used, capacity - used, ".%" PRIu32,
            place.projection_path[i]);
        if (part <= 0 || (size_t)part >= capacity - used)
            return false;
        used += (size_t)part;
    }
    return true;
}

#endif
