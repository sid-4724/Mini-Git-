#pragma once

#include "minigit/types.hpp"

namespace minigit {

class Encoder {
public:
    string bytes;
    void number(uint64_t value);
    void field(const string& value);
};

class Decoder {
public:
    explicit Decoder(const string& bytes);
    uint64_t number();
    size_t count(size_t limit);
    string field(size_t limit);
    void finish() const;
private:
    const string& bytes_;
    size_t position_ = 0;
};

string envelope(const string& type, const string& payload);
string unpack(const string& bytes, const string& expectedType);
string sha256(const string& bytes);
string encodeState(const State& state);
State decodeState(const string& bytes);

void checkSafeComponents(const FilePath& path);
bool pathExists(const FilePath& path);
string readFile(const FilePath& path, size_t limit);
void writeAtomic(const FilePath& path, const string& bytes);
void failPoint(const string& name);

class RepositoryLock {
public:
    explicit RepositoryLock(const FilePath& metadataPath);
    ~RepositoryLock();
    RepositoryLock(const RepositoryLock&) = delete;
    RepositoryLock& operator=(const RepositoryLock&) = delete;
private:
    FilePath lockPath_;
};

class ObjectStore {
public:
    explicit ObjectStore(const FilePath& metadataPath);
    string putBlob(const string& bytes);
    string getBlob(const string& id) const;
    string putSnapshot(const Snapshot& snapshot);
    Snapshot getSnapshot(const string& id) const;
    // Share successes only within one validation pass over immutable objects.
    Snapshot getSnapshot(const string& id, set<string>& validatedBlobIds) const;
    string putCommit(const Commit& commit);
    Commit getCommit(const string& id) const;
private:
    FilePath objectsPath_;
    string put(const string& type, const string& payload);
    string get(const string& id, const string& type) const;
};
}
