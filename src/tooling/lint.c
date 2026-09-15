#include "lint.h"
#include "cst.h"
#include "rewrite.h"
#include "compat.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <process.h>
#define lint_process_id _getpid
#else
#include <unistd.h>
#define lint_process_id getpid
#endif

typedef struct { char **items; size_t count, capacity; } PathList;

static const LintRule LINT_RULES[] = {
    {"style/unary-receiver", "Use the implicit receiver in unary guards",
     LINT_PHASE_SOURCE, LINT_WARNING, LINT_FIX_SAFE},
    {"style/receiver-relative-guard", "Use receiver-relative unary guard syntax",
     LINT_PHASE_SOURCE, LINT_WARNING, LINT_FIX_SAFE},
    {"style/commentary-prose", "Let Commentary prose be prose",
     LINT_PHASE_SOURCE, LINT_WARNING, LINT_FIX_SAFE},
    {"style/multiline-asm", "Lay inline assembly out as a multiline body",
     LINT_PHASE_SOURCE, LINT_WARNING, LINT_FIX_SUGGESTED},
    {"style/first-class-test-metadata", "Use first-class test metadata",
     LINT_PHASE_SOURCE, LINT_WARNING, LINT_FIX_SUGGESTED},
    {"style/arrow-map-literal", "Use arrows in map literals",
     LINT_PHASE_SOURCE, LINT_WARNING, LINT_FIX_SUGGESTED},
    {"style/grouped-module-exports", "Group a large module export catalogue",
     LINT_PHASE_SOURCE, LINT_WARNING, LINT_FIX_NONE},
    {"style/group-repeated-pattern-guards", "Group repeated guarded patterns",
     LINT_PHASE_SOURCE, LINT_WARNING, LINT_FIX_SAFE},
};

const LintRule *lint_rule_find(const char *id) {
    for (size_t i = 0; i < sizeof(LINT_RULES) / sizeof(*LINT_RULES); i++)
        if (strcmp(LINT_RULES[i].id, id) == 0) return &LINT_RULES[i];
    return NULL;
}

static char *copy_n(const char *text, size_t length) {
    char *copy = malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

static char *copy_text(const char *text) {
    return text ? copy_n(text, strlen(text)) : NULL;
}

void lint_result_init(LintResult *result) {
    diagnostic_set_init(result);
}

void lint_result_dispose(LintResult *result) {
    diagnostic_set_dispose(result);
}

static bool add_diagnostic(LintResult *result, const char *path,
                           const char *rule, LintSeverity severity,
                           size_t line, size_t column, size_t end_column,
                           const char *message, const char *suggestion,
                           const char *replacement) {
    LintDiagnostic diagnostic = {0};
    const LintRule *definition = lint_rule_find(rule);
    diagnostic.path = copy_text(path);
    diagnostic.code = copy_text(rule);
    diagnostic.severity = severity;
    diagnostic.origin = DIAGNOSTIC_ORIGIN_COMPILER;
    diagnostic.complete = true;
    diagnostic.phase = definition ? definition->phase : LINT_PHASE_SOURCE;
    LintFixSafety safety = definition ? definition->fix_safety : LINT_FIX_NONE;
    diagnostic.applicability = safety == LINT_FIX_SAFE
        ? DIAGNOSTIC_EDIT_MACHINE_APPLICABLE
        : safety == LINT_FIX_SUGGESTED
            ? DIAGNOSTIC_EDIT_MAYBE_INCORRECT
            : DIAGNOSTIC_EDIT_NONE;
    diagnostic.line = diagnostic.end_line = line;
    diagnostic.column = column;
    diagnostic.end_column = end_column;
    diagnostic.message = copy_text(message);
    diagnostic.explanation = copy_text(suggestion);
    if (!diagnostic.path || !diagnostic.code || !diagnostic.message ||
        (suggestion && !diagnostic.explanation)) {
        diagnostic_dispose(&diagnostic);
        result->internal_error = true;
        return false;
    }
    if (replacement &&
        !diagnostic_add_edit(&diagnostic, path, line, column, line, end_column,
                             replacement, diagnostic.applicability)) {
        diagnostic_dispose(&diagnostic);
        result->internal_error = true;
        return false;
    }
    if (!diagnostic_set_push(result, &diagnostic)) {
        diagnostic_dispose(&diagnostic);
        result->internal_error = true;
        return false;
    }
    return true;
}

static bool add_multiline_diagnostic(
    LintResult *result, const char *path, const char *rule,
    size_t line, size_t column, size_t end_line, size_t end_column,
    const char *message, const char *suggestion, const char *replacement) {
    LintDiagnostic diagnostic = {0};
    const LintRule *definition = lint_rule_find(rule);
    diagnostic.path = copy_text(path);
    diagnostic.code = copy_text(rule);
    diagnostic.severity = definition ? definition->default_severity : LINT_WARNING;
    diagnostic.origin = DIAGNOSTIC_ORIGIN_COMPILER;
    diagnostic.complete = true;
    diagnostic.phase = definition ? definition->phase : LINT_PHASE_SOURCE;
    diagnostic.applicability = DIAGNOSTIC_EDIT_MACHINE_APPLICABLE;
    diagnostic.line = line;
    diagnostic.column = column;
    diagnostic.end_line = end_line;
    diagnostic.end_column = end_column;
    diagnostic.message = copy_text(message);
    diagnostic.explanation = copy_text(suggestion);
    if (!diagnostic.path || !diagnostic.code || !diagnostic.message ||
        !diagnostic.explanation ||
        !diagnostic_add_edit(&diagnostic, path, line, column,
                             end_line, end_column, replacement,
                             DIAGNOSTIC_EDIT_MACHINE_APPLICABLE) ||
        !diagnostic_set_push(result, &diagnostic)) {
        diagnostic_dispose(&diagnostic);
        result->internal_error = true;
        return false;
    }
    return true;
}

static const char *skip_space(const char *cursor, const char *end) {
    while (cursor < end && (*cursor == ' ' || *cursor == '\t')) cursor++;
    return cursor;
}

static const char *scan_name(const char *cursor, const char *end) {
    while (cursor < end &&
           (isalnum((unsigned char)*cursor) || *cursor == '_' ||
            *cursor == '-' || *cursor == '?' || *cursor == '!')) cursor++;
    return cursor;
}

static bool starts_comparison(const char *cursor, const char *end) {
    if (cursor >= end) return false;
    return *cursor == '<' || *cursor == '>' || *cursor == '=' ||
           (*cursor == '!' && cursor + 1 < end && cursor[1] == '=');
}

static const char *scan_comparison_operator(const char *cursor,
                                             const char *end) {
    if (cursor >= end || !strchr("<>=!", *cursor)) return cursor;
    cursor++;
    if (cursor < end && *cursor == '=') cursor++;
    return cursor;
}

static bool parse_prefix_comparison(const char **cursor, const char *end,
                                    const char *receiver, size_t receiver_length,
                                    char **normalized) {
    const char *p = skip_space(*cursor, end);
    if (p >= end || *p++ != '(') return false;
    p = skip_space(p, end);
    const char *operator_start = p;
    const char *operator_end = scan_comparison_operator(p, end);
    if (operator_end == operator_start) return false;
    p = skip_space(operator_end, end);
    if ((size_t)(end - p) < receiver_length ||
        strncmp(p, receiver, receiver_length) != 0 ||
        (p + receiver_length < end &&
         !isspace((unsigned char)p[receiver_length]))) return false;
    p = skip_space(p + receiver_length, end);
    const char *operand_start = p;
    while (p < end && *p != ')' && !isspace((unsigned char)*p)) p++;
    const char *operand_end = p;
    p = skip_space(p, end);
    if (operand_end == operand_start || p >= end || *p++ != ')') return false;

    size_t length = (size_t)(operator_end - operator_start) + 1 +
                    (size_t)(operand_end - operand_start);
    char *text = malloc(length + 1);
    if (!text) return false;
    snprintf(text, length + 1, "%.*s %.*s",
             (int)(operator_end - operator_start), operator_start,
             (int)(operand_end - operand_start), operand_start);
    *cursor = p;
    *normalized = text;
    return true;
}

static char *normalize_prefix_guard(const char *start, const char *end,
                                    const char *receiver,
                                    size_t receiver_length) {
    const char *p = skip_space(start, end);
    if (p >= end || *p != '(') return NULL;

    const char *after_open = skip_space(p + 1, end);
    if ((size_t)(end - after_open) >= 3 &&
        strncmp(after_open, "and", 3) == 0 &&
        after_open + 3 < end && isspace((unsigned char)after_open[3])) {
        p = after_open + 3;
        char *left = NULL, *right = NULL;
        if (!parse_prefix_comparison(&p, end, receiver, receiver_length, &left) ||
            !parse_prefix_comparison(&p, end, receiver, receiver_length, &right)) {
            free(left);
            free(right);
            return NULL;
        }
        p = skip_space(p, end);
        if (p >= end || *p++ != ')' || skip_space(p, end) != end) {
            free(left);
            free(right);
            return NULL;
        }
        size_t length = strlen(left) + strlen(right) + 6;
        char *text = malloc(length);
        if (text) snprintf(text, length, "%s and %s", left, right);
        free(left);
        free(right);
        return text;
    }

    p = start;
    char *comparison = NULL;
    if (!parse_prefix_comparison(&p, end, receiver, receiver_length,
                                 &comparison) || skip_space(p, end) != end) {
        free(comparison);
        return NULL;
    }
    return comparison;
}

static bool same_name(const char *start, const char *end,
                      const char *name, size_t name_length) {
    return (size_t)(end - start) == name_length &&
           strncmp(start, name, name_length) == 0;
}

static void check_receiver_relative_guard(const char *path,
                                          const char *start, const char *end,
                                          size_t line, const char *active_name,
                                          size_t active_name_length,
                                          bool continuation,
                                          LintResult *result) {
    const char *receiver = skip_space(start, end);
    const char *receiver_end = scan_name(receiver, end);
    if (receiver == receiver_end ||
        !same_name(receiver, receiver_end, active_name, active_name_length))
        return;
    const char *after_receiver = skip_space(receiver_end, end);

    if (continuation && after_receiver < end && *after_receiver != '|') {
        const char *arrow = strstr(after_receiver, "->");
        if (arrow && arrow < end) {
            const char *body = skip_space(arrow + 2, end);
            const char *body_end = scan_name(body, end);
            if ((same_name(body, body_end, "False", 5) ||
                 same_name(body, body_end, "True", 4)) &&
                skip_space(body_end, end) == end) {
                add_diagnostic(result, path, "style/receiver-relative-guard",
                               LINT_WARNING, line,
                               (size_t)(receiver - start) + 1,
                               (size_t)(arrow - start) + 1,
                               "a unary guarded definition ends with an otherwise clause",
                               "write `| otherwise ->` instead of a repeated catch-all name",
                               "  | otherwise ");
            }
        }
        return;
    }
    if (after_receiver >= end || *after_receiver != '|') return;

    const char *guard = skip_space(after_receiver + 1, end);
    const char *arrow = strstr(guard, "->");
    if (!arrow || arrow >= end) return;
    const char *guard_end = arrow;
    while (guard_end > guard && isspace((unsigned char)guard_end[-1])) guard_end--;
    char *replacement = normalize_prefix_guard(guard, guard_end,
                                               active_name,
                                               active_name_length);
    if (!replacement) return;

    /* Emit the earlier edit first.  The current atomic fixer walks
     * diagnostics backwards, so this guarantees that two edits on one line
     * are applied from right to left and their source columns stay stable. */
    if (continuation) {
        add_diagnostic(result, path, "style/receiver-relative-guard", LINT_WARNING,
                       line, (size_t)(receiver - start) + 1,
                       (size_t)(receiver_end - start) + 1,
                       "continuation guards inherit their unary receiver",
                       "align the continuation pipe beneath the first pipe", " ");
    }
    add_diagnostic(result, path, "style/receiver-relative-guard", LINT_WARNING,
                   line, (size_t)(guard - start) + 1,
                   (size_t)(guard_end - start) + 1,
                   "prefix guard calls obscure the unary receiver",
                   "use infix Boolean words and omit the receiver from comparisons",
                   replacement);
    free(replacement);
}

static void check_unary_receiver(const char *path, const char *start,
                                 const char *end, size_t line,
                                 LintResult *result) {
    const char *receiver = skip_space(start, end);
    if (receiver >= end || *receiver == ';') return;
    const char *receiver_end = scan_name(receiver, end);
    const char *pipe = skip_space(receiver_end, end);
    if (receiver_end == receiver || pipe >= end || *pipe != '|') return;
    const char *repeat = skip_space(pipe + 1, end);
    const char *repeat_end = scan_name(repeat, end);
    size_t name_length = (size_t)(receiver_end - receiver);
    if ((size_t)(repeat_end - repeat) != name_length ||
        strncmp(receiver, repeat, name_length) != 0) return;
    const char *guard = skip_space(repeat_end, end);
    if (!starts_comparison(guard, end)) return;
    const char *guard_end = strstr(guard, "->");
    if (!guard_end || guard_end > end) guard_end = end;
    while (guard_end > guard && isspace((unsigned char)guard_end[-1])) guard_end--;
    char *guard_text = copy_n(guard, (size_t)(guard_end - guard));
    if (!guard_text) { result->internal_error = true; return; }
    size_t size = strlen(guard_text) + 24;
    char *suggestion = malloc(size);
    if (!suggestion) { free(guard_text); result->internal_error = true; return; }
    snprintf(suggestion, size, "write `| %s`", guard_text);
    add_diagnostic(result, path, "style/unary-receiver", LINT_WARNING,
                   line, (size_t)(repeat - start) + 1,
                   (size_t)(guard - start) + 1,
                   "a unary guard already has an implicit receiver",
                   suggestion, "");
    free(suggestion);
    free(guard_text);
}

static void check_parenthesized_asm(const char *path, const char *start,
                                    const char *end, size_t line,
                                    LintResult *result) {
    const char *found = start;
    while ((found = strstr(found, "(asm ")) && found < end) {
        add_diagnostic(result, path, "style/multiline-asm", LINT_WARNING,
                       line, (size_t)(found - start) + 1,
                       (size_t)(found - start) + 5,
                       "inline assembly should be a detached multiline body",
                       "put `asm` on the line after `->` and align its instructions",
                       NULL);
        found += 5;
    }
}

static void check_legacy_map(const char *path, const char *start,
                             const char *end, size_t line,
                             LintResult *result) {
    const char *found = start;
    while ((found = strstr(found, "#{")) && found < end) {
        add_diagnostic(result, path, "style/arrow-map-literal", LINT_WARNING,
                       line, (size_t)(found - start) + 1,
                       (size_t)(found - start) + 3,
                       "map literals use arrows inside ordinary braces",
                       "write `{key -> value}`; `--fix` waits for parsed entry boundaries",
                       NULL);
        found += 2;
    }
}

static void check_commented_metadata(const char *path, const char *start,
                                     const char *end, size_t line,
                                     LintResult *result) {
    const char *cursor = skip_space(start, end);
    if (end - cursor < 8 || cursor[0] != ';' || cursor[1] != ';') return;
    while (cursor < end && *cursor == ';') cursor++;
    cursor = skip_space(cursor, end);
    if (end - cursor < 5 || strncmp(cursor, "TEST-", 5) != 0) return;
    const char *colon = memchr(cursor, ':', (size_t)(end - cursor));
    if (!colon) return;
    add_diagnostic(result, path, "style/first-class-test-metadata",
                   LINT_WARNING, line, (size_t)(cursor - start) + 1,
                   (size_t)(colon - start) + 2,
                   "test metadata is a first-class top-level form, not a comment",
                   "replace the comment prefix and key colon with `:TEST-KEY value`",
                   NULL);
}

static bool section_marker(const char *start, const char *end,
                           const char *marker) {
    const char *cursor = skip_space(start, end);
    size_t length = strlen(marker);
    return (size_t)(end - cursor) >= length &&
           strncmp(cursor, marker, length) == 0;
}

static void check_large_flat_exports(const char *path, const char *source,
                                     LintResult *result) {
    const size_t threshold = 512;
    const char *line = source;
    size_t line_number = 1;
    while (*line) {
        const char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        const char *cursor = skip_space(line, end);
        if ((size_t)(end - cursor) >= 6 && strncmp(cursor, "module", 6) == 0 &&
            (cursor + 6 == end || isspace((unsigned char)cursor[6]))) {
            const char *open = memchr(cursor, '[', (size_t)(end - cursor));
            const char *scan_end = end;
            if (!open) {
                const char *next = *end ? end + 1 : end;
                const char *next_end = strchr(next, '\n');
                if (!next_end) next_end = next + strlen(next);
                open = memchr(next, '[', (size_t)(next_end - next));
                scan_end = next_end;
            }
            if (!open) return;
            size_t exports = 0;
            const char *scan = open + 1;
            while (*scan) {
                while (scan < scan_end && isspace((unsigned char)*scan)) scan++;
                if (scan == scan_end) {
                    if (!*scan_end) break;
                    scan = scan_end + 1;
                    scan_end = strchr(scan, '\n');
                    if (!scan_end) scan_end = scan + strlen(scan);
                    continue;
                }
                if (*scan == ']') break;
                const char *name_end = scan_name(scan, scan_end);
                if (name_end > scan) { exports++; scan = name_end; }
                else scan++;
            }
            if (exports > threshold)
                add_diagnostic(result, path, "style/grouped-module-exports",
                               LINT_WARNING, line_number,
                               (size_t)(cursor - line) + 1,
                               (size_t)(cursor - line) + 7,
                               "large flat export list hides the module's shape",
                               "use grouped `module … where` syntax with named `:sections` as an export catalogue",
                               NULL);
            return;
        }
        if (!*end) break;
        line = end + 1;
        line_number++;
    }
}

static void check_commentary_prose(const char *path, const char *start,
                                   const char *end, size_t line,
                                   bool in_commentary, LintResult *result) {
    if (!in_commentary) return;
    const char *cursor = skip_space(start, end);
    if (end - cursor < 2 || cursor[0] != ';' || cursor[1] != ';' ||
        (end - cursor >= 3 && cursor[2] == ';')) return;
    if (end - cursor > 2 && cursor[2] != ' ' && cursor[2] != '\t') return;
    add_diagnostic(
        result, path, "style/commentary-prose", LINT_WARNING, line,
        (size_t)(cursor - start) + 1, (size_t)(cursor - start) + 3,
        "commentary prose is wearing a second comment disguise",
        "`;;; Commentary:` already makes this prose non-code; drop the `;;` — they are looking a little sussy",
        "");
}

typedef struct {
    const char *guard_start, *guard_end;
    const char *body_start, *body_end;
} RepeatedGuardBranch;

static const char *top_level_character(const char *start, const char *end,
                                       char wanted) {
    int round = 0, square = 0, brace = 0;
    bool in_string = false, in_char = false, escaped = false;
    for (const char *p = start; p < end; p++) {
        if (in_string || in_char) {
            if (escaped) escaped = false;
            else if (*p == '\\') escaped = true;
            else if (in_string && *p == '"') in_string = false;
            else if (in_char && *p == '\'') in_char = false;
            continue;
        }
        if (*p == '"') { in_string = true; continue; }
        if (*p == '\'') { in_char = true; continue; }
        if (*p == '(') round++;
        else if (*p == ')') round--;
        else if (*p == '[') square++;
        else if (*p == ']') square--;
        else if (*p == '{') brace++;
        else if (*p == '}') brace--;
        else if (*p == wanted && !round && !square && !brace) return p;
    }
    return NULL;
}

static const char *line_arrow(const char *start, const char *end) {
    const char *dash = start;
    while ((dash = memchr(dash, '-', (size_t)(end - dash)))) {
        if (dash + 1 < end && dash[1] == '>') return dash;
        dash++;
    }
    return NULL;
}

static bool same_trimmed_text(const char *left, const char *left_end,
                              const char *right, const char *right_end) {
    while (left < left_end && isspace((unsigned char)*left)) left++;
    while (left_end > left && isspace((unsigned char)left_end[-1])) left_end--;
    while (right < right_end && isspace((unsigned char)*right)) right++;
    while (right_end > right && isspace((unsigned char)right_end[-1])) right_end--;
    return left_end - left == right_end - right &&
           strncmp(left, right, (size_t)(left_end - left)) == 0;
}

static void check_repeated_pattern_guards(const char *path, const char *source,
                                          LintResult *result) {
    const char *line = source;
    size_t line_number = 1;
    while (*line) {
        const char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        const char *first = skip_space(line, end);
        const char *pipe = top_level_character(first, end, '|');
        const char *arrow = pipe ? line_arrow(pipe + 1, end) : NULL;
        if (!pipe || !arrow) goto next_line;

        const char *pattern_end = pipe;
        while (pattern_end > first && isspace((unsigned char)pattern_end[-1]))
            pattern_end--;
        bool structured_pattern = false;
        for (const char *p = first; p < pattern_end; p++) {
            if (isspace((unsigned char)*p) || *p == '[' || *p == '(') {
                structured_pattern = true;
            }
        }
        if (!structured_pattern) goto next_line;
        size_t indent = (size_t)(first - line);
        RepeatedGuardBranch branches[64];
        size_t branch_count = 0, max_guard = 0;
        const char *scan = line;
        const char *scan_end = end;
        size_t scan_line = line_number;
        const char *last_branch_start = line;
        const char *last_branch_end = end;
        size_t last_branch_line = line_number;
        bool caught_fallback = false;

        while (branch_count < 64) {
            const char *scan_first = skip_space(scan, scan_end);
            if ((size_t)(scan_first - scan) != indent) break;
            const char *scan_pipe =
                top_level_character(scan_first, scan_end, '|');
            const char *scan_arrow = line_arrow(
                scan_pipe ? scan_pipe + 1 : scan_first, scan_end);
            if (!scan_arrow) break;
            const char *scan_pattern_end = scan_pipe ? scan_pipe : scan_arrow;
            if (!same_trimmed_text(first, pattern_end,
                                   scan_first, scan_pattern_end)) break;

            const char *guard_start, *guard_end;
            if (scan_pipe) {
                guard_start = skip_space(scan_pipe + 1, scan_arrow);
                guard_end = scan_arrow;
                while (guard_end > guard_start &&
                       isspace((unsigned char)guard_end[-1])) guard_end--;
            } else {
                guard_start = "otherwise";
                guard_end = guard_start + strlen(guard_start);
                caught_fallback = true;
            }
            const char *body_start = skip_space(scan_arrow + 2, scan_end);
            const char *body_end = scan_end;
            while (body_end > body_start &&
                   isspace((unsigned char)body_end[-1])) body_end--;
            branches[branch_count++] =
                (RepeatedGuardBranch){guard_start, guard_end,
                                      body_start, body_end};
            last_branch_start = scan;
            last_branch_end = scan_end;
            last_branch_line = scan_line;
            size_t guard_length = (size_t)(guard_end - guard_start);
            if (guard_length > max_guard) max_guard = guard_length;
            if (caught_fallback) break;
            if (!*scan_end) break;
            scan = scan_end + 1;
            scan_end = strchr(scan, '\n');
            if (!scan_end) scan_end = scan + strlen(scan);
            scan_line++;
        }

        /* A guarded constructor row followed by its unguarded twin is the
         * smallest complete decision table: the second row is semantically
         * `otherwise`.  Constructor syntax used to suppress this two-row
         * case, so exactly the most readable recursive clauses escaped the
         * grouping rule.  Unary bare-name clauses still do not reach this
         * checker because they are not structured patterns. */
        bool group_short_catch_all = branch_count == 2;
        bool group_guarded_run = branch_count >= 2 && !caught_fallback;
        if (((branch_count >= 3 || group_short_catch_all) && caught_fallback) ||
            group_guarded_run) {
            size_t capacity = (size_t)(last_branch_end - line) +
                              branch_count * (indent + max_guard + 16) + 1;
            char *replacement = malloc(capacity);
            if (!replacement) { result->internal_error = true; return; }
            size_t used = 0;
            memcpy(replacement + used, line, indent); used += indent;
            memcpy(replacement + used, first, (size_t)(pattern_end - first));
            used += (size_t)(pattern_end - first);
            replacement[used++] = '\n';
            for (size_t i = 0; i < branch_count; i++) {
                memcpy(replacement + used, line, indent); used += indent;
                replacement[used++] = ' ';
                replacement[used++] = ' ';
                replacement[used++] = '|';
                replacement[used++] = ' ';
                size_t guard_length =
                    (size_t)(branches[i].guard_end - branches[i].guard_start);
                memcpy(replacement + used, branches[i].guard_start, guard_length);
                used += guard_length;
                while (guard_length++ < max_guard) replacement[used++] = ' ';
                size_t body_length =
                    (size_t)(branches[i].body_end - branches[i].body_start);
                if (body_length) {
                    memcpy(replacement + used, " -> ", 4); used += 4;
                } else {
                    memcpy(replacement + used, " ->", 3); used += 3;
                }
                memcpy(replacement + used, branches[i].body_start, body_length);
                used += body_length;
                if (i + 1 < branch_count) replacement[used++] = '\n';
            }
            replacement[used] = '\0';
            add_multiline_diagnostic(
                result, path, "style/group-repeated-pattern-guards",
                line_number, 1, last_branch_line,
                (size_t)(last_branch_end - last_branch_start) + 1,
                "repeated pattern heads hide one guarded decision table",
                caught_fallback
                    ? "write the shared pattern once, then align its guards and use `otherwise` for the fallback"
                    : "write the shared pattern once, then align its guards",
                replacement);
            free(replacement);
            end = last_branch_end;
            line_number = last_branch_line;
        }

next_line:
        if (!*end) break;
        line = end + 1;
        line_number++;
    }
}

bool lint_source(const char *path, const char *source, LintResult *result) {
    if (!path || !source || !result) return false;
    ToolingCstDocument *document = tooling_cst_parse(path, source);
    char *code = document ? tooling_cst_code_projection(document) : NULL;
    if (!code) {
        tooling_cst_dispose(document);
        result->internal_error = true;
        return false;
    }
    check_large_flat_exports(path, code, result);
    /* This rule builds a whole-clause replacement, so it must retain literal
     * spelling from the original source rather than CST's whitespace-masked
     * code projection. Its scanner is quote- and nesting-aware. */
    check_repeated_pattern_guards(path, source, result);
    size_t line = 1;
    const char *cursor = code;
    const char *original = source;
    char active_guard_name[256] = {0};
    size_t active_guard_name_length = 0;
    bool have_guard_clause = false;
    bool in_commentary = false;
    while (*cursor) {
        const char *end = strchr(cursor, '\n');
        if (!end) end = cursor + strlen(cursor);
        const char *original_end = original + (end - cursor);
        bool starts_commentary =
            section_marker(original, original_end, ";;; Commentary:");
        bool starts_code =
            section_marker(original, original_end, ";;; Code:");
        if (starts_commentary) in_commentary = true;
        if (starts_code) in_commentary = false;
        const char *first = skip_space(cursor, end);
        const char *first_end = scan_name(first, end);
        const char *after_first = skip_space(first_end, end);
        bool explicit_guard = first_end > first && after_first < end &&
                              *after_first == '|';
        bool continuing_guard = have_guard_clause;
        if (explicit_guard && !have_guard_clause) {
            active_guard_name_length = (size_t)(first_end - first);
            if (active_guard_name_length < sizeof(active_guard_name)) {
                memcpy(active_guard_name, first, active_guard_name_length);
                active_guard_name[active_guard_name_length] = '\0';
                have_guard_clause = true;
            }
        }
        if (have_guard_clause)
            check_receiver_relative_guard(path, cursor, end, line,
                                          active_guard_name,
                                          active_guard_name_length,
                                          continuing_guard &&
                                          same_name(first, first_end,
                                                    active_guard_name,
                                                    active_guard_name_length),
                                          result);
        check_unary_receiver(path, cursor, end, line, result);
        check_parenthesized_asm(path, cursor, end, line, result);
        check_legacy_map(path, cursor, end, line, result);
        check_commented_metadata(path, original, original_end, line, result);
        check_commentary_prose(path, original, original_end, line,
                               in_commentary && !starts_commentary, result);
        if (first == end || (first < end && *first == ';')) {
            /* Blank/comment lines do not terminate a guarded definition. */
        } else if (!explicit_guard && have_guard_clause) {
            const char *arrow = strstr(after_first, "->");
            bool catch_all = same_name(first, first_end, active_guard_name,
                                       active_guard_name_length) &&
                             arrow && arrow < end;
            if (!catch_all) {
                have_guard_clause = false;
                active_guard_name_length = 0;
            }
        }
        if (!*end) break;
        cursor = end + 1;
        original = original_end + 1;
        line++;
    }
    free(code);
    tooling_cst_dispose(document);
    result->files_checked++;
    return !result->internal_error;
}

static bool add_path(PathList *list, const char *path) {
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2 : 32;
        void *items = realloc(list->items, capacity * sizeof(*list->items));
        if (!items) return false;
        list->items = items;
        list->capacity = capacity;
    }
    list->items[list->count] = copy_text(path);
    return list->items[list->count++] != NULL;
}

static bool monad_file(const char *path) {
    size_t length = strlen(path);
    return length >= 4 && strcmp(path + length - 4, ".mon") == 0;
}

static bool collect_paths(const char *path, PathList *list) {
    struct stat info;
    if (lstat(path, &info) != 0) return false;
    if (S_ISREG(info.st_mode)) return !monad_file(path) || add_path(list, path);
    if (!S_ISDIR(info.st_mode)) return true;
    DIR *directory = opendir(path);
    if (!directory) return false;
    bool ok = true;
    struct dirent *entry;
    while (ok && (entry = readdir(directory))) {
        if (entry->d_name[0] == '.') continue;
        size_t size = strlen(path) + strlen(entry->d_name) + 2;
        char *child = malloc(size);
        if (!child) { ok = false; break; }
        snprintf(child, size, "%s/%s", path, entry->d_name);
        ok = collect_paths(child, list);
        free(child);
    }
    closedir(directory);
    return ok;
}

static int compare_paths(const void *left, const void *right) {
    return strcmp(*(const char *const *)left, *(const char *const *)right);
}

static char *read_source(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) || ftell(file) < 0) { fclose(file); return NULL; }
    long length = ftell(file);
    if (fseek(file, 0, SEEK_SET)) { fclose(file); return NULL; }
    char *source = malloc((size_t)length + 1);
    if (!source) { fclose(file); return NULL; }
    size_t read = fread(source, 1, (size_t)length, file);
    fclose(file);
    if (read != (size_t)length) { free(source); return NULL; }
    source[read] = '\0';
    return source;
}

bool lint_path(const char *path, LintResult *result) {
    PathList paths = {0};
    if (!collect_paths(path, &paths)) {
        fprintf(stderr, "monad lint: cannot read '%s': %s\n", path, strerror(errno));
        result->internal_error = true;
    } else {
        qsort(paths.items, paths.count, sizeof(*paths.items), compare_paths);
        for (size_t i = 0; i < paths.count && !result->internal_error; i++) {
            char *source = read_source(paths.items[i]);
            if (!source) { result->internal_error = true; break; }
            lint_source(paths.items[i], source, result);
            free(source);
        }
    }
    for (size_t i = 0; i < paths.count; i++) free(paths.items[i]);
    free(paths.items);
    return !result->internal_error;
}

static bool write_source_atomically(const char *path, const char *source,
                                    size_t length) {
    struct stat original;
    if (stat(path, &original) != 0) return false;
    size_t temporary_size = strlen(path) + 48;
    char *temporary = malloc(temporary_size);
    if (!temporary) return false;
    snprintf(temporary, temporary_size, "%s.monad-lint.%ld.tmp", path,
             (long)lint_process_id());
    FILE *file = fopen(temporary, "wb");
    bool ok = file && fwrite(source, 1, length, file) == length;
    if (file && fclose(file) != 0) ok = false;
    if (ok && chmod(temporary, original.st_mode) != 0) ok = false;
    if (ok && rename(temporary, path) != 0) ok = false;
    if (!ok) remove(temporary);
    free(temporary);
    return ok;
}

static bool apply_fixes(const LintResult *result, size_t *fixed) {
    *fixed = 0;
    for (size_t cursor = 0; cursor < result->count;) {
        const char *path = result->items[cursor].path;
        size_t end = cursor + 1, file_fixes = 0;
        while (end < result->count &&
               strcmp(result->items[end].path, path) == 0) end++;
        char *source = read_source(path);
        if (!source) return false;
        ToolingRewritePlan plan;
        tooling_rewrite_plan_init(&plan);
        ToolingRewriteStatus planned =
            tooling_rewrite_plan_build(&plan, path, source, result);
        if (planned != TOOLING_REWRITE_READY &&
            planned != TOOLING_REWRITE_EMPTY) {
            fprintf(stderr, "monad lint: rewrite plan for '%s' is %s\n",
                    path, tooling_rewrite_status_name(planned));
            tooling_rewrite_plan_dispose(&plan);
            free(source);
            return false;
        }
        char *rewritten = NULL;
        size_t rewritten_length = 0;
        if (planned == TOOLING_REWRITE_READY) {
            ToolingRewriteStatus applied = tooling_rewrite_apply(
                &plan, source, &rewritten, &rewritten_length);
            if (applied != TOOLING_REWRITE_APPLIED ||
                !write_source_atomically(path, rewritten, rewritten_length)) {
                fprintf(stderr, "monad lint: checked rewrite for '%s' failed: %s\n",
                        path, tooling_rewrite_status_name(applied));
                free(rewritten);
                tooling_rewrite_plan_dispose(&plan);
                free(source);
                return false;
            }
            file_fixes = plan.edit_count;
        }
        free(rewritten);
        tooling_rewrite_plan_dispose(&plan);
        free(source);
        *fixed += file_fixes;
        cursor = end;
    }
    return true;
}

static void json_string(const char *text) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
        if (*p == '"' || *p == '\\') { putchar('\\'); putchar(*p); }
        else if (*p == '\n') fputs("\\n", stdout);
        else if (*p == '\r') fputs("\\r", stdout);
        else if (*p == '\t') fputs("\\t", stdout);
        else if (*p < 0x20) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}

static void print_json(const LintResult *result) {
    fputs("{\"diagnostics\":[", stdout);
    for (size_t i = 0; i < result->count; i++) {
        const LintDiagnostic *d = &result->items[i];
        if (i) putchar(',');
        fputs("{\"path\":", stdout); json_string(d->path);
        fputs(",\"rule\":", stdout); json_string(d->code);
        fputs(",\"severity\":", stdout); json_string(diagnostic_severity_name(d->severity));
        fputs(",\"phase\":", stdout); json_string(diagnostic_phase_name(d->phase));
        fputs(",\"origin\":", stdout); json_string(diagnostic_origin_name(d->origin));
        fputs(",\"complete\":", stdout); fputs(d->complete ? "true" : "false", stdout);
        fputs(",\"applicability\":", stdout);
        json_string(diagnostic_applicability_name(d->applicability));
        fputs(",\"fixSafety\":", stdout);
        json_string(d->applicability == DIAGNOSTIC_EDIT_MACHINE_APPLICABLE
                        ? "safe"
                        : d->applicability == DIAGNOSTIC_EDIT_MAYBE_INCORRECT
                            ? "suggested" : "none");
        printf(",\"line\":%zu,\"column\":%zu,\"endLine\":%zu,\"endColumn\":%zu,\"message\":",
               d->line, d->column, d->end_line, d->end_column);
        json_string(d->message);
        fputs(",\"suggestion\":", stdout); json_string(d->explanation);
        fputs(",\"edits\":[", stdout);
        for (size_t j = 0; j < d->edit_count; j++) {
            const DiagnosticEdit *edit = &d->edits[j];
            if (j) putchar(',');
            fputs("{\"path\":", stdout); json_string(edit->span.path);
            printf(",\"line\":%zu,\"column\":%zu,\"endLine\":%zu,\"endColumn\":%zu,\"applicability\":",
                   edit->span.line, edit->span.column,
                   edit->span.end_line, edit->span.end_column);
            json_string(diagnostic_applicability_name(edit->applicability));
            fputs(",\"replacement\":", stdout); json_string(edit->replacement);
            putchar('}');
        }
        fputs("],\"related\":[", stdout);
        for (size_t j = 0; j < d->related_count; j++) {
            const DiagnosticRelated *related = &d->related[j];
            if (j) putchar(',');
            fputs("{\"path\":", stdout); json_string(related->span.path);
            printf(",\"line\":%zu,\"column\":%zu,\"endLine\":%zu,\"endColumn\":%zu,\"message\":",
                   related->span.line, related->span.column,
                   related->span.end_line, related->span.end_column);
            json_string(related->message);
            putchar('}');
        }
        putchar(']');
        putchar('}');
    }
    printf("],\"filesChecked\":%zu}\n", result->files_checked);
}

int cmd_lint(const char *path, bool json, bool fix) {
    if (!path) { fputs("Usage: monad lint [--json] [--fix] <file-or-directory>\n", stderr); return 2; }
    LintResult result;
    lint_result_init(&result);
    lint_path(path, &result);
    size_t fixed = 0;
    if (fix && !result.internal_error) {
        if (!apply_fixes(&result, &fixed)) result.internal_error = true;
        if (!result.internal_error && fixed) {
            lint_result_dispose(&result);
            lint_result_init(&result);
            lint_path(path, &result);
        }
    }
    if (json) print_json(&result);
    else {
        if (fixed) printf("monad lint: fixed %zu diagnostic%s\n", fixed,
                          fixed == 1 ? "" : "s");
        for (size_t i = 0; i < result.count; i++) {
            LintDiagnostic *d = &result.items[i];
            printf("%s:%zu:%zu: %s: %s [%s]\n", d->path, d->line, d->column,
                   diagnostic_severity_name(d->severity), d->message, d->code);
            if (d->explanation) printf("  help: %s\n", d->explanation);
        }
        if (!result.internal_error && !result.count)
            printf("monad lint: no lint diagnostics in %zu file%s\n",
                   result.files_checked, result.files_checked == 1 ? "" : "s");
        else if (!result.internal_error)
            printf("monad lint: %zu diagnostic%s in %zu file%s\n",
                   result.count, result.count == 1 ? "" : "s",
                   result.files_checked, result.files_checked == 1 ? "" : "s");
    }
    int status = result.internal_error ? 2 : result.count ? 1 : 0;
    lint_result_dispose(&result);
    return status;
}
