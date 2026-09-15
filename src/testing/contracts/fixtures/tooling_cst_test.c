#include "cst.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    const char *source =
        ";;; Example.mon --- lossless syntax\n"
        "\n"
        "define values\n"
        "  { :text -> \"a ; not-comment\"\n"
        "    :count -> 2 }  ;; trailing commentary\n"
        "-| paragraph\n"
        "   commentary |-\n";

    ToolingCstDocument *first = tooling_cst_parse("Example.mon", source);
    assert(first);
    assert(tooling_cst_validate(first));
    assert(tooling_cst_token_count(first) > 0);

    size_t rendered_length = 0;
    char *rendered = tooling_cst_render(first, &rendered_length);
    assert(rendered);
    assert(rendered_length == strlen(source));
    assert(memcmp(rendered, source, rendered_length) == 0);
    char *code_projection = tooling_cst_code_projection(first);
    assert(code_projection);
    assert(strlen(code_projection) == strlen(source));
    assert(strstr(code_projection, "not-comment") == NULL);
    assert(strstr(code_projection, "trailing commentary") == NULL);
    assert(strstr(code_projection, "define values") != NULL);

    ToolingCstDocument *second = tooling_cst_parse("Example.mon", rendered);
    assert(second);
    assert(tooling_cst_fingerprint(first) == tooling_cst_fingerprint(second));

    bool saw_string = false, saw_line_comment = false, saw_block_comment = false;
    for (size_t i = 0; i < tooling_cst_token_count(first); i++) {
        const ToolingCstToken *token = tooling_cst_token(first, i);
        assert(token);
        saw_string |= token->kind == TOOLING_CST_STRING;
        saw_line_comment |= token->kind == TOOLING_CST_LINE_COMMENT;
        saw_block_comment |= token->kind == TOOLING_CST_BLOCK_COMMENT;
    }
    assert(saw_string && saw_line_comment && saw_block_comment);
    assert(tooling_cst_node_count(first) >= 2);
    assert(!tooling_cst_has_errors(first));
    const ToolingCstNode *root = tooling_cst_root(first);
    assert(root && root->kind == TOOLING_CST_ROOT);

    bool saw_brace_group = false;
    for (size_t i = 0; i < tooling_cst_node_count(first); i++) {
        const ToolingCstNode *node = tooling_cst_node(first, i);
        assert(node);
        saw_brace_group |= node->kind == TOOLING_CST_BRACE_GROUP;
    }
    assert(saw_brace_group);

    const char *broken_source = "define broken ([1 2}\n";
    ToolingCstDocument *broken = tooling_cst_parse("Broken.mon", broken_source);
    assert(broken);
    assert(tooling_cst_validate(broken));
    assert(tooling_cst_has_errors(broken));
    char *broken_rendered = tooling_cst_render(broken, &rendered_length);
    assert(broken_rendered);
    assert(rendered_length == strlen(broken_source));
    assert(memcmp(broken_rendered, broken_source, rendered_length) == 0);

    free(rendered);
    free(code_projection);
    tooling_cst_dispose(second);
    tooling_cst_dispose(first);
    tooling_cst_dispose(broken);
    free(broken_rendered);
    return 0;
}
