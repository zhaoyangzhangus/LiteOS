"""Fail-closed packet/PTS parsing and execution of the real comparison tool."""
import importlib.util
from pathlib import Path
import struct
import tempfile

root=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('video_compare',root/'tools/chromium/compare-video-readback.py')
module=importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
key=b'\0\0\0\1\x67sps\0\0\1\x68pps\0\0\1\x65idr'
delta=b'\0\0\1\x41slice'
def record(pts,payload):return struct.pack('<qI',pts,len(payload))+payload
assert len(module.packets(record(1,key)+record(2,delta)))==2
for data in (b'',b'x',record(1,key)[:-1],record(2,key)+record(1,delta),
             record(1,key)+record(1,delta),record(1,b'not-annex-b')):
    try:module.packets(data)
    except ValueError:pass
    else:raise AssertionError('malformed capture accepted')
with tempfile.TemporaryDirectory(prefix='video-readback-compare-') as name:
    directory=Path(name)
    (directory/'test.packets').write_bytes(record(1,key)+record(2,delta))
    (directory/'test-capture.log').write_text(
        'DECODED_FRAME index=0 pts_us=1 size=8x8\n'
        'DECODED_FRAME index=1 pts_us=2 size=8x8\n')
    for index in range(2):
        (directory/('test-%02d.pgm'%index)).write_bytes(b'P5\n8 4\n255\n'+bytes([index])*32)
    fake=directory/'ffmpeg'
    fake.write_text('#!/usr/bin/env python3\nimport sys\n'
                    'assert sys.argv[sys.argv.index("-hwaccel")+1]=="none"\n'
                    'sys.stdout.buffer.write(bytes(96)+bytes([1])*96)\n')
    fake.chmod(0o755)
    report=module.run(directory,'test',fake)
    assert report['matched_frames']==2 and all(not row['max_delta'] for row in report['frames'])
    path=directory/'test-01.pgm'
    path.write_bytes(b'P5\n8 4\n255\n'+bytes([200])+bytes([1])*31)
    report=module.run(directory,'test',fake)
    assert report['frames'][1]['delta_over_8']==1 and report['frames'][1]['max_delta']==199
    (directory/'test-capture.log').write_text('DECODED_FRAME index=0 pts_us=9 size=8x8\n')
    try:module.run(directory,'test',fake)
    except ValueError:pass
    else:raise AssertionError('unaligned timestamps accepted')
print('Video readback comparison: real pipeline, forced CPU reference, exact PTS, '
      'clean/damaged pixels, truncation and missing-alignment rejection passed')
