# Learning guide

## Read the source in this order

1. `include/minigit/types.hpp`: understand `Snapshot`, `Commit`, `State`, and `MergeState`.
2. `src/main.cpp`: see the small command grammar, root discovery, lock, and exception boundary.
3. `src/storage.cpp`: follow `Encoder`, `Decoder`, envelope/hash handling, `ObjectStore`, and atomic writes.
4. `src/repository.cpp`: start with `initializeRepository`, `add`, `status`, and `commit`, then read restore/branches.
5. `src/diff.cpp`: inspect tokenization, the limit check, suffix DP, and deterministic reconstruction.
6. `src/graph.cpp`: read loading/cycle validation, ancestor traversal, log order, and best common ancestors.
7. `src/merge.cpp`: trace the four file-version rules before reading merge coordination.
8. `src/worktree.cpp`: study planning, journaling, rollback, and crash recovery after the simpler data model is familiar.
9. Tests: choose a named CLI scenario, run it, then inspect how it verifies stored bytes/parents independently.

You can inspect the CLI without creating a repository:

```sh
./build/minigit --help
```

After building, run one test group:

```sh
ctest --test-dir build -R cli.merges --output-on-failure
```

Or run a single scenario:

```sh
python3 tests/test_cli.py build/minigit MergeTests.test_text_conflict_requires_explicit_staging_and_original_parents
```

Each scenario gets a new temporary directory. The executable path is made absolute before commands change directories. Test assertions remain active in Release.

## Trace a concrete command

Suppose a repository contains `notes/chapter.txt`, and you run:

```sh
minigit add notes/chapter.txt
```

Follow these actual functions:

1. `main` separates the command and its arguments. `pathArgument` accepts one path or `--` followed by one path.
2. `findRepository` walks from the current directory toward the filesystem root looking for `.minigit`.
3. `RepositoryLock` atomically creates the lock directory. Its local lifetime spans the command.
4. `Repository::Repository` decodes state and calls `validateStateReferences`, which loads/validates referenced snapshots and the parent graph.
5. `Repository::add` calls `resolvePath`: reject outside/traversal/symlink paths, normalize the relative path, and find its existing filesystem spelling. Exact filenames win; two hard links are distinct names. A non-exact alias with multiple candidates is an error.
6. `indexSnapshot` loads the **complete** existing index. Other indexed files remain in it.
7. `readFile` opens chapter bytes in binary mode with the size limit. `putBlob` calls `ObjectStore::put`, `envelope`, and `sha256`, then writes the immutable object if absent.
8. `add` replaces the chapter's blob ID in the map, handles any scoped tracked deletion/conflict resolution, and calls `validateFilesystemLayout`.
9. `putSnapshot` serializes sorted path/ID pairs and stores their object.
10. `saveState` writes a temporary state file and renames it atomically. The branch tip is unchanged; only the index and possibly merge-resolution bookkeeping change.
11. Exiting the scope releases the lock through its destructor.

Now edit chapter again and run `minigit commit -m "Chapter draft"`. `commit` uses `state_.indexId`, not the new working bytes. The new commit object is stored first, and the current branch advances through `saveState` afterward. The second edit remains unstaged and appears in status.

Try this trace with a debugger or add temporary diagnostic prints. Do not modify the binary object files to learn normal behavior; use the test fixture helpers when deliberately exploring corruption.

## Unfamiliar C++ syntax used here

`std` is C++'s standard-library namespace. A prefix such as `std::string` tells
the compiler which library owns the name. The source uses `using namespace std;`
so the functions can use `string`, `vector`, and `cout` directly.
`FilePath` is a readable name for the standard filesystem path type; it replaces
the earlier `fs::path` shorthand. These names refer to the same library features.

| Syntax | Meaning in this project |
| --- | --- |
| `using namespace std;` | Make standard-library names such as `string`, `vector`, and `cout` available without repeating `std::`. |
| `using FilePath = filesystem::path;` | Give filesystem paths a clear type name, for example `FilePath root`. |
| `using Snapshot = map<string, string>;` | A readable alias for the ordered path → blob-ID map. |
| `const Snapshot& files` | Borrow an existing map without copying; this function cannot modify it through that reference. |
| `const pair<const string, string>& entry` | A map entry: its key is immutable, and this loop borrows the pair. `entry.first` is the path; `entry.second` is the ID. |
| `Snapshot::const_iterator` | The named iterator type for traversing a map without modifying entries. |
| `Snapshot{}` / `MergeState{}` | Value-initialize an empty snapshot or reset merge fields to their defaults. |
| `static_cast<size_t>(value)` | Explicitly convert a validated integer length to the container-size type. |
| `ios::binary` | Read/write bytes without text newline conversion. |
| `error_code` | Capture a filesystem error without throwing, used for careful inspection/cleanup; meaningful failures are reported. |
| `symlink_status(path)` | Inspect the link itself, unlike `status`, which can follow it. Components are checked before subsequent filesystem calls. |
| `= delete` on lock copying | Prevent accidental copies that would both try to own/release the same lock. |
| Constructor/destructor scope | RAII: the lock is acquired by construction and normally released when its object leaves scope, including exception unwinding. |
| Anonymous namespace | Keep implementation helper structs/functions private to one `.cpp` file. |
| `constexpr` limits | Compile-time constants with types, used instead of application macros. |
| `throw Error(...)` | Abort the operation with an explanation; `main` catches exceptions and returns a nonzero status. |

The code deliberately uses explicit iterator/entry types where useful, focused helpers, vectors/maps by value, and no manual `new`/`delete`. `#pragma once` is a conventional header inclusion guard. The vendored hash library has its own upstream style.

## Interview questions tied to the implementation

**What is the difference between HEAD, index, and working directory?**

HEAD's branch selects the last committed snapshot. The index is a complete proposed next snapshot identified by `State::indexId`. Working files are whatever currently exists. `status` performs HEAD/index and index/working comparisons separately, which is why a file can be both staged and unstaged.

**Why store a blob when adding instead of rereading at commit time?**

Staging is a promise about specific bytes. `add` stores those bytes immutably; `commit` references the resulting snapshot. Editing afterward cannot change the staged content.

**How do you safely update a branch?**

Write and check the immutable snapshot/commit first. Then publish the branch and related mutable fields through one atomic state replacement. If object writing fails, the old ref remains. An unreachable object after a later failure is acceptable.

**Why does a commit refer to a whole snapshot? Is that wasteful?**

Whole snapshots make restore/comparison easy. Unchanged files share blob IDs, so file contents are deduplicated. Snapshot path maps are still repeated. A local history-pass set now avoids validating the same shared blob repeatedly, while fresh commands validate it again. This keeps the design simpler than compressed trees or a persistent cache.

**Why not use tree LCA after a merge?**

A two-parent commit turns ancestry into a DAG. There can be several incomparable best common ancestors. `bestCommonAncestors` intersects complete ancestor sets and removes dominated nodes; it returns all best candidates.

**Why isn't the first shared BFS node necessarily the best base?**

Traversal order reflects shortest edge routes and queue order, not the partial ordering of ancestry. Multiple parent routes can make an old dominated ancestor appear early. The definition depends on whether another common node descends from it.

**What if there are multiple bases?**

`merge-base` returns them in sorted order. `merge` refuses before writing anything. Recursive virtual-base synthesis is an explicit missing feature, not silently approximated by a timestamp or arbitrary candidate.

**What does three-way merge compare?**

The base, ours, and theirs blob ID or absent state for every path. Equal versions or one unchanged side yield a choice. Otherwise the whole file conflicts. This is conservative file-level merging, not automatic line merging.

**Why store conflict state rather than look for markers?**

Marker-like text can be legitimate content, and binary conflicts have no markers. `MergeState::unresolvedPaths` persists what still needs explicit user staging. `add` marks a chosen file/deletion resolved.

**Why can a resolved merge commit have an unchanged snapshot?**

Even if the user chooses every original ours version, the merge still needs to connect the other branch's history. Both original parent IDs are recorded. Ordinary unchanged commits are rejected.

**How does the LCS diff work, and where is it expensive?**

It fills a suffix DP table over line pairs, then emits context/addition/deletion lines along a longest-subsequence path. There are O(nm) cells and line comparisons; string comparison adds byte cost. The implementation checks byte, line, and cell limits before allocation.

**How does abort avoid deleting unrelated new work?**

The merge state records the original index and a separate owned-path snapshot. `abortMerge` restores those original paths and removes owned creations. A new unrelated file staged during the merge is not automatically considered owned and remains untracked after abort. Obstructing untracked replacements cause refusal.

**Is checkout atomic?**

The state replacement is atomic under the assumed filesystem semantics. The sequence of working-file changes is not. A journal captures before/after bytes, a preflight checks collisions, and ordinary failure triggers rollback. Interrupted-process recovery is tested, but full power-loss durability is not implemented.

**What prevents two commands from racing?**

Atomic lock-directory creation gives ownership to one cooperating command. The others fail. Reads also acquire it. Abrupt exits can leave stale locks; a person must confirm the process is gone before manual removal.

**Why use a real hash library?**

Cryptographic implementation is not the learning goal. PicoSHA2 is a small established implementation; MiniGit validates its integration with known vectors and Python hashes. `std::hash` has no persistent cryptographic-ID guarantee.

**How are failures tested without damaging personal files?**

Each process test creates an isolated temporary repository. Controlled exception and abrupt-exit hooks exercise journals and rollback. Corrupt objects are constructed with independent Python serialization/hashing. Permission and live-lock tests verify controlled error exits.

## Small exercises

1. Add `minigit show <full-commit-ID> <path>` that prints binary bytes safely to an output file. Reuse validated object loading and path checks.
2. Add a dry-run switch summary. Reuse planning rather than creating a second unsafe checkout implementation.
3. Add author fields in a **new format version**. Keep version 1 readable, and test deterministic serialization and rejection of unknown versions.
4. Replace the LCS table with a linear-space reconstruction algorithm. Keep deterministic ties, empty-file/newline behavior, and independent diff tests.
5. Add a repository-local ignore-pattern mechanism. Define whether already tracked files can ever be ignored; do not change deletion staging accidentally.
6. Run `tests/benchmark_history.py` and vary its bounded fixture. Explain the local history-pass success cache, then measure remaining repeated validation across separate passes before proposing more caching.
7. Implement `fsck` to inspect unreachable objects as well as reachable ones. Report corruption without changing refs or deleting data.
8. Add garbage collection only after designing roots for branches, index, active merges, and journals. Start with a report-only mode.

Use temporary repositories for each exercise. Expand tests around changed behavior; avoid testing implementation details that provide no independent correctness check.

## Honest tradeoffs

This is a working educational CLI with meaningful safety checks, not a production Git replacement. It trades performance and feature breadth for an explainable complete-snapshot model, flat object storage, explicit graph algorithms, conservative merging, and simple recovery.

There is no Git format compatibility, remote transport, line-level merge, virtual merge-base synthesis, author identity, detached HEAD, ignore engine, compression, garbage collection, rename detection, versioned executable permissions, symlink support, or empty-directory tracking. Supported filesystem behavior and fixed limits are documented in the README.

Commands eagerly validate reachable history, with a local success-only blob-ID cache for each history-loading pass. This reduces repeated shared-blob hashing in the measured fixture, while commit/snapshot reading and other validation costs remain. Journaling retains exact bytes in memory/on disk up to a 64 MiB operation limit. Atomic rename does not supply durable power-loss transactions; stale locks, partial initialization, and temporary-write remnants can require explicit operator recovery. The tests demonstrate actual covered behavior rather than a benchmark or production-scale claim.

## Trace the fixes in an interview

- **Hard links:** Two names can refer to the same inode. Selecting the first equivalent file picked the wrong name. `resolvePath` now chooses the exact directory entry first; an ambiguous non-exact alias is refused. Restoring one name replaces that entry without rewriting the other name.
- **Shell guidance:** Human-readable double quotes do not stop `$variable`, backticks, or `$(command)` in a shell. `reportMergeConflicts` uses `quoteShellArgument` for its command and `display` for diagnostics. The quoting helper single-quotes literal bytes and escapes only embedded single quotes. MiniGit itself never calls a shell.
- **Recovery:** A structurally valid journal could contain damaged backup bytes. Transaction v2 adds SHA-256 over the whole payload. `decodeJournal` verifies it before rollback or published-state cleanup. V1 objects/state keep the same IDs; unchecksummed legacy journals are retained for conservative manual recovery. A checksum is not authentication because a repository writer could replace it too.
- **Failure recovery:** `planTransition` delegates backup collection to `collectFileChanges`, collision checks to `checkDestinationCollisions`, and directory planning to `planDirectoryChanges`; all finish before a journal or working-file write. A test interrupts rollback after one backup write, then proves that a retry restores exact bytes/state while preserving unrelated files.
- **Merge status:** Exit 0 means the merge command successfully performed its operation, which may be starting an unresolved merge. Check `status`, stage each choice, and commit; the final commit keeps both original parent IDs even when the chosen contents include markers.
- **Validation cost:** Rechecking a large unchanged blob in every snapshot was measurable. A local set remembers only successful validation during a history pass and is discarded afterward. Corruption between commands is still detected.

`versionAt` is now declared alongside `Snapshot` helpers in `types.hpp` and defined in `storage.cpp`; merge and repository code share it. The unused `Repository::state()` accessor was removed after checking source, tests, and documentation for callers.

The two `getSnapshot` declarations are **function overloads**: C++ chooses one by its argument list. The two-argument form borrows a `set<string>&` of already validated blob IDs from a history pass; the one-argument form creates its own local set. There is no cross-command mutable cache. Quoting strings such as `"'\"'\"'"` are C++ string literals whose backslashes escape double quotes in source; the emitted shell text closes a single-quoted region, emits a literal quote, and reopens it. Journal SHA-256 covers the exact encoded bytes, not a reformatted display of them.
