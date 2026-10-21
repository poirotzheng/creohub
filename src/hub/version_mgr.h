/* creo-hub M3b b3: P1 version manager (docs\14 5.4 / docs\23 3.6).
 * Immutable version dirs plugins\<id>\versions\<v>\ + a thin root pointer
 * plugin.json {"currentVersion":"<v>"} (v2 unknown-field tolerance keeps old
 * packages working; classic layouts have no pointer = zero migration).
 * update/rollback = pointer switch + remount; failure preserves the scene
 * (P1b manual rollback: CAD sessions have a human present). */
#ifndef VERSION_MGR_H
#define VERSION_MGR_H

/* List version dirs that contain plugin.json, ascending strcmp order.
 * Returns count filled into out[][32] (<= max). */
int vm_list(const char* pkg_dir, char out[][32], int max);

/* 1 when versions\<ver>\plugin.json exists. */
int vm_has(const char* pkg_dir, const char* ver);

/* Rewrite the ROOT plugin.json pointer to <ver> (atomic .tmp + replace).
 * Root must parse as JSON; the field is added when absent. Returns 1 ok,
 * 0 on failure with err filled. */
int vm_set_current(const char* pkg_dir, const char* ver, char* err, int errsz);

/* Version-string charset gate (b4: exported for the registry pointer parse
 * so currentVersion can never carry path separators / ".." - the versions\
 * path is built by concatenation). 1 ok: 1..31 chars of [0-9A-Za-z.+-]. */
int vm_valid_ver_chars(const char* v);

#endif /* VERSION_MGR_H */
