#include "bindings.h"

#include <stdlib.h>
#include <string.h>

typedef struct ScopeEntry {
    const char *name;
    uint64_t id;
    struct ScopeEntry *next;
} ScopeEntry;

typedef struct {
    ScopeEntry *scope;
    uint64_t next_id;
    uint64_t next_closure_id;
    QttBindingSummary summary;
} Resolver;

typedef struct LayoutEntry {
    Type *type;
    struct LayoutEntry *next;
} LayoutEntry;

static LayoutEntry *source_layouts;

static char *binding_copy_text(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
}

static Type *source_field_type(const char *name) {
    if (!name) return NULL;
    Type *type = calloc(1, sizeof(*type));
    if (!type) return NULL;
    if (!strcmp(name, "String")) type->kind = TYPE_STRING;
    else if (!strcmp(name, "Int")) type->kind = TYPE_INT;
    else if (!strcmp(name, "Float")) type->kind = TYPE_FLOAT;
    else if (!strcmp(name, "Bool")) type->kind = TYPE_BOOL;
    else if (!strcmp(name, "Char")) type->kind = TYPE_CHAR;
    else {
        type->kind = TYPE_LAYOUT;
        type->layout_name = binding_copy_text(name);
        if (!type->layout_name) {
            free(type);
            return NULL;
        }
    }
    return type;
}

static void source_field_type_free(Type *type) {
    if (!type) return;
    if (type->kind == TYPE_LAYOUT) free(type->layout_name);
    if (type->kind == TYPE_ARR)
        source_field_type_free(type->arr_element_type);
    free(type);
}

static Type *source_layout_field_type(const ASTLayoutField *field) {
    if (!field) return NULL;
    if (!field->is_array)
        return source_field_type(field->type_name);

    Type *element = source_field_type(field->array_elem);
    if (!element) return NULL;
    Type *array = calloc(1, sizeof(*array));
    if (!array) {
        source_field_type_free(element);
        return NULL;
    }
    array->kind = TYPE_ARR;
    array->arr_element_type = element;
    array->arr_size = field->array_size;
    array->arr_is_fat = true;
    return array;
}

static void clear_source_layouts(void) {
    while (source_layouts) {
        LayoutEntry *next = source_layouts->next;
        Type *type = source_layouts->type;
        for (int i = 0; type && i < type->layout_field_count; i++) {
            free(type->layout_fields[i].name);
            source_field_type_free(type->layout_fields[i].type);
        }
        if (type) free(type->layout_fields);
        if (type) free(type->layout_name);
        free(type);
        free(source_layouts);
        source_layouts = next;
    }
}

const Type *qtt_bindings_layout_type(const char *name) {
    for (LayoutEntry *entry = source_layouts; entry; entry = entry->next)
        if (entry->type && entry->type->layout_name && name &&
            strcmp(entry->type->layout_name, name) == 0)
            return entry->type;
    return NULL;
}

static bool collect_source_layout(AST *form) {
    if (!form || form->type != AST_LAYOUT || !form->layout.name ||
        form->layout.field_count <= 0)
        return true;
    LayoutField *fields = calloc(
        (size_t)form->layout.field_count, sizeof(*fields));
    LayoutEntry *entry = calloc(1, sizeof(*entry));
    if (!fields || !entry) {
        free(fields);
        free(entry);
        return false;
    }
    for (int i = 0; i < form->layout.field_count; i++) {
        fields[i].name = binding_copy_text(form->layout.fields[i].name);
        fields[i].type = source_layout_field_type(&form->layout.fields[i]);
        if (!fields[i].name || !fields[i].type) {
            for (int j = 0; j <= i; j++) {
                free(fields[j].name);
                source_field_type_free(fields[j].type);
            }
            free(fields);
            free(entry);
            return false;
        }
    }
    entry->type = calloc(1, sizeof(*entry->type));
    if (entry->type) {
        entry->type->kind = TYPE_LAYOUT;
        entry->type->layout_name = binding_copy_text(form->layout.name);
        entry->type->layout_fields = fields;
        entry->type->layout_field_count = form->layout.field_count;
        entry->type->layout_packed = form->layout.packed;
        entry->type->layout_align = form->layout.align;
        if (!entry->type->layout_name) {
            free(entry->type);
            entry->type = NULL;
        }
    }
    if (!entry->type) {
        for (int i = 0; i < form->layout.field_count; i++) {
            free(fields[i].name);
            source_field_type_free(fields[i].type);
        }
        free(fields);
        free(entry);
        return false;
    }
    entry->next = source_layouts;
    source_layouts = entry;
    return true;
}

static uint64_t lookup(const Resolver *resolver, const char *name) {
    for (const ScopeEntry *entry = resolver->scope;
         entry;
         entry = entry->next) {
        if (strcmp(entry->name, name) == 0) return entry->id;
    }
    return QTT_BINDER_UNRESOLVED;
}

static uint64_t lookup_place_root(const Resolver *resolver,
                                  const char *name) {
    const char *dot = name ? strchr(name, '.') : NULL;
    if (!dot) return lookup(resolver, name);
    size_t root_length = (size_t)(dot - name);
    for (const ScopeEntry *entry = resolver->scope;
         entry; entry = entry->next)
        if (strlen(entry->name) == root_length &&
            strncmp(entry->name, name, root_length) == 0)
            return entry->id;
    return QTT_BINDER_UNRESOLVED;
}

static bool push(Resolver *resolver, const char *name, uint64_t id) {
    ScopeEntry *entry = malloc(sizeof(*entry));
    if (!entry) {
        resolver->summary.error = QTT_BINDINGS_OUT_OF_MEMORY;
        return false;
    }
    entry->name = name;
    entry->id = id;
    entry->next = resolver->scope;
    resolver->scope = entry;
    return true;
}

static void pop_to(Resolver *resolver, ScopeEntry *saved) {
    while (resolver->scope != saved) {
        ScopeEntry *entry = resolver->scope;
        resolver->scope = entry->next;
        free(entry);
    }
}

static uint64_t fresh(Resolver *resolver) {
    uint64_t id = resolver->next_id++;
    resolver->summary.binder_count++;
    return id;
}

static bool list_head_is(const AST *ast, const char *name) {
    return ast && ast->type == AST_LIST && ast->list.count &&
           ast->list.items[0] &&
           ast->list.items[0]->type == AST_SYMBOL &&
           strcmp(ast->list.items[0]->symbol, name) == 0;
}

static void resolve_ast(Resolver *resolver, AST *ast);

static void bind_pattern(Resolver *resolver, ASTPattern *pattern) {
    if (!pattern || resolver->summary.error != QTT_BINDINGS_OK) return;
    if (pattern->kind == PAT_VAR && pattern->var_name) {
        pattern->binder_id = fresh(resolver);
        push(resolver, pattern->var_name, pattern->binder_id);
    }
    for (int i = 0; i < pattern->element_count; i++)
        bind_pattern(resolver, &pattern->elements[i]);
    if (pattern->tail) bind_pattern(resolver, pattern->tail);
    for (int i = 0; i < pattern->ctor_field_count; i++)
        bind_pattern(resolver, &pattern->ctor_fields[i]);
}

static void resolve_pmatch(Resolver *resolver, AST *ast) {
    for (int i = 0;
         i < ast->pmatch.clause_count &&
         resolver->summary.error == QTT_BINDINGS_OK;
         i++) {
        ASTPMatchClause *clause = &ast->pmatch.clauses[i];
        ScopeEntry *saved = resolver->scope;
        for (int p = 0; p < clause->pattern_count; p++)
            bind_pattern(resolver, &clause->patterns[p]);
        for (int g = 0; g < clause->guard_count; g++) {
            resolve_ast(resolver, clause->guard_conds[g]);
            resolve_ast(resolver, clause->guard_bodies[g]);
        }
        resolve_ast(resolver, clause->body);
        pop_to(resolver, saved);
    }
}

static void resolve_local_bindings(Resolver *resolver, AST *ast) {
    AST *bindings = ast->list.items[1];
    ScopeEntry *saved = resolver->scope;
    for (size_t i = 0;
         i + 1 < bindings->array.element_count &&
         resolver->summary.error == QTT_BINDINGS_OK;
         i += 2) {
        AST *name = bindings->array.elements[i];
        AST *value = bindings->array.elements[i + 1];
        resolve_ast(resolver, value);
        if (!name || name->type != AST_SYMBOL) continue;
        name->resolved_binder_id = fresh(resolver);
        push(resolver, name->symbol, name->resolved_binder_id);
    }
    for (size_t i = 2; i < ast->list.count; i++)
        resolve_ast(resolver, ast->list.items[i]);
    pop_to(resolver, saved);
}

static void resolve_ast(Resolver *resolver, AST *ast) {
    if (!ast || resolver->summary.error != QTT_BINDINGS_OK) return;
    ast->resolved_binder_id = QTT_BINDER_UNRESOLVED;
    ast->qtt_closure_id = 0;

    switch (ast->type) {
    case AST_SYMBOL:
        ast->resolved_binder_id = lookup_place_root(
            resolver, ast->symbol);
        return;

    case AST_LIST:
        if (list_head_is(ast, "quote")) return;
        if ((list_head_is(ast, "with") || list_head_is(ast, "let")) &&
            ast->list.count >= 2 && ast->list.items[1] &&
            ast->list.items[1]->type == AST_ARRAY) {
            resolve_local_bindings(resolver, ast);
            return;
        }
        for (size_t i = 0; i < ast->list.count; i++)
            resolve_ast(resolver, ast->list.items[i]);
        return;

    case AST_LAMBDA: {
        ast->qtt_closure_id = resolver->next_closure_id++;
        resolver->summary.closure_count++;
        ScopeEntry *saved = resolver->scope;
        for (int i = 0; i < ast->lambda.param_count; i++) {
            ASTParam *param = &ast->lambda.params[i];
            param->binder_id = fresh(resolver);
            if (!push(resolver, param->name, param->binder_id)) break;
        }
        for (int i = 0; i < ast->lambda.body_count; i++)
            resolve_ast(resolver, ast->lambda.body_exprs[i]);
        if (ast->lambda.pattern_match)
            resolve_ast(resolver, ast->lambda.pattern_match);
        pop_to(resolver, saved);
        return;
    }

    case AST_ARRAY:
        for (size_t i = 0; i < ast->array.element_count; i++)
            resolve_ast(resolver, ast->array.elements[i]);
        return;
    case AST_SET:
        for (size_t i = 0; i < ast->set.element_count; i++)
            resolve_ast(resolver, ast->set.elements[i]);
        return;
    case AST_MAP:
        for (size_t i = 0; i < ast->map.count; i++) {
            resolve_ast(resolver, ast->map.keys[i]);
            resolve_ast(resolver, ast->map.vals[i]);
        }
        return;
    case AST_RANGE:
        resolve_ast(resolver, ast->range.start);
        resolve_ast(resolver, ast->range.step);
        resolve_ast(resolver, ast->range.end);
        return;
    case AST_ADDRESS_OF:
        if (ast->list.count) resolve_ast(resolver, ast->list.items[0]);
        return;
    case AST_REFINEMENT:
        resolve_ast(resolver, ast->refinement.predicate);
        return;
    case AST_TESTS:
        for (int i = 0; i < ast->tests.count; i++)
            resolve_ast(resolver, ast->tests.assertions[i]);
        return;
    case AST_PMATCH:
        resolve_pmatch(resolver, ast);
        return;
    case AST_CLASS:
        for (int i = 0; i < ast->class_decl.default_count; i++)
            resolve_ast(resolver, ast->class_decl.default_bodies[i]);
        for (int i = 0; i < ast->class_decl.law_count; i++)
            resolve_ast(resolver, ast->class_decl.law_bodies[i]);
        return;
    case AST_INSTANCE:
        for (int i = 0; i < ast->instance_decl.method_count; i++)
            resolve_ast(resolver, ast->instance_decl.method_bodies[i]);
        return;
    default:
        return;
    }
}

QttBindingSummary qtt_bindings_resolve(AST *root) {
    return qtt_bindings_resolve_many(&root, 1);
}

QttBindingSummary qtt_bindings_resolve_many(AST **roots, size_t count) {
    clear_source_layouts();
    Resolver resolver = {0};
    resolver.next_id = UINT64_C(1);
    resolver.next_closure_id = UINT64_C(1);
    resolver.summary.error = QTT_BINDINGS_OK;
    for (size_t i = 0; i < count; i++)
        if (!collect_source_layout(roots ? roots[i] : NULL)) {
            resolver.summary.error = QTT_BINDINGS_OUT_OF_MEMORY;
            return resolver.summary;
        }
    for (size_t i = 0;
         i < count && resolver.summary.error == QTT_BINDINGS_OK;
         i++)
        resolve_ast(&resolver, roots[i]);
    pop_to(&resolver, NULL);
    return resolver.summary;
}
