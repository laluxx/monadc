#include "wisp_syntax_policy.h"

#include <string.h>

int wisp_syntax_operator_precedence(const char *name) {
    if (!name) return -1;
    if (strcmp(name, "|>")  == 0) return 1;
    if (strcmp(name, "or")  == 0) return 1;
    if (strcmp(name, "and") == 0) return 2;
    if (strcmp(name, "=")   == 0 || strcmp(name, "!=") == 0 ||
        strcmp(name, "<")   == 0 || strcmp(name, ">")  == 0 ||
        strcmp(name, "<=")  == 0 || strcmp(name, ">=") == 0) return 3;
    if (strcmp(name, "+")   == 0 || strcmp(name, "-")  == 0 ||
        strcmp(name, "mod") == 0 || strcmp(name, "%")  == 0) return 4;
    if (strcmp(name, "*")   == 0 || strcmp(name, "/")  == 0) return 5;
    return 6;
}

bool wisp_syntax_token_can_call_group(const char *text) {
    if (!text || !text[0])
        return false;

    unsigned char c = (unsigned char)text[0];
    if ((c >= '0' && c <= '9') ||
        (text[0] == '-' && text[1] >= '0' && text[1] <= '9') ||
        text[0] == '"' || text[0] == '\'')
        return false;

    return true;
}

bool wisp_syntax_group_contains_quote(const char *text) {
    if (!text)
        return false;

    for (const char *p = text; p[0] && p[1]; p++) {
        if (p[0] != '\'')
            continue;
        if ((p[1] == '(' || p[1] == '[' || p[1] == '{') &&
            p[2] != '\'')
            return true;

        const char *q = p + 1;
        bool closes_char = false;
        while (*q && *q != ' ' && *q != '\t' && *q != '\n' &&
               *q != ')' && *q != ']' && *q != '}') {
            if (*q == '\\' && q[1]) {
                q += 2;
                continue;
            }
            if (*q == '\'') {
                closes_char = true;
                break;
            }
            q++;
        }
        if (!closes_char)
            return true;
    }
    return false;
}

bool wisp_syntax_is_infix_operator(const char *text) {
    return text &&
           (strcmp(text, "&")   == 0 ||
            strcmp(text, "+")   == 0 ||
            strcmp(text, "-")   == 0 ||
            strcmp(text, "*")   == 0 ||
            strcmp(text, "/")   == 0 ||
            strcmp(text, "%")   == 0 ||
            strcmp(text, "=")   == 0 ||
            strcmp(text, "!=")  == 0 ||
            strcmp(text, "<")   == 0 ||
            strcmp(text, ">")   == 0 ||
            strcmp(text, "<=")  == 0 ||
            strcmp(text, ">=")  == 0 ||
            strcmp(text, "++")  == 0 ||
            strcmp(text, "and") == 0 ||
            strcmp(text, "or")  == 0 ||
            strcmp(text, "mod") == 0);
}
