/* test_scope_names.c — cbm_scope_names_at: the names a C/C++ line can see.
 *
 * Each fixture is a whole file; line numbers in the comments are the ones the
 * assertions use. Fixtures that model Unreal tests sit inside
 * `#if WITH_DEV_AUTOMATION_TESTS`, as real ones do. */
#include "test_framework.h"
#include "scope_names.h"

#include <string.h>

static const cbm_scope_name_t *find_name(const cbm_scope_result_t *r, const char *name) {
    for (int i = 0; i < r->count; i++) {
        if (strcmp(r->names[i].name, name) == 0) {
            return &r->names[i];
        }
    }
    return NULL;
}

static cbm_scope_status_t scope_at(const char *src, int line, cbm_scope_result_t *out) {
    return cbm_scope_names_at(src, (int)strlen(src), true, line, out);
}

static const char *BASICS =
    "void Fill(const FString& Name, int32 Count = 2, TArray<int32> Cells = {})\n" /* 1 */
    "{\n"                                                                         /* 2 */
    "\tint a = 1, *b = nullptr;\n"                                                /* 3 */
    "\tauto [x, y] = Pair;\n"                                                     /* 4 */
    "\tfor (const FString& S : Names)\n"                                          /* 5 */
    "\t{\n"                                                                       /* 6 */
    "\t\tUse(S);\n"                                                               /* 7 */
    "\t}\n"                                                                       /* 8 */
    "\tint Later = 0;\n"                                                          /* 9 */
    "}\n";                                                                        /* 10 */

TEST(scope_lists_parameters_locals_and_loop_variable_innermost_first) {
    cbm_scope_result_t r;
    ASSERT_EQ(scope_at(BASICS, 7, &r), CBM_SCOPE_OK);
    ASSERT_STR_EQ(r.function, "Fill");
    ASSERT_EQ(r.function_line, 1);
    /* the `= {}` default is not a parse error (same rule as coverage) */
    ASSERT_FALSE(r.parse_partial);
    ASSERT_EQ(r.count, 8);
    ASSERT_STR_EQ(r.names[0].name, "S"); /* innermost first */
    ASSERT_STR_EQ(r.names[0].kind, "loop_variable");
    ASSERT_STR_EQ(r.names[0].type, "const FString&");
    const cbm_scope_name_t *b = find_name(&r, "b");
    ASSERT_NOT_NULL(b);
    ASSERT_STR_EQ(b->type, "int*");
    ASSERT_STR_EQ(find_name(&r, "a")->type, "int");
    ASSERT_STR_EQ(find_name(&r, "x")->type, "auto");
    ASSERT_NOT_NULL(find_name(&r, "y"));
    const cbm_scope_name_t *name = find_name(&r, "Name");
    ASSERT_NOT_NULL(name);
    ASSERT_STR_EQ(name->kind, "parameter");
    ASSERT_STR_EQ(name->type, "const FString&");
    ASSERT_EQ(name->line, 1);
    ASSERT_NOT_NULL(find_name(&r, "Cells"));
    /* declared below the line: not in scope */
    ASSERT_NULL(find_name(&r, "Later"));
    ASSERT_NULL(r.names[0].capture);
    cbm_scope_result_free(&r);
    PASS();
}

/* A name declared on the line itself is not in scope yet; one above it is. */
TEST(scope_a_line_does_not_see_its_own_declaration) {
    cbm_scope_result_t r;
    ASSERT_EQ(scope_at(BASICS, 3, &r), CBM_SCOPE_OK);
    ASSERT_NULL(find_name(&r, "a"));
    ASSERT_NOT_NULL(find_name(&r, "Count"));
    cbm_scope_result_free(&r);
    ASSERT_EQ(scope_at(BASICS, 4, &r), CBM_SCOPE_OK);
    ASSERT_NOT_NULL(find_name(&r, "a"));
    ASSERT_NULL(find_name(&r, "x"));
    cbm_scope_result_free(&r);
    PASS();
}

static const char *CONDITIONS = "int Run(int N)\n"                        /* 1 */
                                "{\n"                                     /* 2 */
                                "\tfor (int i = 0; i < N; ++i)\n"         /* 3 */
                                "\t{\n"                                   /* 4 */
                                "\t\tif (auto* P = Find(i); P)\n"         /* 5 */
                                "\t\t{\n"                                 /* 6 */
                                "\t\t\ttry { Go(P); }\n"                  /* 7 */
                                "\t\t\tcatch (const std::exception& E)\n" /* 8 */
                                "\t\t\t{\n"                               /* 9 */
                                "\t\t\t\tLog(E, P, i);\n"                 /* 10 */
                                "\t\t\t}\n"                               /* 11 */
                                "\t\t}\n"                                 /* 12 */
                                "\t}\n"                                   /* 13 */
                                "\treturn 0;\n"                           /* 14 */
                                "}\n";                                    /* 15 */

TEST(scope_lists_for_condition_and_catch_variables) {
    cbm_scope_result_t r;
    ASSERT_EQ(scope_at(CONDITIONS, 10, &r), CBM_SCOPE_OK);
    ASSERT_STR_EQ(find_name(&r, "E")->kind, "catch_parameter");
    ASSERT_STR_EQ(find_name(&r, "E")->type, "const std::exception&");
    ASSERT_STR_EQ(find_name(&r, "P")->kind, "condition_variable");
    ASSERT_STR_EQ(find_name(&r, "P")->type, "auto*");
    ASSERT_STR_EQ(find_name(&r, "i")->kind, "loop_variable");
    ASSERT_STR_EQ(find_name(&r, "N")->kind, "parameter");
    cbm_scope_result_free(&r);
    /* after the loop, none of them is in scope */
    ASSERT_EQ(scope_at(CONDITIONS, 14, &r), CBM_SCOPE_OK);
    ASSERT_EQ(r.count, 1);
    ASSERT_STR_EQ(r.names[0].name, "N");
    cbm_scope_result_free(&r);
    PASS();
}

static const char *LAMBDAS =
    "#if WITH_DEV_AUTOMATION_TESTS\n"                                     /* 1 */
    "bool FTest::RunTest(const FString& Parameters)\n"                    /* 2 */
    "{\n"                                                                 /* 3 */
    "\tint32 Total = 0;\n"                                                /* 4 */
    "\tint32 Hits = 0;\n"                                                 /* 5 */
    "\tFString Label;\n"                                                  /* 6 */
    "\tauto Outer = [&Hits, Total, Copy = Label](int32 Step)\n"           /* 7 */
    "\t{\n"                                                               /* 8 */
    "\t\tint32 Total = Step;\n"                                           /* 9 */
    "\t\tauto Inner = [=]()\n"                                            /* 10 */
    "\t\t{\n"                                                             /* 11 */
    "\t\t\treturn Total + Hits + Step + Copy.Len() + Parameters.Len();\n" /* 12 */
    "\t\t};\n"                                                            /* 13 */
    "\t\treturn Inner();\n"                                               /* 14 */
    "\t};\n"                                                              /* 15 */
    "\treturn Outer(1) > 0;\n"                                            /* 16 */
    "}\n"                                                                 /* 17 */
    "#endif\n";                                                           /* 18 */

TEST(scope_reports_how_each_enclosing_lambda_captures_a_name) {
    cbm_scope_result_t r;
    ASSERT_EQ(scope_at(LAMBDAS, 12, &r), CBM_SCOPE_OK);
    ASSERT_STR_EQ(r.function, "FTest::RunTest");
    ASSERT_EQ(r.lambda_count, 2);
    ASSERT_EQ(r.lambdas[0].line, 10); /* innermost first */
    ASSERT_STR_EQ(r.lambdas[0].captures, "[=]");
    ASSERT_EQ(r.lambdas[1].line, 7);

    /* declared inside the outer lambda, reached through [=] */
    const cbm_scope_name_t *step = find_name(&r, "Step");
    ASSERT_STR_EQ(step->kind, "lambda_parameter");
    ASSERT_STR_EQ(step->capture, "default [=]");
    const cbm_scope_name_t *copy = find_name(&r, "Copy");
    ASSERT_STR_EQ(copy->kind, "init_capture");
    ASSERT_STR_EQ(copy->capture, "default [=]");
    /* the outer lambda captures Hits by reference, the inner one by default */
    ASSERT_STR_EQ(find_name(&r, "Hits")->capture, "default [=]");
    /* Parameters crosses the outer lambda, whose explicit list omits it */
    ASSERT_STR_EQ(find_name(&r, "Parameters")->capture, "NOT CAPTURED by lambda at line 7");
    ASSERT_STR_EQ(find_name(&r, "Label")->capture, "NOT CAPTURED by lambda at line 7");
    cbm_scope_result_free(&r);

    /* inside the outer lambda only: its own capture modes. `Copy = Label`
     * makes Copy visible, not Label. */
    ASSERT_EQ(scope_at(LAMBDAS, 14, &r), CBM_SCOPE_OK);
    ASSERT_STR_EQ(find_name(&r, "Hits")->capture, "by reference");
    ASSERT_STR_EQ(find_name(&r, "Parameters")->capture, "NOT CAPTURED by lambda at line 7");
    ASSERT_STR_EQ(find_name(&r, "Label")->capture, "NOT CAPTURED by lambda at line 7");
    ASSERT_NULL(find_name(&r, "Step")->capture); /* its own parameter */
    ASSERT_NULL(find_name(&r, "Copy")->capture); /* its own init-capture */
    cbm_scope_result_free(&r);
    PASS();
}

TEST(scope_marks_shadowing) {
    cbm_scope_result_t r;
    ASSERT_EQ(scope_at(LAMBDAS, 14, &r), CBM_SCOPE_OK);
    /* `int32 Total = Step;` on line 9 hides the test's Total on line 4 */
    const cbm_scope_name_t *inner = NULL;
    const cbm_scope_name_t *outer = NULL;
    for (int i = 0; i < r.count; i++) {
        if (strcmp(r.names[i].name, "Total") == 0) {
            if (!inner) {
                inner = &r.names[i];
            } else {
                outer = &r.names[i];
            }
        }
    }
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(outer);
    ASSERT_EQ(inner->line, 9);
    ASSERT_EQ(inner->shadows_line, 4);
    ASSERT_FALSE(inner->shadowed);
    ASSERT_TRUE(outer->shadowed);
    ASSERT_EQ(outer->shadows_line, 0);
    cbm_scope_result_free(&r);
    PASS();
}

/* The block of an Unreal ON_SCOPE_EXIT is an ordinary block here too. */
TEST(scope_sees_through_on_scope_exit_blocks) {
    static const char *src = "#if WITH_DEV_AUTOMATION_TESTS\n"         /* 1 */
                             "bool FTest::RunTest(const FString& P)\n" /* 2 */
                             "{\n"                                     /* 3 */
                             "\tUObject* Script = Spawn();\n"          /* 4 */
                             "\tON_SCOPE_EXIT\n"                       /* 5 */
                             "\t{\n"                                   /* 6 */
                             "\t\tDestroy(Script);\n"                  /* 7 */
                             "\t};\n"                                  /* 8 */
                             "\treturn true;\n"                        /* 9 */
                             "}\n"                                     /* 10 */
                             "#endif\n";                               /* 11 */
    cbm_scope_result_t r;
    ASSERT_EQ(scope_at(src, 7, &r), CBM_SCOPE_OK);
    ASSERT_FALSE(r.parse_partial);
    ASSERT_NOT_NULL(find_name(&r, "Script"));
    ASSERT_NOT_NULL(find_name(&r, "P"));
    cbm_scope_result_free(&r);
    PASS();
}

TEST(scope_rejects_lines_outside_function_bodies) {
    cbm_scope_result_t r;
    ASSERT_EQ(scope_at(BASICS, 1, &r), CBM_SCOPE_NOT_IN_FUNCTION); /* the signature */
    ASSERT_EQ(r.count, 0);
    ASSERT_EQ(scope_at(LAMBDAS, 1, &r), CBM_SCOPE_NOT_IN_FUNCTION); /* a directive */
    ASSERT_EQ(scope_at(BASICS, 11, &r), CBM_SCOPE_LINE_OUT_OF_RANGE);
    ASSERT_EQ(scope_at(BASICS, 0, &r), CBM_SCOPE_LINE_OUT_OF_RANGE);
    PASS();
}

SUITE(scope_names) {
    RUN_TEST(scope_lists_parameters_locals_and_loop_variable_innermost_first);
    RUN_TEST(scope_a_line_does_not_see_its_own_declaration);
    RUN_TEST(scope_lists_for_condition_and_catch_variables);
    RUN_TEST(scope_reports_how_each_enclosing_lambda_captures_a_name);
    RUN_TEST(scope_marks_shadowing);
    RUN_TEST(scope_sees_through_on_scope_exit_blocks);
    RUN_TEST(scope_rejects_lines_outside_function_bodies);
}
