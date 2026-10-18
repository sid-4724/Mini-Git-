# Recorded validation

## 18 October 2026: readability update

Replaced repeated namespace prefixes with `using namespace std;` and the `FilePath` alias.
Updated the learning guide to match. The Release build completed with the existing
compiler warnings enabled, and all nine CTest groups passed with
`ctest --test-dir build-release --output-on-failure -j 4`.

## Historical project validation (original record)

Verified in the project workspace on macOS arm64 with Apple Clang 17.0.0, locally installed CMake 4.3.4, and CMake-selected Python 3.13.9. No Git commands were used.

The exact final normal and Release build/test commands were:

```sh
export PATH="$PWD/.tools/cmake/data/bin:$PATH"
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
ctest --test-dir build-release --output-on-failure
```

Both configurations compiled successfully and passed **9 of 9 CTest groups**, with **zero failures and zero skipped CLI cases**. Each configuration actually ran these process scenarios:

| Group | CLI cases |
| --- | ---: |
| Storage/staging | 5 |
| History/diff/restore | 5 |
| Branches/switching | 6 |
| Merges/conflicts/abort | 13 |
| DAG ancestry/merge bases | 3 |
| Path/metadata/permission/lock safety | 11 |
| Failure injection/process-exit recovery | 11 |
| End-to-end CLI integration | 1 |
| **Total CLI cases** | **55** |

The ninth CTest group runs the C++ algorithms/codec executable. It checks SHA-256 known vectors (empty, abc, million-a), typed binary serialization, parser errors, graph cycles/missing parents, text diff behavior, and **1,024 merge-base tip-pair comparisons** against independently calculated reachability across 64 small DAGs. This is not a claim of 1,024 separate CTest tests.

The two shell demos copied from README.md were also executed successfully together in a managed temporary repository: a conflict-free two-parent merge followed by a text conflict and explicit staged resolution.

CTest's detailed generated logs are `build/Testing/Temporary/LastTest.log` and `build-release/Testing/Temporary/LastTest.log`. These results are correctness checks on this environment, not performance benchmarks, Windows validation, power-loss testing, or production-readiness claims.

## Fix validation — 16 October 2026 (Asia/Kolkata)

This run used the existing macOS arm64 workspace as ordinary user UID 501, Apple Clang 17.0.0, existing local CMake 4.3.4, CMake-selected `/opt/anaconda3/bin/python3.13` (3.13.9), and Python 3.14.3 for direct process-test commands. No tooling installation was needed; CMake was absent from the default PATH, so the existing `.tools/cmake/data/bin` directory was added using the PATH command above. No Git commands, commits, pushes, or real `.git` changes were made. Experiments used isolated temporary repositories.

Before editing source, tests, or docs, a source/configuration/dependency backup was made at:

```text
/Users/siddharthduggar/Downloads/MiniGit-backup-20261015T220812Z
```

It contains all original source, tests, docs, dependency files/license, configuration, and the original ZIP; generated builds, tooling, and caches were excluded. File comparison against it found no removed source or documentation, and every `third_party` file was byte-identical. `CMakeLists.txt` and `.gitignore` were unchanged. The existing ZIP was clean already; it was refreshed with the final source and documentation, without build/tooling/cache/compiled contents.

The **same six configure/build/CTest commands shown in the historical record above** were run before changes and again after all code/test changes. Both configurations were additionally rebuilt with `cmake --build build --clean-first` and `cmake --build build-release --clean-first` to inspect fresh compiler output: no compiler warnings were emitted. Core compilation retains `-Wall -Wextra -Wpedantic`. The default configuration has an empty `CMAKE_BUILD_TYPE`; Release uses `-O3 -DNDEBUG`.

| Run/configuration | CTest passed | CTest failed | CLI passed | CLI failed | CLI skipped |
| --- | ---: | ---: | ---: | ---: | ---: |
| This run, original default | 9 | 0 | 55 | 0 | 0 |
| This run, original Release | 9 | 0 | 55 | 0 | 0 |
| This run, final default | 9 | 0 | 70 | 0 | 0 |
| This run, final Release | 9 | 0 | 70 | 0 | 0 |

These current results are distinct from the supplied earlier independent review, which reported 54 passed / 1 skipped of 55 CLI cases because it ran as root. In this run the ordinary-user permission checks ran and passed. Hard-link, ambiguous case-alias, and Unicode-normalization scenarios also ran on this filesystem without skips. On unsupported filesystems the hard-link tests explicitly report why they skip; the ambiguous case-alias test skips when those spellings do not alias.

| Final process group | CLI cases |
| --- | ---: |
| Storage/staging | 7 |
| History/diff/restore | 5 |
| Branches/switching | 6 |
| Merges/conflicts/abort | 15 |
| DAG ancestry/merge bases | 3 |
| Path/metadata/permission/lock safety | 16 |
| Failure injection/process-exit recovery | 17 |
| End-to-end CLI integration | 1 |
| **Total** | **70** |

The C++ executable is one additional CTest group, not part of the 70 CLI cases. Its original SHA/codec/graph checks and 1,024 DAG comparisons remain, with exact/next-bound checks for text bytes, LCS cells, and codec lengths, plus shell-quote checks. A temporary copy of `unit_tests.cpp` was compiled with `c++ -std=c++17 -DNDEBUG -I <project>/include <temporary>/ndebug-check.cpp <project>/build-release/libminigit_core.a -o <temporary>/ndebug-check`; it inserted an intentional `require(false, ...)` before the ordinary checks. Execution returned **1** with `Test failed: intentional NDEBUG verification`. This expected negative check proves checks remain active under `-DNDEBUG`; it is not a failing final-suite case.

### Confirmed bugs and old/new regression evidence

1. **Hard-link name selection — `Repository::resolvePath`, `src/repository.cpp`.** Before the fix, `test_hard_links_keep_distinct_requested_names` failed two subtests: explicit `add second.txt` stored `first.txt`, and `restore second.txt` reported `first.txt`. After the fix, the test passes for both requested names, directory staging retains both, restoration leaves the other linked name's expected dirty bytes intact, and state stays unchanged. Iterating both requested names makes the regression independent of directory order. Existing case/symlink checks plus new ambiguous-alias and Unicode checks pass.
2. **Shell guidance — conflict reporting in `Repository::merge`/`reportMergeConflicts`, `src/repository.cpp`, and `quoteShellArgument`, `src/storage.cpp`.** Before the fix, `test_binary_conflict_suggestion_quotes_literal_shell_characters` failed all three filename subtests: a variable expanded to the decoy name, escaped newline text failed path validation, and a harmless backtick substitution created its marker. The corrected printed commands were executed with `/bin/sh` in isolated repositories and restored exactly the requested bytes. Dollar signs, single/double quotes, spaces, newlines, backticks, and `$(...)` remain literal. Neither substitution marker appeared; unrelated/decoy files, original tips, index, and unresolved conflicts were checked. Restore may legitimately update the merge-owned snapshot, so the test checks that bookkeeping appropriately rather than requiring byte-identical state.
3. **Journal integrity — `encodeJournal`, `decodeJournal`, rollback path, and `recoverTransaction`, `src/worktree.cpp`.** Before the fix, the same-length `ORIGINAL\n` → `CORRUPT!\n` regression returned success after restoring damaged bytes. The published-state corruption test also returned success and deleted the damaged journal. After the fix, backup/target/path corruption fails checksum verification before mutation. Separate checks cover old/new state, permission bits, both directory lists, truncation, trailing data, unknown version, and legacy v1 refusal. Each rejection compares working paths/bytes, state, and the intact rejected journal; valid retry recovery passes. Existing before/after-publication and same-state-restore recovery tests pass.

Focused commands actually run during development included:

```sh
python3 tests/test_cli.py build/minigit StorageStagingTests.test_hard_links_keep_distinct_requested_names
python3 tests/test_cli.py build/minigit StorageStagingTests.test_hard_links_keep_distinct_requested_names SafetyTests.test_filesystem_spelling_and_alias_collision SafetyTests.test_traversal_outside_paths_and_symlink_components
python3 tests/test_cli.py build/minigit MergeTests.test_binary_conflict_suggestion_quotes_literal_shell_characters
python3 tests/test_cli.py build/minigit RecoveryTests.test_journal_corruption_is_rejected_without_mutation RecoveryTests.test_published_journal_is_verified_before_cleanup
python3 tests/test_cli.py build/minigit RecoveryTests
python3 tests/test_cli.py build/minigit SafetyTests.test_nonexact_hard_link_alias_is_refused_when_ambiguous SafetyTests.test_unicode_normalization_alias_uses_existing_spelling
python3 tests/test_cli.py build/minigit RecoveryTests.test_journal_checksum_covers_states_permissions_and_directory_lists
python3 tests/test_cli.py build/minigit RecoveryTests.test_journal_size_limit_refuses_before_working_writes RecoveryTests.test_rollback_failure_retains_journal_and_can_be_retried
```

Old-code negative runs intentionally failed while reproducing the bugs. After fixing the code, the targeted regressions and final full suites passed. An early shell-test fixture attempt hit nested-repository rejection; it was corrected to use separate temporary repositories before measuring the quoting failure. The initial post-quoting assertion incorrectly expected no merge-ownership bookkeeping change after restore; it was corrected to check the actual preserved conflict invariants. Neither intermediate harness issue is a production bug or a final pass claim.

### Additional behavior and boundary coverage

- Added/staged/deleted-before-commit: status shows staged addition plus unstaged deletion, commit stores staged binary bytes, and restore recovers them.
- Rollback failure: a transition failure plus the scoped `after-first-backup-write` hook returns nonzero with both incomplete-recovery and backup/retry guidance. The journal stays, one original file is still absent, and a hook-free retry restores exact original state/bytes/permissions and preserves unrelated contents.
- Criss-cross refusal now compares complete working-file paths/bytes in addition to state, objects, and both merge bases.
- Explicit acceptance of marker text now verifies committed bytes, both original parent IDs, and cleared merge state.
- Filename serialization checks every exact name and associated blob bytes, rather than only counting entries.
- Conflicted merge explicitly returns 0, leaves the branch at ours, and persists both tips/base and unresolved paths. Staging/commit completes it with both parents.
- Exact 16 MiB staging succeeds; 16 MiB + 1 leaves the index unchanged. Sparse 32 MiB + 1 objects and 64 MiB + 1 journal files fail before oversized reads. A real two-file transition requiring 64 MiB of before/after bytes **plus metadata** refuses before working writes or journal creation, preserving state, objects, files, and unrelated work. Tiny codec checks cover declared lengths without allocating huge buffers; LCS checks allow exactly 1,000,000 cells and refuse the next row, and allow exactly the text byte cap but refuse the next byte. A huge valid maximum-sized object or journal was not fabricated just to mirror serializer constants.

### Compatibility, refactors, and public behavior

Only transactions change format: v2 appends a SHA-256 field over the complete existing payload in a transaction-specific envelope. Blob/snapshot/commit/state serialization and IDs remain v1. Legacy journals are preserved and clearly refused; README describes conservative manual recovery from copies. This checksum is accidental-corruption protection, not authentication.

An isolated repository created and committed by the saved original executable was read, restaged, branched, switched both ways, and returned to its original branches by both final executables. State, the complete object set, every object byte, and IDs were unchanged. Original tip for that fixture: `4ff444b8ae0500be635b14752d4559ecf634b0c2e54842949f1cfc449602d6c3`. Independent Python v1 fixtures also remain in the CLI tests.

The public merge exit-status behavior stays unchanged. CLI help/README now explain exit 0 with unresolved conflicts and how `status`, `add`, `commit`, and `merge --abort` complete or cancel the merge. Exact hard-link paths now select the requested name; ambiguous non-exact aliases are refused. Printed POSIX commands now quote literal names correctly.

Small refactors extract `collectFileChanges`, `checkDestinationCollisions`, and `planDirectoryChanges` while keeping preflight-before-write and journal limits; extract `reportMergeConflicts`; expand storage validation branches; and share `versionAt` through `types.hpp`/`storage.cpp` instead of duplicating `fileVersion`. Source/tests/docs caller searches found the `Repository::state()` accessor unused; its declaration and definition were removed. No complete source file or header was removed. The new benchmark script uses only Python's standard library and stays outside CTest.

### Bounded history measurement

`tests/benchmark_history.py` creates a separate temporary v1 repository containing **20 distinct snapshots/commits**, one unchanged **4,194,304-byte file** shared by all snapshots, and a changing small counter. Fixture serialization is independent Python code. It measures wall-clock `log` execution in three fresh processes per executable, discarding normal command output; fixture construction is outside the timed interval. No warmup, cache flushing, or controlled system-load claim is made.

The commands used saved original executables and then the final executables:

```sh
python3 tests/benchmark_history.py <evidence>/build-baseline-minigit <evidence>/build-release-baseline-minigit
python3 tests/benchmark_history.py build/minigit build-release/minigit
```

Here `<evidence>` is the run's temporary evidence directory printed below.

| Build / implementation | Three actual samples (seconds) | Median (seconds) |
| --- | --- | ---: |
| Default, original | 4.281472, 4.294333, 4.266637 | 4.281472 |
| Default, local success cache | 0.450678, 0.456883, 0.448572 | 0.450678 |
| Release, original | 0.828290, 0.392775, 0.399995 | 0.399995 |
| Release, local success cache | 0.076821, 0.073292, 0.072343 | 0.073292 |

Repeated validation is material in this bounded fixture. `loadHistory` now shares successfully validated blob IDs among its snapshot reads, and discards the set on return. Full hash/type/size checks precede insertion; failures are never cached. Snapshot hashes/types/paths/reference IDs are still checked. This avoids persistent state and an invalidation framework; separate passes in a command can still repeat work. The benchmark corrupted the shared object between commands and both original/final executables detected it. A CLI regression also corrupts a shared blob that remains only in history, verifies rejection, restores it, and verifies a fresh command succeeds. These are small-fixture observations, not a claim about large repositories or a newly invented production performance defect.

### Demos, artifacts, and remaining limits

Both exact README demo command bodies were executed together under `/bin/sh` with `set -eu` for each final build, in managed temporary repositories. Only the generated temporary-directory setup and executable PATH were supplied by the harness. All commands succeeded; final story/cast/setting bytes and absence of active merge state were checked. That is two demo sections per build, four successful executions total.

Raw baseline/final CTest logs, command output, benchmark samples, demo logs, v1 compatibility evidence, the NDEBUG negative check, and a non-Git source diff were retained during this run at:

```text
/private/var/folders/lt/hn707n0x3w71_b9bss4qk46c0000gn/T/minigit-validation-5ewt774d
```

Temporary evidence may eventually be cleaned by the operating system; the counts, commands, and samples above are retained in this source documentation. Normal generated logs remain in each build directory. The refreshed ZIP contains the application, tests (including the benchmark), docs, configuration, and unchanged PicoSHA2/license, with no tooling/build/cache/compiled files.

Optional line-level merging, recursive virtual bases, hard-link preservation, persistent caches, and broader architectural rewrites were left unchanged to preserve the educational scope and avoid additional public behavior/invalidation designs. Full power-loss durability (`fsync`), hostile filesystem races/external mutation during a command, Windows replacement semantics, and ACL/versioned permission preservation remain outside the existing guarantees. No required macOS check remains outstanding; rerun the documented commands on another target filesystem/OS to validate its behavior. The work does not establish that the project is bug-free.
