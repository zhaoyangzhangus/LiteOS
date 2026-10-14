#!/usr/bin/env python3
"""Remove only retired generated runtime aliases from an ESP staging tree."""
import argparse
from pathlib import Path

RETIRED = (
    'lib/ld-liteos.so.1',
    'lib/ld-linux-liteos.so.1',
    'lib/libliteosc.so.1',
    'lib/libliteosgfx.so.1',
    'etc/fonts/liteos.ttf',
)


def clean(root):
    root = Path(root)
    if root.is_symlink() or not root.is_dir() or root.name != 'esp':
        raise ValueError('expected a real ESP staging directory')
    targets = []
    for relative in RETIRED:
        target = root / relative
        for parent in target.parents:
            if parent == root:
                break
            if parent.is_symlink():
                raise ValueError(f'refusing symlinked staging directory: {parent}')
        if target.is_symlink() or target.is_file():
            targets.append(target)
        elif target.exists():
            raise ValueError(f'refusing non-file alias: {target}')
    # Validate every target before removing any of them; never recurse.
    for target in targets:
        target.unlink()
    return targets


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('esp', type=Path)
    args = parser.parse_args()
    for path in clean(args.esp):
        print(f'Removed retired generated alias: {path}')
