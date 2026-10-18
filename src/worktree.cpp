#include "minigit/worktree.hpp"
#include "minigit/graph.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace minigit {
using namespace std;
using namespace std::filesystem;

namespace {

struct FileChange {
    string path;
    bool existedBefore = false;
    string before;
    uint64_t permissionsBefore = 0;
    bool existsAfter = false;
    string after;
};

struct Journal {
    string oldState;
    string newState;
    vector<FileChange> changes;
    vector<string> createdDirectories;
    vector<string> removedDirectories;
};

bool deeperFirst(const string& first, const string& second) {
    size_t firstDepth = count(first.begin(), first.end(), '/');
    size_t secondDepth = count(second.begin(), second.end(), '/');
    if (firstDepth != secondDepth) return firstDepth > secondDepth;
    return first > second;
}

string encodeJournal(const Journal& journal) {
    if (journal.changes.size() > MAX_ENTRIES * 2 || journal.createdDirectories.size() > MAX_ENTRIES * 2 || journal.removedDirectories.size() > MAX_ENTRIES * 2) throw Error("Operation exceeds the recovery journal entry limit.");
    Encoder encoder;
    encoder.field(journal.oldState);
    encoder.field(journal.newState);
    encoder.number(journal.changes.size());
    for (const FileChange& change : journal.changes) {
        encoder.field(change.path);
        encoder.number(change.existedBefore ? 1 : 0);
        encoder.field(change.before);
        encoder.number(change.permissionsBefore);
        encoder.number(change.existsAfter ? 1 : 0);
        encoder.field(change.after);
        if (encoder.bytes.size() > MAX_TRANSACTION_BYTES) throw Error("Operation exceeds the 64 MiB recovery journal limit; use smaller snapshots.");
    }
    encoder.number(journal.createdDirectories.size());
    for (const string& path : journal.createdDirectories) encoder.field(path);
    encoder.number(journal.removedDirectories.size());
    for (const string& path : journal.removedDirectories) encoder.field(path);
    // Only journals use v2. Content-addressed objects and state remain v1.
    Encoder record;
    record.field("MiniGit");
    record.number(2);
    record.field("transaction");
    record.field(encoder.bytes);
    record.field(sha256(encoder.bytes));
    string bytes = record.bytes;
    if (bytes.size() > MAX_TRANSACTION_BYTES) throw Error("Recovery journal exceeds the 64 MiB limit.");
    return bytes;
}

Journal decodeJournal(const string& bytes) {
    Decoder record(bytes);
    if (record.field(7) != "MiniGit") throw Error("Invalid MiniGit journal signature.");
    uint64_t version = record.number();
    if (version == 1) {
        throw Error("Legacy v1 journal has no checksum; automatic recovery refused. Journal retained. Back up the entire repository, including working files and .minigit, before manual recovery. See README.md, 'Legacy journal recovery'.");
    }
    if (version != 2) throw Error("Unsupported transaction journal version; journal retained.");
    if (record.field(32) != "transaction") throw Error("Incorrect journal metadata type.");
    string payload = record.field(MAX_TRANSACTION_BYTES);
    string checksum = record.field(64);
    record.finish();
    if (checksum != sha256(payload)) {
        throw Error("Recovery journal checksum mismatch; working files and journal left unchanged. Back up the repository before manual repair.");
    }
    Decoder decoder(payload);
    Journal journal;
    journal.oldState = decoder.field(MAX_STATE_BYTES);
    journal.newState = decoder.field(MAX_STATE_BYTES);
    decodeState(journal.oldState);
    decodeState(journal.newState);
    size_t changeCount = decoder.count(MAX_ENTRIES * 2);
    set<string> paths;
    for (size_t index = 0; index < changeCount; ++index) {
        FileChange change;
        change.path = decoder.field(MAX_PATH_BYTES);
        validateRelativePath(change.path);
        if (!paths.insert(change.path).second) throw Error("Duplicate journal path.");
        change.existedBefore = decoder.count(1) == 1;
        change.before = decoder.field(MAX_FILE_BYTES);
        change.permissionsBefore = decoder.number();
        if (change.permissionsBefore > 07777) throw Error("Invalid journal permissions.");
        change.existsAfter = decoder.count(1) == 1;
        change.after = decoder.field(MAX_FILE_BYTES);
        if ((!change.existedBefore && !change.before.empty()) || (!change.existsAfter && !change.after.empty())) throw Error("Malformed journal file state.");
        journal.changes.push_back(change);
    }
    size_t createdCount = decoder.count(MAX_ENTRIES * 2);
    paths.clear();
    for (size_t index = 0; index < createdCount; ++index) {
        string path = decoder.field(MAX_PATH_BYTES);
        validateRelativePath(path);
        if (!paths.insert(path).second) throw Error("Duplicate journal directory.");
        journal.createdDirectories.push_back(path);
    }
    size_t removedCount = decoder.count(MAX_ENTRIES * 2);
    paths.clear();
    for (size_t index = 0; index < removedCount; ++index) {
        string path = decoder.field(MAX_PATH_BYTES);
        validateRelativePath(path);
        if (!paths.insert(path).second) throw Error("Duplicate journal directory.");
        journal.removedDirectories.push_back(path);
    }
    decoder.finish();
    return journal;
}

void removeFile(const FilePath& path) {
    checkSafeComponents(path);
    if (!remove(path)) throw Error("Could not remove expected file: " + display(path.string()));
}

set<string> collectFileChanges(const FilePath& root, const ObjectStore& objects,
                                         const Snapshot& owned, const Snapshot& target,
                                         const set<string>& forceWrites,
                                         Journal& journal) {
    set<string> paths;
    for (const pair<const string, string>& entry : owned) paths.insert(entry.first);
    for (const pair<const string, string>& entry : target) paths.insert(entry.first);
    set<string> removableFiles;
    size_t backupBytes = journal.oldState.size() + journal.newState.size();
    for (const string& path : paths) {
        Snapshot::const_iterator oldEntry = owned.find(path);
        Snapshot::const_iterator newEntry = target.find(path);
        if (oldEntry != owned.end() && newEntry != target.end() && oldEntry->second == newEntry->second && forceWrites.count(path) == 0) continue;
        FilePath absolutePath = root / path;
        checkSafeComponents(absolutePath);
        FileChange change;
        change.path = path;
        if (pathExists(absolutePath) && is_regular_file(symlink_status(absolutePath))) {
            if (oldEntry == owned.end()) throw Error("Untracked file would be overwritten: " + display(path) + ". Move it aside first.");
            change.existedBefore = true;
            change.before = readFile(absolutePath, MAX_FILE_BYTES);
            change.permissionsBefore = static_cast<uint64_t>(status(absolutePath).permissions()) & 07777;
            removableFiles.insert(path);
        } else if (pathExists(absolutePath) && oldEntry != owned.end()) {
            throw Error("Tracked file was replaced by a directory or unsupported type: " + display(path) + ". Move it aside first.");
        }
        if (newEntry != target.end()) {
            change.existsAfter = true;
            change.after = objects.getBlob(newEntry->second);
        }
        backupBytes += change.before.size() + change.after.size() + path.size() + 64;
        if (backupBytes > MAX_TRANSACTION_BYTES) throw Error("Operation exceeds the 64 MiB recovery journal limit.");
        journal.changes.push_back(change);
    }

    return removableFiles;
}

void checkDestinationCollisions(const FilePath& root, const FileChange& change,
                                const set<string>& removableFiles,
                                set<string>& removedDirectories) {
    FilePath destination = root / change.path;
    if (pathExists(destination) && is_directory(symlink_status(destination))) {
        bool hasTrackedDescendant = false;
        for (const string& file : removableFiles) {
            if (file.compare(0, change.path.size() + 1, change.path + "/") == 0) hasTrackedDescendant = true;
        }
        if (!hasTrackedDescendant) throw Error("Untracked directory collision: " + display(change.path) + ". Move it aside first.");
        removedDirectories.insert(change.path);
        recursive_directory_iterator current(destination);
        recursive_directory_iterator end;
        while (current != end) {
            FilePath path = current->path();
            string relative = path.lexically_relative(root).generic_string();
            checkSafeComponents(path);
            file_status status = current->symlink_status();
            if (is_regular_file(status)) {
                if (removableFiles.count(relative) == 0) throw Error("Untracked file/directory collision: " + display(relative));
            } else if (is_directory(status)) {
                bool containsTrackedFile = false;
                for (const string& file : removableFiles) {
                    if (file.compare(0, relative.size() + 1, relative + "/") == 0) containsTrackedFile = true;
                }
                if (!containsTrackedFile) throw Error("Untracked directory collision: " + display(relative));
                removedDirectories.insert(relative);
            } else throw Error("Unsupported file/directory collision: " + display(relative));
            ++current;
        }
    } else if (pathExists(destination) && !is_regular_file(symlink_status(destination))) {
        throw Error("Unsupported destination type: " + display(change.path));
    }
}

void planDirectoryChanges(const FilePath& root, const set<string>& removableFiles,
                          Journal& journal) {
    set<string> createdDirectories;
    set<string> removedDirectories;
    // Validate every destination, including all parent components, before any write.
    for (const FileChange& change : journal.changes) {
        if (!change.existsAfter) continue;
        checkDestinationCollisions(root, change, removableFiles, removedDirectories);
        FilePath parent = FilePath(change.path).parent_path();
        while (!parent.empty()) {
            string relative = parent.generic_string();
            FilePath absolute = root / parent;
            checkSafeComponents(absolute);
            if (!pathExists(absolute)) createdDirectories.insert(relative);
            else if (!is_directory(symlink_status(absolute))) {
                if (removableFiles.count(relative) == 0) throw Error("Untracked parent file collision: " + display(relative));
                createdDirectories.insert(relative);
            }
            parent = parent.parent_path();
        }
    }
    journal.createdDirectories.assign(createdDirectories.begin(), createdDirectories.end());
    journal.removedDirectories.assign(removedDirectories.begin(), removedDirectories.end());
    sort(journal.createdDirectories.begin(), journal.createdDirectories.end(), deeperFirst);
    reverse(journal.createdDirectories.begin(), journal.createdDirectories.end());
    sort(journal.removedDirectories.begin(), journal.removedDirectories.end(), deeperFirst);
}

Journal planTransition(const FilePath& root, const ObjectStore& objects,
                       const Snapshot& owned, const Snapshot& target,
                       const State& nextState, const set<string>& forceWrites) {
    validateSnapshot(owned);
    validateSnapshot(target);
    Journal journal;
    journal.oldState = readFile(root / ".minigit/state", MAX_STATE_BYTES);
    journal.newState = encodeState(nextState);
    set<string> removableFiles = collectFileChanges(root, objects, owned, target, forceWrites, journal);
    planDirectoryChanges(root, removableFiles, journal);
    return journal;
}

void preflightRollback(const FilePath& root, const Journal& journal) {
    set<string> changedPaths;
    set<string> knownDirectories(journal.createdDirectories.begin(), journal.createdDirectories.end());
    knownDirectories.insert(journal.removedDirectories.begin(), journal.removedDirectories.end());
    for (const FileChange& change : journal.changes) {
        changedPaths.insert(change.path);
        FilePath path = root / change.path;
        checkSafeComponents(path);
        if (!pathExists(path)) continue;
        file_status status = symlink_status(path);
        if (is_regular_file(status)) {
            string contents = readFile(path, MAX_FILE_BYTES);
            bool isBefore = change.existedBefore && contents == change.before;
            bool isAfter = change.existsAfter && contents == change.after;
            if (!isBefore && !isAfter) throw Error("Recovery would overwrite new edits to " + display(change.path) + ". Back up and move those edits aside, then retry 'minigit recover'.");
        } else if (!is_directory(status) || knownDirectories.count(change.path) == 0) {
            throw Error("Unsafe recovery collision: " + display(change.path));
        }
    }
    for (const string& directory : journal.createdDirectories) {
        FilePath path = root / directory;
        checkSafeComponents(path);
        if (!pathExists(path) || !is_directory(symlink_status(path))) continue;
        recursive_directory_iterator current(path);
        recursive_directory_iterator end;
        while (current != end) {
            string relative = current->path().lexically_relative(root).generic_string();
            checkSafeComponents(current->path());
            if (current->is_directory()) {
                if (knownDirectories.count(relative) == 0) throw Error("Recovery would remove a new untracked directory: " + display(relative));
            } else if (!current->is_regular_file() || changedPaths.count(relative) == 0) {
                throw Error("Recovery would remove a new untracked file: " + display(relative));
            }
            ++current;
        }
    }
}

void rollback(const FilePath& root, const Journal& journal) {
    preflightRollback(root, journal);
    for (const FileChange& change : journal.changes) {
        FilePath path = root / change.path;
        if (pathExists(path) && is_regular_file(symlink_status(path))) removeFile(path);
    }
    vector<string> created = journal.createdDirectories;
    sort(created.begin(), created.end(), deeperFirst);
    for (const string& directory : created) {
        FilePath path = root / directory;
        if (pathExists(path) && is_directory(symlink_status(path))) removeFile(path);
    }
    vector<string> removed = journal.removedDirectories;
    reverse(removed.begin(), removed.end());
    for (const string& directory : removed) {
        checkSafeComponents(root / directory);
        if (!pathExists(root / directory)) create_directory(root / directory);
    }
    bool firstBackup = true;
    for (const FileChange& change : journal.changes) {
        if (change.existedBefore) {
            checkSafeComponents(root / change.path);
            writeAtomic(root / change.path, change.before);
            permissions(root / change.path, static_cast<perms>(change.permissionsBefore));
            if (firstBackup) {
                firstBackup = false;
                const char* requested = getenv("MINIGIT_TEST_ROLLBACK_FAIL_POINT");
                if (requested != nullptr && string(requested) == "after-first-backup-write") {
                    throw Error("Simulated rollback I/O failure after first backup write.");
                }
            }
        }
    }
    writeAtomic(root / ".minigit/state", journal.oldState);
    removeFile(root / ".minigit/transaction");
}
}

void validateFilesystemLayout(const FilePath& root, const Snapshot& snapshot) {
    validateSnapshot(snapshot);
    FilePath probe = root / ".minigit/lock/layout";
    checkSafeComponents(probe);
    if (!create_directory(probe)) throw Error("Layout validation directory already exists.");
    try {
        for (const pair<const string, string>& entry : snapshot) {
            FilePath path = probe / entry.first;
            create_directories(path.parent_path());
            if (pathExists(path)) throw Error("Filename collision on this filesystem: " + display(entry.first));
            writeAtomic(path, "");
        }
        remove_all(probe);
    } catch (...) {
        error_code cleanupError;
        remove_all(probe, cleanupError);
        throw;
    }
}

void transitionWorktree(const FilePath& root, const ObjectStore& objects,
                        const Snapshot& ownedFiles, const Snapshot& targetFiles,
                        const State& nextState, const set<string>& forceWrites) {
    validateFilesystemLayout(root, targetFiles);
    Journal journal = planTransition(root, objects, ownedFiles, targetFiles, nextState, forceWrites);
    FilePath journalPath = root / ".minigit/transaction";
    if (pathExists(journalPath)) throw Error("A recovery journal already exists. Run 'minigit recover'.");
    writeAtomic(journalPath, encodeJournal(journal));
    bool publishedState = false;
    try {
        failPoint("after-journal-write");
        for (const FileChange& change : journal.changes) {
            if (change.existedBefore) removeFile(root / change.path);
        }
        for (const string& directory : journal.removedDirectories) removeFile(root / directory);
        for (const string& directory : journal.createdDirectories) {
            checkSafeComponents(root / directory);
            create_directory(root / directory);
        }
        bool firstWrite = true;
        for (const FileChange& change : journal.changes) {
            if (change.existsAfter) writeAtomic(root / change.path, change.after);
            if (firstWrite) {
                firstWrite = false;
                failPoint("after-working-write");
            }
        }
        failPoint("before-state-write");
        writeAtomic(root / ".minigit/state", journal.newState);
        publishedState = true;
        failPoint("after-state-write");
        removeFile(journalPath);
    } catch (const exception& error) {
        string failure = error.what();
        if (publishedState) throw Error("Operation committed, but journal cleanup failed: " + failure + ". Run 'minigit recover'.");
        try {
            // Check the persisted backup before trusting it during rollback too.
            rollback(root, decodeJournal(readFile(journalPath, MAX_TRANSACTION_BYTES)));
        } catch (const exception& rollbackError) {
            throw Error("Operation failed: " + failure + ". Recovery is incomplete: " + rollbackError.what() + ". Journal retained; back up your files and run 'minigit recover'.");
        }
        throw Error("Operation failed and was rolled back: " + failure);
    }
}

void recoverTransaction(const FilePath& root) {
    FilePath path = root / ".minigit/transaction";
    if (!pathExists(path)) {
        cout << "No interrupted transaction to recover.\n";
        return;
    }
    Journal journal = decodeJournal(readFile(path, MAX_TRANSACTION_BYTES));
    ObjectStore objects(root / ".minigit");
    validateStateReferences(objects, decodeState(journal.oldState));
    if (journal.oldState != journal.newState) validateStateReferences(objects, decodeState(journal.newState));
    string currentState = readFile(root / ".minigit/state", MAX_STATE_BYTES);
    if (journal.oldState != journal.newState && currentState == journal.newState) {
        removeFile(path);
        cout << "Completed transaction was already published; recovery journal cleared.\n";
    } else if (currentState == journal.oldState) {
        rollback(root, journal);
        cout << "Interrupted transaction rolled back.\n";
    } else throw Error("Repository state matches neither side of the journal. Back up the repository before manual repair.");
}
}
