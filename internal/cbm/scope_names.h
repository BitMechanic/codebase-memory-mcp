/* scope_names.h — the names a C/C++ statement can see.
 *
 * cbm_scope_names_at() parses one C/C++ source buffer and reports every name
 * declared in scope at the start of a given line: the enclosing function's
 * parameters, block locals declared above the line, loop, condition and catch
 * variables, lambda parameters and init-captures. Names are listed innermost
 * scope first. For each name it also says
 *   - whether it hides an outer declaration of the same name (shadowing), and
 *   - when one or more lambdas lie between the declaration and the line,
 *     whether every one of them captures it, so a missing capture is visible
 *     before a compile.
 * Members reached through `this`, namespace-scope names and globals are not
 * listed. The source is parsed as given; nothing reads the graph. */
#ifndef CBM_SCOPE_NAMES_H
#define CBM_SCOPE_NAMES_H

#include <stdbool.h>

typedef struct {
    char *name;
    const char *kind; /* parameter, local, loop_variable, condition_variable,
                       * catch_parameter, lambda_parameter, init_capture */
    char *type;       /* as written, whitespace collapsed; "" when deduced */
    int line;         /* 1-based line of the declaration */
    int depth;        /* 0 = innermost scope around the line */
    int shadows_line; /* > 0: this name hides an outer declaration at that line */
    bool shadowed;    /* an inner declaration hides this one at the line */
    char *capture;    /* NULL: no lambda in between. Otherwise one of
                       * "by value", "by reference", "default [=]",
                       * "default [&]", "NOT CAPTURED by lambda at line N" */
} cbm_scope_name_t;

typedef struct {
    int line;       /* line of the lambda's capture list */
    char *captures; /* the capture list as written, e.g. "[&, Count]" */
} cbm_scope_lambda_t;

typedef struct {
    char *function; /* enclosing function's declarator name, e.g. "FTest::RunTest" */
    int function_line;
    bool parse_partial;          /* the enclosing function holds parse errors */
    cbm_scope_lambda_t *lambdas; /* innermost first */
    int lambda_count;
    cbm_scope_name_t *names; /* innermost scope first, source order within a scope */
    int count;
} cbm_scope_result_t;

typedef enum {
    CBM_SCOPE_OK = 0,
    CBM_SCOPE_LINE_OUT_OF_RANGE, /* line < 1 or past the end of the file */
    CBM_SCOPE_NOT_IN_FUNCTION,   /* the line is outside every function body */
    CBM_SCOPE_PARSE_FAILED,      /* no tree, or no memory */
} cbm_scope_status_t;

/* cpp_mode: parse with the C++ grammar (true) or the C grammar (false). */
cbm_scope_status_t cbm_scope_names_at(const char *source, int source_len, bool cpp_mode, int line,
                                      cbm_scope_result_t *out);

void cbm_scope_result_free(cbm_scope_result_t *result);

#endif /* CBM_SCOPE_NAMES_H */
