/* creo-hub M3b b1: schema whitelist single-source + runtime value validator.
 * See schema_val.h. Pure cJSON traversal, no Creo/PTK, no new system APIs
 * (Win7 invariant). ~300 lines budget per docs\22 §6.2. */
#include "schema_val.h"

#include <stdio.h>
#include <string.h>

/* ---------------- whitelist tables (single source) ---------------- */

static const struct { const char* name; SvType t; } SV_TYPES[] = {
    { "object",  SVT_OBJECT  },
    { "array",   SVT_ARRAY   },
    { "string",  SVT_STRING  },
    { "number",  SVT_NUMBER  },
    { "boolean", SVT_BOOLEAN },
    { NULL,      SVT_NONE    }
};

static const struct { const char* key; SvKw k; } SV_KWS[] = {
    { "type",                 SVKW_TYPE },
    { "properties",           SVKW_PROPERTIES },
    { "items",                SVKW_ITEMS },
    { "required",             SVKW_REQUIRED },
    { "description",          SVKW_DESCRIPTION },
    { "enum",                 SVKW_ENUM },
    { "additionalProperties", SVKW_ADDL },
    { NULL,                   SVKW_UNKNOWN }
};

SvType sv_type_of(const char* name)
{
    int i;
    if (!name) return SVT_NONE;
    for (i = 0; SV_TYPES[i].name; i++)
        if (!strcmp(SV_TYPES[i].name, name)) return SV_TYPES[i].t;
    return SVT_NONE;
}

const char* sv_type_name(SvType t)
{
    int i;
    for (i = 0; SV_TYPES[i].name; i++)
        if (SV_TYPES[i].t == t) return SV_TYPES[i].name;
    return "none";
}

SvKw sv_kw_of(const char* key)
{
    int i;
    if (!key) return SVKW_UNKNOWN;
    for (i = 0; SV_KWS[i].key; i++)
        if (!strcmp(SV_KWS[i].key, key)) return SV_KWS[i].k;
    return SVKW_UNKNOWN;
}

/* ---------------- runtime value-domain validation ---------------- */

#define SV_MAX_DEPTH 4

static SvType sv_node_declared_type(const cJSON* node)
{
    const cJSON* t;
    if (!node || !cJSON_IsObject(node)) return SVT_NONE;
    t = cJSON_GetObjectItemCaseSensitive((cJSON*)node, "type");
    if (!t) return SVT_NONE;              /* untyped node: value checks only */
    if (!cJSON_IsString(t)) return SVT_NONE;
    return sv_type_of(t->valuestring);
}

static int sv_json_type_matches(const cJSON* v, SvType t)
{
    switch (t) {
    case SVT_OBJECT:  return cJSON_IsObject(v);
    case SVT_ARRAY:   return cJSON_IsArray(v);
    case SVT_STRING:  return cJSON_IsString(v);
    case SVT_NUMBER:  return cJSON_IsNumber(v);
    case SVT_BOOLEAN: return cJSON_IsBool(v);
    default:          return 1;           /* SVT_NONE: no type constraint */
    }
}

static int sv_enum_contains(const cJSON* enumarr, const cJSON* v)
{
    const cJSON* e = NULL;
    cJSON_ArrayForEach(e, (cJSON*)enumarr) {
        if (cJSON_IsString(e) && cJSON_IsString(v)) {
            if (!strcmp(e->valuestring, v->valuestring)) return 1;
        } else if (cJSON_IsNumber(e) && cJSON_IsNumber(v)) {
            if (e->valuedouble == v->valuedouble) return 1;
        } else if (cJSON_IsBool(e) && cJSON_IsBool(v)) {
            if (cJSON_IsTrue(e) == cJSON_IsTrue(v)) return 1;
        }
    }
    return 0;
}

static int sv_node(const cJSON* node, const cJSON* v, int depth,
                   const char* path, char* err, int errsz)
{
    SvType t;
    const cJSON* kw;

    if (depth > SV_MAX_DEPTH) {
        _snprintf(err, errsz, "%s: schema nesting too deep (validator guard)", path);
        return 0;
    }
    if (!node || !cJSON_IsObject(node)) return 1;   /* defensive: reg-validated */

    t = sv_node_declared_type(node);
    if (t != SVT_NONE && !sv_json_type_matches(v, t)) {
        _snprintf(err, errsz, "%s: expected %s, got %s",
                  path, sv_type_name(t),
                  cJSON_IsObject(v) ? "object" :
                  cJSON_IsArray(v) ? "array" :
                  cJSON_IsString(v) ? "string" :
                  cJSON_IsNumber(v) ? "number" :
                  cJSON_IsBool(v) ? "boolean" :
                  cJSON_IsNull(v) ? "null" : "unknown");
        return 0;
    }

    kw = cJSON_GetObjectItemCaseSensitive((cJSON*)node, "enum");
    if (kw && cJSON_IsArray(kw) && !sv_enum_contains(kw, v)) {
        _snprintf(err, errsz, "%s: value not in enum", path);
        return 0;
    }

    if (t == SVT_OBJECT) {
        const cJSON* props = cJSON_GetObjectItemCaseSensitive((cJSON*)node, "properties");
        const cJSON* req   = cJSON_GetObjectItemCaseSensitive((cJSON*)node, "required");
        const cJSON* addl  = cJSON_GetObjectItemCaseSensitive((cJSON*)node, "additionalProperties");
        const cJSON* child = NULL;
        const cJSON* r = NULL;

        if (req && cJSON_IsArray(req)) {
            cJSON_ArrayForEach(r, (cJSON*)req) {
                if (cJSON_IsString(r) &&
                    cJSON_GetObjectItemCaseSensitive((cJSON*)v, r->valuestring) == NULL) {
                    _snprintf(err, errsz, "%s.%s: required property missing", path, r->valuestring);
                    return 0;
                }
            }
        }
        if (addl && cJSON_IsFalse(addl) && props) {
            cJSON_ArrayForEach(child, (cJSON*)v) {
                if (cJSON_GetObjectItemCaseSensitive((cJSON*)props, child->string) == NULL) {
                    _snprintf(err, errsz, "%s.%s: additionalProperties=false forbids this key",
                              path, child->string ? child->string : "?");
                    return 0;
                }
            }
        }
        if (props && cJSON_IsObject(props)) {
            cJSON_ArrayForEach(child, (cJSON*)v) {
                const cJSON* ps = cJSON_GetObjectItemCaseSensitive((cJSON*)props, child->string);
                char sub[256];
                if (!ps) continue;      /* unknown key handled above */
                _snprintf(sub, sizeof(sub), "%s.%s", path, child->string ? child->string : "?");
                if (!sv_node(ps, child, depth + 1, sub, err, errsz)) return 0;
            }
        }
    } else if (t == SVT_ARRAY) {
        const cJSON* items = cJSON_GetObjectItemCaseSensitive((cJSON*)node, "items");
        const cJSON* el = NULL;
        int i = 0;
        if (items && cJSON_IsObject(items)) {
            cJSON_ArrayForEach(el, (cJSON*)v) {
                char sub[256];
                _snprintf(sub, sizeof(sub), "%s[%d]", path, i);
                if (!sv_node(items, el, depth + 1, sub, err, errsz)) return 0;
                i++;
            }
        }
    }
    return 1;
}

int sv_validate_args(const cJSON* schema, const cJSON* args,
                     char* err, int errsz)
{
    cJSON empty_obj;
    if (!schema || !cJSON_IsObject(schema)) return 1;   /* defensive */
    if (!args) {
        /* absent args = empty object (stack-side, no alloc, no fail-open) */
        memset(&empty_obj, 0, sizeof(empty_obj));
        empty_obj.type = cJSON_Object;
        return sv_node(schema, &empty_obj, 0, "args", err, errsz);
    }
    return sv_node(schema, args, 0, "args", err, errsz);
}
