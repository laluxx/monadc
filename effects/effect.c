#include "effect.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    EFFECT_EMPTY,
    EFFECT_VARIABLE,
    EFFECT_EXTEND,
} EffectRowKind;

struct QttEffectRow {
    EffectRowKind kind;
    uint64_t variable;
    char *label;
    bool has_atom;
    QttEffectAtom atom;
    QttEffectRow *tail;
};

struct QttEffectArena {
    QttEffectRow **nodes;
    size_t count;
    size_t capacity;
    uint64_t next_variable;
    size_t references;
};

struct QttEffectSolver {
    QttEffectArena *arena;
    QttEffectRow **bindings;
    size_t binding_count;
};

struct QttEffectScheme {
    QttEffectArena *arena;
    QttEffectRow *row;
    uint64_t quantified;
    size_t quantified_count;
    size_t references;
    char *portable_evidence;
    size_t evidence_count;
    uint64_t evidence_fingerprint;
    QttEffectEvidenceStatus evidence_status;
    uint32_t coverage_gaps;
    size_t *callable_parameter_indices;
    size_t *callable_parameter_arities;
    size_t callable_parameter_count;
};

typedef struct {
    const QttEffectRow **items;
    size_t count;
    size_t capacity;
    uint64_t tail;
    bool valid;
} EffectFlat;

static EffectFlat flatten(
    const QttEffectSolver *solver, const QttEffectRow *row);
static void flat_free(EffectFlat *flat);

typedef struct EffectDeclarationEntry {
    QttEffectDeclaration declaration;
    struct EffectDeclarationEntry *next;
} EffectDeclarationEntry;

static EffectDeclarationEntry *effect_declarations;

typedef struct EffectHandlerProfileEntry {
    QttEffectHandlerProfile profile;
    struct EffectHandlerProfileEntry *next;
} EffectHandlerProfileEntry;

static EffectHandlerProfileEntry *effect_handler_profiles;

typedef struct EffectTraitImplication {
    char *premise;
    char *consequence;
    struct EffectTraitImplication *next;
} EffectTraitImplication;

static EffectTraitImplication *effect_trait_implications;

static char *effect_copy_text(const char *text) {
    if (!text) return NULL;
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
}

static int effect_trait_compare(const void *left, const void *right) {
    return strcmp(*(const char *const *)left, *(const char *const *)right);
}

char *qtt_effect_traits_normalize(const char *traits) {
    if (!traits || !*traits) return NULL;
    char *storage = effect_copy_text(traits);
    if (!storage) return NULL;
    size_t count = 1;
    for (const char *p = traits; *p; p++) if (*p == ',') count++;
    char **items = calloc(count, sizeof(*items));
    if (!items) { free(storage); return NULL; }
    size_t used = 0;
    for (char *token = storage; token;) {
        char *next = strchr(token, ',');
        if (next) *next++ = '\0';
        if (!*token) { free(items); free(storage); return NULL; }
        for (const unsigned char *p = (unsigned char *)token; *p; p++)
            if (!( (*p >= 'a' && *p <= 'z') ||
                   (*p >= 'A' && *p <= 'Z') ||
                   (*p >= '0' && *p <= '9') || *p == '.' ||
                   *p == '_' || *p == '-')) {
                free(items); free(storage); return NULL;
            }
        items[used++] = token;
        token = next;
    }
    qsort(items, used, sizeof(*items), effect_trait_compare);
    size_t size = 1, unique = 0;
    for (size_t i = 0; i < used; i++)
        if (!i || strcmp(items[i], items[i - 1])) {
            size += strlen(items[i]) + (unique ? 1 : 0);
            items[unique++] = items[i];
        }
    char *normal = malloc(size);
    if (normal) {
        size_t at = 0;
        for (size_t i = 0; i < unique; i++) {
            if (i) normal[at++] = ',';
            size_t length = strlen(items[i]);
            memcpy(normal + at, items[i], length);
            at += length;
        }
        normal[at] = '\0';
    }
    free(items); free(storage);
    return normal;
}

static bool effect_traits_equal(const char *left, const char *right) {
    char *a = qtt_effect_traits_normalize(left);
    char *b = qtt_effect_traits_normalize(right);
    bool equal = (!a && !b) || (a && b && strcmp(a, b) == 0);
    free(a); free(b);
    return equal;
}

static bool effect_single_trait(const char *trait) {
    char *normal = qtt_effect_traits_normalize(trait);
    bool valid = normal && strcmp(normal, trait) == 0 && !strchr(trait, ',');
    free(normal);
    return valid;
}

bool qtt_effect_trait_implication_register(
    const char *premise, const char *consequence) {
    if (!effect_single_trait(premise) || !effect_single_trait(consequence))
        return false;
    EffectTraitImplication **at = &effect_trait_implications;
    while (*at) {
        int order = strcmp((*at)->premise, premise);
        if (!order) order = strcmp((*at)->consequence, consequence);
        if (!order) return true;
        if (order > 0) break;
        at = &(*at)->next;
    }
    EffectTraitImplication *edge = calloc(1, sizeof(*edge));
    if (!edge) return false;
    edge->premise = effect_copy_text(premise);
    edge->consequence = effect_copy_text(consequence);
    if (!edge->premise || !edge->consequence) {
        free(edge->premise); free(edge->consequence); free(edge); return false;
    }
    edge->next = *at; *at = edge;
    return true;
}

void qtt_effect_trait_implications_clear(void) {
    while (effect_trait_implications) {
        EffectTraitImplication *next = effect_trait_implications->next;
        free(effect_trait_implications->premise);
        free(effect_trait_implications->consequence);
        free(effect_trait_implications);
        effect_trait_implications = next;
    }
}

static bool effect_trait_path(
    const char *premise, const char *consequence,
    const char **path, size_t *path_count) {
    if (!effect_single_trait(premise) || !effect_single_trait(consequence))
        return false;
    const char *queue[512], *parent[512];
    size_t head = 0, count = 1;
    queue[0] = premise; parent[0] = NULL;
    while (head < count) {
        const char *current = queue[head];
        if (strcmp(current, consequence) == 0) {
            size_t length = 0, cursor = head;
            while (true) {
                path[length++] = queue[cursor];
                if (!parent[cursor]) break;
                const char *wanted = parent[cursor];
                for (cursor = 0; cursor < count; cursor++)
                    if (strcmp(queue[cursor], wanted) == 0) break;
            }
            for (size_t i = 0; i < length / 2; i++) {
                const char *swap = path[i]; path[i] = path[length-i-1];
                path[length-i-1] = swap;
            }
            *path_count = length; return true;
        }
        for (EffectTraitImplication *edge = effect_trait_implications;
             edge; edge = edge->next) {
            if (strcmp(edge->premise, current)) continue;
            bool seen = false;
            for (size_t i = 0; i < count; i++)
                if (strcmp(queue[i], edge->consequence) == 0) seen = true;
            if (!seen && count < 512) {
                queue[count] = edge->consequence;
                parent[count] = current;
                count++;
            }
        }
        head++;
    }
    return false;
}

bool qtt_effect_trait_implies(
    const char *premise, const char *consequence) {
    const char *path[512]; size_t count = 0;
    return effect_trait_path(premise, consequence, path, &count);
}

char *qtt_effect_trait_implication_witness(
    const char *premise, const char *consequence) {
    const char *path[512]; size_t count = 0;
    if (!effect_trait_path(premise, consequence, path, &count)) return NULL;
    size_t size = 1;
    for (size_t i = 0; i < count; i++) size += strlen(path[i]) + (i ? 4 : 0);
    char *text = malloc(size); if (!text) return NULL;
    text[0] = '\0';
    for (size_t i = 0; i < count; i++) {
        if (i) strcat(text, " -> ");
        strcat(text, path[i]);
    }
    return text;
}

static uint64_t effect_declaration_id(const char *name) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        hash ^= *p;
        hash *= UINT64_C(1099511628211);
    }
    return hash ? hash : 1;
}

const QttEffectDeclaration *qtt_effect_declaration_lookup(const char *name) {
    if (!name) return NULL;
    for (EffectDeclarationEntry *entry = effect_declarations;
         entry; entry = entry->next)
        if (strcmp(entry->declaration.name, name) == 0)
            return &entry->declaration;
    return NULL;
}

const QttEffectDeclaration *qtt_effect_declaration_resolve(
    const char *reference, bool *ambiguous) {
    if (ambiguous) *ambiguous = false;
    if (!reference || !*reference) return NULL;
    const QttEffectDeclaration *exact =
        qtt_effect_declaration_lookup(reference);
    if (exact) return exact;
    const char *separator = strrchr(reference, '.');
    if (!separator || separator == reference || !separator[1]) return NULL;
    size_t trait_size = (size_t)(separator - reference);
    char *trait = malloc(trait_size + 1);
    if (!trait) return NULL;
    memcpy(trait, reference, trait_size);
    trait[trait_size] = '\0';
    const QttEffectDeclaration *resolved = NULL;
    for (EffectDeclarationEntry *entry = effect_declarations;
         entry; entry = entry->next) {
        const QttEffectDeclaration *candidate = &entry->declaration;
        if (!candidate->operation ||
                strcmp(candidate->operation, separator + 1) != 0 ||
                !qtt_effect_declaration_has_trait(candidate, trait))
            continue;
        if (resolved) {
            if (ambiguous) *ambiguous = true;
            resolved = NULL;
            break;
        }
        resolved = candidate;
    }
    free(trait);
    return resolved;
}

static bool effect_declaration_equal(
    const QttEffectDeclaration *left,
    const QttEffectDeclaration *right) {
    bool operation_equal = left->operation == right->operation ||
        (left->operation && right->operation &&
         strcmp(left->operation, right->operation) == 0);
    bool traits_equal = effect_traits_equal(left->traits, right->traits);
    bool payload_equal = left->payload_type == right->payload_type ||
        (left->payload_type && right->payload_type &&
         strcmp(left->payload_type, right->payload_type) == 0);
    bool result_equal = left->result_type == right->result_type ||
        (left->result_type && right->result_type &&
         strcmp(left->result_type, right->result_type) == 0);
    bool scheme_equal = left->operation_scheme == right->operation_scheme ||
        (left->operation_scheme && right->operation_scheme &&
         strcmp(left->operation_scheme, right->operation_scheme) == 0);
    if (left->operation_scheme && right->operation_scheme && scheme_equal) {
        /* Payload/result names are presentation aliases once the canonical
         * alpha-normal scheme is present. */
        payload_equal = true;
        result_equal = true;
    }
    return left->kind == right->kind && traits_equal &&
        left->constructor_id == right->constructor_id &&
        operation_equal &&
        payload_equal && result_equal && scheme_equal &&
        left->resumption.is_omega == right->resumption.is_omega &&
        left->resumption.finite == right->resumption.finite &&
        left->scoped == right->scoped;
}

bool qtt_effect_declaration_has_trait(
    const QttEffectDeclaration *declaration, const char *trait) {
    if (!declaration || !declaration->traits || !trait || !*trait) return false;
    size_t wanted = strlen(trait);
    for (const char *at = declaration->traits; *at;) {
        const char *end = strchr(at, ',');
        size_t length = end ? (size_t)(end - at) : strlen(at);
        if (length == wanted && strncmp(at, trait, wanted) == 0) return true;
        if (!end) break;
        at = end + 1;
    }
    return false;
}

bool qtt_effect_trait_is_declared(const char *trait) {
    if (!trait || !*trait || strchr(trait, ',')) return false;
    for (EffectDeclarationEntry *entry = effect_declarations;
         entry; entry = entry->next)
        if (qtt_effect_declaration_has_trait(&entry->declaration, trait))
            return true;
    for (EffectTraitImplication *edge = effect_trait_implications;
         edge; edge = edge->next)
        if (!strcmp(edge->premise, trait) ||
            !strcmp(edge->consequence, trait))
            return true;
    return false;
}

bool qtt_effect_declaration_register(
    const QttEffectDeclaration *declaration) {
    if (!declaration || !declaration->name || !declaration->name[0] ||
        declaration->kind > QTT_EFFECT_CONTROL ||
        (!!declaration->payload_type != !!declaration->result_type))
        return false;
    QttEffectDeclaration normalized = *declaration;
    char *normal_traits = qtt_effect_traits_normalize(declaration->traits);
    if (declaration->traits && !normal_traits) return false;
    normalized.traits = normal_traits;
    if (!normalized.constructor_id)
        normalized.constructor_id = effect_declaration_id(normalized.name);
    const QttEffectDeclaration *existing =
        qtt_effect_declaration_lookup(normalized.name);
    if (existing) {
        bool equal = effect_declaration_equal(existing, &normalized);
        free(normal_traits);
        return equal;
    }
    EffectDeclarationEntry *entry = calloc(1, sizeof(*entry));
    if (!entry) return false;
    entry->declaration = normalized;
    entry->declaration.name = effect_copy_text(normalized.name);
    entry->declaration.traits = effect_copy_text(normalized.traits);
    entry->declaration.operation = effect_copy_text(normalized.operation);
    entry->declaration.payload_type = effect_copy_text(normalized.payload_type);
    entry->declaration.result_type = effect_copy_text(normalized.result_type);
    entry->declaration.operation_scheme =
        effect_copy_text(normalized.operation_scheme);
    free(normal_traits);
    if (!entry->declaration.name ||
        (normalized.traits && !entry->declaration.traits) ||
        (normalized.operation && !entry->declaration.operation) ||
        (normalized.payload_type && !entry->declaration.payload_type) ||
        (normalized.result_type && !entry->declaration.result_type) ||
        (normalized.operation_scheme &&
         !entry->declaration.operation_scheme)) {
        free((char *)entry->declaration.name);
        free((char *)entry->declaration.traits);
        free((char *)entry->declaration.operation);
        free((char *)entry->declaration.payload_type);
        free((char *)entry->declaration.result_type);
        free((char *)entry->declaration.operation_scheme);
        free(entry);
        return false;
    }
    entry->next = effect_declarations;
    effect_declarations = entry;
    return true;
}

void qtt_effect_declarations_clear(void) {
    qtt_effect_handler_profiles_clear();
    while (effect_declarations) {
        EffectDeclarationEntry *next = effect_declarations->next;
        free((char *)effect_declarations->declaration.name);
        free((char *)effect_declarations->declaration.traits);
        free((char *)effect_declarations->declaration.operation);
        free((char *)effect_declarations->declaration.payload_type);
        free((char *)effect_declarations->declaration.result_type);
        free((char *)effect_declarations->declaration.operation_scheme);
        free(effect_declarations);
        effect_declarations = next;
    }
}

const QttEffectHandlerProfile *qtt_effect_handler_profile_lookup(
    const char *name) {
    if (!name) return NULL;
    for (EffectHandlerProfileEntry *entry = effect_handler_profiles;
         entry; entry = entry->next)
        if (strcmp(entry->profile.name, name) == 0)
            return &entry->profile;
    return NULL;
}

bool qtt_effect_handler_profile_register(
    const QttEffectHandlerProfile *profile) {
    if (!profile || !profile->name || !profile->name[0] ||
        !profile->effect_name || !profile->effect_name[0])
        return false;
    const QttEffectDeclaration *effect =
        qtt_effect_declaration_lookup(profile->effect_name);
    if (!effect ||
        !qtt_quantity_leq(profile->continuation_usage, effect->resumption))
        return false;
    const QttEffectHandlerProfile *existing =
        qtt_effect_handler_profile_lookup(profile->name);
    if (existing)
        return strcmp(existing->effect_name, profile->effect_name) == 0 &&
               qtt_quantity_equal(
                   existing->continuation_usage,
                   profile->continuation_usage) &&
               existing->deep == profile->deep;
    EffectHandlerProfileEntry *entry = calloc(1, sizeof(*entry));
    if (!entry) return false;
    entry->profile = *profile;
    entry->profile.name = effect_copy_text(profile->name);
    entry->profile.effect_name = effect_copy_text(profile->effect_name);
    if (!entry->profile.name || !entry->profile.effect_name) {
        free((char *)entry->profile.name);
        free((char *)entry->profile.effect_name);
        free(entry);
        return false;
    }
    entry->next = effect_handler_profiles;
    effect_handler_profiles = entry;
    return true;
}

void qtt_effect_handler_profiles_clear(void) {
    while (effect_handler_profiles) {
        EffectHandlerProfileEntry *next = effect_handler_profiles->next;
        free((char *)effect_handler_profiles->profile.name);
        free((char *)effect_handler_profiles->profile.effect_name);
        free(effect_handler_profiles);
        effect_handler_profiles = next;
    }
}

static QttEffectRow *arena_node(QttEffectArena *arena) {
    if (!arena) return NULL;
    if (arena->count == arena->capacity) {
        size_t next = arena->capacity ? arena->capacity * 2 : 32;
        QttEffectRow **grown =
            realloc(arena->nodes, next * sizeof(*grown));
        if (!grown) return NULL;
        arena->nodes = grown;
        arena->capacity = next;
    }
    QttEffectRow *row = calloc(1, sizeof(*row));
    if (row) arena->nodes[arena->count++] = row;
    return row;
}

QttEffectArena *qtt_effect_arena_new(void) {
    QttEffectArena *arena = calloc(1, sizeof(*arena));
    if (arena) {
        arena->next_variable = 1;
        arena->references = 1;
    }
    return arena;
}

void qtt_effect_arena_free(QttEffectArena *arena) {
    if (!arena || --arena->references) return;
    for (size_t i = 0; i < arena->count; i++) {
        free(arena->nodes[i]->label);
        if (arena->nodes[i]->has_atom) {
            free((char *)arena->nodes[i]->atom.name);
            free((char *)arena->nodes[i]->atom.operation);
            free((char *)arena->nodes[i]->atom.traits);
        }
        free(arena->nodes[i]);
    }
    free(arena->nodes);
    free(arena);
}

QttEffectRow *qtt_effect_empty(QttEffectArena *arena) {
    QttEffectRow *row = arena_node(arena);
    if (row) row->kind = EFFECT_EMPTY;
    return row;
}

QttEffectRow *qtt_effect_fresh(QttEffectArena *arena) {
    QttEffectRow *row = arena_node(arena);
    if (row) {
        row->kind = EFFECT_VARIABLE;
        row->variable = arena->next_variable++;
    }
    return row;
}

QttEffectRow *qtt_effect_extend(
    QttEffectArena *arena, const char *label, QttEffectRow *tail) {
    if (!arena || !label || !*label || !tail) return NULL;
    QttEffectRow *row = arena_node(arena);
    if (!row) return NULL;
    size_t length = strlen(label) + 1;
    row->label = malloc(length);
    if (!row->label) return NULL;
    memcpy(row->label, label, length);
    row->kind = EFFECT_EXTEND;
    row->tail = tail;
    return row;
}

static bool nullable_string_equal(const char *left, const char *right) {
    return left == right || (left && right && strcmp(left, right) == 0);
}

static char *copy_string(const char *text) {
    if (!text) return NULL;
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
}

bool qtt_effect_atom_equal(
    const QttEffectAtom *left, const QttEffectAtom *right) {
    return left && right && left->kind == right->kind &&
        effect_traits_equal(left->traits, right->traits) &&
        left->constructor_id == right->constructor_id &&
        left->type_id == right->type_id &&
        left->capability_id == right->capability_id &&
        left->scoped == right->scoped &&
        qtt_quantity_equal(left->resumption, right->resumption) &&
        nullable_string_equal(left->name, right->name) &&
        nullable_string_equal(left->operation, right->operation);
}

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t size) {
    const unsigned char *bytes = data;
    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t hash_string(uint64_t hash, const char *text) {
    unsigned char present = text != NULL;
    hash = hash_bytes(hash, &present, 1);
    return text ? hash_bytes(hash, text, strlen(text) + 1) : hash;
}

static uint64_t hash_u64(uint64_t hash, uint64_t value) {
    unsigned char bytes[8];
    for (size_t i = 0; i < 8; i++)
        bytes[i] = (unsigned char)(value >> (i * 8));
    return hash_bytes(hash, bytes, sizeof(bytes));
}

uint64_t qtt_effect_atom_fingerprint(const QttEffectAtom *atom) {
    if (!atom) return 0;
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = hash_u64(hash, (uint64_t)atom->kind);
    char *normal_traits = qtt_effect_traits_normalize(atom->traits);
    if (normal_traits) hash = hash_string(hash, normal_traits);
    free(normal_traits);
    hash = hash_u64(hash, atom->constructor_id);
    hash = hash_u64(hash, atom->type_id);
    hash = hash_u64(hash, atom->capability_id);
    hash = hash_u64(hash, atom->resumption.finite);
    unsigned char omega = atom->resumption.is_omega;
    unsigned char scoped = atom->scoped;
    hash = hash_bytes(hash, &omega, 1);
    hash = hash_bytes(hash, &scoped, 1);
    hash = hash_string(hash, atom->name);
    return hash_string(hash, atom->operation);
}

bool qtt_effect_atom_has_trait(
    const QttEffectAtom *atom, const char *trait) {
    if (!atom || !trait) return false;
    QttEffectDeclaration view = {.traits = atom->traits};
    if (qtt_effect_declaration_has_trait(&view, trait)) return true;
    const QttEffectDeclaration *declaration =
        qtt_effect_declaration_lookup(atom->name);
    return qtt_effect_declaration_has_trait(declaration, trait);
}

bool qtt_effect_atom_satisfies_trait(
    const QttEffectAtom *atom, const char *trait) {
    if (!atom || !trait) return false;
    const char *traits = atom->traits;
    if (!traits) {
        const QttEffectDeclaration *declaration =
            qtt_effect_declaration_lookup(atom->name);
        traits = declaration ? declaration->traits : NULL;
    }
    char *normal = qtt_effect_traits_normalize(traits);
    if (!normal) return false;
    bool satisfied = false;
    for (char *token = normal; token && !satisfied;) {
        char *next = strchr(token, ',');
        if (next) *next++ = '\0';
        satisfied = qtt_effect_trait_implies(token, trait);
        token = next;
    }
    free(normal);
    return satisfied;
}

QttEffectRelation qtt_effect_row_satisfies_trait(
    const QttEffectSolver *solver, const QttEffectRow *row,
    const char *trait, char **witness) {
    if (witness) *witness = NULL;
    if (!row || !effect_single_trait(trait)) return QTT_EFFECT_RELATION_REFUTED;
    EffectFlat flat = flatten(solver, row);
    if (!flat.valid) { flat_free(&flat); return QTT_EFFECT_RELATION_UNKNOWN; }
    char *best = NULL;
    for (size_t i = 0; i < flat.count; i++) {
        if (!flat.items[i]->has_atom) continue;
        const QttEffectAtom *atom = &flat.items[i]->atom;
        char *normal = qtt_effect_traits_normalize(atom->traits);
        if (!normal && atom->name) {
            const QttEffectDeclaration *declaration =
                qtt_effect_declaration_lookup(atom->name);
            normal = qtt_effect_traits_normalize(
                declaration ? declaration->traits : NULL);
        }
        for (char *token = normal; token;) {
            char *next = strchr(token, ','); if (next) *next++ = '\0';
            char *candidate = qtt_effect_trait_implication_witness(token, trait);
            if (candidate && (!best || strlen(candidate) < strlen(best) ||
                    (strlen(candidate) == strlen(best) &&
                     strcmp(candidate, best) < 0))) {
                free(best); best = candidate; candidate = NULL;
            }
            free(candidate); token = next;
        }
        free(normal);
    }
    QttEffectRelation result = best ? QTT_EFFECT_RELATION_PROVED
        : flat.tail ? QTT_EFFECT_RELATION_UNKNOWN
        : QTT_EFFECT_RELATION_REFUTED;
    flat_free(&flat);
    if (witness) *witness = best; else free(best);
    return result;
}

static const char *effect_kind_name(QttEffectKind kind) {
    switch (kind) {
    case QTT_EFFECT_IO: return "io";
    case QTT_EFFECT_EXCEPTION: return "exception";
    case QTT_EFFECT_STATE: return "state";
    case QTT_EFFECT_READ: return "read";
    case QTT_EFFECT_WRITE: return "write";
    case QTT_EFFECT_ALLOCATE: return "alloc";
    case QTT_EFFECT_FOREIGN: return "ffi";
    case QTT_EFFECT_DIVERGE: return "div";
    case QTT_EFFECT_ASYNC: return "async";
    case QTT_EFFECT_CONTROL: return "control";
    case QTT_EFFECT_CUSTOM: return "effect";
    }
    return "effect";
}

static char *format_atom(const QttEffectAtom *atom) {
    if (!atom) return NULL;
    const char *name = atom->name ? atom->name : effect_kind_name(atom->kind);
    const char *operation = atom->operation ? atom->operation : "perform";
    char resume[32];
    if (atom->resumption.is_omega) strcpy(resume, "omega");
    else snprintf(resume, sizeof(resume), "%llu",
                  (unsigned long long)atom->resumption.finite);
    size_t size = strlen(name) + strlen(operation) + strlen(resume) + 96;
    char *text = malloc(size);
    if (!text) return NULL;
    snprintf(text, size, "%s#%llu:%s[type=#%016llx,resume=%s]%s",
             name, (unsigned long long)atom->capability_id, operation,
             (unsigned long long)atom->type_id, resume,
             atom->scoped ? "[scoped]" : "");
    return text;
}

QttEffectRow *qtt_effect_extend_atom(
    QttEffectArena *arena, const QttEffectAtom *atom, QttEffectRow *tail) {
    if (!arena || !atom || !tail) return NULL;
    char *formatted = format_atom(atom);
    if (!formatted) return NULL;
    QttEffectRow *row = qtt_effect_extend(arena, formatted, tail);
    free(formatted);
    if (!row) return NULL;
    row->atom = *atom;
    row->has_atom = true;
    row->atom.name = copy_string(atom->name);
    row->atom.operation = copy_string(atom->operation);
    row->atom.traits = qtt_effect_traits_normalize(atom->traits);
    if ((atom->name && !row->atom.name) ||
        (atom->operation && !row->atom.operation) ||
        (atom->traits && !row->atom.traits))
        return NULL;
    return row;
}

QttEffectRow *qtt_effect_extend_declared(
    QttEffectArena *arena, const char *name,
    uint64_t capability_id, uint64_t type_id, QttEffectRow *tail) {
    const QttEffectDeclaration *declaration =
        qtt_effect_declaration_lookup(name);
    if (!declaration) return NULL;
    QttEffectAtom atom = {
        .traits = declaration->traits,
        .kind = declaration->kind,
        .constructor_id = declaration->constructor_id,
        .type_id = type_id,
        .capability_id = capability_id,
        .name = declaration->name,
        .operation = declaration->operation,
        .resumption = declaration->resumption,
        .scoped = declaration->scoped,
    };
    return qtt_effect_extend_atom(arena, &atom, tail);
}

static QttEffectRow *effect_place(
    QttEffectArena *arena, const char *operation,
    QttPlace place, QttEffectRow *tail) {
    char label[96];
    return qtt_place_effect_label(
               label, sizeof(label), operation, place)
        ? qtt_effect_extend(arena, label, tail) : NULL;
}

QttEffectRow *qtt_effect_read_place(
    QttEffectArena *arena, QttPlace place, QttEffectRow *tail) {
    return effect_place(arena, "read", place, tail);
}

QttEffectRow *qtt_effect_write_place(
    QttEffectArena *arena, QttPlace place, QttEffectRow *tail) {
    return effect_place(arena, "write", place, tail);
}

QttEffectSolver *qtt_effect_solver_new(QttEffectArena *arena) {
    if (!arena) return NULL;
    QttEffectSolver *solver = calloc(1, sizeof(*solver));
    if (solver) solver->arena = arena;
    return solver;
}

void qtt_effect_solver_free(QttEffectSolver *solver) {
    if (!solver) return;
    free(solver->bindings);
    free(solver);
}

static QttEffectRow *binding(
    const QttEffectSolver *solver, uint64_t variable) {
    return solver && variable < solver->binding_count
        ? solver->bindings[variable] : NULL;
}

static bool flat_push(EffectFlat *flat, const QttEffectRow *item) {
    if (flat->count == flat->capacity) {
        size_t next = flat->capacity ? flat->capacity * 2 : 8;
        const QttEffectRow **grown =
            realloc(flat->items, next * sizeof(*grown));
        if (!grown) return false;
        flat->items = grown;
        flat->capacity = next;
    }
    flat->items[flat->count++] = item;
    return true;
}

static void flatten_into(
    const QttEffectSolver *solver, const QttEffectRow *row,
    EffectFlat *flat, size_t depth) {
    if (!row || depth > 4096) {
        flat->valid = false;
        return;
    }
    if (row->kind == EFFECT_EMPTY) return;
    if (row->kind == EFFECT_EXTEND) {
        if (!flat_push(flat, row)) {
            flat->valid = false;
            return;
        }
        flatten_into(solver, row->tail, flat, depth + 1);
        return;
    }
    QttEffectRow *bound = binding(solver, row->variable);
    if (bound)
        flatten_into(solver, bound, flat, depth + 1);
    else if (flat->tail && flat->tail != row->variable)
        flat->valid = false;
    else
        flat->tail = row->variable;
}

static int compare_items(const void *left, const void *right) {
    const QttEffectRow *const *a = left;
    const QttEffectRow *const *b = right;
    if ((*a)->has_atom && (*b)->has_atom) {
        const QttEffectAtom *x = &(*a)->atom;
        const QttEffectAtom *y = &(*b)->atom;
#define COMPARE_FIELD(field) \
        do { if (x->field < y->field) return -1; \
             if (x->field > y->field) return 1; } while (0)
        COMPARE_FIELD(kind);
        COMPARE_FIELD(constructor_id);
        COMPARE_FIELD(type_id);
        COMPARE_FIELD(capability_id);
        COMPARE_FIELD(scoped);
        COMPARE_FIELD(resumption.is_omega);
        COMPARE_FIELD(resumption.finite);
#undef COMPARE_FIELD
        const char *xn = x->name ? x->name : "";
        const char *yn = y->name ? y->name : "";
        int order = strcmp(xn, yn);
        if (order) return order;
        const char *xt = x->traits ? x->traits : "";
        const char *yt = y->traits ? y->traits : "";
        order = strcmp(xt, yt);
        if (order) return order;
        const char *xo = x->operation ? x->operation : "";
        const char *yo = y->operation ? y->operation : "";
        return strcmp(xo, yo);
    }
    return strcmp((*a)->label, (*b)->label);
}

static EffectFlat flatten(
    const QttEffectSolver *solver, const QttEffectRow *row) {
    EffectFlat flat = {.valid = true};
    flatten_into(solver, row, &flat, 0);
    if (flat.valid && flat.count)
        qsort(flat.items, flat.count, sizeof(*flat.items), compare_items);
    return flat;
}

static void flat_free(EffectFlat *flat) {
    free(flat->items);
    memset(flat, 0, sizeof(*flat));
}

static bool solver_capacity(QttEffectSolver *solver, uint64_t variable) {
    if (variable < solver->binding_count) return true;
    size_t next = solver->binding_count ? solver->binding_count : 8;
    while (next <= variable) next *= 2;
    QttEffectRow **grown =
        realloc(solver->bindings, next * sizeof(*grown));
    if (!grown) return false;
    memset(grown + solver->binding_count, 0,
           (next - solver->binding_count) * sizeof(*grown));
    solver->bindings = grown;
    solver->binding_count = next;
    return true;
}

static bool occurs(
    const QttEffectSolver *solver, uint64_t variable,
    const QttEffectRow *row, size_t depth) {
    if (!row || depth > 4096) return true;
    if (row->kind == EFFECT_EMPTY) return false;
    if (row->kind == EFFECT_EXTEND)
        return occurs(solver, variable, row->tail, depth + 1);
    if (row->variable == variable) return true;
    QttEffectRow *bound = binding(solver, row->variable);
    return bound && occurs(solver, variable, bound, depth + 1);
}

static QttEffectUnifyResult bind_variable(
    QttEffectSolver *solver, uint64_t variable, QttEffectRow *row) {
    if (row->kind == EFFECT_VARIABLE && row->variable == variable)
        return QTT_EFFECT_UNIFY_OK;
    if (occurs(solver, variable, row, 0))
        return QTT_EFFECT_UNIFY_OCCURS;
    if (!solver_capacity(solver, variable))
        return QTT_EFFECT_UNIFY_OUT_OF_MEMORY;
    solver->bindings[variable] = row;
    return QTT_EFFECT_UNIFY_OK;
}

static QttEffectRow *row_from_items(
    QttEffectArena *arena, const QttEffectRow *const *items,
    size_t count, QttEffectRow *tail) {
    QttEffectRow *row = tail ? tail : qtt_effect_empty(arena);
    if (!row) return NULL;
    for (size_t i = count; i > 0; i--) {
        row = items[i - 1]->has_atom
            ? qtt_effect_extend_atom(arena, &items[i - 1]->atom, row)
            : qtt_effect_extend(arena, items[i - 1]->label, row);
        if (!row) return NULL;
    }
    return row;
}

QttEffectUnifyResult qtt_effect_unify(
    QttEffectSolver *solver, QttEffectRow *left, QttEffectRow *right) {
    if (!solver || !left || !right)
        return QTT_EFFECT_UNIFY_MISMATCH;
    if (left->kind == EFFECT_VARIABLE &&
        !binding(solver, left->variable))
        return bind_variable(solver, left->variable, right);
    if (right->kind == EFFECT_VARIABLE &&
        !binding(solver, right->variable))
        return bind_variable(solver, right->variable, left);
    EffectFlat a = flatten(solver, left);
    EffectFlat b = flatten(solver, right);
    if (!a.valid || !b.valid) {
        flat_free(&a); flat_free(&b);
        return QTT_EFFECT_UNIFY_OCCURS;
    }
    const QttEffectRow **only_a = a.count ? malloc(a.count * sizeof(*only_a)) : NULL;
    const QttEffectRow **only_b = b.count ? malloc(b.count * sizeof(*only_b)) : NULL;
    if ((a.count && !only_a) || (b.count && !only_b)) {
        free(only_a); free(only_b); flat_free(&a); flat_free(&b);
        return QTT_EFFECT_UNIFY_OUT_OF_MEMORY;
    }
    size_t ia = 0, ib = 0, na = 0, nb = 0;
    while (ia < a.count && ib < b.count) {
        int order = compare_items(&a.items[ia], &b.items[ib]);
        if (!order) { ia++; ib++; }
        else if (order < 0) only_a[na++] = a.items[ia++];
        else only_b[nb++] = b.items[ib++];
    }
    while (ia < a.count) only_a[na++] = a.items[ia++];
    while (ib < b.count) only_b[nb++] = b.items[ib++];
    QttEffectUnifyResult result = QTT_EFFECT_UNIFY_MISMATCH;
    if (!a.tail && !b.tail)
        result = (!na && !nb)
            ? QTT_EFFECT_UNIFY_OK : QTT_EFFECT_UNIFY_MISMATCH;
    else if (a.tail && !b.tail) {
        if (!na) {
            QttEffectRow *row =
                row_from_items(solver->arena, only_b, nb, NULL);
            result = row ? bind_variable(solver, a.tail, row)
                         : QTT_EFFECT_UNIFY_OUT_OF_MEMORY;
        }
    } else if (!a.tail && b.tail) {
        if (!nb) {
            QttEffectRow *row =
                row_from_items(solver->arena, only_a, na, NULL);
            result = row ? bind_variable(solver, b.tail, row)
                         : QTT_EFFECT_UNIFY_OUT_OF_MEMORY;
        }
    } else if (a.tail == b.tail) {
        result = (!na && !nb)
            ? QTT_EFFECT_UNIFY_OK : QTT_EFFECT_UNIFY_MISMATCH;
    } else {
        QttEffectRow *gamma = qtt_effect_fresh(solver->arena);
        QttEffectRow *for_a = gamma
            ? row_from_items(solver->arena, only_b, nb, gamma) : NULL;
        QttEffectRow *for_b = gamma
            ? row_from_items(solver->arena, only_a, na, gamma) : NULL;
        if (!gamma || !for_a || !for_b)
            result = QTT_EFFECT_UNIFY_OUT_OF_MEMORY;
        else {
            result = bind_variable(solver, a.tail, for_a);
            if (result == QTT_EFFECT_UNIFY_OK)
                result = bind_variable(solver, b.tail, for_b);
        }
    }
    free(only_a); free(only_b); flat_free(&a); flat_free(&b);
    return result;
}

bool qtt_effect_is_closed(
    const QttEffectSolver *solver, const QttEffectRow *row) {
    EffectFlat flat = flatten(solver, row);
    bool result = flat.valid && !flat.tail;
    flat_free(&flat);
    return result;
}

bool qtt_effect_rows_equal(
    const QttEffectSolver *solver,
    const QttEffectRow *left, const QttEffectRow *right) {
    EffectFlat a = flatten(solver, left);
    EffectFlat b = flatten(solver, right);
    bool equal = a.valid && b.valid && a.tail == b.tail &&
        a.count == b.count;
    for (size_t i = 0; equal && i < a.count; i++)
        equal = compare_items(&a.items[i], &b.items[i]) == 0;
    flat_free(&a); flat_free(&b);
    return equal;
}

QttEffectRelation qtt_effect_subrow(
    const QttEffectSolver *solver, const QttEffectRow *sub,
    const QttEffectRow *super) {
    if (!sub || !super) return QTT_EFFECT_RELATION_REFUTED;
    EffectFlat a = flatten(solver, sub);
    EffectFlat b = flatten(solver, super);
    if (!a.valid || !b.valid) {
        flat_free(&a);
        flat_free(&b);
        return QTT_EFFECT_RELATION_REFUTED;
    }
    if (qtt_effect_rows_equal(solver, sub, super)) {
        flat_free(&a);
        flat_free(&b);
        return QTT_EFFECT_RELATION_PROVED;
    }
    bool unknown = a.tail != 0;
    size_t ia = 0, ib = 0;
    while (ia < a.count) {
        while (ib < b.count &&
               compare_items(&b.items[ib], &a.items[ia]) < 0)
            ib++;
        if (ib >= b.count ||
            compare_items(&a.items[ia], &b.items[ib]) != 0) {
            if (!b.tail) {
                flat_free(&a);
                flat_free(&b);
                return QTT_EFFECT_RELATION_REFUTED;
            }
            unknown = true;
            ia++;
            continue;
        }
        ia++;
        ib++;
    }
    flat_free(&a);
    flat_free(&b);
    return unknown ? QTT_EFFECT_RELATION_UNKNOWN
                   : QTT_EFFECT_RELATION_PROVED;
}

QttEffectUnifyResult qtt_effect_refine_subrow(
    QttEffectSolver *solver, QttEffectRow *sub, QttEffectRow *super,
    QttEffectRelation *relation) {
    if (relation) *relation = QTT_EFFECT_RELATION_REFUTED;
    if (!solver || !sub || !super || !relation)
        return QTT_EFFECT_UNIFY_MISMATCH;
    QttEffectRelation current = qtt_effect_subrow(solver, sub, super);
    if (current != QTT_EFFECT_RELATION_UNKNOWN) {
        *relation = current;
        return current == QTT_EFFECT_RELATION_REFUTED
            ? QTT_EFFECT_UNIFY_MISMATCH : QTT_EFFECT_UNIFY_OK;
    }
    EffectFlat a = flatten(solver, sub);
    EffectFlat b = flatten(solver, super);
    if (!a.valid || !b.valid) {
        flat_free(&a);
        flat_free(&b);
        return QTT_EFFECT_UNIFY_OCCURS;
    }
    /* With a closed upper row, an unknown lower tail remains a genuine
     * inclusion constraint. An open upper tail can still absorb the lower
     * row's currently known missing component without equating the tails. */
    if (!b.tail) {
        flat_free(&a);
        flat_free(&b);
        *relation = QTT_EFFECT_RELATION_UNKNOWN;
        return QTT_EFFECT_UNIFY_OK;
    }
    const QttEffectRow **missing = a.count
        ? malloc(a.count * sizeof(*missing)) : NULL;
    if (a.count && !missing) {
        flat_free(&a);
        flat_free(&b);
        return QTT_EFFECT_UNIFY_OUT_OF_MEMORY;
    }
    size_t ia = 0, ib = 0, count = 0;
    while (ia < a.count) {
        while (ib < b.count &&
               compare_items(&b.items[ib], &a.items[ia]) < 0)
            ib++;
        if (ib < b.count &&
            compare_items(&a.items[ia], &b.items[ib]) == 0) {
            ia++;
            ib++;
        } else {
            missing[count++] = a.items[ia++];
        }
    }
    if (!count) {
        free(missing);
        flat_free(&a);
        flat_free(&b);
        *relation = QTT_EFFECT_RELATION_UNKNOWN;
        return QTT_EFFECT_UNIFY_OK;
    }
    bool lower_open = a.tail != 0;
    QttEffectRow *gamma = qtt_effect_fresh(solver->arena);
    QttEffectRow *extension = gamma
        ? row_from_items(solver->arena, missing, count, gamma) : NULL;
    free(missing);
    flat_free(&a);
    uint64_t tail = b.tail;
    flat_free(&b);
    if (!gamma || !extension) return QTT_EFFECT_UNIFY_OUT_OF_MEMORY;
    QttEffectUnifyResult bound = bind_variable(solver, tail, extension);
    if (bound != QTT_EFFECT_UNIFY_OK) return bound;
    *relation = lower_open ? QTT_EFFECT_RELATION_UNKNOWN
                           : qtt_effect_subrow(solver, sub, super);
    return QTT_EFFECT_UNIFY_OK;
}

size_t qtt_effect_label_count(
    const QttEffectSolver *solver,
    const QttEffectRow *row, const char *label) {
    if (!label) return 0;
    EffectFlat flat = flatten(solver, row);
    size_t count = 0;
    for (size_t i = 0; flat.valid && i < flat.count; i++)
        if (strcmp(flat.items[i]->label, label) == 0) count++;
    flat_free(&flat);
    return count;
}

QttEffectRelation qtt_effect_row_lacks(
    const QttEffectSolver *solver,
    const QttEffectRow *row, const char *label) {
    if (!row || !label) return QTT_EFFECT_RELATION_REFUTED;
    EffectFlat flat = flatten(solver, row);
    if (!flat.valid) {
        flat_free(&flat);
        return QTT_EFFECT_RELATION_REFUTED;
    }
    for (size_t i = 0; i < flat.count; i++)
        if (strcmp(flat.items[i]->label, label) == 0) {
            flat_free(&flat);
            return QTT_EFFECT_RELATION_REFUTED;
        }
    QttEffectRelation relation = flat.tail
        ? QTT_EFFECT_RELATION_UNKNOWN : QTT_EFFECT_RELATION_PROVED;
    flat_free(&flat);
    return relation;
}

size_t qtt_effect_atom_count(
    const QttEffectSolver *solver, const QttEffectRow *row,
    const QttEffectAtom *atom) {
    if (!atom) return 0;
    EffectFlat flat = flatten(solver, row);
    size_t count = 0;
    for (size_t i = 0; flat.valid && i < flat.count; i++)
        if (flat.items[i]->has_atom &&
            qtt_effect_atom_equal(&flat.items[i]->atom, atom))
            count++;
    flat_free(&flat);
    return count;
}

QttEffectRow *qtt_effect_handle_one(
    QttEffectArena *arena, const QttEffectSolver *solver,
    const QttEffectRow *row, const char *label) {
    if (!arena || !row || !label) return NULL;
    EffectFlat flat = flatten(solver, row);
    size_t remove = SIZE_MAX;
    for (size_t i = 0; flat.valid && i < flat.count; i++)
        if (strcmp(flat.items[i]->label, label) == 0) {
            remove = i;
            break;
        }
    if (!flat.valid || remove == SIZE_MAX) {
        flat_free(&flat);
        return NULL;
    }
    for (size_t i = remove + 1; i < flat.count; i++)
        flat.items[i - 1] = flat.items[i];
    QttEffectRow *tail = flat.tail ? qtt_effect_fresh(arena) : NULL;
    if (tail) tail->variable = flat.tail;
    QttEffectRow *result = row_from_items(
        arena, flat.items, flat.count - 1, tail);
    flat_free(&flat);
    return result;
}

QttEffectRow *qtt_effect_handle_atom(
    QttEffectArena *arena, const QttEffectSolver *solver,
    const QttEffectRow *row, const QttEffectAtom *atom) {
    if (!arena || !row || !atom) return NULL;
    EffectFlat flat = flatten(solver, row);
    size_t remove = SIZE_MAX;
    for (size_t i = 0; flat.valid && i < flat.count; i++)
        if (flat.items[i]->has_atom &&
            qtt_effect_atom_equal(&flat.items[i]->atom, atom)) {
            remove = i;
            break;
        }
    if (!flat.valid || remove == SIZE_MAX) {
        flat_free(&flat);
        return NULL;
    }
    for (size_t i = remove + 1; i < flat.count; i++)
        flat.items[i - 1] = flat.items[i];
    QttEffectRow *tail = flat.tail ? qtt_effect_fresh(arena) : NULL;
    if (tail) tail->variable = flat.tail;
    QttEffectRow *result = row_from_items(
        arena, flat.items, flat.count - 1, tail);
    flat_free(&flat);
    return result;
}

QttEffectHandlerResult qtt_effect_elaborate_handler(
    QttEffectArena *arena, const QttEffectSolver *solver,
    const QttEffectRow *row, const QttEffectAtom *atom,
    QttQuantity continuation_usage, QttQuantity capture_allowance) {
    QttEffectHandlerResult result = {
        .status = QTT_EFFECT_HANDLER_INVALID,
        .residual = NULL,
        .continuation_demand = qtt_quantity_finite(0),
    };
    if (!arena || !solver || !row || !atom)
        return result;

    if (qtt_effect_atom_count(solver, row, atom) == 0) {
        result.status = QTT_EFFECT_HANDLER_ABSENT;
        return result;
    }

    result.continuation_demand = qtt_quantity_multiply(
        atom->resumption, continuation_usage);
    if (!qtt_quantity_leq(
            result.continuation_demand, capture_allowance)) {
        result.status = QTT_EFFECT_HANDLER_GRADE_VIOLATION;
        return result;
    }

    result.residual = qtt_effect_handle_atom(
        arena, solver, row, atom);
    result.status = result.residual ? QTT_EFFECT_HANDLER_OK
                                    : QTT_EFFECT_HANDLER_INVALID;
    return result;
}

static int handler_fingerprint_compare(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

/* Handler certificates quantify over an open row tail.  Its arena-local
 * variable number is therefore a binder, not semantic evidence: hash the
 * existence of the tail, making the certificate invariant under alpha
 * renaming while retaining the complete known effect prefix. */
static uint64_t handler_row_fingerprint(
    const QttEffectSolver *solver, const QttEffectRow *row) {
    if (!row) return 0;
    EffectFlat flat = flatten(solver, row);
    if (!flat.valid) {
        flat_free(&flat);
        return 0;
    }
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = hash_u64(hash, flat.count);
    for (size_t i = 0; i < flat.count; i++) {
        unsigned char typed = flat.items[i]->has_atom;
        hash = hash_bytes(hash, &typed, 1);
        hash = flat.items[i]->has_atom
            ? hash_u64(hash,
                qtt_effect_atom_fingerprint(&flat.items[i]->atom))
            : hash_string(hash, flat.items[i]->label);
    }
    hash = hash_u64(hash, flat.tail != 0);
    flat_free(&flat);
    return hash ? hash : 1;
}

QttEffectHandlerSetResult qtt_effect_elaborate_handler_set(
    QttEffectArena *arena, const QttEffectSolver *solver,
    const QttEffectRow *computation_effects,
    const QttEffectHandlerClause *clauses, size_t clause_count) {
    QttEffectHandlerSetResult result = {
        .status = QTT_EFFECT_HANDLER_INVALID,
    };
    if (!arena || !solver || !computation_effects ||
        (clause_count > 0 && !clauses))
        return result;

    for (size_t i = 0; i < clause_count; i++) {
        if (!clauses[i].atom) return result;
        for (size_t j = 0; j < i; j++)
            if (qtt_effect_atom_equal(clauses[i].atom, clauses[j].atom)) {
                result.status = QTT_EFFECT_HANDLER_DUPLICATE_CLAUSE;
                return result;
            }
    }

    QttEffectRow *empty = qtt_effect_empty(arena);
    QttEffectRow *residual = qtt_effect_join(
        arena, solver, computation_effects, empty);
    QttEffectRow *clause_union = qtt_effect_empty(arena);
    uint64_t *clause_fingerprints = clause_count
        ? malloc(clause_count * sizeof(uint64_t)) : NULL;
    if (!residual || !clause_union ||
        (clause_count && !clause_fingerprints)) {
        free(clause_fingerprints);
        return result;
    }

    for (size_t i = 0; i < clause_count; i++) {
        const QttEffectHandlerClause *clause = &clauses[i];
        if (clause->atom->scoped && clause->clause_effects &&
            qtt_effect_atom_count(
                solver, clause->clause_effects, clause->atom) > 0) {
            result.status = QTT_EFFECT_HANDLER_SCOPED_ESCAPE;
            free(clause_fingerprints);
            return result;
        }

        QttEffectHandlerResult one = qtt_effect_elaborate_handler(
            arena, solver, residual, clause->atom,
            clause->continuation_usage, clause->capture_allowance);
        if (one.status != QTT_EFFECT_HANDLER_OK) {
            result.status = one.status;
            free(clause_fingerprints);
            return result;
        }
        residual = one.residual;

        const QttEffectRow *effects = clause->clause_effects
            ? clause->clause_effects : empty;
        clause_union = qtt_effect_join(
            arena, solver, clause_union, effects);
        if (!clause_union) {
            free(clause_fingerprints);
            return result;
        }

        uint64_t fingerprint = UINT64_C(1469598103934665603);
        fingerprint = hash_u64(
            fingerprint, qtt_effect_atom_fingerprint(clause->atom));
        fingerprint = hash_u64(
            fingerprint, clause->continuation_usage.is_omega);
        fingerprint = hash_u64(
            fingerprint, clause->continuation_usage.finite);
        fingerprint = hash_u64(
            fingerprint, clause->capture_allowance.is_omega);
        fingerprint = hash_u64(
            fingerprint, clause->capture_allowance.finite);
        fingerprint = hash_u64(
            fingerprint, handler_row_fingerprint(solver, effects));
        clause_fingerprints[i] = fingerprint;
    }

    QttEffectRow *output = qtt_effect_join(
        arena, solver, residual, clause_union);
    if (!output) {
        free(clause_fingerprints);
        return result;
    }

    qsort(clause_fingerprints, clause_count, sizeof(uint64_t),
          handler_fingerprint_compare);
    uint64_t proof = UINT64_C(1469598103934665603);
    proof = hash_u64(proof, clause_count);
    for (size_t i = 0; i < clause_count; i++)
        proof = hash_u64(proof, clause_fingerprints[i]);
    proof = hash_u64(
        proof, handler_row_fingerprint(solver, residual));
    proof = hash_u64(
        proof, handler_row_fingerprint(solver, output));
    free(clause_fingerprints);

    result.status = QTT_EFFECT_HANDLER_OK;
    result.residual = residual;
    result.output_effects = output;
    result.handled_count = clause_count;
    result.proof_fingerprint = proof ? proof : 1;
    return result;
}

bool qtt_effect_verify_handler_set(
    QttEffectArena *arena, const QttEffectSolver *solver,
    const QttEffectRow *computation_effects,
    const QttEffectHandlerClause *clauses, size_t clause_count,
    uint64_t expected_proof_fingerprint) {
    if (!expected_proof_fingerprint) return false;
    QttEffectHandlerSetResult replay = qtt_effect_elaborate_handler_set(
        arena, solver, computation_effects, clauses, clause_count);
    return replay.status == QTT_EFFECT_HANDLER_OK &&
           replay.proof_fingerprint == expected_proof_fingerprint;
}

char *qtt_effect_handler_proof_serialize(
    const QttEffectSolver *solver,
    const QttEffectHandlerSetResult *result) {
    if (!solver || !result || result->status != QTT_EFFECT_HANDLER_OK ||
        !result->residual || !result->output_effects ||
        !result->proof_fingerprint) return NULL;
    uint64_t residual = handler_row_fingerprint(solver, result->residual);
    uint64_t output = handler_row_fingerprint(solver, result->output_effects);
    int size = snprintf(NULL, 0,
        "monad-handler-proof-v2|%zu|%016" PRIx64 "|%016" PRIx64
        "|%016" PRIx64,
        result->handled_count, result->proof_fingerprint, residual, output);
    if (size < 0) return NULL;
    char *portable = malloc((size_t)size + 1);
    if (!portable) return NULL;
    snprintf(portable, (size_t)size + 1,
        "monad-handler-proof-v2|%zu|%016" PRIx64 "|%016" PRIx64
        "|%016" PRIx64,
        result->handled_count, result->proof_fingerprint, residual, output);
    return portable;
}

bool qtt_effect_handler_proof_deserialize(
    const char *portable, QttEffectHandlerProof *proof) {
    if (!portable || !proof) return false;
    QttEffectHandlerProof parsed = {0};
    char extra;
    int fields = sscanf(portable,
        "monad-handler-proof-v2|%zu|%16" SCNx64 "|%16" SCNx64
        "|%16" SCNx64 "%c",
        &parsed.handled_count, &parsed.proof_fingerprint,
        &parsed.residual_fingerprint, &parsed.output_fingerprint, &extra);
    if (fields == 4) parsed.format_version = 2;
    else {
        fields = sscanf(portable,
            "monad-handler-proof-v1|%zu|%16" SCNx64 "|%16" SCNx64
            "|%16" SCNx64 "%c",
            &parsed.handled_count, &parsed.proof_fingerprint,
            &parsed.residual_fingerprint, &parsed.output_fingerprint, &extra);
        if (fields == 4) parsed.format_version = 1;
    }
    if (fields != 4 || !parsed.proof_fingerprint ||
        !parsed.residual_fingerprint || !parsed.output_fingerprint)
        return false;
    *proof = parsed;
    return true;
}

bool qtt_effect_verify_portable_handler_proof(
    QttEffectArena *arena, const QttEffectSolver *solver,
    const QttEffectRow *computation_effects,
    const QttEffectHandlerClause *clauses, size_t clause_count,
    const char *portable) {
    QttEffectHandlerProof proof;
    if (!qtt_effect_handler_proof_deserialize(portable, &proof) ||
        proof.handled_count != clause_count) return false;
    QttEffectHandlerSetResult replay = qtt_effect_elaborate_handler_set(
        arena, solver, computation_effects, clauses, clause_count);
    return replay.status == QTT_EFFECT_HANDLER_OK &&
        replay.proof_fingerprint == proof.proof_fingerprint &&
        handler_row_fingerprint(solver, replay.residual) ==
            proof.residual_fingerprint &&
        handler_row_fingerprint(solver, replay.output_effects) ==
            proof.output_fingerprint;
}

QttEffectRow *qtt_effect_join(
    QttEffectArena *arena, const QttEffectSolver *solver,
    const QttEffectRow *left, const QttEffectRow *right) {
    if (!arena || !left || !right) return NULL;
    EffectFlat a = flatten(solver, left);
    EffectFlat b = flatten(solver, right);
    if (!a.valid || !b.valid) {
        flat_free(&a);
        flat_free(&b);
        return NULL;
    }
    size_t capacity = a.count + b.count;
    const QttEffectRow **joined = capacity
        ? malloc(capacity * sizeof(*joined)) : NULL;
    if (capacity && !joined) {
        flat_free(&a);
        flat_free(&b);
        return NULL;
    }
    size_t ia = 0, ib = 0, count = 0;
    while (ia < a.count && ib < b.count) {
        int order = compare_items(&a.items[ia], &b.items[ib]);
        if (order < 0) joined[count++] = a.items[ia++];
        else if (order > 0) joined[count++] = b.items[ib++];
        else {
            joined[count++] = a.items[ia++];
            ib++;
        }
    }
    while (ia < a.count) joined[count++] = a.items[ia++];
    while (ib < b.count) joined[count++] = b.items[ib++];
    uint64_t tail_variable = a.tail && b.tail
        ? (a.tail == b.tail ? a.tail : 0)
        : (a.tail ? a.tail : b.tail);
    QttEffectRow *tail = (a.tail || b.tail)
        ? qtt_effect_fresh(arena) : NULL;
    /* Preserve an exact shared/single remainder. Distinct open tails still
     * require a fresh existential summary until constraints relate them. */
    if (tail && tail_variable) tail->variable = tail_variable;
    QttEffectRow *result = row_from_items(
        arena, joined, count, tail);
    free(joined);
    flat_free(&a);
    flat_free(&b);
    return result;
}

QttEffectRow *qtt_effect_join_closed(
    QttEffectArena *arena, const QttEffectSolver *solver,
    const QttEffectRow *left, const QttEffectRow *right) {
    if (!qtt_effect_is_closed(solver, left) ||
        !qtt_effect_is_closed(solver, right))
        return NULL;
    return qtt_effect_join(arena, solver, left, right);
}

QttEffectRow *qtt_effect_clone_closed(
    QttEffectArena *arena, const QttEffectSolver *solver,
    const QttEffectRow *row) {
    if (!arena || !row) return NULL;
    EffectFlat flat = flatten(solver, row);
    if (!flat.valid || flat.tail) {
        flat_free(&flat);
        return NULL;
    }
    QttEffectRow *copy = row_from_items(
        arena, flat.items, flat.count, NULL);
    flat_free(&flat);
    return copy;
}

uint64_t qtt_effect_row_fingerprint(
    const QttEffectSolver *solver, const QttEffectRow *row) {
    if (!row) return 0;
    EffectFlat flat = flatten(solver, row);
    if (!flat.valid) {
        flat_free(&flat);
        return 0;
    }
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = hash_u64(hash, flat.count);
    for (size_t i = 0; i < flat.count; i++) {
        unsigned char typed = flat.items[i]->has_atom;
        hash = hash_bytes(hash, &typed, 1);
        hash = flat.items[i]->has_atom
            ? hash_u64(hash,
                qtt_effect_atom_fingerprint(&flat.items[i]->atom))
            : hash_string(hash, flat.items[i]->label);
    }
    hash = hash_u64(hash, flat.tail);
    flat_free(&flat);
    return hash ? hash : 1;
}

char *qtt_effect_format(
    const QttEffectSolver *solver, const QttEffectRow *row) {
    EffectFlat flat = flatten(solver, row);
    if (!flat.valid) {
        flat_free(&flat);
        return NULL;
    }
    size_t size = 4;
    for (size_t i = 0; i < flat.count; i++)
        size += strlen(flat.items[i]->label) + 1;
    if (flat.tail) size += 32;
    char *text = malloc(size);
    if (!text) {
        flat_free(&flat);
        return NULL;
    }
    size_t used = 0;
    text[used++] = '<';
    for (size_t i = 0; i < flat.count; i++) {
        if (i) text[used++] = ',';
        size_t length = strlen(flat.items[i]->label);
        memcpy(text + used, flat.items[i]->label, length);
        used += length;
    }
    if (flat.tail)
        used += (size_t)snprintf(
            text + used, size - used, "%s|e%llu",
            flat.count ? "," : "",
            (unsigned long long)flat.tail);
    text[used++] = '>';
    text[used] = '\0';
    flat_free(&flat);
    return text;
}

QttEffectScheme *qtt_effect_generalize(
    QttEffectArena *arena, const QttEffectSolver *solver,
    const QttEffectRow *row) {
    if (!arena || !row) return NULL;
    EffectFlat flat = flatten(solver, row);
    if (!flat.valid) {
        flat_free(&flat);
        return NULL;
    }
    QttEffectRow *tail = flat.tail ? qtt_effect_fresh(arena) : NULL;
    if (tail) tail->variable = flat.tail;
    QttEffectRow *normalized =
        row_from_items(arena, flat.items, flat.count, tail);
    if (!normalized) {
        flat_free(&flat);
        return NULL;
    }
    QttEffectScheme *scheme = calloc(1, sizeof(*scheme));
    if (scheme) {
        arena->references++;
        scheme->arena = arena;
        scheme->row = normalized;
        scheme->quantified = flat.tail;
        scheme->quantified_count = flat.tail ? 1 : 0;
        scheme->references = 1;
    }
    flat_free(&flat);
    return scheme;
}

QttEffectScheme *qtt_effect_scheme_retain(QttEffectScheme *scheme) {
    if (scheme) scheme->references++;
    return scheme;
}

size_t qtt_effect_scheme_quantified_count(
    const QttEffectScheme *scheme) {
    return scheme ? scheme->quantified_count : 0;
}

uint64_t qtt_effect_scheme_fingerprint(
    const QttEffectScheme *scheme) {
    if (!scheme) return 0;
    EffectFlat flat = flatten(NULL, scheme->row);
    if (!flat.valid) {
        flat_free(&flat);
        return 0;
    }
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = hash_u64(hash, flat.count);
    for (size_t i = 0; i < flat.count; i++) {
        unsigned char typed = flat.items[i]->has_atom;
        hash = hash_bytes(hash, &typed, 1);
        hash = flat.items[i]->has_atom
            ? hash_u64(hash,
                qtt_effect_atom_fingerprint(&flat.items[i]->atom))
            : hash_string(hash, flat.items[i]->label);
    }
    /* Alpha-equivalent quantified tails have one canonical marker. */
    hash = hash_u64(hash, scheme->quantified_count ? 1 : 0);
    hash = hash_u64(hash, scheme->callable_parameter_count);
    for (size_t i = 0; i < scheme->callable_parameter_count; i++) {
        hash = hash_u64(hash, scheme->callable_parameter_indices[i]);
        hash = hash_u64(hash, scheme->callable_parameter_arities[i]);
    }
    flat_free(&flat);
    return hash ? hash : 1;
}

static uint64_t portable_evidence_fingerprint(const char *text) {
    if (!text) return 0;
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        hash ^= *p;
        hash *= UINT64_C(1099511628211);
    }
    return hash ? hash : 1;
}

bool qtt_effect_scheme_set_evidence(
    QttEffectScheme *scheme, const char *portable_certificate,
    size_t obligation_count, uint64_t certificate_fingerprint,
    QttEffectEvidenceStatus status) {
    static const char header_v1[] = "monad-effect-certificate-v1\n";
    static const char header_v2[] = "monad-effect-certificate-v2\n";
    if (!scheme || !portable_certificate || !certificate_fingerprint ||
        status == QTT_EFFECT_EVIDENCE_NONE ||
        (strncmp(portable_certificate, header_v1,
             sizeof(header_v1) - 1) != 0 &&
         strncmp(portable_certificate, header_v2,
             sizeof(header_v2) - 1) != 0) ||
        portable_evidence_fingerprint(portable_certificate) !=
            certificate_fingerprint)
        return false;
    size_t length = strlen(portable_certificate) + 1;
    char *owned = malloc(length);
    if (!owned) return false;
    memcpy(owned, portable_certificate, length);
    free(scheme->portable_evidence);
    scheme->portable_evidence = owned;
    scheme->evidence_count = obligation_count;
    scheme->evidence_fingerprint = certificate_fingerprint;
    scheme->evidence_status = status;
    return true;
}

const char *qtt_effect_scheme_evidence(const QttEffectScheme *scheme) {
    return scheme ? scheme->portable_evidence : NULL;
}

size_t qtt_effect_scheme_evidence_count(const QttEffectScheme *scheme) {
    return scheme ? scheme->evidence_count : 0;
}

uint64_t qtt_effect_scheme_evidence_fingerprint(
    const QttEffectScheme *scheme) {
    return scheme ? scheme->evidence_fingerprint : 0;
}

QttEffectEvidenceStatus qtt_effect_scheme_evidence_status(
    const QttEffectScheme *scheme) {
    return scheme ? scheme->evidence_status : QTT_EFFECT_EVIDENCE_NONE;
}

bool qtt_effect_scheme_evidence_intact(const QttEffectScheme *scheme) {
    return scheme && scheme->portable_evidence &&
        scheme->evidence_status != QTT_EFFECT_EVIDENCE_NONE &&
        scheme->evidence_fingerprint ==
            portable_evidence_fingerprint(scheme->portable_evidence);
}

void qtt_effect_scheme_set_coverage_gaps(
    QttEffectScheme *scheme, uint32_t gaps) {
    if (scheme) scheme->coverage_gaps = gaps;
}

uint32_t qtt_effect_scheme_coverage_gaps(const QttEffectScheme *scheme) {
    return scheme ? scheme->coverage_gaps : 0;
}

bool qtt_effect_scheme_set_callable_parameters(
    QttEffectScheme *scheme, const size_t *indices, size_t count) {
    size_t *arities = count ? malloc(count * sizeof(*arities)) : NULL;
    if (count && !arities) return false;
    for (size_t i = 0; i < count; i++) arities[i] = 1;
    bool result = qtt_effect_scheme_set_callable_parameter_contracts(
        scheme, indices, arities, count);
    free(arities);
    return result;
}

bool qtt_effect_scheme_set_callable_parameter_contracts(
    QttEffectScheme *scheme, const size_t *indices,
    const size_t *arities, size_t count) {
    if (!scheme || (count && (!indices || !arities))) return false;
    size_t *owned = count ? malloc(count * sizeof(*owned)) : NULL;
    size_t *owned_arities = count
        ? malloc(count * sizeof(*owned_arities)) : NULL;
    if (count && (!owned || !owned_arities)) {
        free(owned);
        free(owned_arities);
        return false;
    }
    if (count) memcpy(owned, indices, count * sizeof(*owned));
    if (count)
        memcpy(owned_arities, arities, count * sizeof(*owned_arities));
    free(scheme->callable_parameter_indices);
    free(scheme->callable_parameter_arities);
    scheme->callable_parameter_indices = owned;
    scheme->callable_parameter_arities = owned_arities;
    scheme->callable_parameter_count = count;
    return true;
}

size_t qtt_effect_scheme_callable_parameter_count(
    const QttEffectScheme *scheme) {
    return scheme ? scheme->callable_parameter_count : 0;
}

size_t qtt_effect_scheme_callable_parameter_index(
    const QttEffectScheme *scheme, size_t index) {
    return scheme && index < scheme->callable_parameter_count
        ? scheme->callable_parameter_indices[index] : SIZE_MAX;
}

size_t qtt_effect_scheme_callable_parameter_arity(
    const QttEffectScheme *scheme, size_t index) {
    return scheme && index < scheme->callable_parameter_count
        ? scheme->callable_parameter_arities[index] : 0;
}

static char effect_hex_digit(unsigned value) {
    return "0123456789abcdef"[value & 15u];
}

static char *effect_hex_encode(const char *text) {
    size_t length = strlen(text);
    char *encoded = malloc(length * 2 + 1);
    if (!encoded) return NULL;
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char)text[i];
        encoded[i * 2] = effect_hex_digit(byte >> 4);
        encoded[i * 2 + 1] = effect_hex_digit(byte);
    }
    encoded[length * 2] = '\0';
    return encoded;
}

static int effect_hex_value(char digit) {
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
    return -1;
}

static char *effect_hex_decode(const char *encoded) {
    size_t length = strlen(encoded);
    if (length & 1u) return NULL;
    char *decoded = malloc(length / 2 + 1);
    if (!decoded) return NULL;
    for (size_t i = 0; i < length; i += 2) {
        int high = effect_hex_value(encoded[i]);
        int low = effect_hex_value(encoded[i + 1]);
        if (high < 0 || low < 0) {
            free(decoded);
            return NULL;
        }
        decoded[i / 2] = (char)((high << 4) | low);
    }
    decoded[length / 2] = '\0';
    return decoded;
}

char *qtt_effect_scheme_serialize(const QttEffectScheme *scheme) {
    if (!scheme) return NULL;
    EffectFlat flat = flatten(NULL, scheme->row);
    if (!flat.valid) {
        flat_free(&flat);
        return NULL;
    }
    bool typed = false;
    size_t capacity = 128 + flat.count * 160 +
        scheme->callable_parameter_count * 48;
    for (size_t i = 0; i < flat.count; i++) {
        typed = typed || flat.items[i]->has_atom;
        capacity += strlen(flat.items[i]->label) * 2 + 1;
        if (flat.items[i]->has_atom) {
            const QttEffectAtom *atom = &flat.items[i]->atom;
            capacity += (atom->name ? strlen(atom->name) * 2 : 0) +
                (atom->operation ? strlen(atom->operation) * 2 : 0) +
                (atom->traits ? strlen(atom->traits) * 2 : 0) + 8;
        }
    }
    char *text = malloc(capacity);
    if (!text) {
        flat_free(&flat);
        return NULL;
    }
    size_t used = (size_t)snprintf(
        text, capacity, typed
            ? "monad-effect-scheme-v3|%016llx|%u|"
            : "monad-effect-scheme-v1|%016llx|%u|",
        (unsigned long long)qtt_effect_scheme_fingerprint(scheme),
        scheme->quantified_count ? 1u : 0u);
    for (size_t i = 0; i < flat.count; i++) {
        const QttEffectRow *item = flat.items[i];
        char *encoded = item->has_atom ? NULL : effect_hex_encode(item->label);
        char *name = item->has_atom && item->atom.name
            ? effect_hex_encode(item->atom.name) : NULL;
        char *operation = item->has_atom && item->atom.operation
            ? effect_hex_encode(item->atom.operation) : NULL;
        char *traits = item->has_atom && item->atom.traits
            ? effect_hex_encode(item->atom.traits) : NULL;
        if ((!item->has_atom && !encoded) ||
            (item->has_atom && item->atom.name && !name) ||
            (item->has_atom && item->atom.operation && !operation) ||
            (item->has_atom && item->atom.traits && !traits)) {
            free(encoded); free(name); free(operation); free(traits);
            free(text);
            flat_free(&flat);
            return NULL;
        }
        if (!item->has_atom)
            used += (size_t)snprintf(
                text + used, capacity - used, "%s%s",
                i ? "," : "", encoded);
        else {
            const QttEffectAtom *atom = &item->atom;
            used += (size_t)snprintf(
                text + used, capacity - used,
                "%sA:%u:%016llx:%016llx:%016llx:%u:%016llx:%u:%u:%s:%u:%s:%u:%s",
                i ? "," : "", (unsigned)atom->kind,
                (unsigned long long)atom->constructor_id,
                (unsigned long long)atom->type_id,
                (unsigned long long)atom->capability_id,
                atom->resumption.is_omega ? 1u : 0u,
                (unsigned long long)atom->resumption.finite,
                atom->scoped ? 1u : 0u, atom->name ? 1u : 0u,
                name ? name : "", atom->operation ? 1u : 0u,
                operation ? operation : "", atom->traits ? 1u : 0u,
                traits ? traits : "");
        }
        free(encoded); free(name); free(operation); free(traits);
    }
    used += (size_t)snprintf(text + used, capacity - used, "|");
    for (size_t i = 0; i < scheme->callable_parameter_count; i++)
        used += (size_t)snprintf(
            text + used, capacity - used, "%s%zu:%zu",
            i ? "," : "", scheme->callable_parameter_indices[i],
            scheme->callable_parameter_arities[i]);
    flat_free(&flat);
    return text;
}

static bool effect_parse_size(const char *text, size_t *value) {
    if (!text || !*text || !value) return false;
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno || !end || *end || parsed > SIZE_MAX) return false;
    *value = (size_t)parsed;
    return true;
}

static bool effect_parse_u64_hex(const char *text, uint64_t *value) {
    if (!text || !*text || !value) return false;
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(text, &end, 16);
    if (errno || !end || *end) return false;
    *value = (uint64_t)parsed;
    return true;
}

static bool effect_parse_bit(const char *text, bool *value) {
    if (!text || !value || (strcmp(text, "0") && strcmp(text, "1")))
        return false;
    *value = text[0] == '1';
    return true;
}

static bool effect_decode_atom(
    char *token, QttEffectAtom *atom, bool carries_traits) {
    if (!token || strncmp(token, "A:", 2) || !atom) return false;
    char *fields[13] = {token + 2};
    size_t field_count = carries_traits ? 13 : 11;
    for (size_t i = 0; i + 1 < field_count; i++) {
        char *colon = strchr(fields[i], ':');
        if (!colon) return false;
        *colon = '\0';
        fields[i + 1] = colon + 1;
    }
    if (strchr(fields[field_count - 1], ':')) return false;
    size_t kind = 0;
    bool omega = false, scoped = false, has_name = false, has_operation = false;
    if (!effect_parse_size(fields[0], &kind) || kind > QTT_EFFECT_CONTROL ||
        !effect_parse_u64_hex(fields[1], &atom->constructor_id) ||
        !effect_parse_u64_hex(fields[2], &atom->type_id) ||
        !effect_parse_u64_hex(fields[3], &atom->capability_id) ||
        !effect_parse_bit(fields[4], &omega) ||
        !effect_parse_u64_hex(fields[5], &atom->resumption.finite) ||
        !effect_parse_bit(fields[6], &scoped) ||
        !effect_parse_bit(fields[7], &has_name) ||
        !effect_parse_bit(fields[9], &has_operation))
        return false;
    atom->kind = (QttEffectKind)kind;
    atom->resumption.is_omega = omega;
    atom->scoped = scoped;
    atom->name = has_name ? effect_hex_decode(fields[8]) : NULL;
    atom->operation = has_operation ? effect_hex_decode(fields[10]) : NULL;
    bool has_traits = false;
    if (carries_traits &&
        (!effect_parse_bit(fields[11], &has_traits))) return false;
    atom->traits = carries_traits && has_traits
        ? effect_hex_decode(fields[12]) : NULL;
    return (!has_name || atom->name) && (!has_operation || atom->operation) &&
        (!has_traits || atom->traits) &&
        (has_name || !*fields[8]) && (has_operation || !*fields[10]) &&
        (!carries_traits || has_traits || !*fields[12]);
}

QttEffectScheme *qtt_effect_scheme_deserialize(const char *text) {
    static const char header_v1[] = "monad-effect-scheme-v1|";
    static const char header_v2[] = "monad-effect-scheme-v2|";
    static const char header_v3[] = "monad-effect-scheme-v3|";
    bool v3 = text && strncmp(text, header_v3, sizeof(header_v3) - 1) == 0;
    bool typed = v3 || (text && strncmp(text, header_v2, sizeof(header_v2) - 1) == 0);
    if (!typed && (!text || strncmp(text, header_v1, sizeof(header_v1) - 1)))
        return NULL;
    const char *payload = text + (v3 ? sizeof(header_v3) - 1 : typed
        ? sizeof(header_v2) - 1 : sizeof(header_v1) - 1);
    char *copy = malloc(strlen(payload) + 1);
    if (!copy) return NULL;
    strcpy(copy, payload);
    char *fields[4] = {copy, NULL, NULL, NULL};
    bool valid = true;
    for (size_t i = 0; i < 3; i++) {
        char *separator = strchr(fields[i], '|');
        if (!separator) { valid = false; break; }
        *separator = '\0';
        fields[i + 1] = separator + 1;
    }
    unsigned long long declared = 0;
    char extra = '\0';
    if (!valid || sscanf(fields[0], "%llx%c", &declared, &extra) != 1 ||
            (strcmp(fields[1], "0") != 0 && strcmp(fields[1], "1") != 0)) {
        free(copy);
        return NULL;
    }
    QttEffectArena *arena = qtt_effect_arena_new();
    QttEffectSolver *solver = arena ? qtt_effect_solver_new(arena) : NULL;
    QttEffectRow *row = arena
        ? (fields[1][0] == '1'
            ? qtt_effect_fresh(arena) : qtt_effect_empty(arena))
        : NULL;
    QttEffectAtom *atoms = NULL;
    bool *is_atom = NULL;
    char **labels = NULL;
    size_t label_count = 0;
    if (row && *fields[2]) {
        size_t entry_capacity = 1;
        for (const char *cursor = fields[2]; *cursor; cursor++)
            if (*cursor == ',') entry_capacity++;
        labels = calloc(entry_capacity, sizeof(*labels));
        atoms = calloc(entry_capacity, sizeof(*atoms));
        is_atom = calloc(entry_capacity, sizeof(*is_atom));
        if (!labels || !atoms || !is_atom) valid = false;
        for (char *token = fields[2]; token;) {
            if (!valid) break;
            char *next = strchr(token, ',');
            if (next) *next++ = '\0';
            is_atom[label_count] = typed && !strncmp(token, "A:", 2);
            labels[label_count] = is_atom[label_count]
                ? NULL : effect_hex_decode(token);
            if ((is_atom[label_count] &&
                 !effect_decode_atom(token, &atoms[label_count], v3)) ||
                (!is_atom[label_count] && !labels[label_count])) {
                free((char *)atoms[label_count].name);
                free((char *)atoms[label_count].operation);
                free((char *)atoms[label_count].traits);
                atoms[label_count].name = NULL;
                atoms[label_count].operation = NULL;
                valid = false; break;
            }
            label_count++;
            token = next;
        }
    }
    for (size_t i = label_count; valid && i > 0; i--)
        row = is_atom[i - 1]
            ? qtt_effect_extend_atom(arena, &atoms[i - 1], row)
            : qtt_effect_extend(arena, labels[i - 1], row);
    for (size_t i = 0; i < label_count; i++) {
        free(labels[i]);
        free((char *)atoms[i].name);
        free((char *)atoms[i].operation);
        free((char *)atoms[i].traits);
    }
    free(labels);
    free(atoms);
    free(is_atom);
    QttEffectScheme *scheme = valid && row
        ? qtt_effect_generalize(arena, solver, row) : NULL;
    size_t indices[64], arities[64], dependency_count = 0;
    if (scheme && *fields[3]) {
        for (char *token = fields[3]; token;) {
            char *next = strchr(token, ',');
            if (next) *next++ = '\0';
            char *colon = strchr(token, ':');
            if (!colon || dependency_count == 64) { valid = false; break; }
            *colon = '\0';
            valid = effect_parse_size(token, &indices[dependency_count]) &&
                effect_parse_size(colon + 1, &arities[dependency_count]) &&
                arities[dependency_count] > 0;
            if (!valid) break;
            dependency_count++;
            token = next;
        }
    }
    if (scheme && valid)
        valid = qtt_effect_scheme_set_callable_parameter_contracts(
            scheme, indices, arities, dependency_count);
    if (!valid || !scheme ||
            qtt_effect_scheme_fingerprint(scheme) != (uint64_t)declared) {
        qtt_effect_scheme_free(scheme);
        scheme = NULL;
    }
    qtt_effect_solver_free(solver);
    qtt_effect_arena_free(arena);
    free(copy);
    return scheme;
}

bool qtt_effect_scheme_is_empty(const QttEffectScheme *scheme) {
    return scheme && scheme->row && scheme->row->kind == EFFECT_EMPTY;
}

char *qtt_effect_coverage_format(uint32_t gaps) {
    static const struct { uint32_t bit; const char *name; } names[] = {
        {QTT_EFFECT_COVERAGE_OPEN_ROW,               "open-row"},
        {QTT_EFFECT_COVERAGE_UNKNOWN_CALLEE,         "unknown-callee"},
        {QTT_EFFECT_COVERAGE_INCOMPLETE_CALLEE,      "incomplete-callee"},
        {QTT_EFFECT_COVERAGE_PARTIAL_LEGACY_CALL,    "partial-legacy-call"},
        {QTT_EFFECT_COVERAGE_ARITY_MISMATCH,         "arity-mismatch"},
        {QTT_EFFECT_COVERAGE_RESIDUAL_OBLIGATION,    "residual-obligation"},
        {QTT_EFFECT_COVERAGE_MISSING_ARROW_CONTRACT, "missing-arrow-contract"},
    };
    size_t size = 5;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (gaps & names[i].bit) size += strlen(names[i].name) + 1;
    char *text = malloc(size);
    if (!text) return NULL;
    size_t used = 0;
    if (!gaps) {
        memcpy(text, "none", 5);
        return text;
    }
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!(gaps & names[i].bit)) continue;
        if (used) text[used++] = ',';
        size_t length = strlen(names[i].name);
        memcpy(text + used, names[i].name, length);
        used += length;
    }
    text[used] = '\0';
    return text;
}

static QttEffectRow *instantiate_row(
    QttEffectArena *arena, const QttEffectRow *row,
    uint64_t quantified, QttEffectRow *fresh) {
    if (row->kind == EFFECT_EMPTY) return qtt_effect_empty(arena);
    if (row->kind == EFFECT_VARIABLE)
        return row->variable == quantified
            ? fresh : (QttEffectRow *)row;
    QttEffectRow *tail = instantiate_row(
        arena, row->tail, quantified, fresh);
    return tail
        ? (row->has_atom
            ? qtt_effect_extend_atom(arena, &row->atom, tail)
            : qtt_effect_extend(arena, row->label, tail))
        : NULL;
}

QttEffectRow *qtt_effect_instantiate(
    QttEffectArena *arena, const QttEffectScheme *scheme) {
    if (!arena || !scheme) return NULL;
    QttEffectRow *fresh = scheme->quantified_count
        ? qtt_effect_fresh(arena) : NULL;
    return instantiate_row(
        arena, scheme->row, scheme->quantified, fresh);
}

static QttEffectRow *open_closed_row(
    QttEffectArena *arena, const QttEffectRow *row,
    QttEffectRow *replacement_tail) {
    if (!row) return NULL;
    if (row->kind == EFFECT_EMPTY) return replacement_tail;
    if (row->kind != EFFECT_EXTEND) return NULL;
    QttEffectRow *tail = open_closed_row(
        arena, row->tail, replacement_tail);
    return tail
        ? (row->has_atom
            ? qtt_effect_extend_atom(arena, &row->atom, tail)
            : qtt_effect_extend(arena, row->label, tail))
        : NULL;
}

QttEffectRow *qtt_effect_instantiate_with_tail(
    QttEffectArena *arena, const QttEffectScheme *scheme,
    QttEffectRow *replacement_tail) {
    if (!arena || !scheme || !replacement_tail) return NULL;
    if (scheme->quantified_count)
        return instantiate_row(
            arena, scheme->row, scheme->quantified, replacement_tail);
    /* Opening a closed scheme replaces its terminal empty row with the
     * caller-provided existential tail; it does not discard known atoms. */
    return open_closed_row(arena, scheme->row, replacement_tail);
}

void qtt_effect_scheme_free(QttEffectScheme *scheme) {
    if (!scheme) return;
    if (--scheme->references) return;
    qtt_effect_arena_free(scheme->arena);
    free(scheme->portable_evidence);
    free(scheme->callable_parameter_indices);
    free(scheme->callable_parameter_arities);
    free(scheme);
}

uint64_t qtt_effect_tail_variable(const QttEffectRow *row) {
    if (!row) return 0;
    while (row->kind == EFFECT_EXTEND) row = row->tail;
    return row->kind == EFFECT_VARIABLE ? row->variable : 0;
}
