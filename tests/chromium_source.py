"""Resolve the explicitly configured Chromium checkout for source regressions."""
import os
from pathlib import Path


def chromium_source():
    configured = os.environ.get('CHROMIUM_SRC')
    if not configured:
        raise RuntimeError('Set CHROMIUM_SRC to the actual Chromium source directory')
    try:
        root = Path(configured).expanduser().resolve()
        valid = root.is_dir() and (root/'BUILD.gn').is_file()
    except (OSError, ValueError) as error:
        raise RuntimeError(f'Invalid CHROMIUM_SRC: {configured}: {error}') from error
    if not valid:
        raise RuntimeError(f'CHROMIUM_SRC is not a Chromium source directory: {root}')
    return root
