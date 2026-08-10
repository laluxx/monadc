#include "type_identity.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const Type *type;
    uint64_t fingerprint;
} QttTypeEntry;

struct QttTypeArena {
    QttTypeEntry *entries;
    size_t count;
    size_t capacity;
};

static bool text_equal(const char *left, const char *right) {
    if (left == right) return true;
    return left && right && strcmp(left, right) == 0;
}

static bool type_equal_at(const Type *left, const Type *right,
                          unsigned depth) {
    if (left == right) return true;
    if (!left || !right || depth > 128 || left->kind != right->kind)
        return false;
    switch (left->kind) {
    case TYPE_VAR:
        return left->var_id == right->var_id;
    case TYPE_ARROW:
        return left->arrow_effect_complete == right->arrow_effect_complete &&
               text_equal(left->arrow_effect_name,
                          right->arrow_effect_name) &&
               text_equal(left->arrow_effect_scheme,
                          right->arrow_effect_scheme) &&
               type_equal_at(left->arrow_param, right->arrow_param, depth + 1) &&
               type_equal_at(left->arrow_ret, right->arrow_ret, depth + 1);
    case TYPE_FN:
        if (left->param_count != right->param_count ||
            !type_equal_at(left->return_type, right->return_type, depth + 1))
            return false;
        for (int i = 0; i < left->param_count; i++)
            if (left->params[i].optional != right->params[i].optional ||
                left->params[i].rest != right->params[i].rest ||
                !type_equal_at(left->params[i].type,
                               right->params[i].type, depth + 1))
                return false;
        return true;
    case TYPE_LIST:
        if (left->list_count != right->list_count) return false;
        for (int i = 0; i < left->list_count; i++)
            if (!type_equal_at(left->list_types[i],
                               right->list_types[i], depth + 1))
                return false;
        return true;
    case TYPE_OPTIONAL:
    case TYPE_PTR:
    case TYPE_COLL:
    case TYPE_VARIADIC:
        return type_equal_at(
            left->element_type, right->element_type, depth + 1);
    case TYPE_ARR:
        return left->arr_size == right->arr_size &&
               left->arr_is_fat == right->arr_is_fat &&
               left->arr_is_heap == right->arr_is_heap &&
               type_equal_at(left->arr_element_type,
                             right->arr_element_type, depth + 1);
    case TYPE_MAP:
        return type_equal_at(left->map_key_type, right->map_key_type,
                             depth + 1) &&
               type_equal_at(left->map_value_type, right->map_value_type,
                             depth + 1);
    case TYPE_LAYOUT:
        if (!text_equal(left->layout_name, right->layout_name) ||
            left->layout_field_count != right->layout_field_count ||
            left->layout_total_size != right->layout_total_size ||
            left->layout_packed != right->layout_packed ||
            left->layout_align != right->layout_align ||
            left->layout_is_scalar != right->layout_is_scalar ||
            left->layout_is_inline != right->layout_is_inline)
            return false;
        for (int i = 0; i < left->layout_field_count; i++)
            if (!text_equal(left->layout_fields[i].name,
                            right->layout_fields[i].name) ||
                left->layout_fields[i].offset !=
                    right->layout_fields[i].offset ||
                left->layout_fields[i].size !=
                    right->layout_fields[i].size ||
                !type_equal_at(left->layout_fields[i].type,
                               right->layout_fields[i].type, depth + 1))
                return false;
        return true;
    case TYPE_INT_ARBITRARY:
        return left->numeric_width == right->numeric_width &&
               left->numeric_signed == right->numeric_signed;
    case TYPE_APP:
        return text_equal(left->app_constructor, right->app_constructor) &&
               type_equal_at(left->app_arg, right->app_arg, depth + 1);
    case TYPE_FINITE_SET:
        return text_equal(left->finite_name, right->finite_name) &&
               left->finite_member_count == right->finite_member_count;
    default:
        return true;
    }
}

static uint64_t hash_mix(uint64_t hash, uint64_t value) {
    hash ^= value;
    return hash * UINT64_C(1099511628211);
}

static uint64_t hash_text(uint64_t hash, const char *text) {
    if (!text) return hash_mix(hash, 0);
    for (; *text; text++) hash = hash_mix(hash, (unsigned char)*text);
    return hash_mix(hash, UINT64_C(0xff));
}

static uint64_t fingerprint_at(const Type *type, unsigned depth) {
    if (!type || depth > 128) return UINT64_C(0x9e3779b97f4a7c15);
    uint64_t hash = hash_mix(
        UINT64_C(1469598103934665603), (uint64_t)type->kind + 1);
    switch (type->kind) {
    case TYPE_VAR:
        return hash_mix(hash, (uint64_t)(uint32_t)type->var_id);
    case TYPE_ARROW:
        hash = hash_text(hash, type->arrow_effect_name);
        hash = hash_text(hash, type->arrow_effect_scheme);
        hash = hash_mix(hash, type->arrow_effect_complete);
        hash = hash_mix(hash, fingerprint_at(type->arrow_param, depth + 1));
        return hash_mix(hash, fingerprint_at(type->arrow_ret, depth + 1));
    case TYPE_FN:
        if (type->param_count < 0 ||
            (type->param_count && !type->params)) return false;
        hash = hash_mix(hash, (uint64_t)type->param_count);
        for (int i = 0; i < type->param_count; i++) {
            hash = hash_mix(hash, type->params[i].optional);
            hash = hash_mix(hash, type->params[i].rest);
            hash = hash_mix(
                hash, fingerprint_at(type->params[i].type, depth + 1));
        }
        return hash_mix(hash, fingerprint_at(type->return_type, depth + 1));
    case TYPE_LIST:
        hash = hash_mix(hash, (uint64_t)type->list_count);
        for (int i = 0; i < type->list_count; i++)
            hash = hash_mix(
                hash, fingerprint_at(type->list_types[i], depth + 1));
        return hash;
    case TYPE_OPTIONAL:
    case TYPE_PTR:
    case TYPE_COLL:
    case TYPE_VARIADIC:
        return hash_mix(
            hash, fingerprint_at(type->element_type, depth + 1));
    case TYPE_ARR:
        hash = hash_mix(hash, (uint64_t)type->arr_size);
        hash = hash_mix(hash, type->arr_is_fat);
        hash = hash_mix(hash, type->arr_is_heap);
        return hash_mix(
            hash, fingerprint_at(type->arr_element_type, depth + 1));
    case TYPE_MAP:
        hash = hash_mix(
            hash, fingerprint_at(type->map_key_type, depth + 1));
        return hash_mix(
            hash, fingerprint_at(type->map_value_type, depth + 1));
    case TYPE_LAYOUT:
        if (type->layout_field_count < 0 ||
            (type->layout_field_count && !type->layout_fields)) return false;
        hash = hash_text(hash, type->layout_name);
        hash = hash_mix(hash, (uint64_t)type->layout_field_count);
        hash = hash_mix(hash, (uint64_t)type->layout_total_size);
        hash = hash_mix(hash, type->layout_packed);
        hash = hash_mix(hash, (uint64_t)type->layout_align);
        hash = hash_mix(hash, type->layout_is_scalar);
        hash = hash_mix(hash, type->layout_is_inline);
        for (int i = 0; i < type->layout_field_count; i++) {
            hash = hash_text(hash, type->layout_fields[i].name);
            hash = hash_mix(
                hash, (uint64_t)type->layout_fields[i].offset);
            hash = hash_mix(hash, (uint64_t)type->layout_fields[i].size);
            hash = hash_mix(
                hash, fingerprint_at(
                    type->layout_fields[i].type, depth + 1));
        }
        return hash;
    case TYPE_INT_ARBITRARY:
        hash = hash_mix(hash, (uint64_t)type->numeric_width);
        return hash_mix(hash, type->numeric_signed);
    case TYPE_APP:
        hash = hash_text(hash, type->app_constructor);
        return hash_mix(hash, fingerprint_at(type->app_arg, depth + 1));
    case TYPE_FINITE_SET:
        hash = hash_text(hash, type->finite_name);
        return hash_mix(hash, type->finite_member_count);
    default:
        return hash;
    }
}

QttTypeArena *qtt_type_arena_new(void) {
    return calloc(1, sizeof(QttTypeArena));
}

void qtt_type_arena_free(QttTypeArena *arena) {
    if (!arena) return;
    free(arena->entries);
    free(arena);
}

size_t qtt_type_arena_count(const QttTypeArena *arena) {
    return arena ? arena->count : 0;
}

QttTypeId qtt_type_intern_with_fingerprint(
    QttTypeArena *arena,
    const Type *type,
    uint64_t fingerprint,
    QttTypeIdentityError *error) {
    if (!arena || !type) {
        if (error) *error = QTT_TYPE_IDENTITY_UNSUPPORTED_RECURSION;
        return (QttTypeId){0};
    }
    for (size_t i = 0; i < arena->count; i++)
        if (arena->entries[i].fingerprint == fingerprint &&
            type_equal_at(arena->entries[i].type, type, 0)) {
            if (error) *error = QTT_TYPE_IDENTITY_OK;
            return (QttTypeId){i + 1};
        }
    if (arena->count == arena->capacity) {
        size_t next = arena->capacity ? arena->capacity * 2 : 16;
        QttTypeEntry *grown =
            realloc(arena->entries, next * sizeof(*grown));
        if (!grown) {
            if (error) *error = QTT_TYPE_IDENTITY_OUT_OF_MEMORY;
            return (QttTypeId){0};
        }
        arena->entries = grown;
        arena->capacity = next;
    }
    arena->entries[arena->count] = (QttTypeEntry){type, fingerprint};
    arena->count++;
    if (error) *error = QTT_TYPE_IDENTITY_OK;
    return (QttTypeId){arena->count};
}

QttTypeId qtt_type_intern(
    QttTypeArena *arena,
    const Type *type,
    QttTypeIdentityError *error) {
    return qtt_type_intern_with_fingerprint(
        arena, type, fingerprint_at(type, 0), error);
}

const Type *qtt_type_lookup(const QttTypeArena *arena, QttTypeId id) {
    if (!arena || !id.value || id.value > arena->count) return NULL;
    return arena->entries[id.value - 1].type;
}

bool qtt_type_id_equal(QttTypeId left, QttTypeId right) {
    return left.value && left.value == right.value;
}

uint64_t qtt_type_fingerprint(const Type *type) {
    return fingerprint_at(type, 0);
}

typedef struct {
    char *text;
    size_t length;
    size_t capacity;
    bool valid;
} TypeText;

static bool type_text_append(TypeText *out, const char *text) {
    if (!out->valid) return false;
    size_t added = strlen(text);
    if (out->length + added + 1 > out->capacity) {
        size_t next = out->capacity ? out->capacity : 128;
        while (next < out->length + added + 1) next *= 2;
        char *grown = realloc(out->text, next);
        if (!grown) return out->valid = false;
        out->text = grown;
        out->capacity = next;
    }
    memcpy(out->text + out->length, text, added + 1);
    out->length += added;
    return true;
}

static bool type_text_number(TypeText *out, int64_t value) {
    char text[48];
    snprintf(text, sizeof(text), "%lld", (long long)value);
    return type_text_append(out, text);
}

static bool type_text_hex(TypeText *out, const char *text) {
    static const char digits[] = "0123456789abcdef";
    if (!text || !*text) return false;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        char pair[3] = {digits[*p >> 4], digits[*p & 15], '\0'};
        if (!type_text_append(out, pair)) return false;
    }
    return true;
}

static bool type_encode_at(TypeText *out, const Type *type, unsigned depth) {
    if (!type || depth > 128) return false;
    char head[32];
    snprintf(head, sizeof(head), "k%d", (int)type->kind);
    if (!type_text_append(out, head)) return false;
    switch (type->kind) {
    case TYPE_VAR:
        return type_text_append(out, ":") &&
            type_text_number(out, type->var_id) && type_text_append(out, ";");
    case TYPE_ARROW:
        if (type->arrow_effect_name &&
            (!type_text_append(out, "@") ||
             !type_text_hex(out, type->arrow_effect_name) ||
             !type_text_append(out, ";")))
            return false;
        if (type->arrow_effect_scheme &&
            (!type_text_append(out,
                 type->arrow_effect_complete ? "!1:" : "!0:") ||
             !type_text_hex(out, type->arrow_effect_scheme) ||
             !type_text_append(out, ";")))
            return false;
        return type_text_append(out, "(") &&
            type_encode_at(out, type->arrow_param, depth + 1) &&
            type_text_append(out, ")(") &&
            type_encode_at(out, type->arrow_ret, depth + 1) &&
            type_text_append(out, ")");
    case TYPE_OPTIONAL: case TYPE_PTR: case TYPE_COLL: case TYPE_VARIADIC:
        return type_text_append(out, "(") &&
            type_encode_at(out, type->element_type, depth + 1) &&
            type_text_append(out, ")");
    case TYPE_MAP:
        return type_text_append(out, "(") &&
            type_encode_at(out, type->map_key_type, depth + 1) &&
            type_text_append(out, ")(") &&
            type_encode_at(out, type->map_value_type, depth + 1) &&
            type_text_append(out, ")");
    case TYPE_ARR:
        return type_text_append(out, ":") &&
            type_text_number(out, type->arr_size) &&
            type_text_append(out, type->arr_is_fat ? ":1" : ":0") &&
            type_text_append(out, type->arr_is_heap ? ":1(" : ":0(") &&
            type_encode_at(out, type->arr_element_type, depth + 1) &&
            type_text_append(out, ")");
    case TYPE_LIST:
        if (type->list_count < 0 ||
            (type->list_count && !type->list_types)) return false;
        if (!type_text_append(out, ":") ||
            !type_text_number(out, type->list_count) ||
            !type_text_append(out, ";")) return false;
        for (int i = 0; i < type->list_count; i++)
            if (!type_text_append(out, "(") ||
                !type_encode_at(out, type->list_types[i], depth + 1) ||
                !type_text_append(out, ")")) return false;
        return true;
    case TYPE_FN:
        if (type->param_count < 0 ||
            (type->param_count && !type->params)) return false;
        if (!type_text_append(out, ":") ||
            !type_text_number(out, type->param_count) ||
            !type_text_append(out, ";")) return false;
        for (int i = 0; i < type->param_count; i++) {
            if (!type_text_append(out, type->params[i].optional ? "1:" : "0:") ||
                !type_text_append(out, type->params[i].rest ? "1(" : "0(") ||
                !type_encode_at(out, type->params[i].type, depth + 1) ||
                !type_text_append(out, ")")) return false;
        }
        return type_text_append(out, "(") &&
            type_encode_at(out, type->return_type, depth + 1) &&
            type_text_append(out, ")");
    case TYPE_APP:
        return type_text_append(out, ":") &&
            type_text_hex(out, type->app_constructor) &&
            type_text_append(out, "(") &&
            type_encode_at(out, type->app_arg, depth + 1) &&
            type_text_append(out, ")");
    case TYPE_FINITE_SET:
        return type_text_append(out, ":") &&
            type_text_hex(out, type->finite_name) &&
            type_text_append(out, ":") &&
            type_text_number(out, (int64_t)type->finite_member_count) &&
            type_text_append(out, ";");
    case TYPE_LAYOUT:
        if (type->layout_field_count < 0 ||
            (type->layout_field_count && !type->layout_fields)) return false;
        if (!type_text_append(out, ":") ||
            !type_text_hex(out, type->layout_name) ||
            !type_text_append(out, ":") ||
            !type_text_number(out, type->layout_field_count) ||
            !type_text_append(out, ":") ||
            !type_text_number(out, type->layout_total_size) ||
            !type_text_append(out, type->layout_packed ? ":1" : ":0") ||
            !type_text_append(out, ":") ||
            !type_text_number(out, type->layout_align) ||
            !type_text_append(out, type->layout_is_scalar ? ":1" : ":0") ||
            !type_text_append(out, type->layout_is_inline ? ":1;" : ":0;"))
            return false;
        for (int i = 0; i < type->layout_field_count; i++) {
            LayoutField *field = &type->layout_fields[i];
            if (!type_text_append(out, "(") ||
                !type_text_hex(out, field->name) ||
                !type_text_append(out, ":") ||
                !type_text_number(out, field->offset) ||
                !type_text_append(out, ":") ||
                !type_text_number(out, field->size) ||
                !type_text_append(out, "(") ||
                !type_encode_at(out, field->type, depth + 1) ||
                !type_text_append(out, "))")) return false;
        }
        return true;
    case TYPE_INT_ARBITRARY:
        return type_text_append(out, ":") &&
            type_text_number(out, type->numeric_width) &&
            type_text_append(out, type->numeric_signed ? ":1;" : ":0;");
    case TYPE_UNKNOWN:
        return false;
    default:
        return type_text_append(out, ";");
    }
}

char *qtt_type_serialize(const Type *type) {
    TypeText out = {.valid = true};
    if (!type_encode_at(&out, type, 0)) {
        free(out.text);
        return NULL;
    }
    return out.text;
}

void qtt_type_free_owned(Type *type) {
    if (!type) return;
    qtt_type_free_owned(type->arrow_param);
    qtt_type_free_owned(type->arrow_ret);
    if (type->arrow_effect_scheme_owned)
        free(type->arrow_effect_scheme);
    free(type->arrow_effect_name);
    qtt_type_free_owned(type->element_type);
    qtt_type_free_owned(type->arr_element_type);
    qtt_type_free_owned(type->map_key_type);
    qtt_type_free_owned(type->map_value_type);
    for (int i = 0; i < type->list_count; i++)
        qtt_type_free_owned(type->list_types[i]);
    free(type->list_types);
    for (int i = 0; i < type->param_count; i++) {
        free(type->params[i].name);
        qtt_type_free_owned(type->params[i].type);
    }
    free(type->params);
    qtt_type_free_owned(type->return_type);
    for (int i = 0; i < type->layout_field_count; i++) {
        free(type->layout_fields[i].name);
        qtt_type_free_owned(type->layout_fields[i].type);
    }
    free(type->layout_fields);
    free(type->layout_name);
    free(type->app_constructor);
    qtt_type_free_owned(type->app_arg);
    free(type->finite_name);
    free(type);
}

static int type_hex_digit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

static char *type_parse_hex(const char **cursor, char end) {
    const char *start = *cursor;
    while (**cursor && **cursor != end) (*cursor)++;
    if (**cursor != end) return NULL;
    size_t length = (size_t)(*cursor - start);
    (*cursor)++;
    if (!length || (length & 1u)) return NULL;
    char *text = malloc(length / 2 + 1);
    if (!text) return NULL;
    for (size_t i = 0; i < length; i += 2) {
        int high = type_hex_digit(start[i]);
        int low = type_hex_digit(start[i + 1]);
        if (high < 0 || low < 0) { free(text); return NULL; }
        text[i / 2] = (char)((high << 4) | low);
    }
    text[length / 2] = '\0';
    return text;
}

static bool type_parse_int(const char **cursor, int64_t *value, char end) {
    char *after = NULL;
    long long parsed = strtoll(*cursor, &after, 10);
    if (after == *cursor || *after != end) return false;
    *value = (int64_t)parsed;
    *cursor = after + 1;
    return true;
}

static Type *type_decode_at(const char **cursor, unsigned depth);

static Type *type_decode_child(const char **cursor, unsigned depth) {
    if (**cursor != '(') return NULL;
    (*cursor)++;
    Type *child = type_decode_at(cursor, depth + 1);
    if (!child || **cursor != ')') {
        qtt_type_free_owned(child);
        return NULL;
    }
    (*cursor)++;
    return child;
}

static Type *type_decode_at(const char **cursor, unsigned depth) {
    if (!cursor || !*cursor || depth > 128 || (*cursor)[0] != 'k') return NULL;
    (*cursor)++;
    int64_t kind_value = 0;
    char *after = NULL;
    kind_value = strtoll(*cursor, &after, 10);
    if (after == *cursor || kind_value < 0 || kind_value > TYPE_FINITE_SET)
        return NULL;
    *cursor = after;
    Type *type = calloc(1, sizeof(*type));
    if (!type) return NULL;
    type->kind = (TypeKind)kind_value;
    switch (type->kind) {
    case TYPE_VAR: {
        int64_t id = 0;
        if (**cursor != ':') goto fail;
        (*cursor)++;
        if (!type_parse_int(cursor, &id, ';') || id < 0 || id > INT32_MAX)
            goto fail;
        type->var_id = (int)id;
        return type;
    }
    case TYPE_ARROW:
        if (**cursor == '@') {
            (*cursor)++;
            type->arrow_effect_name = type_parse_hex(cursor, ';');
            if (!type->arrow_effect_name) goto fail;
        }
        if (**cursor == '!') {
            (*cursor)++;
            if (((*cursor)[0] != '0' && (*cursor)[0] != '1') ||
                (*cursor)[1] != ':')
                goto fail;
            type->arrow_effect_complete = (*cursor)[0] == '1';
            *cursor += 2;
            type->arrow_effect_scheme = type_parse_hex(cursor, ';');
            if (!type->arrow_effect_scheme) goto fail;
            type->arrow_effect_scheme_owned = true;
        }
        type->arrow_param = type_decode_child(cursor, depth);
        type->arrow_ret = type_decode_child(cursor, depth);
        if (!type->arrow_param || !type->arrow_ret) goto fail;
        return type;
    case TYPE_OPTIONAL: case TYPE_PTR: case TYPE_COLL: case TYPE_VARIADIC:
        type->element_type = type_decode_child(cursor, depth);
        if (!type->element_type) goto fail;
        return type;
    case TYPE_MAP:
        type->map_key_type = type_decode_child(cursor, depth);
        type->map_value_type = type_decode_child(cursor, depth);
        if (!type->map_key_type || !type->map_value_type) goto fail;
        return type;
    case TYPE_ARR: {
        int64_t size = 0, fat = 0, heap = 0;
        if (**cursor != ':') goto fail;
        (*cursor)++;
        if (!type_parse_int(cursor, &size, ':') ||
            !type_parse_int(cursor, &fat, ':') ||
            !type_parse_int(cursor, &heap, '(') ||
            (fat != 0 && fat != 1) || (heap != 0 && heap != 1)) goto fail;
        type->arr_size = size;
        type->arr_is_fat = fat != 0;
        type->arr_is_heap = heap != 0;
        type->arr_element_type = type_decode_at(cursor, depth + 1);
        if (!type->arr_element_type || **cursor != ')') goto fail;
        (*cursor)++;
        return type;
    }
    case TYPE_LIST: {
        int64_t count = 0;
        if (**cursor != ':') goto fail;
        (*cursor)++;
        if (!type_parse_int(cursor, &count, ';') || count < 0 || count > 1024)
            goto fail;
        type->list_count = (int)count;
        type->list_types = count
            ? calloc((size_t)count, sizeof(*type->list_types)) : NULL;
        if (count && !type->list_types) goto fail;
        for (int i = 0; i < type->list_count; i++) {
            type->list_types[i] = type_decode_child(cursor, depth);
            if (!type->list_types[i]) goto fail;
        }
        return type;
    }
    case TYPE_FN: {
        int64_t count = 0;
        if (**cursor != ':') goto fail;
        (*cursor)++;
        if (!type_parse_int(cursor, &count, ';') || count < 0 || count > 1024)
            goto fail;
        type->param_count = (int)count;
        type->params = count ? calloc((size_t)count, sizeof(*type->params)) : NULL;
        if (count && !type->params) goto fail;
        for (int i = 0; i < type->param_count; i++) {
            int64_t optional = 0, rest = 0;
            if (!type_parse_int(cursor, &optional, ':') ||
                !type_parse_int(cursor, &rest, '(') ||
                (optional != 0 && optional != 1) ||
                (rest != 0 && rest != 1)) goto fail;
            type->params[i].optional = optional != 0;
            type->params[i].rest = rest != 0;
            type->params[i].type = type_decode_at(cursor, depth + 1);
            if (!type->params[i].type || **cursor != ')') goto fail;
            (*cursor)++;
        }
        type->return_type = type_decode_child(cursor, depth);
        if (!type->return_type) goto fail;
        return type;
    }
    case TYPE_APP:
        if (**cursor != ':') goto fail;
        (*cursor)++;
        type->app_constructor = type_parse_hex(cursor, '(');
        if (!type->app_constructor) goto fail;
        type->app_arg = type_decode_at(cursor, depth + 1);
        if (!type->app_arg || **cursor != ')') goto fail;
        (*cursor)++;
        return type;
    case TYPE_FINITE_SET: {
        int64_t count = 0;
        if (**cursor != ':') goto fail;
        (*cursor)++;
        type->finite_name = type_parse_hex(cursor, ':');
        if (!type->finite_name ||
            !type_parse_int(cursor, &count, ';') || count < 0)
            goto fail;
        type->finite_member_count = (size_t)count;
        return type;
    }
    case TYPE_LAYOUT: {
        int64_t count = 0, total = 0, packed = 0, align = 0;
        int64_t scalar = 0, inlined = 0;
        if (**cursor != ':') goto fail;
        (*cursor)++;
        type->layout_name = type_parse_hex(cursor, ':');
        if (!type->layout_name ||
            !type_parse_int(cursor, &count, ':') || count < 0 || count > 1024 ||
            !type_parse_int(cursor, &total, ':') || total < 0 ||
            !type_parse_int(cursor, &packed, ':') ||
            !type_parse_int(cursor, &align, ':') || align < 0 ||
            !type_parse_int(cursor, &scalar, ':') ||
            !type_parse_int(cursor, &inlined, ';') ||
            (packed != 0 && packed != 1) ||
            (scalar != 0 && scalar != 1) ||
            (inlined != 0 && inlined != 1)) goto fail;
        type->layout_field_count = (int)count;
        type->layout_total_size = (int)total;
        type->layout_packed = packed != 0;
        type->layout_align = (int)align;
        type->layout_is_scalar = scalar != 0;
        type->layout_is_inline = inlined != 0;
        type->layout_fields = count
            ? calloc((size_t)count, sizeof(*type->layout_fields)) : NULL;
        if (count && !type->layout_fields) goto fail;
        for (int i = 0; i < type->layout_field_count; i++) {
            int64_t offset = 0, size = 0;
            if (**cursor != '(') goto fail;
            (*cursor)++;
            LayoutField *field = &type->layout_fields[i];
            field->name = type_parse_hex(cursor, ':');
            if (!field->name ||
                !type_parse_int(cursor, &offset, ':') ||
                !type_parse_int(cursor, &size, '(')) goto fail;
            field->offset = (int)offset;
            field->size = (int)size;
            field->type = type_decode_at(cursor, depth + 1);
            if (!field->type || (*cursor)[0] != ')' || (*cursor)[1] != ')')
                goto fail;
            *cursor += 2;
        }
        return type;
    }
    case TYPE_INT_ARBITRARY: {
        int64_t width = 0, sign = 0;
        if (**cursor != ':') goto fail;
        (*cursor)++;
        if (!type_parse_int(cursor, &width, ':') ||
            !type_parse_int(cursor, &sign, ';') || width <= 0 ||
            width > INT32_MAX || (sign != 0 && sign != 1)) goto fail;
        type->numeric_width = (int)width;
        type->numeric_signed = sign != 0;
        return type;
    }
    default:
        if (**cursor != ';') goto fail;
        (*cursor)++;
        return type;
    }
fail:
    qtt_type_free_owned(type);
    return NULL;
}

Type *qtt_type_deserialize(const char *text) {
    if (!text || !*text) return NULL;
    const char *cursor = text;
    Type *type = type_decode_at(&cursor, 0);
    if (!type || *cursor) {
        qtt_type_free_owned(type);
        return NULL;
    }
    return type;
}
