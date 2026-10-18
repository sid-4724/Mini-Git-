#include "minigit/merge.hpp"
#include "minigit/diff.hpp"

namespace minigit {
using namespace std;

namespace {
string markerSide(const string& bytes, bool present) {
    if (!present) return "(file deleted)\n";
    string result = bytes;
    if (!result.empty() && result.back() != '\n') result += '\n';
    return result;
}
}

MergeResult compareThreeSnapshots(ObjectStore& objects, const Snapshot& base,
                                  const Snapshot& ours, const Snapshot& theirs) {
    // This conservative implementation refuses file/directory changes across
    // these snapshots rather than attempting to combine incompatible layouts.
    Snapshot allPaths = base;
    for (const pair<const string, string>& entry : ours) allPaths[entry.first] = entry.second;
    for (const pair<const string, string>& entry : theirs) allPaths[entry.first] = entry.second;
    try {
        validateSnapshot(allPaths);
    } catch (const Error& error) {
        throw Error("Automatic divergent merge cannot handle this file/directory layout collision; no working files changed. " + string(error.what()));
    }

    MergeResult result;
    for (const pair<const string, string>& entry : allPaths) {
        const string& path = entry.first;
        string baseVersion = versionAt(base, path);
        string ourVersion = versionAt(ours, path);
        string theirVersion = versionAt(theirs, path);
        string selected;
        bool conflict = false;
        if (ourVersion == theirVersion) selected = ourVersion;
        else if (ourVersion == baseVersion) selected = theirVersion;
        else if (theirVersion == baseVersion) selected = ourVersion;
        else conflict = true;
        if (!conflict) {
            if (!selected.empty()) {
                result.stagedFiles[path] = selected;
                result.workingFiles[path] = selected;
            }
            continue;
        }
        result.conflicts.insert(path);
        if (!ourVersion.empty()) result.stagedFiles[path] = ourVersion;
        string baseBytes;
        string ourBytes;
        string theirBytes;
        if (!baseVersion.empty()) baseBytes = objects.getBlob(baseVersion);
        if (!ourVersion.empty()) ourBytes = objects.getBlob(ourVersion);
        if (!theirVersion.empty()) theirBytes = objects.getBlob(theirVersion);
        if (isBinary(baseBytes) || isBinary(ourBytes) || isBinary(theirBytes)) {
            result.binaryConflicts.insert(path);
            if (!ourVersion.empty()) result.workingFiles[path] = ourVersion;
        } else {
            string marked = "<<<<<<< ours\n" + markerSide(ourBytes, !ourVersion.empty()) +
                                 "=======\n" + markerSide(theirBytes, !theirVersion.empty()) + ">>>>>>> theirs\n";
            result.workingFiles[path] = objects.putBlob(marked);
        }
    }
    return result;
}
}
