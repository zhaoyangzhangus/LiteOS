#!/usr/bin/env python3
"""Publish a stripped Chromium binary without destroying the previous payload."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def fingerprint(path):
    stat = path.stat()
    return stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns


def effective_executable_mode(directory):
    """Measure chmod semantics on the destination, including FAT/NTFS masks."""
    fd, name = tempfile.mkstemp(prefix='.chrome-mode-', dir=directory)
    os.close(fd)
    probe = Path(name)
    try:
        probe.chmod(0o755)
        mode = probe.stat().st_mode & 0o7777
        if not mode & 0o111:
            raise ValueError('destination cannot represent an executable file')
        return mode
    finally:
        probe.unlink(missing_ok=True)


def validate_executable(path):
    """Reject foreign, truncated or non-loadable ELF before publication."""
    with Path(path).open('rb') as stream:
        header = stream.read(64)
        if len(header) != 64 or header[:7] != b'\x7fELF\x02\x01\x01':
            raise ValueError('expected little-endian ELF64 executable: ' + str(path))
        kind, machine, version = struct.unpack_from('<HHI', header, 16)
        phoff = struct.unpack_from('<Q', header, 32)[0]
        ehsize, phsize, phnum = struct.unpack_from('<HHH', header, 52)
        size = os.fstat(stream.fileno()).st_size
        if (kind not in (2, 3) or machine != 62 or version != 1 or ehsize != 64 or
                phsize != 56 or not phnum or phoff < 64 or phoff + phsize * phnum > size):
            raise ValueError('invalid x86-64 executable header: ' + str(path))
        stream.seek(phoff)
        loadable = False
        for _ in range(phnum):
            program = stream.read(phsize)
            if len(program) != phsize:
                raise ValueError('truncated ELF program header: ' + str(path))
            segment, _, offset, _, _, filesz, memsz, _ = struct.unpack('<IIQQQQQQ', program)
            if offset + filesz > size or (segment == 1 and filesz > memsz):
                raise ValueError('invalid ELF segment bounds: ' + str(path))
            loadable |= segment == 1
        if not loadable:
            raise ValueError('ELF executable has no loadable segments: ' + str(path))


def stage_binary(source, target, strip):
    source, target, strip = map(Path, (source, target, strip))
    watched = {path: fingerprint(path) for path in (source, strip)}
    validate_executable(source)
    def validate_inputs():
        for path, before in watched.items():
            if fingerprint(path) != before:
                raise RuntimeError('staging input changed: ' + str(path))
    signature = {'source': sha(source), 'strip': sha(strip),
                 'flags': ['--strip-unneeded'], 'mode': 0o755}
    validate_inputs()
    target.parent.mkdir(parents=True, exist_ok=True)
    executable_mode = effective_executable_mode(target.parent)
    receipt = target.with_name(target.name + '.staging.json')
    try:
        previous = json.loads(receipt.read_text())
    except (OSError, ValueError):
        previous = {}
    if (isinstance(previous, dict) and previous.get('inputs') == signature and
            target.is_file() and previous.get('output') == sha(target) and
            target.stat().st_mode & 0o7777 == executable_mode):
        validate_inputs()
        validate_executable(target)
        return False
    fd, name = tempfile.mkstemp(prefix='.' + target.name + '-stage-', dir=target.parent)
    os.close(fd)
    temporary = Path(name)
    receipt_temporary = None
    try:
        shutil.copyfile(source, temporary)
        subprocess.run([str(strip.resolve()), '--strip-unneeded', str(temporary)], check=True)
        validate_inputs()
        validate_executable(temporary)
        output = sha(temporary)
        temporary.chmod(0o755)
        if (not target.is_file() or sha(target) != output or
                target.stat().st_mode & 0o7777 != executable_mode):
            os.replace(temporary, target)
        fd, name = tempfile.mkstemp(prefix='.' + receipt.name + '-', dir=target.parent)
        receipt_temporary = Path(name)
        with os.fdopen(fd, 'w') as stream:
            json.dump({'inputs': signature, 'output': output}, stream, sort_keys=True)
        os.replace(receipt_temporary, receipt)
        return True
    finally:
        temporary.unlink(missing_ok=True)
        if receipt_temporary is not None:
            receipt_temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', required=True)
    parser.add_argument('--target', required=True)
    parser.add_argument('--strip', required=True)
    args = parser.parse_args()
    changed = stage_binary(args.source, args.target, args.strip)
    print('CHROME_BINARY_STAGE=' + ('updated' if changed else 'cached'))


if __name__ == '__main__':
    main()
