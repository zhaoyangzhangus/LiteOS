#!/usr/bin/env python3
"""Download signed Debian dependency packages into the project, never install them."""
import argparse
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
PACKAGES = ('libx11-6', 'libxext6', 'libxcb1', 'libxau6', 'libxdmcp6', 'libbsd0', 'libmd0',
            'libgcc-s1')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', default='build', type=Path)
    args = parser.parse_args()
    if args.build.is_absolute() or '..' in args.build.parts:
        parser.error('BUILD must stay inside the project')
    base = ROOT/args.build/'nvidia/vulkan/deps'
    lists, archives = base/'lists', base/'archives'
    for path in (lists/'partial', archives/'partial', base/'packages', base/'root'):
        path.mkdir(parents=True, exist_ok=True)
    options = ['-o', 'Dir::Etc::sourcelist='+str(ROOT/'tools/nvidia/vulkan-bookworm.sources'),
               '-o', 'Dir::Etc::sourceparts=-', '-o', 'Dir::State::lists='+str(lists),
               '-o', 'Dir::Cache::archives='+str(archives),
               '-o', 'Dir::Cache::pkgcache='+str(base/'pkgcache.bin'),
               '-o', 'Dir::Cache::srcpkgcache='+str(base/'srcpkgcache.bin'),
               '-o', 'APT::Get::List-Cleanup=0', '-o', 'APT::Sandbox::User='+str(os.getuid())]
    subprocess.run(['apt-get', *options, 'update'], check=True)
    subprocess.run(['apt-get', *options, 'download', *[name+'/bookworm' for name in PACKAGES]],
                   cwd=base/'packages', check=True)
    for package in PACKAGES:
        matches = sorted((base/'packages').glob(package+'_*.deb'))
        if len(matches) != 1:
            raise RuntimeError('ambiguous or missing dependency package: '+package)
        subprocess.run(['dpkg-deb', '--extract', str(matches[0]), str(base/'root')], check=True)
    print('VULKAN_DEPENDENCIES_FETCHED='+str(base/'root'), flush=True)


if __name__ == '__main__':
    main()
