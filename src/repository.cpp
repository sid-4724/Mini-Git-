#include "minigit/repository.hpp"
#include "minigit/diff.hpp"
#include "minigit/worktree.hpp"
#include "minigit/merge.hpp"

#include <iostream>
#include <chrono>
#include <cctype>

namespace minigit {
using namespace std;
using namespace std::filesystem;
using namespace std::chrono;

FilePath findRepository(const FilePath& startingPath) {
    FilePath current = absolute(startingPath).lexically_normal();
    checkSafeComponents(current);
    while (!current.empty()) {
        if (pathExists(current / ".minigit")) {
            checkSafeComponents(current / ".minigit");
            if (!is_directory(current / ".minigit")) throw Error(".minigit is not a directory.");
            return current;
        }
        FilePath parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    throw Error("Not inside a MiniGit repository. Run 'minigit init'.");
}

void initializeRepository(const FilePath& directory) {
    FilePath root = absolute(directory).lexically_normal();
    checkSafeComponents(root);
    FilePath parent = root.parent_path();
    while (parent != root && !parent.empty()) {
        if (pathExists(parent / ".minigit")) throw Error("Cannot initialize a nested MiniGit repository.");
        root = parent;
        parent = parent.parent_path();
    }
    root = absolute(directory).lexically_normal();
    FilePath metadata = root / ".minigit";
    if (pathExists(metadata)) {
        checkSafeComponents(metadata);
        RepositoryLock lock(metadata);
        Repository repository(root);
        cout << "Already initialized; existing history preserved.\n";
        return;
    }
    if (!create_directory(metadata)) throw Error("Cannot create .minigit directory.");
    RepositoryLock lock(metadata);
    create_directory(metadata / "objects");
    ObjectStore objects(metadata);
    State state;
    state.currentBranch = "main";
    state.branches["main"] = "";
    state.indexId = objects.putSnapshot(Snapshot{});
    writeAtomic(metadata / "state", encodeState(state));
    cout << "Initialized MiniGit repository on unborn branch main.\n";
}

Repository::Repository(const FilePath& root) : root_(root), metadataPath_(root / ".minigit"), objects_(metadataPath_) {
    if (pathExists(metadataPath_ / "transaction")) throw Error("Interrupted transaction found. Run 'minigit recover' before other commands.");
    state_ = decodeState(readFile(metadataPath_ / "state", MAX_STATE_BYTES));
    commitGraph_ = validateStateReferences(objects_, state_);
}
string Repository::headId() const { return state_.branches.at(state_.currentBranch); }
Snapshot Repository::headSnapshot() const {
    if (headId().empty()) return Snapshot{};
    return objects_.getSnapshot(objects_.getCommit(headId()).snapshotId);
}
Snapshot Repository::indexSnapshot() const { return objects_.getSnapshot(state_.indexId); }

set<string> snapshotPaths(const Snapshot& first, const Snapshot& second) {
    set<string> paths;
    for (const pair<const string, string>& entry : first) paths.insert(entry.first);
    for (const pair<const string, string>& entry : second) paths.insert(entry.first);
    return paths;
}

FilePath Repository::resolvePath(const string& argument) const {
    if (argument.empty() || argument.find('\0') != string::npos || argument.find('\\') != string::npos) throw Error("Invalid path argument.");
    FilePath input(argument);
    for (const FilePath& component : input) {
        if (component == "..") throw Error("Path traversal ('..') is not allowed.");
    }
    FilePath absolutePath = absolute(input);
    checkSafeComponents(absolutePath);
    absolutePath = absolutePath.lexically_normal();
    FilePath relativePath = absolutePath.lexically_relative(root_);
    if (relativePath.empty() || relativePath.is_absolute()) throw Error("Path is outside the repository.");
    for (const FilePath& component : relativePath) {
        if (component == "..") throw Error("Path is outside the repository.");
    }
    if (relativePath == ".") return root_;
    validateRelativePath(relativePath.generic_string());
    // Directory iteration gives the real spelling on case-insensitive or
    // Unicode-normalizing filesystems, keeping CLI paths and status consistent.
    FilePath actualPath = root_;
    for (const FilePath& component : relativePath) {
        FilePath candidate = actualPath / component;
        bool found = false;
        if (pathExists(candidate) && is_directory(symlink_status(actualPath))) {
            FilePath alias;
            size_t aliasCount = 0;
            for (const directory_entry& entry : directory_iterator(actualPath)) {
                file_status status = entry.symlink_status();
                if (!is_regular_file(status) && !is_directory(status)) continue;
                // An exact name wins even when other names share its inode.
                if (entry.path().filename() == component) {
                    actualPath = entry.path();
                    found = true;
                    break;
                }
                if (equivalent(entry.path(), candidate)) {
                    alias = entry.path();
                    ++aliasCount;
                }
            }
            if (!found && aliasCount > 1) {
                throw Error("Ambiguous filesystem alias: " + display(candidate.string()) + ". Use an exact directory-entry spelling.");
            }
            if (!found && aliasCount == 1) {
                actualPath = alias;
                found = true;
            }
        }
        if (!found) actualPath = candidate;
    }
    validateRelativePath(actualPath.lexically_relative(root_).generic_string());
    return actualPath;
}

set<string> Repository::scanFiles(const FilePath& directory) const {
    checkSafeComponents(directory);
    set<string> paths;
    recursive_directory_iterator current(directory);
    recursive_directory_iterator end;
    while (current != end) {
        FilePath path = current->path();
        string name = path.filename().string();
        if (isMetadataName(name)) {
            current.disable_recursion_pending();
            ++current;
            continue;
        }
        file_status fileStatus = current->symlink_status();
        if (is_symlink(fileStatus)) throw Error("Symlinks are unsupported: " + display(path.string()));
        if (is_regular_file(fileStatus)) {
            string relativePath = path.lexically_relative(root_).generic_string();
            validateRelativePath(relativePath);
            paths.insert(relativePath);
            if (paths.size() > MAX_ENTRIES) throw Error("Working directory contains more than 20000 files.");
        } else if (!is_directory(fileStatus)) {
            throw Error("Only regular files and directories are supported: " + display(path.string()));
        }
        ++current;
    }
    return paths;
}

void Repository::saveState(const State& nextState) {
    string bytes = encodeState(nextState);
    failPoint("before-state-write");
    writeAtomic(metadataPath_ / "state", bytes);
    state_ = nextState;
}

void Repository::add(const string& argument) {
    FilePath selectedPath = resolvePath(argument);
    string selected = selectedPath.lexically_relative(root_).generic_string();
    bool wholeRepository = selected == ".";
    Snapshot stagedFiles = indexSnapshot();
    set<string> selectedFiles;
    if (pathExists(selectedPath)) {
        file_status fileStatus = symlink_status(selectedPath);
        if (is_regular_file(fileStatus)) selectedFiles.insert(selected);
        else if (is_directory(fileStatus)) selectedFiles = scanFiles(selectedPath);
        else throw Error("Only regular files and directories can be staged.");
    }
    set<string> knownPaths;
    for (const pair<const string, string>& entry : stagedFiles) knownPaths.insert(entry.first);
    knownPaths.insert(state_.merge.unresolvedPaths.begin(), state_.merge.unresolvedPaths.end());
    bool matched = !selectedFiles.empty();
    State nextState = state_;
    Snapshot mergeOwned;
    Snapshot mergeOriginal;
    if (state_.merge.active) {
        mergeOwned = objects_.getSnapshot(state_.merge.ownedSnapshotId);
        mergeOriginal = objects_.getSnapshot(state_.merge.originalIndexId);
    }
    for (const string& path : knownPaths) {
        bool inScope = wholeRepository || path == selected || path.compare(0, selected.size() + 1, selected + "/") == 0;
        if (inScope) {
            matched = true;
            if (selectedFiles.count(path) == 0) stagedFiles.erase(path);
            nextState.merge.unresolvedPaths.erase(path);
        }
    }
    if (!matched && !pathExists(selectedPath)) throw Error("Path is absent and is not tracked or conflicted: " + display(selected));
    for (const string& path : selectedFiles) {
        stagedFiles[path] = objects_.putBlob(readFile(root_ / path, MAX_FILE_BYTES));
        if (state_.merge.active && (state_.merge.unresolvedPaths.count(path) != 0 || mergeOwned.count(path) != 0 || mergeOriginal.count(path) != 0)) mergeOwned[path] = stagedFiles[path];
        nextState.merge.unresolvedPaths.erase(path);
    }
    validateFilesystemLayout(root_, stagedFiles);
    if (state_.merge.active) nextState.merge.ownedSnapshotId = objects_.putSnapshot(mergeOwned);
    nextState.indexId = objects_.putSnapshot(stagedFiles);
    saveState(nextState);
    cout << "Staged " << display(selected) << ".\n";
}

Snapshot Repository::workingSnapshot(const Snapshot& trackedFiles) const {
    Snapshot workingFiles;
    for (const pair<const string, string>& entry : trackedFiles) {
        FilePath path = root_ / entry.first;
        checkSafeComponents(path);
        if (!pathExists(path)) continue;
        file_status fileStatus = symlink_status(path);
        if (is_directory(fileStatus)) continue;
        if (!is_regular_file(fileStatus)) throw Error("Unsupported tracked file type: " + display(entry.first));
        workingFiles[entry.first] = sha256(envelope("blob", readFile(path, MAX_FILE_BYTES)));
    }
    return workingFiles;
}

static void printChanges(const string& title, const Snapshot& before, const Snapshot& after) {
    cout << title << ":\n";
    bool found = false;
    for (const string& path : snapshotPaths(before, after)) {
        string oldVersion = versionAt(before, path);
        string newVersion = versionAt(after, path);
        if (oldVersion == newVersion) continue;
        found = true;
        string change = "modified";
        if (oldVersion.empty()) change = "added";
        else if (newVersion.empty()) change = "deleted";
        cout << "  " << change << " " << display(path) << '\n';
    }
    if (!found) cout << "  (none)\n";
}

void Repository::status() const {
    Snapshot head = headSnapshot();
    Snapshot index = indexSnapshot();
    Snapshot working = workingSnapshot(index);
    cout << "On branch " << state_.currentBranch;
    if (headId().empty()) cout << " (unborn)";
    cout << '\n';
    printChanges("Staged changes", head, index);
    printChanges("Unstaged changes", index, working);
    cout << "Untracked files:\n";
    bool found = false;
    for (const string& path : scanFiles(root_)) {
        if (index.count(path) == 0) {
            found = true;
            cout << "  " << display(path) << '\n';
        }
    }
    if (!found) cout << "  (none)\n";
    cout << "Unresolved merge conflicts:\n";
    for (const string& path : state_.merge.unresolvedPaths) cout << "  " << display(path) << '\n';
    if (state_.merge.unresolvedPaths.empty()) cout << "  (none)\n";
    if (state_.merge.active) cout << "Merge in progress. Stage each resolution, then commit; or run 'minigit merge --abort'.\n";
}

void Repository::commit(const string& message) {
    bool hasNonWhitespace = false;
    for (unsigned char character : message) {
        if (!isspace(character)) hasNonWhitespace = true;
    }
    if (!hasNonWhitespace || message.size() > MAX_MESSAGE_BYTES) throw Error("Commit message must contain non-whitespace text and be at most 65536 bytes.");
    if (!state_.merge.unresolvedPaths.empty()) throw Error("Cannot commit: unresolved merge conflicts remain. Stage every resolution with 'minigit add'.");
    if (!state_.merge.active && indexSnapshot() == headSnapshot()) throw Error("Nothing staged to commit.");
    Commit newCommit;
    newCommit.snapshotId = state_.indexId;
    newCommit.timestamp = static_cast<uint64_t>(system_clock::to_time_t(system_clock::now()));
    newCommit.message = message;
    if (state_.merge.active) {
        newCommit.parentCommitIds.push_back(state_.merge.ours);
        newCommit.parentCommitIds.push_back(state_.merge.theirs);
    } else if (!headId().empty()) {
        newCommit.parentCommitIds.push_back(headId());
    }
    validateHistoryExtension(commitGraph_, newCommit);
    string newCommitId = objects_.putCommit(newCommit);
    State nextState = state_;
    nextState.branches[state_.currentBranch] = newCommitId;
    nextState.merge = MergeState{};
    saveState(nextState);
    commitGraph_[newCommitId] = newCommit;
    cout << "Committed " << newCommitId << " on " << state_.currentBranch << ": " << display(message) << '\n';
}

void Repository::log() const {
    if (headId().empty()) {
        cout << "No commits yet.\n";
        return;
    }
    for (const string& id : reachableOrder(commitGraph_, headId())) {
        const Commit& commit = commitGraph_.at(id);
        cout << "commit " << id << "\nparents:";
        if (commit.parentCommitIds.empty()) cout << " (root)";
        for (const string& parentId : commit.parentCommitIds) cout << ' ' << parentId;
        cout << "\ntimestamp: " << commit.timestamp << " (Unix seconds, UTC)\nmessage: " << display(commit.message) << "\n\n";
    }
}

void Repository::diff(bool staged) const {
    Snapshot before = staged ? headSnapshot() : indexSnapshot();
    Snapshot after = staged ? indexSnapshot() : workingSnapshot(before);
    bool changed = false;
    for (const string& path : snapshotPaths(before, after)) {
        string oldVersion = versionAt(before, path);
        string newVersion = versionAt(after, path);
        if (oldVersion == newVersion) continue;
        changed = true;
        string oldBytes;
        string newBytes;
        if (!oldVersion.empty()) oldBytes = objects_.getBlob(oldVersion);
        if (!newVersion.empty()) {
            if (staged) newBytes = objects_.getBlob(newVersion);
            else newBytes = readFile(root_ / path, MAX_FILE_BYTES);
        }
        cout << "diff " << display(path) << "\n--- ";
        if (oldVersion.empty()) cout << "(absent)";
        else cout << (staged ? "HEAD/" : "index/") << display(path);
        cout << "\n+++ ";
        if (newVersion.empty()) cout << "(absent)";
        else cout << (staged ? "index/" : "working/") << display(path);
        cout << '\n';
        printTextDiff(cout, oldBytes, newBytes, !oldVersion.empty(), !newVersion.empty());
    }
    if (!changed) cout << "No differences.\n";
}

void Repository::restore(const string& argument, const string& source) {
    FilePath selectedPath = resolvePath(argument);
    if (selectedPath == root_) throw Error("Restore expects one file path, not a directory.");
    string path = selectedPath.lexically_relative(root_).generic_string();
    Snapshot index = indexSnapshot();
    Snapshot selected = index;
    if (!source.empty()) {
        string sourceId = source;
        if (state_.branches.count(source) != 0) sourceId = branchTip(source);
        if (sourceId.empty()) throw Error("Source branch has no commit.");
        validateId(sourceId);
        CommitGraph sourceHistory = loadHistory(objects_, {sourceId});
        selected = objects_.getSnapshot(sourceHistory.at(sourceId).snapshotId);
    }
    string targetVersion = versionAt(selected, path);
    string trackedVersion = versionAt(index, path);
    if (trackedVersion.empty() && state_.merge.active) trackedVersion = versionAt(objects_.getSnapshot(state_.merge.ownedSnapshotId), path);
    if (targetVersion.empty() && (source.empty() || trackedVersion.empty())) throw Error("Path is absent from the selected snapshot: " + display(path));
    Snapshot owned;
    Snapshot target;
    if (!trackedVersion.empty()) owned[path] = trackedVersion;
    if (!targetVersion.empty()) target[path] = targetVersion;
    State nextState = state_;
    if (state_.merge.active && state_.merge.unresolvedPaths.count(path) != 0 && !targetVersion.empty()) {
        Snapshot mergeOwned = objects_.getSnapshot(state_.merge.ownedSnapshotId);
        mergeOwned[path] = targetVersion;
        nextState.merge.ownedSnapshotId = objects_.putSnapshot(mergeOwned);
    }
    transitionWorktree(root_, objects_, owned, target, nextState, {path});
    state_ = nextState;
    cout << "Restored " << display(path) << "; index unchanged.\n";
}

string Repository::branchTip(const string& name) const {
    validateBranchName(name);
    map<string, string>::const_iterator branch = state_.branches.find(name);
    if (branch == state_.branches.end()) throw Error("Unknown branch: " + name);
    return branch->second;
}

void Repository::branches() const {
    for (const pair<const string, string>& branch : state_.branches) {
        cout << (branch.first == state_.currentBranch ? "* " : "  ") << branch.first << " ";
        if (branch.second.empty()) cout << "(unborn)";
        else cout << branch.second;
        cout << '\n';
    }
}

void Repository::createBranch(const string& name) {
    validateBranchName(name);
    if (state_.merge.active) throw Error("Finish or abort the merge before changing branches.");
    if (headId().empty()) throw Error("Create the first commit before creating another branch.");
    if (state_.branches.count(name) != 0) throw Error("Branch already exists: " + name);
    if (state_.branches.size() >= MAX_ENTRIES) throw Error("Branch limit reached: at most 20000 branches.");
    State nextState = state_;
    nextState.branches[name] = headId();
    saveState(nextState);
    cout << "Created branch " << name << ".\n";
}
void Repository::deleteBranch(const string& name) {
    if (state_.merge.active) throw Error("Finish or abort the merge before changing branches.");
    string tip = branchTip(name);
    if (name == state_.currentBranch) throw Error("Cannot delete the current branch.");
    if (!isAncestor(commitGraph_, tip, headId())) throw Error("Deletion would discard unmerged history. Merge the branch first.");
    State nextState = state_;
    nextState.branches.erase(name);
    saveState(nextState);
    cout << "Deleted branch " << name << ".\n";
}

void Repository::requireClean() const {
    Snapshot index = indexSnapshot();
    if (index != headSnapshot()) throw Error("Staged tracked changes prevent this operation. Commit them first.");
    if (workingSnapshot(index) != index) throw Error("Unstaged tracked changes prevent this operation. Commit or restore them first.");
}

void Repository::switchBranch(const string& name) {
    if (state_.merge.active) throw Error("Cannot switch during an unfinished merge. Commit or abort it first.");
    string destinationId = branchTip(name);
    requireClean();
    if (name == state_.currentBranch) {
        cout << "Already on branch " << name << ".\n";
        return;
    }
    Snapshot target;
    if (!destinationId.empty()) target = objects_.getSnapshot(commitGraph_.at(destinationId).snapshotId);
    State nextState = state_;
    nextState.currentBranch = name;
    nextState.indexId = objects_.putSnapshot(target);
    transitionWorktree(root_, objects_, indexSnapshot(), target, nextState);
    state_ = nextState;
    cout << "Switched to branch " << name << ".\n";
}

void Repository::mergeBase(const string& first, const string& second) const {
    string firstId = branchTip(first);
    string secondId = branchTip(second);
    if (firstId.empty() || secondId.empty()) throw Error("Merge-base requires two branches with commits.");
    vector<string> best = bestCommonAncestors(commitGraph_, firstId, secondId);
    if (best.empty()) throw Error("Unrelated histories: no common ancestor.");
    for (const string& id : best) cout << id << '\n';
}

static void reportMergeConflicts(const MergeResult& result, const string& theirId) {
    for (const string& path : result.conflicts) {
        if (result.binaryConflicts.count(path) != 0) {
            cout << "Binary conflict " << display(path) << ": kept ours (including deletion). To use theirs: minigit restore --source " << theirId << " -- " << quoteShellArgument(path) << "\n";
        } else cout << "Text conflict " << display(path) << ": edit the ours/theirs markers.\n";
    }
    cout << "Stage every resolved file or deletion with 'minigit add', then commit. To cancel: minigit merge --abort.\n";
}

void Repository::merge(const string& name) {
    if (state_.merge.active) throw Error("A merge is already in progress. Resolve it or run 'minigit merge --abort'.");
    string theirId = branchTip(name);
    if (headId().empty() || theirId.empty()) throw Error("Merge requires branches with commits.");
    requireClean();
    string ourId = headId();
    if (isAncestor(commitGraph_, theirId, ourId)) {
        cout << "Already up to date.\n";
        return;
    }
    Snapshot ours = indexSnapshot();
    Snapshot theirs = objects_.getSnapshot(commitGraph_.at(theirId).snapshotId);
    if (isAncestor(commitGraph_, ourId, theirId)) {
        State nextState = state_;
        nextState.branches[state_.currentBranch] = theirId;
        nextState.indexId = commitGraph_.at(theirId).snapshotId;
        transitionWorktree(root_, objects_, ours, theirs, nextState);
        state_ = nextState;
        cout << "Fast-forwarded " << state_.currentBranch << " to " << theirId << ".\n";
        return;
    }
    vector<string> bases = bestCommonAncestors(commitGraph_, ourId, theirId);
    if (bases.empty()) throw Error("Unrelated histories: no common ancestor. Merge refused.");
    if (bases.size() != 1) {
        string message = "Multiple best common ancestors; this educational version refuses automatic merging before making changes:";
        for (const string& id : bases) message += " " + id;
        throw Error(message);
    }
    Snapshot base = objects_.getSnapshot(commitGraph_.at(bases[0]).snapshotId);
    MergeResult result = compareThreeSnapshots(objects_, base, ours, theirs);
    State nextState = state_;
    nextState.indexId = objects_.putSnapshot(result.stagedFiles);
    if (result.conflicts.empty()) {
        Commit mergedCommit;
        mergedCommit.snapshotId = nextState.indexId;
        mergedCommit.parentCommitIds = {ourId, theirId};
        mergedCommit.timestamp = static_cast<uint64_t>(system_clock::to_time_t(system_clock::now()));
        mergedCommit.message = "Merge branch " + name + " into " + state_.currentBranch;
        validateHistoryExtension(commitGraph_, mergedCommit);
        string mergedId = objects_.putCommit(mergedCommit);
        nextState.branches[state_.currentBranch] = mergedId;
        transitionWorktree(root_, objects_, ours, result.workingFiles, nextState);
        state_ = nextState;
        commitGraph_[mergedId] = mergedCommit;
        cout << "Merged with two-parent commit " << mergedId << ".\n";
    } else {
        nextState.merge.active = true;
        nextState.merge.ours = ourId;
        nextState.merge.theirs = theirId;
        nextState.merge.base = bases[0];
        nextState.merge.originalIndexId = state_.indexId;
        nextState.merge.ownedSnapshotId = objects_.putSnapshot(result.workingFiles);
        nextState.merge.unresolvedPaths = result.conflicts;
        transitionWorktree(root_, objects_, ours, result.workingFiles, nextState);
        state_ = nextState;
        // Starting a conflicted merge succeeds; unresolved paths persist for status/add.
        reportMergeConflicts(result, theirId);
    }
}

void Repository::abortMerge() {
    if (!state_.merge.active) throw Error("No merge is in progress.");
    Snapshot owned = objects_.getSnapshot(state_.merge.ownedSnapshotId);
    Snapshot original = objects_.getSnapshot(state_.merge.originalIndexId);
    State nextState = state_;
    nextState.indexId = state_.merge.originalIndexId;
    nextState.merge = MergeState{};
    transitionWorktree(root_, objects_, owned, original, nextState, snapshotPaths(owned, original));
    state_ = nextState;
    cout << "Merge aborted; original tracked snapshot and index restored.\n";
}
}
