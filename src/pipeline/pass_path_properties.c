/*
 * pass_path_properties.c — Node properties derived from where a file sits.
 *
 * Applies the "path_properties" rules of the user config (see
 * discover/userconfig.h). Each rule names a property and a directory pattern
 * with one "*"; every node whose file lies under a matching directory gets
 * that property, set to the directory name the "*" stands for.
 *
 * This lets a query group or filter by a unit that spans several folders and
 * so has no single Folder node — e.g. a toolset kept in both
 * Public/Tools/<X> and Private/Tools/<X>:
 *   MATCH (a)-[r:CALLS]->(b) RETURN a.toolset, b.toolset, count(r)
 *
 * The value depends only on the node's own path, so the pass is safe on any
 * buffer: a full index, or the changed part of an incremental one.
 */
#include "foundation/constants.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "graph_buffer/graph_buffer.h"
#include "discover/userconfig.h"
#include "foundation/log.h"
#include "foundation/compat.h"
#include "foundation/str_util.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Int → string for structured logging (thread-safe ring buffer). */
static const char *itoa_pp(int val) {
    enum { RING = 2, MASK = 1 };
    static CBM_TLS char bufs[RING][CBM_SZ_32];
    static CBM_TLS int idx = 0;
    int i = idx;
    idx = (idx + 1) & MASK;
    snprintf(bufs[i], sizeof(bufs[i]), "%d", val);
    return bufs[i];
}

/* True when the flat JSON object already has this key. */
static bool json_has_key(const char *json, const char *key) {
    char pat[CBM_SZ_128];
    int n = snprintf(pat, sizeof(pat), "\"%s\":", key);
    if (n < 0 || (size_t)n >= sizeof(pat)) {
        return true; /* cannot check — treat as present, so nothing is written */
    }
    for (const char *p = strstr(json, pat); p; p = strstr(p + n, pat)) {
        if (p > json && (p[-1] == '{' || p[-1] == ',')) {
            return true;
        }
    }
    return false;
}

/* Append "key":"value" to a node's properties JSON object. An existing key is
 * left alone: a re-run over nodes loaded from the store must not duplicate it,
 * and a property another pass wrote under the same name is not ours to change. */
static bool append_string_prop(cbm_gbuf_node_t *node, const char *key, const char *value,
                               size_t value_len) {
    const char *old = node->properties_json ? node->properties_json : "{}";
    size_t olen = strlen(old);
    if (olen < 2 || old[olen - 1] != '}') {
        return false; /* not a JSON object — leave untouched */
    }
    char raw[CBM_SZ_256];
    if (value_len >= sizeof(raw) || json_has_key(old, key)) {
        return false;
    }
    memcpy(raw, value, value_len);
    raw[value_len] = '\0';
    char esc[CBM_SZ_2K]; /* room for every byte of raw escaped as \uXXXX */
    cbm_json_escape(esc, (int)sizeof(esc), raw);

    size_t cap = olen + strlen(key) + strlen(esc) + CBM_SZ_16;
    char *neu = malloc(cap);
    if (!neu) {
        return false;
    }
    bool empty = (olen == 2);   /* "{}" */
    memcpy(neu, old, olen - 1); /* copy without trailing '}' */
    snprintf(neu + (olen - 1), cap - (olen - 1), "%s\"%s\":\"%s\"}", empty ? "" : ",", key, esc);
    int rc = cbm_gbuf_node_set_properties_json(node, neu);
    free(neu);
    return rc == 0;
}

typedef struct {
    const cbm_userconfig_t *cfg;
    int written;
} path_props_ctx_t;

static void apply_path_props(const cbm_gbuf_node_t *cnode, void *userdata) {
    path_props_ctx_t *pc = userdata;
    cbm_gbuf_node_t *node = (cbm_gbuf_node_t *)cnode;
    if (!node->file_path || !node->file_path[0]) {
        return;
    }
    /* A Folder node's path is itself a directory; every other node's ends in a file name. */
    bool is_dir = node->label && strcmp(node->label, "Folder") == 0;
    for (int i = 0; i < pc->cfg->path_prop_count; i++) {
        const cbm_path_prop_t *rule = &pc->cfg->path_props[i];
        const char *value = NULL;
        size_t len = cbm_path_prop_value(rule->pattern, node->file_path, is_dir, &value);
        if (len > 0 && append_string_prop(node, rule->property, value, len)) {
            pc->written++;
        }
    }
}

void cbm_pipeline_pass_path_properties(cbm_pipeline_ctx_t *ctx) {
    const cbm_userconfig_t *cfg = cbm_get_user_lang_config();
    if (!ctx || !ctx->gbuf || !cfg || cfg->path_prop_count == 0) {
        return;
    }
    path_props_ctx_t pc = {.cfg = cfg, .written = 0};
    cbm_gbuf_foreach_node(ctx->gbuf, apply_path_props, &pc);
    cbm_log_info("pass.path_properties", "rules", itoa_pp(cfg->path_prop_count), "written",
                 itoa_pp(pc.written));
}
