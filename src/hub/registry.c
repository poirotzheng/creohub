/* creo-hub M1: plugin registry - scan / validate / state machine / guarded ops.
 * Validation rules per docs\06_M1技术路径与拆解.md §2.2:
 *  - id: 1..63 chars [a-z0-9-], unique
 *  - adapter: only "ptk-sync" in M1 (others -> UNSUPPORTED)
 *  - entry: must exist on disk
 *  - textDir: must contain text\usascii subdir (M0 lesson: Creo appends \text)
 *  - commands[].id: must start with "<id>." (namespace contract)
 */
#include "registry.h"
#include "hub_log.h"
#include "adapter_ptk.h"
#include "state.h"
#include "nonce.h"
#include "ledger.h"
#include "version_mgr.h"   /* b4: currentVersion charset gate at parse time */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "cJSON.h"

#define TAG "REG"

PluginRec g_plugins[MAX_PLUGINS];
int g_n_plugins = 0;
static PluginRec s_tmp[MAX_PLUGINS];   /* rescan staging (BSS, no stack risk) */

const char* registry_state_name(PluginState s)
{
    switch (s) {
    case PS_INVALID:     return "INVALID";
    case PS_UNSUPPORTED: return "UNSUPPORTED";
    case PS_REGISTERED:  return "REGISTERED";
    case PS_MOUNTING:    return "MOUNTING";
    case PS_ACTIVE:      return "ACTIVE";
    case PS_UNMOUNTING:  return "UNMOUNTING";
    case PS_ERROR:       return "ERROR";
    case PS_TAINTED:     return "TAINTED";
    }
    return "?";
}

int registry_find_idx(const char* id)
{
    int i;
    if (!id || !id[0]) return -1;
    for (i = 0; i < g_n_plugins; i++)
        if (g_plugins[i].id[0] && strcmp(g_plugins[i].id, id) == 0)
            return i;
    return -1;
}

PluginRec* registry_find(const char* id)
{
    int i = registry_find_idx(id);
    return (i >= 0) ? &g_plugins[i] : NULL;
}

/* ---------------------------------------------------------------- helpers */

static char* reg_read_whole_file(const char* path)
{
    FILE* f = fopen(path, "rb");
    long len;
    char* buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 4 * 1024 * 1024) { fclose(f); return NULL; }
    buf = (char*)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

static void mark_invalid(PluginRec* p, const char* fmt, ...)
{
    va_list ap;
    p->state = PS_INVALID;
    va_start(ap, fmt);
    _vsnprintf(p->invalid_reason, sizeof(p->invalid_reason) - 1, fmt, ap);
    va_end(ap);
    p->invalid_reason[sizeof(p->invalid_reason) - 1] = '\0';
}

static void sanitize_id(const char* id, char* out, int outsz)
{
    int i;
    for (i = 0; i < outsz - 1 && id[i]; i++)
        out[i] = (id[i] == '-' || id[i] == '.') ? '_' : id[i];
    out[i] = '\0';
}

static int valid_id_chars(const char* id)
{
    int i;
    if (!id || !id[0]) return 0;
    for (i = 0; id[i]; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return 0;
    }
    return i <= 63;
}

static int dir_exists(const char* path)
{
    DWORD a = GetFileAttributesA(path);
    return (a != INVALID_FILE_ATTRIBUTES) && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static int file_exists(const char* path)
{
    DWORD a = GetFileAttributesA(path);
    return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void normalize_slashes(char* s)
{
    int i;
    for (i = 0; s[i]; i++)
        if (s[i] == '/') s[i] = '\\';
}

/* ------------------------------------------------- M2: fingerprint (R2) */

static unsigned long long fnv1a64(const void* data, size_t len, unsigned long long h)
{
    const unsigned char* b = (const unsigned char*)data;
    size_t i;
    for (i = 0; i < len; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

/* R2 version-aware fingerprint = FNV-1a64(plugin.json full text + resolved
 * entry absolute path). Byte-level sensitivity is intended ("换人即事件"):
 * any manifest edit or entry relocation flips the badge for ACTIVE plugins. */
static unsigned long long compute_fingerprint(const char* json_path, const char* entry_abs)
{
    unsigned long long h = 1469598103934665603ULL;
    char* raw = reg_read_whole_file(json_path);
    if (raw) { h = fnv1a64(raw, strlen(raw), h); free(raw); }
    h = fnv1a64(entry_abs, strlen(entry_abs), h);
    return h;
}

/* A1 hook (docs\15): entry resolution is THIS single function - M3 versioned
 * directories (bin\<ver>\ + latest pointer + downgrade-to-installed semantics)
 * plug in here without touching the parse flow. */
static int resolve_entry_abs(PluginRec* p, const char* entry_rel, const char* base)
{
    _snprintf(p->entry_abs, sizeof(p->entry_abs), "%s\\%s", base, entry_rel);
    normalize_slashes(p->entry_abs);
    if (!file_exists(p->entry_abs)) {
        mark_invalid(p, "entry file not found: %s", entry_rel);
        return 0;
    }
    return 1;
}

/* --------------------------------------- M2: R1 inputSchema subset check */

static int schema_type_ok(const char* t)
{
    return !strcmp(t, "string") || !strcmp(t, "number") || !strcmp(t, "integer") ||
           !strcmp(t, "boolean") || !strcmp(t, "array") || !strcmp(t, "object");
}

/* M2 allowed JSON Schema subset (R1):
 *   type / properties / items (one nesting level) / required / description /
 *   enum / additionalProperties - anything else is rejected with a readable
 *   reason naming the offending keyword (M3 may widen the subset). */
static int validate_schema_node(const cJSON* node, int depth, char* err, int errsz, const char* path)
{
    const cJSON* k = NULL;
    if (!cJSON_IsObject(node)) {
        _snprintf(err, errsz, "%s: must be a JSON object", path);
        return 0;
    }
    for (k = node->child; k; k = k->next) {
        const char* key = k->string ? k->string : "?";
        if (!strcmp(key, "type")) {
            if (!cJSON_IsString(k) || !schema_type_ok(k->valuestring)) {
                _snprintf(err, errsz, "%s.type: must be one of string/number/integer/boolean/array/object", path);
                return 0;
            }
        } else if (!strcmp(key, "description")) {
            if (!cJSON_IsString(k)) {
                _snprintf(err, errsz, "%s.description: must be a string", path);
                return 0;
            }
        } else if (!strcmp(key, "enum")) {
            if (!cJSON_IsArray(k)) {
                _snprintf(err, errsz, "%s.enum: must be an array", path);
                return 0;
            }
        } else if (!strcmp(key, "properties")) {
            const cJSON* pr = NULL;
            if (!cJSON_IsObject(k)) {
                _snprintf(err, errsz, "%s.properties: must be an object", path);
                return 0;
            }
            for (pr = k->child; pr; pr = pr->next) {
                char sub[224];
                _snprintf(sub, sizeof(sub), "%s.properties.%s", path, pr->string ? pr->string : "?");
                if (!validate_schema_node(pr, depth + 1, err, errsz, sub)) return 0;
            }
        } else if (!strcmp(key, "items")) {
            char sub[224];
            _snprintf(sub, sizeof(sub), "%s.items", path);
            if (depth >= 2) {
                _snprintf(err, errsz, "%s: nested deeper than M2 subset allows (items of items)", path);
                return 0;
            }
            if (!validate_schema_node(k, depth + 1, err, errsz, sub)) return 0;
        } else if (!strcmp(key, "required")) {
            const cJSON* e = NULL;
            if (!cJSON_IsArray(k)) {
                _snprintf(err, errsz, "%s.required: must be an array", path);
                return 0;
            }
            for (e = k->child; e; e = e->next) {
                if (!cJSON_IsString(e)) {
                    _snprintf(err, errsz, "%s.required: all elements must be strings", path);
                    return 0;
                }
            }
        } else if (!strcmp(key, "additionalProperties")) {
            if (!cJSON_IsBool(k)) {
                _snprintf(err, errsz, "%s.additionalProperties: must be a boolean", path);
                return 0;
            }
        } else {
            _snprintf(err, errsz,
                      "%s: unsupported keyword '%s' (M2 subset: type/properties/items/required/description/enum/additionalProperties)",
                      path, key);
            return 0;
        }
    }
    return 1;
}

static int validate_input_schema(const cJSON* sch, char* err, int errsz)
{
    const cJSON* tt;
    if (!validate_schema_node(sch, 0, err, errsz, "inputSchema")) return 0;
    tt = cJSON_GetObjectItemCaseSensitive(sch, "type");
    if (tt && (!cJSON_IsString(tt) || strcmp(tt->valuestring, "object") != 0)) {
        _snprintf(err, errsz, "inputSchema.type: top level must be \"object\" (command args)");
        return 0;
    }
    return 1;
}

static int valid_risk(const char* s)
{
    return !strcmp(s, "safe") || !strcmp(s, "confirm") || !strcmp(s, "destructive");
}

/* ------------------------------------------------------------- parse one */

static void parse_plugin_dir_inner(const char* plugins_root, const char* dirname,
                                   PluginRec* p, const char* json_path_in,
                                   const char* expect_id, const char* pointer_ver,
                                   int depth);

static void parse_plugin_dir(const char* plugins_root, const char* dirname, PluginRec* p)
{
    parse_plugin_dir_inner(plugins_root, dirname, p, NULL, NULL, NULL, 0);
}

static void parse_plugin_dir_inner(const char* plugins_root, const char* dirname,
                                   PluginRec* p, const char* json_path_in,
                                   const char* expect_id, const char* pointer_ver,
                                   int depth)
{
    char json_path[MAX_PATH + 64];
    char base[MAX_PATH];
    char check[MAX_PATH + 64];
    char* txt;
    cJSON* root;
    cJSON* it;
    cJSON* cmds;
    int i;

    memset(p, 0, sizeof(*p));
    p->state = PS_REGISTERED;
    p->user_display = 1;
    _snprintf(p->dir, sizeof(p->dir), "%s\\%s", plugins_root, dirname);
    if (json_path_in) {
        strncpy(json_path, json_path_in, sizeof(json_path) - 1);
        json_path[sizeof(json_path) - 1] = '\0';
    } else {
        _snprintf(json_path, sizeof(json_path), "%s\\plugin.json", p->dir);
    }
    if (pointer_ver) {
        strncpy(p->current_version, pointer_ver, sizeof(p->current_version) - 1);
        p->current_version[sizeof(p->current_version) - 1] = '\0';
    }
    {   /* base dir for entry/textDir resolution = dir of the EFFECTIVE manifest
         * (package root for classic layout, versions\<v>\ for pointer layout) */
        char* sl = strrchr(json_path, '\\');
        if (sl) { *sl = '\0'; strncpy(base, json_path, sizeof(base) - 1); *sl = '\\'; }
        else    strncpy(base, p->dir, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
    }

    txt = reg_read_whole_file(json_path);
    if (!txt) {
        strncpy(p->id, dirname, sizeof(p->id) - 1);
        sanitize_id(p->id, p->sid, sizeof(p->sid));
        mark_invalid(p, "plugin.json not readable");
        return;
    }
    root = cJSON_Parse(txt);
    free(txt);
    if (!root) {
        strncpy(p->id, dirname, sizeof(p->id) - 1);
        sanitize_id(p->id, p->sid, sizeof(p->sid));
        mark_invalid(p, "plugin.json parse error (syntax)");
        return;
    }

    it = cJSON_GetObjectItemCaseSensitive(root, "manifestVersion");
    if (!cJSON_IsNumber(it) || (it->valueint != 1 && it->valueint != 2)) {
        strncpy(p->id, dirname, sizeof(p->id) - 1);
        sanitize_id(p->id, p->sid, sizeof(p->sid));
        mark_invalid(p, "manifestVersion missing or not in {1,2}");
        cJSON_Delete(root);
        return;
    }
    p->manifest_version = it->valueint;   /* M2 G2: lenient read, 1 or 2 */

    it = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (!cJSON_IsString(it) || !valid_id_chars(it->valuestring)) {
        strncpy(p->id, dirname, sizeof(p->id) - 1);
        sanitize_id(p->id, p->sid, sizeof(p->sid));
        mark_invalid(p, "id missing/invalid (need 1-63 chars of [a-z0-9-])");
        cJSON_Delete(root);
        return;
    }
    strncpy(p->id, it->valuestring, sizeof(p->id) - 1);
    sanitize_id(p->id, p->sid, sizeof(p->sid));
    if (expect_id && strcmp(p->id, expect_id) != 0) {
        char bad[64];
        strncpy(bad, p->id, sizeof(bad) - 1);   /* keep record keyed by pointer id */
        bad[sizeof(bad) - 1] = '\0';
        strncpy(p->id, expect_id, sizeof(p->id) - 1);
        sanitize_id(p->id, p->sid, sizeof(p->sid));
        mark_invalid(p, "version manifest id '%s' != pointer id '%s'", bad, expect_id);
        cJSON_Delete(root);
        return;
    }

    it = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (cJSON_IsString(it)) strncpy(p->name, it->valuestring, sizeof(p->name) - 1);
    else strncpy(p->name, p->id, sizeof(p->name) - 1);

    it = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (cJSON_IsString(it)) strncpy(p->version, it->valuestring, sizeof(p->version) - 1);
    else strcpy(p->version, "0.0.0");

    it = cJSON_GetObjectItemCaseSensitive(root, "adapter");
    if (!cJSON_IsString(it) || !it->valuestring[0]) {
        mark_invalid(p, "adapter missing");
        cJSON_Delete(root);
        return;
    }
    strncpy(p->adapter, it->valuestring, sizeof(p->adapter) - 1);
    if (strcmp(p->adapter, "ptk-sync") != 0) {
        p->state = PS_UNSUPPORTED;
        _snprintf(p->invalid_reason, sizeof(p->invalid_reason),
                  "adapter '%s' not supported in M1 (ptk-sync only)", p->adapter);
        cJSON_Delete(root);
        return;
    }

    /* b3 P1 (docs\23 3.6): version pointer redirection. A root plugin.json
     * carrying "currentVersion" is a THIN POINTER: the effective manifest is
     * versions\<v>\plugin.json (immutable version dir). Classic layouts
     * (no pointer) parse unchanged = zero migration. One level only. */
    it = cJSON_GetObjectItemCaseSensitive(root, "currentVersion");
    if (depth == 0 && cJSON_IsString(it) && it->valuestring[0]) {
        char vpath[MAX_PATH + 64], idcopy[64], cv[32];
        strncpy(cv, it->valuestring, sizeof(cv) - 1);
        cv[sizeof(cv) - 1] = '\0';
        /* b4 hardening (found while writing manifest_lint): the versions\
         * path is built by concatenation - a currentVersion with separators
         * or ".." would escape the package dir. Same charset gate the whole
         * vm_* face already enforces. */
        if (!vm_valid_ver_chars(cv)) {
            mark_invalid(p, "currentVersion '%s': invalid version chars "
                            "(need 1-31 of [0-9A-Za-z.+-])", cv);
            cJSON_Delete(root);
            return;
        }
        _snprintf(vpath, sizeof(vpath), "%s\\versions\\%s\\plugin.json", p->dir, cv);
        if (!file_exists(vpath)) {
            mark_invalid(p, "currentVersion '%s': versions\\%s\\plugin.json missing", cv, cv);
            cJSON_Delete(root);
            return;
        }
        strncpy(idcopy, p->id, sizeof(idcopy) - 1);
        idcopy[sizeof(idcopy) - 1] = '\0';
        cJSON_Delete(root);
        parse_plugin_dir_inner(plugins_root, dirname, p, vpath, idcopy, cv, 1);
        return;
    }

    it = cJSON_GetObjectItemCaseSensitive(root, "entry");
    if (!cJSON_IsString(it) || !it->valuestring[0]) {
        mark_invalid(p, "entry missing");
        cJSON_Delete(root);
        return;
    }
    if (!resolve_entry_abs(p, it->valuestring, base)) {
        cJSON_Delete(root);
        return;
    }

    it = cJSON_GetObjectItemCaseSensitive(root, "textDir");
    {
        const char* td = (cJSON_IsString(it) && it->valuestring[0]) ? it->valuestring : "text";
        _snprintf(p->text_abs, sizeof(p->text_abs), "%s\\%s", base, td);
        normalize_slashes(p->text_abs);
        _snprintf(check, sizeof(check), "%s\\text\\usascii", p->text_abs);
        if (!dir_exists(check)) {
            mark_invalid(p, "textDir lacks text\\usascii subdir (Creo appends \\text!)");
            cJSON_Delete(root);
            return;
        }
    }

    it = cJSON_GetObjectItemCaseSensitive(root, "userDisplay");
    if (cJSON_IsBool(it)) p->user_display = cJSON_IsTrue(it) ? 1 : 0;

    /* M3b b1 (docs\23 §3.1): hubCmdExport (default "hub_cmd") + callRoute
     * ("B" default in-process direct | "A" ProToolkitTaskExecute contrast).
     * Unknown-field tolerance: absent = defaults; present = strictly typed. */
    it = cJSON_GetObjectItemCaseSensitive(root, "hubCmdExport");
    if (it) {
        if (!cJSON_IsString(it) || !it->valuestring[0] ||
            strlen(it->valuestring) >= sizeof(p->hub_cmd_export)) {
            mark_invalid(p, "hubCmdExport must be a non-empty string (< %u chars)",
                         (unsigned)sizeof(p->hub_cmd_export));
            cJSON_Delete(root);
            return;
        }
        strncpy(p->hub_cmd_export, it->valuestring, sizeof(p->hub_cmd_export) - 1);
    }
    it = cJSON_GetObjectItemCaseSensitive(root, "callRoute");
    if (it) {
        if (!cJSON_IsString(it) ||
            (strcmp(it->valuestring, "A") != 0 && strcmp(it->valuestring, "B") != 0)) {
            mark_invalid(p, "callRoute must be \"B\" (direct, default) or \"A\" (task)");
            cJSON_Delete(root);
            return;
        }
        strncpy(p->call_route, it->valuestring, sizeof(p->call_route) - 1);
    }

    it = cJSON_GetObjectItemCaseSensitive(root, "autoMount");
    if (cJSON_IsBool(it)) p->auto_mount = cJSON_IsTrue(it) ? 1 : 0;

    cmds = cJSON_GetObjectItemCaseSensitive(root, "commands");
    if (p->manifest_version == 2) {
        /* M2 R1 (docs\15): v2 structural validation is MANDATORY. Per command:
         * id (namespaced) + title/desc/descForLLM (strings, required) +
         * risk (required, safe|confirm|destructive) + inputSchema (required
         * object, M2 allowed subset); optional but typed: timeoutSec (number>0),
         * reversible/idempotent (bool). The commands array itself is optional
         * (a v2 package without commands behaves like a command-less v1).
         * Failure => INVALID + readable reason (M1 validator philosophy). */
        if (cmds && !cJSON_IsArray(cmds)) {
            mark_invalid(p, "v2: commands must be an array");
            cJSON_Delete(root);
            return;
        }
        if (cJSON_IsArray(cmds)) {
            cJSON* c = NULL;
            int n = 0;
            size_t idlen = strlen(p->id);
            cJSON_ArrayForEach(c, cmds) {
                cJSON* f;
                if (!cJSON_IsObject(c)) {
                    mark_invalid(p, "v2: commands[%d] must be an object", n);
                    cJSON_Delete(root); return;
                }
                f = cJSON_GetObjectItemCaseSensitive(c, "id");
                if (!cJSON_IsString(f)) {
                    mark_invalid(p, "v2: commands[%d].id missing", n);
                    cJSON_Delete(root); return;
                }
                if (strncmp(f->valuestring, p->id, idlen) != 0 || f->valuestring[idlen] != '.') {
                    mark_invalid(p, "command id '%s' must start with '%s.'",
                                 f->valuestring, p->id);
                    cJSON_Delete(root); return;
                }
                if (n < 16) strncpy(p->commands[n], f->valuestring, sizeof(p->commands[n]) - 1);
                if (n < 16) {
                    PluginCmd* cv = &p->cmdv[n];
                    char* sch_txt;
                    memset(cv, 0, sizeof(*cv));
                    strncpy(cv->id, f->valuestring, sizeof(cv->id) - 1);
                    cv->reversible = -1;
                    cv->idempotent = -1;
                    cv->timeout_sec = 0;
                    strcpy(cv->context, "any");   /* M3b b1 P-b1-1: default any */
                    f = cJSON_GetObjectItemCaseSensitive(c, "title");
                    if (!cJSON_IsString(f)) {
                        mark_invalid(p, "v2: commands[%d].title missing or not a string (required)", n);
                        cJSON_Delete(root); return;
                    }
                    strncpy(cv->title, f->valuestring, sizeof(cv->title) - 1);
                    f = cJSON_GetObjectItemCaseSensitive(c, "desc");
                    if (!cJSON_IsString(f)) {
                        mark_invalid(p, "v2: commands[%d].desc missing or not a string (required)", n);
                        cJSON_Delete(root); return;
                    }
                    strncpy(cv->desc, f->valuestring, sizeof(cv->desc) - 1);
                    f = cJSON_GetObjectItemCaseSensitive(c, "descForLLM");
                    if (!cJSON_IsString(f)) {
                        mark_invalid(p, "v2: commands[%d].descForLLM missing or not a string (required)", n);
                        cJSON_Delete(root); return;
                    }
                    strncpy(cv->desc_llm, f->valuestring, sizeof(cv->desc_llm) - 1);
                    f = cJSON_GetObjectItemCaseSensitive(c, "risk");
                    if (!cJSON_IsString(f)) {
                        mark_invalid(p, "v2: commands[%d].risk missing or not a string (required: safe|confirm|destructive)", n);
                        cJSON_Delete(root); return;
                    }
                    if (!valid_risk(f->valuestring)) {
                        mark_invalid(p, "v2: commands[%d].risk invalid (must be safe|confirm|destructive)", n);
                        cJSON_Delete(root); return;
                    }
                    strncpy(cv->risk, f->valuestring, sizeof(cv->risk) - 1);
                    f = cJSON_GetObjectItemCaseSensitive(c, "inputSchema");
                    if (!cJSON_IsObject(f)) {
                        mark_invalid(p, "v2: commands[%d].inputSchema missing or not an object (required)", n);
                        cJSON_Delete(root); return;
                    }
                    {
                        char serr[320];
                        if (!validate_input_schema(f, serr, sizeof(serr))) {
                            mark_invalid(p, "v2: commands[%d].%s", n, serr);
                            cJSON_Delete(root); return;
                        }
                        sch_txt = cJSON_PrintUnformatted(f);
                        if (!sch_txt) {
                            mark_invalid(p, "v2: commands[%d].inputSchema serialize failed", n);
                            cJSON_Delete(root); return;
                        }
                        if (strlen(sch_txt) >= sizeof(cv->input_schema)) {
                            mark_invalid(p, "v2: commands[%d].inputSchema too large (max %u bytes serialized)",
                                         n, (unsigned)sizeof(cv->input_schema) - 1);
                            cJSON_free(sch_txt); cJSON_Delete(root); return;
                        }
                        strncpy(cv->input_schema, sch_txt, sizeof(cv->input_schema) - 1);
                        cJSON_free(sch_txt);
                    }
                    f = cJSON_GetObjectItemCaseSensitive(c, "timeoutSec");
                    if (f) {
                        if (!cJSON_IsNumber(f) || f->valuedouble <= 0) {
                            mark_invalid(p, "v2: commands[%d].timeoutSec must be a number > 0", n);
                            cJSON_Delete(root); return;
                        }
                        cv->timeout_sec = f->valueint;
                    }
                    f = cJSON_GetObjectItemCaseSensitive(c, "context");   /* M3b b1 */
                    if (f) {
                        if (!cJSON_IsString(f) ||
                            (strcmp(f->valuestring, "part") && strcmp(f->valuestring, "assembly")
                             && strcmp(f->valuestring, "any"))) {
                            mark_invalid(p, "v2: commands[%d].context must be part|assembly|any", n);
                            cJSON_Delete(root); return;
                        }
                        strncpy(cv->context, f->valuestring, sizeof(cv->context) - 1);
                    }
                    f = cJSON_GetObjectItemCaseSensitive(c, "reversible");
                    if (f) {
                        if (!cJSON_IsBool(f)) {
                            mark_invalid(p, "v2: commands[%d].reversible must be a boolean", n);
                            cJSON_Delete(root); return;
                        }
                        cv->reversible = cJSON_IsTrue(f) ? 1 : 0;
                    }
                    f = cJSON_GetObjectItemCaseSensitive(c, "idempotent");
                    if (f) {
                        if (!cJSON_IsBool(f)) {
                            mark_invalid(p, "v2: commands[%d].idempotent must be a boolean", n);
                            cJSON_Delete(root); return;
                        }
                        cv->idempotent = cJSON_IsTrue(f) ? 1 : 0;
                    }
                    /* M4b: async long-task marker (docs\39 3.1) */
                    f = cJSON_GetObjectItemCaseSensitive(c, "async");
                    if (f) {
                        if (!cJSON_IsBool(f)) {
                            mark_invalid(p, "v2: commands[%d].async must be a boolean", n);
                            cJSON_Delete(root); return;
                        }
                        cv->is_async = cJSON_IsTrue(f) ? 1 : 0;
                    }
                    /* M4b: undoCommand compensation hint (docs\39 3.5) */
                    f = cJSON_GetObjectItemCaseSensitive(c, "undoCommand");
                    if (f) {
                        if (!cJSON_IsString(f) || !f->valuestring[0] ||
                            strncmp(f->valuestring, p->id, idlen) != 0 ||
                            f->valuestring[idlen] != '.') {
                            mark_invalid(p, "v2: commands[%d].undoCommand must be '<id>.<cmd>' in the same package", n);
                            cJSON_Delete(root); return;
                        }
                        if (strcmp(f->valuestring, cv->id) == 0) {
                            mark_invalid(p, "v2: commands[%d].undoCommand must not reference itself", n);
                            cJSON_Delete(root); return;
                        }
                        strncpy(cv->undo_cmd, f->valuestring, sizeof(cv->undo_cmd) - 1);
                    }
                    /* M4b: group (docs\39 3.6) */
                    f = cJSON_GetObjectItemCaseSensitive(c, "group");
                    if (f) {
                        if (!cJSON_IsString(f) || !f->valuestring[0] ||
                            strlen(f->valuestring) > sizeof(cv->group) - 1) {
                            mark_invalid(p, "v2: commands[%d].group must be a non-empty string (max 32)", n);
                            cJSON_Delete(root); return;
                        }
                        strncpy(cv->group, f->valuestring, sizeof(cv->group) - 1);
                    }
                }
                n++;
            }
            /* M4b: undoCommand target must exist in the same package
             * (post-pass: forward references are allowed). */
            {
                int u, k, found;
                for (u = 0; u < n && u < 16; u++) {
                    if (!p->cmdv[u].undo_cmd[0]) continue;
                    found = 0;
                    for (k = 0; k < n && k < 16; k++)
                        if (strcmp(p->cmdv[u].undo_cmd, p->cmdv[k].id) == 0) { found = 1; break; }
                    if (!found) {
                        mark_invalid(p, "v2: commands[%d].undoCommand '%s' not found in this package",
                                     u, p->cmdv[u].undo_cmd);
                        cJSON_Delete(root); return;
                    }
                }
            }
            p->n_commands = n;
        }
    } else if (cJSON_IsArray(cmds)) {
        /* ---- v1 path: UNCHANGED (DoD 11 - byte-identical M1 behavior) ---- */
        cJSON* c = NULL;
        int n = 0;
        size_t idlen = strlen(p->id);
        cJSON_ArrayForEach(c, cmds) {
            cJSON* cid = cJSON_GetObjectItemCaseSensitive(c, "id");
            if (!cJSON_IsString(cid)) {
                mark_invalid(p, "commands[%d].id missing", n);
                cJSON_Delete(root);
                return;
            }
            if (strncmp(cid->valuestring, p->id, idlen) != 0 || cid->valuestring[idlen] != '.') {
                mark_invalid(p, "command id '%s' must start with '%s.'",
                             cid->valuestring, p->id);
                cJSON_Delete(root);
                return;
            }
            if (n < 16) strncpy(p->commands[n], cid->valuestring, sizeof(p->commands[n]) - 1);
            n++;
        }
        p->n_commands = n;
    }

    /* M2 G2/P3: requires/provides parsed + exposed for v2; NOT enforced
     * (dependency-graph reconciliation = M3, service registry = M3+).
     * v1 packages keep ignoring unknown fields (byte-identical behavior). */
    if (p->manifest_version == 2) {
        cJSON* rq = cJSON_GetObjectItemCaseSensitive(root, "requires");
        cJSON* pv = cJSON_GetObjectItemCaseSensitive(root, "provides");
        if (rq) {
            cJSON* e = NULL;
            int n = 0;
            if (!cJSON_IsArray(rq)) {
                mark_invalid(p, "v2: requires must be an array of strings");
                cJSON_Delete(root); return;
            }
            cJSON_ArrayForEach(e, rq) {
                if (!cJSON_IsString(e)) {
                    mark_invalid(p, "v2: requires[%d] must be a string", n);
                    cJSON_Delete(root); return;
                }
                if (n < 8) strncpy(p->requires_list[n], e->valuestring, sizeof(p->requires_list[n]) - 1);
                n++;
            }
            if (n > 8) {
                mark_invalid(p, "v2: requires has %d entries (max 8 in M2)", n);
                cJSON_Delete(root); return;
            }
            p->n_requires = n;
        }
        if (pv) {
            cJSON* e = NULL;
            int n = 0;
            if (!cJSON_IsArray(pv)) {
                mark_invalid(p, "v2: provides must be an array of strings");
                cJSON_Delete(root); return;
            }
            cJSON_ArrayForEach(e, pv) {
                if (!cJSON_IsString(e)) {
                    mark_invalid(p, "v2: provides[%d] must be a string", n);
                    cJSON_Delete(root); return;
                }
                if (n < 8) strncpy(p->provides_list[n], e->valuestring, sizeof(p->provides_list[n]) - 1);
                n++;
            }
            if (n > 8) {
                mark_invalid(p, "v2: provides has %d entries (max 8 in M2)", n);
                cJSON_Delete(root); return;
            }
            p->n_provides = n;
        }
    }

    /* R2: version-aware fingerprint of the successfully parsed record */
    p->fingerprint = compute_fingerprint(json_path, p->entry_abs);

    (void)i;
    cJSON_Delete(root);
}

/* ------------------------------------------------------------------ scan */

static int scan_into(PluginRec* arr, const char* root)
{
    char plugins_root[MAX_PATH + 16];
    char pattern[MAX_PATH + 32];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0;

    _snprintf(plugins_root, sizeof(plugins_root), "%s\\plugins", root);
    _snprintf(pattern, sizeof(pattern), "%s\\*", plugins_root);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        hub_log(TAG, "plugins dir not found: %s", plugins_root);
        return 0;
    }
    do {
        char jsonpath[MAX_PATH + 64];
        int j, dup;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        _snprintf(jsonpath, sizeof(jsonpath), "%s\\%s\\plugin.json", plugins_root, fd.cFileName);
        if (!file_exists(jsonpath)) {
            hub_log(TAG, "skip dir without plugin.json: %s", fd.cFileName);
            continue;
        }
        if (n >= MAX_PLUGINS) {
            hub_log(TAG, "MAX_PLUGINS(%d) reached, skipping %s", MAX_PLUGINS, fd.cFileName);
            continue;
        }
        parse_plugin_dir(plugins_root, fd.cFileName, &arr[n]);
        /* duplicate id check (only when id was parsed) */
        if (arr[n].state != PS_INVALID || arr[n].id[0]) {
            dup = 0;
            for (j = 0; j < n; j++)
                if (arr[j].id[0] && strcmp(arr[j].id, arr[n].id) == 0) { dup = 1; break; }
            if (dup) mark_invalid(&arr[n], "duplicate id '%s'", arr[n].id);
        }
        hub_log(TAG, "plugin[%d] dir='%s' id='%s' state=%s%s%s", n, fd.cFileName,
                arr[n].id[0] ? arr[n].id : "(none)",
                registry_state_name(arr[n].state),
                (arr[n].state == PS_INVALID || arr[n].state == PS_UNSUPPORTED) ? " reason=" : "",
                (arr[n].state == PS_INVALID || arr[n].state == PS_UNSUPPORTED) ? arr[n].invalid_reason : "");
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

int registry_scan(const char* root)
{
    g_n_plugins = scan_into(g_plugins, root);
    registry_depgraph_refresh();
    hub_log(TAG, "scan complete: %d package(s) under %s\\plugins", g_n_plugins, root);
    return g_n_plugins;
}

int registry_rescan(const char* root)
{
    int tn, i, j, carried = 0;
    tn = scan_into(s_tmp, root);
    /* carry session-critical runtime state by id */
    for (i = 0; i < tn; i++) {
        if (!s_tmp[i].id[0]) continue;
        for (j = 0; j < g_n_plugins; j++) {
            if (strcmp(s_tmp[i].id, g_plugins[j].id) != 0) continue;
            {
                PluginRec* o = &g_plugins[j];
                strcpy(s_tmp[i].prev_state, o->prev_state);
                s_tmp[i].mounted_once = o->mounted_once;
                /* M2: session target survives rescan (target view is session-
                 * authoritative; disk changes must not silently edit targets) */
                s_tmp[i].want_mount = o->want_mount;
                s_tmp[i].want_known = o->want_known;
                if (o->state == PS_ACTIVE || o->state == PS_TAINTED) {
                    /* session-critical: keep live state + handle even if manifest changed */
                    s_tmp[i].state = o->state;
                    s_tmp[i].handle = o->handle;
                    s_tmp[i].last_err = o->last_err;
                    /* R2: fingerprint change detection against disk truth (fresh
                     * fingerprint already computed in s_tmp[i] by the scan).
                     * Badge only - NO auto-remount (human decision in M2;
                     * reconcile policy in M3). */
                    if (o->state == PS_ACTIVE && o->fingerprint && s_tmp[i].fingerprint &&
                        s_tmp[i].fingerprint != o->fingerprint) {
                        s_tmp[i].fingerprint_changed = 1;
                        /* b2 (§6.3): confirmations against a manifest that no
                         * longer matches disk must die with the old truth. */
                        nonce_invalidate_plugin(j);
                        hub_log(TAG, "manifest fingerprint CHANGED for ACTIVE '%s' "
                                     "(badge: manifest updated / remount recommended)", s_tmp[i].id);
                    } else {
                        s_tmp[i].fingerprint_changed = o->fingerprint_changed;
                    }
                    carried++;
                }
            }
            break;
        }
    }
    /* old ACTIVE plugins vanished from disk: keep record so unload stays possible */
    for (j = 0; j < g_n_plugins; j++) {
        int found = 0;
        if (g_plugins[j].state != PS_ACTIVE) continue;
        for (i = 0; i < tn; i++)
            if (strcmp(s_tmp[i].id, g_plugins[j].id) == 0) { found = 1; break; }
        if (!found && tn < MAX_PLUGINS) {
            hub_log(TAG, "WARNING: plugin '%s' vanished from disk while ACTIVE - "
                         "keeping record (unload still possible)", g_plugins[j].id);
            s_tmp[tn++] = g_plugins[j];
        }
    }
    memcpy(g_plugins, s_tmp, sizeof(PluginRec) * (size_t)tn);
    g_n_plugins = tn;
    registry_depgraph_refresh();
    hub_log(TAG, "rescan complete: %d package(s), %d live state(s) carried", g_n_plugins, carried);
    return g_n_plugins;
}

/* ------------------------------------------------------- b3 depgraph
 * docs\22 8.1 / docs\23 3.5: requires resolution (plugin id first, then a
 * provides-service), scan-time cycle detection (bad graph never reaches the
 * runtime: members -> INVALID + readable chain), missing dep != failure
 * (REGISTERED + "waiting for:" badge), drain-unmount = transitive dependent
 * closure in reverse-topological order (dependents first, target last). */

static int dep_resolve(const char* req, int self_idx)
{
    int k, m;
    for (k = 0; k < g_n_plugins; k++) {
        if (k == self_idx || !g_plugins[k].id[0]) continue;
        if (strcmp(g_plugins[k].id, req) == 0) return k;
    }
    for (k = 0; k < g_n_plugins; k++) {
        if (k == self_idx) continue;
        for (m = 0; m < g_plugins[k].n_provides; m++)
            if (strcmp(g_plugins[k].provides_list[m], req) == 0) return k;
    }
    return -1;
}

static unsigned char s_dfs_state[MAX_PLUGINS];  /* 0 white 1 gray 2 black */

static void dep_mark_cycle(int* stack, int top, int back_to)
{
    char chain[256];
    int i;
    chain[0] = '\0';
    for (i = back_to; i <= top; i++) {
        if (chain[0]) strcat(chain, "->");
        strcat(chain, g_plugins[stack[i]].id);
        g_plugins[stack[i]].cycle_member = 1;
    }
    strcat(chain, "->");
    strcat(chain, g_plugins[stack[back_to]].id);
    for (i = back_to; i <= top; i++) {
        PluginRec* p = &g_plugins[stack[i]];
        p->state = PS_INVALID;
        p->wait_reason[0] = '\0';
        _snprintf(p->invalid_reason, sizeof(p->invalid_reason),
                  "dependency cycle: %s", chain);
        p->invalid_reason[sizeof(p->invalid_reason) - 1] = '\0';
        hub_log(TAG, "dependency cycle detected: %s (members INVALID)", chain);
    }
}

static void dep_dfs(int u, int* stack, int top)
{
    int j, v, b;
    s_dfs_state[u] = 1;
    stack[top] = u;
    for (j = 0; j < g_plugins[u].n_requires; j++) {
        v = g_plugins[u].req_idx[j];
        if (v < 0) continue;
        if (s_dfs_state[v] == 1) {
            for (b = 0; b <= top; b++) if (stack[b] == v) break;
            dep_mark_cycle(stack, top, b);
        } else if (s_dfs_state[v] == 0) {
            dep_dfs(v, stack, top + 1);
        }
    }
    s_dfs_state[u] = 2;
}

void registry_depgraph_refresh(void)
{
    int i, j;
    for (i = 0; i < g_n_plugins; i++) {
        PluginRec* p = &g_plugins[i];
        p->cycle_member = 0;
        p->wait_reason[0] = '\0';
        for (j = 0; j < p->n_requires; j++)
            p->req_idx[j] = dep_resolve(p->requires_list[j], i);
        for (; j < 8; j++) p->req_idx[j] = -1;
    }
    memset(s_dfs_state, 0, sizeof(s_dfs_state));
    for (i = 0; i < g_n_plugins; i++) {
        int stack[MAX_PLUGINS];
        if (!g_plugins[i].id[0]) continue;
        if (g_plugins[i].state == PS_INVALID || g_plugins[i].state == PS_UNSUPPORTED)
            continue;                       /* already-bad records never enter the graph */
        if (s_dfs_state[i] == 0) dep_dfs(i, stack, 0);
    }
    for (i = 0; i < g_n_plugins; i++) {     /* waiting badges (missing dep != failure) */
        PluginRec* p = &g_plugins[i];
        if (!p->id[0] || p->state == PS_INVALID || p->state == PS_UNSUPPORTED) continue;
        for (j = 0; j < p->n_requires; j++) {
            if (p->req_idx[j] < 0) {
                _snprintf(p->wait_reason, sizeof(p->wait_reason),
                          "waiting for: %s", p->requires_list[j]);
                p->wait_reason[sizeof(p->wait_reason) - 1] = '\0';
                break;
            }
        }
    }
}

int registry_drain_plan(int idx, int* order, int max_order)
{
    int in_set[MAX_PLUGINS], emitted[MAX_PLUGINS];
    int i, j, k, n = 0, changed = 1;
    if (idx < 0 || idx >= g_n_plugins || !g_plugins[idx].id[0]) return 0;
    if (max_order < 1) return 0;
    memset(in_set, 0, sizeof(in_set));
    memset(emitted, 0, sizeof(emitted));
    in_set[idx] = 1;
    while (changed) {                       /* transitive dependents closure */
        changed = 0;
        for (i = 0; i < g_n_plugins; i++) {
            if (in_set[i]) continue;
            for (j = 0; j < g_plugins[i].n_requires; j++) {
                int v = g_plugins[i].req_idx[j];
                if (v >= 0 && in_set[v]) { in_set[i] = 1; changed = 1; break; }
            }
        }
    }
    while (n < max_order) {                 /* Kahn on dependent relation: deepest first */
        int pick = -1;
        for (i = 0; i < g_n_plugins && pick < 0; i++) {
            int has_dep = 0;
            if (!in_set[i] || emitted[i]) continue;
            for (k = 0; k < g_n_plugins && !has_dep; k++) {
                if (k == i || !in_set[k] || emitted[k]) continue;
                for (j = 0; j < g_plugins[k].n_requires; j++)
                    if (g_plugins[k].req_idx[j] == i) { has_dep = 1; break; }
            }
            if (!has_dep) pick = i;
        }
        if (pick < 0) break;                /* residual cycle: never force (guards win) */
        emitted[pick] = 1;
        order[n++] = pick;
    }
    return n;
}

ProError registry_unmount_drain(int idx, int quiet)
{
    int order[MAX_PLUGINS], n, i;
    ProError e = PRO_TK_NO_ERROR;
    n = registry_drain_plan(idx, order, MAX_PLUGINS);
    for (i = 0; i < n; i++) {
        if (g_plugins[order[i]].state != PS_ACTIVE) continue;
        e = registry_unmount(order[i], quiet);
        if (e != PRO_TK_NO_ERROR) {
            hub_log(TAG, "drain-unmount STOPPED at '%s' (ptk=%d) - scene preserved",
                    g_plugins[order[i]].id, (int)e);
            return e;
        }
    }
    return e;
}

/* ------------------------------------------------------- guarded ops */

ProError registry_mount(int idx, int quiet)
{
    PluginRec* p;
    ProError err;

    if (idx < 0 || idx >= g_n_plugins) return PRO_TK_BAD_INPUTS;
    p = &g_plugins[idx];

    if (p->state == PS_ACTIVE) {
        hub_log(TAG, "mount '%s' REFUSED: already ACTIVE", p->id);
        if (!quiet) hub_popup(1, L"creo-hub M1", L"Mount refused: plugin is already ACTIVE");
        return PRO_TK_E_IN_USE;
    }
    if (p->state == PS_INVALID || p->state == PS_UNSUPPORTED || p->state == PS_TAINTED) {
        wchar_t wbuf[640];
        hub_log(TAG, "mount '%s' REFUSED: state=%s reason='%s'",
                p->id, registry_state_name(p->state), p->invalid_reason);
        if (!quiet) {
            swprintf(wbuf, 640, L"Mount refused: %hs is %hs\n%hs",
                     p->id, registry_state_name(p->state),
                     p->invalid_reason[0] ? p->invalid_reason : "(see log)");
            if (p->state == PS_TAINTED)
                swprintf(wbuf, 640, L"Mount refused: %hs is TAINTED\nRestart Creo to clear this state.", p->id);
            hub_popup(1, L"creo-hub M1", wbuf);
        }
        return PRO_TK_BAD_CONTEXT;
    }

    /* REGISTERED or ERROR -> attempt */
    hub_log(TAG, "[STATE] %s: %s -> MOUNTING", p->id, registry_state_name(p->state));
    p->state = PS_MOUNTING;

    err = adapter_ptk_mount(p);

    if (err == PRO_TK_NO_ERROR) {
        /* M3b b1 (docs\23 §3.1): B-route export pre-validation - a v2 package
         * that declares commands MUST actually export hubCmdExport, else the
         * package goes INVALID up-front (never a half-dead runtime state).
         * v1 / command-less packages are unaffected (menu-only plugins). */
        if (p->manifest_version == 2 && p->n_commands > 0) {
            char verr[256];
            if (!adapter_verify_export(p, verr, sizeof(verr))) {
                ProError uerr = adapter_ptk_unmount(p);
                hub_log(TAG, "[STATE] %s: MOUNTING -> INVALID (export pre-check: %s; unload ret=%d)",
                        p->id, verr, (int)uerr);
                p->state = PS_INVALID;
                _snprintf(p->invalid_reason, sizeof(p->invalid_reason), "%s", verr);
                p->invalid_reason[sizeof(p->invalid_reason) - 1] = '\0';
                nonce_invalidate_plugin(idx);   /* b2: state change kills confirmations */
                state_save(g_root);
                if (!quiet)
                    hub_popup(1, L"creo-hub M3b",
                              L"Mount refused: hubCmdExport missing (package INVALID, see log)");
                return PRO_TK_BAD_INPUTS;
            }
        }
        p->state = PS_ACTIVE;
        p->mounted_once = 1;
        p->fingerprint_changed = 0;   /* R2: fresh mount = fresh manifest, clear badge */
        strncpy(p->prev_state, "ACTIVE", sizeof(p->prev_state) - 1); /* S10 fix: restore mark */
        ledger_add(idx, "mount", 0, NULL, NULL);   /* b3 P2: mount-session anchor */
        hub_log(TAG, "[STATE] %s: MOUNTING -> ACTIVE", p->id);
        state_save(g_root);
        if (!quiet) {
            wchar_t b[384];
            swprintf(b, 384, L"Mounted: %hs %hs\n(its menu appears in the Tools tab)",
                     p->id, p->version);
            hub_popup(0, L"creo-hub M1", b);
        }
    } else if (err == PRO_TK_BAD_CONTEXT) {
        p->state = PS_TAINTED;
        nonce_invalidate_plugin(idx);   /* b2: state change kills confirmations */
        hub_log(TAG, "[STATE] %s: MOUNTING -> TAINTED (BAD_CONTEXT, restart Creo)", p->id);
        state_save(g_root);
        if (!quiet)
            hub_popup_result(1, "Mount FAILED -> TAINTED (restart Creo recommended)",
                             err, p->last_user_err, p->last_msg);
    } else {
        p->state = PS_ERROR;
        nonce_invalidate_plugin(idx);   /* b2: state change kills confirmations */
        hub_log(TAG, "[STATE] %s: MOUNTING -> ERROR (%s)", p->id, hub_errname(err));
        if (!quiet) {
            char tag[160];
            _snprintf(tag, sizeof(tag), "Mount '%s' FAILED (state=ERROR, retry allowed)", p->id);
            hub_popup_result(1, tag, err, p->last_user_err, p->last_msg);
        }
    }
    return err;
}

ProError registry_unmount(int idx, int quiet)
{
    PluginRec* p;
    ProError err;

    if (idx < 0 || idx >= g_n_plugins) return PRO_TK_BAD_INPUTS;
    p = &g_plugins[idx];

    if (p->state != PS_ACTIVE) {
        hub_log(TAG, "unmount '%s' REFUSED: state=%s (not ACTIVE)",
                p->id, registry_state_name(p->state));
        if (!quiet) hub_popup(1, L"creo-hub M1", L"Unload refused: plugin is not mounted");
        return PRO_TK_BAD_INPUTS;
    }

    hub_log(TAG, "[STATE] %s: ACTIVE -> UNMOUNTING", p->id);
    p->state = PS_UNMOUNTING;
    /* b2 (docs\22 §6.3): any state change kills this plugin's confirmations -
     * approvals against a dying/dead package must not survive the transition. */
    nonce_invalidate_plugin(idx);
    /* b3 P2 (docs\14 3.4 / docs\23 3.7): drain the hub ledger LIFO BEFORE
     * user_terminate/DllUnload - hub-side L-Unload accumulator order. */
    {
        int nd = ledger_drain(idx);
        int resid = ledger_count(idx);
        if (resid)
            hub_log(TAG, "ERR P2 ledger residual=%d after drain('%s') - hub bookkeeping bug",
                    resid, p->id);
        else
            hub_log(TAG, "P2 ledger drained=%d residual=0 ('%s')", nd, p->id);
    }

    err = adapter_ptk_unmount(p);

    if (err == PRO_TK_NO_ERROR) {
        p->state = PS_REGISTERED;
        strncpy(p->prev_state, "REGISTERED", sizeof(p->prev_state) - 1); /* S10 fix: explicit unmount clears restore mark */
        hub_log(TAG, "[STATE] %s: UNMOUNTING -> REGISTERED", p->id);
        state_save(g_root);
        if (!quiet) {
            wchar_t b[384];
            swprintf(b, 384, L"Unmounted: %hs\n(its menu should be gone from the Tools tab)", p->id);
            hub_popup(0, L"creo-hub M1", b);
        }
    } else if (err == PRO_TK_GENERAL_ERROR || err == PRO_TK_BAD_CONTEXT || err == PRO_TK_NOT_VALID) {
        p->state = PS_TAINTED;
        hub_log(TAG, "[STATE] %s: UNMOUNTING -> TAINTED (%s, restart Creo)", p->id, hub_errname(err));
        state_save(g_root);
        if (!quiet)
            hub_popup_result(1, "Unload FAILED -> TAINTED (restart Creo recommended)",
                             err, PRO_TK_NO_ERROR, L"");
    } else {
        /* e.g. E_BUSY: still loaded, back to ACTIVE */
        p->state = PS_ACTIVE;
        ledger_add(idx, "mount", 0, NULL, NULL);   /* re-arm anchor: still loaded */
        hub_log(TAG, "[STATE] %s: UNMOUNTING -> ACTIVE (unload failed: %s)", p->id, hub_errname(err));
        if (!quiet)
            hub_popup_result(1, "Unload FAILED (plugin stays ACTIVE; close its dialogs and retry)",
                             err, PRO_TK_NO_ERROR, L"");
    }
    return err;
}

int registry_automount_all(int quiet, int* ok_out, int* fail_out)
{
    int i, ok = 0, fail = 0, attempted = 0;
    for (i = 0; i < g_n_plugins; i++) {
        PluginRec* p = &g_plugins[i];
        int restore;
        /* S10 fix: restore set = autoMount plugins + plugins whose previous
         * session ended ACTIVE (persisted lastState). Explicit unmount clears
         * the mark, so unmounted-before-exit plugins are NOT force-restored. */
        restore = (p->prev_state[0] && strcmp(p->prev_state, "ACTIVE") == 0);
        if (!p->auto_mount && !restore) continue;
        if (p->state != PS_REGISTERED && p->state != PS_ERROR) continue;
        attempted++;
        hub_log(TAG, "auto-init '%s' (%s)", p->id,
                (p->auto_mount && restore) ? "autoMount + prev-session ACTIVE" :
                p->auto_mount ? "autoMount" : "restore prev-session ACTIVE");
        if (registry_mount(i, quiet) == PRO_TK_NO_ERROR) ok++;
        else fail++;
    }
    if (ok_out) *ok_out = ok;
    if (fail_out) *fail_out = fail;
    return attempted;
}

int registry_set_automount(int idx, int value)
{
    PluginRec* p;
    if (idx < 0 || idx >= g_n_plugins) return 0;
    p = &g_plugins[idx];
    if (!p->id[0]) return 0;
    p->auto_mount = value ? 1 : 0;
    /* M2 target discipline: the switch IS a target edit - off means "do not
     * want mounted" for the rest of this session; persistence keeps the flag
     * so the next start rebuilds the same target. */
    p->want_mount = p->auto_mount;
    p->want_known = 1;
    hub_log(TAG, "automount('%s') = %d (target updated, persisting)", p->id, p->auto_mount);
    state_save(g_root);
    return 1;
}
