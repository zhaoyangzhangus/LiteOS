#!/usr/bin/env python3
"""Check exact runtime identities from ELF64 program headers, not sections."""
import struct
import sys
from pathlib import Path


def runtime_identity(path):
    data = Path(path).read_bytes()
    def unpack(fmt, offset):
        size = struct.calcsize(fmt)
        if offset < 0 or offset + size > len(data):
            raise ValueError('truncated ELF field')
        return struct.unpack_from(fmt, data, offset)
    if data[:7] != b'\x7fELF\x02\x01\x01':
        raise ValueError('expected little-endian ELF64')
    kind, machine, version = unpack('<HHI', 16)
    phoff, = unpack('<Q', 32)
    ehsize, phsize, phnum = unpack('<HHH', 52)
    if (kind not in (2, 3) or machine != 62 or version != 1 or ehsize != 64 or
            phsize != 56 or not phnum or phoff < 64):
        raise ValueError('invalid x86-64 ELF header')
    programs = [unpack('<IIQQQQQQ', phoff + i * phsize) for i in range(phnum)]
    for p in programs:
        if p[2] + p[5] > len(data) or (p[0] == 1 and p[5] > p[6]):
            raise ValueError('invalid ELF segment bounds')
    def unique(kind):
        found = [p for p in programs if p[0] == kind]
        if len(found) > 1:
            raise ValueError('duplicate ELF program header')
        return found[0] if found else None
    interpreter = unique(3)
    if interpreter:
        value = data[interpreter[2]:interpreter[2] + interpreter[5]]
        if not value.endswith(b'\0') or b'\0' in value[:-1]:
            raise ValueError('invalid interpreter terminator')
        interpreter = value[:-1].decode('ascii')
    dynamic = unique(2)
    if dynamic is None or dynamic[5] % 16:
        raise ValueError('missing or malformed dynamic segment')
    entries = []
    for offset in range(dynamic[2], dynamic[2] + dynamic[5], 16):
        tag, value = unpack('<qQ', offset)
        if tag == 0:
            break
        entries.append((tag, value))
    else:
        raise ValueError('unterminated dynamic segment')
    def singleton(tag):
        values = [value for key, value in entries if key == tag]
        if len(values) != 1:
            raise ValueError('missing or duplicate dynamic string metadata')
        return values[0]
    address, length = singleton(5), singleton(10)
    mappings = [p[2] + address - p[3] for p in programs if p[0] == 1 and
                p[3] <= address and address + length <= p[3] + p[5]]
    if len(mappings) != 1 or length == 0:
        raise ValueError('invalid dynamic string table mapping')
    strings = data[mappings[0]:mappings[0] + length]
    def string(offset):
        if offset >= len(strings):
            raise ValueError('dynamic string offset outside table')
        end = strings.find(b'\0', offset)
        if end < 0:
            raise ValueError('unterminated dynamic string')
        return strings[offset:end].decode('ascii')
    needed = [string(value) for tag, value in entries if tag == 1]
    sonames = [string(value) for tag, value in entries if tag == 14]
    if len(sonames) > 1:
        raise ValueError('duplicate SONAME')
    return interpreter, needed, sonames[0] if sonames else None


def verify(loader, library, application, expected_library):
    for path, soname in ((loader, 'ld-system.so.1'), (library, expected_library)):
        interpreter, needed, actual = runtime_identity(path)
        if interpreter is not None or needed or actual != soname:
            raise ValueError(f'{path}: expected standalone library SONAME {soname}')
    interpreter, needed, _ = runtime_identity(application)
    if interpreter != '/lib/ld-system.so.1':
        raise ValueError(f'{application}: incorrect native interpreter {interpreter!r}')
    if expected_library not in needed:
        raise ValueError(f'{application}: missing exact dependency {expected_library}')


if __name__ == '__main__':
    try:
        if len(sys.argv) != 5:
            raise ValueError('usage: verify-elf-runtime.py LOADER LIBRARY APP SONAME')
        verify(*sys.argv[1:])
    except (OSError, ValueError, UnicodeError) as error:
        sys.exit(f'ELF runtime identity: {error}')
