#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace minigit {
using namespace std;

using FilePath = filesystem::path;
using Snapshot = map<string, string>;

constexpr size_t MAX_FILE_BYTES = 16 * 1024 * 1024;
constexpr size_t MAX_OBJECT_BYTES = 32 * 1024 * 1024;
constexpr size_t MAX_STATE_BYTES = 8 * 1024 * 1024;
constexpr size_t MAX_TRANSACTION_BYTES = 64 * 1024 * 1024;
constexpr size_t MAX_ENTRIES = 20000;
constexpr size_t MAX_GRAPH_COMMITS = 20000;
constexpr size_t MAX_HISTORY_BYTES = 64 * 1024 * 1024;
constexpr size_t MAX_PATH_BYTES = 4096;
constexpr size_t MAX_MESSAGE_BYTES = 65536;

struct Commit {
    string snapshotId;
    vector<string> parentCommitIds;
    uint64_t timestamp = 0;
    string message;
};

struct MergeState {
    bool active = false;
    string ours;
    string theirs;
    string base;
    string originalIndexId;
    string ownedSnapshotId;
    set<string> unresolvedPaths;
};

struct State {
    string currentBranch;
    map<string, string> branches;
    string indexId;
    MergeState merge;
};

class Error : public runtime_error {
public:
    explicit Error(const string& message) : runtime_error(message) {}
};

// An absent path has no blob ID; an empty file still has a nonempty ID.
string versionAt(const Snapshot& snapshot, const string& path);

void validateId(const string& id);
void validateRelativePath(const string& path);
void validateSnapshot(const Snapshot& snapshot);
void validateBranchName(const string& name);
bool isMetadataName(const string& name);
string display(const string& value);
string quoteShellArgument(const string& value);
}
