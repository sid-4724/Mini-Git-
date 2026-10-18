#pragma once

#include "minigit/storage.hpp"

namespace minigit {

void validateFilesystemLayout(const FilePath& root, const Snapshot& snapshot);

// Preflight, journal, and apply one complete working-directory transition.
void transitionWorktree(const FilePath& root, const ObjectStore& objects,
                        const Snapshot& ownedFiles, const Snapshot& targetFiles,
                        const State& nextState,
                        const set<string>& forceWrites = {});
void recoverTransaction(const FilePath& root);
}
