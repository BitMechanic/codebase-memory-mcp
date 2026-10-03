/*
 * userconfig.h — User-defined file extension → language mappings.
 *
 * Reads extra_extensions from two optional JSON config files:
 *   Global:  $XDG_CONFIG_HOME/codebase-memory-mcp/config.json
 *            (falls back to ~/.config/codebase-memory-mcp/config.json)
 *   Project: {repo_root}/.codebase-memory.json
 *
 * Project config wins over global. Unknown language values warn and are
 * skipped (fail-open). Missing files are silently ignored.
 *
 * Format:
 *   {"extra_extensions": {".blade.php": "php", ".mjs": "javascript"}}
 *
 * The language string matching is case-insensitive.
 *
 * The same two files may also carry path_properties: node properties derived
 * from where a file sits, for grouping by a unit that spans several folders.
 * Each entry maps a property name to directory names joined by '/', exactly
 * one of them "*". Mapping "toolset" to "Tools" followed by "*" gives every
 * node under .../Tools/<X>/ the property toolset = "<X>". (The pattern is
 * spelled apart here because it cannot appear inside a C comment; see
 * docs/CONFIGURATION.md for it as written in the file.)
 */
#ifndef CBM_USERCONFIG_H
#define CBM_USERCONFIG_H

#include "cbm.h" /* CBMLanguage */
#include "foundation/sha256.h"

#include <stdbool.h>
#include <stddef.h>

/* ── Types ──────────────────────────────────────────────────────── */

typedef struct {
    char *ext;        /* file extension including dot, e.g. ".blade.php" */
    CBMLanguage lang; /* resolved language enum */
} cbm_userext_t;

typedef struct {
    char *property; /* property written on nodes, e.g. "toolset" */
    char *pattern;  /* '/'-separated directory names, exactly one of them "*" */
} cbm_path_prop_t;

typedef struct {
    cbm_userext_t *entries;      /* heap-allocated array */
    int count;                   /* number of entries */
    cbm_path_prop_t *path_props; /* heap-allocated array */
    int path_prop_count;         /* number of path_props */
    /* Digests of the exact bytes/state consumed by cbm_userconfig_load(). */
    char global_source_sha256[CBM_SHA256_HEX_LEN + 1];
    char project_source_sha256[CBM_SHA256_HEX_LEN + 1];
} cbm_userconfig_t;

/* ── API ────────────────────────────────────────────────────────── */

/*
 * Load user config from global + project files, merge (project wins).
 * repo_path: absolute path to the repository root (for project config).
 * Returns a heap-allocated cbm_userconfig_t (caller must free via
 * cbm_userconfig_free). Returns NULL only on allocation failure.
 * Missing config files are silently ignored.
 */
cbm_userconfig_t *cbm_userconfig_load(const char *repo_path);

/*
 * Look up a file extension in the user config.
 * ext: extension including dot, e.g. ".blade.php"
 * Returns the mapped CBMLanguage, or CBM_LANG_COUNT if not found.
 */
CBMLanguage cbm_userconfig_lookup(const cbm_userconfig_t *cfg, const char *ext);

/*
 * Value of a path property for one repo-relative path: the directory name the
 * pattern's "*" stands for. The pattern's names must match consecutive
 * directories of the path, starting at any depth (leftmost match wins), and
 * are compared exactly. The last component of rel_path is a file name and
 * never takes part, unless path_is_dir says rel_path is itself a directory.
 * Returns the value's length and points *out at it inside rel_path, or 0 when
 * the pattern does not match.
 */
size_t cbm_path_prop_value(const char *pattern, const char *rel_path, bool path_is_dir,
                           const char **out);

/* Free a cbm_userconfig_t returned by cbm_userconfig_load. NULL-safe. */
void cbm_userconfig_free(cbm_userconfig_t *cfg);

/* ── Integration hook ───────────────────────────────────────────── */

/*
 * Set the process-global user config that cbm_language_for_extension()
 * will consult before the built-in table.
 * cfg may be NULL to clear the override.
 * Not thread-safe — call before spawning worker threads.
 */
void cbm_set_user_lang_config(const cbm_userconfig_t *cfg);

/*
 * Get the currently active process-global user config.
 * Returns NULL if none has been set.
 * Called internally by cbm_language_for_extension().
 */
const cbm_userconfig_t *cbm_get_user_lang_config(void);

#endif /* CBM_USERCONFIG_H */
