# Architecture

## Modules

| File | Responsibility |
| --- | --- |
| `src/main.cpp` | Parse the small CLI grammar, locate the root, acquire the lock, dispatch, and print controlled errors with exit status 1. |
| `include/minigit/types.hpp` | Plain commit/state structs, the `Snapshot` map alias, exceptions, and resource limits. |
| `src/storage.cpp` | Path/name checks, length-prefixed codec, versioned envelopes, SHA-256 through PicoSHA2, object reads/writes, state serialization, binary file I/O, and lock lifetime. |
| `src/repository.cpp` | Coordinate init, add, status, commit, log, restore, branch operations, clean-state checks, merge cases, and abort. |
| `src/worktree.cpp` | Validate filesystem layouts, plan complete working changes, preflight collisions, journal exact before/after bytes, apply, roll back, and recover. |
| `src/graph.cpp` | Load reachable commits, validate parent references/cycles, traverse ancestors/history, and compute all best common ancestors. |
| `src/diff.cpp` | Binary heuristic and bounded line-based LCS diff. |
| `src/merge.cpp` | Pure file-version decisions plus writing immutable marker blobs; it does not change working files or branches. |
| `tests/test_cli.py` | Standard-library process harness, isolated repositories, and independent object-format fixtures/checks. |
| `tests/unit_tests.cpp` | SHA-256 vectors, parser checks, graph errors, exhaustive small-DAG comparison against an independent reachability definition, and diff checks. |

Headers expose a few functions/classes; there is no inheritance framework or raw owning pointer. `Repository` owns paths, the object store, current state, and the loaded commit graph by value.

## Layout and logical model

```text
example-repository/
  story.txt
  folder/notes.txt
  .minigit/
    state                 # atomic mutable state: HEAD, refs, index, merge state
    objects/
      <64 lowercase hex>  # versioned, type-tagged immutable object bytes
      ...
    lock/                 # created atomically; normally short-lived
      owner
      layout/             # temporary zero-byte filesystem-layout probe
    transaction           # present only during a multi-file operation/recovery
```

Temporary replacement files use random names beginning `.minigit-tmp-` in the destination's directory. Successful operations remove them. Object files are flat in `objects/` for ease of inspection.

- **Blob:** exact file bytes, including NUL, CRLF, empty data, and missing final newline.
- **Snapshot:** sorted mapping from normalized relative path to blob ID. Directories are implied by file paths.
- **Commit:** snapshot ID, Unix timestamp, zero/one/two parent IDs, and message. Parent order is meaningful: ours first, theirs second for a merge.
- **Branch:** an ASCII name mapped to a commit ID. The initial `main` tip is the empty string (unborn); it is not an object ID.
- **HEAD:** the current branch name. Detached HEAD is not implemented.
- **Index:** the ID of a complete snapshot, rather than only the list of changed files.
- **Merge state:** the fixed original tips/base, original index snapshot, operation-owned paths snapshot, and explicitly unresolved paths.

Branch names, HEAD, and the index are fields inside one `state` file rather than separately updated ref files. This keeps related mutable updates atomic.

## Exact version 1 object and state format

All integers are **unsigned 64-bit big-endian**, regardless of host endianness. A field is an integer byte length followed by exactly those bytes. All lengths/counts are checked against limits before allocating or iterating. Unexpected trailing bytes are rejected.

An envelope contains, in order:

1. Field `"MiniGit"` (7 bytes).
2. Integer format version `1`.
3. Field containing the object or metadata type.
4. Field containing the payload.

Object IDs are lowercase SHA-256 hexadecimal of the **entire envelope**, including signature, version, type, and lengths. For example, identical raw payloads typed as a blob and snapshot have different IDs. There is no compression. State uses this v1 envelope without a content-addressed filename. Journals are separate v2 records described below; this does not change any immutable object bytes or IDs.

| Type | Payload order |
| --- | --- |
| `blob` | Raw bytes, already length-prefixed by the outer payload field. |
| `snapshot` | File count; repeated path field, blob-ID field, in strictly increasing path order. |
| `commit` | Snapshot-ID field; timestamp integer; parent count (0–2); ordered parent-ID fields; message field. |
| `state` | Current-branch field; branch count; sorted name/tip fields; index-ID field; merge-active integer (0/1); optional merge fields below. |
| Active merge fields | Ours-ID, theirs-ID, base-ID, original-index-ID, owned-snapshot-ID fields; unresolved count; strictly sorted unresolved-path fields. |
| Legacy v1 `transaction` payload (also used inside v2) | Old-state field; new-state field; changed-file count; repeated file records; created-directory count/fields; removed-directory count/fields. |
| Journal file record | Path field; existed-before integer (0/1); before-bytes field; original POSIX permission bits integer; exists-after integer (0/1); after-bytes field. |

### Transaction journal version 2

A new journal contains, in exact order:

1. Field `"MiniGit"`.
2. Integer `2`.
3. Field `"transaction"`.
4. Field containing the journal payload specified in the table above, unchanged in layout.
5. Field containing the 64 lowercase hexadecimal characters of SHA-256 of the **entire raw payload in step 4**.

The checksum covers old/new serialized state, changed-file paths, before/after presence flags and bytes, original permissions, counts, and both directory lists. Header fields have fixed expected values; the decoder checks signature, supported version, type, bounded lengths, checksum, and absence of trailing bytes before parsing/trusting the payload. The entire record, including the checksum, must fit the existing 64 MiB journal cap. `encodeJournal` and `decodeJournal` use the existing PicoSHA2-backed `sha256` helper; the generic v1 `envelope`/`unpack` functions remain unchanged.

Both ordinary rollback (which rereads the persisted journal) and explicit recovery verify integrity before changing working files or removing the journal. A failed check leaves working files, state, and journal untouched. Even a journal whose distinct new state is already published must pass verification before cleanup. A checksum detects accidental corruption, not authentication: a writer able to replace the payload can recompute its digest.

Legacy v1 journals are preserved and refused for automatic recovery. They have no independent protection for dirty backup bytes; structural validity and object references cannot establish those bytes' integrity. Follow README's **Legacy journal recovery** guidance: preserve the complete original, inspect a separate copy, compare independently verifiable versions/backups, and reconstruct a validated recovered copy. Never relabel or silently upgrade an old journal. Unsupported versions are also preserved. Existing v1 repositories without a pending journal load normally; blobs, snapshots, commits, and state remain v1 with identical IDs.

Paths use `/` and must be relative, normalized, nonempty, and within the documented limit. Empty components, `.`, `..`, backslash, NUL, and reserved metadata components are rejected. Reserved names are checked case-insensitively to protect metadata aliases on case-insensitive filesystems. Length-prefixed fields avoid ambiguities from spaces, pipes, colons, equals signs, or newlines.

Snapshot IDs and commit IDs are always full 64-character hashes. Entries must be sorted and unique. A snapshot cannot contain both `chapter` and `chapter/page.txt`. Commit readers reject more than two parents, duplicate/self parent IDs, unsupported timestamps, empty messages, and malformed IDs. Loading history resolves every parent and rejects missing objects or cycles. Referenced snapshots and blobs are hash/type/format checked; unreferenced abandoned objects are not scanned.

The application also checks HEAD's branch, unborn-branch invariants, active merge tips, original index matching ours, merge-base ancestry, and conflict-path references.

## Command data flow

### Initialization

`initializeRepository` inspects ancestors to refuse nested repositories. If metadata already exists, it locks and validates it, then reports that history was preserved. For a new repository it creates metadata/objects, stores the empty snapshot, and writes an unborn-main state. Partial initialization never touches working files, but may require manual cleanup of newly created incomplete metadata after a failed setup.

### Add

`resolvePath` rejects traversal, outside paths, and symlinks, then finds actual filesystem spelling. An exact directory-entry filename wins before filesystem-object equivalence is considered. Distinct hard-link names may share an inode but stay separate snapshot keys. If a non-exact alias has multiple equivalent entries, resolution refuses ambiguity rather than using directory order. Hard-link relationships are not versioned; atomic replacement of one path leaves the other entry's bytes intact. `scanFiles` recursively selects regular files while excluding metadata. `add` starts from the complete index:

1. Erase scoped indexed paths that no longer appear.
2. Read selected bytes in binary mode and store immutable blobs.
3. Update scoped blob IDs and clear only explicitly staged conflict resolutions.
4. Maintain merge ownership for originally tracked/merge-owned/conflicted paths.
5. Check the complete staged filesystem layout in a temporary scratch tree.
6. Store the new snapshot and atomically publish its index ID.

Failed staging leaves the previous state intact; newly written unreachable blobs are harmless and retained. A future commit does not reread working bytes.

### Status and commit

Status compares HEAD/index for staged changes and index/working hashes for unstaged changes. It separately scans names to identify files absent from the index. An absent file is distinct from an empty blob.

`commit` checks the message, unresolved paths, unchanged ordinary snapshots, and graph resource capacity. It writes the commit object before replacing state, advances only HEAD's branch, and clears merge state for a completed merge. It does not change working files. A resolved merge may commit an unchanged index because the two-parent graph connection itself matters.

### Restore and switch

Restore selects one index or source-snapshot entry. The ownership check uses the index (and active merge ownership), so a file recreated after a staged deletion is protected as untracked. It sends that one path to the transition planner with a forced write. A source restore may delete an indexed path absent from the source. It leaves the index unchanged and does not mark conflicts resolved.

Switch requires clean indexed/tracked state and no active merge. It selects the destination snapshot, prepares a new HEAD/index state, and uses the transition layer. Files absent from the destination are removed; unrelated untracked contents remain. Safe tracked file-to-directory and directory-to-file transitions are supported.

### Merge

`merge` checks clean tracked state and original branch tips. Ancestor membership selects up-to-date or fast-forward behavior. Otherwise, all best merge bases are computed; zero or multiple bases cause refusal.

For one base, `compareThreeSnapshots` first rejects unsupported cross-snapshot file/directory layouts, then chooses versions by equality. Conflict-free divergence writes a two-parent merge object and automatically publishes it through the transition layer. Conflicts leave the branch at ours, stage all unambiguous changes, place markers/ours binary bytes in the working directory, and persist unresolved paths. Successfully starting this conflicted state returns exit 0 under the preserved public CLI behavior; `status` shows whether resolution is still pending. `reportMergeConflicts` keeps readable `display()` text separate from `quoteShellArgument()` in the suggested POSIX restore command. The latter surrounds bytes with single quotes and represents an embedded single quote by closing the quotes, emitting a double-quoted single quote, and reopening them. Literal newlines remain inside the quoted argument. Restoration uses direct filesystem calls, never shell execution.

`add` is the resolution signal even if marker text remains. `commit` keeps both original tips regardless of later staging. `abortMerge` forces restoration of the original snapshot, using merge-owned paths rather than indiscriminately deleting everything in the index. Unrelated new/staged files become or remain untracked.

## Preflight, journal, rollback, and crash behavior

`validateFilesystemLayout` builds a zero-byte scratch tree under the owned lock on the same filesystem. This detects case/Unicode aliases and layout failures before changing real files. The scratch tree is removed on normal completion.

`planTransition` coordinates `collectFileChanges`, `checkDestinationCollisions`, and `planDirectoryChanges` to:

1. Determines every changed/forced path.
2. Captures exact existing bytes and permissions.
3. Loads/validates every target blob.
4. Rejects untracked files, directories, special files, symlinks, and parent-path obstructions.
5. Plans required directory creation/removal and checks the journal size.

Only after this entire plan succeeds does `transitionWorktree` write the journal. It removes replaced tracked files and obstructing tracked directories, creates target directories, writes target bytes using temporary replacements, then atomically publishes state. Finally it removes the journal. These filesystem operations are sequential, not an atomic whole.

On an ordinary exception before publication, `rollback` preflights recovery, removes applied versions, restores directory shape, writes original bytes/permissions and old state, then clears the journal. A rollback failure produces an explicit incomplete-recovery error and retains backups.

After an abrupt exit, normal commands refuse a journaled repository. An operator first verifies the process is gone and removes the stale lock. `recoverTransaction` verifies the v2 journal checksum, then validates both recorded states and their referenced objects/graphs before touching working files, then compares the current state to the journal:

- A distinct new state already present means the operation published: keep it and remove the journal.
- Old state still present means restore backups.
- Matching neither side means refuse and ask for a backup/manual investigation.
- If old and new state bytes are equal (ordinary restore), recover conservatively rolls back.

Before rollback, existing changed files must match recorded before/after bytes, or be absent. Newly edited bytes and newly inserted untracked content in operation-created directories cause refusal with an actionable message. The user must save and move them aside. MiniGit does not blindly delete them.

Atomic replacement is implemented with a temporary file plus `std::filesystem::rename` in the same directory. It checks write, flush, close, and rename failures. It does **not** call `fsync`; metadata/data durability after power loss is not guaranteed. Concurrent external editors or malicious replacement races are outside the guarantee. A crash while a temporary file is being written can leave that temporary file; inspect/move it as instructed if it blocks recovery.

## Locking and test hooks

`RepositoryLock` creates `.minigit/lock` atomically, writes an owner timestamp, and releases it with RAII on normal exits. Existing locks are errors, including for read commands. Destructor cleanup failures are printed and may leave a stale lock. Locks are never forcibly removed by the program.

The integration harness can set `MINIGIT_TEST_FAIL_POINT` to throw a controlled error, or `MINIGIT_TEST_CRASH_POINT` to exit abruptly with status 86. Supported points are `before-object-write`, `after-journal-write`, `after-working-write`, `before-state-write`, and `after-state-write`. `MINIGIT_TEST_ROLLBACK_FAIL_POINT=after-first-backup-write` separately throws inside rollback after its first original-file restoration, allowing a transition failure and a partial rollback failure in one process. The harness removes this variable for later commands, so a fresh retry can recover normally. These are internal test instrumentation, not normal CLI options. After-state publication is already committed, so a subsequent failure asks for journal cleanup instead of pretending to undo the published state.

CLI assertions use Python's standard `unittest`; C++ checks throw on failure instead of relying on `assert`. All CLI test setup/fixtures live in temporary directories, including manually constructed malformed objects and lock holders. A fresh executable process is used for every command.
