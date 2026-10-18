#include "minigit/storage.hpp"
#include "picosha2.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>

namespace minigit {
using namespace std;
using namespace std::filesystem;
using namespace std::chrono;

void validateId(const string& id) {
    if (id.size() != 64) {
        throw Error("Invalid object ID: expected 64 lowercase hexadecimal characters.");
    }
    for (char character : id) {
        if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
            throw Error("Invalid hexadecimal object ID.");
        }
    }
}

bool isMetadataName(const string& name) {
    string lowercase = name;
    for (char& character : lowercase) {
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    }
    return lowercase == ".git" || lowercase == ".minigit";
}

void validateRelativePath(const string& path) {
    if (path.empty() || path.size() > MAX_PATH_BYTES || path.front() == '/' || path.find('\0') != string::npos || path.find('\\') != string::npos) {
        throw Error("Invalid repository path: " + display(path));
    }
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find('/', start);
        if (end == string::npos) end = path.size();
        string component = path.substr(start, end - start);
        if (component.empty() || component == "." || component == ".." || isMetadataName(component)) {
            throw Error("Unsafe or non-normalized repository path: " + display(path));
        }
        if (end == path.size()) break;
        start = end + 1;
    }
}

string versionAt(const Snapshot& snapshot, const string& path) {
    Snapshot::const_iterator entry = snapshot.find(path);
    if (entry == snapshot.end()) return "";
    return entry->second;
}
void validateSnapshot(const Snapshot& snapshot) {
    if (snapshot.size() > MAX_ENTRIES) {
        throw Error("Snapshot has too many files.");
    }
    for (const pair<const string, string>& entry : snapshot) {
        validateRelativePath(entry.first);
        validateId(entry.second);
        FilePath parent = FilePath(entry.first).parent_path();
        while (!parent.empty()) {
            if (snapshot.count(parent.generic_string()) != 0) {
                throw Error("File/directory collision in snapshot: " + display(entry.first));
            }
            parent = parent.parent_path();
        }
    }
}

void validateBranchName(const string& name) {
    if (name.empty() || name.size() > 64 || name.front() == '-' || name == "." || name == "..") {
        throw Error("Invalid branch name. Use 1-64 ASCII letters, digits, '.', '_' or '-', starting with a letter or digit.");
    }
    for (char character : name) {
        if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') || character == '.' || character == '_' || character == '-')) {
            throw Error("Invalid branch name.");
        }
    }
    char first = name.front();
    if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || (first >= '0' && first <= '9'))) {
        throw Error("Branch names must start with a letter or digit.");
    }
}

string display(const string& value) {
    ostringstream output;
    output << '"';
    for (unsigned char character : value) {
        if (character == '\n') output << "\\n";
        else if (character == '\r') output << "\\r";
        else if (character == '\t') output << "\\t";
        else if (character == '"' || character == '\\') output << '\\' << character;
        else if (character < 32 || character == 127) output << "\\x" << hex << setw(2) << setfill('0') << static_cast<unsigned int>(character) << dec;
        else output << character;
    }
    output << '"';
    return output.str();
}

string quoteShellArgument(const string& value) {
    string quoted = "'";
    for (char character : value) {
        // Leave single quotes only to emit a literal quote, then reopen them.
        if (character == '\'') quoted += "'\"'\"'";
        else quoted += character;
    }
    quoted += '\'';
    return quoted;
}

void Encoder::number(uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) bytes.push_back(static_cast<char>((value >> shift) & 255));
}
void Encoder::field(const string& value) {
    number(value.size());
    bytes += value;
}
Decoder::Decoder(const string& bytes) : bytes_(bytes) {}
uint64_t Decoder::number() {
    if (bytes_.size() - position_ < 8) {
        throw Error("Truncated data: missing integer.");
    }
    uint64_t value = 0;
    for (int index = 0; index < 8; ++index) value = (value << 8) | static_cast<unsigned char>(bytes_[position_++]);
    return value;
}
size_t Decoder::count(size_t limit) {
    uint64_t value = number();
    if (value > limit) {
        throw Error("Malformed length/count exceeds format limit.");
    }
    return static_cast<size_t>(value);
}
string Decoder::field(size_t limit) {
    size_t length = count(limit);
    if (length > bytes_.size() - position_) {
        throw Error("Truncated length-prefixed field.");
    }
    string result = bytes_.substr(position_, length);
    position_ += length;
    return result;
}
void Decoder::finish() const {
    if (position_ != bytes_.size()) {
        throw Error("Malformed data: unexpected trailing bytes.");
    }
}

string envelope(const string& type, const string& payload) {
    Encoder encoder;
    encoder.field("MiniGit");
    encoder.number(1);
    encoder.field(type);
    encoder.field(payload);
    return encoder.bytes;
}
string unpack(const string& bytes, const string& expectedType) {
    Decoder decoder(bytes);
    if (decoder.field(7) != "MiniGit") {
        throw Error("Invalid MiniGit format signature.");
    }
    if (decoder.number() != 1) {
        throw Error("Unsupported MiniGit format version.");
    }
    if (decoder.field(32) != expectedType) {
        throw Error("Incorrect object/metadata type; expected " + expectedType + ".");
    }
    string payload = decoder.field(MAX_TRANSACTION_BYTES);
    decoder.finish();
    return payload;
}
string sha256(const string& bytes) {
    return picosha2::hash256_hex_string(bytes);
}

string encodeState(const State& state) {
    Encoder encoder;
    encoder.field(state.currentBranch);
    encoder.number(state.branches.size());
    for (const pair<const string, string>& branch : state.branches) {
        encoder.field(branch.first);
        encoder.field(branch.second);
    }
    encoder.field(state.indexId);
    encoder.number(state.merge.active ? 1 : 0);
    if (state.merge.active) {
        encoder.field(state.merge.ours);
        encoder.field(state.merge.theirs);
        encoder.field(state.merge.base);
        encoder.field(state.merge.originalIndexId);
        encoder.field(state.merge.ownedSnapshotId);
        encoder.number(state.merge.unresolvedPaths.size());
        for (const string& path : state.merge.unresolvedPaths) encoder.field(path);
    }
    string bytes = envelope("state", encoder.bytes);
    if (bytes.size() > MAX_STATE_BYTES) {
        throw Error("Repository state exceeds size limit.");
    }
    return bytes;
}
State decodeState(const string& bytes) {
    string payload = unpack(bytes, "state");
    Decoder decoder(payload);
    State state;
    state.currentBranch = decoder.field(64);
    validateBranchName(state.currentBranch);
    size_t branchCount = decoder.count(MAX_ENTRIES);
    string previous;
    for (size_t index = 0; index < branchCount; ++index) {
        string name = decoder.field(64);
        string id = decoder.field(64);
        validateBranchName(name);
        if (!id.empty()) validateId(id);
        if (index != 0 && name <= previous) {
            throw Error("Branch names must be unique and sorted.");
        }
        state.branches[name] = id;
        previous = name;
    }
    if (state.branches.count(state.currentBranch) == 0) {
        throw Error("HEAD refers to a missing branch.");
    }
    state.indexId = decoder.field(64);
    validateId(state.indexId);
    state.merge.active = decoder.count(1) == 1;
    if (state.merge.active) {
        state.merge.ours = decoder.field(64);
        state.merge.theirs = decoder.field(64);
        state.merge.base = decoder.field(64);
        state.merge.originalIndexId = decoder.field(64);
        state.merge.ownedSnapshotId = decoder.field(64);
        validateId(state.merge.ours);
        validateId(state.merge.theirs);
        validateId(state.merge.base);
        validateId(state.merge.originalIndexId);
        validateId(state.merge.ownedSnapshotId);
        size_t unresolvedCount = decoder.count(MAX_ENTRIES);
        previous.clear();
        for (size_t index = 0; index < unresolvedCount; ++index) {
            string path = decoder.field(MAX_PATH_BYTES);
            validateRelativePath(path);
            if (index != 0 && path <= previous) {
                throw Error("Conflict paths must be unique and sorted.");
            }
            state.merge.unresolvedPaths.insert(path);
            previous = path;
        }
        if (state.branches.at(state.currentBranch) != state.merge.ours) {
            throw Error("Merge state does not match HEAD.");
        }
    }
    decoder.finish();
    return state;
}

bool pathExists(const FilePath& path) {
    error_code error;
    file_status status = symlink_status(path, error);
    if (error == errc::no_such_file_or_directory || error == errc::not_a_directory) return false;
    if (error) {
        throw Error("Cannot inspect " + display(path.string()) + ": " + error.message());
    }
    return status.type() != file_type::not_found;
}
void checkSafeComponents(const FilePath& path) {
    FilePath current;
    for (const FilePath& component : absolute(path)) {
        current /= component;
        error_code error;
        file_status status = symlink_status(current, error);
        if (error == errc::no_such_file_or_directory || error == errc::not_a_directory) continue;
        if (error) {
            throw Error("Cannot inspect path component " + display(current.string()) + ": " + error.message());
        }
        if (is_symlink(status)) {
            throw Error("Symlinks are unsupported: " + display(current.string()));
        }
    }
}
string readFile(const FilePath& path, size_t limit) {
    checkSafeComponents(path);
    if (!pathExists(path)) {
        throw Error("Missing file or object: " + display(path.string()));
    }
    if (!is_regular_file(symlink_status(path))) {
        throw Error("Expected a regular file: " + display(path.string()));
    }
    uintmax_t size = file_size(path);
    if (size > limit) {
        throw Error("File/object exceeds size limit: " + display(path.string()));
    }
    ifstream input(path, ios::binary);
    if (!input) {
        throw Error("Cannot open for reading: " + display(path.string()));
    }
    string bytes(static_cast<size_t>(size), '\0');
    if (size != 0) input.read(&bytes[0], static_cast<streamsize>(size));
    if (!input || input.peek() != char_traits<char>::eof()) {
        throw Error("Read failed or file changed during reading: " + display(path.string()));
    }
    return bytes;
}

void writeAtomic(const FilePath& path, const string& bytes) {
    checkSafeComponents(path);
    if (pathExists(path) && !is_regular_file(symlink_status(path))) {
        throw Error("Cannot replace a non-regular file: " + display(path.string()));
    }
    random_device random;
    FilePath temporaryPath = path.parent_path() / (".minigit-tmp-" + to_string(random()) + "-" + to_string(random()));
    if (pathExists(temporaryPath)) {
        throw Error("Temporary path unexpectedly exists; retry the command.");
    }
    try {
        ofstream output(temporaryPath, ios::binary | ios::trunc);
        if (!output) {
            throw Error("Cannot create temporary file for " + display(path.string()));
        }
        output.write(bytes.data(), static_cast<streamsize>(bytes.size()));
        output.flush();
        if (!output) {
            throw Error("Write failed for " + display(path.string()));
        }
        output.close();
        if (!output) {
            throw Error("Close failed for " + display(path.string()));
        }
        rename(temporaryPath, path);
    } catch (...) {
        error_code cleanupError;
        remove(temporaryPath, cleanupError);
        throw;
    }
}

void failPoint(const string& name) {
    const char* crashPoint = getenv("MINIGIT_TEST_CRASH_POINT");
    // Integration tests use an abrupt exit to exercise persisted recovery.
    if (crashPoint != nullptr && name == crashPoint) _Exit(86);
    const char* requested = getenv("MINIGIT_TEST_FAIL_POINT");
    if (requested != nullptr && name == requested) {
        throw Error("Simulated I/O failure at " + name + ".");
    }
}

RepositoryLock::RepositoryLock(const FilePath& metadataPath) : lockPath_(metadataPath / "lock") {
    checkSafeComponents(lockPath_);
    if (!create_directory(lockPath_)) {
        throw Error("Repository is locked. If the owning process has exited, remove .minigit/lock manually, then run 'minigit recover' if a transaction exists.");
    }
    try {
        writeAtomic(lockPath_ / "owner", "MiniGit command started at Unix seconds " + to_string(system_clock::to_time_t(system_clock::now())) + "\nDo not remove this directory while a MiniGit command is running.\n");
    } catch (...) {
        error_code cleanupError;
        remove_all(lockPath_, cleanupError);
        throw;
    }
}
RepositoryLock::~RepositoryLock() {
    error_code error;
    remove(lockPath_ / "owner", error);
    if (!error) remove(lockPath_, error);
    if (error) cerr << "Warning: lock cleanup failed: " << error.message() << '\n';
}

ObjectStore::ObjectStore(const FilePath& metadataPath) : objectsPath_(metadataPath / "objects") {
    checkSafeComponents(objectsPath_);
    if (!is_directory(objectsPath_)) {
        throw Error("Missing objects directory.");
    }
}
string ObjectStore::put(const string& type, const string& payload) {
    string bytes = envelope(type, payload);
    if (bytes.size() > MAX_OBJECT_BYTES) {
        throw Error("Object exceeds size limit.");
    }
    string id = sha256(bytes);
    FilePath path = objectsPath_ / id;
    if (pathExists(path)) {
        if (readFile(path, MAX_OBJECT_BYTES) != bytes) {
            throw Error("Existing object is corrupt: " + id);
        }
    } else {
        failPoint("before-object-write");
        writeAtomic(path, bytes);
    }
    return id;
}
string ObjectStore::get(const string& id, const string& type) const {
    validateId(id);
    string bytes = readFile(objectsPath_ / id, MAX_OBJECT_BYTES);
    if (sha256(bytes) != id) {
        throw Error("Object hash mismatch: " + id);
    }
    return unpack(bytes, type);
}
string ObjectStore::putBlob(const string& bytes) {
    if (bytes.size() > MAX_FILE_BYTES) {
        throw Error("Blob exceeds 16 MiB limit.");
    }
    return put("blob", bytes);
}
string ObjectStore::getBlob(const string& id) const {
    string bytes = get(id, "blob");
    if (bytes.size() > MAX_FILE_BYTES) {
        throw Error("Blob exceeds 16 MiB limit.");
    }
    return bytes;
}
string ObjectStore::putSnapshot(const Snapshot& snapshot) {
    validateSnapshot(snapshot);
    Encoder encoder;
    encoder.number(snapshot.size());
    for (const pair<const string, string>& entry : snapshot) {
        encoder.field(entry.first);
        encoder.field(entry.second);
    }
    return put("snapshot", encoder.bytes);
}
Snapshot ObjectStore::getSnapshot(const string& id) const {
    set<string> validatedBlobIds;
    return getSnapshot(id, validatedBlobIds);
}
Snapshot ObjectStore::getSnapshot(const string& id, set<string>& validatedBlobIds) const {
    string payload = get(id, "snapshot");
    Decoder decoder(payload);
    size_t fileCount = decoder.count(MAX_ENTRIES);
    Snapshot snapshot;
    string previous;
    for (size_t index = 0; index < fileCount; ++index) {
        string path = decoder.field(MAX_PATH_BYTES);
        string blobId = decoder.field(64);
        if (index != 0 && path <= previous) {
            throw Error("Snapshot paths must be unique and sorted.");
        }
        snapshot[path] = blobId;
        previous = path;
    }
    decoder.finish();
    validateSnapshot(snapshot);
    // Validate referenced blobs too; a valid snapshot hash alone is not enough.
    for (const pair<const string, string>& entry : snapshot) {
        if (validatedBlobIds.count(entry.second) == 0) {
            getBlob(entry.second);
            // Never cache a failed hash, type, or size check.
            validatedBlobIds.insert(entry.second);
        }
    }
    return snapshot;
}
string ObjectStore::putCommit(const Commit& commit) {
    validateId(commit.snapshotId);
    if (commit.parentCommitIds.size() > 2) {
        throw Error("A commit may have at most two parents.");
    }
    if (commit.message.empty() || commit.message.size() > MAX_MESSAGE_BYTES) {
        throw Error("Commit message must contain 1-65536 bytes.");
    }
    Encoder encoder;
    encoder.field(commit.snapshotId);
    encoder.number(commit.timestamp);
    encoder.number(commit.parentCommitIds.size());
    for (const string& parent : commit.parentCommitIds) {
        validateId(parent);
        encoder.field(parent);
    }
    encoder.field(commit.message);
    return put("commit", encoder.bytes);
}
Commit ObjectStore::getCommit(const string& id) const {
    string payload = get(id, "commit");
    Decoder decoder(payload);
    Commit commit;
    commit.snapshotId = decoder.field(64);
    validateId(commit.snapshotId);
    commit.timestamp = decoder.number();
    if (commit.timestamp > 253402300799ULL) {
        throw Error("Commit timestamp is out of supported range.");
    }
    size_t parentCount = decoder.count(2);
    for (size_t index = 0; index < parentCount; ++index) {
        string parentId = decoder.field(64);
        validateId(parentId);
        if (parentId == id || find(commit.parentCommitIds.begin(), commit.parentCommitIds.end(), parentId) != commit.parentCommitIds.end()) {
            throw Error("Invalid self/duplicate parent reference.");
        }
        commit.parentCommitIds.push_back(parentId);
    }
    commit.message = decoder.field(MAX_MESSAGE_BYTES);
    if (commit.message.empty()) {
        throw Error("Commit has an empty message.");
    }
    decoder.finish();
    return commit;
}
}
