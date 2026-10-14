"""Strict reader for the canonical QEMU 1920x1080 RGB screendump.

This accepts QEMU's three-line P6 header, not arbitrary Netpbm files.
Read sizes are bounded even when the file or its header is corrupt.
"""
from pathlib import Path

WIDTH = 1920
HEIGHT = 1080
RGB_BYTES = WIDTH * HEIGHT * 3


def read_qemu_frame(path):
    with Path(path).open('rb') as image:
        if image.readline(256).strip() != b'P6':
            raise ValueError('QEMU frame must use binary RGB P6 encoding')
        if image.readline(256).split() != [b'1920', b'1080']:
            raise ValueError('QEMU frame must be 1920x1080')
        if image.readline(256).strip() != b'255':
            raise ValueError('QEMU frame must use 8-bit RGB channels')
        pixels = image.read(RGB_BYTES + 1)
        if len(pixels) != RGB_BYTES:
            raise ValueError('QEMU frame has truncated or trailing RGB data')
        return pixels
