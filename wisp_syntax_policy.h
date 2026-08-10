#ifndef WISP_SYNTAX_POLICY_H
#define WISP_SYNTAX_POLICY_H

#include <stdbool.h>

/*
 * Pure surface-syntax policy.  These queries classify already grouped text;
 * they do not inspect arities, mutate token streams, or perform expansion.
 */
int  wisp_syntax_operator_precedence(const char *name);
bool wisp_syntax_token_can_call_group(const char *text);
bool wisp_syntax_group_contains_quote(const char *text);
bool wisp_syntax_is_infix_operator(const char *text);

#endif
