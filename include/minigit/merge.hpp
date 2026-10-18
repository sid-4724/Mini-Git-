#pragma once

#include "minigit/storage.hpp"

namespace minigit {

struct MergeResult {
    Snapshot stagedFiles;
    Snapshot workingFiles;
    set<string> conflicts;
    set<string> binaryConflicts;
};

MergeResult compareThreeSnapshots(ObjectStore& objects, const Snapshot& base,
                                  const Snapshot& ours, const Snapshot& theirs);
}
