/* scope_names.c — the names a C/C++ statement can see. See scope_names.h.
 *
 * One parse, one walk up from the line's first character to the enclosing
 * function. Each construct that opens a scope contributes the names it makes
 * visible at that position:
 *   compound_statement   declarations that end above the line
 *   for_statement        its initializer declaration
 *   for_range_loop       its loop variable, inside the body
 *   if/while/switch      declarations in the condition clause, past it
 *   catch_clause         its parameter, inside the body
 *   lambda_expression    its parameters and init-captures, inside the body;
 *                        every name found further out must cross its capture list
 *   function_definition  its parameters; the walk stops here
 * The parse leaves Unreal brace-block macro names out exactly as indexing does
 * (cbm_parser_skip_unreal_block_macros), so `ON_SCOPE_EXIT { ... };` bodies are
 * ordinary blocks here too. */
#include "scope_names.h"
#include "cbm.h" /* cbm_parser_skip_unreal_block_macros, cbm_is_brace_default_type_miss */
#include "tree_sitter/api.h"

/* cbm.h routes field lookups through a thread-local cache that only indexing
 * worker threads release. This module runs on MCP request threads, so it
 * calls tree-sitter directly and allocates nothing per thread. */
#undef ts_node_child_by_field_name

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const TSLanguage *tree_sitter_c(void);
extern const TSLanguage *tree_sitter_cpp(void);

enum { SCOPE_TYPE_MAX = 120, SCOPE_CAPTURES_MAX = 160, SCOPE_NAME_IDS_MAX = 32 };

typedef struct {
    TSNode spec;     /* lambda_capture_specifier, may be null */
    int first_outer; /* names at or after this index were declared outside the lambda */
    int line;
} scope_lambda_t;

typedef struct {
    const char *src;
    uint32_t len;
    cbm_scope_name_t *names;
    int count;
    int cap;
    scope_lambda_t *lambdas;
    int lambda_count;
    int lambda_cap;
    int depth;
    bool oom;
} scope_ctx_t;

/* Copy src[a, b) with every whitespace run collapsed to one space, trimmed,
 * and cut at `max` bytes. */
static char *scope_text(const char *src, uint32_t a, uint32_t b, size_t max) {
    if (b < a) {
        b = a;
    }
    size_t cap = (size_t)(b - a) < max ? (size_t)(b - a) : max;
    char *out = (char *)malloc(cap + 4U);
    if (!out) {
        return NULL;
    }
    size_t n = 0;
    bool space = false;
    for (uint32_t i = a; i < b && n < cap; i++) {
        char c = src[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            space = n > 0;
            continue;
        }
        if (space && n < cap) {
            out[n++] = ' ';
        }
        space = false;
        if (n < cap) {
            out[n++] = c;
        }
    }
    if (b - a > cap && n + 3U <= cap + 3U) {
        memcpy(out + n, "...", 3U);
        n += 3U;
    }
    out[n] = '\0';
    return out;
}

static char *scope_node_text(const scope_ctx_t *ctx, TSNode node, size_t max) {
    return scope_text(ctx->src, ts_node_start_byte(node), ts_node_end_byte(node), max);
}

static bool scope_is(TSNode node, const char *type) {
    return !ts_node_is_null(node) && strcmp(ts_node_type(node), type) == 0;
}

static TSNode scope_field(TSNode node, const char *field) {
    return ts_node_child_by_field_name(node, field, (uint32_t)strlen(field));
}

static bool scope_inside(TSNode node, uint32_t off) {
    return !ts_node_is_null(node) && ts_node_start_byte(node) <= off &&
           off < ts_node_end_byte(node);
}

/* The identifiers a declarator introduces: one, several for a structured
 * binding, none for a function declarator (a declaration, not a variable). */
static int scope_declarator_ids(TSNode d, TSNode *out, int n, int max) {
    if (ts_node_is_null(d) || n >= max) {
        return n;
    }
    const char *k = ts_node_type(d);
    if (strcmp(k, "identifier") == 0) {
        out[n++] = d;
        return n;
    }
    if (strcmp(k, "function_declarator") == 0) {
        return n;
    }
    if (strcmp(k, "structured_binding_declarator") == 0) {
        uint32_t c = ts_node_named_child_count(d);
        for (uint32_t i = 0; i < c && n < max; i++) {
            TSNode id = ts_node_named_child(d, i);
            if (scope_is(id, "identifier")) {
                out[n++] = id;
            }
        }
        return n;
    }
    TSNode inner = scope_field(d, "declarator");
    if (ts_node_is_null(inner)) {
        /* reference_declarator and friends carry the inner declarator unnamed */
        uint32_t c = ts_node_named_child_count(d);
        for (uint32_t i = 0; i < c; i++) {
            TSNode ch = ts_node_named_child(d, i);
            const char *ck = ts_node_type(ch);
            if (strcmp(ck, "identifier") == 0 || strstr(ck, "declarator") != NULL) {
                inner = ch;
                break;
            }
        }
    }
    return scope_declarator_ids(inner, out, n, max);
}

static void scope_push(scope_ctx_t *ctx, TSNode id, const char *kind, char *type) {
    if (ctx->oom) {
        free(type);
        return;
    }
    if (ctx->count == ctx->cap) {
        int cap = ctx->cap ? ctx->cap * 2 : 16;
        cbm_scope_name_t *grown =
            (cbm_scope_name_t *)realloc(ctx->names, (size_t)cap * sizeof(*grown));
        if (!grown) {
            ctx->oom = true;
            free(type);
            return;
        }
        ctx->names = grown;
        ctx->cap = cap;
    }
    cbm_scope_name_t *n = &ctx->names[ctx->count];
    memset(n, 0, sizeof(*n));
    n->name = scope_node_text(ctx, id, SCOPE_TYPE_MAX);
    n->kind = kind;
    n->type = type ? type : scope_text("", 0, 0, 0);
    n->line = (int)ts_node_start_point(id).row + 1;
    n->depth = ctx->depth;
    if (!n->name || !n->type) {
        free(n->name);
        free(n->type);
        ctx->oom = true;
        return;
    }
    ctx->count++;
}

/* The declared type as written: the owner's specifiers through its `type`
 * field, then any pointer or reference marks between the declarator's start
 * and the name. `int a, *b` gives "int" and "int*". */
static char *scope_type_of(const scope_ctx_t *ctx, TSNode owner, TSNode declarator, TSNode id,
                           bool structured) {
    TSNode type = scope_field(owner, "type");
    if (ts_node_is_null(type)) {
        return NULL;
    }
    /* Start at the first qualifier before the type (`const`, `static`), not at
     * the owner: a range-for loop starts at `for (`. */
    uint32_t spec_start = ts_node_start_byte(type);
    uint32_t c = ts_node_named_child_count(owner);
    for (uint32_t i = 0; i < c; i++) {
        TSNode ch = ts_node_named_child(owner, i);
        if (ts_node_start_byte(ch) >= ts_node_start_byte(type)) {
            break;
        }
        if (scope_is(ch, "type_qualifier") || scope_is(ch, "storage_class_specifier")) {
            spec_start = ts_node_start_byte(ch);
            break;
        }
    }
    char *spec = scope_text(ctx->src, spec_start, ts_node_end_byte(type), SCOPE_TYPE_MAX);
    if (!spec || structured || ts_node_is_null(declarator)) {
        return spec;
    }
    char *marks = scope_text(ctx->src, ts_node_start_byte(declarator), ts_node_start_byte(id),
                             SCOPE_TYPE_MAX);
    if (!marks || !marks[0]) {
        free(marks);
        return spec;
    }
    size_t a = strlen(spec);
    size_t b = 0;
    char *joined = (char *)malloc(a + strlen(marks) + 1U);
    if (joined) {
        memcpy(joined, spec, a);
        for (const char *p = marks; *p; p++) {
            if (*p != ' ') {
                joined[a + b++] = *p;
            }
        }
        joined[a + b] = '\0';
    }
    free(spec);
    free(marks);
    return joined;
}

/* Every declarator of `owner` (a declaration, parameter or range-for loop). */
static void scope_take_declarators(scope_ctx_t *ctx, TSNode owner, const char *kind) {
    uint32_t c = ts_node_child_count(owner);
    for (uint32_t i = 0; i < c; i++) {
        const char *field = ts_node_field_name_for_child(owner, i);
        if (!field || strcmp(field, "declarator") != 0) {
            continue;
        }
        TSNode d = ts_node_child(owner, i);
        TSNode ids[SCOPE_NAME_IDS_MAX];
        int n = scope_declarator_ids(d, ids, 0, SCOPE_NAME_IDS_MAX);
        bool structured = strstr(ts_node_type(d), "structured_binding") != NULL ||
                          (scope_is(d, "init_declarator") &&
                           scope_is(scope_field(d, "declarator"), "structured_binding_declarator"));
        for (int k = 0; k < n; k++) {
            scope_push(ctx, ids[k], kind, scope_type_of(ctx, owner, d, ids[k], structured));
        }
    }
}

static void scope_take_parameters(scope_ctx_t *ctx, TSNode params, const char *kind) {
    if (ts_node_is_null(params)) {
        return;
    }
    uint32_t c = ts_node_named_child_count(params);
    for (uint32_t i = 0; i < c; i++) {
        TSNode p = ts_node_named_child(params, i);
        const char *pk = ts_node_type(p);
        if (strstr(pk, "parameter_declaration") != NULL) {
            scope_take_declarators(ctx, p, kind);
        }
    }
}

/* Declarations anywhere in a condition clause: `if (auto* P = Find(); P)`. */
static void scope_take_condition(scope_ctx_t *ctx, TSNode clause) {
    if (ts_node_is_null(clause)) {
        return;
    }
    if (scope_is(clause, "declaration")) {
        scope_take_declarators(ctx, clause, "condition_variable");
        return;
    }
    uint32_t c = ts_node_named_child_count(clause);
    for (uint32_t i = 0; i < c; i++) {
        TSNode ch = ts_node_named_child(clause, i);
        if (scope_is(ch, "declaration") || scope_is(ch, "init_statement") ||
            scope_is(ch, "condition_clause")) {
            scope_take_condition(ctx, ch);
        }
    }
}

static void scope_take_block(scope_ctx_t *ctx, TSNode block, uint32_t off) {
    uint32_t c = ts_node_named_child_count(block);
    for (uint32_t i = 0; i < c; i++) {
        TSNode ch = ts_node_named_child(block, i);
        if (ts_node_end_byte(ch) > off) {
            break; /* this statement holds the line, or comes after it */
        }
        if (scope_is(ch, "declaration")) {
            scope_take_declarators(ctx, ch, "local");
        }
    }
}

static void scope_take_lambda(scope_ctx_t *ctx, TSNode lambda) {
    TSNode spec = scope_field(lambda, "captures");
    TSNode decl = scope_field(lambda, "declarator");
    if (!ts_node_is_null(decl)) {
        scope_take_parameters(ctx, scope_field(decl, "parameters"), "lambda_parameter");
    }
    if (!ts_node_is_null(spec)) {
        uint32_t c = ts_node_named_child_count(spec);
        for (uint32_t i = 0; i < c; i++) {
            TSNode ch = ts_node_named_child(spec, i);
            if (scope_is(ch, "lambda_capture_initializer")) {
                TSNode left = scope_field(ch, "left");
                if (scope_is(left, "identifier")) {
                    scope_push(ctx, left, "init_capture", NULL);
                }
            }
        }
    }
    if (ctx->lambda_count == ctx->lambda_cap) {
        int cap = ctx->lambda_cap ? ctx->lambda_cap * 2 : 4;
        scope_lambda_t *grown =
            (scope_lambda_t *)realloc(ctx->lambdas, (size_t)cap * sizeof(*grown));
        if (!grown) {
            ctx->oom = true;
            return;
        }
        ctx->lambdas = grown;
        ctx->lambda_cap = cap;
    }
    scope_lambda_t *l = &ctx->lambdas[ctx->lambda_count++];
    l->spec = spec;
    l->first_outer = ctx->count;
    l->line = (int)ts_node_start_point(lambda).row + 1;
}

/* How lambda `l` reaches `name`: an explicit capture, an init-capture of the
 * same name, its default, or not at all. Returns a static string, or NULL for
 * "not captured". */
static const char *scope_capture_mode(const scope_ctx_t *ctx, const scope_lambda_t *l,
                                      const char *name) {
    if (ts_node_is_null(l->spec)) {
        return NULL;
    }
    const char *dflt = NULL;
    uint32_t c = ts_node_child_count(l->spec);
    for (uint32_t i = 0; i < c; i++) {
        TSNode ch = ts_node_child(l->spec, i);
        if (!ts_node_is_named(ch)) {
            continue;
        }
        if (scope_is(ch, "lambda_default_capture")) {
            char *t = scope_node_text(ctx, ch, 4);
            dflt = t && t[0] == '&' ? "default [&]" : "default [=]";
            free(t);
        } else if (scope_is(ch, "identifier") || scope_is(ch, "lambda_capture_initializer")) {
            TSNode id = scope_is(ch, "identifier") ? ch : scope_field(ch, "left");
            if (ts_node_is_null(id)) {
                continue;
            }
            char *t = scope_node_text(ctx, id, SCOPE_TYPE_MAX);
            bool same = t && strcmp(t, name) == 0;
            free(t);
            if (!same) {
                continue;
            }
            if (scope_is(ch, "lambda_capture_initializer")) {
                return "by init-capture";
            }
            TSNode before = ts_node_prev_sibling(ch);
            return scope_is(before, "&") ? "by reference" : "by value";
        }
    }
    return dflt;
}

static void scope_resolve_captures(scope_ctx_t *ctx) {
    for (int i = 0; i < ctx->count && !ctx->oom; i++) {
        cbm_scope_name_t *n = &ctx->names[i];
        const char *mode = NULL;
        int missing_line = 0;
        /* lambdas[] is innermost first; one crosses this name when the name
         * was collected after that lambda's own names. */
        for (int k = ctx->lambda_count - 1; k >= 0; k--) {
            const scope_lambda_t *l = &ctx->lambdas[k];
            if (i < l->first_outer) {
                continue;
            }
            const char *m = scope_capture_mode(ctx, l, n->name);
            if (!m) {
                missing_line = l->line; /* keep the innermost lambda that misses it */
            } else {
                mode = m; /* ends on the innermost lambda's mode */
            }
        }
        if (missing_line > 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "NOT CAPTURED by lambda at line %d", missing_line);
            n->capture = strdup(buf);
        } else if (mode) {
            n->capture = strdup(mode);
        }
    }
}

static void scope_mark_shadowing(scope_ctx_t *ctx) {
    for (int i = 0; i < ctx->count; i++) {
        for (int j = 0; j < i; j++) {
            cbm_scope_name_t *inner = &ctx->names[j];
            cbm_scope_name_t *outer = &ctx->names[i];
            if (inner->depth < outer->depth && strcmp(inner->name, outer->name) == 0) {
                if (inner->shadows_line == 0) {
                    inner->shadows_line = outer->line;
                }
                outer->shadowed = true;
            }
        }
    }
}

/* Offset of the line's first non-blank byte (or its end when it is blank).
 * Returns false when the line does not exist. */
static bool scope_line_offset(const char *src, uint32_t len, int line, uint32_t *off) {
    if (line < 1) {
        return false;
    }
    uint32_t i = 0;
    for (int l = 1; l < line; l++) {
        while (i < len && src[i] != '\n') {
            i++;
        }
        if (i >= len) {
            return false;
        }
        i++;
    }
    if (i > len || (i == len && line > 1 && len > 0 && src[len - 1] == '\n')) {
        return false;
    }
    while (i < len && (src[i] == ' ' || src[i] == '\t' || src[i] == '\r')) {
        i++;
    }
    *off = i;
    return true;
}

/* The function_declarator of a function definition, under any pointer or
 * reference return wrappers (`FString& Get()`, `T* Make()`). */
static TSNode scope_function_declarator(TSNode fn) {
    TSNode d = scope_field(fn, "declarator");
    while (!ts_node_is_null(d) && !scope_is(d, "function_declarator")) {
        TSNode inner = scope_field(d, "declarator");
        if (ts_node_is_null(inner) && ts_node_named_child_count(d) > 0) {
            inner = ts_node_named_child(d, 0);
        }
        d = inner;
    }
    return d;
}

/* True when the subtree holds an ERROR or MISSING node that coverage would
 * also report: the `= {}` default-argument node is not one. */
static bool scope_has_real_error(TSNode n) {
    if (!ts_node_has_error(n)) {
        return false;
    }
    if (ts_node_is_missing(n)) {
        return !cbm_is_brace_default_type_miss(n);
    }
    if (ts_node_is_error(n)) {
        return true;
    }
    uint32_t c = ts_node_child_count(n);
    for (uint32_t i = 0; i < c; i++) {
        if (scope_has_real_error(ts_node_child(n, i))) {
            return true;
        }
    }
    return false;
}

static void scope_set_function(scope_ctx_t *ctx, TSNode fn, cbm_scope_result_t *out) {
    TSNode d = scope_function_declarator(fn);
    TSNode name = ts_node_is_null(d) ? d : scope_field(d, "declarator");
    out->function = ts_node_is_null(name) ? strdup("") : scope_node_text(ctx, name, 200);
    out->function_line = (int)ts_node_start_point(fn).row + 1;
    out->parse_partial = scope_has_real_error(fn);
}

cbm_scope_status_t cbm_scope_names_at(const char *source, int source_len, bool cpp_mode, int line,
                                      cbm_scope_result_t *out) {
    memset(out, 0, sizeof(*out));
    if (!source || source_len < 0) {
        return CBM_SCOPE_PARSE_FAILED;
    }
    uint32_t len = (uint32_t)source_len;
    uint32_t off = 0;
    if (!scope_line_offset(source, len, line, &off)) {
        return CBM_SCOPE_LINE_OUT_OF_RANGE;
    }
    TSParser *parser = ts_parser_new();
    if (!parser ||
        !ts_parser_set_language(parser, cpp_mode ? tree_sitter_cpp() : tree_sitter_c())) {
        ts_parser_delete(parser);
        return CBM_SCOPE_PARSE_FAILED;
    }
    (void)cbm_parser_skip_unreal_block_macros(parser, source, source_len);
    TSTree *tree = ts_parser_parse_string(parser, NULL, source, len);
    ts_parser_delete(parser);
    if (!tree) {
        return CBM_SCOPE_PARSE_FAILED;
    }

    scope_ctx_t ctx = {.src = source, .len = len};
    cbm_scope_status_t status = CBM_SCOPE_NOT_IN_FUNCTION;
    TSNode node = ts_node_descendant_for_byte_range(ts_tree_root_node(tree), off, off);
    for (; !ts_node_is_null(node) && !ctx.oom; node = ts_node_parent(node)) {
        const char *k = ts_node_type(node);
        if (strcmp(k, "compound_statement") == 0) {
            scope_take_block(&ctx, node, off);
            ctx.depth++;
        } else if (strcmp(k, "for_statement") == 0) {
            TSNode init = scope_field(node, "initializer");
            if (scope_is(init, "declaration") && off >= ts_node_end_byte(init)) {
                scope_take_declarators(&ctx, init, "loop_variable");
                ctx.depth++;
            }
        } else if (strcmp(k, "for_range_loop") == 0) {
            if (scope_inside(scope_field(node, "body"), off)) {
                scope_take_declarators(&ctx, node, "loop_variable");
                ctx.depth++;
            }
        } else if (strcmp(k, "if_statement") == 0 || strcmp(k, "while_statement") == 0 ||
                   strcmp(k, "switch_statement") == 0) {
            TSNode cond = scope_field(node, "condition");
            if (!ts_node_is_null(cond) && off >= ts_node_end_byte(cond)) {
                int before = ctx.count;
                scope_take_condition(&ctx, cond);
                ctx.depth += ctx.count > before ? 1 : 0;
            }
        } else if (strcmp(k, "catch_clause") == 0) {
            if (scope_inside(scope_field(node, "body"), off)) {
                scope_take_parameters(&ctx, scope_field(node, "parameters"), "catch_parameter");
                ctx.depth++;
            }
        } else if (strcmp(k, "lambda_expression") == 0) {
            if (scope_inside(scope_field(node, "body"), off)) {
                scope_take_lambda(&ctx, node);
                ctx.depth++;
            }
        } else if (strcmp(k, "function_definition") == 0) {
            if (!scope_inside(scope_field(node, "body"), off)) {
                break; /* in the signature, not the body */
            }
            TSNode d = scope_function_declarator(node);
            if (!ts_node_is_null(d)) {
                scope_take_parameters(&ctx, scope_field(d, "parameters"), "parameter");
            }
            scope_set_function(&ctx, node, out);
            status = CBM_SCOPE_OK;
            break;
        }
    }

    if (status == CBM_SCOPE_OK && !ctx.oom) {
        scope_mark_shadowing(&ctx);
        scope_resolve_captures(&ctx);
        out->lambdas = ctx.lambda_count ? (cbm_scope_lambda_t *)calloc((size_t)ctx.lambda_count,
                                                                       sizeof(*out->lambdas))
                                        : NULL;
        if (ctx.lambda_count && !out->lambdas) {
            ctx.oom = true;
        }
        for (int i = 0; out->lambdas && i < ctx.lambda_count; i++) {
            out->lambdas[i].line = ctx.lambdas[i].line;
            out->lambdas[i].captures =
                ts_node_is_null(ctx.lambdas[i].spec)
                    ? strdup("")
                    : scope_node_text(&ctx, ctx.lambdas[i].spec, SCOPE_CAPTURES_MAX);
        }
        out->lambda_count = out->lambdas ? ctx.lambda_count : 0;
    }
    free(ctx.lambdas);
    ts_tree_delete(tree);

    out->names = ctx.names;
    out->count = ctx.count;
    if (ctx.oom || (status == CBM_SCOPE_OK && !out->function)) {
        cbm_scope_result_free(out);
        return CBM_SCOPE_PARSE_FAILED;
    }
    if (status != CBM_SCOPE_OK) {
        cbm_scope_result_free(out);
    }
    return status;
}

void cbm_scope_result_free(cbm_scope_result_t *result) {
    if (!result) {
        return;
    }
    for (int i = 0; i < result->count; i++) {
        free(result->names[i].name);
        free(result->names[i].type);
        free(result->names[i].capture);
    }
    free(result->names);
    for (int i = 0; i < result->lambda_count; i++) {
        free(result->lambdas[i].captures);
    }
    free(result->lambdas);
    free(result->function);
    memset(result, 0, sizeof(*result));
}
