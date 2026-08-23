#ifndef FEATURES_H
#define FEATURES_H

#include "reader.h"

// Create a list AST node containing all detected platform features as keywords
// This should be called during environment initialization to populate *features*
AST *detect_features(void);

// Check if a specific feature is present in the current platform
// Returns 1 if feature is present, 0 otherwise
int has_feature(const char *feature_name);
bool feature_register(const char *feature_name);
typedef struct FeaturePersistentState FeaturePersistentState;
FeaturePersistentState *feature_persistent_state_create(void);
void feature_persistent_state_destroy(FeaturePersistentState *state);
bool feature_persistent_state_enter(FeaturePersistentState *state);
void feature_persistent_state_leave(FeaturePersistentState *state);

#endif // FEATURES_H
