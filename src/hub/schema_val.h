/* creo-hub M3b b1: schema whitelist single-source + runtime value validator.
 * docs\23 §3.2 / docs\22 §6.2: 7-keyword whitelist subset
 * (type/properties/items<=1 nest/required/description/enum/additionalProperties).
 * Single-source discipline: registry.c structural check AND call_exec runtime
 * value check consume the SAME keyword/type tables declared here.
 * Win7 invariant: pure C89-ish + cJSON only, no new system APIs. */
#ifndef SCHEMA_VAL_H
#define SCHEMA_VAL_H

#include "cJSON.h"

typedef enum {
    SVT_NONE = 0,
    SVT_OBJECT,
    SVT_ARRAY,
    SVT_STRING,
    SVT_NUMBER,
    SVT_BOOLEAN
} SvType;

typedef enum {
    SVKW_UNKNOWN = 0,
    SVKW_TYPE,
    SVKW_PROPERTIES,
    SVKW_ITEMS,
    SVKW_REQUIRED,
    SVKW_DESCRIPTION,
    SVKW_ENUM,
    SVKW_ADDL          /* additionalProperties */
} SvKw;

/* whitelist lookups (SVT_NONE / SVKW_UNKNOWN = outside subset) */
SvType sv_type_of(const char* name);
const char* sv_type_name(SvType t);
SvKw sv_kw_of(const char* key);

/* Runtime value-domain validation: `args` (cJSON, may be NULL=absent) against
 * `schema` (cJSON object, registration-validated subset).
 * Returns 1 on pass. On fail returns 0 and fills err (errsz bytes) with a
 * simplified JSON-Pointer style first failure: "args.x.y: reason"
 * (hint text is written for model self-heal, docs\22 §6.2). */
int sv_validate_args(const cJSON* schema, const cJSON* args,
                     char* err, int errsz);

#endif /* SCHEMA_VAL_H */
