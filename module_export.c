#include "module_export.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *dup(const char *s) {
    if (!s) return NULL;
    char *r = malloc(strlen(s) + 1);
    strcpy(r, s);
    return r;
}

char *manifest_mangle(const char *module_name, const char *local_name) {
    // Replace dots with underscores, then double-underscore separator
    size_t mlen = strlen(module_name);
    size_t llen = strlen(local_name);
    char *m = malloc(mlen + 2 + llen + 1);
    // copy module name, replacing '.' with '_'
    for (size_t i = 0; i < mlen; i++) {
        m[i] = (module_name[i] == '.') ? '_' : module_name[i];
    }
    m[mlen]     = '_';
    m[mlen + 1] = '_';
    memcpy(m + mlen + 2, local_name, llen + 1);
    return m;
}

ExportManifest *manifest_create(const char *module_name) {
    ExportManifest *m = calloc(1, sizeof(ExportManifest));
    m->module_name = dup(module_name);
    m->entry_capacity = 8;
    m->entries = malloc(sizeof(ExportEntry) * m->entry_capacity);
    return m;
}

void manifest_free(ExportManifest *m) {
    if (!m) return;
    for (size_t i = 0; i < m->entry_count; i++) {
        free(m->entries[i].mangled_name);
        free(m->entries[i].local_name);
        if (m->entries[i].params) {
            for (int j = 0; j < m->entries[i].param_count; j++)
                free(m->entries[i].params[j].name);
            free(m->entries[i].params);
        }
    }
    free(m->entries);
    free(m->module_name);
    free(m);
}

static ExportEntry *new_entry(ExportManifest *m) {
    if (m->entry_count >= m->entry_capacity) {
        m->entry_capacity *= 2;
        m->entries = realloc(m->entries, sizeof(ExportEntry) * m->entry_capacity);
    }
    ExportEntry *e = &m->entries[m->entry_count++];
    memset(e, 0, sizeof(*e));
    return e;
}

void manifest_add_var(ExportManifest *m, const char *local_name,
                      const char *mangled_name, int type_kind,
                      int arr_size, int arr_elem_kind) {
    ExportEntry *e = new_entry(m);
    e->kind         = EXPORT_ENTRY_VAR;
    e->local_name   = dup(local_name);
    e->mangled_name = dup(mangled_name);
    e->type_kind    = type_kind;
    e->arr_size     = arr_size;
    e->arr_elem_kind = arr_elem_kind;
}

void manifest_add_func(ExportManifest *m, const char *local_name,
                       const char *mangled_name, int ret_kind,
                       ExportParamDesc *params, int param_count) {
    ExportEntry *e = new_entry(m);
    e->kind         = EXPORT_ENTRY_FUNC;
    e->local_name   = dup(local_name);
    e->mangled_name = dup(mangled_name);
    e->type_kind    = ret_kind;
    e->param_count  = param_count;
    if (param_count > 0) {
        e->params = malloc(sizeof(ExportParamDesc) * param_count);
        for (int i = 0; i < param_count; i++) {
            e->params[i].name      = dup(params[i].name);
            e->params[i].type_kind = params[i].type_kind;
        }
    }
}

bool manifest_write(ExportManifest *m, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return false;

    fprintf(f, "MODULE %s\n", m->module_name);
    for (size_t i = 0; i < m->entry_count; i++) {
        ExportEntry *e = &m->entries[i];
        if (e->kind == EXPORT_ENTRY_VAR) {
            fprintf(f, "VAR %s %s %d %d %d\n",
                    e->local_name, e->mangled_name,
                    e->type_kind, e->arr_size, e->arr_elem_kind);
        } else {
            fprintf(f, "FUNC %s %s %d %d",
                    e->local_name, e->mangled_name,
                    e->type_kind, e->param_count);
            for (int j = 0; j < e->param_count; j++) {
                fprintf(f, " %s %d",
                        e->params[j].name ? e->params[j].name : "_",
                        e->params[j].type_kind);
            }
            fprintf(f, "\n");
        }
    }
    fclose(f);
    return true;
}

// Helper: get next whitespace-delimited token from *p, NUL-terminate it in
// buf (max buf_size), advance *p past it and any trailing spaces.
// Returns true if a token was found.
static bool next_token(char **p, char *buf, int buf_size) {
    // skip leading whitespace
    while (**p == ' ' || **p == '\t') (*p)++;
    if (**p == '\0') return false;
    int i = 0;
    while (**p && **p != ' ' && **p != '\t' && **p != '\n' && i < buf_size - 1) {
        buf[i++] = **p;
        (*p)++;
    }
    buf[i] = '\0';
    return i > 0;
}

ExportManifest *manifest_read(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    char line[2048];
    ExportManifest *m = NULL;

    while (fgets(line, sizeof(line), f)) {
        // Strip trailing newline / carriage return
        size_t l = strlen(line);
        while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';
        if (l == 0) continue;

        char *p = line;
        char keyword[32];
        if (!next_token(&p, keyword, sizeof(keyword))) continue;

        if (strcmp(keyword, "MODULE") == 0) {
            char name[256];
            if (next_token(&p, name, sizeof(name)))
                m = manifest_create(name);

        } else if (m && strcmp(keyword, "VAR") == 0) {
            char local[256], mangled[256], tk_s[32], as_s[32], ae_s[32];
            if (next_token(&p, local,   sizeof(local))   &&
                next_token(&p, mangled, sizeof(mangled)) &&
                next_token(&p, tk_s,   sizeof(tk_s))    &&
                next_token(&p, as_s,   sizeof(as_s))    &&
                next_token(&p, ae_s,   sizeof(ae_s))) {
                manifest_add_var(m, local, mangled,
                                 atoi(tk_s), atoi(as_s), atoi(ae_s));
            }

        } else if (m && strcmp(keyword, "FUNC") == 0) {
            char local[256], mangled[256], rk_s[32], pc_s[32];
            if (!next_token(&p, local,   sizeof(local)))   continue;
            if (!next_token(&p, mangled, sizeof(mangled))) continue;
            if (!next_token(&p, rk_s,   sizeof(rk_s)))    continue;
            if (!next_token(&p, pc_s,   sizeof(pc_s)))    continue;

            int ret_kind    = atoi(rk_s);
            int param_count = atoi(pc_s);

            ExportParamDesc *params = NULL;
            if (param_count > 0) {
                params = malloc(sizeof(ExportParamDesc) * param_count);
                for (int i = 0; i < param_count; i++) {
                    char pname[128], pkind_s[32];
                    if (next_token(&p, pname,   sizeof(pname)) &&
                        next_token(&p, pkind_s, sizeof(pkind_s))) {
                        params[i].name      = strdup(pname);
                        params[i].type_kind = atoi(pkind_s);
                    } else {
                        // Malformed line — fill with defaults
                        params[i].name      = strdup("_");
                        params[i].type_kind = 0;
                    }
                }
            }

            manifest_add_func(m, local, mangled, ret_kind, params, param_count);

            if (params) {
                for (int i = 0; i < param_count; i++) free(params[i].name);
                free(params);
            }
        }
    }

    fclose(f);
    return m;
}
