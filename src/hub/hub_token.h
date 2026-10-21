/* creo-hub M2 (G4, docs\13): session token. 128-bit CSPRNG hex generated
 * per hub start (rotation), held in memory for the L3 header check and
 * mirrored to <root>\hub_token.txt for same-machine bridge processes (M2.5
 * DSH bridge reads it). File ACL is inherited from the root directory
 * (user-owned) = same-user access; the token only ever guards a
 * loopback-only face, and every start invalidates the previous file. */
#ifndef HUB_TOKEN_H
#define HUB_TOKEN_H

/* Generate a fresh 128-bit token (32 lowercase hex chars). Returns 1. */
int  hub_token_generate(void);
/* Current token value ("" before first generate). NEVER log this. */
const char* hub_token_value(void);
/* Write <root>\hub_token.txt (overwrite = rotation). Returns 1 on success. */
int  hub_token_write_file(const char* root);
/* Delete the token file (shutdown hygiene). Returns 1 (missing = success). */
int  hub_token_delete_file(const char* root);

#endif /* HUB_TOKEN_H */
