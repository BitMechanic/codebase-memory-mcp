/*
 * test_userconfig.c — Tests for user-defined extension→language mappings.
 *
 * Tests cbm_userconfig_load(), cbm_userconfig_lookup(), and the
 * cbm_set_user_lang_config() / cbm_language_for_extension() integration.
 */
#include "../src/foundation/compat.h"
#include "../src/foundation/compat_fs.h"
#include "../src/foundation/platform.h"
#include "test_framework.h"
#include "discover/discover.h"
#include "discover/userconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Helpers ─────────────────────────────────────────────────────── */

/* Write a JSON file to path. Returns 0 on success. */
static int write_json(const char *path, const char *json) {
    FILE *f = fopen(path, "w");
    if (!f) {
        return -1;
    }
    fputs(json, f);
    fclose(f);
    return 0;
}

/* ── Tests: project config ───────────────────────────────────────── */

TEST(userconfig_project_basic) {
    /* Write a .codebase-memory.json in a temp dir */
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/uctest_proj_basic", cbm_tmpdir());
    cbm_mkdir_p(dir, 0755); /* from compat_fs.h via compat.h */

    char proj[512];
    snprintf(proj, sizeof(proj), "%s/.codebase-memory.json", dir);
    ASSERT_EQ(
        write_json(proj, "{\"extra_extensions\":{\".blade.php\":\"php\",\".mjs\":\"javascript\"}}"),
        0);

    cbm_userconfig_t *cfg = cbm_userconfig_load(dir);
    ASSERT_NOT_NULL(cfg);

    ASSERT_EQ(cbm_userconfig_lookup(cfg, ".blade.php"), CBM_LANG_PHP);
    ASSERT_EQ(cbm_userconfig_lookup(cfg, ".mjs"), CBM_LANG_JAVASCRIPT);
    ASSERT_EQ(cbm_userconfig_lookup(cfg, ".go"), CBM_LANG_COUNT); /* not in user config */

    cbm_userconfig_free(cfg);
    remove(proj);
    PASS();
}

/* ── Tests: global config ────────────────────────────────────────── */

TEST(userconfig_global_via_env) {
    /* Point config dir to a temp dir via the platform-appropriate env var:
     * XDG_CONFIG_HOME on Linux/macOS, APPDATA on Windows. */
    char cfg_dir[256];
    snprintf(cfg_dir, sizeof(cfg_dir), "%s/uctest_global_xdg", cbm_tmpdir());

    char app_dir[512];
    snprintf(app_dir, sizeof(app_dir), "%s/codebase-memory-mcp", cfg_dir);
    cbm_mkdir_p(app_dir, 0755);

    char global_path[768];
    snprintf(global_path, sizeof(global_path), "%s/config.json", app_dir);
    ASSERT_EQ(
        write_json(global_path, "{\"extra_extensions\":{\".twig\":\"html\"}}"),
        0);

#ifdef _WIN32
    char old_appdata[512] = "";
    cbm_safe_getenv("APPDATA", old_appdata, sizeof(old_appdata), NULL);
    cbm_setenv("APPDATA", cfg_dir, 1);
#else
    cbm_setenv("XDG_CONFIG_HOME", cfg_dir, 1);
#endif
    cbm_userconfig_t *cfg = cbm_userconfig_load(NULL); /* no project dir */
#ifdef _WIN32
    if (old_appdata[0]) {
        cbm_setenv("APPDATA", old_appdata, 1);
    } else {
        cbm_unsetenv("APPDATA");
    }
#else
    cbm_unsetenv("XDG_CONFIG_HOME");
#endif

    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(cbm_userconfig_lookup(cfg, ".twig"), CBM_LANG_HTML);

    cbm_userconfig_free(cfg);
    remove(global_path);
    PASS();
}

/* ── Tests: project wins over global ────────────────────────────── */

TEST(userconfig_project_wins_over_global) {
    /* Global says .xyz → python; project says .xyz → rust */
    char xdg_dir[256];
    snprintf(xdg_dir, sizeof(xdg_dir), "%s/uctest_priority_xdg", cbm_tmpdir());

    char app_dir[512];
    snprintf(app_dir, sizeof(app_dir), "%s/codebase-memory-mcp", xdg_dir);
    cbm_mkdir_p(app_dir, 0755);

    char global_path[768];
    snprintf(global_path, sizeof(global_path), "%s/config.json", app_dir);
    ASSERT_EQ(
        write_json(global_path, "{\"extra_extensions\":{\".xyz\":\"python\"}}"),
        0);

    char proj_dir[256];
    snprintf(proj_dir, sizeof(proj_dir), "%s/uctest_priority_proj", cbm_tmpdir());
    cbm_mkdir_p(proj_dir, 0755);

    char proj_path[512];
    snprintf(proj_path, sizeof(proj_path), "%s/.codebase-memory.json", proj_dir);
    ASSERT_EQ(
        write_json(proj_path, "{\"extra_extensions\":{\".xyz\":\"rust\"}}"),
        0);

    cbm_setenv("XDG_CONFIG_HOME", xdg_dir, 1);
    cbm_userconfig_t *cfg = cbm_userconfig_load(proj_dir);
    cbm_unsetenv("XDG_CONFIG_HOME");

    ASSERT_NOT_NULL(cfg);
    /* Project definition (rust) must win */
    ASSERT_EQ(cbm_userconfig_lookup(cfg, ".xyz"), CBM_LANG_RUST);

    cbm_userconfig_free(cfg);
    remove(global_path);
    remove(proj_path);
    PASS();
}

/* ── Tests: unknown language values are skipped ──────────────────── */

TEST(userconfig_unknown_lang_skipped) {
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/uctest_unknown_lang", cbm_tmpdir());
    cbm_mkdir_p(dir, 0755);

    char proj[512];
    snprintf(proj, sizeof(proj), "%s/.codebase-memory.json", dir);
    /* "klingon" is not a valid language; ".wasm" should be silently skipped */
    ASSERT_EQ(
        write_json(proj,
                   "{\"extra_extensions\":{\".wasm\":\"klingon\",\".mjs\":\"javascript\"}}"),
        0);

    cbm_userconfig_t *cfg = cbm_userconfig_load(dir);
    ASSERT_NOT_NULL(cfg);

    /* .wasm with unknown lang → not in config */
    ASSERT_EQ(cbm_userconfig_lookup(cfg, ".wasm"), CBM_LANG_COUNT);
    /* .mjs with valid lang → present */
    ASSERT_EQ(cbm_userconfig_lookup(cfg, ".mjs"), CBM_LANG_JAVASCRIPT);

    cbm_userconfig_free(cfg);
    remove(proj);
    PASS();
}

/* ── Tests: missing files are silently ignored ───────────────────── */

TEST(userconfig_missing_files_ok) {
    /* Point to a non-existent repo dir */
    cbm_userconfig_t *cfg = cbm_userconfig_load("/tmp/__nonexistent_repo_12345__");
    ASSERT_NOT_NULL(cfg); /* must not return NULL — just empty */
    ASSERT_EQ(cfg->count, 0);
    cbm_userconfig_free(cfg);
    PASS();
}

/* ── Tests: integration with cbm_language_for_extension ─────────── */

TEST(userconfig_integration_override) {
    /* Verify that setting the global config makes cbm_language_for_extension
     * respect the override. We map ".blade.php" → PHP, which is not in the
     * built-in table. */
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/uctest_integ", cbm_tmpdir());
    cbm_mkdir_p(dir, 0755);

    char proj[512];
    snprintf(proj, sizeof(proj), "%s/.codebase-memory.json", dir);
    ASSERT_EQ(
        write_json(proj, "{\"extra_extensions\":{\".blade.php\":\"php\"}}"),
        0);

    cbm_userconfig_t *cfg = cbm_userconfig_load(dir);
    ASSERT_NOT_NULL(cfg);

    /* Before setting, .blade.php is unknown */
    ASSERT_EQ(cbm_language_for_extension(".blade.php"), CBM_LANG_COUNT);

    cbm_set_user_lang_config(cfg);
    /* After setting, .blade.php → PHP */
    ASSERT_EQ(cbm_language_for_extension(".blade.php"), CBM_LANG_PHP);
    /* Built-in extensions still work */
    ASSERT_EQ(cbm_language_for_extension(".go"), CBM_LANG_GO);

    /* Clean up global state */
    cbm_set_user_lang_config(NULL);
    cbm_userconfig_free(cfg);
    remove(proj);
    PASS();
}

/* ── Tests: free is NULL-safe ────────────────────────────────────── */

TEST(userconfig_free_null) {
    cbm_userconfig_free(NULL); /* must not crash */
    PASS();
}

/* ── Suite ──────────────────────────────────────────────────────── */

/* ── Tests: path_properties ─────────────────────────────────────── */

/* Value a pattern yields for a path, as a string ("" when it does not match). */
static const char *path_prop(const char *pattern, const char *rel_path, bool path_is_dir) {
    static char buf[128];
    const char *value = NULL;
    size_t len = cbm_path_prop_value(pattern, rel_path, path_is_dir, &value);
    if (len >= sizeof(buf)) {
        len = sizeof(buf) - 1;
    }
    if (len > 0) {
        memcpy(buf, value, len);
    }
    buf[len] = '\0';
    return buf;
}

TEST(userconfig_path_prop_value_matching) {
    /* Both halves of a unit split across Public/ and Private/ get one value. */
    ASSERT_STR_EQ(path_prop("Tools/*", "Mod/Public/Tools/Alpha/Thing.h", false), "Alpha");
    ASSERT_STR_EQ(path_prop("Tools/*", "Mod/Private/Tools/Alpha/Thing.cpp", false), "Alpha");
    /* Nested folders keep the value of the matched one. */
    ASSERT_STR_EQ(path_prop("Tools/*", "Mod/Private/Tools/Alpha/Sub/Deep.cpp", false), "Alpha");
    /* The file name never takes part: a file directly in Tools/ has no value. */
    ASSERT_STR_EQ(path_prop("Tools/*", "Mod/Private/Tools/Loose.cpp", false), "");
    ASSERT_STR_EQ(path_prop("Tools/*", "Mod/Private/Other/Plain.cpp", false), "");
    /* Leftmost match wins when the marker folder repeats. */
    ASSERT_STR_EQ(path_prop("Tools/*", "B/Tools/Gamma/Tools/Delta/Deep.cpp", false), "Gamma");
    /* A lone "*" is the first folder; a file at the root has none. */
    ASSERT_STR_EQ(path_prop("*", "Mod/Private/Other/Plain.cpp", false), "Mod");
    ASSERT_STR_EQ(path_prop("*", "Plain.cpp", false), "");
    /* The "*" need not be last, and names are compared exactly. */
    ASSERT_STR_EQ(path_prop("*/Private", "Mod/Private/Other/Plain.cpp", false), "Mod");
    ASSERT_STR_EQ(path_prop("tools/*", "Mod/Private/Tools/Alpha/Thing.cpp", false), "");
    ASSERT_STR_EQ(path_prop("Tool/*", "Mod/Private/Tools/Alpha/Thing.cpp", false), "");
    /* A Folder node's path is a directory all the way to its last name. */
    ASSERT_STR_EQ(path_prop("Tools/*", "Mod/Private/Tools/Alpha", true), "Alpha");
    ASSERT_STR_EQ(path_prop("Tools/*", "Mod/Private/Tools", true), "");
    PASS();
}

TEST(userconfig_path_properties_parsed_and_validated) {
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/uctest_path_props", cbm_tmpdir());
    cbm_mkdir_p(dir, 0755);

    char proj[512];
    snprintf(proj, sizeof(proj), "%s/.codebase-memory.json", dir);
    /* Two valid rules (one written with a backslash), then one of each kind of
     * invalid rule: a name that is not an identifier, no "*", a partial
     * wildcard, two "*", a trailing '/', a non-string value. */
    ASSERT_EQ(write_json(proj, "{\"extra_extensions\":{\".mjs\":\"javascript\"},"
                               "\"path_properties\":{"
                               "\"toolset\":\"Tools/*\","
                               "\"area\":\"Private\\\\*\","
                               "\"bad-name\":\"Tools/*\","
                               "\"nostar\":\"Tools/Alpha\","
                               "\"partial\":\"Tools/A*\","
                               "\"twostars\":\"*/*\","
                               "\"trailing\":\"Tools/*/\","
                               "\"number\":5}}"),
              0);

    cbm_userconfig_t *cfg = cbm_userconfig_load(dir);
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(cfg->path_prop_count, 2);
    ASSERT_STR_EQ(cfg->path_props[0].property, "toolset");
    ASSERT_STR_EQ(cfg->path_props[0].pattern, "Tools/*");
    ASSERT_STR_EQ(cfg->path_props[1].property, "area");
    ASSERT_STR_EQ(cfg->path_props[1].pattern, "Private/*"); /* backslash normalised */
    /* The extension mapping in the same file is unaffected. */
    ASSERT_EQ(cbm_userconfig_lookup(cfg, ".mjs"), CBM_LANG_JAVASCRIPT);

    cbm_userconfig_free(cfg);
    remove(proj);
    PASS();
}

TEST(userconfig_path_properties_project_wins_over_global) {
    char cfg_dir[256];
    snprintf(cfg_dir, sizeof(cfg_dir), "%s/uctest_path_props_global", cbm_tmpdir());
    char app_dir[512];
    snprintf(app_dir, sizeof(app_dir), "%s/codebase-memory-mcp", cfg_dir);
    cbm_mkdir_p(app_dir, 0755);
    char global_path[768];
    snprintf(global_path, sizeof(global_path), "%s/config.json", app_dir);
    ASSERT_EQ(
        write_json(global_path, "{\"path_properties\":{\"toolset\":\"Tools/*\",\"module\":\"*\"}}"),
        0);

    char proj_dir[256];
    snprintf(proj_dir, sizeof(proj_dir), "%s/uctest_path_props_proj", cbm_tmpdir());
    cbm_mkdir_p(proj_dir, 0755);
    char proj_path[512];
    snprintf(proj_path, sizeof(proj_path), "%s/.codebase-memory.json", proj_dir);
    ASSERT_EQ(write_json(proj_path, "{\"path_properties\":{\"toolset\":\"Private/*\"}}"), 0);

    const char *config_env =
#ifdef _WIN32
        "APPDATA";
#else
        "XDG_CONFIG_HOME";
#endif
    char old_value[512] = "";
    cbm_safe_getenv(config_env, old_value, sizeof(old_value), NULL);
    cbm_setenv(config_env, cfg_dir, 1);
    cbm_userconfig_t *cfg = cbm_userconfig_load(proj_dir);
    if (old_value[0]) {
        cbm_setenv(config_env, old_value, 1);
    } else {
        cbm_unsetenv(config_env);
    }

    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(cfg->path_prop_count, 2);
    const char *toolset = NULL;
    const char *module = NULL;
    for (int i = 0; i < cfg->path_prop_count; i++) {
        if (strcmp(cfg->path_props[i].property, "toolset") == 0) {
            toolset = cfg->path_props[i].pattern;
        } else if (strcmp(cfg->path_props[i].property, "module") == 0) {
            module = cfg->path_props[i].pattern;
        }
    }
    ASSERT_NOT_NULL(toolset);
    ASSERT_NOT_NULL(module);
    ASSERT_STR_EQ(toolset, "Private/*"); /* the project's rule replaces the global one */
    ASSERT_STR_EQ(module, "*");          /* a global-only rule still applies */

    cbm_userconfig_free(cfg);
    remove(global_path);
    remove(proj_path);
    PASS();
}

SUITE(userconfig) {
    RUN_TEST(userconfig_project_basic);
    RUN_TEST(userconfig_global_via_env);
    RUN_TEST(userconfig_project_wins_over_global);
    RUN_TEST(userconfig_unknown_lang_skipped);
    RUN_TEST(userconfig_missing_files_ok);
    RUN_TEST(userconfig_integration_override);
    RUN_TEST(userconfig_free_null);
    RUN_TEST(userconfig_path_prop_value_matching);
    RUN_TEST(userconfig_path_properties_parsed_and_validated);
    RUN_TEST(userconfig_path_properties_project_wins_over_global);
}
