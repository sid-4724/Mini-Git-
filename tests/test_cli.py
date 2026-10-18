"""Small process-level harness. unittest assertions also run in Release builds."""
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

EXECUTABLE = str(Path(sys.argv.pop(1)).resolve())


def number(value):
    return struct.pack('>Q', value)


def field(value):
    if isinstance(value, str):
        value = value.encode('utf-8')
    return number(len(value)) + value


def envelope(kind, payload):
    return field('MiniGit') + number(1) + field(kind) + field(payload)


class Reader:
    def __init__(self, data):
        self.data = data
        self.position = 0

    def number(self):
        value = struct.unpack_from('>Q', self.data, self.position)[0]
        self.position += 8
        return value

    def field(self):
        length = self.number()
        value = self.data[self.position:self.position + length]
        self.position += length
        return value


def payload(data, kind):
    reader = Reader(data)
    if reader.field() != b'MiniGit' or reader.number() != 1 or reader.field() != kind.encode():
        raise RuntimeError('Unexpected test fixture format')
    return reader.field()


class RepositoryTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='minigit-test-')
        # macOS /var is a symlink; MiniGit deliberately rejects symlink components.
        self.root = Path(self.temporary.name).resolve()
        self.run_cli('init')

    def tearDown(self):
        self.temporary.cleanup()

    def run_cli(self, *arguments, fail=None, cwd=None, env=None):
        environment = os.environ.copy()
        environment.pop('MINIGIT_TEST_FAIL_POINT', None)
        environment.pop('MINIGIT_TEST_CRASH_POINT', None)
        environment.pop('MINIGIT_TEST_ROLLBACK_FAIL_POINT', None)
        if env:
            environment.update(env)
        result = subprocess.run([EXECUTABLE, *arguments], cwd=cwd or self.root,
                                env=environment, capture_output=True, text=True,
                                encoding='utf-8', errors='replace', timeout=30)
        if fail is None:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            messages = (fail,) if isinstance(fail, str) else fail
            for message in messages:
                self.assertIn(message, result.stderr)
        return result.stdout

    def write(self, path, contents):
        destination = self.root / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(contents.encode() if isinstance(contents, str) else contents)

    def commit_file(self, path='story.txt', contents='base\n', message='root'):
        self.write(path, contents)
        self.run_cli('add', '--', path)
        return self.commit(message)

    def commit(self, message):
        return self.run_cli('commit', '-m', message).split()[1]

    def state_data(self):
        return (self.root / '.minigit/state').read_bytes()

    def state_fields(self):
        reader = Reader(payload(self.state_data(), 'state'))
        current = reader.field().decode()
        branches = {}
        for unused in range(reader.number()):
            name = reader.field().decode()
            branches[name] = reader.field().decode()
        return current, branches, reader.field().decode(), reader

    def merge_fields(self):
        reader = self.state_fields()[3]
        if reader.number() == 0:
            return {'active': False}
        result = {'active': True}
        for name in ('ours', 'theirs', 'base', 'original_index', 'owned_snapshot'):
            result[name] = reader.field().decode()
        result['unresolved'] = [reader.field().decode() for unused in range(reader.number())]
        return result

    def working_files(self):
        return {path.relative_to(self.root).as_posix(): path.read_bytes()
                for path in self.root.rglob('*')
                if path.is_file() and '.minigit' not in path.relative_to(self.root).parts}

    def object_payload(self, object_id, kind):
        data = (self.root / '.minigit/objects' / object_id).read_bytes()
        self.assertEqual(hashlib.sha256(data).hexdigest(), object_id)
        return payload(data, kind)

    def snapshot(self, snapshot_id):
        reader = Reader(self.object_payload(snapshot_id, 'snapshot'))
        files = {}
        for unused in range(reader.number()):
            path = reader.field().decode()
            files[path] = reader.field().decode()
        return files

    def commit_data(self, commit_id):
        reader = Reader(self.object_payload(commit_id, 'commit'))
        snapshot_id = reader.field().decode()
        timestamp = reader.number()
        parents = [reader.field().decode() for unused in range(reader.number())]
        return snapshot_id, timestamp, parents, reader.field().decode()

    def put_object(self, kind, contents):
        data = envelope(kind, contents)
        object_id = hashlib.sha256(data).hexdigest()
        (self.root / '.minigit/objects' / object_id).write_bytes(data)
        return object_id

    def put_commit(self, parents, snapshot_id, message):
        contents = field(snapshot_id) + number(1234567890) + number(len(parents))
        contents += b''.join(field(parent) for parent in parents)
        return self.put_object('commit', contents + field(message))

    def replace_branches(self, branches, current=None, index_id=None):
        old_current, unused, old_index, reader = self.state_fields()
        contents = field(current or old_current) + number(len(branches))
        contents += b''.join(field(name) + field(tip) for name, tip in sorted(branches.items()))
        contents += field(index_id or old_index) + reader.data[reader.position:]
        (self.root / '.minigit/state').write_bytes(envelope('state', contents))


class StorageStagingTests(RepositoryTest):
    def test_repeated_init_preserves_history_and_subdirectories(self):
        tip = self.commit_file()
        before = self.state_data()
        self.run_cli('init')
        self.assertEqual(before, self.state_data())
        self.assertEqual(self.state_fields()[1]['main'], tip)
        (self.root / 'sub').mkdir()
        self.assertIn('On branch main', self.run_cli('status', cwd=self.root / 'sub'))
        self.run_cli('init', cwd=self.root / 'sub', fail='nested')

    def test_exact_empty_binary_and_staged_bytes(self):
        self.write('empty', b'')
        binary = bytes(range(256)) + b'\x00\xff\r\n'
        self.write('binary', binary)
        self.write('staged', 'first\n')
        self.run_cli('add', '.')
        self.write('staged', 'second\n')
        tip = self.commit('exact bytes | : \n message')
        files = self.snapshot(self.commit_data(tip)[0])
        self.assertEqual(self.object_payload(files['empty'], 'blob'), b'')
        self.assertEqual(self.object_payload(files['binary'], 'blob'), binary)
        self.assertEqual(self.object_payload(files['staged'], 'blob'), b'first\n')
        self.assertEqual((self.root / 'staged').read_bytes(), b'second\n')
        self.assertIn('modified "staged"', self.run_cli('status'))

    def test_added_staged_then_deleted_commits_stored_bytes(self):
        self.write('vanished', b'staged\x00bytes\n')
        self.run_cli('add', 'vanished')
        (self.root / 'vanished').unlink()
        status = self.run_cli('status')
        self.assertIn('Staged changes:\n  added "vanished"', status)
        self.assertIn('Unstaged changes:\n  deleted "vanished"', status)
        tip = self.commit('commit the stored addition')
        files = self.snapshot(self.commit_data(tip)[0])
        self.assertEqual(self.object_payload(files['vanished'], 'blob'), b'staged\x00bytes\n')
        self.assertFalse((self.root / 'vanished').exists())
        self.run_cli('restore', 'vanished')
        self.assertEqual((self.root / 'vanished').read_bytes(), b'staged\x00bytes\n')

    def test_status_add_modify_delete_and_untracked(self):
        self.commit_file('modified', 'old')
        self.commit_file('deleted', 'old')
        self.write('modified', 'new')
        (self.root / 'deleted').unlink()
        self.write('added', 'new')
        self.write('untracked', 'keep out')
        before = self.run_cli('status')
        self.assertIn('modified "modified"', before)
        self.assertIn('deleted "deleted"', before)
        for path in ('modified', 'deleted', 'added'):
            self.run_cli('add', path)
        after = self.run_cli('status')
        self.assertIn('added "added"', after)
        self.assertIn('modified "modified"', after)
        self.assertIn('deleted "deleted"', after)
        tip = self.commit('changes')
        self.assertNotIn('untracked', self.snapshot(self.commit_data(tip)[0]))

    def test_hard_links_keep_distinct_requested_names(self):
        self.write('first.txt', b'original\n')
        try:
            os.link(self.root / 'first.txt', self.root / 'second.txt')
        except (OSError, NotImplementedError) as error:
            self.skipTest('Filesystem cannot create hard links: ' + str(error))
        initial = self.state_data()
        for requested in ('first.txt', 'second.txt'):
            with self.subTest(add=requested):
                (self.root / '.minigit/state').write_bytes(initial)
                self.run_cli('add', requested)
                files = self.snapshot(self.state_fields()[2])
                self.assertEqual(set(files), {requested})
                self.assertEqual(self.object_payload(files[requested], 'blob'), b'original\n')
        self.run_cli('add', '.')
        files = self.snapshot(self.state_fields()[2])
        self.assertEqual(set(files), {'first.txt', 'second.txt'})
        self.commit('both hard-link paths')
        before = self.state_data()
        for requested, other in (('first.txt', 'second.txt'), ('second.txt', 'first.txt')):
            with self.subTest(restore=requested):
                # Recreate the shared inode before each restore; checkout may split it.
                (self.root / 'second.txt').unlink()
                os.link(self.root / 'first.txt', self.root / 'second.txt')
                self.write('second.txt', b'dirty\n')
                output = self.run_cli('restore', requested)
                self.assertIn('Restored "' + requested + '"', output)
                self.assertEqual((self.root / requested).read_bytes(), b'original\n')
                self.assertEqual((self.root / other).read_bytes(), b'dirty\n')
                self.assertEqual(before, self.state_data())

    def test_spaces_unicode_separators_and_leading_dash(self):
        names = ('space name.txt', 'नमस्ते.txt', 'pipe|name=ok:yes.txt', '-dash', 'line\nbreak')
        for path in names:
            self.write(path, path)
            self.run_cli('add', '--', path)
        self.run_cli('add', '-dash', fail='leading-dash')
        tip = self.commit('filename coverage')
        files = self.snapshot(self.commit_data(tip)[0])
        self.assertEqual(set(files), set(names))
        for name in names:
            self.assertEqual(self.object_payload(files[name], 'blob'), name.encode('utf-8'))

    def test_metadata_excluded_and_empty_commits_rejected(self):
        (self.root / '.git').mkdir()
        self.write('.git/personal-data', 'do not touch')
        self.run_cli('add', '.',)
        self.run_cli('commit', '-m', 'nothing', fail='Nothing staged')
        self.run_cli('commit', '-m', '  \n', fail='non-whitespace')
        self.run_cli('add', '.git', fail='Unsafe')
        self.run_cli('add', '.minigit', fail='Unsafe')
        self.run_cli('add', '.GIT', fail='Unsafe')
        self.assertEqual((self.root / '.git/personal-data').read_text(), 'do not touch')


class HistoryDiffTests(RepositoryTest):
    def test_log_parent_order_messages_and_process_persistence(self):
        first = self.commit_file(message='root | message\nwith newline')
        second = self.commit_file(contents='second\n', message='second')
        third = self.commit_file(contents='third\n', message='third')
        log = self.run_cli('log')
        ids = [line.split()[1] for line in log.splitlines() if line.startswith('commit ')]
        self.assertEqual(ids, [third, second, first])
        self.assertIn('parents: ' + second, log)
        self.assertIn('timestamp:', log)
        self.assertIn('root | message\\nwith newline', log)

    def test_restore_index_source_and_absence(self):
        first = self.commit_file(contents='first\n')
        self.write('story.txt', 'staged\n')
        self.run_cli('add', 'story.txt')
        self.write('story.txt', 'unstaged\n')
        before = self.state_data()
        self.run_cli('restore', 'story.txt')
        self.assertEqual((self.root / 'story.txt').read_text(), 'staged\n')
        self.assertEqual(before, self.state_data())
        self.run_cli('restore', '--source', first, 'story.txt')
        self.assertEqual((self.root / 'story.txt').read_text(), 'first\n')
        self.assertEqual(before, self.state_data())
        self.run_cli('restore', 'missing', fail='absent')
        self.commit('second')
        self.run_cli('restore', 'story.txt')
        self.commit_file('later', 'added')
        self.run_cli('restore', '--source', first, 'later')
        self.assertFalse((self.root / 'later').exists())

    def test_restore_refuses_untracked_overwrite(self):
        first = self.commit_file('gone', 'original')
        (self.root / 'gone').unlink()
        self.run_cli('add', 'gone')
        staged_deletion_state = self.state_data()
        self.write('gone', 'untracked after staged deletion')
        self.run_cli('restore', '--source', first, 'gone', fail='Untracked')
        self.assertEqual(staged_deletion_state, self.state_data())
        self.assertEqual((self.root / 'gone').read_text(), 'untracked after staged deletion')
        (self.root / 'gone').unlink()
        self.commit('delete')
        self.write('gone', 'untracked treasure')
        self.run_cli('restore', '--source', first, 'gone', fail='Untracked')
        self.assertEqual((self.root / 'gone').read_text(), 'untracked treasure')
        (self.root / 'gone').unlink()
        self.run_cli('restore', '--source', first, 'gone')
        self.assertEqual((self.root / 'gone').read_text(), 'original')

    def test_diff_add_delete_empty_binary_repeated_and_no_newline(self):
        self.write('empty', b'')
        self.write('text', 'a\nb\na\n')
        self.write('binary', b'\x00\xff')
        self.run_cli('add', '.')
        added = self.run_cli('diff', '--staged')
        self.assertIn('[empty file added]', added)
        self.assertIn('+a\n+b\n+a\n', added)
        self.assertIn('Binary contents differ', added)
        self.commit('files')
        self.write('text', 'a\na\nc')
        diff = self.run_cli('diff')
        self.assertIn('-b\n', diff)
        self.assertIn('+c\n', diff)
        self.assertIn('No newline at end of file', diff)
        before = (self.root / 'text').read_bytes()
        self.run_cli('diff')
        self.assertEqual((self.root / 'text').read_bytes(), before)
        (self.root / 'empty').unlink()
        (self.root / 'text').unlink()
        self.run_cli('add', '.')
        deleted = self.run_cli('diff', '--staged')
        self.assertIn('[empty file deleted]', deleted)
        self.assertIn('-a\n-b\n-a\n', deleted)

    def test_diff_limits_before_quadratic_allocation(self):
        self.commit_file(contents='x\n' * 1100)
        self.write('story.txt', 'y\n' * 1100)
        self.assertIn('LCS table exceeds', self.run_cli('diff'))
        self.write('story.txt', 'z' * (1024 * 1024 + 1))
        self.assertIn('combined text exceeds', self.run_cli('diff'))
        self.write('story.txt', '\n' * 20001)
        self.assertIn('Detailed diff omitted', self.run_cli('diff'))


class BranchTests(RepositoryTest):
    def test_branch_creation_and_independent_advancement(self):
        self.run_cli('branch', 'early', fail='first commit')
        first = self.commit_file()
        self.run_cli('branch', 'feature')
        self.run_cli('branch', 'feature', fail='already exists')
        for invalid in ('../bad', 'with space', '.hidden', '-dash', 'x/y'):
            self.run_cli('branch', invalid, fail='name')
        self.run_cli('switch', 'feature')
        second = self.commit_file(contents='feature\n')
        self.assertEqual(self.state_fields()[1], {'feature': second, 'main': first})
        self.assertIn('* feature', self.run_cli('branch'))
        self.run_cli('switch', 'main')
        self.assertEqual((self.root / 'story.txt').read_text(), 'base\n')
        self.assertEqual(self.state_fields()[0], 'main')

    def test_switch_restores_removes_and_preserves_untracked(self):
        self.commit_file()
        self.run_cli('branch', 'feature')
        self.run_cli('switch', 'feature')
        self.commit_file('feature-only', 'feature')
        self.write('untracked', 'keep')
        self.run_cli('switch', 'main')
        self.assertFalse((self.root / 'feature-only').exists())
        self.assertEqual((self.root / 'untracked').read_text(), 'keep')
        self.run_cli('switch', 'feature')
        self.assertEqual((self.root / 'feature-only').read_text(), 'feature')

    def test_dirty_state_prevents_switch(self):
        self.commit_file()
        self.run_cli('branch', 'feature')
        before = self.state_data()
        self.write('story.txt', 'dirty')
        self.run_cli('switch', 'feature', fail='Unstaged')
        self.assertEqual(before, self.state_data())
        self.run_cli('add', 'story.txt')
        self.run_cli('switch', 'feature', fail='Staged')
        self.assertEqual((self.root / 'story.txt').read_text(), 'dirty')

    def test_untracked_file_and_directory_collisions_preflight(self):
        self.commit_file()
        self.run_cli('branch', 'feature')
        self.run_cli('switch', 'feature')
        self.commit_file('target', 'tracked target')
        self.commit_file('another', 'tracked another')
        self.run_cli('switch', 'main')
        before = self.state_data()
        self.write('target', 'untracked')
        self.run_cli('switch', 'feature', fail='Untracked')
        self.assertEqual(before, self.state_data())
        self.assertFalse((self.root / 'another').exists())
        self.assertEqual((self.root / 'target').read_text(), 'untracked')
        (self.root / 'target').unlink()
        (self.root / 'target').mkdir()
        self.run_cli('switch', 'feature', fail='directory collision')
        self.assertEqual(before, self.state_data())
        self.assertFalse((self.root / 'another').exists())

    def test_file_directory_transitions_and_extra_directory_collision(self):
        self.commit_file('item', 'file')
        self.run_cli('branch', 'directory')
        self.run_cli('switch', 'directory')
        (self.root / 'item').unlink()
        self.write('item/child', 'child')
        self.run_cli('add', '.')
        self.commit('directory replacement')
        self.write('item/extra', 'untracked')
        self.run_cli('switch', 'main', fail='Untracked')
        self.assertEqual((self.root / 'item/extra').read_text(), 'untracked')
        (self.root / 'item/extra').unlink()
        self.run_cli('switch', 'main')
        self.assertEqual((self.root / 'item').read_text(), 'file')
        self.run_cli('switch', 'directory')
        self.assertEqual((self.root / 'item/child').read_text(), 'child')

    def test_deletion_checks_and_simple_common_ancestor(self):
        root = self.commit_file()
        self.run_cli('branch', 'feature')
        self.run_cli('switch', 'feature')
        self.commit_file(contents='feature\n')
        self.run_cli('switch', 'main')
        self.commit_file('main-only', 'main')
        self.assertEqual(self.run_cli('merge-base', 'main', 'feature').strip(), root)
        self.run_cli('branch', '-d', 'main', fail='current branch')
        self.run_cli('branch', '-d', 'feature', fail='unmerged history')
        self.run_cli('branch', 'safe')
        self.run_cli('branch', '-d', 'safe')
        self.assertNotIn('safe', self.state_fields()[1])


class MergeTests(RepositoryTest):
    def divergent(self, base='base\n', ours='ours\n', theirs='theirs\n'):
        root_id = self.commit_file(contents=base)
        self.run_cli('branch', 'feature')
        our_id = self.commit_file(contents=ours, message='ours')
        self.run_cli('switch', 'feature')
        their_id = self.commit_file(contents=theirs, message='theirs')
        self.run_cli('switch', 'main')
        return root_id, our_id, their_id

    def test_already_up_to_date_and_fast_forward(self):
        root_id = self.commit_file()
        self.run_cli('branch', 'feature')
        before = self.state_data()
        self.assertIn('Already up to date', self.run_cli('merge', 'feature'))
        self.assertEqual(before, self.state_data())
        self.run_cli('switch', 'feature')
        tip = self.commit_file(contents='feature\n')
        self.run_cli('switch', 'main')
        self.assertIn('Fast-forwarded', self.run_cli('merge', 'feature'))
        self.assertEqual(self.state_fields()[1], {'feature': tip, 'main': tip})
        self.assertEqual(self.commit_data(tip)[2], [root_id])
        self.assertEqual((self.root / 'story.txt').read_text(), 'feature\n')
        self.assertIn('Already up to date', self.run_cli('merge', 'feature'))

    def test_conflict_free_merge_different_files_and_two_parents(self):
        self.commit_file()
        self.run_cli('branch', 'feature')
        our_id = self.commit_file('ours', 'ours')
        self.run_cli('switch', 'feature')
        their_id = self.commit_file('theirs', 'theirs')
        self.run_cli('switch', 'main')
        self.write('untracked', 'preserve')
        self.assertIn('two-parent', self.run_cli('merge', 'feature'))
        merged_id = self.state_fields()[1]['main']
        self.assertEqual(self.commit_data(merged_id)[2], [our_id, their_id])
        self.assertEqual(self.state_fields()[1]['feature'], their_id)
        self.assertEqual((self.root / 'ours').read_text(), 'ours')
        self.assertEqual((self.root / 'theirs').read_text(), 'theirs')
        self.assertEqual((self.root / 'untracked').read_text(), 'preserve')

    def test_text_conflict_requires_explicit_staging_and_original_parents(self):
        unused, our_id, their_id = self.divergent()
        output = self.run_cli('merge', 'feature')
        self.assertIn('Text conflict', output)
        marked = (self.root / 'story.txt').read_text()
        self.assertEqual(marked, '<<<<<<< ours\nours\n=======\ntheirs\n>>>>>>> theirs\n')
        self.run_cli('commit', '-m', 'unresolved', fail='unresolved')
        self.assertIn('"story.txt"', self.run_cli('status'))
        self.run_cli('switch', 'feature', fail='unfinished merge')
        self.write('story.txt', 'resolved\n')
        self.run_cli('commit', '-m', 'not staged', fail='unresolved')
        self.run_cli('add', 'story.txt')
        # An unstaged edit after resolution is preserved by the merge commit.
        self.write('story.txt', 'unstaged after resolution\n')
        merged_id = self.commit('resolved merge')
        self.assertEqual(self.commit_data(merged_id)[2], [our_id, their_id])
        snapshot = self.snapshot(self.commit_data(merged_id)[0])
        self.assertEqual(self.object_payload(snapshot['story.txt'], 'blob'), b'resolved\n')
        self.assertEqual((self.root / 'story.txt').read_text(), 'unstaged after resolution\n')

    def test_staging_is_resolution_even_with_marker_strings(self):
        unused, our_id, their_id = self.divergent()
        self.run_cli('merge', 'feature')
        marked = (self.root / 'story.txt').read_bytes()
        self.assertIn(b'<<<<<<< ours', marked)
        self.run_cli('add', 'story.txt')
        tip = self.commit('explicit user acceptance of markers')
        snapshot_id, unused, parents, unused = self.commit_data(tip)
        self.assertEqual(parents, [our_id, their_id])
        files = self.snapshot(snapshot_id)
        self.assertEqual(self.object_payload(files['story.txt'], 'blob'), marked)
        self.assertEqual(self.merge_fields(), {'active': False})
        self.assertNotIn('Merge in progress', self.run_cli('status'))

    def test_conflicted_merge_exits_zero_and_persists_unresolved_state(self):
        base, ours, theirs = self.divergent()
        self.run_cli('merge', 'feature')  # run_cli explicitly asserts exit status 0.
        merge = self.merge_fields()
        self.assertTrue(merge['active'])
        self.assertEqual((merge['base'], merge['ours'], merge['theirs']), (base, ours, theirs))
        self.assertEqual(merge['unresolved'], ['story.txt'])
        self.assertEqual(self.state_fields()[1]['main'], ours)
        self.assertIn('Merge in progress', self.run_cli('status'))
        self.run_cli('commit', '-m', 'not resolved', fail='unresolved')
        self.write('story.txt', 'resolved\n')
        self.run_cli('add', 'story.txt')
        tip = self.commit('complete the merge')
        self.assertEqual(self.commit_data(tip)[2], [ours, theirs])
        self.assertEqual(self.merge_fields(), {'active': False})

    def test_binary_conflict_restore_theirs_and_resolve(self):
        unused, our_id, their_id = self.divergent(b'\x00base', b'\x00ours', b'\x00theirs')
        output = self.run_cli('merge', 'feature')
        self.assertIn('Binary conflict', output)
        self.assertIn('--source ' + their_id, output)
        self.assertEqual((self.root / 'story.txt').read_bytes(), b'\x00ours')
        self.run_cli('restore', '--source', their_id, 'story.txt')
        self.assertEqual((self.root / 'story.txt').read_bytes(), b'\x00theirs')
        self.run_cli('commit', '-m', 'still unresolved', fail='unresolved')
        self.run_cli('add', 'story.txt')
        merged = self.commit('binary resolution')
        self.assertEqual(self.commit_data(merged)[2], [our_id, their_id])

    def test_binary_conflict_suggestion_quotes_literal_shell_characters(self):
        original_root = self.root
        names = ('cash$MINIGIT_REVIEW_SUFFIX.bin', "quote'\" space\nname.bin",
                 '`touch MINIGIT_BACKTICK`$(touch MINIGIT_SUBST).bin')
        try:
            for name in names:
                with self.subTest(filename=name):
                    temporary = tempfile.TemporaryDirectory(prefix='minigit-shell-test-')
                    self.addCleanup(temporary.cleanup)
                    self.root = Path(temporary.name).resolve()
                    self.run_cli('init')
                    self.commit_file('unrelated', b'keep\x00exact')
                    self.commit_file(name, b'\x00base')
                    self.run_cli('branch', 'feature')
                    our_id = self.commit_file(name, b'\x00ours', 'ours')
                    self.run_cli('switch', 'feature')
                    their_id = self.commit_file(name, b'\x00theirs', 'theirs')
                    self.run_cli('switch', 'main')
                    self.write('cashEXPANDED.bin', b'decoy')
                    output = self.run_cli('merge', 'feature')
                    before = self.state_fields()[:3]
                    merge_before = self.merge_fields()
                    merge_before.pop('owned_snapshot')
                    command = output.split('To use theirs: ', 1)[1].split(
                        '\nStage every', 1)[0]
                    environment = os.environ.copy()
                    environment['PATH'] = str(Path(EXECUTABLE).parent) + os.pathsep + environment['PATH']
                    environment['MINIGIT_REVIEW_SUFFIX'] = 'EXPANDED'
                    result = subprocess.run(['/bin/sh', '-c', command], cwd=self.root,
                                            env=environment, capture_output=True, timeout=30)
                    self.assertFalse((self.root / 'MINIGIT_BACKTICK').exists())
                    self.assertFalse((self.root / 'MINIGIT_SUBST').exists())
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual((self.root / name).read_bytes(), b'\x00theirs')
                    self.assertEqual((self.root / 'cashEXPANDED.bin').read_bytes(), b'decoy')
                    self.assertEqual((self.root / 'unrelated').read_bytes(), b'keep\x00exact')
                    self.assertEqual(before, self.state_fields()[:3])
                    merge_after = self.merge_fields()
                    merge_after.pop('owned_snapshot')
                    self.assertEqual(merge_before, merge_after)
                    self.run_cli('commit', '-m', 'still unresolved', fail='unresolved')
                    self.run_cli('add', '--', name)
                    merged = self.commit('literal filename resolved')
                    self.assertEqual(self.commit_data(merged)[2], [our_id, their_id])
        finally:
            self.root = original_root

    def test_modify_delete_and_deletion_resolution(self):
        self.commit_file()
        self.run_cli('branch', 'feature')
        our_id = self.commit_file(contents='ours\n')
        self.run_cli('switch', 'feature')
        (self.root / 'story.txt').unlink()
        self.run_cli('add', 'story.txt')
        their_id = self.commit('delete theirs')
        self.run_cli('switch', 'main')
        self.run_cli('merge', 'feature')
        self.assertIn('(file deleted)', (self.root / 'story.txt').read_text())
        (self.root / 'story.txt').unlink()
        self.run_cli('add', 'story.txt')
        merged = self.commit('resolve as deletion')
        self.assertEqual(self.commit_data(merged)[2], [our_id, their_id])
        self.assertNotIn('story.txt', self.snapshot(self.commit_data(merged)[0]))

    def test_binary_delete_modify_restore_then_abort(self):
        self.commit_file(contents=b'\x00base')
        self.run_cli('branch', 'feature')
        (self.root / 'story.txt').unlink()
        self.run_cli('add', 'story.txt')
        self.commit('ours deleted')
        self.run_cli('switch', 'feature')
        their_id = self.commit_file(contents=b'\x00theirs')
        self.run_cli('switch', 'main')
        before = self.state_data()
        self.run_cli('merge', 'feature')
        self.assertFalse((self.root / 'story.txt').exists())
        self.run_cli('status')
        self.run_cli('restore', '--source', their_id, 'story.txt')
        self.run_cli('merge', '--abort')
        self.assertEqual(before, self.state_data())
        self.assertFalse((self.root / 'story.txt').exists())

    def test_add_add_conflict_and_multiple_unresolved_paths(self):
        self.commit_file()
        self.run_cli('branch', 'feature')
        self.commit_file('new', 'ours')
        self.commit_file('second', 'ours')
        self.run_cli('switch', 'feature')
        self.commit_file('new', 'theirs')
        self.commit_file('second', 'theirs')
        self.run_cli('switch', 'main')
        self.run_cli('merge', 'feature')
        self.run_cli('add', 'new')
        self.run_cli('commit', '-m', 'one unresolved', fail='unresolved')
        self.run_cli('add', 'second')
        self.commit('both resolved')

    def test_identical_changes_and_one_sided_additions_deletions(self):
        self.commit_file()
        self.commit_file('removed-by-ours', 'old')
        self.commit_file('removed-by-theirs', 'old')
        self.run_cli('branch', 'feature')
        self.write('story.txt', 'identical\n')
        (self.root / 'removed-by-ours').unlink()
        self.write('ours-only', 'new')
        self.run_cli('add', '.')
        self.commit('ours')
        self.run_cli('switch', 'feature')
        self.write('story.txt', 'identical\n')
        (self.root / 'removed-by-theirs').unlink()
        self.write('theirs-only', 'new')
        self.run_cli('add', '.')
        self.commit('theirs')
        self.run_cli('switch', 'main')
        self.run_cli('merge', 'feature')
        self.assertEqual((self.root / 'story.txt').read_text(), 'identical\n')
        self.assertFalse((self.root / 'removed-by-ours').exists())
        self.assertFalse((self.root / 'removed-by-theirs').exists())
        self.assertTrue((self.root / 'ours-only').exists())
        self.assertTrue((self.root / 'theirs-only').exists())

    def test_abort_restores_index_and_preserves_unrelated_files(self):
        self.divergent()
        self.run_cli('switch', 'feature')
        self.commit_file('created-by-merge', 'theirs')
        self.run_cli('switch', 'main')
        before = self.state_data()
        self.write('untracked-before', 'before')
        self.run_cli('merge', 'feature')
        self.write('story.txt', 'edited conflict')
        self.run_cli('add', 'story.txt')
        self.write('untracked-after', 'after')
        self.run_cli('add', 'untracked-after')
        self.run_cli('merge', '--abort')
        self.assertEqual(before, self.state_data())
        self.assertEqual((self.root / 'story.txt').read_text(), 'ours\n')
        self.assertFalse((self.root / 'created-by-merge').exists())
        self.assertEqual((self.root / 'untracked-before').read_text(), 'before')
        self.assertEqual((self.root / 'untracked-after').read_text(), 'after')
        self.run_cli('merge', '--abort', fail='No merge')

    def test_abort_refuses_new_untracked_collision_and_keeps_merge_state(self):
        self.divergent()
        self.commit_file('deleted-by-merge', 'ours')
        # Make the path part of the common base, then diverge again.
        self.run_cli('merge', 'feature')
        self.run_cli('add', 'story.txt')
        self.commit('combined base')
        self.run_cli('branch', 'next')
        self.commit_file(contents='new ours')
        self.run_cli('switch', 'next')
        self.write('story.txt', 'new theirs')
        (self.root / 'deleted-by-merge').unlink()
        self.run_cli('add', '.')
        self.commit('theirs deletes')
        self.run_cli('switch', 'main')
        self.run_cli('merge', 'next')
        self.assertFalse((self.root / 'deleted-by-merge').exists())
        self.write('deleted-by-merge', 'untracked replacement')
        before = self.state_data()
        self.run_cli('merge', '--abort', fail='Untracked')
        self.assertEqual(before, self.state_data())
        self.assertEqual((self.root / 'deleted-by-merge').read_text(), 'untracked replacement')
        (self.root / 'deleted-by-merge').unlink()
        self.run_cli('merge', '--abort')
        self.assertEqual((self.root / 'deleted-by-merge').read_text(), 'ours')

    def test_divergent_file_directory_collision_refused_without_changes(self):
        self.commit_file()
        self.run_cli('branch', 'feature')
        self.commit_file('item', 'file')
        self.run_cli('switch', 'feature')
        self.commit_file('item/child', 'child')
        self.run_cli('switch', 'main')
        before = self.state_data()
        objects_before = set((self.root / '.minigit/objects').iterdir())
        self.run_cli('merge', 'feature', fail='file/directory layout collision')
        self.assertEqual(before, self.state_data())
        self.assertEqual(objects_before, set((self.root / '.minigit/objects').iterdir()))
        self.assertEqual((self.root / 'item').read_text(), 'file')

    def test_merge_dirty_and_untracked_collision_refusal(self):
        self.commit_file()
        self.run_cli('branch', 'feature')
        self.run_cli('switch', 'feature')
        self.commit_file('target', 'target')
        self.run_cli('switch', 'main')
        self.write('story.txt', 'dirty')
        self.run_cli('merge', 'feature', fail='Unstaged')
        self.run_cli('restore', 'story.txt')
        self.write('target', 'untracked')
        before = self.state_data()
        self.run_cli('merge', 'feature', fail='Untracked')
        self.assertEqual(before, self.state_data())
        self.assertEqual((self.root / 'target').read_text(), 'untracked')


class DagTests(RepositoryTest):
    def test_dag_ancestry_second_parent_log_uniqueness_and_safe_deletion(self):
        root_id = self.commit_file()
        self.run_cli('branch', 'feature')
        our_id = self.commit_file('ours', 'ours')
        self.run_cli('switch', 'feature')
        their_id = self.commit_file('theirs', 'theirs')
        self.run_cli('switch', 'main')
        self.run_cli('merge', 'feature')
        merged_id = self.state_fields()[1]['main']
        self.assertEqual(self.run_cli('merge-base', 'main', 'feature').strip(), their_id)
        ids = [line.split()[1] for line in self.run_cli('log').splitlines() if line.startswith('commit ')]
        self.assertEqual(ids, [merged_id, our_id, root_id, their_id])
        self.assertEqual(len(ids), len(set(ids)))
        self.run_cli('branch', '-d', 'feature')

    def test_criss_cross_returns_all_best_bases_and_merge_refuses(self):
        self.commit_file()
        self.run_cli('branch', 'a')
        self.run_cli('branch', 'b')
        self.run_cli('switch', 'a')
        a_id = self.commit_file('a-file', 'a')
        self.run_cli('branch', 'a-old')
        self.run_cli('switch', 'b')
        b_id = self.commit_file('b-file', 'b')
        self.run_cli('branch', 'b-old')
        self.run_cli('switch', 'a')
        self.run_cli('merge', 'b-old')
        self.run_cli('switch', 'b')
        self.run_cli('merge', 'a-old')
        self.assertEqual(self.run_cli('merge-base', 'a', 'b').splitlines(), sorted([a_id, b_id]))
        self.run_cli('switch', 'a')
        before = self.state_data()
        objects_before = set((self.root / '.minigit/objects').iterdir())
        files_before = self.working_files()
        self.run_cli('merge', 'b', fail='Multiple best common ancestors')
        self.assertEqual(files_before, self.working_files())
        self.assertEqual(before, self.state_data())
        self.assertEqual(objects_before, set((self.root / '.minigit/objects').iterdir()))

    def test_unrelated_histories_refused(self):
        main_id = self.commit_file()
        snapshot_id = self.commit_data(main_id)[0]
        independent = self.put_commit([], snapshot_id, 'independent root')
        self.replace_branches({'main': main_id, 'unrelated': independent})
        before = self.state_data()
        self.run_cli('merge-base', 'main', 'unrelated', fail='Unrelated histories')
        self.run_cli('merge', 'unrelated', fail='Unrelated histories')
        self.assertEqual(before, self.state_data())


class SafetyTests(RepositoryTest):
    def test_filesystem_spelling_and_alias_collision(self):
        self.commit_file('Case.txt', 'original')
        if not (self.root / 'case.txt').exists():
            # Case-sensitive filesystems can legitimately version both names.
            self.commit_file('case.txt', 'second file')
            self.assertEqual((self.root / 'Case.txt').read_text(), 'original')
            return
        before = self.state_data()
        self.run_cli('add', 'case.txt')
        self.assertEqual(before, self.state_data())
        tip = self.state_fields()[1]['main']
        blob_id = self.snapshot(self.commit_data(tip)[0])['Case.txt']
        alias_snapshot = self.put_object('snapshot', number(2) + field('Case.txt') + field(blob_id) + field('case.txt') + field(blob_id))
        alias_commit = self.put_commit([tip], alias_snapshot, 'alias fixture')
        self.replace_branches({'main': tip, 'aliases': alias_commit})
        before = self.state_data()
        self.run_cli('switch', 'aliases', fail='Filename collision')
        self.assertEqual(before, self.state_data())
        self.assertEqual((self.root / 'Case.txt').read_text(), 'original')

    def test_nonexact_hard_link_alias_is_refused_when_ambiguous(self):
        self.write('Case.txt', b'original')
        if not (self.root / 'CASE.TXT').exists():
            self.skipTest('Filesystem does not alias these case spellings')
        try:
            os.link(self.root / 'Case.txt', self.root / 'another.txt')
        except (OSError, NotImplementedError) as error:
            self.skipTest('Filesystem cannot create hard links: ' + str(error))
        before = self.state_data(), self.working_files()
        self.run_cli('add', 'CASE.TXT', fail='Ambiguous filesystem alias')
        self.assertEqual(before, (self.state_data(), self.working_files()))
        self.run_cli('add', 'Case.txt')
        self.assertEqual(set(self.snapshot(self.state_fields()[2])), {'Case.txt'})

    def test_unicode_normalization_alias_uses_existing_spelling(self):
        name = 'caf\u00e9.txt'
        alias = 'cafe\u0301.txt'
        self.write(name, b'exact bytes')
        actual = next(path.name for path in self.root.iterdir() if path.name != '.minigit')
        if not (self.root / alias).exists():
            self.write(alias, b'distinct bytes')
            self.run_cli('add', '.')
            files = self.snapshot(self.state_fields()[2])
            self.assertEqual(set(files), {name, alias})
            self.assertEqual(self.object_payload(files[name], 'blob'), b'exact bytes')
            self.assertEqual(self.object_payload(files[alias], 'blob'), b'distinct bytes')
        else:
            self.run_cli('add', alias)
            files = self.snapshot(self.state_fields()[2])
            self.assertEqual(set(files), {actual})
            self.assertEqual(self.object_payload(files[actual], 'blob'), b'exact bytes')

    def test_traversal_outside_paths_and_symlink_components(self):
        self.commit_file()
        for path in ('../escape', 'folder/../../escape'):
            self.run_cli('add', path, fail='traversal')
        outside = str(self.root.parent / 'minigit-outside-not-created')
        self.run_cli('add', outside, fail='outside the repository')
        self.run_cli('restore', outside, fail='outside the repository')
        (self.root / 'link').symlink_to(self.root / 'story.txt')
        self.run_cli('add', 'link', fail='Symlinks')
        self.run_cli('add', '.', fail='Symlinks')
        self.run_cli('restore', 'link', fail='Symlinks')
        (self.root / 'link').unlink()
        (self.root / 'linked-directory').symlink_to(self.root, target_is_directory=True)
        self.run_cli('add', 'linked-directory/story.txt', fail='Symlinks')
        (self.root / 'linked-directory').unlink()
        (self.root / 'dangling').symlink_to(self.root / 'absent')
        self.run_cli('add', 'dangling', fail='Symlinks')

    def test_absolute_inside_and_subdirectory_paths(self):
        self.write('sub/space file', 'bytes')
        self.run_cli('add', str(self.root / 'sub/space file'))
        self.commit('absolute inside')
        self.write('sub/space file', 'new')
        self.run_cli('add', 'space file', cwd=self.root / 'sub')
        self.commit('relative to current directory')
        self.run_cli('restore', 'space file', cwd=self.root / 'sub')
        self.assertEqual((self.root / 'sub/space file').read_text(), 'new')

    def test_corrupt_hash_and_missing_objects_fail_without_mutation(self):
        tip = self.commit_file()
        snapshot_id = self.commit_data(tip)[0]
        object_path = self.root / '.minigit/objects' / snapshot_id
        original = object_path.read_bytes()
        before = self.state_data()
        object_path.write_bytes(original[:-1])
        self.run_cli('status', fail='hash mismatch')
        self.assertEqual(before, self.state_data())
        object_path.unlink()
        self.run_cli('log', fail='Missing file or object')
        self.assertEqual(before, self.state_data())
        object_path.write_bytes(original)
        files = self.snapshot(snapshot_id)
        (self.root / '.minigit/objects' / files['story.txt']).unlink()
        self.run_cli('status', fail='Missing file or object')

    def test_shared_historical_blob_is_revalidated_between_commands(self):
        first = self.commit_file('shared', b'unchanged historical bytes')
        shared_id = self.snapshot(self.commit_data(first)[0])['shared']
        self.commit_file('counter', 'one')
        self.commit_file('counter', 'two')
        (self.root / 'shared').unlink()
        self.run_cli('add', 'shared')
        self.commit('shared only remains in history')
        self.run_cli('log')
        object_path = self.root / '.minigit/objects' / shared_id
        original = object_path.read_bytes()
        object_path.write_bytes(original[:-1] + b'!')
        before = self.state_data(), self.working_files()
        self.run_cli('log', fail='hash mismatch')
        self.assertEqual(before, (self.state_data(), self.working_files()))
        object_path.write_bytes(original)
        self.run_cli('log')

    def test_malformed_lengths_counts_versions_ids_and_types(self):
        initial = self.state_data()
        good_index = self.state_fields()[2]
        malformed = [
            (self.put_object('snapshot', number(1) + field('path')), 'Truncated'),
            (self.put_object('snapshot', number(2**64 - 1)), 'length/count'),
            (self.put_object('snapshot', number(1) + field('path') + field('bad')), 'Invalid object ID'),
            (self.put_object('blob', b'not a snapshot'), 'Incorrect object'),
        ]
        unsupported = field('MiniGit') + number(99) + field('snapshot') + field(number(0))
        unsupported_id = hashlib.sha256(unsupported).hexdigest()
        (self.root / '.minigit/objects' / unsupported_id).write_bytes(unsupported)
        malformed.append((unsupported_id, 'Unsupported'))
        for object_id, error in malformed:
            (self.root / '.minigit/state').write_bytes(initial)
            self.replace_branches({'main': ''}, index_id=object_id)
            self.run_cli('status', fail=error)
        (self.root / '.minigit/state').write_bytes(initial)
        self.assertEqual(self.state_fields()[2], good_index)

    def test_snapshot_paths_and_file_directory_collisions_are_validated(self):
        initial = self.state_data()
        blob_id = self.put_object('blob', b'content')
        for unsafe in ('../escape', '/absolute', '.minigit/state', '.git/config', 'dir/../file', 'dir//file', 'dir/./file', 'dir\\file'):
            bad_snapshot = self.put_object('snapshot', number(1) + field(unsafe) + field(blob_id))
            (self.root / '.minigit/state').write_bytes(initial)
            self.replace_branches({'main': ''}, index_id=bad_snapshot)
            self.run_cli('status', fail='path')
        collision = self.put_object('snapshot', number(2) + field('a') + field(blob_id) + field('a/b') + field(blob_id))
        (self.root / '.minigit/state').write_bytes(initial)
        self.replace_branches({'main': ''}, index_id=collision)
        self.run_cli('status', fail='File/directory collision')
        (self.root / '.minigit/state').write_bytes(initial)

    def test_invalid_missing_and_duplicate_parents(self):
        tip = self.commit_file()
        snapshot_id = self.commit_data(tip)[0]
        initial = self.state_data()
        missing = self.put_commit(['1' * 64], snapshot_id, 'missing parent')
        duplicate = self.put_commit([tip, tip], snapshot_id, 'duplicate parents')
        too_many = self.put_commit([tip, tip, tip], snapshot_id, 'too many')
        invalid = self.put_commit(['bad'], snapshot_id, 'invalid parent')
        for commit_id, error in ((missing, 'Missing file or object'), (duplicate, 'duplicate parent'), (too_many, 'length/count'), (invalid, 'Invalid object ID')):
            (self.root / '.minigit/state').write_bytes(initial)
            self.replace_branches({'main': commit_id})
            self.run_cli('log', fail=error)
            self.assertEqual((self.root / 'story.txt').read_text(), 'base\n')
        (self.root / '.minigit/state').write_bytes(initial)

    def test_metadata_symlinks_are_rejected(self):
        self.commit_file()
        objects = self.root / '.minigit/objects'
        saved = self.root / '.minigit/saved-objects'
        objects.rename(saved)
        objects.symlink_to(saved, target_is_directory=True)
        self.run_cli('status', fail='Symlinks')
        objects.unlink()
        saved.rename(objects)
        self.run_cli('status')

    def test_file_size_limit_leaves_index_unchanged(self):
        before = self.state_data()
        self.write('large', b'x' * (16 * 1024 * 1024 + 1))
        self.run_cli('add', 'large', fail='size limit')
        self.assertEqual(before, self.state_data())

    def test_file_size_limit_accepts_exact_boundary(self):
        data = b'x' * (16 * 1024 * 1024)
        self.write('at-limit', data)
        self.run_cli('add', 'at-limit')
        files = self.snapshot(self.state_fields()[2])
        self.assertEqual(self.object_payload(files['at-limit'], 'blob'), data)

    def test_oversized_object_and_journal_rejected_before_reading(self):
        before = self.state_data()
        object_id = '1' * 64
        path = self.root / '.minigit/objects' / object_id
        with path.open('wb') as output:
            output.truncate(32 * 1024 * 1024 + 1)
        self.replace_branches({'main': ''}, index_id=object_id)
        malformed_state = self.state_data()
        self.run_cli('status', fail='size limit')
        self.assertEqual(malformed_state, self.state_data())
        (self.root / '.minigit/state').write_bytes(before)
        journal = self.root / '.minigit/transaction'
        with journal.open('wb') as output:
            output.truncate(64 * 1024 * 1024 + 1)
        journal_stat = journal.stat()
        self.write('unrelated', b'unchanged')
        self.run_cli('recover', fail='size limit')
        self.assertEqual(journal.stat().st_size, journal_stat.st_size)
        self.assertEqual(journal.stat().st_mtime_ns, journal_stat.st_mtime_ns)
        self.assertEqual(before, self.state_data())
        self.assertEqual((self.root / 'unrelated').read_bytes(), b'unchanged')

    def test_live_write_lock_rejects_another_mutation(self):
        self.write('new', 'new')
        before = self.state_data()
        holder_code = (
            "from pathlib import Path; import sys; "
            "lock=Path(sys.argv[1]); lock.mkdir(); "
            "(lock/'owner').write_text('test lock holder'); "
            "print('ready', flush=True); sys.stdin.readline(); "
            "(lock/'owner').unlink(); lock.rmdir()"
        )
        holder = subprocess.Popen([sys.executable, '-c', holder_code, str(self.root / '.minigit/lock')],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            self.assertEqual(holder.stdout.readline().strip(), 'ready')
            self.run_cli('add', 'new', fail='locked')
            self.run_cli('init', fail='locked')
            self.assertEqual(before, self.state_data())
        finally:
            holder.communicate('\n', timeout=10)
        self.run_cli('add', 'new')

    def test_real_permission_errors_are_controlled(self):
        if hasattr(os, 'geteuid') and os.geteuid() == 0:
            self.skipTest('Root can bypass ordinary permission bits')
        self.commit_file()
        state_path = self.root / '.minigit/state'
        permissions = state_path.stat().st_mode & 0o777
        try:
            state_path.chmod(0)
            self.run_cli('status', fail='Cannot open')
        finally:
            state_path.chmod(permissions)
        self.write('story.txt', 'staged')
        self.run_cli('add', 'story.txt')
        before = self.state_data()
        objects = self.root / '.minigit/objects'
        permissions = objects.stat().st_mode & 0o777
        try:
            objects.chmod(0o500)
            self.run_cli('commit', '-m', 'cannot write object', fail='Cannot create')
            self.assertEqual(before, self.state_data())
        finally:
            objects.chmod(permissions)


class RecoveryTests(RepositoryTest):
    def test_journal_size_limit_refuses_before_working_writes(self):
        # Two 16 MiB files need 64 MiB of before/after bytes, plus metadata.
        old_bytes = b'o' * (16 * 1024 * 1024)
        new_bytes = b'n' * (16 * 1024 * 1024)
        old_blob = self.put_object('blob', old_bytes)
        new_blob = self.put_object('blob', new_bytes)
        def two_file_snapshot(blob):
            return self.put_object('snapshot', number(2) + field('a') + field(blob)
                                   + field('b') + field(blob))
        old_snapshot = two_file_snapshot(old_blob)
        new_snapshot = two_file_snapshot(new_blob)
        old_tip = self.put_commit([], old_snapshot, 'old')
        new_tip = self.put_commit([old_tip], new_snapshot, 'new')
        self.replace_branches({'main': old_tip, 'feature': new_tip}, index_id=old_snapshot)
        self.write('a', old_bytes)
        self.write('b', old_bytes)
        self.write('unrelated', b'preserve')
        before = self.state_data()
        objects_before = set((self.root / '.minigit/objects').iterdir())
        self.run_cli('switch', 'feature', fail='64 MiB recovery journal limit')
        self.assertEqual(before, self.state_data())
        self.assertEqual(objects_before, set((self.root / '.minigit/objects').iterdir()))
        self.assertEqual((self.root / 'a').read_bytes(), old_bytes)
        self.assertEqual((self.root / 'b').read_bytes(), old_bytes)
        self.assertEqual((self.root / 'unrelated').read_bytes(), b'preserve')
        self.assertFalse((self.root / '.minigit/transaction').exists())

    def test_journal_corruption_is_rejected_without_mutation(self):
        self.commit_file('f', b'ORIGINAL\n')
        self.run_cli('branch', 'feature')
        self.run_cli('switch', 'feature')
        self.commit_file('f', b'NEW DATA\n')
        self.run_cli('switch', 'main')
        self.write('unrelated', b'preserve\x00bytes')
        self.run_cli('switch', 'feature',
                     env={'MINIGIT_TEST_CRASH_POINT': 'before-state-write'}, fail='')
        self.remove_stale_test_lock()
        journal_path = self.root / '.minigit/transaction'
        valid = journal_path.read_bytes()
        for old, new in ((b'ORIGINAL\n', b'CORRUPT!\n'),
                         (b'NEW DATA\n', b'BAD DATA\n'),
                         (field('f'), field('g'))):
            with self.subTest(corrupted=old):
                self.assertIn(old, valid)
                damaged = valid.replace(old, new, 1)
                self.assertEqual(len(damaged), len(valid))
                journal_path.write_bytes(damaged)
                before = self.working_files(), self.state_data(), journal_path.read_bytes()
                self.run_cli('recover', fail='checksum mismatch')
                self.assertEqual(before, (self.working_files(), self.state_data(), journal_path.read_bytes()))
        journal_path.write_bytes(valid)
        self.run_cli('recover')
        self.assertEqual((self.root / 'f').read_bytes(), b'ORIGINAL\n')
        self.assertEqual((self.root / 'unrelated').read_bytes(), b'preserve\x00bytes')

    def test_journal_checksum_covers_states_permissions_and_directory_lists(self):
        self.commit_file('first', 'old first')
        self.commit_file('second/child', 'old child')
        self.run_cli('branch', 'feature')
        self.run_cli('switch', 'feature')
        (self.root / 'first').unlink()
        (self.root / 'second/child').unlink()
        (self.root / 'second').rmdir()
        self.write('first/child', 'new child')
        self.write('second', 'new second')
        self.run_cli('add', '.')
        self.commit('swap both directory shapes')
        self.run_cli('switch', 'main')
        self.run_cli('switch', 'feature',
                     env={'MINIGIT_TEST_CRASH_POINT': 'before-state-write'}, fail='')
        self.remove_stale_test_lock()
        path = self.root / '.minigit/transaction'
        valid = path.read_bytes()
        record = Reader(valid)
        record.field()
        self.assertEqual(record.number(), 2)
        record.field()
        contents = record.field()
        reader = Reader(contents)
        positions = []
        for unused in range(2):
            reader.field()
            positions.append(reader.position - 1)  # Each complete serialized state.
        for unused in range(reader.number()):
            reader.field()
            reader.number()
            reader.field()
            reader.number()
            positions.append(reader.position - 1)  # Permission bits, same encoded length.
            reader.number()
            reader.field()
        for unused in range(2):
            count = reader.number()
            self.assertGreater(count, 0)
            for unused in range(count):
                reader.field()
                positions.append(reader.position - 1)
        for position in positions:
            with self.subTest(payload_offset=position):
                damaged = bytearray(contents)
                damaged[position] ^= 1
                data = field('MiniGit') + number(2) + field('transaction') + field(bytes(damaged))
                data += field(hashlib.sha256(contents).hexdigest())
                path.write_bytes(data)
                before = self.working_files(), self.state_data(), path.read_bytes()
                self.run_cli('recover', fail='checksum mismatch')
                self.assertEqual(before, (self.working_files(), self.state_data(), path.read_bytes()))
        path.write_bytes(valid)
        self.run_cli('recover')
        self.assertEqual(self.working_files(), {'first': b'old first', 'second/child': b'old child'})

    def test_invalid_and_legacy_journals_are_preserved(self):
        self.feature()
        self.run_cli('switch', 'feature',
                     env={'MINIGIT_TEST_CRASH_POINT': 'before-state-write'}, fail='')
        self.remove_stale_test_lock()
        path = self.root / '.minigit/transaction'
        valid = path.read_bytes()
        reader = Reader(valid)
        self.assertEqual(reader.field(), b'MiniGit')
        self.assertEqual(reader.number(), 2)
        self.assertEqual(reader.field(), b'transaction')
        contents = reader.field()
        checksum = reader.field()
        self.assertEqual(checksum.decode(), hashlib.sha256(contents).hexdigest())
        self.assertEqual(reader.position, len(valid))
        unsupported = field('MiniGit') + number(99) + field('transaction') + field(contents) + field(checksum)
        legacy = envelope('transaction', contents)
        cases = [(valid[:length], 'Truncated') for length in (0, 18, len(valid) - 1)]
        cases += [(unsupported, 'Unsupported transaction journal version'),
                  (legacy, 'Legacy v1 journal has no checksum'),
                  (valid + b'junk', 'trailing bytes')]
        for damaged, message in cases:
            with self.subTest(error=message, size=len(damaged)):
                path.write_bytes(damaged)
                before = self.working_files(), self.state_data(), path.read_bytes()
                self.run_cli('recover', fail=message)
                self.assertEqual(before, (self.working_files(), self.state_data(), path.read_bytes()))
        path.write_bytes(valid)
        self.run_cli('recover')
        self.assertEqual((self.root / 'story.txt').read_bytes(), b'main\n')

    def test_published_journal_is_verified_before_cleanup(self):
        self.feature()
        self.run_cli('switch', 'feature',
                     env={'MINIGIT_TEST_CRASH_POINT': 'after-state-write'}, fail='')
        self.remove_stale_test_lock()
        path = self.root / '.minigit/transaction'
        valid = path.read_bytes()
        path.write_bytes(valid.replace(b'main\n', b'BAD!\n', 1))
        before = self.working_files(), self.state_data(), path.read_bytes()
        self.run_cli('recover', fail='checksum mismatch')
        self.assertEqual(before, (self.working_files(), self.state_data(), path.read_bytes()))
        path.write_bytes(valid)
        self.run_cli('recover')
        self.assertEqual((self.root / 'story.txt').read_bytes(), b'feature\n')

    def test_recovery_validates_referenced_objects_before_restoring(self):
        self.feature()
        self.run_cli('switch', 'feature', env={'MINIGIT_TEST_CRASH_POINT': 'after-working-write'}, fail='')
        self.remove_stale_test_lock()
        feature_id = self.state_fields()[1]['feature']
        object_path = self.root / '.minigit/objects' / feature_id
        original = object_path.read_bytes()
        current_files = {path: (self.root / path).read_bytes() for path in ('story.txt', 'feature-created/child') if (self.root / path).exists()}
        object_path.unlink()
        self.run_cli('recover', fail='Missing file or object')
        for path, data in current_files.items():
            self.assertEqual((self.root / path).read_bytes(), data)
        self.assertTrue((self.root / '.minigit/transaction').exists())
        object_path.write_bytes(original)
        self.run_cli('recover')
        self.assertEqual((self.root / 'story.txt').read_text(), 'main\n')

    def feature(self):
        self.commit_file(contents='main\n')
        self.run_cli('branch', 'feature')
        self.run_cli('switch', 'feature')
        self.commit_file(contents='feature\n')
        self.commit_file('feature-created/child', 'created')
        self.run_cli('switch', 'main')

    def remove_stale_test_lock(self):
        # Only called after subprocess.run confirms the injected crash exited.
        lock = self.root / '.minigit/lock'
        (lock / 'owner').unlink()
        lock.rmdir()

    def test_commit_write_failures_do_not_advance_branch(self):
        self.commit_file()
        self.write('story.txt', 'staged\n')
        self.run_cli('add', 'story.txt')
        before = self.state_data()
        for point in ('before-object-write', 'before-state-write'):
            self.run_cli('commit', '-m', point, env={'MINIGIT_TEST_FAIL_POINT': point}, fail='Simulated')
            self.assertEqual(before, self.state_data())
            self.assertEqual((self.root / 'story.txt').read_text(), 'staged\n')
        self.commit('works after failure')

    def test_switch_failure_rolls_back_state_contents_and_created_files(self):
        self.feature()
        before = self.state_data()
        self.write('untracked', 'preserve')
        for point in ('after-journal-write', 'after-working-write', 'before-state-write'):
            self.run_cli('switch', 'feature', env={'MINIGIT_TEST_FAIL_POINT': point}, fail='rolled back')
            self.assertEqual(before, self.state_data())
            self.assertEqual((self.root / 'story.txt').read_text(), 'main\n')
            self.assertFalse((self.root / 'feature-created/child').exists())
            self.assertEqual((self.root / 'untracked').read_text(), 'preserve')
            self.assertFalse((self.root / '.minigit/transaction').exists())
        self.run_cli('switch', 'feature')

    def test_rollback_failure_retains_journal_and_can_be_retried(self):
        self.feature()
        self.commit_file('z-existing', b'exact original\x00extra')
        (self.root / 'story.txt').chmod(0o640)
        self.write('unrelated', b'keep\x00untracked')
        before = self.state_data(), self.working_files()
        permissions = (self.root / 'story.txt').stat().st_mode & 0o777
        self.run_cli('switch', 'feature', fail=(
            'Recovery is incomplete',
            "Journal retained; back up your files and run 'minigit recover'."), env={
            'MINIGIT_TEST_FAIL_POINT': 'before-state-write',
            'MINIGIT_TEST_ROLLBACK_FAIL_POINT': 'after-first-backup-write',
        })
        self.assertTrue((self.root / '.minigit/transaction').exists())
        self.assertFalse((self.root / 'z-existing').exists())
        self.assertEqual(self.state_data(), before[0])
        self.assertEqual((self.root / 'unrelated').read_bytes(), b'keep\x00untracked')
        self.run_cli('status', fail='Interrupted transaction')
        self.assertIn('rolled back', self.run_cli('recover'))
        self.assertEqual(before, (self.state_data(), self.working_files()))
        self.assertEqual((self.root / 'story.txt').stat().st_mode & 0o777, permissions)
        self.assertFalse((self.root / '.minigit/transaction').exists())

    def test_file_directory_transition_failure_restores_original(self):
        self.commit_file('item', 'original')
        self.run_cli('branch', 'directory')
        self.run_cli('switch', 'directory')
        (self.root / 'item').unlink()
        self.write('item/child', 'child')
        self.run_cli('add', '.')
        self.commit('directory layout')
        directory_state = self.state_data()
        self.run_cli('switch', 'main', env={'MINIGIT_TEST_FAIL_POINT': 'after-working-write'}, fail='rolled back')
        self.assertEqual(directory_state, self.state_data())
        self.assertEqual((self.root / 'item/child').read_text(), 'child')
        self.run_cli('switch', 'main')
        file_state = self.state_data()
        self.run_cli('switch', 'directory', env={'MINIGIT_TEST_FAIL_POINT': 'after-working-write'}, fail='rolled back')
        self.assertEqual(file_state, self.state_data())
        self.assertEqual((self.root / 'item').read_text(), 'original')

    def test_restore_failure_preserves_original_dirty_contents(self):
        self.commit_file()
        self.write('story.txt', 'dirty contents')
        before = self.state_data()
        self.run_cli('restore', 'story.txt', env={'MINIGIT_TEST_FAIL_POINT': 'after-working-write'}, fail='rolled back')
        self.assertEqual(before, self.state_data())
        self.assertEqual((self.root / 'story.txt').read_text(), 'dirty contents')

    def test_fast_forward_merge_failure_preserves_tip_and_files(self):
        self.feature()
        before = self.state_data()
        self.run_cli('merge', 'feature', env={'MINIGIT_TEST_FAIL_POINT': 'before-state-write'}, fail='rolled back')
        self.assertEqual(before, self.state_data())
        self.assertEqual((self.root / 'story.txt').read_text(), 'main\n')
        self.assertFalse((self.root / 'feature-created/child').exists())

    def test_conflicted_merge_and_abort_failures_roll_back(self):
        self.feature()
        self.commit_file(contents='ours\n')
        before_merge = self.state_data()
        self.run_cli('merge', 'feature', env={'MINIGIT_TEST_FAIL_POINT': 'after-working-write'}, fail='rolled back')
        self.assertEqual(before_merge, self.state_data())
        self.assertEqual((self.root / 'story.txt').read_text(), 'ours\n')
        self.run_cli('merge', 'feature')
        self.write('story.txt', 'edited resolution')
        during_merge = self.state_data()
        self.run_cli('merge', '--abort', env={'MINIGIT_TEST_FAIL_POINT': 'before-state-write'}, fail='rolled back')
        self.assertEqual(during_merge, self.state_data())
        self.assertEqual((self.root / 'story.txt').read_text(), 'edited resolution')
        self.assertTrue((self.root / 'feature-created/child').exists())
        self.run_cli('merge', '--abort')
        self.assertEqual(before_merge, self.state_data())

    def test_abrupt_exit_before_publish_recovers_backups(self):
        self.feature()
        before = self.state_data()
        self.run_cli('switch', 'feature', env={'MINIGIT_TEST_CRASH_POINT': 'after-working-write'}, fail='')
        self.assertTrue((self.root / '.minigit/transaction').exists())
        self.run_cli('status', fail='locked')
        self.remove_stale_test_lock()
        self.run_cli('status', fail='Interrupted transaction')
        self.assertIn('rolled back', self.run_cli('recover'))
        self.assertEqual(before, self.state_data())
        self.assertEqual((self.root / 'story.txt').read_text(), 'main\n')
        self.assertFalse((self.root / 'feature-created/child').exists())
        self.run_cli('status')

    def test_abrupt_exit_after_publish_keeps_committed_state(self):
        self.feature()
        self.run_cli('switch', 'feature', env={'MINIGIT_TEST_CRASH_POINT': 'after-state-write'}, fail='')
        self.remove_stale_test_lock()
        self.assertIn('already published', self.run_cli('recover'))
        self.assertEqual(self.state_fields()[0], 'feature')
        self.assertEqual((self.root / 'story.txt').read_text(), 'feature\n')
        self.assertTrue((self.root / 'feature-created/child').exists())
        self.assertFalse((self.root / '.minigit/transaction').exists())

    def test_recovery_refuses_to_overwrite_post_crash_edits(self):
        self.feature()
        self.run_cli('switch', 'feature', env={'MINIGIT_TEST_CRASH_POINT': 'after-working-write'}, fail='')
        self.remove_stale_test_lock()
        self.write('feature-created/child', 'new edits after crash')
        self.run_cli('recover', fail='overwrite new edits')
        self.assertEqual((self.root / 'feature-created/child').read_text(), 'new edits after crash')
        self.assertTrue((self.root / '.minigit/transaction').exists())
        (self.root / 'feature-created/child').unlink()
        self.run_cli('recover')
        self.assertEqual((self.root / 'story.txt').read_text(), 'main\n')

    def test_same_state_restore_crash_is_conservatively_undone(self):
        self.commit_file()
        self.write('story.txt', 'dirty before restore')
        self.run_cli('restore', 'story.txt', env={'MINIGIT_TEST_CRASH_POINT': 'after-state-write'}, fail='')
        self.remove_stale_test_lock()
        self.run_cli('recover')
        self.assertEqual((self.root / 'story.txt').read_text(), 'dirty before restore')


class IntegrationTests(RepositoryTest):
    def test_complete_cli_workflow(self):
        self.write('story.txt', 'A traveller reaches a lighthouse.\n')
        self.run_cli('add', 'story.txt')
        root_id = self.commit('Opening scene')
        self.run_cli('branch', 'characters')
        self.run_cli('switch', 'characters')
        self.write('cast.txt', 'Mira: lighthouse keeper.\n')
        self.run_cli('add', 'cast.txt')
        tip = self.commit('Add a character')
        self.run_cli('switch', 'main')
        self.assertIn('Fast-forwarded', self.run_cli('merge', 'characters'))
        log = self.run_cli('log')
        self.assertIn(tip, log)
        self.assertIn(root_id, log)
        self.assertIn('Add a character', log)
        self.assertEqual((self.root / 'cast.txt').read_text(), 'Mira: lighthouse keeper.\n')
        self.assertIn('Staged changes:\n  (none)', self.run_cli('status'))


if __name__ == '__main__':
    unittest.main(verbosity=2)
