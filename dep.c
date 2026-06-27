#include "dep.h"
#include "reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>

/// Trace Utilities

int g_trace_depth = 0;
bool g_trace_enabled = false; // Enable with --trace=dep or --trace=all

void shared_trace_indent(void) {
    if (!g_trace_enabled) return;
    for (int i = 0; i < g_trace_depth; i++) fprintf(stderr, "│   ");
}

/// Universe levels

static Level *level_alloc(LevelKind kind) {
    Level *l = calloc(1, sizeof(Level));
    l->kind  = kind;
    return l;
}

Level *level_const(int n) {
    Level *l = level_alloc(LEVEL_CONST);
    l->value = n;
    return l;
}

Level *level_var(int id) {
    Level *l  = level_alloc(LEVEL_VAR);
    l->var_id = id;
    return l;
}

Level *level_max(Level *u, Level *v) {
    // Eagerly reduce: max(const a, const b) = const(max a b)
    if (u->kind == LEVEL_CONST && v->kind == LEVEL_CONST) {
        int m = u->value > v->value ? u->value : v->value;
        level_free(u); level_free(v);
        return level_const(m);
    }
    Level *l  = level_alloc(LEVEL_MAX);
    l->left   = u;
    l->right  = v;
    return l;
}

Level *level_succ(Level *u) {
    // Eagerly reduce: succ(const n) = const(n+1)
    if (u->kind == LEVEL_CONST) {
        int n = u->value + 1;
        if (n > DEP_LEVEL_MAX) {
            fprintf(stderr, "dep: universe level overflow (max %d)\n", DEP_LEVEL_MAX);
            n = DEP_LEVEL_MAX;
        }
        level_free(u);
        return level_const(n);
    }
    Level *l = level_alloc(LEVEL_SUCC);
    l->left  = u;
    l->value = 1;
    return l;
}

Level *level_clone(Level *u) {
    if (!u) return NULL;
    switch (u->kind) {
        case LEVEL_CONST: return level_const(u->value);
        case LEVEL_VAR:   return level_var(u->var_id);
        case LEVEL_MAX:   return level_max(level_clone(u->left), level_clone(u->right));
        case LEVEL_SUCC:  { Level *l = level_alloc(LEVEL_SUCC); l->left = level_clone(u->left); l->value = u->value; return l; }
    }
    return NULL;
}

void level_free(Level *u) {
    if (!u) return;
    level_free(u->left);
    level_free(u->right);
    free(u);
}

int level_eval(Level *u) {
    if (!u) return 0;
    switch (u->kind) {
        case LEVEL_CONST: return u->value;
        case LEVEL_VAR:   return -1; // cannot evaluate variable
        case LEVEL_MAX: {
            int a = level_eval(u->left), b = level_eval(u->right);
            if (a < 0 || b < 0) return -1;
            return a > b ? a : b;
        }
        case LEVEL_SUCC: {
            int a = level_eval(u->left);
            if (a < 0) return -1;
            return a + u->value;
        }
    }
    return -1;
}

bool levels_equal(Level *u, Level *v) {
    if (!u && !v) return true;
    if (!u || !v) return false;
    if (u->kind != v->kind) {
        // Try evaluating both to concrete ints
        int a = level_eval(u), b = level_eval(v);
        if (a >= 0 && b >= 0) return a == b;
        return false;
    }
    switch (u->kind) {
        case LEVEL_CONST: return u->value  == v->value;
        case LEVEL_VAR:   return u->var_id == v->var_id;
        case LEVEL_MAX:   return levels_equal(u->left, v->left)
                              && levels_equal(u->right, v->right);
        case LEVEL_SUCC:  return u->value == v->value
                              && levels_equal(u->left, v->left);
    }
    return false;
}

bool level_leq(Level *u, Level *v) {
    int a = level_eval(u), b = level_eval(v);
    if (a >= 0 && b >= 0) return a <= b;
    return false; // conservative: unknown ≤ unknown = false
}

const char *level_to_string(Level *u) {
    static char bufs[32][64];
    static int buf_idx = 0;
    char *buf = bufs[buf_idx];
    buf_idx = (buf_idx + 1) % 32;
    if (!u) { snprintf(buf, 64, "?"); return buf; }
    int n = level_eval(u);
    if (n >= 0) snprintf(buf, 64, "%d", n);
    else        snprintf(buf, 64, "?lvl");
    return buf;
}


/// Term constructors

static Term *term_alloc(TermKind kind) {
    Term *t = calloc(1, sizeof(Term));
    t->kind = kind;
    return t;
}

Term *term_bvar(int index) {
    Term *t = term_alloc(TERM_BVAR);
    t->bvar_index = index;
    return t;
}

Term *term_fvar(const char *name) {
    Term *t = term_alloc(TERM_FVAR);
    t->fvar_name = strdup(name);
    return t;
}

Term *term_type(Level *level) {
    Term *t = term_alloc(TERM_TYPE);
    t->type_level = level;
    return t;
}

Term *term_type_n(int n) {
    return term_type(level_const(n));
}

Term *term_kind(void) {
    return term_alloc(TERM_KIND);
}

Term *term_pi(const char *name, Term *dom, Term *body, Implicitness impl) {
    Term *t = term_alloc(TERM_PI);
    t->binder_name = name ? strdup(name) : strdup("_");
    t->binder_dom  = dom;
    t->binder_body = body;
    t->implicit    = impl;
    return t;
}

Term *term_lam(const char *name, Term *dom, Term *body) {
    Term *t = term_alloc(TERM_LAM);
    t->binder_name = name ? strdup(name) : strdup("_");
    t->binder_dom  = dom;
    t->binder_body = body;
    return t;
}

Term *term_app(Term *fn, Term **args, int argc) {
    Term *t    = term_alloc(TERM_APP);
    t->app_fn  = fn;
    t->app_args = args;   // takes ownership
    t->app_argc = argc;
    return t;
}

Term *term_app1(Term *fn, Term *arg) {
    Term **args = malloc(sizeof(Term *));
    args[0]     = arg;
    return term_app(fn, args, 1);
}

Term *term_sigma(const char *name, Term *dom, Term *body) {
    Term *t = term_alloc(TERM_SIGMA);
    t->binder_name = name ? strdup(name) : strdup("_");
    t->binder_dom  = dom;
    t->binder_body = body;
    return t;
}

Term *term_pair(Term *fst, Term *snd, Term *type) {
    Term *t    = term_alloc(TERM_PAIR);
    t->pair_fst  = fst;
    t->pair_snd  = snd;
    t->pair_type = type;
    return t;
}

Term *term_fst(Term *pair) {
    Term *t    = term_alloc(TERM_FST);
    t->pair_fst = pair;
    return t;
}

Term *term_snd(Term *pair) {
    Term *t    = term_alloc(TERM_SND);
    t->pair_fst = pair;   // reuse pair_fst as the source
    return t;
}

Term *term_eq(Term *lhs, Term *rhs, Term *type) {
    Term *t   = term_alloc(TERM_EQ);
    t->eq_lhs  = lhs;
    t->eq_rhs  = rhs;
    t->eq_type = type;
    return t;
}

Term *term_refl(Term *val) {
    Term *t    = term_alloc(TERM_REFL);
    t->refl_val = val;
    return t;
}

Term *term_subst(Term *proof, Term *motive, Term *base) {
    Term *t       = term_alloc(TERM_SUBST);
    t->subst_proof  = proof;
    t->subst_motive = motive;
    t->subst_base   = base;
    return t;
}

Term *term_nat (void) { return term_alloc(TERM_NAT);  }
Term *term_zero(void) { return term_alloc(TERM_ZERO); }

Term *term_succ(Term *pred) {
    Term *t     = term_alloc(TERM_SUCC_T);
    t->succ_pred = pred;
    return t;
}

Term *term_num_lit(unsigned long long n) {
    Term *t = term_alloc(TERM_NUM_LIT);
    t->num_lit = n;
    return t;
}

Term *term_nat_elim(Term *motive, Term *pz, Term *ps, Term *n) {
    Term *t       = term_alloc(TERM_NAT_ELIM);
    t->nelim_motive = motive;
    t->nelim_zero   = pz;
    t->nelim_succ   = ps;
    t->nelim_arg    = n;
    return t;
}

Term *term_if(Term *cond, Term *t, Term *e) {
    Term *r = term_alloc(TERM_IF);
    r->if_cond = cond;
    r->if_then = t;
    r->if_else = e;
    return r;
}

Term *term_let(const char *name, Term *type, Term *val, Term *body) {
    Term *t = term_alloc(TERM_LET);
    t->binder_name = name ? strdup(name) : strdup("_");
    t->let_type    = type;
    t->let_val     = val;
    t->binder_body = body;
    return t;
}

Term *term_meta(int id) {
    Term *t   = term_alloc(TERM_META);
    t->meta_id = id;
    return t;
}

Term *term_hole(void) { return term_alloc(TERM_HOLE); }

Term *term_embed(Type *ground_type) {
    Term *t      = term_alloc(TERM_EMBED);
    t->embed_type = ground_type;
    return t;
}

Term *term_ann(Term *t_, Term *ty) {
    Term *t   = term_alloc(TERM_ANN);
    t->ann_term = t_;
    t->ann_type = ty;
    return t;
}


/// Structural operations on terms
//
//  dep_shift — raise all free De Bruijn indices ≥ cutoff by delta.
//  This is required whenever a term is placed under a new binder:
//    · When building  Π(x:A).B  the body B may contain references to
//      outer binders at indices 0, 1, 2 … Those indices must be raised
//      by 1 to account for the new x binder.
//    · Similarly for λ, Σ, let.
//  cutoff tracks how many binders we have descended into within the
//  shift operation itself, so that locally-bound variables are not moved.
//
Term *dep_shift(Term *t, int delta, int cutoff) {
    if (!t) return NULL;
    if (delta == 0) return term_clone(t);
    switch (t->kind) {
    case TERM_BVAR:
        if (t->bvar_index >= cutoff)
            return term_bvar(t->bvar_index + delta);
        return term_clone(t);

    case TERM_FVAR:    return term_clone(t);
    case TERM_TYPE:    return term_type(level_clone(t->type_level));
    case TERM_KIND:    return term_kind();
    case TERM_NAT:     return term_nat();
    case TERM_ZERO:    return term_zero();
    case TERM_META:    return term_meta(t->meta_id);
    case TERM_HOLE:    return term_hole();
    case TERM_EMBED:   return term_embed(t->embed_type);

    case TERM_PI:
    case TERM_LAM:
    case TERM_SIGMA: {
        Term *dom  = dep_shift(t->binder_dom,  delta, cutoff);
        Term *body = dep_shift(t->binder_body, delta, cutoff + 1); /* under binder */
        if (t->kind == TERM_PI)    return term_pi(t->binder_name, dom, body, t->implicit);
        if (t->kind == TERM_LAM)   return term_lam(t->binder_name, dom, body);
        return term_sigma(t->binder_name, dom, body);
    }

    case TERM_LET: {
        Term *ty   = dep_shift(t->let_type,    delta, cutoff);
        Term *val  = dep_shift(t->let_val,     delta, cutoff);
        Term *body = dep_shift(t->binder_body, delta, cutoff + 1);
        return term_let(t->binder_name, ty, val, body);
    }

    case TERM_APP: {
        Term  *fn    = dep_shift(t->app_fn, delta, cutoff);
        Term **args  = malloc(sizeof(Term *) * t->app_argc);
        for (int i = 0; i < t->app_argc; i++)
            args[i] = dep_shift(t->app_args[i], delta, cutoff);
        return term_app(fn, args, t->app_argc);
    }

    case TERM_PAIR:
        return term_pair(dep_shift(t->pair_fst,  delta, cutoff),
                         dep_shift(t->pair_snd,  delta, cutoff),
                         dep_shift(t->pair_type, delta, cutoff));

    case TERM_FST: { Term *r = term_alloc(TERM_FST); r->pair_fst = dep_shift(t->pair_fst, delta, cutoff); return r; }
    case TERM_SND: { Term *r = term_alloc(TERM_SND); r->pair_fst = dep_shift(t->pair_fst, delta, cutoff); return r; }

    case TERM_EQ:
        return term_eq(dep_shift(t->eq_lhs,  delta, cutoff),
                       dep_shift(t->eq_rhs,  delta, cutoff),
                       dep_shift(t->eq_type, delta, cutoff));

    case TERM_REFL: { Term *r = term_alloc(TERM_REFL); r->refl_val = dep_shift(t->refl_val, delta, cutoff); return r; }

    case TERM_SUBST:
        return term_subst(dep_shift(t->subst_proof,  delta, cutoff),
                          dep_shift(t->subst_motive, delta, cutoff),
                          dep_shift(t->subst_base,   delta, cutoff));

    case TERM_IF:
        return term_if(dep_shift(t->if_cond, delta, cutoff),
                       dep_shift(t->if_then, delta, cutoff),
                       dep_shift(t->if_else, delta, cutoff));

    case TERM_SUCC_T: { Term *r = term_alloc(TERM_SUCC_T); r->succ_pred = dep_shift(t->succ_pred, delta, cutoff); return r; }
    case TERM_NUM_LIT: return term_num_lit(t->num_lit);

    case TERM_NAT_ELIM:
        return term_nat_elim(dep_shift(t->nelim_motive, delta, cutoff),
                             dep_shift(t->nelim_zero,   delta, cutoff),
                             dep_shift(t->nelim_succ,   delta, cutoff),
                             dep_shift(t->nelim_arg,    delta, cutoff));

    case TERM_ANN:
        return term_ann(dep_shift(t->ann_term, delta, cutoff),
                        dep_shift(t->ann_type, delta, cutoff));
    }
    return term_clone(t);
}

//  dep_subst_bvar — perform one β-reduction step:
//    body[BVar(0) |-> s]
//  All De Bruijn indices ≥ 1 in body are decremented by 1 (since the
//  binder they referred to has been consumed).  s must be shifted up by
//  depth each time it is substituted under depth additional binders —
//  here depth starts at 0 and grows as we descend.
//
static Term *subst_at(Term *body, Term *s, int depth) {
    if (!body) return NULL;
    switch (body->kind) {
    case TERM_BVAR:
        if (body->bvar_index == depth) {
            // Hit — replace with s shifted to the current depth
            return dep_shift(s, depth, 0);
        }
        if (body->bvar_index > depth)
            return term_bvar(body->bvar_index - 1);  // close the gap
        return term_clone(body);                     // index < depth: refers to inner binder

    case TERM_FVAR:    return term_clone(body);
    case TERM_TYPE:    return term_type(level_clone(body->type_level));
    case TERM_KIND:    return term_kind();
    case TERM_NAT:     return term_nat();
    case TERM_ZERO:    return term_zero();
    case TERM_META:    return term_meta(body->meta_id);
    case TERM_HOLE:    return term_hole();
    case TERM_EMBED:   return term_embed(body->embed_type);

    case TERM_PI:
    case TERM_LAM:
    case TERM_SIGMA: {
        Term *dom  = subst_at(body->binder_dom,  s, depth);
        Term *bd   = subst_at(body->binder_body, s, depth + 1);
        if (body->kind == TERM_PI)    return term_pi(body->binder_name, dom, bd, body->implicit);
        if (body->kind == TERM_LAM)   return term_lam(body->binder_name, dom, bd);
        return term_sigma(body->binder_name, dom, bd);
    }

    case TERM_LET: {
        Term *ty  = subst_at(body->let_type,    s, depth);
        Term *val = subst_at(body->let_val,     s, depth);
        Term *bd  = subst_at(body->binder_body, s, depth + 1);
        return term_let(body->binder_name, ty, val, bd);
    }

    case TERM_APP: {
        Term  *fn   = subst_at(body->app_fn, s, depth);
        Term **args = malloc(sizeof(Term *) * body->app_argc);
        for (int i = 0; i < body->app_argc; i++)
            args[i] = subst_at(body->app_args[i], s, depth);
        return term_app(fn, args, body->app_argc);
    }

    case TERM_PAIR:
        return term_pair(subst_at(body->pair_fst,  s, depth),
                         subst_at(body->pair_snd,  s, depth),
                         subst_at(body->pair_type, s, depth));

    case TERM_FST: { Term *r = term_alloc(TERM_FST); r->pair_fst = subst_at(body->pair_fst, s, depth); return r; }
    case TERM_SND: { Term *r = term_alloc(TERM_SND); r->pair_fst = subst_at(body->pair_fst, s, depth); return r; }

    case TERM_EQ:
        return term_eq(subst_at(body->eq_lhs, s, depth),
                       subst_at(body->eq_rhs, s, depth),
                       subst_at(body->eq_type, s, depth));

    case TERM_REFL: { Term *r = term_alloc(TERM_REFL); r->refl_val = subst_at(body->refl_val, s, depth); return r; }

    case TERM_SUBST:
        return term_subst(subst_at(body->subst_proof,  s, depth),
                          subst_at(body->subst_motive, s, depth),
                          subst_at(body->subst_base,   s, depth));

    case TERM_IF:
        return term_if(subst_at(body->if_cond, s, depth),
                       subst_at(body->if_then, s, depth),
                       subst_at(body->if_else, s, depth));

    case TERM_SUCC_T: { Term *r = term_alloc(TERM_SUCC_T); r->succ_pred = subst_at(body->succ_pred, s, depth); return r; }
    case TERM_NUM_LIT: return term_num_lit(body->num_lit);

    case TERM_NAT_ELIM:
        return term_nat_elim(subst_at(body->nelim_motive, s, depth),
                             subst_at(body->nelim_zero,   s, depth),
                             subst_at(body->nelim_succ,   s, depth),
                             subst_at(body->nelim_arg,    s, depth));

    case TERM_ANN:
        return term_ann(subst_at(body->ann_term, s, depth),
                        subst_at(body->ann_type, s, depth));
    }
    return term_clone(body);
}

Term *dep_subst_bvar(Term *body, Term *s) {
    return subst_at(body, s, 0);
}

Term *dep_subst_fvar(Term *t, const char *name, Term *replacement) {
    if (!t) return NULL;
    switch (t->kind) {
    case TERM_FVAR:
        if (strcmp(t->fvar_name, name) == 0) return term_clone(replacement);
        return term_clone(t);

    case TERM_BVAR:
    case TERM_TYPE:
    case TERM_KIND:
    case TERM_NAT:
    case TERM_ZERO:
    case TERM_META:
    case TERM_HOLE:
    case TERM_EMBED:
        return term_clone(t);

    case TERM_PI:
    case TERM_LAM:
    case TERM_SIGMA: {
        Term *dom  = dep_subst_fvar(t->binder_dom,  name, replacement);
        Term *body = dep_subst_fvar(t->binder_body, name, replacement);
        if (t->kind == TERM_PI)    return term_pi(t->binder_name, dom, body, t->implicit);
        if (t->kind == TERM_LAM)   return term_lam(t->binder_name, dom, body);
        return term_sigma(t->binder_name, dom, body);
    }

    case TERM_LET: {
        Term *ty   = dep_subst_fvar(t->let_type,    name, replacement);
        Term *val  = dep_subst_fvar(t->let_val,     name, replacement);
        Term *body = dep_subst_fvar(t->binder_body, name, replacement);
        return term_let(t->binder_name, ty, val, body);
    }

    case TERM_APP: {
        Term  *fn   = dep_subst_fvar(t->app_fn, name, replacement);
        Term **args = malloc(sizeof(Term *) * t->app_argc);
        for (int i = 0; i < t->app_argc; i++)
            args[i] = dep_subst_fvar(t->app_args[i], name, replacement);
        return term_app(fn, args, t->app_argc);
    }

    case TERM_PAIR:
        return term_pair(dep_subst_fvar(t->pair_fst,  name, replacement),
                         dep_subst_fvar(t->pair_snd,  name, replacement),
                         dep_subst_fvar(t->pair_type, name, replacement));

    case TERM_FST: { Term *r = term_alloc(TERM_FST); r->pair_fst = dep_subst_fvar(t->pair_fst, name, replacement); return r; }
    case TERM_SND: { Term *r = term_alloc(TERM_SND); r->pair_fst = dep_subst_fvar(t->pair_fst, name, replacement); return r; }

    case TERM_EQ:
        return term_eq(dep_subst_fvar(t->eq_lhs,  name, replacement),
                       dep_subst_fvar(t->eq_rhs,  name, replacement),
                       dep_subst_fvar(t->eq_type, name, replacement));

    case TERM_REFL: { Term *r = term_alloc(TERM_REFL); r->refl_val = dep_subst_fvar(t->refl_val, name, replacement); return r; }

    case TERM_SUBST:
        return term_subst(dep_subst_fvar(t->subst_proof,  name, replacement),
                          dep_subst_fvar(t->subst_motive, name, replacement),
                          dep_subst_fvar(t->subst_base,   name, replacement));

    case TERM_IF:
        return term_if(dep_subst_fvar(t->if_cond, name, replacement),
                       dep_subst_fvar(t->if_then, name, replacement),
                       dep_subst_fvar(t->if_else, name, replacement));

    case TERM_SUCC_T: { Term *r = term_alloc(TERM_SUCC_T); r->succ_pred = dep_subst_fvar(t->succ_pred, name, replacement); return r; }
    case TERM_NUM_LIT: return term_num_lit(t->num_lit);

    case TERM_NAT_ELIM:
        return term_nat_elim(dep_subst_fvar(t->nelim_motive, name, replacement),
                             dep_subst_fvar(t->nelim_zero,   name, replacement),
                             dep_subst_fvar(t->nelim_succ,   name, replacement),
                             dep_subst_fvar(t->nelim_arg,    name, replacement));

    case TERM_ANN:
        return term_ann(dep_subst_fvar(t->ann_term, name, replacement),
                        dep_subst_fvar(t->ann_type, name, replacement));
    }
    return term_clone(t);
}

Term *term_clone(Term *t) {
    if (!t) return NULL;
    switch (t->kind) {
    case TERM_BVAR:    return term_bvar(t->bvar_index);
    case TERM_FVAR:    return term_fvar(t->fvar_name);
    case TERM_TYPE:    return term_type(level_clone(t->type_level));
    case TERM_KIND:    return term_kind();
    case TERM_NAT:     return term_nat();
    case TERM_ZERO:    return term_zero();
    case TERM_META:    return term_meta(t->meta_id);
    case TERM_HOLE:    return term_hole();
    case TERM_EMBED:   return term_embed(t->embed_type);
    case TERM_PI:      return term_pi(t->binder_name, term_clone(t->binder_dom), term_clone(t->binder_body), t->implicit);
    case TERM_LAM:     return term_lam(t->binder_name, term_clone(t->binder_dom), term_clone(t->binder_body));
    case TERM_SIGMA:   return term_sigma(t->binder_name, term_clone(t->binder_dom), term_clone(t->binder_body));
    case TERM_LET:     return term_let(t->binder_name, term_clone(t->let_type), term_clone(t->let_val), term_clone(t->binder_body));
    case TERM_APP: {
        Term **args = malloc(sizeof(Term *) * t->app_argc);
        for (int i = 0; i < t->app_argc; i++) args[i] = term_clone(t->app_args[i]);
        return term_app(term_clone(t->app_fn), args, t->app_argc);
    }
    case TERM_PAIR:    return term_pair(term_clone(t->pair_fst), term_clone(t->pair_snd), term_clone(t->pair_type));
    case TERM_FST:     return term_fst(term_clone(t->pair_fst));
    case TERM_SND:     return term_snd(term_clone(t->pair_fst));
    case TERM_EQ:      return term_eq(term_clone(t->eq_lhs), term_clone(t->eq_rhs), term_clone(t->eq_type));
    case TERM_REFL:    { Term *r = term_alloc(TERM_REFL); r->refl_val = term_clone(t->refl_val); return r; }
    case TERM_SUBST:   return term_subst(term_clone(t->subst_proof), term_clone(t->subst_motive), term_clone(t->subst_base));
    case TERM_IF:      return term_if(term_clone(t->if_cond), term_clone(t->if_then), term_clone(t->if_else));
    case TERM_SUCC_T:  { Term *r = term_alloc(TERM_SUCC_T); r->succ_pred = term_clone(t->succ_pred); return r; }
    case TERM_NUM_LIT: return term_num_lit(t->num_lit);
    case TERM_NAT_ELIM: return term_nat_elim(term_clone(t->nelim_motive), term_clone(t->nelim_zero), term_clone(t->nelim_succ), term_clone(t->nelim_arg));
    case TERM_ANN:     return term_ann(term_clone(t->ann_term), term_clone(t->ann_type));
    }
    return term_alloc(t->kind);
}

void term_free(Term *t) {
    if (!t) return;
    free(t->fvar_name);
    free(t->binder_name);
    level_free(t->type_level);
    term_free(t->binder_dom);
    term_free(t->binder_body);
    term_free(t->let_type);
    term_free(t->let_val);
    term_free(t->app_fn);
    if (t->app_args) {
        for (int i = 0; i < t->app_argc; i++) term_free(t->app_args[i]);
        free(t->app_args);
    }
    term_free(t->pair_fst);
    term_free(t->pair_snd);
    term_free(t->pair_type);
    term_free(t->eq_lhs);
    term_free(t->eq_rhs);
    term_free(t->eq_type);
    term_free(t->refl_val);
    term_free(t->subst_proof);
    term_free(t->subst_motive);
    term_free(t->subst_base);
    term_free(t->if_cond);
    term_free(t->if_then);
    term_free(t->if_else);
    term_free(t->succ_pred);
    term_free(t->nelim_motive);
    term_free(t->nelim_zero);
    term_free(t->nelim_succ);
    term_free(t->nelim_arg);
    term_free(t->ann_term);
    term_free(t->ann_type);
    // embed_type is not owned — it belongs to the type registry
    free(t);
}

const char *term_to_string(Term *t) {
    static char bufs[128][4096];
    static int buf_idx = 0;
    char *buf = bufs[buf_idx];
    buf_idx = (buf_idx + 1) % 128;
    if (!t) { return "?"; }
    switch (t->kind) {
    case TERM_BVAR: snprintf(buf, 4096, "#%d", t->bvar_index); return buf;
    case TERM_FVAR: return t->fvar_name ? t->fvar_name : "?";
    case TERM_TYPE: snprintf(buf, 4096, "Type %s", level_to_string(t->type_level)); return buf;
    case TERM_KIND: return "Kind";
    case TERM_NAT:  return "Nat";
    case TERM_ZERO: return "zero";
    case TERM_META: snprintf(buf, 4096, "?%d", t->meta_id); return buf;
    case TERM_HOLE: return "_";
    case TERM_EMBED: return type_to_string(t->embed_type);
    case TERM_PI:
        snprintf(buf, 4096, "Π(%s : %s). %s",
                 t->binder_name ? t->binder_name : "_",
                 term_to_string(t->binder_dom),
                 term_to_string(t->binder_body));
        return buf;
    case TERM_LAM:
        snprintf(buf, 4096, "λ(%s : %s). %s",
                 t->binder_name ? t->binder_name : "_",
                 term_to_string(t->binder_dom),
                 term_to_string(t->binder_body));
        return buf;
    case TERM_SIGMA:
        snprintf(buf, 4096, "Σ(%s : %s). %s",
                 t->binder_name ? t->binder_name : "_",
                 term_to_string(t->binder_dom),
                 term_to_string(t->binder_body));
        return buf;
    case TERM_APP: {
        int offset = snprintf(buf, 4096, "(%s", term_to_string(t->app_fn));
        for (int i = 0; i < t->app_argc && offset < 4095; i++) {
            offset += snprintf(buf + offset, 4096 - offset, " %s", term_to_string(t->app_args[i]));
        }
        if (offset < 4095) snprintf(buf + offset, 4096 - offset, ")");
        return buf;
    }
    case TERM_PAIR:
        snprintf(buf, 4096, "⟨%s, %s⟩",
                 term_to_string(t->pair_fst),
                 term_to_string(t->pair_snd));
        return buf;
    case TERM_FST: snprintf(buf, 4096, "fst(%s)", term_to_string(t->pair_fst)); return buf;
    case TERM_SND: snprintf(buf, 4096, "snd(%s)", term_to_string(t->pair_fst)); return buf;
    case TERM_EQ:
        snprintf(buf, 4096, "%s ≡ %s",
                 term_to_string(t->eq_lhs), term_to_string(t->eq_rhs));
        return buf;
    case TERM_REFL: snprintf(buf, 4096, "refl %s", term_to_string(t->refl_val)); return buf;
    case TERM_SUCC_T: {
        int n = 0;
        Term *curr = t;
        while (curr->kind == TERM_SUCC_T) { n++; curr = curr->succ_pred; }
        if (curr->kind == TERM_ZERO) {
            snprintf(buf, 4096, "%d", n);
        } else {
            snprintf(buf, 4096, "%d + %s", n, term_to_string(curr));
        }
        return buf;
    }
    case TERM_NUM_LIT:
        snprintf(buf, 4096, "%llu", t->num_lit);
        return buf;
    case TERM_NAT_ELIM:
        snprintf(buf, 4096, "Nat-elim(%s, %s, %s, %s)",
                 term_to_string(t->nelim_motive),
                 term_to_string(t->nelim_zero),
                 term_to_string(t->nelim_succ),
                 term_to_string(t->nelim_arg));
        return buf;
    case TERM_IF:
        snprintf(buf, 4096, "if %s then %s else %s",
                 term_to_string(t->if_cond),
                 term_to_string(t->if_then),
                 term_to_string(t->if_else));
        return buf;
    case TERM_LET:
        snprintf(buf, 4096, "let %s := %s in %s",
                 t->binder_name ? t->binder_name : "_",
                 term_to_string(t->let_val),
                 term_to_string(t->binder_body));
        return buf;
    case TERM_SUBST:
        snprintf(buf, 4096, "subst(%s, %s, %s)",
                 term_to_string(t->subst_proof),
                 term_to_string(t->subst_motive),
                 term_to_string(t->subst_base));
        return buf;
    case TERM_ANN:
        snprintf(buf, 4096, "(%s : %s)",
                 term_to_string(t->ann_term), term_to_string(t->ann_type));
        return buf;
    }
    return "?";
}

bool terms_syntactically_equal(Term *a, Term *b) {
    if (!a && !b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case TERM_BVAR:  return a->bvar_index == b->bvar_index;
    case TERM_FVAR:  return strcmp(a->fvar_name, b->fvar_name) == 0;
    case TERM_TYPE:  return levels_equal(a->type_level, b->type_level);
    case TERM_KIND:  return true;
    case TERM_NAT:   return true;
    case TERM_ZERO:  return true;
    case TERM_META:  return a->meta_id == b->meta_id;
    case TERM_HOLE:  return false;   // two holes are never the same
    case TERM_EMBED: return types_equal(a->embed_type, b->embed_type);
    case TERM_PI:
    case TERM_LAM:
    case TERM_SIGMA:
        return terms_syntactically_equal(a->binder_dom,  b->binder_dom)
            && terms_syntactically_equal(a->binder_body, b->binder_body);
    case TERM_LET:
        return terms_syntactically_equal(a->let_type,    b->let_type)
            && terms_syntactically_equal(a->let_val,     b->let_val)
            && terms_syntactically_equal(a->binder_body, b->binder_body);
    case TERM_APP:
        if (a->app_argc != b->app_argc) return false;
        if (!terms_syntactically_equal(a->app_fn, b->app_fn)) return false;
        for (int i = 0; i < a->app_argc; i++)
            if (!terms_syntactically_equal(a->app_args[i], b->app_args[i])) return false;
        return true;
    case TERM_PAIR:
        return terms_syntactically_equal(a->pair_fst, b->pair_fst)
            && terms_syntactically_equal(a->pair_snd, b->pair_snd);
    case TERM_EQ:
        return terms_syntactically_equal(a->eq_lhs, b->eq_lhs)
            && terms_syntactically_equal(a->eq_rhs, b->eq_rhs);
    case TERM_REFL:   return terms_syntactically_equal(a->refl_val, b->refl_val);
    case TERM_IF:     return terms_syntactically_equal(a->if_cond, b->if_cond) &&
                             terms_syntactically_equal(a->if_then, b->if_then) &&
                             terms_syntactically_equal(a->if_else, b->if_else);
    case TERM_SUCC_T: return terms_syntactically_equal(a->succ_pred, b->succ_pred);
    case TERM_NUM_LIT: return a->num_lit == b->num_lit;
    default: return true;
    }
}


/// Semantic environment

EvalEnv *eval_env_empty(void) {
    EvalEnv *e = calloc(1, sizeof(EvalEnv));
    e->head  = NULL;
    e->level = 0;
    return e;
}

EvalEnv *eval_env_extend(EvalEnv *e, Value *v) {
    EvalEnv      *ne    = malloc(sizeof(EvalEnv));
    EvalEnvEntry *entry = malloc(sizeof(EvalEnvEntry));
    entry->val  = v;
    entry->next = e ? e->head : NULL;
    ne->head    = entry;
    ne->level   = e ? e->level + 1 : 1;
    return ne;
}

Value *eval_env_lookup(EvalEnv *e, int index) {
    if (!e) return NULL;
    EvalEnvEntry *cur = e->head;
    for (int i = 0; i < index && cur; i++) cur = cur->next;
    return cur ? cur->val : NULL;
}

void eval_env_free(EvalEnv *e) {
    if (!e) return;
    EvalEnvEntry *cur = e->head;
    while (cur) {
        EvalEnvEntry *next = cur->next;
        free(cur);
        cur = next;
    }
    free(e);
}

void eval_env_discard_top(EvalEnv *e) {
    if (!e) return;
    if (e->head) {
        EvalEnvEntry *old = e->head;
        e->head = old->next;
        free(old);
    }
    free(e);
}

EvalEnv *eval_env_clone(EvalEnv *e) {
    if (!e) return eval_env_empty();
    EvalEnv *ne      = malloc(sizeof(EvalEnv));
    ne->level        = e->level;
    ne->head         = NULL;
    EvalEnvEntry **tail = &ne->head;
    for (EvalEnvEntry *cur = e->head; cur; cur = cur->next) {
        EvalEnvEntry *entry = malloc(sizeof(EvalEnvEntry));
        entry->val  = cur->val;
        entry->next = NULL;
        *tail = entry;
        tail  = &entry->next;
    }
    return ne;
}


/// Values and closures

static Value *val_alloc(ValKind kind) {
    Value *v = calloc(1, sizeof(Value));
    v->kind  = kind;
    return v;
}

Value *val_universe(Level *level) {
    Value *v  = val_alloc(VAL_UNIVERSE);
    v->level  = level;
    return v;
}

Value *val_universe_n(int n) {
    return val_universe(level_const(n));
}

Value *val_pi(const char *name, Value *dom, Closure body, Implicitness impl) {
    Value *v       = val_alloc(VAL_PI);
    v->binder_name = name ? strdup(name) : strdup("_");
    v->domain      = dom;
    v->closure     = body;
    v->implicit    = impl;
    return v;
}

Value *val_sigma(const char *name, Value *dom, Closure body) {
    Value *v       = val_alloc(VAL_SIGMA);
    v->binder_name = name ? strdup(name) : strdup("_");
    v->domain      = dom;
    v->closure     = body;
    return v;
}

Value *val_lam(const char *name, Value *dom, Closure body) {
    Value *v       = val_alloc(VAL_LAM);
    v->binder_name = name ? strdup(name) : strdup("_");
    v->domain      = dom;
    v->closure     = body;
    return v;
}

Value *val_pair(Value *fst, Value *snd) {
    Value *v = val_alloc(VAL_PAIR);
    v->fst   = fst;
    v->snd   = snd;
    return v;
}

Value *val_nat(void)  { return val_alloc(VAL_NAT);  }
Value *val_zero(void) { return val_alloc(VAL_ZERO); }

Value *val_succ(Value *pred) {
    Value *v = val_alloc(VAL_SUCC);
    v->pred  = pred;
    return v;
}

Value *val_num_lit(unsigned long long n) {
    Value *v = val_alloc(VAL_NUM_LIT);
    v->num_lit = n;
    return v;
}

Value *val_eq(Value *lhs, Value *rhs, Value *type) {
    Value *v      = val_alloc(VAL_EQ);
    v->eq_lhs     = lhs;
    v->eq_rhs     = rhs;
    v->eq_type_val = type;
    return v;
}

Value *val_refl(Value *v_) {
    Value *v   = val_alloc(VAL_REFL);
    v->refl_val = v_;
    return v;
}

Value *val_if(Value *c, Value *t, Value *e) {
    Value *v = val_alloc(VAL_IF);
    v->if_cond_val = c;
    v->if_then_val = t;
    v->if_else_val = e;
    return v;
}

Spine val_spine_empty(void) {
    Spine sp = { .args = NULL, .count = 0, .cap = 0 };
    return sp;
}

void val_spine_push(Spine *sp, Value *arg) {
    if (sp->count >= sp->cap) {
        sp->cap  = sp->cap ? sp->cap * 2 : 4;
        sp->args = realloc(sp->args, sizeof(Value *) * sp->cap);
    }
    sp->args[sp->count++] = arg;
}

Spine val_spine_clone(Spine sp) {
    Spine ns = val_spine_empty();
    for (int i = 0; i < sp.count; i++) val_spine_push(&ns, sp.args[i]);
    return ns;
}

void val_spine_free(Spine sp) {
    free(sp.args);
}

Value *val_neutral(const char *name, int level, Spine spine) {
    Value *v         = val_alloc(VAL_NEUTRAL);
    v->neutral_name  = strdup(name ? name : "?null");
    v->neutral_level = level;
    v->spine         = spine;
    return v;
}

Value *val_meta(int id, Spine spine) {
    Value *v   = val_alloc(VAL_META);
    v->meta_id = id;
    v->spine   = spine;
    return v;
}

Value *val_embed(Type *t) {
    Value *v      = val_alloc(VAL_EMBED);
    v->embed_type = t;
    return v;
}

void val_free(Value *v) {
    if (!v) return;
    free(v->binder_name);
    free(v->neutral_name);
    level_free(v->level);
    /* NOTE: We do NOT recursively free Values because they are shared
     * across the semantic environment.  The allocator/GC is responsible.
     * For now (single-pass compilation) we accept the leak; a precise
     * ref-counted or arena allocator is the correct production solution. */
    free(v);
}

Value *dep_closure_apply(Closure c, Value *arg) {
    EvalEnv *new_env = eval_env_extend(c.env, arg);
    Value *result = dep_eval(c.body, new_env, NULL);
    eval_env_discard_top(new_env);
    return result;
}


/// Metavariable context

MetaCtx *meta_ctx_create(void) {
    MetaCtx *m = calloc(1, sizeof(MetaCtx));
    m->cap     = 64;
    m->entries = calloc(m->cap, sizeof(MetaEntry));
    m->count   = 0;
    return m;
}

void meta_ctx_free(MetaCtx *mctx) {
    if (!mctx) return;
    for (int i = 0; i < mctx->count; i++) {
        free(mctx->entries[i].hint);
        term_free(mctx->entries[i].solution);
    }
    free(mctx->entries);
    free(mctx);
}

int meta_fresh(MetaCtx *mctx, Value *type, int depth, const char *hint) {
    if (mctx->count >= mctx->cap) {
        mctx->cap     *= 2;
        mctx->entries  = realloc(mctx->entries, sizeof(MetaEntry) * mctx->cap);
    }
    int id = mctx->count++;
    MetaEntry *e = &mctx->entries[id];
    e->id       = id;
    e->state    = META_UNSOLVED;
    e->type     = type;
    e->solution = NULL;
    e->depth    = depth;
    e->hint     = hint ? strdup(hint) : NULL;
    return id;
}

MetaEntry *meta_lookup(MetaCtx *mctx, int id) {
    if (!mctx || id < 0 || id >= mctx->count) return NULL;
    return &mctx->entries[id];
}

bool meta_solve(MetaCtx *mctx, int id, Term *solution) {
    MetaEntry *e = meta_lookup(mctx, id);
    if (!e || e->state == META_SOLVED) return false;
    // Occurs check: the solution must not mention meta id itself
    EvalEnv *empty = eval_env_empty();
    bool occurs = meta_occurs(mctx, id, dep_eval(solution, empty, mctx));
    eval_env_free(empty);
    if (occurs) return false;
    e->state    = META_SOLVED;
    e->solution = term_clone(solution);
    return true;
}

bool term_occurs_meta(int id, Term *t) {
    if (!t) return false;
    switch (t->kind) {
        case TERM_META: return t->meta_id == id;
        case TERM_PI:
        case TERM_LAM:
        case TERM_SIGMA:
            return term_occurs_meta(id, t->binder_dom) || term_occurs_meta(id, t->binder_body);
        case TERM_APP:
            if (term_occurs_meta(id, t->app_fn)) return true;
            for (int i = 0; i < t->app_argc; i++) {
                if (term_occurs_meta(id, t->app_args[i])) return true;
            }
            return false;
        case TERM_LET:
            return term_occurs_meta(id, t->let_type) || term_occurs_meta(id, t->let_val) || term_occurs_meta(id, t->binder_body);
        case TERM_PAIR:
            return term_occurs_meta(id, t->pair_fst) || term_occurs_meta(id, t->pair_snd) || term_occurs_meta(id, t->pair_type);
        case TERM_FST:
        case TERM_SND:
            return term_occurs_meta(id, t->pair_fst);
        case TERM_EQ:
            return term_occurs_meta(id, t->eq_lhs) || term_occurs_meta(id, t->eq_rhs) || term_occurs_meta(id, t->eq_type);
        case TERM_REFL:
            return term_occurs_meta(id, t->refl_val);
        case TERM_SUBST:
            return term_occurs_meta(id, t->subst_proof) || term_occurs_meta(id, t->subst_motive) || term_occurs_meta(id, t->subst_base);
        case TERM_IF:
            return term_occurs_meta(id, t->if_cond) || term_occurs_meta(id, t->if_then) || term_occurs_meta(id, t->if_else);
        case TERM_SUCC_T:
            return term_occurs_meta(id, t->succ_pred);
        case TERM_NAT_ELIM:
            return term_occurs_meta(id, t->nelim_motive) || term_occurs_meta(id, t->nelim_zero) || term_occurs_meta(id, t->nelim_succ) || term_occurs_meta(id, t->nelim_arg);
        case TERM_ANN:
            return term_occurs_meta(id, t->ann_term) || term_occurs_meta(id, t->ann_type);
        default: return false;
    }
}

//  meta_occurs: check whether meta `id` appears in value `v`.
//  Uses dep_quote to expand closures and check full spines safely.
//
bool meta_occurs(MetaCtx *mctx, int id, Value *v) {
    if (!v) return false;
    Term *quoted = dep_quote(v, 0, mctx);
    bool occurs = term_occurs_meta(id, quoted);
    term_free(quoted);
    return occurs;
}

bool meta_all_solved(MetaCtx *mctx) {
    for (int i = 0; i < mctx->count; i++)
        if (mctx->entries[i].state == META_UNSOLVED) return false;
    return true;
}


/// Evaluation — NbE  (Normalisation by Evaluation)

//  dep_force: if v is a solved meta applied to a spine, substitute and
//  reduce.  Otherwise return v unchanged.  This is the "forcing" step
//  that drives the conversion check forward when metas are solved.
//
Value *dep_force(Value *v, MetaCtx *mctx) {
    if (!v || !mctx) return v;
    if (v->kind != VAL_META) return v;
    MetaEntry *e = meta_lookup(mctx, v->meta_id);
    if (!e || e->state != META_SOLVED) return v;
    // Re-evaluate the solution in the empty environment, then apply spine
    EvalEnv *empty = eval_env_empty();
    Value *result = dep_eval(e->solution, empty, mctx);
    eval_env_free(empty);
    for (int i = 0; i < v->spine.count; i++) {
        if (result->kind == VAL_LAM) {
            result = dep_closure_apply(result->closure, v->spine.args[i]);
        } else {
            // Neutral application: rebuild a neutral spine
            Spine sp = val_spine_empty();
            val_spine_push(&sp, v->spine.args[i]);
            /* This path means the meta solved to a non-function — the
             * type checker should catch this as a type error separately  */
            return result;
        }
    }
    return result;
}

//  dep_eval: evaluate term t in environment env to a Value.
//  env maps De Bruijn indices to Values via eval_env_lookup.
//  mctx may be NULL (no metavariable resolution during pure evaluation).
//
Value *dep_eval(Term *t, EvalEnv *env, MetaCtx *mctx) {
    if (!t) return val_universe_n(0);

    switch (t->kind) {

    // ── Atoms ────────────────────────────────────────────────────
    case TERM_BVAR: {
        Value *v = eval_env_lookup(env, t->bvar_index);
        return v ? v : val_neutral("?bvar", -1, val_spine_empty());
    }

    case TERM_FVAR: {
        /* Free variables evaluate to themselves as neutrals.
         * δ-unfolding (definitions) happens in dep_conv when both sides
         * are neutrals with the same head — we look up the definition
         * and reduce if available.                                       */
        return val_neutral(t->fvar_name, -1, val_spine_empty());
    }

    case TERM_TYPE:
        return val_universe(level_clone(t->type_level));

    case TERM_KIND:
        return val_universe(level_const(DEP_LEVEL_MAX));

    case TERM_NAT:   return val_nat();
    case TERM_ZERO:  return val_zero();
    case TERM_EMBED: return val_embed(t->embed_type);

    case TERM_META: {
        if (mctx) {
            MetaEntry *e = meta_lookup(mctx, t->meta_id);
            if (e && e->state == META_SOLVED) {
                EvalEnv *empty = eval_env_empty();
                Value *v = dep_eval(e->solution, empty, mctx);
                eval_env_free(empty);
                return v;
            }
        }
        return val_meta(t->meta_id, val_spine_empty());
    }

    case TERM_HOLE:
        return val_meta(-1, val_spine_empty());

    // ── Binders — create closures ─────────────────────────────────
    case TERM_PI: {
        Value   *dom  = dep_eval(t->binder_dom, env, mctx);
        Closure  body = { .env = eval_env_clone(env), .body = t->binder_body };
        return val_pi(t->binder_name, dom, body, t->implicit);
    }

    case TERM_LAM: {
        Value   *dom  = dep_eval(t->binder_dom, env, mctx);
        Closure  body = { .env = eval_env_clone(env), .body = t->binder_body };
        return val_lam(t->binder_name, dom, body);
    }

    case TERM_SIGMA: {
        Value   *dom  = dep_eval(t->binder_dom, env, mctx);
        Closure  body = { .env = eval_env_clone(env), .body = t->binder_body };
        return val_sigma(t->binder_name, dom, body);
    }

    // ── Application — β-reduction ─────────────────────────────────
    case TERM_APP: {
        Value *fn = dep_eval(t->app_fn, env, mctx);
        for (int i = 0; i < t->app_argc; i++) {
            Value *arg = dep_eval(t->app_args[i], env, mctx);
            fn = dep_force(fn, mctx);
            if (fn->kind == VAL_LAM) {
                // β-reduction: substitute arg into the closure body
                fn = dep_closure_apply(fn->closure, arg);
            } else if (fn->kind == VAL_NEUTRAL) {
                // Accumulate onto the neutral spine
                val_spine_push(&fn->spine, arg);
            } else if (fn->kind == VAL_META) {
                val_spine_push(&fn->spine, arg);
            } else {
                /* Type ERROR: applying a non-function.  The type checker
                 * should have caught this; produce a neutral to continue. */
                Spine sp = val_spine_empty();
                val_spine_push(&sp, arg);
                fn = val_neutral("!not-a-function", -1, sp);
            }
        }
        return fn;
    }

    // ── Pairs ─────────────────────────────────────────────────────
    case TERM_PAIR:
        return val_pair(dep_eval(t->pair_fst,  env, mctx),
                        dep_eval(t->pair_snd,  env, mctx));

    case TERM_FST: {
        Value *p = dep_eval(t->pair_fst, env, mctx);
        p = dep_force(p, mctx);
        if (p->kind == VAL_PAIR) return p->fst;
        // Neutral fst
        Spine sp = val_spine_empty();
        char name[64]; snprintf(name, 64, "fst!");
        return val_neutral(name, -1, sp);
    }

    case TERM_SND: {
        Value *p = dep_eval(t->pair_fst, env, mctx);
        p = dep_force(p, mctx);
        if (p->kind == VAL_PAIR) return p->snd;
        Spine sp = val_spine_empty();
        return val_neutral("snd!", -1, sp);
    }

    // ── Equality ──────────────────────────────────────────────────
    case TERM_EQ:
        return val_eq(dep_eval(t->eq_lhs,  env, mctx),
                      dep_eval(t->eq_rhs,  env, mctx),
                      dep_eval(t->eq_type, env, mctx));

    case TERM_REFL:
        return val_refl(dep_eval(t->refl_val, env, mctx));

    case TERM_SUBST: {
        // subst h P pa :  if h reduces to refl then result is pa
        Value *h = dep_eval(t->subst_proof, env, mctx);
        h = dep_force(h, mctx);
        if (h->kind == VAL_REFL) {
            return dep_eval(t->subst_base, env, mctx);
        }
        return val_neutral("subst!", -1, val_spine_empty());
    }

    case TERM_IF: {
        Value *cond = dep_eval(t->if_cond, env, mctx);
        cond = dep_force(cond, mctx);
        // We lack runtime evaluation of True/False currently. It stays stuck as VAL_IF.
        return val_if(cond, dep_eval(t->if_then, env, mctx), dep_eval(t->if_else, env, mctx));
    }

    // ── Nat ───────────────────────────────────────────────────────
    case TERM_SUCC_T:
        return val_succ(dep_eval(t->succ_pred, env, mctx));

    case TERM_NUM_LIT:
        return val_num_lit(t->num_lit);

    case TERM_NAT_ELIM: {
        //  Nat-elim P pz ps n  reduces when n is a concrete numeral.
        Value *n  = dep_eval(t->nelim_arg, env, mctx);
        n = dep_force(n, mctx);
        if (n->kind == VAL_ZERO || (n->kind == VAL_NUM_LIT && n->num_lit == 0)) {
            return dep_eval(t->nelim_zero, env, mctx);
        }
        if (n->kind == VAL_SUCC || (n->kind == VAL_NUM_LIT && n->num_lit > 0)) {
            //  ps  :  Π(n:Nat). P n → P (succ n)
            //  Result = ((ps pred) (Nat-elim P pz ps pred))
            Value *ps   = dep_eval(t->nelim_succ,  env, mctx);
            Value *pz   = dep_eval(t->nelim_zero,  env, mctx);
            Value *moti = dep_eval(t->nelim_motive, env, mctx);

            Value *pred_val;
            Term  *pred_term;
            if (n->kind == VAL_SUCC) {
                pred_val = n->pred;
                pred_term = dep_quote(n->pred, env ? env->level : 0, mctx);
            } else {
                pred_val = val_num_lit(n->num_lit - 1);
                pred_term = term_num_lit(n->num_lit - 1);
            }

            // Build the recursive call on the predecessor
            Term *rec_call = term_nat_elim(term_clone(t->nelim_motive),
                                           term_clone(t->nelim_zero),
                                           term_clone(t->nelim_succ),
                                           pred_term);
            Value *rec_val = dep_eval(rec_call, env, mctx);
            term_free(rec_call);
            // Apply ps to predecessor, then to recursive result
            Value *step1 = dep_closure_apply(ps->closure, pred_val);
            Value *step2 = dep_closure_apply(step1->closure, rec_val);

            if (n->kind == VAL_NUM_LIT) {
                val_free(pred_val);
            }

            (void)moti; (void)pz;
            return step2;
        }
        // Stuck: n is neutral
        return val_neutral("Nat-elim!", -1, val_spine_empty());
    }

    // ── Let ───────────────────────────────────────────────────────
    case TERM_LET: {
        Value   *v   = dep_eval(t->let_val, env, mctx);
        EvalEnv *ne  = eval_env_extend(env, v);
        Value   *res = dep_eval(t->binder_body, ne, mctx);
        eval_env_discard_top(ne);
        return res;
    }

    // ── Annotation — transparent ──────────────────────────────────
    case TERM_ANN:
        return dep_eval(t->ann_term, env, mctx);
    }

    return val_universe_n(0);
}

//  dep_quote: reify a value back into a term at binding depth `depth`.
//  Fresh variables are introduced for binders to ensure the result is
//  in β-normal η-long form.
//
//  The η-expansion rules are:
//    · Functions:  quote(v) = λ(x:A). quote(v (neutral x))
//    · Pairs:      quote(v) = ⟨quote(fst v), quote(snd v)⟩
//  These ensure that all values are quoting-consistent.
//
Term *dep_quote(Value *v, int depth, MetaCtx *mctx) {
    if (!v) return term_type_n(0);
    v = dep_force(v, mctx);

    switch (v->kind) {

    case VAL_UNIVERSE:
        return term_type(level_clone(v->level));

    case VAL_NAT:   return term_nat();
    case VAL_ZERO:  return term_zero();
    case VAL_EMBED: return term_embed(v->embed_type);

    case VAL_SUCC:
        return term_succ(dep_quote(v->pred, depth, mctx));

    case VAL_NUM_LIT:
        return term_num_lit(v->num_lit);

    // η-expand lambda values into explicit lambdas
    case VAL_LAM: {
        Term  *dom  = dep_quote(v->domain, depth, mctx);
        // Fresh neutral variable at this depth
        char   fresh_name[32];
        snprintf(fresh_name, sizeof(fresh_name), "x%d", depth);
        Value *fresh_var  = val_neutral(fresh_name, depth, val_spine_empty());
        Value *body_val   = dep_closure_apply(v->closure, fresh_var);
        Term  *body_term  = dep_quote(body_val, depth + 1, mctx);
        return term_lam(v->binder_name, dom, body_term);
    }

    // η-expand Π-types
    case VAL_PI: {
        Term  *dom  = dep_quote(v->domain, depth, mctx);
        char   fresh_name[32];
        snprintf(fresh_name, sizeof(fresh_name), "x%d", depth);
        Value *fresh_var  = val_neutral(fresh_name, depth, val_spine_empty());
        Value *codom_val  = dep_closure_apply(v->closure, fresh_var);
        Term  *codom_term = dep_quote(codom_val, depth + 1, mctx);
        return term_pi(v->binder_name, dom, codom_term, v->implicit);
    }

    case VAL_SIGMA: {
        Term  *dom  = dep_quote(v->domain, depth, mctx);
        char   fresh_name[32];
        snprintf(fresh_name, sizeof(fresh_name), "x%d", depth);
        Value *fresh_var  = val_neutral(fresh_name, depth, val_spine_empty());
        Value *codom_val  = dep_closure_apply(v->closure, fresh_var);
        Term  *codom_term = dep_quote(codom_val, depth + 1, mctx);
        return term_sigma(v->binder_name, dom, codom_term);
    }

    // η-expand pairs: always expand to ⟨fst v, snd v⟩
    case VAL_PAIR:
        return term_pair(dep_quote(v->fst, depth, mctx),
                         dep_quote(v->snd, depth, mctx),
                         NULL);  // type will be re-inferred

    case VAL_EQ:
        return term_eq(dep_quote(v->eq_lhs,      depth, mctx),
                       dep_quote(v->eq_rhs,      depth, mctx),
                       dep_quote(v->eq_type_val, depth, mctx));

    case VAL_REFL: {
        Term *r = term_alloc(TERM_REFL);
        r->refl_val = dep_quote(v->refl_val, depth, mctx);
        return r;
    }

    case VAL_IF:
        return term_if(dep_quote(v->if_cond_val, depth, mctx),
                       dep_quote(v->if_then_val, depth, mctx),
                       dep_quote(v->if_else_val, depth, mctx));

    /* Neutral: a free variable applied to a spine of arguments.
     * With neutral_level, we can now emit perfectly valid De Bruijn
     * indices (TERM_BVAR) for mathematically sound scope checking! */
    case VAL_NEUTRAL: {
        Term *head;
        if (v->neutral_level >= 0) {
            head = term_bvar(depth - v->neutral_level - 1);
        } else {
            head = term_fvar(v->neutral_name);
        }
        if (v->spine.count == 0) return head;
        Term **args = malloc(sizeof(Term *) * v->spine.count);
        for (int i = 0; i < v->spine.count; i++)
            args[i] = dep_quote(v->spine.args[i], depth, mctx);
        return term_app(head, args, v->spine.count);
    }

    case VAL_META: {
        MetaEntry *e = mctx ? meta_lookup(mctx, v->meta_id) : NULL;
        if (e && e->state == META_SOLVED) {
            Value *sol = dep_eval(e->solution, eval_env_empty(), mctx);
            for (int i = 0; i < v->spine.count; i++)
                sol = dep_closure_apply(sol->closure, v->spine.args[i]);
            return dep_quote(sol, depth, mctx);
        }
        return term_meta(v->meta_id);
    }
    }

    return term_type_n(0);
}

Term *dep_normalise(Term *t, EvalEnv *env, MetaCtx *mctx) {
    Value *v = dep_eval(t, env, mctx);
    return dep_quote(v, env ? env->level : 0, mctx);
}


/// Definitional equality  — conversion checking

ConvCtx conv_ctx_make(DepCtx *dctx, int depth) {
    ConvCtx c;
    c.dctx      = dctx;
    c.depth     = depth;
    c.fuel      = DEP_CONV_FUEL;
    c.had_error = false;
    c.error_msg[0] = '\0';
    return c;
}

//  dep_conv_vals: the core conversion algorithm.
//
//  Algorithm (following Coquand's algorithm for intensional type theory):
//    1. Force both values (resolve solved metas).
//    2. If either is a λ, η-expand both and recurse under a fresh variable.
//    3. If both are Π or Σ, check domains then codomain under a fresh var.
//    4. If both are neutrals with the same head, zip their spines.
//    5. If one is an unsolved meta, attempt to unify.
//    6. Otherwise, fail.
//
static bool dep_conv_vals(ConvCtx *cctx, Value *v1, Value *v2, Value *ty);

static bool embedded_type_arg_compatible(Type *a, Type *b) {
    if (!a || !b) return true;
    if (a->kind == TYPE_UNKNOWN || b->kind == TYPE_UNKNOWN) return true;
    if (a->kind == TYPE_VAR || b->kind == TYPE_VAR) return true;
    return types_equal(a, b);
}

static Type *embedded_collection_elem(Type *t) {
    if (!t) return NULL;
    if (t->kind == TYPE_COLL) return t->element_type;
    if (t->kind == TYPE_ARR) return t->arr_element_type;
    if (t->kind == TYPE_LIST && t->list_count == 1) return t->list_elem;
    return NULL;
}

static bool embedded_coll_compatible(Type *a, Type *b) {
    if (!a || !b) return false;

    if (a->kind == TYPE_COLL && b->kind == TYPE_COLL) {
        return embedded_type_arg_compatible(a->element_type,
                                            b->element_type);
    }

    if (a->kind == TYPE_COLL &&
        (b->kind == TYPE_ARR || b->kind == TYPE_LIST || b->kind == TYPE_STRING)) {
        return embedded_type_arg_compatible(a->element_type,
                                            embedded_collection_elem(b));
    }

    if (b->kind == TYPE_COLL &&
        (a->kind == TYPE_ARR || a->kind == TYPE_LIST || a->kind == TYPE_STRING)) {
        return embedded_type_arg_compatible(b->element_type,
                                            embedded_collection_elem(a));
    }

    return false;
}

static bool embedded_adt_type_compatible(Type *a, Type *b) {
    if (!a || !b) return false;
    if (types_equal(a, b)) return true;
    if (embedded_coll_compatible(a, b)) return true;

    if (a->kind == TYPE_APP && b->kind == TYPE_LAYOUT) {
        if (a->app_constructor && b->layout_name &&
            strcmp(a->app_constructor, b->layout_name) == 0)
            return true;
        const char *bname = b->layout_name ? b->layout_name : "";
        if (strncmp(bname, "__type_", 7) == 0 && a->app_constructor &&
            strcmp(a->app_constructor, bname + 7) == 0)
            return true;
    }

    if (b->kind == TYPE_APP && a->kind == TYPE_LAYOUT) {
        if (b->app_constructor && a->layout_name &&
            strcmp(b->app_constructor, a->layout_name) == 0)
            return true;
        const char *aname = a->layout_name ? a->layout_name : "";
        if (strncmp(aname, "__type_", 7) == 0 && b->app_constructor &&
            strcmp(b->app_constructor, aname + 7) == 0)
            return true;
    }

    return false;
}

static bool embedded_adt_matches_neutral(Type *embed_type, Value *neutral) {
    if (!embed_type || !neutral) return false;
    if (neutral->kind != VAL_NEUTRAL) return false;
    if (neutral->spine.count != 0) return false;
    if (!neutral->neutral_name) return false;

    const char *name = neutral->neutral_name;
    if (strncmp(name, "__type_", 7) == 0) {
        name += 7;
    }

    if (embed_type->kind == TYPE_APP &&
        embed_type->app_constructor &&
        strcmp(embed_type->app_constructor, name) == 0) {
        return true;
    }

    if (embed_type->kind == TYPE_LAYOUT &&
        embed_type->layout_name) {
        const char *layout_name = embed_type->layout_name;
        if (strncmp(layout_name, "__type_", 7) == 0) {
            layout_name += 7;
        }
        if (strcmp(layout_name, name) == 0) {
            return true;
        }
    }

    return false;
}

//  Apply a fresh neutral at depth `cctx->depth` to extend the context
static Value *fresh_neutral(ConvCtx *cctx, const char *hint) {
    char name[64];
    snprintf(name, sizeof(name), "%s!%d", hint ? hint : "x", cctx->depth);
    return val_neutral(name, cctx->depth, val_spine_empty());
}

static bool dep_conv_vals_internal(ConvCtx *cctx, Value *v1, Value *v2, Value *ty);

static bool dep_conv_vals(ConvCtx *cctx, Value *v1, Value *v2, Value *ty) {
    if (cctx->had_error) return false;

    MetaCtx *mctx = cctx->dctx ? cctx->dctx->mctx : NULL;
    if (g_trace_enabled) {
        Term *q1 = dep_quote(v1, cctx->depth, mctx);
        Term *q2 = dep_quote(v2, cctx->depth, mctx);
        shared_trace_indent();
        fprintf(stderr, "├─ \033[35mConv \033[0m %s ≡ %s\n", term_to_string(q1), term_to_string(q2));
        term_free(q1);
        term_free(q2);
        g_trace_depth++;
    }

    bool res = dep_conv_vals_internal(cctx, v1, v2, ty);

    if (g_trace_enabled) {
        g_trace_depth--;
        shared_trace_indent();
        if (res) {
            fprintf(stderr, "└─ \033[32mOK\033[0m\n");
        } else {
            fprintf(stderr, "└─ \033[31mFAIL\033[0m\n");
        }
    }
    return res;
}

static bool dep_conv_vals_internal(ConvCtx *cctx, Value *v1, Value *v2, Value *ty) {
    if (--cctx->fuel <= 0) {
        snprintf(cctx->error_msg, sizeof(cctx->error_msg),
                 "dep: conversion check ran out of fuel (possible non-termination)");
        cctx->had_error = true;
        return false;
    }

    MetaCtx *mctx = cctx->dctx ? cctx->dctx->mctx : NULL;

    v1 = dep_force(v1, mctx);
    v2 = dep_force(v2, mctx);

    /* Hole sentinel (-1): always succeeds — it is an intentional wildcard */
    if (v1 && v1->kind == VAL_META && v1->meta_id == -1) return true;
    if (v2 && v2->kind == VAL_META && v2->meta_id == -1) return true;

    /* Fast-path and occurs-check bypass for identical metavariables */
    if (v1 && v2 && v1->kind == VAL_META && v2->kind == VAL_META && v1->meta_id == v2->meta_id) {
        if (v1->spine.count != v2->spine.count) {
            snprintf(cctx->error_msg, sizeof(cctx->error_msg),
                     "dep: spine length mismatch for meta ?%d", v1->meta_id);
            cctx->had_error = true;
            return false;
        }
        for (int i = 0; i < v1->spine.count; i++) {
            if (!dep_conv_vals(cctx, v1->spine.args[i], v2->spine.args[i], NULL)) return false;
        }
        return true;
    }

    /* Solve metavariables when one side is an unsolved meta.
     * Simple pattern: meta ?n applied to distinct neutral variables.
     * This is "pattern unification" — the most tractable fragment.     */
    if (v1 && v1->kind == VAL_META) {
        MetaEntry *e = mctx ? meta_lookup(mctx, v1->meta_id) : NULL;
        if (e && e->state == META_UNSOLVED) {
            /* TODO (Idris 2 Level): Proper pattern unification requires checking that v1->spine
             * contains only distinct bound variables, and pruning variables not in scope. */
            Term *sol = dep_quote(v2, cctx->depth, mctx);
            bool ok = meta_solve(mctx, v1->meta_id, sol);
            if (!ok) term_free(sol);
            return ok;
        }
    }
    if (v2 && v2->kind == VAL_META) {
        MetaEntry *e = mctx ? meta_lookup(mctx, v2->meta_id) : NULL;
        if (e && e->state == META_UNSOLVED) {
            Term *sol = dep_quote(v1, cctx->depth, mctx);
            bool ok = meta_solve(mctx, v2->meta_id, sol);
            if (!ok) term_free(sol);
            return ok;
        }
    }

    if (!v1 || !v2) return v1 == v2;

    /* delta-unfold refinement/alias neutrals early, before the kind check.
     * When one side is a named neutral (e.g. N) that resolves to a ground
     * embed (e.g. val_embed(Int)), unfold it so Nat vs N succeeds. */
    if (v1->kind == VAL_NEUTRAL && v1->spine.count == 0 && cctx->dctx) {
        DepEnvEntry *e = dep_env_lookup(cctx->dctx->globals, v1->neutral_name);
        if (e && e->def_val && e->def_val->kind == VAL_EMBED) {
            return dep_conv_vals(cctx, e->def_val, v2, ty);
        }
    }
    if (v2->kind == VAL_NEUTRAL && v2->spine.count == 0 && cctx->dctx) {
        DepEnvEntry *e = dep_env_lookup(cctx->dctx->globals, v2->neutral_name);
        if (e && e->def_val && e->def_val->kind == VAL_EMBED) {
            return dep_conv_vals(cctx, v1, e->def_val, ty);
        }
    }

    // η-expand functions: compare (λx.v1 x) and (λx.v2 x)
    if (v1->kind == VAL_LAM || v2->kind == VAL_LAM) {
        Value *var = fresh_neutral(cctx, "η");
        // η-expand whichever side is not already a lambda
        Value *body1;
        if (v1->kind == VAL_LAM) {
            body1 = dep_closure_apply(v1->closure, var);
        } else {
            Spine sp = val_spine_clone(v1->spine);
            val_spine_push(&sp, var);
            body1 = val_neutral(v1->neutral_name, v1->neutral_level, sp);
        }

        Value *body2;
        if (v2->kind == VAL_LAM) {
            body2 = dep_closure_apply(v2->closure, var);
        } else {
            Spine sp = val_spine_clone(v2->spine);
            val_spine_push(&sp, var);
            body2 = val_neutral(v2->neutral_name, v2->neutral_level, sp);
        }
        // The return type is the codomain applied to `var
        Value *codom = NULL;
        if (ty && ty->kind == VAL_PI)
            codom = dep_closure_apply(ty->closure, var);
        cctx->depth++;
        bool ok = dep_conv_vals(cctx, body1, body2, codom);
        cctx->depth--;
        return ok;
    }

    // η-expand pairs
    if (v1->kind == VAL_PAIR || v2->kind == VAL_PAIR) {
        Value *fst1 = (v1->kind == VAL_PAIR) ? v1->fst : val_neutral("fst!", -1, val_spine_empty());
        Value *snd1 = (v1->kind == VAL_PAIR) ? v1->snd : val_neutral("snd!", -1, val_spine_empty());
        Value *fst2 = (v2->kind == VAL_PAIR) ? v2->fst : val_neutral("fst!", -1, val_spine_empty());
        Value *snd2 = (v2->kind == VAL_PAIR) ? v2->snd : val_neutral("snd!", -1, val_spine_empty());
        Value *fst_ty = NULL, *snd_ty = NULL;
        if (ty && ty->kind == VAL_SIGMA) {
            fst_ty = ty->domain;
            snd_ty = dep_closure_apply(ty->closure, fst1);
        }
        return dep_conv_vals(cctx, fst1, fst2, fst_ty)
            && dep_conv_vals(cctx, snd1, snd2, snd_ty);
    }

    if (v1->kind != v2->kind) {
        if (v1->kind == VAL_EMBED && v2->kind == VAL_NEUTRAL &&
            embedded_adt_matches_neutral(v1->embed_type, v2)) {
            return true;
        }
        if (v2->kind == VAL_EMBED && v1->kind == VAL_NEUTRAL &&
            embedded_adt_matches_neutral(v2->embed_type, v1)) {
            return true;
        }

        // Unify numbers across representations
        if (v1->kind == VAL_NUM_LIT && v1->num_lit == 0 && v2->kind == VAL_ZERO) return true;
        if (v2->kind == VAL_NUM_LIT && v2->num_lit == 0 && v1->kind == VAL_ZERO) return true;
        if (v1->kind == VAL_NUM_LIT && v1->num_lit > 0 && v2->kind == VAL_SUCC) {
            Value *pred1 = val_num_lit(v1->num_lit - 1);
            bool ok = dep_conv_vals(cctx, pred1, v2->pred, val_nat());
            val_free(pred1);
            return ok;
        }
        if (v2->kind == VAL_NUM_LIT && v2->num_lit > 0 && v1->kind == VAL_SUCC) {
            Value *pred2 = val_num_lit(v2->num_lit - 1);
            bool ok = dep_conv_vals(cctx, v1->pred, pred2, val_nat());
            val_free(pred2);
            return ok;
        }

        // Embed vs ground — compare underlying types
        if (v1->kind == VAL_EMBED && v2->kind == VAL_EMBED) {
            if (embedded_adt_type_compatible(v1->embed_type, v2->embed_type)) return true;
            int k1 = v1->embed_type ? v1->embed_type->kind : -1;
            int k2 = v2->embed_type ? v2->embed_type->kind : -1;
            if ((k1 == TYPE_INT && k2 == TYPE_CHAR) || (k1 == TYPE_CHAR && k2 == TYPE_INT)) return true;
            if ((k1 == TYPE_INT_ARBITRARY && k2 == TYPE_CHAR) || (k1 == TYPE_CHAR && k2 == TYPE_INT_ARBITRARY)) return true;
            /* Allow implicit Int <-> Float coercion in dependent checker */
            if ((k1 == TYPE_INT && k2 == TYPE_FLOAT) || (k1 == TYPE_FLOAT && k2 == TYPE_INT)) return true;
            if ((k1 == TYPE_INT_ARBITRARY && k2 == TYPE_FLOAT) || (k1 == TYPE_FLOAT && k2 == TYPE_INT_ARBITRARY)) return true;
            if ((k1 == TYPE_INT && k2 == TYPE_F80) || (k1 == TYPE_F80 && k2 == TYPE_INT)) return true;
            if ((k1 == TYPE_INT_ARBITRARY && k2 == TYPE_F80) || (k1 == TYPE_F80 && k2 == TYPE_INT_ARBITRARY)) return true;
            return false;
        }

        // Allow String to act as Coll in the dependent checker (HM handles the exact element types)
        if (v1->kind == VAL_EMBED && v1->embed_type && v1->embed_type->kind == TYPE_STRING &&
            v2->kind == VAL_NEUTRAL && v2->neutral_name && strcmp(v2->neutral_name, "Coll") == 0) return true;
        if (v2->kind == VAL_EMBED && v2->embed_type && v2->embed_type->kind == TYPE_STRING &&
            v1->kind == VAL_NEUTRAL && v1->neutral_name && strcmp(v1->neutral_name, "Coll") == 0) return true;

        // Allow Nat (literals < 64) to implicitly coerce to Int, Char, and Float signatures smoothly
        bool v1_nat = (v1->kind == VAL_NAT || v1->kind == VAL_SUCC || v1->kind == VAL_ZERO || v1->kind == VAL_NUM_LIT);
        bool v2_nat = (v2->kind == VAL_NAT || v2->kind == VAL_SUCC || v2->kind == VAL_ZERO || v2->kind == VAL_NUM_LIT);
        #define IS_NUMERIC_EMBED(v) ((v)->kind == VAL_EMBED && (v)->embed_type && \
            ((v)->embed_type->kind == TYPE_INT        || \
             (v)->embed_type->kind == TYPE_INT_ARBITRARY || \
             (v)->embed_type->kind == TYPE_CHAR       || \
             (v)->embed_type->kind == TYPE_FLOAT      || \
             (v)->embed_type->kind == TYPE_F80        || \
             (v)->embed_type->kind == TYPE_I8         || \
             (v)->embed_type->kind == TYPE_U8         || \
             (v)->embed_type->kind == TYPE_I16        || \
             (v)->embed_type->kind == TYPE_U16        || \
             (v)->embed_type->kind == TYPE_I32        || \
             (v)->embed_type->kind == TYPE_U32        || \
             (v)->embed_type->kind == TYPE_I64        || \
             (v)->embed_type->kind == TYPE_U64        || \
             (v)->embed_type->kind == TYPE_I128       || \
             (v)->embed_type->kind == TYPE_U128       || \
             (v)->embed_type->kind == TYPE_F32))
        bool v1_target = IS_NUMERIC_EMBED(v1);
        bool v2_target = IS_NUMERIC_EMBED(v2);
        #undef IS_NUMERIC_EMBED

        if ((v1_nat && v2_target) || (v1_target && v2_nat)) return true;

        snprintf(cctx->error_msg, sizeof(cctx->error_msg),
                 "Structural mismatch between '%s' and '%s'",
                 term_to_string(dep_quote(v1, cctx->depth, mctx)),
                 term_to_string(dep_quote(v2, cctx->depth, mctx)));
        cctx->had_error = true;
        return false;
    }

    switch (v1->kind) {
    case VAL_UNIVERSE:
        if (!levels_equal(v1->level, v2->level)) {
            snprintf(cctx->error_msg, sizeof(cctx->error_msg),
                     "dep: universe level mismatch: Type %s ≠ Type %s",
                     level_to_string(v1->level), level_to_string(v2->level));
            cctx->had_error = true;
            return false;
        }
        return true;

    case VAL_NAT:
    case VAL_ZERO:
        return true;

    case VAL_SUCC:
        return dep_conv_vals(cctx, v1->pred, v2->pred, val_nat());

    case VAL_NUM_LIT:
        return v1->num_lit == v2->num_lit;

    case VAL_PI:
    case VAL_SIGMA: {
        // Check domains, then codomains under a fresh variable
        bool dom_ok = dep_conv_vals(cctx, v1->domain, v2->domain, NULL);
        if (!dom_ok) return false;
        Value *var    = fresh_neutral(cctx, v1->binder_name);
        Value *cod1   = dep_closure_apply(v1->closure, var);
        Value *cod2   = dep_closure_apply(v2->closure, var);
        cctx->depth++;
        bool cod_ok   = dep_conv_vals(cctx, cod1, cod2, NULL);
        cctx->depth--;
        return cod_ok;
    }

    case VAL_EQ:
        return dep_conv_vals(cctx, v1->eq_lhs,      v2->eq_lhs,      v1->eq_type_val)
            && dep_conv_vals(cctx, v1->eq_rhs,      v2->eq_rhs,      v1->eq_type_val)
            && dep_conv_vals(cctx, v1->eq_type_val, v2->eq_type_val, NULL);

    case VAL_REFL:
        return dep_conv_vals(cctx, v1->refl_val, v2->refl_val, NULL);

    case VAL_IF:
        return dep_conv_vals(cctx, v1->if_cond_val, v2->if_cond_val, val_embed(type_bool()))
            && dep_conv_vals(cctx, v1->if_then_val, v2->if_then_val, ty)
            && dep_conv_vals(cctx, v1->if_else_val, v2->if_else_val, ty);

    case VAL_EMBED: {
        if (embedded_adt_type_compatible(v1->embed_type, v2->embed_type)) return true;

        /* Implicit coercions for characters acting as small integers */
        int k1 = v1->embed_type->kind;
        int k2 = v2->embed_type->kind;
        if ((k1 == TYPE_INT && k2 == TYPE_CHAR) || (k1 == TYPE_CHAR && k2 == TYPE_INT)) return true;
        if ((k1 == TYPE_INT_ARBITRARY && k2 == TYPE_CHAR) || (k1 == TYPE_CHAR && k2 == TYPE_INT_ARBITRARY)) return true;

        /* Pointer :: ? unifies with Pointer :: T when ? is an unknown pointee.
         * This happens when *U8 on a parameter resolves to Pointer :: unknown
         * because the inner type annotation was lost. Accept structurally. */
        if (k1 == TYPE_PTR && k2 == TYPE_PTR) {
            Type *p1 = v1->embed_type->element_type;
            Type *p2 = v2->embed_type->element_type;
            if (!p1 || p1->kind == TYPE_UNKNOWN) return true;
            if (!p2 || p2->kind == TYPE_UNKNOWN) return true;
            return types_equal(p1, p2);
        }

        return false;
    }

    case VAL_NEUTRAL: {
        // Both neutral: heads must match, spines must match element-wise
        const char *n1 = v1->neutral_name ? v1->neutral_name : "";
        const char *n2 = v2->neutral_name ? v2->neutral_name : "";
        if (strcmp(n1, n2) != 0) {
            // Try δ-unfolding both heads in the global env
            DepEnvEntry *e1 = (cctx->dctx && v1->neutral_name)
                ? dep_env_lookup(cctx->dctx->globals, v1->neutral_name) : NULL;
            DepEnvEntry *e2 = (cctx->dctx && v2->neutral_name)
                ? dep_env_lookup(cctx->dctx->globals, v2->neutral_name) : NULL;
            bool unfolded = false;
            if (e1 && e1->def_val && !e1->opaque) {
                Value *unf1 = e1->def_val;
                for (int i = 0; i < v1->spine.count; i++)
                    unf1 = dep_closure_apply(unf1->closure, v1->spine.args[i]);
                return dep_conv_vals(cctx, unf1, v2, ty);
            }
            if (e2 && e2->def_val && !e2->opaque) {
                Value *unf2 = e2->def_val;
                for (int i = 0; i < v2->spine.count; i++)
                    unf2 = dep_closure_apply(unf2->closure, v2->spine.args[i]);
                return dep_conv_vals(cctx, v1, unf2, ty);
            }
            (void)unfolded;
            snprintf(cctx->error_msg, sizeof(cctx->error_msg),
                     "dep: cannot unify %s with %s",
                     v1->neutral_name ? v1->neutral_name : "?null",
                     v2->neutral_name ? v2->neutral_name : "?null");
            cctx->had_error = true;
            return false;
        }
        if (v1->spine.count != v2->spine.count) {
            snprintf(cctx->error_msg, sizeof(cctx->error_msg),
                     "dep: spine length mismatch for %s: %d vs %d",
                     v1->neutral_name, v1->spine.count, v2->spine.count);
            cctx->had_error = true;
            return false;
        }
        for (int i = 0; i < v1->spine.count; i++) {
            if (!dep_conv_vals(cctx, v1->spine.args[i], v2->spine.args[i], NULL))
                return false;
        }
        return true;
    }

    default:
        return true;
    }
}

bool dep_conv(ConvCtx *cctx, Value *v1, Value *v2, Value *ty) {
    return dep_conv_vals(cctx, v1, v2, ty);
}

bool dep_conv_terms(DepCtx *dctx, Term *t1, Term *t2, int depth) {
    Value *v1  = dep_eval(t1, dctx->env, dctx->mctx);
    Value *v2  = dep_eval(t2, dctx->env, dctx->mctx);
    ConvCtx cc = conv_ctx_make(dctx, depth);
    bool ok = dep_conv(&cc, v1, v2, NULL);
    if (!ok && !dctx->had_error) {
        strncpy(dctx->error_msg, cc.error_msg, sizeof(dctx->error_msg) - 1);
        dctx->had_error = true;
    }
    return ok;
}


/// Global environment

static size_t dep_env_hash(const char *name) {
    if (!name) return 0;
    size_t h = 5381;
    for (; *name; name++) h = h * 33 ^ (unsigned char)*name;
    return h % DEP_ENV_BUCKETS;
}

DepEnv *dep_env_create(void) {
    DepEnv *e = calloc(1, sizeof(DepEnv));
    e->buckets = calloc(DEP_ENV_BUCKETS, sizeof(DepEnvEntry *));
    e->size    = DEP_ENV_BUCKETS;
    e->parent  = NULL;
    return e;
}

DepEnv *dep_env_create_child(DepEnv *parent) {
    DepEnv *e = dep_env_create();
    e->parent = parent;
    return e;
}

void dep_env_free(DepEnv *env) {
    if (!env) return;
    for (size_t i = 0; i < env->size; i++) {
        DepEnvEntry *e = env->buckets[i];
        while (e) {
            DepEnvEntry *next = e->next;
            free(e->name);
            term_free(e->def_term);
            free(e);
            e = next;
        }
    }
    free(env->buckets);
    free(env);
}

void dep_env_declare(DepEnv *env, const char *name, Value *type) {
    dep_env_define(env, name, type, NULL, NULL, false);
}

void dep_env_define(DepEnv *env, const char *name, Value *type,
                    Term *def_term, Value *def_val, bool opaque) {
    size_t idx = dep_env_hash(name);
    /* Overwrite if present */
    for (DepEnvEntry *e = env->buckets[idx]; e; e = e->next) {
        if (strcmp(e->name, name) == 0) {
            e->type     = type;
            e->def_term = def_term;
            e->def_val  = def_val;
            e->opaque   = opaque;
            return;
        }
    }
    DepEnvEntry *e = calloc(1, sizeof(DepEnvEntry));
    e->name     = strdup(name);
    e->type     = type;
    e->def_term = def_term;
    e->def_val  = def_val;
    e->opaque   = opaque;
    e->next     = env->buckets[idx];
    env->buckets[idx] = e;
}

DepEnvEntry *dep_env_lookup(DepEnv *env, const char *name) {
    if (!env || !name) return NULL;
    for (DepEnv *cur = env; cur; cur = cur->parent) {
        size_t idx = dep_env_hash(name);
        for (DepEnvEntry *e = cur->buckets[idx]; e; e = e->next)
            if (e->name && strcmp(e->name, name) == 0) return e;
    }
    return NULL;
}


/// Typing context

DepCtx *dep_ctx_create(const char *filename) {
    DepCtx *ctx    = calloc(1, sizeof(DepCtx));
    ctx->locals    = NULL;
    ctx->depth     = 0;
    ctx->globals   = dep_env_create();
    ctx->mctx      = meta_ctx_create();
    ctx->env       = eval_env_empty();
    ctx->filename  = filename ? filename : "<unknown>";
    ctx->had_error = false;
    return ctx;
}

DepCtx *dep_ctx_child(DepCtx *parent) {
    // Shallow child: shares globals, mctx, and env; gets fresh locals
    DepCtx *ctx    = calloc(1, sizeof(DepCtx));
    ctx->locals    = NULL;
    ctx->depth     = parent->depth;
    ctx->globals   = parent->globals;  // shared
    ctx->mctx      = parent->mctx;     // shared
    ctx->env       = eval_env_clone(parent->env);
    ctx->filename  = parent->filename;
    ctx->had_error = false;
    return ctx;
}

void dep_ctx_free(DepCtx *ctx) {
    if (!ctx) return;
    DepCtxEntry *e = ctx->locals;
    while (e) {
        DepCtxEntry *next = e->next;
        free(e->name);
        free(e);
        e = next;
    }
    eval_env_free(ctx->env);
    // globals and mctx are either shared or freed by the caller
    free(ctx);
}

void dep_ctx_push(DepCtx *ctx, const char *name, Value *type) {
    DepCtxEntry *e = calloc(1, sizeof(DepCtxEntry));
    e->name  = strdup(name);
    e->type  = type;
    e->val   = NULL;
    e->next  = ctx->locals;
    ctx->locals = e;

    // Extend the semantic environment with a fresh neutral
    char fresh[64];
    snprintf(fresh, sizeof(fresh), "%s!%d", name, ctx->depth);
    EvalEnv *old_env = ctx->env;
    ctx->env = eval_env_extend(old_env, val_neutral(fresh, ctx->depth, val_spine_empty()));
    free(old_env);
    ctx->depth++;
}

void dep_ctx_push_def(DepCtx *ctx, const char *name, Value *type, Value *val) {
    DepCtxEntry *e = calloc(1, sizeof(DepCtxEntry));
    e->name  = strdup(name);
    e->type  = type;
    e->val   = val;
    e->next  = ctx->locals;
    ctx->locals = e;

    EvalEnv *old_env = ctx->env;
    ctx->env = eval_env_extend(old_env, val);
    free(old_env);
    ctx->depth++;
}

void dep_ctx_pop(DepCtx *ctx) {
    if (!ctx->locals) return;
    DepCtxEntry *e = ctx->locals;
    ctx->locals = e->next;
    free(e->name);
    free(e);
    // Shrink env: rebuild without the head entry
    if (ctx->env && ctx->env->head) {
        EvalEnvEntry *old_head = ctx->env->head;
        ctx->env->head = old_head->next;
        ctx->env->level--;
        free(old_head);
    }
    if (ctx->depth > 0) ctx->depth--;
}

DepCtxEntry *dep_ctx_lookup_local(DepCtx *ctx, const char *name) {
    for (DepCtxEntry *e = ctx->locals; e; e = e->next)
        if (strcmp(e->name, name) == 0) return e;
    return NULL;
}

Value *dep_ctx_type_at_level(DepCtx *ctx, int level) {
    int i = 0;
    for (DepCtxEntry *e = ctx->locals; e; e = e->next, i++)
        if (i == (ctx->depth - 1 - level)) return e->type;
    return NULL;
}

/// Bidirectional type checker
//
//  dep_infer — Γ ⊢ t ⇒ A
//
//  Returns the synthesised type Value, or NULL on ERROR.
//  Annotates ctx->had_error on failure.
//
// dep_freshen_type: walk a Term (already quoted from a Value) and replace
// every TERM_META node with a brand-new fresh meta. This implements
// let-polymorphism: each call site to a polymorphic global gets its own
// independent unification variables, so (id "ciao") and (id 3) never
// interfere with each other's solutions.
//
// We pass a small parallel arrays of (old_id -> new_id) mappings so that
// the same meta appearing multiple times in one type (e.g. Π(x:?0).?0)
// gets consistently renamed to the same fresh meta throughout.
//
/* dep_freshen_type_val was removed; dep_freshen_term drives freshening now. */

static Term *dep_freshen_term(DepCtx *ctx, Term *t, int *old_ids, int *new_ids, int *count, int cap) {
    if (!t) return NULL;
    switch (t->kind) {
    case TERM_META: {
        int id = t->meta_id;
        // Look for an existing mapping
        for (int i = 0; i < *count; i++) {
            if (old_ids[i] == id) return term_meta(new_ids[i]);
        }
        // Create a new fresh meta
        int fresh_id = meta_fresh(ctx->mctx, val_universe_n(0), ctx->depth, "?inst");
        if (*count < cap) {
            old_ids[(*count)] = id;
            new_ids[(*count)] = fresh_id;
            (*count)++;
        }
        return term_meta(fresh_id);
    }
    case TERM_BVAR:    return term_bvar(t->bvar_index);
    case TERM_FVAR:    return term_fvar(t->fvar_name);
    case TERM_TYPE:    return term_type(level_clone(t->type_level));
    case TERM_KIND:    return term_kind();
    case TERM_NAT:     return term_nat();
    case TERM_ZERO:    return term_zero();
    case TERM_HOLE:    return term_hole();
    case TERM_EMBED:   return term_embed(t->embed_type);
    case TERM_NUM_LIT: return term_num_lit(t->num_lit);
    case TERM_PI: {
        Term *dom  = dep_freshen_term(ctx, t->binder_dom,  old_ids, new_ids, count, cap);
        Term *body = dep_freshen_term(ctx, t->binder_body, old_ids, new_ids, count, cap);
        return term_pi(t->binder_name, dom, body, t->implicit);
    }
    case TERM_LAM: {
        Term *dom  = dep_freshen_term(ctx, t->binder_dom,  old_ids, new_ids, count, cap);
        Term *body = dep_freshen_term(ctx, t->binder_body, old_ids, new_ids, count, cap);
        return term_lam(t->binder_name, dom, body);
    }
    case TERM_SIGMA: {
        Term *dom  = dep_freshen_term(ctx, t->binder_dom,  old_ids, new_ids, count, cap);
        Term *body = dep_freshen_term(ctx, t->binder_body, old_ids, new_ids, count, cap);
        return term_sigma(t->binder_name, dom, body);
    }
    case TERM_APP: {
        Term  *fn   = dep_freshen_term(ctx, t->app_fn, old_ids, new_ids, count, cap);
        Term **args = malloc(sizeof(Term *) * t->app_argc);
        for (int i = 0; i < t->app_argc; i++)
            args[i] = dep_freshen_term(ctx, t->app_args[i], old_ids, new_ids, count, cap);
        return term_app(fn, args, t->app_argc);
    }
    default:
        return term_clone(t);
    }
}

Value *dep_freshen_type(DepCtx *ctx, Term *quoted_type) {
    int old_ids[256], new_ids[256], count = 0;
    Term *fresh_term = dep_freshen_term(ctx, quoted_type, old_ids, new_ids, &count, 256);
    EvalEnv *ee = eval_env_empty();
    Value *v = dep_eval(fresh_term, ee, ctx->mctx);
    eval_env_free(ee);
    // fresh_term is intentionally not freed: v's closures may reference it
    return v;
}

static bool dep_term_numeric_index(Term *t, int *out_index);

static Value *dep_infer_internal(DepCtx *ctx, Term *t);

Value *dep_infer(DepCtx *ctx, Term *t) {
    if (!t) return val_universe_n(0);
    if (ctx->had_error) return NULL;

    if (g_trace_enabled) {
        shared_trace_indent();
        fprintf(stderr, "├─ \033[36mInfer\033[0m %s\n", term_to_string(t));
        g_trace_depth++;
    }

    Value *res = dep_infer_internal(ctx, t);

    if (res && t && t->source_ast) {
        Type *hm_ty = dep_ground_of_value_env(res, ctx->mctx, ctx->globals);
        if (hm_ty) {
            if (t->source_ast->inferred_type) type_free(t->source_ast->inferred_type);
            t->source_ast->inferred_type = hm_ty;
        }
    }

    if (g_trace_enabled) {
        g_trace_depth--;
        shared_trace_indent();
        if (res) {
            Term *q = dep_quote(res, ctx->depth, ctx->mctx);
            fprintf(stderr, "└─ \033[32mOK\033[0m: %s : %s\n", term_to_string(t), term_to_string(q));
            term_free(q);
        } else {
            fprintf(stderr, "└─ \033[31mFAIL\033[0m\n");
        }
    }
    return res;
}

static Value *dep_infer_internal(DepCtx *ctx, Term *t) {
    switch (t->kind) {

    // ── Universes ──────────────────────────────────────────────────
    case TERM_TYPE: {
        // Type u : Type (u+1)  — the universe formation rule
        int u = level_eval(t->type_level);
        if (u < 0) {
            dep_error_set(ctx, t->line, t->col,
                          "cannot evaluate universe level to a concrete integer");
            return NULL;
        }
        if (u >= DEP_LEVEL_MAX) {
            dep_error_set(ctx, t->line, t->col,
                          "universe level %d exceeds maximum %d", u, DEP_LEVEL_MAX);
            return NULL;
        }
        return val_universe_n(u + 1);
    }

    case TERM_KIND:
        return val_universe(level_const(DEP_LEVEL_MAX));

    case TERM_NAT:
        return val_universe_n(0);

    case TERM_ZERO:
        return val_nat();

    case TERM_NUM_LIT:
        return val_nat();

    case TERM_EMBED:
        // Ground types live at universe 0 by convention
        return val_universe_n(0);

    // ── Variables ──────────────────────────────────────────────────
    case TERM_BVAR: {
        Value *ty = dep_ctx_type_at_level(ctx, t->bvar_index);
        if (!ty) {
            dep_error_set(ctx, t->line, t->col,
                          "De Bruijn index %d out of range (depth %d)",
                          t->bvar_index, ctx->depth);
            return NULL;
        }
        return ty;
    }

    case TERM_FVAR: {
        // Check local context first, then globals
        DepCtxEntry *local = dep_ctx_lookup_local(ctx, t->fvar_name);
        if (local) return local->type;
        DepEnvEntry *global = dep_env_lookup(ctx->globals, t->fvar_name);
        if (global) {
            // Instantiate the global's type by quoting it and re-evaluating
            // with fresh metavariables. This is the core of let-polymorphism:
            // every call site gets an independent copy of the type variables
            // so that e.g. (id "ciao") and (id 3) can each solve ?0 freely
            // without the first call permanently specializing the function.
            Term *quoted = dep_quote(global->type, 0, ctx->mctx);
            Value *instantiated = dep_freshen_type(ctx, quoted);
            term_free(quoted);
            return instantiated;
        }

        // TEMPORARY SHADOW PASS HACK:
        // Automatically invent a hole for any unbound global function (like 'show' or 'if')
        int id = meta_fresh(ctx->mctx, val_universe_n(0), ctx->depth, t->fvar_name);
        return val_meta(id, val_spine_empty());
    }


    // ── Annotation — explicit type ascription ─────────────────────
    case TERM_ANN: {
        Value *ann_ty = dep_eval(t->ann_type, ctx->env, ctx->mctx);
        // Check the term against the ascribed type
        if (!dep_check(ctx, t->ann_term, ann_ty)) return NULL;
        return ann_ty;
    }

    // ── Lambda (Fallback Inference) ────────────────────────────────
    case TERM_LAM: {
        // If no domain annotation, generate a fresh metavariable for the
        // parameter type. This supports let-as-lambda and other unannotated
        // lambdas where the argument type is determined from the call site.
        Value *dom_val;
        Term  *dom_term;
        if (!t->binder_dom || t->binder_dom->kind == TERM_HOLE) {
            int meta_id = meta_fresh(ctx->mctx, val_universe_n(0), ctx->depth,
                                     t->binder_name ? t->binder_name : "?lam");
            dom_val  = val_meta(meta_id, val_spine_empty());
            dom_term = term_meta(meta_id);
        } else {
            dom_val  = dep_eval(t->binder_dom, ctx->env, ctx->mctx);
            dom_term = term_clone(t->binder_dom);
        }
        dep_ctx_push(ctx, t->binder_name, dom_val);
        Value *body_ty = dep_infer(ctx, t->binder_body);

        if (!body_ty) {
            dep_ctx_pop(ctx);
            return NULL;
        }
        Term *body_ty_term = dep_quote(body_ty, ctx->depth, ctx->mctx);
        dep_ctx_pop(ctx);

        Term *pi_term = term_pi(t->binder_name, dom_term, body_ty_term, IMPLICIT_EXPLICIT);
        Value *pi_val = dep_eval(pi_term, ctx->env, ctx->mctx);
        /* DO NOT FREE pi_term: pi_val captures its body in a closure! */
        return pi_val;
    }

    // ── Π-type formation ───────────────────────────────────────────
    case TERM_PI: {
        int u = dep_infer_level(ctx, t->binder_dom);
        if (u < 0) return NULL;
        // Check body under extended context
        Value *dom_val = dep_eval(t->binder_dom, ctx->env, ctx->mctx);
        dep_ctx_push(ctx, t->binder_name, dom_val);
        int v = dep_infer_level(ctx, t->binder_body);
        dep_ctx_pop(ctx);
        if (v < 0) return NULL;
        int level = u > v ? u : v;   // max(u, v) — predicative Π
        return val_universe_n(level);
    }

    // ── Σ-type formation ───────────────────────────────────────────
    case TERM_SIGMA: {
        int u = dep_infer_level(ctx, t->binder_dom);
        if (u < 0) return NULL;
        Value *dom_val = dep_eval(t->binder_dom, ctx->env, ctx->mctx);
        dep_ctx_push(ctx, t->binder_name, dom_val);
        int v = dep_infer_level(ctx, t->binder_body);
        dep_ctx_pop(ctx);
        if (v < 0) return NULL;
        int level = u > v ? u : v;
        return val_universe_n(level);
    }

    // ── Application ───────────────────────────────────────────────
    case TERM_APP: {
        Value *fn_ty = dep_infer(ctx, t->app_fn);
        if (!fn_ty) return NULL;

        for (int i = 0; i < t->app_argc; i++) {
            fn_ty = dep_force(fn_ty, ctx->mctx);
            // Insert implicit arguments automatically
            while (fn_ty && fn_ty->kind == VAL_PI &&
                   fn_ty->implicit == IMPLICIT_IMPLICIT) {
                int meta_id = meta_fresh(ctx->mctx, fn_ty->domain, ctx->depth, "?impl");
                Value *meta_val = val_meta(meta_id, val_spine_empty());
                fn_ty = dep_closure_apply(fn_ty->closure, meta_val);
                fn_ty = dep_force(fn_ty, ctx->mctx);
            }
            if (fn_ty && fn_ty->kind == VAL_META) {
                // If it's a hole (like our dummy stdlib functions), we cannot check domain types.
                // We just assume the argument is fine and return another hole as the result type.
                fn_ty = val_meta(meta_fresh(ctx->mctx, val_universe_n(0), ctx->depth, "?ret"), val_spine_empty());
                continue;
            }
            // Protect against eager Peano coercion masquerading as a function type
            if (fn_ty && (fn_ty->kind == VAL_NAT || fn_ty->kind == VAL_SUCC || fn_ty->kind == VAL_ZERO || fn_ty->kind == VAL_NUM_LIT)) {
                 fn_ty = val_embed(type_int()); // Safely collapse numeric constants back to structural bounds
            }

            // Handle collections acting as functions (indexing)
            if (fn_ty && fn_ty->kind == VAL_EMBED && fn_ty->embed_type) {
                int k = fn_ty->embed_type->kind;
                if (k == TYPE_COLL || k == TYPE_LIST || k == TYPE_ARR || k == TYPE_STRING || k == TYPE_MAP) {
                    if (k != TYPE_MAP) {
                        if (!dep_check(ctx, t->app_args[i], val_embed(type_int()))) return NULL;
                    }
                    Type *elem_type = NULL;
                    if (k == TYPE_COLL) elem_type = fn_ty->embed_type->element_type;
                    else if (k == TYPE_LIST && fn_ty->embed_type->list_count == 1) elem_type = fn_ty->embed_type->list_elem;
                    else if (k == TYPE_ARR) elem_type = fn_ty->embed_type->arr_element_type;

                    if (elem_type) {
                        fn_ty = val_embed(type_clone(elem_type));
                    } else {
                        fn_ty = val_meta(meta_fresh(ctx->mctx, val_universe_n(0), ctx->depth, "?elem"), val_spine_empty());
                    }
                    continue;
                }
            }

            if (fn_ty && fn_ty->kind == VAL_SIGMA) {
                int index_value = -1;
                if (dep_term_numeric_index(t->app_args[i], &index_value)) {
                    if (index_value == 0) {
                        return fn_ty->domain;
                    }

                    if (index_value == 1) {
                        Term *fst_term = term_fst(term_clone(t->app_fn));
                        Value *fst_val = dep_eval(fst_term, ctx->env, ctx->mctx);
                        term_free(fst_term);
                        return dep_closure_apply(fn_ty->closure, fst_val);
                    }
                }
            }

            if (!fn_ty || fn_ty->kind != VAL_PI) {
                /* Tuple/product formation: (A b c ...) where A is not a Pi.
                 * This implements the "lists and tuples are the same" semantic:
                 * (JNull r) is a 2-tuple of (Json, String), not a function call.
                 * We accept remaining args as tuple elements and return a LIST type
                 * whose elements are the types of all components.              */
                if (fn_ty && (fn_ty->kind == VAL_EMBED   ||
                              fn_ty->kind == VAL_NEUTRAL ||
                              fn_ty->kind == VAL_UNIVERSE)) {
                    /* Consume all remaining args as tuple elements, infer each */
                    for (int j = i; j < t->app_argc; j++) {
                        dep_infer(ctx, t->app_args[j]);
                    }
                    /* Return unknown — HM will resolve the tuple type from context */
                    int meta_id = meta_fresh(ctx->mctx, val_universe_n(0), ctx->depth, "?tuple");
                    return val_meta(meta_id, val_spine_empty());
                }
                dep_error_set(ctx, t->line, t->col,
                              "\n"
                              "    • Applied too many arguments to function\n"
                              "    • Expected a function type (Pi) for argument %d, but got:\n"
                              "        %s\n"
                              "    • In the expression: %s\n"
                              "    - Hint: Check the arity of the function you are calling.",
                              i + 1,
                              fn_ty ? term_to_string(dep_quote(fn_ty, ctx->depth, ctx->mctx)) : "unknown",
                              term_to_string(t));
                return NULL;
            }
            if (!dep_check(ctx, t->app_args[i], fn_ty->domain)) return NULL;
            Value *arg_val = dep_eval(t->app_args[i], ctx->env, ctx->mctx);
            fn_ty = dep_closure_apply(fn_ty->closure, arg_val);
        }
        return fn_ty;
    }

    // ── Projections ───────────────────────────────────────────────
    case TERM_FST: {
        Value *pair_ty = dep_infer(ctx, t->pair_fst);
        if (!pair_ty) return NULL;
        pair_ty = dep_force(pair_ty, ctx->mctx);
        if (pair_ty->kind != VAL_SIGMA) {
            dep_error_set(ctx, t->line, t->col,
                          "fst: expected Σ-type, got %s",
                          term_to_string(dep_quote(pair_ty, ctx->depth, ctx->mctx)));
            return NULL;
        }
        return pair_ty->domain;
    }

    case TERM_SND: {
        Value *pair_ty = dep_infer(ctx, t->pair_fst);
        if (!pair_ty) return NULL;
        pair_ty = dep_force(pair_ty, ctx->mctx);
        if (pair_ty->kind != VAL_SIGMA) {
            dep_error_set(ctx, t->line, t->col,
                          "snd: expected Σ-type, got %s",
                          term_to_string(dep_quote(pair_ty, ctx->depth, ctx->mctx)));
            return NULL;
        }
        // snd p : B[fst p / x]
        Value *fst_val = dep_eval(term_fst(term_clone(t->pair_fst)), ctx->env, ctx->mctx);
        return dep_closure_apply(pair_ty->closure, fst_val);
    }

    // ── Nat successor ─────────────────────────────────────────────
    case TERM_SUCC_T: {
        if (!dep_check(ctx, t->succ_pred, val_nat())) return NULL;
        return val_nat();
    }

    // ── Nat eliminator ────────────────────────────────────────────
    case TERM_NAT_ELIM: {
        /*  Nat-elim : Π(P : Nat → Type u). P zero → (Π(n:Nat). P n → P (succ n))
         *             → Π(n : Nat). P n
         *
         *  We synthesise by checking each piece against its expected type.
         */
        Value *nat_to_type = val_pi("n", val_nat(),
            (Closure){ .env = eval_env_empty(), .body = term_type_n(0) },
            IMPLICIT_EXPLICIT);
        if (!dep_check(ctx, t->nelim_motive, nat_to_type)) return NULL;

        Value *P  = dep_eval(t->nelim_motive, ctx->env, ctx->mctx);
        Value *Pz = dep_closure_apply(P->closure, val_zero());
        if (!dep_check(ctx, t->nelim_zero, Pz)) return NULL;

        // ps : Π(n:Nat). P n → P (succ n)
        Term *ps_type_term = term_pi("n", term_nat(),
            term_pi("ih", term_app1(term_clone(t->nelim_motive), term_bvar(0)),
                term_app1(term_clone(t->nelim_motive), term_succ(term_bvar(1))),
                IMPLICIT_EXPLICIT),
            IMPLICIT_EXPLICIT);
        Value *ps_type = dep_eval(ps_type_term, ctx->env, ctx->mctx);
        /* DO NOT FREE ps_type_term: ps_type captures its body in a closure! */
        if (!dep_check(ctx, t->nelim_succ, ps_type)) return NULL;

        if (!dep_check(ctx, t->nelim_arg, val_nat())) return NULL;
        Value *n_val = dep_eval(t->nelim_arg, ctx->env, ctx->mctx);
        return dep_closure_apply(P->closure, n_val);
    }

    case TERM_IF: {
        if (!dep_check(ctx, t->if_cond, val_embed(type_bool()))) return NULL;
        Value *then_ty = dep_infer(ctx, t->if_then);
        if (!then_ty) return NULL;
        if (!dep_check(ctx, t->if_else, then_ty)) return NULL;
        return then_ty;
    }

    // ── Equality type ─────────────────────────────────────────────
    case TERM_EQ: {
        Value *ty = dep_infer(ctx, t->eq_lhs);
        if (!ty) return NULL;
        if (!dep_check(ctx, t->eq_rhs, ty)) return NULL;
        // a ≡ b : A  lives in Type u where A : Type u
        return dep_infer(ctx, t->eq_type);
    }

    // ── Meta ──────────────────────────────────────────────────────
    case TERM_HOLE: {
        /* Hole in infer position: mint a fresh meta of unknown type.
         * This mirrors dep_check's TERM_HOLE handling and is required
         * for array literals (which return term_hole() to defer to HM)
         * and any other unannotated wildcard in synthesis position.    */
        int id = meta_fresh(ctx->mctx, val_universe_n(0), ctx->depth, "_hole_infer");
        return val_meta(id, val_spine_empty());
    }

    case TERM_META: {
        MetaEntry *e = meta_lookup(ctx->mctx, t->meta_id);
        if (!e) {
            dep_error_set(ctx, t->line, t->col,
                          "unknown metavariable ?%d", t->meta_id);
            return NULL;
        }
        return e->type;
    }

    // ── Let ───────────────────────────────────────────────────────
    case TERM_LET: {
        Value *ty_val = dep_eval(t->let_type, ctx->env, ctx->mctx);
        if (!dep_check(ctx, t->let_val, ty_val)) return NULL;
        Value *val_val = dep_eval(t->let_val, ctx->env, ctx->mctx);
        dep_ctx_push_def(ctx, t->binder_name, ty_val, val_val);
        Value *body_ty = dep_infer(ctx, t->binder_body);
        dep_ctx_pop(ctx);
        return body_ty;
    }

    default:
        dep_error_set(ctx, t->line, t->col,
                      "cannot infer type of %s", term_to_string(t));
        return NULL;
    }
}

//  dep_check — Γ ⊢ t ⇐ A
//
//  Check term t against known type ty.  Returns true on success.
//  This is the "push" direction of bidirectional checking: the type
//  flows *in* from the context.
//
static bool dep_check_internal(DepCtx *ctx, Term *t, Value *expected_type);

bool dep_check(DepCtx *ctx, Term *t, Value *expected_type) {
    if (!t || !expected_type) return false;
    if (ctx->had_error) return false;

    if (g_trace_enabled) {
        Term *q_exp = dep_quote(expected_type, ctx->depth, ctx->mctx);
        shared_trace_indent();
        fprintf(stderr, "├─ \033[33mCheck\033[0m %s ⇐ %s\n", term_to_string(t), term_to_string(q_exp));
        term_free(q_exp);
        g_trace_depth++;
    }

    bool res = dep_check_internal(ctx, t, expected_type);

    if (res && t && t->source_ast) {
        Type *hm_ty = dep_ground_of_value_env(expected_type, ctx->mctx, ctx->globals);
        if (hm_ty) {
            if (t->source_ast->inferred_type) type_free(t->source_ast->inferred_type);
            t->source_ast->inferred_type = hm_ty;
        }
    }

    if (g_trace_enabled) {
        g_trace_depth--;
        shared_trace_indent();
        if (res) {
            fprintf(stderr, "└─ \033[32mOK\033[0m\n");
        } else {
            fprintf(stderr, "└─ \033[31mFAIL\033[0m\n");
        }
    }
    return res;
}


static bool dep_check_internal(DepCtx *ctx, Term *t, Value *expected_type) {
    expected_type = dep_force(expected_type, ctx->mctx);

    switch (t->kind) {

    // ── Lambda — check against Π ──────────────────────────────────
    case TERM_LAM: {
        if (expected_type->kind != VAL_PI) {
            goto check_default;
        }
        // Check domain annotation (if provided and not a hole) matches the expected Π
        if (t->binder_dom && t->binder_dom->kind != TERM_HOLE) {
            Value *ann_dom = dep_eval(t->binder_dom, ctx->env, ctx->mctx);
            ConvCtx cc = conv_ctx_make(ctx, ctx->depth);
            if (!dep_conv(&cc, ann_dom, expected_type->domain, NULL)) {
                dep_error_set(ctx, t->line, t->col,
                              "lambda parameter type mismatch: %s",
                              cc.error_msg);
                return false;
            }
        }
        // Extend context with the bound variable
        char   fresh[64];
        snprintf(fresh, sizeof(fresh), "%s!%d", t->binder_name, ctx->depth);
        Value *var = val_neutral(fresh, ctx->depth, val_spine_empty());
        dep_ctx_push(ctx, t->binder_name, expected_type->domain);
        Value *body_ty = dep_closure_apply(expected_type->closure, var);
        bool ok = dep_check(ctx, t->binder_body, body_ty);
        dep_ctx_pop(ctx);
        return ok;
    }

    // ── Pair — check against Σ ────────────────────────────────────
    case TERM_PAIR: {
        if (expected_type->kind != VAL_SIGMA) {
            dep_error_set(ctx, t->line, t->col,
                          "pair in non-Σ position: expected %s",
                          term_to_string(dep_quote(expected_type, ctx->depth, ctx->mctx)));
            return false;
        }
        if (!dep_check(ctx, t->pair_fst, expected_type->domain)) return false;
        Value *fst_val = dep_eval(t->pair_fst, ctx->env, ctx->mctx);
        Value *snd_ty  = dep_closure_apply(expected_type->closure, fst_val);
        return dep_check(ctx, t->pair_snd, snd_ty);
    }

    case TERM_IF: {
        if (!dep_check(ctx, t->if_cond, val_embed(type_bool()))) return false;
        if (!dep_check(ctx, t->if_then, expected_type)) return false;
        if (!dep_check(ctx, t->if_else, expected_type)) return false;
        return true;
    }

    // ── Refl — check against equality type ───────────────────────
    case TERM_REFL: {
        if (expected_type->kind != VAL_EQ) {
            dep_error_set(ctx, t->line, t->col,
                          "refl: expected equality type, got %s",
                          term_to_string(dep_quote(expected_type, ctx->depth, ctx->mctx)));
            return false;
        }
        /* Check that the two sides are definitionally equal            */
        ConvCtx cc = conv_ctx_make(ctx, ctx->depth);
        if (!dep_conv(&cc, expected_type->eq_lhs,
                          expected_type->eq_rhs,
                          expected_type->eq_type_val)) {
            dep_error_set(ctx, t->line, t->col,
                          "refl: sides not definitionally equal: %s", cc.error_msg);
            return false;
        }
        return true;
    }

    // ── Hole / meta — generate a fresh metavariable ───────────────
    case TERM_HOLE: {
        int id = meta_fresh(ctx->mctx, expected_type, ctx->depth, "_");
        (void)id;     // The hole is now represented as ?id in the mctx
        return true;  // Always succeeds; solver fills it later
    }

    case TERM_META: {
        MetaEntry *e = meta_lookup(ctx->mctx, t->meta_id);
        if (!e) {
            dep_error_set(ctx, t->line, t->col,
                          "unknown metavariable ?%d", t->meta_id);
            return false;
        }
        if (e->state == META_SOLVED) {
            EvalEnv *empty = eval_env_empty();
            Value *sol_val = dep_eval(e->solution, empty, ctx->mctx);
            eval_env_free(empty);
            ConvCtx cc = conv_ctx_make(ctx, ctx->depth);
            return dep_conv(&cc, sol_val, dep_eval(term_meta(t->meta_id), ctx->env, ctx->mctx), expected_type);
        }
        // Unsolved meta: set its expected type and succeed
        if (!e->type) e->type = expected_type;
        return true;
    }

    check_default:
    // ── Default: infer and compare ────────────────────────────────
    default: {
        Value *inferred = dep_infer(ctx, t);
        if (!inferred) return false;
        ConvCtx cc = conv_ctx_make(ctx, ctx->depth);
        if (!dep_conv(&cc, inferred, expected_type, NULL)) {
            dep_error_set(ctx, t->line, t->col,
                          "\n"
                          "    • Couldn't match expected type:  %s\n"
                          "    • with actual type:              %s\n"
                          "    • While checking the expression: %s\n"
                          "   - Hint: %s",
                          term_to_string(dep_quote(expected_type,  ctx->depth, ctx->mctx)),
                          term_to_string(dep_quote(inferred,       ctx->depth, ctx->mctx)),
                          term_to_string(t),
                          cc.error_msg[0] ? cc.error_msg : "Ensure your type annotations align with the values.");
            return false;
        }
        return true;
    }
    }
}

int dep_infer_level(DepCtx *ctx, Term *ty) {
    Value *sort = dep_infer(ctx, ty);
    if (!sort) return -1;
    sort = dep_force(sort, ctx->mctx);
    if (sort->kind != VAL_UNIVERSE) {
        dep_error_set(ctx, ty ? ty->line : 0, ty ? ty->col : 0,
                      "expected a type (universe), got %s",
                      term_to_string(dep_quote(sort, ctx->depth, ctx->mctx)));
        return -1;
    }
    int n = level_eval(sort->level);
    return n >= 0 ? n : 0;
}


/// Interop: ground types <-> core terms

Term *dep_term_of_ground(Type *ground_type) {
    return term_embed(ground_type);
}

Type *dep_ground_of_value(Value *v, MetaCtx *mctx) {
    return dep_ground_of_value_env(v, mctx, NULL);
}

Type *dep_ground_of_value_env(Value *v, MetaCtx *mctx, DepEnv *globals) {
    if (!v) return NULL;
    v = dep_force(v, mctx);

    if (v->kind == VAL_EMBED) {
        return type_clone(v->embed_type);
    }
    if (v->kind == VAL_PI) {
        // Skip implicit compile-time arguments entirely.
        if (v->implicit != IMPLICIT_EXPLICIT) {
            Value *dummy = val_neutral("hm_impl", -1, val_spine_empty());
            Value *codom = dep_closure_apply(v->closure, dummy);
            return dep_ground_of_value_env(codom, mctx, globals);
        }

        // Skip explicit universe-level (Type u) arguments - compile-time
        // type parameters. We pass a VAL_EMBED(unknown) as the dummy so
        // that when the body refers to this type variable it resolves to
        // TYPE_UNKNOWN rather than a neutral that produces NULL.
        if (v->domain && v->domain->kind == VAL_UNIVERSE) {
            Value *dummy = val_embed(type_unknown());
            Value *codom = dep_closure_apply(v->closure, dummy);
            Type *result = dep_ground_of_value_env(codom, mctx, globals);
            return result;
        }

        Type *param = dep_ground_of_value_env(v->domain, mctx, globals);
        Value *dummy = val_neutral("hm_arg", -1, val_spine_empty());
        Value *codom = dep_closure_apply(v->closure, dummy);
        Type *ret = dep_ground_of_value_env(codom, mctx, globals);

        if (param && ret) return type_arrow(param, ret);
        if (param) type_free(param);
        if (ret) type_free(ret);
        return type_unknown();
    }
    if (v->kind == VAL_NAT || v->kind == VAL_SUCC || v->kind == VAL_ZERO || v->kind == VAL_NUM_LIT) {
        return type_int();
    }

    /* VAL_NEUTRAL introduced as a type-arg dummy resolves to unknown.
     * EXCEPT: if the neutral is a known ADT type constructor (__type_X)
     * applied to a spine, we can recover a TYPE_APP. This is the bridge
     * that lets (Maybe a) in dep produce TYPE_APP("Maybe", unknown) in HM,
     * which the HM unifier can then unify against bare Maybe (TYPE_LAYOUT).
     * REFINEMENT TYPES: if __type_X resolves to a VAL_EMBED in globals
     * (i.e. it is a refinement type over a base ground type like Int),
     * return that base type directly. This prevents Even from becoming
     * TYPE_APP("Even", unknown) which HM cannot unify against Int. */
    if (v->kind == VAL_NEUTRAL && v->neutral_name) {
        const char *name = v->neutral_name;
        /* First: try to resolve via globals — handles refinement type aliases */
        if (globals && v->spine.count == 0) {
            DepEnvEntry *e = dep_env_lookup(globals, name);
            if (e && e->def_val && e->def_val->kind == VAL_EMBED) {
                return type_clone(e->def_val->embed_type);
            }
        }
        /* Strip __type_ prefix to get the constructor name */
        const char *ctor = NULL;
        if (strncmp(name, "__type_", 7) == 0) {
            ctor = name + 7;
            /* Try globals lookup for __type_X with empty spine too */
            if (globals && v->spine.count == 0) {
                DepEnvEntry *e = dep_env_lookup(globals, name);
                if (e && e->def_val && e->def_val->kind == VAL_EMBED) {
                    return type_clone(e->def_val->embed_type);
                }
            }
        }
        if (ctor) {
            /* If there is one spine argument, it is the type parameter */
            Type *arg = NULL;
            if (v->spine.count == 1) {
                arg = dep_ground_of_value_env(v->spine.args[0], mctx, globals);
            }
            if (!arg) arg = type_unknown();
            return type_app(ctor, arg);
        }
    }
    /* VAL_META that is unsolved also resolves to unknown. */
    return type_unknown();
}

static char *dep_type_parse_source(const char *type_name) {
    if (!type_name) return NULL;

    const char *start = strstr(type_name, "=>");
    if (start) {
        start += 2;
    } else {
        start = type_name;
    }

    while (*start == ' ' || *start == '\t' || *start == '\n')
        start++;

    const char *end = start + strlen(start);
    while (end > start &&
           (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n'))
        end--;

    return strndup(start, (size_t)(end - start));
}

static bool dep_ast_numeric_index(AST *ast, int *out_index) {
    if (!ast || ast->type != AST_NUMBER)
        return false;

    long long n = (long long)ast->number;
    if ((double)n != ast->number)
        return false;

    if (out_index)
        *out_index = (int)n;
    return true;
}

static bool dep_term_numeric_index(Term *t, int *out_index) {
    if (!t || t->kind != TERM_NUM_LIT)
        return false;

    if (t->num_lit > 2147483647ULL)
        return false;

    if (out_index)
        *out_index = (int)t->num_lit;
    return true;
}

static bool dep_ast_was_postfix_index(AST *call, AST *receiver) {
    if (!call || !receiver)
        return false;

    return call->line == receiver->line &&
           call->column == receiver->end_column;
}

static Term *dep_term_of_ast_internal(DepCtx *ctx, AST *ast) {
    if (!ast) return term_hole();

    /* Bypass Dependent Checker for value-level Arrays.
     * This allows polymorphic collections to pass cleanly as holes to the
     * HM checker which natively handles the Coll interfaces and exact types. */
    if (ast->type == AST_ARRAY) {
        return term_hole();
    }
    switch (ast->type) {
    case AST_NUMBER:
        {
            /* Float literals must stay as embedded Float, not Nat.
             * Check the literal string first — if it contains '.' or 'e'/'E'
             * it is a float and must be embedded as TYPE_FLOAT so the dep
             * checker's conversion rules handle it correctly (Float ≠ Nat).
             * Negative integers also go through as embedded Int.           */
            bool is_float = false;
            if (ast->literal_str) {
                bool radix_literal =
                    ast->literal_str[0] == '0' &&
                    (ast->literal_str[1] == 'x' || ast->literal_str[1] == 'X' ||
                     ast->literal_str[1] == 'b' || ast->literal_str[1] == 'B' ||
                     ast->literal_str[1] == 'o' || ast->literal_str[1] == 'O');
                if (!radix_literal) {
                    for (const char *_p = ast->literal_str; *_p; _p++) {
                        if (*_p == '.' || *_p == 'e' || *_p == 'E') {
                            is_float = true;
                            break;
                        }
                    }
                }
            } else {
                is_float = (ast->number != (double)(long long)ast->number);
            }
            if (is_float) {
                return term_ann(term_hole(), term_embed(type_float()));
            }
            long long n = (long long)ast->number;
            if (n >= 0) {
                return term_num_lit((unsigned long long)n);
            }
            /* Negative integer: annotate a hole as Int so it passes as a
             * VALUE of type Int, not as the TYPE Int itself. */
            return term_ann(term_hole(), term_embed(type_int()));
        }
    case AST_STRING:  return term_ann(term_hole(), term_embed(type_string()));
    case AST_PATH:    return term_ann(term_hole(), term_embed(type_path()));
    case AST_CHAR: {
        long long n = (long long)ast->character;
        if (n >= 0) {
            return term_num_lit((unsigned long long)n);
        }
        return term_ann(term_hole(), term_embed(type_char()));
    }
    case AST_KEYWORD: return term_ann(term_hole(), term_embed(type_keyword()));
    case AST_SYMBOL:
        // Explicit hole — ? and generated undefined sentinels in any position
        if (strcmp(ast->symbol, "?") == 0 || strcmp(ast->symbol, "undefined") == 0) {
            Term *h = term_hole();
            h->line = ast->line;
            h->col  = ast->column;
            return h;
        }
        // Check if it is a universe literal "Type" or "Type N"
        if (strcmp(ast->symbol, "Type") == 0) return term_type_n(0);
        if (strcmp(ast->symbol, "Nat")  == 0) return term_nat();
        if (strcmp(ast->symbol, "zero") == 0) return term_zero();
        if (ast->symbol[0] >= 'A' && ast->symbol[0] <= 'Z') {
            char ctor_name[256];
            snprintf(ctor_name, sizeof(ctor_name), "__ctor_%s", ast->symbol);
            if (dep_env_lookup(ctx->globals, ctor_name)) {
                return term_fvar(ctor_name);
            }

            Type *ground = type_from_name(ast->symbol);
            if (ground) return term_embed(ground);
        }
        return term_fvar(ast->symbol);
    case AST_LIST:
        if (ast->list.count == 0) return term_hole();
        {
            AST *head = ast->list.items[0];
            // (Π [x : A] -> B)
            if (head->type == AST_SYMBOL && strcmp(head->symbol, "Pi") == 0 && ast->list.count == 3) {
                Term *dom  = dep_term_of_ast(ctx, ast->list.items[1]);
                Term *body = dep_term_of_ast(ctx, ast->list.items[2]);
                return term_pi("x", dom, body, IMPLICIT_EXPLICIT);
            }
            // (Sigma [x : A] B)
            if (head->type == AST_SYMBOL && strcmp(head->symbol, "Sigma") == 0 && ast->list.count == 3) {
                Term *dom  = dep_term_of_ast(ctx, ast->list.items[1]);
                Term *body = dep_term_of_ast(ctx, ast->list.items[2]);
                return term_sigma("x", dom, body);
            }
            // (= A a b) — equality type
            if (head->type == AST_SYMBOL && strcmp(head->symbol, "=") == 0 && ast->list.count == 4) {
                Term *ty  = dep_term_of_ast(ctx, ast->list.items[1]);
                Term *lhs = dep_term_of_ast(ctx, ast->list.items[2]);
                Term *rhs = dep_term_of_ast(ctx, ast->list.items[3]);
                return term_eq(lhs, rhs, ty);
            }
            // (refl t)
            if (head->type == AST_SYMBOL && strcmp(head->symbol, "refl") == 0 && ast->list.count == 2)
                return term_refl(dep_term_of_ast(ctx, ast->list.items[1]));
            // (if cond then else)
            if (head->type == AST_SYMBOL && strcmp(head->symbol, "if") == 0 && ast->list.count >= 3) {
                Term *cond = dep_term_of_ast(ctx, ast->list.items[1]);
                Term *t    = dep_term_of_ast(ctx, ast->list.items[2]);
                Term *e    = ast->list.count >= 4 ? dep_term_of_ast(ctx, ast->list.items[3]) : term_hole();
                return term_if(cond, t, e);
            }
            // Variadic logical operators: (and a b c) -> (and a (and b c))
            if (head->type == AST_SYMBOL && (strcmp(head->symbol, "and") == 0 || strcmp(head->symbol, "or") == 0)) {
                if (ast->list.count == 1) {
                    return term_fvar(strcmp(head->symbol, "and") == 0 ? "True" : "False");
                }
                if (ast->list.count == 2) {
                    return dep_term_of_ast(ctx, ast->list.items[1]);
                }
                Term *result = dep_term_of_ast(ctx, ast->list.items[ast->list.count - 1]);
                for (int i = (int)ast->list.count - 2; i >= 1; i--) {
                    Term *elem = dep_term_of_ast(ctx, ast->list.items[i]);
                    Term **args = malloc(sizeof(Term *) * 2);
                    args[0] = elem;
                    args[1] = result;
                    result = term_app(term_fvar(head->symbol), args, 2);
                }
                return result;
            }
            // (lambda (x : A) body) or (λ x body)
            if (head->type == AST_SYMBOL &&
                (strcmp(head->symbol, "lambda") == 0 || strcmp(head->symbol, "λ") == 0) &&
                ast->list.count >= 3) {
                const char *param = "_";
                Term       *dom   = term_hole();
                AST        *param_ast = ast->list.items[1];
                if (param_ast->type == AST_SYMBOL) {
                    param = param_ast->symbol;
                } else if (param_ast->type == AST_LIST && param_ast->list.count == 3) {
                    /* (x : T) */
                    if (param_ast->list.items[0]->type == AST_SYMBOL)
                        param = param_ast->list.items[0]->symbol;
                    dom = dep_term_of_ast(ctx, param_ast->list.items[2]);
                }
                Term *body = dep_term_of_ast(ctx, ast->list.items[ast->list.count - 1]);
                return term_lam(param, dom, body);
            }
            // (succ n)
            if (head->type == AST_SYMBOL && strcmp(head->symbol, "succ") == 0 && ast->list.count == 2)
                return term_succ(dep_term_of_ast(ctx, ast->list.items[1]));
            // (let [x : T = val] body) — simplified form
            if (head->type == AST_SYMBOL && strcmp(head->symbol, "let") == 0 && ast->list.count == 4)
                return term_let(
                    ast->list.items[1]->type == AST_SYMBOL ? ast->list.items[1]->symbol : "_",
                    term_hole(),
                    dep_term_of_ast(ctx, ast->list.items[2]),
                    dep_term_of_ast(ctx, ast->list.items[3]));
            /* Postfix tuple index:
             *
             *   pair[0]  -> fst(pair)
             *   pair[1]  -> snd(pair)
             *
             * The reader currently represents postfix index as the same
             * list shape as a normal call, but it preserves source columns:
             * the list column is the '[' column, which is also the receiver's
             * end_column. Use that to avoid changing normal calls like
             * (f 0).
             */
            if (ast->list.count == 2 &&
                dep_ast_was_postfix_index(ast, head)) {
                int index_value = -1;
                if (dep_ast_numeric_index(ast->list.items[1], &index_value)) {
                    Term *receiver = dep_term_of_ast(ctx, head);
                    if (index_value == 0)
                        return term_fst(receiver);
                    if (index_value == 1)
                        return term_snd(receiver);
                }
            }

            /* Tuple value:
             *
             *   (x, y)  -> TERM_PAIR(x, y)
             *
             * The reader represents tuple syntax as a list containing a comma
             * symbol. Lower it here before the generic function application
             * path, otherwise (x, y) looks like a call whose head is x.
             */
            {
                size_t comma_idx = 0;
                bool is_tuple_value = false;

                for (size_t i = 0; i < ast->list.count; i++) {
                    AST *item = ast->list.items[i];
                    if (item &&
                        item->type == AST_SYMBOL &&
                        item->symbol &&
                        strcmp(item->symbol, ",") == 0) {
                        comma_idx = i;
                        is_tuple_value = true;
                        break;
                    }
                }

                if (is_tuple_value && comma_idx > 0 &&
                    comma_idx + 1 < ast->list.count) {
                    AST *lhs_ast = NULL;
                    AST *rhs_ast = NULL;

                    if (comma_idx == 1) {
                        lhs_ast = ast->list.items[0];
                    } else {
                        lhs_ast = ast_new_list();
                        lhs_ast->line = ast->line;
                        lhs_ast->column = ast->column;
                        for (size_t i = 0; i < comma_idx; i++)
                            ast_list_append(lhs_ast, ast_clone(ast->list.items[i]));
                    }

                    if (ast->list.count - comma_idx - 1 == 1) {
                        rhs_ast = ast->list.items[comma_idx + 1];
                    } else {
                        rhs_ast = ast_new_list();
                        rhs_ast->line = ast->list.items[comma_idx + 1]->line;
                        rhs_ast->column = ast->list.items[comma_idx + 1]->column;
                        for (size_t i = comma_idx + 1; i < ast->list.count; i++)
                            ast_list_append(rhs_ast, ast_clone(ast->list.items[i]));
                    }

                    return term_pair(dep_term_of_ast(ctx, lhs_ast),
                                     dep_term_of_ast(ctx, rhs_ast),
                                     NULL);
                }
            }

            // Function application: (f a1 a2 ... an)
            Term  *fn    = dep_term_of_ast(ctx, head);
            Term **args  = malloc(sizeof(Term *) * (ast->list.count - 1));
            for (size_t i = 1; i < ast->list.count; i++)
                args[i - 1] = dep_term_of_ast(ctx, ast->list.items[i]);
            return term_app(fn, args, (int)(ast->list.count - 1));
        }
    case AST_ARRAY: {
        // Empty array
        if (ast->array.element_count == 0) {
            return term_app1(term_fvar("Nil"), term_hole());
        }
        // Desugar [a, b, c] into Cons a (Cons b (Cons c Nil))
        Term *result = term_app1(term_fvar("Nil"), term_hole());
        for (int i = (int)ast->array.element_count - 1; i >= 0; i--) {
            Term *elem = dep_term_of_ast(ctx, ast->array.elements[i]);
            Term **args = malloc(sizeof(Term *) * 2);
            args[0] = elem;
            args[1] = result;
            result = term_app(term_fvar("Cons"), args, 2);
        }
        return result;
    }
    case AST_LAMBDA: {
        Term *body = dep_term_of_ast(ctx, ast->lambda.body);
        Term *ty   = NULL;

        if (ast->lambda.return_type) {
            char *rname = dep_type_parse_source(ast->lambda.return_type);

            AST *ret_ast = parse(rname);
            if (ret_ast) {
                ty = dep_term_of_type_ast(ctx, ret_ast);
                /* Intentional leak to prevent UAF in HM phase */
            }
            free(rname);
        }

        for (int i = ast->lambda.param_count - 1; i >= 0; i--) {
            ASTParam *p = &ast->lambda.params[i];
            Term *dom = term_hole();
            if (p->type_name) {
                char *tname = dep_type_parse_source(p->type_name);

                if (getenv("MONAD_DEP_DEBUG")) {
                    fprintf(stderr,
                            "[dep-lambda-param] name=%s type='%s'\n",
                            p->name ? p->name : "_",
                            tname ? tname : "<null>");
                }

                AST *ty_ast = parse(tname);
                if (ty_ast) {
                    dom = dep_term_of_type_ast(ctx, ty_ast);
                    /* Intentional leak to prevent UAF in HM phase */
                }
                free(tname);
            }
            body = term_lam(p->name, term_clone(dom), body);
            if (ty) {
                ty = term_pi(p->name, dom, ty, IMPLICIT_EXPLICIT);
            } else {
                term_free(dom);
            }
        }

        if (ty) {
            return term_ann(body, ty);
        }
        return body;
    }
    case AST_TESTS:
    case AST_DATA:
    case AST_CLASS:
    case AST_INSTANCE:
    case AST_LAYOUT:
        /* Bypass top-level definitions that the dependent checker doesn't fully intercept yet */
        return term_fvar("True");
    case AST_ASM:
        /* asm blocks are opaque to the type checker — trust the declared signature.
         * term_hole() defers to whatever type is expected (check position: always OK,
         * infer position: mints a fresh meta). This is the correct "trust the sig" semantics. */
        return term_hole();
    default:
        return term_hole();
    }
}

Term *dep_term_of_ast(DepCtx *ctx, AST *ast) {
    Term *t = dep_term_of_ast_internal(ctx, ast);
    if (t) {
        t->source_ast = ast;
        if (ast) {
            t->line = ast->line;
            t->col  = ast->column;
        }
    }
    return t;
}

Term *dep_term_of_type_ast(DepCtx *ctx, AST *ast) {
    if (!ast) return term_meta(meta_fresh(ctx->mctx, val_universe_n(0), ctx->depth, "?ty"));

    Term *res = NULL;

    if (ast->type == AST_SYMBOL) {
        /* Pointer sugar: *U8 -> Pointer :: U8 */
        if (ast->symbol[0] == '*' && ast->symbol[1] >= 'A' && ast->symbol[1] <= 'Z') {
            const char *base = ast->symbol + 1;
            while (*base == '*') base++;
            Type *g = type_from_name(base);
            if (g) return term_embed(type_ptr(g));
            return term_meta(meta_fresh(ctx->mctx, val_universe_n(0), ctx->depth, ast->symbol));
        }
        /* Explicit hole in type position: (a) -> (? a) or define f :: ? */
        if (strcmp(ast->symbol, "?") == 0) {
            int id = meta_fresh(ctx->mctx, val_universe_n(0), ctx->depth, "?");
            Term *t = term_meta(id);
            t->line = ast->line;
            t->col  = ast->column;
            return t;
        }
        if (strcmp(ast->symbol, "Fn") == 0) {
            int id = meta_fresh(ctx->mctx, val_universe_n(0), ctx->depth, "?fn");
            Term *t = term_meta(id);
            t->line = ast->line;
            t->col  = ast->column;
            return t;
        }
        if (ast->symbol[0] >= 'a' && ast->symbol[0] <= 'z') {
            int id = meta_fresh(ctx->mctx, val_universe_n(0), ctx->depth, ast->symbol);
            return term_meta(id);
        }
        if (strcmp(ast->symbol, "List") == 0 || strcmp(ast->symbol, "Coll") == 0) {
            return term_meta(meta_fresh(ctx->mctx, val_universe_n(0), ctx->depth, "?coll"));
        }
        if (ast->symbol[0] >= 'A' && ast->symbol[0] <= 'Z') {
            char type_name[256];
            snprintf(type_name, sizeof(type_name), "__type_%s", ast->symbol);
            if (dep_env_lookup(ctx->globals, type_name)) {
                return term_fvar(type_name);
            }
        }
    } else if (ast->type == AST_ARRAY) {
        if (ast->array.element_count == 3 &&
                ast->array.elements[0]->type == AST_SYMBOL && strcmp(ast->array.elements[0]->symbol, "Fn") == 0 &&
                ast->array.elements[1]->type == AST_SYMBOL && strcmp(ast->array.elements[1]->symbol, "::") == 0) {
                res = dep_term_of_type_ast(ctx, ast->array.elements[2]);
        } else if (ast->array.element_count == 0) {
            res = term_embed(type_arr_fat(type_unknown()));
        } else if (ast->array.element_count == 1) {
            AST *elem_ast = ast->array.elements[0];

            Type *coll = type_coll();

            if (elem_ast->type == AST_SYMBOL &&
                elem_ast->symbol[0] >= 'a' && elem_ast->symbol[0] <= 'z') {
                coll->element_type = type_unknown();
                res = term_embed(coll);
            } else if (elem_ast->type == AST_SYMBOL) {
                Type *elem = type_from_name(elem_ast->symbol);
                coll->element_type = elem ? elem : type_layout_ref(elem_ast->symbol);
                res = term_embed(coll);
            } else {
                Term *elem_term = dep_term_of_type_ast(ctx, elem_ast);
                if (elem_term && elem_term->kind == TERM_EMBED && elem_term->embed_type) {
                    coll->element_type = type_clone(elem_term->embed_type);
                } else {
                    coll->element_type = type_unknown();
                }
                res = term_embed(coll);
            }
        } else {
            char type_buf[512] = {0};

            for (size_t i = 0; i < ast->array.element_count; i++) {
                AST *elem = ast->array.elements[i];
                if (!elem || elem->type != AST_SYMBOL || !elem->symbol) {
                    type_buf[0] = '\0';
                    break;
                }
                if (type_buf[0]) {
                    strncat(type_buf, " ", sizeof(type_buf) - strlen(type_buf) - 1);
                }
                strncat(type_buf, elem->symbol, sizeof(type_buf) - strlen(type_buf) - 1);
            }

            Type *coll = type_coll();
            if (type_buf[0]) {
                Type *elem = type_from_name(type_buf);
                coll->element_type = elem ? elem : type_unknown();
            } else {
                coll->element_type = type_unknown();
            }
            res = term_embed(coll);
        }

    } else if (ast->type == AST_LIST) {
        bool is_arrow = false;
        for (size_t i = 0; i < ast->list.count; i++) {
            if (ast->list.items[i]->type == AST_SYMBOL && strcmp(ast->list.items[i]->symbol, "->") == 0) {
                is_arrow = true; break;
            }
        }
        if (is_arrow) {
            size_t arrow_idx = 0;
            for (size_t i = 0; i < ast->list.count; i++) {
                if (ast->list.items[i]->type == AST_SYMBOL && strcmp(ast->list.items[i]->symbol, "->") == 0) {
                    arrow_idx = i;
                    break;
                }
            }

            AST *lhs_ast = NULL;
            if (arrow_idx == 1) {
                lhs_ast = ast->list.items[0];
            } else if (arrow_idx > 1) {
                lhs_ast = ast_new_list();
                lhs_ast->line = ast->list.items[0]->line;
                lhs_ast->column = ast->list.items[0]->column;
                for(size_t i=0; i<arrow_idx; i++) ast_list_append(lhs_ast, ast_clone(ast->list.items[i]));
            }
            Term *dom = lhs_ast ? dep_term_of_type_ast(ctx, lhs_ast) : term_meta(meta_fresh(ctx->mctx, val_universe_n(0), ctx->depth, "?dom"));
            /* Intentional leak of lhs_ast to prevent UAF in HM phase */

            AST *rhs_ast = NULL;
            if (ast->list.count - arrow_idx - 1 == 1) {
                rhs_ast = ast->list.items[arrow_idx + 1];
            } else if (ast->list.count - arrow_idx - 1 > 1) {
                rhs_ast = ast_new_list();
                rhs_ast->line = ast->list.items[arrow_idx + 1]->line;
                rhs_ast->column = ast->list.items[arrow_idx + 1]->column;
                for(size_t i=arrow_idx+1; i<ast->list.count; i++) ast_list_append(rhs_ast, ast_clone(ast->list.items[i]));
            }
            Term *ret = rhs_ast ? dep_term_of_type_ast(ctx, rhs_ast) : term_meta(meta_fresh(ctx->mctx, val_universe_n(0), ctx->depth, "?ret"));
            /* Intentional leak of rhs_ast to prevent UAF in HM phase */

            res = term_pi("_", dom, ret, IMPLICIT_EXPLICIT);
        } else {
            size_t comma_idx = 0;
            bool is_tuple_type = false;

            for (size_t i = 0; i < ast->list.count; i++) {
                AST *item = ast->list.items[i];
                if (item &&
                    item->type == AST_SYMBOL &&
                    item->symbol &&
                    strcmp(item->symbol, ",") == 0) {
                    comma_idx = i;
                    is_tuple_type = true;
                    break;
                }
            }

            if (is_tuple_type && comma_idx > 0 &&
                comma_idx + 1 < ast->list.count) {
                AST *lhs_ast = NULL;
                AST *rhs_ast = NULL;

                if (comma_idx == 1) {
                    lhs_ast = ast->list.items[0];
                } else {
                    lhs_ast = ast_new_list();
                    lhs_ast->line = ast->line;
                    lhs_ast->column = ast->column;
                    for (size_t i = 0; i < comma_idx; i++)
                        ast_list_append(lhs_ast, ast_clone(ast->list.items[i]));
                }

                if (ast->list.count - comma_idx - 1 == 1) {
                    rhs_ast = ast->list.items[comma_idx + 1];
                } else {
                    rhs_ast = ast_new_list();
                    rhs_ast->line = ast->list.items[comma_idx + 1]->line;
                    rhs_ast->column = ast->list.items[comma_idx + 1]->column;
                    for (size_t i = comma_idx + 1; i < ast->list.count; i++)
                        ast_list_append(rhs_ast, ast_clone(ast->list.items[i]));
                }

                Term *dom = dep_term_of_type_ast(ctx, lhs_ast);
                Term *cod = dep_term_of_type_ast(ctx, rhs_ast);
                res = term_sigma("_", dom, cod);

                /* lhs_ast/rhs_ast may alias ast children, so intentionally
                 * do not free them here. This file already uses intentional
                 * leaks in type elaboration to avoid closure UAF. */
            } else {
                res = term_meta(meta_fresh(ctx->mctx,
                                           val_universe_n(0),
                                           ctx->depth,
                                           "?ty"));
            }
        }
    }

    if (!res) res = dep_term_of_ast_internal(ctx, ast);
    if (res) {
        res->source_ast = ast;
        res->line = ast->line;
        res->col  = ast->column;
    }
    return res;
}

Term *dep_elab_term(DepCtx *ctx, AST *ast, Value *expected_type) {
    Term *t = dep_term_of_ast(ctx, ast);
    if (expected_type) {
        if (!dep_check(ctx, t, expected_type)) return NULL;
    } else {
        if (!dep_infer(ctx, t)) return NULL;
    }
    return t;
}

Term *dep_elab_type(DepCtx *ctx, AST *ast) {
    Term  *t  = dep_term_of_ast(ctx, ast);
    int    lv = dep_infer_level(ctx, t);
    if (lv < 0) return NULL;
    return t;
}

Value *dep_elab_and_infer(DepCtx *ctx, AST *ast) {
    Term *t = dep_term_of_ast(ctx, ast);
    return dep_infer(ctx, t);
}


/// Top-level pipeline

Term *dep_toplevel(DepCtx *ctx, AST *ast, Term **out_type) {
    if (!ast) return NULL;

    /* Intercept top-level (type Name { x in T | pred }) refinement definitions */
    if (ast->type == AST_REFINEMENT) {
        Type *base_ground = type_from_name(ast->refinement.base_type);
        if (!base_ground) base_ground = type_int();
        Term *base_term = term_embed(base_ground);
        Value *base_val = val_embed(base_ground);

        /* Register __type_Name so field type lookups work */
        char type_name[256];
        snprintf(type_name, sizeof(type_name), "__type_%s", ast->refinement.name);
        dep_env_define(ctx->globals, type_name, val_universe_n(0),
                       term_clone(base_term), val_embed(type_clone(base_ground)), true);

        /* Register the full name (e.g. "Natural") as the base embed type */
        dep_env_define(ctx->globals, ast->refinement.name, val_universe_n(0),
                       term_clone(base_term), base_val, true);

        /* Register the alias (e.g. "N") as the same base embed type */
        if (ast->refinement.alias_name && strlen(ast->refinement.alias_name) > 0) {
            dep_env_define(ctx->globals, ast->refinement.alias_name, val_universe_n(0),
                           term_clone(base_term), val_embed(type_clone(base_ground)), true);
        }

        if (out_type) *out_type = term_type_n(0);
        return term_fvar("True");
    }

    /* Intercept top-level (layout ...) */
    if (ast->type == AST_LAYOUT) {
        char type_name[256];
        snprintf(type_name, sizeof(type_name), "__type_%s", ast->layout.name);
        dep_env_declare(ctx->globals, type_name, val_universe_n(0));

        Term *ctor_ty = term_fvar(type_name);
        for (int i = ast->layout.field_count - 1; i >= 0; i--) {
            ASTLayoutField *f = &ast->layout.fields[i];
            Term *dom = NULL;
            if (f->is_array) {
                dom = term_hole();
            } else {
                AST *tast = parse(f->type_name);
                dom = dep_term_of_type_ast(ctx, tast);
                ast_free(tast);
            }

            char acc_name[256];
            snprintf(acc_name, sizeof(acc_name), "__field_%s_%d", ast->layout.name, i);

            Term *acc_ty = term_pi("v", term_fvar(type_name), term_clone(dom), IMPLICIT_EXPLICIT);
            EvalEnv *empty_env1 = eval_env_empty();
            Value *acc_val_ty = dep_eval(acc_ty, empty_env1, ctx->mctx);
            eval_env_free(empty_env1);
            dep_env_declare(ctx->globals, acc_name, acc_val_ty);
            /* DO NOT FREE acc_ty: acc_val_ty captures its body in a closure! */

            /* Use purely synthetic internal variable names to prevent shadowing user variables */
            char synthetic_pname[64];
            snprintf(synthetic_pname, sizeof(synthetic_pname), "__ctor_p%d", i);
            ctor_ty = term_pi(synthetic_pname, dom, ctor_ty, IMPLICIT_EXPLICIT);
        }

        char ctor_name[256];
        snprintf(ctor_name, sizeof(ctor_name), "__ctor_%s", ast->layout.name);

        EvalEnv *empty_env2 = eval_env_empty();
        Value *ctor_val_ty = dep_eval(ctor_ty, empty_env2, ctx->mctx);
        eval_env_free(empty_env2);

        dep_env_declare(ctx->globals, ctor_name, ctor_val_ty);
        /* DO NOT FREE ctor_ty: ctor_val_ty captures its body in a closure! */

        return term_fvar("True");
    }

    /* Intercept top-level (data ...) */
    if (ast->type == AST_DATA) {
        char type_name[256];
        snprintf(type_name, sizeof(type_name), "__type_%s", ast->data.name);
        dep_env_declare(ctx->globals, type_name, val_universe_n(0));

        for (int i = 0; i < ast->data.constructor_count; i++) {
            ASTDataConstructor *c = &ast->data.constructors[i];
            if (!c->name) continue;
            /* Build result type: if data has type params, apply them.
             * data Maybe a => __type_Maybe applied to fresh meta for a */
            Term *ctor_result = term_fvar(type_name);
            /* For parameterized types, the constructor returns the applied type.
             * We represent Maybe a as __type_Maybe for now (monomorphic codegen). */
            Term *ctor_ty = ctor_result;
            for (int j = c->field_count - 1; j >= 0; j--) {
                const char *ftype_str = c->field_types[j];
                Term *dom = NULL;
                /* Check if the field type is a type parameter of the data type */
                bool is_type_param = false;
                if (ftype_str) {
                    for (int tp = 0; tp < ast->data.type_param_count; tp++) {
                        if (ast->data.type_params[tp] &&
                            strcmp(ftype_str, ast->data.type_params[tp]) == 0) {
                            is_type_param = true;
                            break;
                        }
                    }
                }
                if (is_type_param) {
                    /* Type parameter 'a' is erased to Universe at the dep level.
                     * Codegen handles it via pointer indirection (opaque/generic).
                     * We do NOT call meta_fresh here — the meta would be immediately
                     * unsolved and term_pi wrapping it causes dep_eval to return a
                     * VAL_META domain, which is correct but unnecessary overhead.
                     * A TERM_HOLE is lighter and has identical semantics here. */
                    dom = term_hole();
                } else {
                    if (!ftype_str) {
                        dom = term_hole();
                    } else {
                        AST *tast = parse(ftype_str);
                        dom = dep_term_of_type_ast(ctx, tast);
                        ast_free(tast);
                    }
                }

                char acc_name[256];
                snprintf(acc_name, sizeof(acc_name), "__field_%s_%d", c->name, j);
                Term *acc_ty = term_pi("v", term_fvar(type_name), term_clone(dom), IMPLICIT_EXPLICIT);
                EvalEnv *empty_env1 = eval_env_empty();
                Value *acc_val_ty = dep_eval(acc_ty, empty_env1, ctx->mctx);
                eval_env_free(empty_env1);
                dep_env_declare(ctx->globals, acc_name, acc_val_ty);
                /* DO NOT FREE acc_ty: acc_val_ty captures its body in a closure! */

                char pname[32]; snprintf(pname, sizeof(pname), "__ctor_p%d", j);
                ctor_ty = term_pi(pname, dom, ctor_ty, IMPLICIT_EXPLICIT);
            }
            char ctor_name[256];
            snprintf(ctor_name, sizeof(ctor_name), "__ctor_%s", c->name);
            EvalEnv *empty_env2 = eval_env_empty();
            Value *ctor_val_ty = dep_eval(ctor_ty, empty_env2, ctx->mctx);
            eval_env_free(empty_env2);
            dep_env_declare(ctx->globals, ctor_name, ctor_val_ty);
            /* DO NOT FREE ctor_ty: ctor_val_ty captures its body in a closure! */
        }
        return term_fvar("True");
    }

    /* Intercept top-level (define name value) */
    if (ast->type == AST_LIST && ast->list.count >= 3 &&
        ast->list.items[0]->type == AST_SYMBOL &&
        strcmp(ast->list.items[0]->symbol, "define") == 0) {

        AST *name_ast = ast->list.items[1];
        AST *val_ast  = ast->list.items[2];

        /* Support typed variable binding: (define [name :: Type] value)
         * The bracket list has the name at position 0 and, when present,
         * an explicit "::"-chain type annotation (e.g. Arr :: 16384 :: U8)
         * in the remaining positions. Capture that annotation BEFORE we
         * collapse name_ast down to the bare symbol, otherwise the type
         * is silently discarded and the variable's type gets inferred
         * from the value expression instead (e.g. [] becomes a zero-size
         * array of an unconstrained element type, which later collapses
         * to Int — breaking 'count' and any other Arr-specific codegen). */
        Type *declared_type = NULL;
        if (name_ast->type == AST_LIST && name_ast->list.count >= 1 &&
            name_ast->list.items[0]->type == AST_SYMBOL) {
            declared_type = parse_type_annotation(name_ast);
            name_ast = name_ast->list.items[0];
        }

        if (name_ast->type != AST_SYMBOL) {
            dep_error_set(ctx, name_ast->line, name_ast->column, "expected symbol for define");
            return NULL;
        }

        Term *val_term = dep_term_of_ast(ctx, val_ast);
        if (!val_term) return NULL;

        Value *ty;
        if (declared_type) {
            Value *declared_val = val_embed(declared_type);
            if (!dep_check(ctx, val_term, declared_val) || ctx->had_error) {
                term_free(val_term);
                return NULL;
            }
            ty = declared_val;
        } else {
            ty = dep_infer(ctx, val_term);
            if (!ty || ctx->had_error) {
                term_free(val_term);
                return NULL;
            }
        }

        /* Evaluate to resolve metas, then quote to get the fully elaborated core term */
        Value *raw_val = dep_eval(val_term, ctx->env, ctx->mctx);
        Term *elaborated_term = dep_quote(raw_val, 0, ctx->mctx);

        /* Evaluate the elaborated term so closures point to safe, persistent memory
         * owned by the DepEnv, instead of the transient val_term that main.c will free. */
        Value *elaborated_val = dep_eval(elaborated_term, ctx->env, ctx->mctx);

        /* Re-base the inferred type onto a persistent term so closures survive val_term being freed */
        Term *ty_term = dep_quote(ty, 0, ctx->mctx);
        EvalEnv *empty_env = eval_env_empty();
        Value *persistent_ty = dep_eval(ty_term, empty_env, ctx->mctx);
        eval_env_free(empty_env);

        /* Define the actual elaborated term in the environment */
        dep_env_define(ctx->globals, name_ast->symbol, persistent_ty, elaborated_term, elaborated_val, false);

        if (out_type) {
            Term *ty_term = dep_quote(ty, ctx->depth, ctx->mctx);
            *out_type = dep_normalise(ty_term, ctx->env, ctx->mctx);
            term_free(ty_term);
        }

        return val_term;
    }

    // 1. Surface → pre-term
    Term *t = dep_term_of_ast(ctx, ast);
    if (!t) return NULL;

    // 2. Bidirectional inference
    Value *ty = dep_infer(ctx, t);
    if (!ty || ctx->had_error) {
        return NULL;
    }

    // 3. Verify all metavariables were solved
    if (!meta_all_solved(ctx->mctx)) {
        int unsolved = 0;
        for (int i = 0; i < ctx->mctx->count; i++)
            if (ctx->mctx->entries[i].state == META_UNSOLVED) unsolved++;

        fprintf(stderr, "\n  \033[33m[dep] Warning: %d unsolved metavariable(s) remain\033[0m\n", unsolved);
        for (int i = 0; i < ctx->mctx->count; i++) {
            if (ctx->mctx->entries[i].state == META_UNSOLVED) {
                MetaEntry *e = &ctx->mctx->entries[i];
                fprintf(stderr, "    • ?%d [%s] at depth %d\n", e->id, e->hint ? e->hint : "unknown", e->depth);
                if (e->type) {
                    Term *ty_term = dep_quote(e->type, 0, ctx->mctx);
                    fprintf(stderr, "      Expected type: %s\n", term_to_string(ty_term));
                    term_free(ty_term);
                }
            }
        }
        fprintf(stderr, "  - Hint: This is expected during bootstrap. Untyped library functions (like '>=') generate holes.\n\n");
    }

    // 4. (HM Bridging is now natively executed during elaboration!)

    // 5. Normalise the result type for output
    if (out_type) {
        Term *ty_term = dep_quote(ty, ctx->depth, ctx->mctx);
        Term *nf      = dep_normalise(ty_term, ctx->env, ctx->mctx);
        term_free(ty_term);
        *out_type = nf;
    }

    return t;
}


/// Builtins bootstrap

void dep_register_builtins(DepCtx *ctx) {
    DepEnv *env = ctx->globals;

    // Nat : Type 0
    dep_env_define(env, "Nat",  val_universe_n(0),
                   term_nat(), val_nat(), true);

    // zero : Nat
    dep_env_define(env, "zero", val_nat(),
                   term_zero(), val_zero(), true);

    // succ : Nat → Nat
    {
        Value *succ_ty = val_pi("n", val_nat(),
            (Closure){ .env = eval_env_empty(), .body = term_nat() },
            IMPLICIT_EXPLICIT);
        dep_env_declare(env, "succ", succ_ty);
    }

    // Bool : Type 0  (encoded as Nat: zero=False, succ zero=True)
    {
        Value *bool_ty = val_universe_n(0);
        dep_env_declare(env, "Bool", bool_ty);
    }
    dep_env_define(env, "True",  val_embed(type_bool()),
                   term_embed(type_bool()), val_embed(type_bool()), true);
    dep_env_define(env, "False", val_embed(type_bool()),
                   term_embed(type_bool()), val_embed(type_bool()), true);

    // Embed all ground types from types.h at universe 0
    const char *ground_names[] = {
        "Int", "Float", "Char", "String", "Bool",
        "I8","U8","I16","U16","I32","U32","I64","U64","I128","U128",
        "F32","F80","Ratio","Symbol","Keyword",
        NULL
    };
    for (int i = 0; ground_names[i]; i++) {
        Type *g = type_from_name(ground_names[i]);
        if (g) {
            dep_env_define(env, ground_names[i],
                           val_universe_n(0),
                           term_embed(g), val_embed(g), true);
        }
    }

    // fst : Π{A: Type u}. Π{B: A → Type u}. Σ(x:A).B x → A
    // snd : Π{A: Type u}. Π{B: A → Type u}. Π(p: Σ(x:A).B x). B (fst p)
    // (These are axiomatic; their type is checked at call sites)
    dep_env_declare(env, "fst",   val_universe_n(0));  // placeholder
    dep_env_declare(env, "snd",   val_universe_n(0));  // placeholder

    // List Bootstrap (for Array Desugaring)
    dep_env_declare(env, "Nil",   val_universe_n(0));  // placeholder
    dep_env_declare(env, "Cons",  val_universe_n(0));  // placeholder

    // refl : Π{A: Type u}. Π{a: A}. a ≡ a
    dep_env_declare(env, "refl",  val_universe_n(0));  // placeholder

    /* subst : Π{A: Type u}. Π{a b: A}. a ≡ b → Π(P: A→Type u). P a → P b */
    dep_env_declare(env, "subst", val_universe_n(0));  // placeholder

    // sym : Π{A:Type}. Π{a b:A}. a≡b → b≡a
    dep_env_declare(env, "sym",   val_universe_n(0));  // placeholder

    // trans : Π{A:Type}. Π{a b c:A}. a≡b → b≡c → a≡c
    dep_env_declare(env, "trans", val_universe_n(0));  // placeholder

    // cong : Π{A B:Type}. Π(f:A→B). Π{a b:A}. a≡b → f a≡f b
    dep_env_declare(env, "cong",  val_universe_n(0));  // placeholder

    // ── Standard Library Bootstrapping ─────────────────────────────
    Term *t_int  = term_embed(type_int());
    Term *t_bool = term_embed(type_bool());
    Term *t_str  = term_embed(type_string());

    // Helper to generate A -> B
    #define ARROW(a, b) term_pi("_", (a), (b), IMPLICIT_EXPLICIT)

    Term *bool_bool_bool = ARROW(term_clone(t_bool), ARROW(term_clone(t_bool), term_clone(t_bool)));

    Term *poly_poly_bool =
        term_pi("A", term_type_n(0),
            term_pi("_", term_bvar(0),
                term_pi("_", term_bvar(1),
                    term_clone(t_bool),
                    IMPLICIT_EXPLICIT),
                IMPLICIT_EXPLICIT),
            IMPLICIT_IMPLICIT);

    Term *poly_poly_poly =
        term_pi("A", term_type_n(0),
            term_pi("_", term_bvar(0),
                term_pi("_", term_bvar(1),
                    term_bvar(2),
                    IMPLICIT_EXPLICIT),
                IMPLICIT_EXPLICIT),
            IMPLICIT_IMPLICIT);

    EvalEnv *ee = eval_env_empty();

    // Math & Logic
    dep_env_declare(env, "+",   dep_eval(poly_poly_poly, ee, NULL));
    dep_env_declare(env, "-",   dep_eval(poly_poly_poly, ee, NULL));
    dep_env_declare(env, "*",   dep_eval(poly_poly_poly, ee, NULL));
    dep_env_declare(env, "/",   dep_eval(poly_poly_poly, ee, NULL));
    dep_env_declare(env, "and", dep_eval(bool_bool_bool, ee, NULL));
    dep_env_declare(env, "or",  dep_eval(bool_bool_bool, ee, NULL));

    dep_env_declare(env, ">=",  dep_eval(poly_poly_bool, ee, NULL));
    dep_env_declare(env, "<=",  dep_eval(poly_poly_bool, ee, NULL));
    dep_env_declare(env, ">",   dep_eval(poly_poly_bool, ee, NULL));
    dep_env_declare(env, "<",   dep_eval(poly_poly_bool, ee, NULL));
    dep_env_declare(env, "=",   dep_eval(poly_poly_bool, ee, NULL));
    dep_env_declare(env, "!=",  dep_eval(poly_poly_bool, ee, NULL));

    // show : Π{A : Type 0}. A -> String
    Term *show_ty = term_pi("A", term_type_n(0),
                        ARROW(term_bvar(0), term_clone(t_str)),
                        IMPLICIT_IMPLICIT);
    dep_env_declare(env, "show", dep_eval(show_ty, ee, NULL));

    eval_env_free(ee);

    /*
     * TEMPORARY SHADOW PASS HACK:
     * We intentionally do NOT free the type terms (int_int_int, if_ty, etc.) here.
     * The NbE 'Closure' struct holds direct C pointers to these Term ASTs for lazy evaluation.
     * Freeing them causes a Use-After-Free memory corruption when the type checker
     * evaluates standard library calls later on!
     */
    #undef ARROW
}


/// Error reporting

void dep_error_set(DepCtx *ctx, int line, int col, const char *fmt, ...) {
    if (ctx->had_error) return;   // keep the first error only
    ctx->had_error = true;
    char msg[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    snprintf(ctx->error_msg, sizeof(ctx->error_msg),
             "%s:%d:%d: Dependent Type Error%s",
             ctx->filename, line, col, msg);
}

void dep_report_holes(DepCtx *ctx) {
    if (!ctx || !ctx->mctx) return;
    int hole_count = 0;
    for (int i = 0; i < ctx->mctx->count; i++) {
        MetaEntry *e = &ctx->mctx->entries[i];
        /* Only report metas that came from explicit ? holes */
        if (!e->hint) continue;
        bool is_hole = (strcmp(e->hint, "_") == 0 || strcmp(e->hint, "?") == 0);
        if (!is_hole) continue;
        hole_count++;
    }
    if (hole_count == 0) return;

    fprintf(stderr, "\n");
    for (int i = 0; i < ctx->mctx->count; i++) {
        MetaEntry *e = &ctx->mctx->entries[i];
        if (!e->hint) continue;
        bool is_hole = (strcmp(e->hint, "_") == 0 || strcmp(e->hint, "?") == 0);
        if (!is_hole) continue;

        int line = 0, col = 0;
        const char *file = ctx->filename ? ctx->filename : "<unknown>";
        /* Try to recover source location from the meta's source term */
        /* We use depth as a proxy for now; real location needs Term->line */

        if (e->state == META_SOLVED) {
            Term *sol = e->solution;
            const char *sol_str = sol ? term_to_string(sol) : "?";
            /* Emit in compiler-clickable format */
            fprintf(stderr, "%s:%d:%d: hole ?%d filled: %s\n",
                    file, line, col, e->id, sol_str);
        } else {
            /* Unsolved hole — show what type was expected */
            const char *ty_str = "unknown";
            if (e->type) {
                Term *qt = dep_quote(e->type, 0, ctx->mctx);
                ty_str = term_to_string(qt);
            }
            fprintf(stderr, "%s:%d:%d: hole ?%d has type: %s\n",
                    file, line, col, e->id, ty_str);
        }
    }
    fprintf(stderr, "\n");
}

void dep_error_print(DepCtx *ctx) {
    if (!ctx->had_error) return;
    fprintf(stderr, "%s\n", ctx->error_msg);
}


/// Pretty printing

void dep_print_term(Term *t) {
    printf("%s", term_to_string(t));
}

void dep_print_value(Value *v, MetaCtx *mctx) {
    if (!v) { printf("?"); return; }
    Term *t = dep_quote(v, 0, mctx);
    printf("%s", term_to_string(t));
    term_free(t);
}

void dep_print_ctx(DepCtx *ctx) {
    printf("[dep context — depth %d]\n", ctx->depth);
    int i = ctx->depth - 1;
    for (DepCtxEntry *e = ctx->locals; e; e = e->next, i--) {
        printf("  %d  %s : ", i, e->name);
        dep_print_value(e->type, ctx->mctx);
        printf("\n");
    }
}

void dep_print_meta_ctx(MetaCtx *mctx) {
    if (!mctx) return;
    printf("[metavariables — %d total]\n", mctx->count);
    for (int i = 0; i < mctx->count; i++) {
        MetaEntry *e = &mctx->entries[i];
        printf("  ?%d [%s]", e->id,
               e->state == META_SOLVED ? "solved" : "UNSOLVED");
        if (e->hint) printf(" (%s)", e->hint);
        if (e->state == META_SOLVED) {
            printf(" = %s", term_to_string(e->solution));
        }
        printf("\n");
    }
}
