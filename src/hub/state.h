/* creo-hub M1: hub_state.json persistence */
#ifndef STATE_H
#define STATE_H

/* Load <root>\hub_state.json and apply autoMount overrides + prev_state to g_plugins.
 * Returns 1 if parsed OK, 0 if absent/corrupt (logged, never fatal - E5). */
int state_load(const char* root);

/* Write current plugin states/autoMount to <root>\hub_state.json (atomic via .tmp).
 * Returns 1 on success. */
int state_save(const char* root);

#endif /* STATE_H */
