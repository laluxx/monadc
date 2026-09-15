#include "kind.h"

#include <stdlib.h>
#include <string.h>

struct QttKind {
    QttKindTag tag;
    uint32_t meta;
    QttKind *domain;
    QttKind *codomain;
    QttKind *next;
};

struct QttKindArena {
    QttKind *nodes;
    uint32_t next_meta;
};

struct QttKindSolver {
    QttKindArena *arena;
    QttKind **bindings;
    size_t capacity;
};

static QttKind *kind_new(QttKindArena *arena, QttKindTag tag) {
    if (!arena) return NULL;
    QttKind *kind = calloc(1, sizeof(*kind));
    if (!kind) return NULL;
    kind->tag = tag;
    kind->next = arena->nodes;
    arena->nodes = kind;
    return kind;
}

QttKindArena *qtt_kind_arena_new(void) { return calloc(1, sizeof(QttKindArena)); }

void qtt_kind_arena_free(QttKindArena *arena) {
    if (!arena) return;
    while (arena->nodes) {
        QttKind *next = arena->nodes->next;
        free(arena->nodes);
        arena->nodes = next;
    }
    free(arena);
}

QttKind *qtt_kind_type(QttKindArena *arena) {
    return kind_new(arena, QTT_KIND_TYPE);
}

QttKind *qtt_kind_effect_row(QttKindArena *arena) {
    return kind_new(arena, QTT_KIND_EFFECT_ROW);
}

QttKind *qtt_kind_arrow(
    QttKindArena *arena, QttKind *domain, QttKind *codomain) {
    if (!domain || !codomain) return NULL;
    QttKind *kind = kind_new(arena, QTT_KIND_ARROW);
    if (kind) {
        kind->domain = domain;
        kind->codomain = codomain;
    }
    return kind;
}

QttKind *qtt_kind_fresh(QttKindArena *arena) {
    QttKind *kind = kind_new(arena, QTT_KIND_META);
    if (kind) kind->meta = arena->next_meta++;
    return kind;
}

QttKindSolver *qtt_kind_solver_new(QttKindArena *arena) {
    if (!arena) return NULL;
    QttKindSolver *solver = calloc(1, sizeof(*solver));
    if (solver) solver->arena = arena;
    return solver;
}

void qtt_kind_solver_free(QttKindSolver *solver) {
    if (!solver) return;
    free(solver->bindings);
    free(solver);
}

static bool ensure_binding(QttKindSolver *solver, uint32_t id) {
    if (id < solver->capacity) return true;
    size_t capacity = solver->capacity ? solver->capacity : 8;
    while (capacity <= id) {
        if (capacity > SIZE_MAX / 2) return false;
        capacity *= 2;
    }
    QttKind **bindings = realloc(
        solver->bindings, capacity * sizeof(*bindings));
    if (!bindings) return false;
    memset(bindings + solver->capacity, 0,
           (capacity - solver->capacity) * sizeof(*bindings));
    solver->bindings = bindings;
    solver->capacity = capacity;
    return true;
}

static QttKind *resolve(QttKindSolver *solver, QttKind *kind) {
    while (solver && kind && kind->tag == QTT_KIND_META &&
           kind->meta < solver->capacity && solver->bindings[kind->meta])
        kind = solver->bindings[kind->meta];
    return kind;
}

static bool occurs(
    QttKindSolver *solver, uint32_t meta, QttKind *kind, unsigned depth) {
    kind = resolve(solver, kind);
    if (!kind || depth > 256) return true;
    if (kind->tag == QTT_KIND_META) return kind->meta == meta;
    return kind->tag == QTT_KIND_ARROW &&
        (occurs(solver, meta, kind->domain, depth + 1) ||
         occurs(solver, meta, kind->codomain, depth + 1));
}

static QttKindResult unify_in_place(
    QttKindSolver *solver, QttKind *left, QttKind *right) {
    if (!solver || !left || !right) return QTT_KIND_MISMATCH;
    left = resolve(solver, left);
    right = resolve(solver, right);
    if (left == right) return QTT_KIND_SOLVED;
    if (left->tag == QTT_KIND_META) {
        if (occurs(solver, left->meta, right, 0)) return QTT_KIND_OCCURS;
        if (!ensure_binding(solver, left->meta)) return QTT_KIND_OUT_OF_MEMORY;
        solver->bindings[left->meta] = right;
        return QTT_KIND_SOLVED;
    }
    if (right->tag == QTT_KIND_META)
        return unify_in_place(solver, right, left);
    if (left->tag != right->tag) return QTT_KIND_MISMATCH;
    if (left->tag != QTT_KIND_ARROW) return QTT_KIND_SOLVED;
    QttKindResult domain = unify_in_place(
        solver, left->domain, right->domain);
    return domain == QTT_KIND_SOLVED
        ? unify_in_place(solver, left->codomain, right->codomain)
        : domain;
}

QttKindResult qtt_kind_unify(
    QttKindSolver *solver, QttKind *left, QttKind *right) {
    if (!solver) return QTT_KIND_MISMATCH;
    size_t old_capacity = solver->capacity;
    QttKind **snapshot = old_capacity
        ? malloc(old_capacity * sizeof(*snapshot)) : NULL;
    if (old_capacity && !snapshot) return QTT_KIND_OUT_OF_MEMORY;
    if (old_capacity)
        memcpy(snapshot, solver->bindings,
               old_capacity * sizeof(*snapshot));
    QttKindResult result = unify_in_place(solver, left, right);
    if (result != QTT_KIND_SOLVED) {
        if (old_capacity)
            memcpy(solver->bindings, snapshot,
                   old_capacity * sizeof(*snapshot));
        if (solver->capacity > old_capacity)
            memset(solver->bindings + old_capacity, 0,
                   (solver->capacity - old_capacity) *
                       sizeof(*solver->bindings));
    }
    free(snapshot);
    return result;
}

bool qtt_kind_equal(
    QttKindSolver *solver, QttKind *left, QttKind *right) {
    left = resolve(solver, left);
    right = resolve(solver, right);
    if (!left || !right || left->tag != right->tag) return false;
    if (left->tag == QTT_KIND_META) return left->meta == right->meta;
    return left->tag != QTT_KIND_ARROW ||
        (qtt_kind_equal(solver, left->domain, right->domain) &&
         qtt_kind_equal(solver, left->codomain, right->codomain));
}

QttKindTag qtt_kind_tag(const QttKind *kind) {
    return kind ? kind->tag : QTT_KIND_META;
}

const char *qtt_kind_name(const QttKind *kind) {
    if (!kind) return "invalid";
    switch (kind->tag) {
    case QTT_KIND_TYPE: return "Type";
    case QTT_KIND_EFFECT_ROW: return "EffectRow";
    case QTT_KIND_ARROW: return "Arrow";
    case QTT_KIND_META: return "Meta";
    }
    return "invalid";
}

static size_t serialized_size(
    QttKindSolver *solver, QttKind *kind, unsigned depth) {
    kind = resolve(solver, kind);
    if (!kind || depth > 256 || kind->tag == QTT_KIND_META) return 0;
    if (kind->tag != QTT_KIND_ARROW) return 1;
    size_t left = serialized_size(solver, kind->domain, depth + 1);
    size_t right = serialized_size(solver, kind->codomain, depth + 1);
    return left && right ? left + right + 4 : 0;
}

static char *serialize_into(
    QttKindSolver *solver, QttKind *kind, char *out) {
    kind = resolve(solver, kind);
    if (kind->tag == QTT_KIND_TYPE) { *out++ = '*'; return out; }
    if (kind->tag == QTT_KIND_EFFECT_ROW) { *out++ = 'E'; return out; }
    *out++ = '(';
    out = serialize_into(solver, kind->domain, out);
    *out++ = '-'; *out++ = '>';
    out = serialize_into(solver, kind->codomain, out);
    *out++ = ')';
    return out;
}

char *qtt_kind_serialize(QttKindSolver *solver, QttKind *kind) {
    static const char header[] = "monad-kind-v1|";
    size_t body = serialized_size(solver, kind, 0);
    if (!body) return NULL;
    char *text = malloc(sizeof(header) + body);
    if (!text) return NULL;
    memcpy(text, header, sizeof(header) - 1);
    char *end = serialize_into(solver, kind, text + sizeof(header) - 1);
    *end = '\0';
    return text;
}

static QttKind *parse_kind(
    QttKindArena *arena, const char **cursor, unsigned depth) {
    if (!arena || !cursor || !*cursor || depth > 256) return NULL;
    if (**cursor == '*') { (*cursor)++; return qtt_kind_type(arena); }
    if (**cursor == 'E') { (*cursor)++; return qtt_kind_effect_row(arena); }
    if (*(*cursor)++ != '(') return NULL;
    QttKind *domain = parse_kind(arena, cursor, depth + 1);
    if (!domain || (*cursor)[0] != '-' || (*cursor)[1] != '>') return NULL;
    *cursor += 2;
    QttKind *codomain = parse_kind(arena, cursor, depth + 1);
    if (!codomain || *(*cursor)++ != ')') return NULL;
    return qtt_kind_arrow(arena, domain, codomain);
}

QttKind *qtt_kind_deserialize(QttKindArena *arena, const char *text) {
    static const char header[] = "monad-kind-v1|";
    if (!arena || !text || strncmp(text, header, sizeof(header) - 1))
        return NULL;
    const char *cursor = text + sizeof(header) - 1;
    QttKind *kind = parse_kind(arena, &cursor, 0);
    return kind && !*cursor ? kind : NULL;
}

uint64_t qtt_kind_fingerprint(QttKindSolver *solver, QttKind *kind) {
    char *text = qtt_kind_serialize(solver, kind);
    if (!text) return 0;
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        hash ^= *p;
        hash *= UINT64_C(1099511628211);
    }
    free(text);
    return hash ? hash : 1;
}
