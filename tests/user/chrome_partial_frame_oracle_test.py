"""The QEMU pixel oracle must reject ghost tiles, split frames and stale pixels."""
import importlib.util
import json
import tempfile
from pathlib import Path

root=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('partial_frame',root/'tools/chromium/qmp-partial-frame-test.py')
probe=importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)
pixels=bytearray(b'\xff'*(1920*1080*3))

def rectangle(x,y,width,height,rgb):
    for row in range(y,y+height):
        start=(row*1920+x)*3
        pixels[start:start+width*3]=bytes(rgb)*width

def rejected():
    try:probe.inspect_frame(pixels)
    except ValueError:return
    raise AssertionError('Broken frame was accepted')

rejected()
rectangle(10,97,16,16,(255,0,255))
left,top=10+34,97+98
rectangle(left+96,top+80,64,32,(240,40,20))
assert probe.inspect_frame(pixels)==(96,80)
rectangle(left+384,top+80,64,32,(240,40,20));rejected()
rectangle(left+384,top+80,64,32,(255,255,255))
rectangle(left+10,top+10,1,1,(20,20,20));rejected()
rectangle(left+10,top+10,1,1,(255,255,255))
rectangle(left+96,top+96,64,16,(255,255,255))
rectangle(left+320,top+96,64,16,(240,40,20));rejected()
diagnostic=probe.frame_diagnostics(pixels)
assert diagnostic['red_pixels']==2048 and len(diagnostic['red_rows'])==32
assert diagnostic['red_rows'][0]=={'y':80,'left':96,'right':159,'count':64}
assert diagnostic['red_rows'][-1]=={'y':111,'left':320,'right':383,'count':64}
with tempfile.TemporaryDirectory(prefix='frame-evidence-') as directory:
    base=Path(directory);frame=base/'original.ppm'
    frame.write_bytes(b'P6\n1920 1080\n255\n'+pixels)
    probe.save_failure(frame,base,ValueError('split fixture'))
    assert (base/'chrome-partial-frame-failure.ppm').read_bytes()==frame.read_bytes()
    saved=json.loads((base/'chrome-partial-frame-failure.json').read_text())
    assert saved['red_rows']==diagnostic['red_rows'] and saved['error']=='split fixture'
rectangle(left+320,top+96,64,16,(255,255,255))
rectangle(left+96,top+96,64,16,(240,40,20))
assert probe.inspect_frame(pixels)==(96,80)
print('Partial-frame pixel oracle: real tile, absent marker, ghost rectangle, split frame and stale pixel negative controls passed')
