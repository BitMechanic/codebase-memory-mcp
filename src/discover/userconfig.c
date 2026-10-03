/*
 * userconfig.c — User-defined extension→language mappings.
 *
 * Reads extra_extensions from:
 *   Global:  $XDG_CONFIG_HOME/codebase-memory-mcp/config.json
 *            (falls back to ~/.config/codebase-memory-mcp/config.json)
 *   Project: {repo_root}/.codebase-memory.json
 *
 * Project config wins over global. Unknown language values warn and are
 * skipped (fail-open). Missing files are silently ignored.
 *
 * Also reads path_properties from the same two files (see userconfig.h).
 */
#include "discover/userconfig.h"
#include "cbm.h" /* CBMLanguage, CBM_LANG_* */
#include "foundation/constants.h"
#include "foundation/platform.h" /* cbm_safe_getenv */
#include "foundation/compat_fs.h"
#include "foundation/sha256.h"

enum { MAX_CONFIG_SIZE = 65536 };
#include "foundation/log.h"

#include <yyjson/yyjson.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Process-global user config pointer ──────────────────────────── */

static const cbm_userconfig_t *g_userconfig = NULL;

static void userconfig_source_digest(const char *state, const void *bytes, size_t len,
                                     char out[CBM_SHA256_HEX_LEN + 1]) {
    static const char domain[] = "cbm-userconfig-source-v1";
    cbm_sha256_ctx sha;
    cbm_sha256_init(&sha);
    cbm_sha256_update(&sha, domain, sizeof(domain));
    cbm_sha256_update(&sha, state, strlen(state) + 1);
    if (bytes && len > 0) {
        cbm_sha256_update(&sha, bytes, len);
    }
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    cbm_sha256_final(&sha, digest);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < CBM_SHA256_DIGEST_LEN; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[CBM_SHA256_HEX_LEN] = '\0';
}

void cbm_set_user_lang_config(const cbm_userconfig_t *cfg) {
    g_userconfig = cfg;
}

const cbm_userconfig_t *cbm_get_user_lang_config(void) {
    return g_userconfig;
}

/* ── Language name → enum table ──────────────────────────────────── */

/*
 * Reverse-mapping from lowercase language name strings to CBMLanguage.
 * Covers all names exposed by cbm_language_name() plus common aliases.
 */
typedef struct {
    const char *name; /* lowercase */
    CBMLanguage lang;
} lang_name_entry_t;

static const lang_name_entry_t LANG_NAME_TABLE[] = {
    {"go", CBM_LANG_GO},
    {"python", CBM_LANG_PYTHON},
    {"javascript", CBM_LANG_JAVASCRIPT},
    {"typescript", CBM_LANG_TYPESCRIPT},
    {"tsx", CBM_LANG_TSX},
    {"arkts", CBM_LANG_ARKTS},
    {"rust", CBM_LANG_RUST},
    {"java", CBM_LANG_JAVA},
    {"c++", CBM_LANG_CPP},
    {"cpp", CBM_LANG_CPP},
    {"c#", CBM_LANG_CSHARP},
    {"csharp", CBM_LANG_CSHARP},
    {"php", CBM_LANG_PHP},
    {"lua", CBM_LANG_LUA},
    {"scala", CBM_LANG_SCALA},
    {"kotlin", CBM_LANG_KOTLIN},
    {"ruby", CBM_LANG_RUBY},
    {"c", CBM_LANG_C},
    {"bash", CBM_LANG_BASH},
    {"sh", CBM_LANG_BASH},
    {"zig", CBM_LANG_ZIG},
    {"elixir", CBM_LANG_ELIXIR},
    {"haskell", CBM_LANG_HASKELL},
    {"ocaml", CBM_LANG_OCAML},
    {"objective-c", CBM_LANG_OBJC},
    {"objc", CBM_LANG_OBJC},
    {"swift", CBM_LANG_SWIFT},
    {"dart", CBM_LANG_DART},
    {"perl", CBM_LANG_PERL},
    {"groovy", CBM_LANG_GROOVY},
    {"erlang", CBM_LANG_ERLANG},
    {"r", CBM_LANG_R},
    {"html", CBM_LANG_HTML},
    {"css", CBM_LANG_CSS},
    {"scss", CBM_LANG_SCSS},
    {"yaml", CBM_LANG_YAML},
    {"toml", CBM_LANG_TOML},
    {"hcl", CBM_LANG_HCL},
    {"terraform", CBM_LANG_HCL},
    {"sql", CBM_LANG_SQL},
    {"dockerfile", CBM_LANG_DOCKERFILE},
    {"clojure", CBM_LANG_CLOJURE},
    {"f#", CBM_LANG_FSHARP},
    {"fsharp", CBM_LANG_FSHARP},
    {"julia", CBM_LANG_JULIA},
    {"vimscript", CBM_LANG_VIMSCRIPT},
    {"nix", CBM_LANG_NIX},
    {"common lisp", CBM_LANG_COMMONLISP},
    {"commonlisp", CBM_LANG_COMMONLISP},
    {"lisp", CBM_LANG_COMMONLISP},
    {"elm", CBM_LANG_ELM},
    {"fortran", CBM_LANG_FORTRAN},
    {"cuda", CBM_LANG_CUDA},
    {"cobol", CBM_LANG_COBOL},
    {"verilog", CBM_LANG_VERILOG},
    {"emacs lisp", CBM_LANG_EMACSLISP},
    {"emacslisp", CBM_LANG_EMACSLISP},
    {"json", CBM_LANG_JSON},
    {"xml", CBM_LANG_XML},
    {"markdown", CBM_LANG_MARKDOWN},
    {"makefile", CBM_LANG_MAKEFILE},
    {"cmake", CBM_LANG_CMAKE},
    {"protobuf", CBM_LANG_PROTOBUF},
    {"graphql", CBM_LANG_GRAPHQL},
    {"vue", CBM_LANG_VUE},
    {"svelte", CBM_LANG_SVELTE},
    {"meson", CBM_LANG_MESON},
    {"glsl", CBM_LANG_GLSL},
    {"ini", CBM_LANG_INI},
    {"matlab", CBM_LANG_MATLAB},
    {"mojo", CBM_LANG_MOJO},
    {"plsql", CBM_LANG_PLSQL},
    {"chialisp", CBM_LANG_CHIALISP},
    {"lean", CBM_LANG_LEAN},
    {"form", CBM_LANG_FORM},
    {"magma", CBM_LANG_MAGMA},
    {"wolfram", CBM_LANG_WOLFRAM},
};

#define LANG_NAME_TABLE_SIZE (sizeof(LANG_NAME_TABLE) / sizeof(LANG_NAME_TABLE[0]))

/*
 * Parse a language string (case-insensitive) to a CBMLanguage enum.
 * Returns CBM_LANG_COUNT if the string is not recognized.
 */
static CBMLanguage lang_from_string(const char *s) {
    if (!s || !s[0]) {
        return CBM_LANG_COUNT;
    }

    /* Build a lowercase copy for comparison */
    char lower[CBM_SZ_64];
    size_t i;
    for (i = 0; i < sizeof(lower) - SKIP_ONE && s[i]; i++) {
        lower[i] = (char)tolower((unsigned char)s[i]);
    }
    lower[i] = '\0';

    for (size_t j = 0; j < LANG_NAME_TABLE_SIZE; j++) {
        if (strcmp(LANG_NAME_TABLE[j].name, lower) == 0) {
            return LANG_NAME_TABLE[j].lang;
        }
    }
    return CBM_LANG_COUNT;
}

/* ── Config directory helper ─────────────────────────────────────── */

/* cbm_app_config_dir() is now in platform.c (cross-platform). */

/* ── JSON parsing ────────────────────────────────────────────────── */

/*
 * Parse extra_extensions from a yyjson object root.
 * Appends valid entries to *entries / *count (growing via realloc).
 * Project-level entries (from_project=true) are appended after global
 * entries so that a later dedup pass can prefer project values.
 *
 * Returns 0 on success, -1 on alloc failure.
 */
static int parse_extra_extensions(yyjson_val *root, cbm_userext_t **entries, int *count,
                                  const char *source_label) {
    if (!yyjson_is_obj(root)) {
        cbm_log_warn("userconfig.bad_root", "file", source_label);
        return 0;
    }

    yyjson_val *extra = yyjson_obj_get(root, "extra_extensions");
    if (!extra) {
        return 0; /* key absent — fine */
    }
    if (!yyjson_is_obj(extra)) {
        cbm_log_warn("userconfig.bad_extra_extensions", "file", source_label);
        return 0;
    }

    yyjson_obj_iter iter;
    yyjson_obj_iter_init(extra, &iter);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
        yyjson_val *val = yyjson_obj_iter_get_val(key);

        const char *ext_str = yyjson_get_str(key);
        const char *lang_str = yyjson_get_str(val);

        if (!ext_str || !lang_str) {
            cbm_log_warn("userconfig.skip_non_string", "file", source_label);
            continue;
        }

        /* Extension must start with '.' */
        if (ext_str[0] != '.') {
            cbm_log_warn("userconfig.skip_bad_ext", "file", source_label, "ext", ext_str);
            continue;
        }

        CBMLanguage lang = lang_from_string(lang_str);
        if (lang == CBM_LANG_COUNT) {
            cbm_log_warn("userconfig.unknown_lang", "file", source_label, "lang", lang_str);
            continue; /* fail-open: skip unknown languages */
        }

        /* Grow the array */
        cbm_userext_t *tmp = realloc(*entries, (size_t)(*count + SKIP_ONE) * sizeof(cbm_userext_t));
        if (!tmp) {
            return CBM_NOT_FOUND;
        }
        *entries = tmp;

        char *ext_copy = strdup(ext_str);
        if (!ext_copy) {
            return CBM_NOT_FOUND;
        }

        (*entries)[*count].ext = ext_copy;
        (*entries)[*count].lang = lang;
        (*count)++;
    }
    return 0;
}

/* A property name must work as n.<name> in a query. */
static bool path_prop_name_ok(const char *s) {
    if (!isalpha((unsigned char)s[0]) && s[0] != '_') {
        return false;
    }
    for (const char *c = s; *c; c++) {
        if (!isalnum((unsigned char)*c) && *c != '_') {
            return false;
        }
    }
    return true;
}

/* Directory names separated by '/', exactly one of them "*". */
static bool path_prop_pattern_ok(const char *s) {
    int stars = 0;
    while (*s) {
        size_t len = strcspn(s, "/");
        if (len == 0) {
            return false; /* leading or doubled '/' */
        }
        if (memchr(s, '*', len)) {
            if (len != SKIP_ONE) {
                return false; /* "*" stands for a whole name only */
            }
            stars++;
        }
        s += len;
        if (*s == '/' && !*++s) {
            return false; /* trailing '/' */
        }
    }
    return stars == SKIP_ONE;
}

static void free_path_props(cbm_userconfig_t *cfg) {
    for (int i = 0; i < cfg->path_prop_count; i++) {
        free(cfg->path_props[i].property);
        free(cfg->path_props[i].pattern);
    }
    free(cfg->path_props);
    cfg->path_props = NULL;
    cfg->path_prop_count = 0;
}

/*
 * Parse path_properties from a yyjson object root into cfg. A property that is
 * already present gets the new pattern, so the project file (read second) wins
 * over the global one. Invalid entries warn and are skipped (fail-open).
 *
 * Returns 0 on success, -1 on alloc failure.
 */
static int parse_path_properties(yyjson_val *root, cbm_userconfig_t *cfg,
                                 const char *source_label) {
    yyjson_val *props = yyjson_obj_get(root, "path_properties");
    if (!props) {
        return 0; /* key absent — fine */
    }
    if (!yyjson_is_obj(props)) {
        cbm_log_warn("userconfig.bad_path_properties", "file", source_label);
        return 0;
    }

    yyjson_obj_iter iter;
    yyjson_obj_iter_init(props, &iter);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
        const char *name = yyjson_get_str(key);
        const char *pattern_str = yyjson_get_str(yyjson_obj_iter_get_val(key));

        if (!name || !pattern_str) {
            cbm_log_warn("userconfig.skip_non_string", "file", source_label);
            continue;
        }

        char *pattern = strdup(pattern_str);
        if (!pattern) {
            return CBM_NOT_FOUND;
        }
        for (char *c = pattern; *c; c++) {
            if (*c == '\\') {
                *c = '/';
            }
        }

        if (!path_prop_name_ok(name) || !path_prop_pattern_ok(pattern)) {
            cbm_log_warn("userconfig.skip_bad_path_property", "file", source_label, "property",
                         name);
            free(pattern);
            continue;
        }

        int slot = 0;
        while (slot < cfg->path_prop_count && strcmp(cfg->path_props[slot].property, name) != 0) {
            slot++;
        }
        if (slot < cfg->path_prop_count) {
            free(cfg->path_props[slot].pattern);
            cfg->path_props[slot].pattern = pattern;
            continue;
        }

        cbm_path_prop_t *tmp = realloc(cfg->path_props, (size_t)(cfg->path_prop_count + SKIP_ONE) *
                                                            sizeof(cbm_path_prop_t));
        char *name_copy = strdup(name);
        if (tmp) {
            cfg->path_props = tmp;
        }
        if (!tmp || !name_copy) {
            free(name_copy);
            free(pattern);
            return CBM_NOT_FOUND;
        }
        cfg->path_props[cfg->path_prop_count].property = name_copy;
        cfg->path_props[cfg->path_prop_count].pattern = pattern;
        cfg->path_prop_count++;
    }
    return 0;
}

/*
 * Read a JSON file and parse extra_extensions and path_properties from it.
 * Silently ignores missing files. Logs warnings for corrupt JSON.
 * Returns 0 on success (or absent file), -1 on alloc failure.
 */
static int load_config_file(const char *path, cbm_userext_t **entries, int *count,
                            cbm_userconfig_t *cfg, char source_sha256[CBM_SHA256_HEX_LEN + 1]) {
    userconfig_source_digest("missing-or-unreadable", NULL, 0, source_sha256);
    FILE *f = cbm_fopen(path, "rb");
    if (!f) {
        return 0; /* file absent — silently ignore */
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        (void)fclose(f);
        userconfig_source_digest("seek-error", NULL, 0, source_sha256);
        return 0;
    }
    long len = ftell(f);
    if (fseek(f, 0, SEEK_SET) != 0) {
        (void)fclose(f);
        userconfig_source_digest("seek-error", NULL, 0, source_sha256);
        return 0;
    }

    if (len <= 0 || len > MAX_CONFIG_SIZE) {
        (void)fclose(f);
        if (len > MAX_CONFIG_SIZE) {
            cbm_log_warn("userconfig.file_too_large", "path", path);
            userconfig_source_digest("oversized", NULL, 0, source_sha256);
        } else {
            userconfig_source_digest("empty", NULL, 0, source_sha256);
        }
        return 0;
    }

    char *buf = malloc((size_t)len + SKIP_ONE);
    if (!buf) {
        (void)fclose(f);
        return CBM_NOT_FOUND;
    }

    size_t nread = fread(buf, SKIP_ONE, (size_t)len, f);
    (void)fclose(f);
    if (nread > (size_t)len) {
        nread = (size_t)len;
    }
    buf[nread] = '\0';
    userconfig_source_digest("present", buf, nread, source_sha256);

    yyjson_doc *doc = yyjson_read(buf, nread, 0);
    free(buf);

    if (!doc) {
        cbm_log_warn("userconfig.corrupt_json", "path", path);
        return 0; /* corrupt JSON — silently ignore (fail-open) */
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    int rc = parse_extra_extensions(root, entries, count, path);
    if (rc == 0) {
        rc = parse_path_properties(root, cfg, path);
    }
    yyjson_doc_free(doc);
    return rc;
}

/* ── Public API ──────────────────────────────────────────────────── */

cbm_userconfig_t *cbm_userconfig_load(const char *repo_path) {
    cbm_userconfig_t *cfg = calloc(CBM_ALLOC_ONE, sizeof(cbm_userconfig_t));
    if (!cfg) {
        return NULL;
    }

    cbm_userext_t *entries = NULL;
    int count = 0;

    /* ── Step 1: Load global config ── */
    enum { PATH_BUF_SZ = 1280 };
    const char *cfg_base = cbm_app_config_dir();
    const char *cfg_fallback = cfg_base ? cfg_base : "/tmp";
    char global_path[PATH_BUF_SZ];
    snprintf(global_path, sizeof(global_path), "%s/codebase-memory-mcp/config.json", cfg_fallback);

    if (load_config_file(global_path, &entries, &count, cfg, cfg->global_source_sha256) != 0) {
        for (int i = 0; i < count; i++) {
            free(entries[i].ext);
        }
        free(entries);
        free_path_props(cfg);
        free(cfg);
        return NULL;
    }

    int global_count = count; /* entries[0..global_count) are from global */

    /* ── Step 2: Load project config ── */
    userconfig_source_digest("not-applicable", NULL, 0, cfg->project_source_sha256);
    if (repo_path && repo_path[0]) {
        char project_path[PATH_BUF_SZ];
        snprintf(project_path, sizeof(project_path), "%s/.codebase-memory.json", repo_path);

        if (load_config_file(project_path, &entries, &count, cfg, cfg->project_source_sha256) !=
            0) {
            /* Free already-allocated entries */
            for (int i = 0; i < count; i++) {
                free(entries[i].ext);
            }
            free(entries);
            free_path_props(cfg);
            free(cfg);
            return NULL;
        }
    }

    /*
     * ── Step 3: Dedup — project entries win over global ──
     *
     * For any extension that appears in both global (indices 0..global_count)
     * and project (indices global_count..count), remove the global entry by
     * replacing it with the last global entry (order-insensitive dedup).
     */
    for (int p = global_count; p < count; p++) {
        for (int g = 0; g < global_count; g++) {
            if (entries[g].ext && strcmp(entries[g].ext, entries[p].ext) == 0) {
                /* Remove global entry: overwrite with last global entry */
                free(entries[g].ext);
                entries[g] = entries[global_count - SKIP_ONE];
                entries[global_count - SKIP_ONE].ext = NULL; /* mark as consumed */
                global_count--;
                break;
            }
        }
    }

    /*
     * Compact: remove any NULL-ext slots left by the dedup step.
     * (Those are the consumed "last global" entries.)
     */
    int write_idx = 0;
    for (int i = 0; i < count; i++) {
        if (entries[i].ext != NULL) {
            entries[write_idx++] = entries[i];
        }
    }
    count = write_idx;

    cfg->entries = entries;
    cfg->count = count;
    return cfg;
}

CBMLanguage cbm_userconfig_lookup(const cbm_userconfig_t *cfg, const char *ext) {
    if (!cfg || !ext || !ext[0]) {
        return CBM_LANG_COUNT;
    }
    for (int i = 0; i < cfg->count; i++) {
        if (cfg->entries[i].ext && strcmp(cfg->entries[i].ext, ext) == 0) {
            return cfg->entries[i].lang;
        }
    }
    return CBM_LANG_COUNT;
}

size_t cbm_path_prop_value(const char *pattern, const char *rel_path, bool path_is_dir,
                           const char **out) {
    if (!pattern || !rel_path || !out) {
        return 0;
    }
    /* Directories only: stop before the file name. */
    const char *dirs_end = rel_path + strlen(rel_path);
    if (!path_is_dir) {
        dirs_end = strrchr(rel_path, '/');
        if (!dirs_end) {
            return 0;
        }
    }

    for (const char *start = rel_path; start < dirs_end;) {
        const char *pat = pattern;
        const char *dir = start;
        const char *value = NULL;
        size_t value_len = 0;
        bool matched = true;
        while (*pat && matched) {
            size_t pat_len = strcspn(pat, "/");
            size_t dir_len = 0;
            while (dir + dir_len < dirs_end && dir[dir_len] != '/') {
                dir_len++;
            }
            if (dir >= dirs_end) {
                matched = false; /* pattern is longer than what is left of the path */
            } else if (pat_len == SKIP_ONE && pat[0] == '*') {
                value = dir;
                value_len = dir_len;
            } else if (pat_len != dir_len || memcmp(pat, dir, pat_len) != 0) {
                matched = false;
            }
            pat += pat_len + (pat[pat_len] == '/');
            dir += dir_len + (dir + dir_len < dirs_end);
        }
        if (matched && value_len > 0) {
            *out = value;
            return value_len;
        }
        const char *slash = memchr(start, '/', (size_t)(dirs_end - start));
        if (!slash) {
            break;
        }
        start = slash + SKIP_ONE;
    }
    return 0;
}

void cbm_userconfig_free(cbm_userconfig_t *cfg) {
    if (!cfg) {
        return;
    }
    for (int i = 0; i < cfg->count; i++) {
        free(cfg->entries[i].ext);
    }
    free(cfg->entries);
    free_path_props(cfg);
    free(cfg);
}
