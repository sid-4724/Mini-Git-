#pragma once

#include "minigit/storage.hpp"
#include "minigit/graph.hpp"

namespace minigit {

FilePath findRepository(const FilePath& startingPath);
void initializeRepository(const FilePath& directory);

class Repository {
public:
    explicit Repository(const FilePath& root);
    string headId() const;
    Snapshot headSnapshot() const;
    Snapshot indexSnapshot() const;
    FilePath resolvePath(const string& argument) const;
    set<string> scanFiles(const FilePath& directory) const;
    void add(const string& argument);
    void status() const;
    void commit(const string& message);
    void log() const;
    void diff(bool staged) const;
    void restore(const string& argument, const string& source);
    void branches() const;
    void createBranch(const string& name);
    void deleteBranch(const string& name);
    void switchBranch(const string& name);
    void mergeBase(const string& first, const string& second) const;
    void merge(const string& name);
    void abortMerge();
private:
    FilePath root_;
    FilePath metadataPath_;
    ObjectStore objects_;
    State state_;
    CommitGraph commitGraph_;
    void saveState(const State& nextState);
    Snapshot workingSnapshot(const Snapshot& trackedFiles) const;
    string branchTip(const string& name) const;
    void requireClean() const;
};

set<string> snapshotPaths(const Snapshot& first, const Snapshot& second);
}
