"""Bounded history-validation measurement; not a CTest or a production benchmark."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import struct
import subprocess
import tempfile
import time


def number(value):
    return struct.pack('>Q', value)


def field(value):
    if isinstance(value, str):
        value = value.encode()
    return number(len(value)) + value


def envelope(kind, contents):
    return field('MiniGit') + number(1) + field(kind) + field(contents)


def measure(executable, root, repetitions):
    timings = []
    for unused in range(repetitions):
        start = time.perf_counter()
        subprocess.run([executable, 'log'], cwd=root, check=True,
                       stdout=subprocess.DEVNULL, timeout=60)
        timings.append(time.perf_counter() - start)
    return {'seconds': timings, 'median_seconds': statistics.median(timings)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executables', nargs='+')
    parser.add_argument('--history', type=int, default=20)
    parser.add_argument('--mib', type=int, default=4)
    parser.add_argument('--repetitions', type=int, default=3)
    args = parser.parse_args()
    if not (1 <= args.history <= 100 and 1 <= args.mib <= 16 and 1 <= args.repetitions <= 10):
        parser.error('Use history 1..100, MiB 1..16, repetitions 1..10')
    executables = [str(Path(path).resolve()) for path in args.executables]
    with tempfile.TemporaryDirectory(prefix='minigit-benchmark-') as temporary:
        root = Path(temporary).resolve()
        subprocess.run([executables[0], 'init'], cwd=root, check=True, stdout=subprocess.DEVNULL)

        def put(kind, contents):
            data = envelope(kind, contents)
            object_id = hashlib.sha256(data).hexdigest()
            (root / '.minigit/objects' / object_id).write_bytes(data)
            return object_id

        # Independent v1 serialization avoids timing CLI fixture construction.
        shared = put('blob', b'x' * (args.mib * 1024 * 1024))
        tip = ''
        for index in range(args.history):
            counter = put('blob', str(index).encode())
            snapshot = put('snapshot', number(2) + field('counter') + field(counter)
                           + field('shared') + field(shared))
            parents = number(1) + field(tip) if tip else number(0)
            tip = put('commit', field(snapshot) + number(1234567890) + parents + field('fixture'))
        state = field('main') + number(1) + field('main') + field(tip) + field(snapshot) + number(0)
        (root / '.minigit/state').write_bytes(envelope('state', state))
        results = {executable: measure(executable, root, args.repetitions) for executable in executables}
        # Every executable must detect corruption in a fresh independent command.
        path = root / '.minigit/objects' / shared
        original = path.read_bytes()
        path.write_bytes(original[:-1] + b'y')
        for executable in executables:
            result = subprocess.run([executable, 'log'], cwd=root, capture_output=True, timeout=60)
            if result.returncode == 0 or b'hash mismatch' not in result.stderr:
                raise RuntimeError('Corruption was not detected: ' + executable)
        print(json.dumps({'shared_file_bytes': args.mib * 1024 * 1024,
                          'distinct_snapshots': args.history, 'command': 'log',
                          'results': results, 'between_command_corruption_detected': True}, indent=2))


if __name__ == '__main__':
    main()
