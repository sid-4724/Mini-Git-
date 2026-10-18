# MiniGit

MiniGit is a working, local version control system written in readable C++17. It is a portfolio project for learning content addressing, snapshots, staging, graph traversal, LCS dynamic programming, and safe filesystem changes. It uses its own versioned format in `.minigit/` and never invokes Git.

The code uses `using namespace std;` so familiar names such as `string`, `vector`,
and `cout` are easy to read. `FilePath` names the filesystem path type. Start with the
[learning guide](docs/LEARNING_GUIDE.md) for a walkthrough of the code and syntax.

Implemented commands include initialization, staging files/directories/deletions, status, commits, reachable history, text diffs, file restoration, branches, safe switching/deletion, DAG merge bases, fast-forward and conservative file-level three-way merges, persistent conflicts, merge abort, and interrupted-operation recovery.

There are no remotes, push/pull, networking, rebase, stash, database, or Git compatibility.

## Build and test

Requirements:

- A C++17 compiler and standard library with `std::filesystem`.
- CMake 3.16 or newer.
- Python 3 for the process-level tests; the executable itself needs no Python.
- SHA-256 comes from the vendored, MIT-licensed [PicoSHA2 v1.0.1](https://github.com/okdshin/PicoSHA2/tree/v1.0.1). CMake locates it through `third_party/`; no external crypto installation or build-time download is needed.

From this project folder:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/minigit --help
```

On the workspace used to create this project, CMake was missing. A local copy was installed in `.tools/`. To use that copy in your current shell, first run:

```sh
export PATH="$PWD/.tools/cmake/data/bin:$PATH"
```

For another machine, install CMake normally, or obtain the same local tooling with `python3 -m pip install --target .tools cmake==4.3.4` and use the PATH line above. `.tools/` and build folders are generated tooling, not application source.

Release verification:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
ctest --test-dir build-release --output-on-failure
```

Build only the application with `-DBUILD_TESTING=OFF` to omit the Python requirement. The filesystem replacement behavior is designed for macOS/Linux. Windows atomic replacement behavior is not claimed.

## Command reference

Add the executable to your PATH with `export PATH="$PWD/build:$PATH"`, or use its absolute path when you change directories.

| Command | Behavior |
| --- | --- |
| `minigit init` | Create an unborn `main`. Repeating it preserves history. Nested initialization is refused. |
| `minigit add notes.txt` | Store exact current bytes and update the complete index snapshot. |
| `minigit add docs` | Stage all regular files and tracked deletions within that directory. |
| `minigit add .` | Stage the current directory recursively; from the root, stage the whole repository. |
| `minigit add -- -draft.txt` | Supply a leading-dash filename. Quote filenames containing spaces. |
| `minigit status` | Compare HEAD → index → working files; also show untracked files and unresolved conflicts. |
| `minigit commit -m "Write introduction"` | Commit the index, preserving unstaged edits. Reject empty/whitespace messages and unchanged ordinary snapshots. |
| `minigit log` | Show IDs, messages, Unix timestamps, and parent IDs, once per reachable commit. |
| `minigit diff` | Compare the index with working files. Untracked files are excluded. |
| `minigit diff --staged` | Compare HEAD with the index. |
| `minigit restore notes.txt` | Discard selected working edits and copy the indexed version; leave the index unchanged. |
| `minigit restore --source main notes.txt` | Restore that file from a branch snapshot. A full 64-character commit ID also works. |
| `minigit restore --source main -- -draft.txt` | Combine a source with a leading-dash path. |
| `minigit branch` | List branches; mark HEAD's branch with `*`. |
| `minigit branch chapter` | Create a branch at HEAD after the first commit. |
| `minigit branch -d chapter` | Delete only when its tip is an ancestor of HEAD; never delete the current branch. |
| `minigit switch chapter` | Require clean tracked state, preflight all collisions, restore the destination, and change HEAD/index. |
| `minigit merge-base main chapter` | Print all best common ancestors in sorted ID order. |
| `minigit merge chapter` | Require clean tracked state; handle up-to-date, fast-forward, or one-base three-way merge. |
| `minigit merge --abort` | Discard merge-owned edits and restore the original tracked snapshot/index, preserving unrelated untracked files. |
| `minigit recover` | Roll back a journaled interrupted operation, or clear a journal whose new state was already published. |

Commands locate the root by walking upward from the current directory. Relative paths are relative to that current directory. Absolute paths inside the repository are accepted; paths outside it and any `..` component are rejected.

Branch names contain 1–64 ASCII letters, digits, dots, underscores, or hyphens, and start with a letter or digit. Branch names are case-sensitive metadata keys. Restore tries an exact branch name first, then a complete commit ID; abbreviated IDs are not supported.

## A complete fictional demo

Run this from the project folder. It creates an isolated temporary repository, leaving the project and your other files alone. It intentionally leaves the temporary demo available for exploration.

```sh
export PATH="$(pwd -P)/build:$PATH"
demo_directory="$(mktemp -d)"
cd "$demo_directory"

minigit init
printf 'A traveller reaches a lighthouse.\n' > story.txt
minigit add story.txt
minigit commit -m "Opening scene"

minigit branch characters
minigit switch characters
printf 'Mira: lighthouse keeper.\n' > cast.txt
minigit add cast.txt
minigit commit -m "Introduce Mira"

minigit switch main
printf 'The island lies beyond the northern fog.\n' > setting.txt
minigit add setting.txt
minigit commit -m "Describe the island"

minigit merge-base main characters
minigit merge characters
minigit log
minigit status
minigit branch -d characters

printf 'A changed opening.\n' > story.txt
minigit diff
minigit restore story.txt
```

The two branches change different files, so the divergent merge automatically creates a commit with both original tips as parents. `cast.txt`, `setting.txt`, and the original `story.txt` all remain. The final restore explicitly discards the new opening.

To explore a conflict in the same demo:

```sh
minigit branch alternate
printf 'Mira opens the gate.\n' > story.txt
minigit add story.txt
minigit commit -m "Main ending"
minigit switch alternate
printf 'Mira closes the gate.\n' > story.txt
minigit add story.txt
minigit commit -m "Alternate ending"
minigit switch main
minigit merge alternate
minigit status
cat story.txt

printf 'Mira leaves the gate ajar.\n' > story.txt
minigit add story.txt
minigit commit -m "Resolve the ending"
minigit log
```

Instead of resolving, `minigit merge --abort` restores the pre-merge tracked state. To resolve a conflict as a deletion, remove the conflicted file and run `minigit add path`. Staging is the explicit resolution signal; the tool does not search for marker strings.

`minigit merge` preserves its existing exit-status behavior: **exit 0 can mean an active merge with unresolved conflicts**, as well as an up-to-date, fast-forward, or completed merge. Run `minigit status` to check the persisted unresolved list. Edit or restore each choice, run `minigit add -- <path>` for every resolution (including deletions), then `minigit commit -m "Resolve merge"`; use `minigit merge --abort` to cancel. Scripts must inspect conflict state/output rather than interpreting exit 0 as a completed merge commit.

For a binary conflict, MiniGit keeps ours, including absence if ours deleted the file. It prints the other original tip so you can use `minigit restore --source <that-full-ID> -- <path>`, then `minigit add -- <path>` and commit. It never inserts text markers into binary data. Diagnostic `display()` formatting escapes control characters for reading; it is not shell argument quoting. The suggested restore command uses separate POSIX single-quote escaping for macOS/Linux shells, including literal dollar signs, quotes, backticks, command substitutions, and newlines. A filename containing a newline makes the quoted command span physical lines; copy the complete command. MiniGit never runs a shell to restore a file.

## Safety and limits

- Commits use staged bytes; untracked files never enter a commit automatically.
- `restore` discards changes to the selected tracked working file. Its source does not update the index. If that source lacks a currently indexed file, it removes the working file. Absent/untracked cases produce a clear error; an existing untracked file is never overwritten.
- Switching and merging reject staged or unstaged tracked changes and any untracked file/directory obstruction. Unrelated untracked files are preserved.
- Merge abort overwrites merge-owned tracked edits and removes merge-owned created files. It preserves unrelated untracked files, including an unrelated new file you staged during the merge. Unsafe replacements must be moved aside before abort can succeed.
- `.git` and `.minigit` path components are excluded case-insensitively, including inside subdirectories. Symlinks and symlinked components are rejected. Special files are unsupported.
- Filenames may contain spaces, Unicode, colons, pipes, equals signs, and newlines. The format stores bytes with lengths. Paths use `/`; backslashes, NUL, empty components, and traversal components are rejected. Printed paths/messages escape control characters.
- Exact directory-entry names take priority when resolving paths. Hard-linked files are tracked as distinct names: `add .` stages both and restoring one replaces only that name, leaving the other name's bytes alone. Checkouts/restores need not preserve the hard-link relationship. A non-exact case/Unicode alias that matches multiple hard-linked names is refused; use the exact stored spelling.
- Existing paths use their filesystem spelling. A scratch layout on the repository's filesystem checks target filename aliases and file/directory collisions before staging or writing.
- Case-only or Unicode-normalization-only path changes can be refused on filesystems where the spellings alias; move conflicting contents aside rather than forcing an overwrite.
- Only regular file contents are versioned. Executable permissions, symlinks, renames as a separate operation, and empty directories are not versioned. Renaming appears as deletion plus addition. Empty parent directories can remain after file removal.
- Ordinary commits with unchanged snapshots are rejected. An empty root snapshot cannot be committed through the CLI. Branch creation requires a first commit.
- Three-way merging is conservative at the **file** level. Two different edits to one file conflict even when they affect different lines. Divergent merges that change file/directory layout are refused. Fast-forwarding and switching can handle tracked file/directory replacements when unobstructed.
- Histories with no common ancestor or multiple best common ancestors cannot be automatically merged. There is no recursive virtual-base synthesis.
- Binary detection is a simple heuristic: NUL, DEL, or ASCII control bytes other than tab/CR/LF mean binary. Some binary formats without those bytes may display as text. Files are never rewritten by diff.
- Log uses deterministic depth-first traversal with the first parent first, skipping previously visited commits. It is neither a graph drawing nor a timestamp-sorted/topological presentation.
- No ignore-pattern engine or object garbage collection is implemented. `.gitignore` is only for users who separately manage this source with Git; MiniGit does not interpret it.

| Resource | Limit |
| --- | --- |
| One regular file/blob | 16 MiB |
| One serialized immutable object | 32 MiB |
| Snapshot / scanned working regular files | 20,000 |
| Relative path / commit message | 4,096 bytes / 65,536 bytes |
| Mutable state | 8 MiB, at most 20,000 branches |
| Reachable commits across branches and merge tips | 20,000; estimated stored commit metadata at most 64 MiB |
| Transaction journal | 64 MiB including before/after bytes and state; up to 40,000 changed files and 40,000 directories per directory list |
| Detailed text diff | Combined input at most 1 MiB, at most 20,000 lines per side, at most 1,000,000 DP cells (about 4 MiB) |

Exceeding a limit produces an error or a diff omission message. A text conflict whose marker file would exceed the blob limit is refused before working files change.

## Locking and recovery

All repository commands acquire the atomically created `.minigit/lock/` directory, including reads so they do not observe a cooperating command halfway through a change. Normal exits remove the lock; an abrupt exit can leave it behind. MiniGit never steals a lock.

After confirming the owning MiniGit process has exited, inspect its `owner` file and remove that stale lock directory manually. Then run `minigit recover` if `.minigit/transaction` exists. Do not delete the journal: it contains original bytes and metadata.

Multi-file operations validate destinations and save before/after bytes before applying changes. Ordinary I/O failures attempt rollback; if rollback fails, the journal remains and the error explains that recovery is incomplete. Recovery refuses to overwrite post-crash edits or remove new untracked contents: back them up and move them aside, then retry.

New journals use an explicit **transaction v2** envelope with a SHA-256 checksum over the entire serialized payload: paths, before/after bytes, permissions, old/new states, and directory lists. Recovery verifies it before changing working files or clearing even an already-published journal. Invalid, truncated, unsupported, and legacy journals remain in place. Objects and state keep their original v1 format and IDs. A checksum detects accidental corruption; someone who can rewrite the repository can also replace the checksum, so it is not authentication.

State is one atomic replacement, so branches, HEAD, index, and merge state publish together. If recovery sees a distinct new state already published, it keeps the completed operation and clears the journal. For a restore whose state bytes did not change, recovery conservatively undoes the interrupted restore.

These guarantees cover controlled ordinary failures and tested process exits on a cooperating local filesystem. There is no `fsync` durability protocol, full power-loss guarantee, hostile-filesystem race protection, or permission/ACL preservation for normal versioned checkout. Abrupt exits during individual temporary-file writes can leave temporary files that need inspection/moving before recovery. Interrupted initialization can leave incomplete metadata; after backing it up and confirming no history/state exists, remove only that newly created incomplete `.minigit` and retry initialization. Never apply this instruction to a repository with history.

### Legacy journal recovery

Automatic recovery refuses unchecksummed v1 journals; it never silently trusts their backup bytes or deletes them. After confirming no MiniGit process is running, make a complete copy outside the repository, including working files, `.minigit/state`, objects, and the intact journal. Keep the original untouched. Work on a separate copy with someone who can inspect the documented length-prefixed v1 journal and compare both recorded states, referenced object hashes, current files, and independent backups. Committed/indexed bytes can be checked against immutable objects; unstaged bytes captured only in the legacy journal cannot be independently verified from its structure. Preserve uncertain versions separately. Reconstruct and validate a recovered copy before using it, and retain the original journal for investigation. Do not edit a version number or invent a checksum to make an old journal appear verified.

## Source and learning

- [Architecture, v1 objects/state, and v2 journals](docs/ARCHITECTURE.md)
- [Algorithms and complexity](docs/ALGORITHMS.md)
- [Source-reading guide, exercises, and interview answers](docs/LEARNING_GUIDE.md)
- [SHA-256 dependency provenance and license](third_party/README.md)
- [Recorded build and validation results](docs/VALIDATION.md)

CTest runs eight process-test groups plus a C++ algorithms/codec executable. Each CLI test uses a fresh temporary directory and a new subprocess for every command. Assertions stay active in Release. Tests exercise normal workflows, malformed metadata, live locks, permission errors, injected write failures, and abrupt-exit recovery. Test-only environment hooks are described in the architecture document; leave them unset during normal use.

## Source packaging

The source ZIP should contain `CMakeLists.txt`, `.gitignore`, `README.md`, `include/`, `src/`, `tests/`, `docs/`, and `third_party/` (including PicoSHA2 and its license). Exclude `.tools/`, every generated build directory, `.minigit/`, real `.git/` metadata, caches, compiled outputs, and the ZIP itself. Keep generated tooling/builds available locally when needed; they are not source-package contents. The existing `.gitignore` covers the standard build/tooling directories. MiniGit does not interpret `.gitignore`.
