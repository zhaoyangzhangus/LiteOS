"""Guest-only QA of fresh centered Files: hover alpha and retained drag pixels."""
from pathlib import Path
import tempfile
import time
root=Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
qmp = QMPClient()
def command(name,arguments=None):
    return qmp.command(name, arguments)
def relative(dx,dy):
    while dx or dy:
        x=max(-100,min(100,dx));y=max(-100,min(100,dy))
        command('input-send-event',{'events':[
            {'type':'rel','data':{'axis':'x','value':x}},
            {'type':'rel','data':{'axis':'y','value':y}}]})
        dx-=x;dy-=y;time.sleep(.008)
def move(x,y):
    relative(-2500,-1500);relative(x,y);time.sleep(.15)
def button(down):
    command('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':down}}]})
    time.sleep(.06)
def screenshot(label):
    command('screendump',{'filename':str(root/f'build/fileman-text-{label}.png'),'format':'png'})

assert command('query-status')['running']
with tempfile.TemporaryDirectory(prefix='fileman-text-qa-') as directory:
    path=str(Path(directory)/'frame.ppm')
    def pixels():
        command('screendump',{'filename':path})
        with open(path,'rb') as frame:
            assert frame.readline().strip()==b'P6'
            assert frame.readline().strip()==b'1920 1080'
            assert frame.readline().strip()==b'255'
            return frame.read()
    def roi(frame,x0,y0,x1,y1):
        return b''.join(frame[(y*1920+x0)*3:(y*1920+x1)*3]for y in range(y0,y1))
    move(100,500);baseline=pixels()
    sidebar=roi(baseline,260,500,261,501)
    assert min(sidebar)>220 and max(sidebar)-min(sidebar)<20, \
        'Requires fresh centered Files; desktop is not a window-under-test'
    # The compositor caption remains at y=135..175. The application must
    # not repeat its title in the sidebar header below it.
    sidebar_title=roi(baseline,252,180,350,212)
    assert all(v>=180 for v in sidebar_title),'Duplicate dark sidebar title remains'
    assert any(v<128 for v in roi(baseline,252,145,305,168)), 'Missing compositor title'
    # Current root-grid labels sit below the second icon row. The old
    # y=420 probe sampled the white document icon, not any filename.
    label_bounds=(790,450,855,478)
    label=roi(baseline,*label_bounds)
    # A completely white ROI would falsely pass without a visible file label.
    assert any(v<128 for v in label),'Requires fresh Files at (240,135), root directory'
    for _ in range(6):
        move(525,245);move(675,245)
        assert roi(pixels(),*label_bounds)==label,'Unrelated label changed on hover'
    move(100,500);baseline=pixels();screenshot('hover-stable')
    reference=roi(baseline,250,180,1665,930)
    move(900,155);button(True)
    x=y=0
    try:
        for dx,dy in [(40,0),(0,20),(-40,0),(0,-20)]*3:
            relative(dx,dy);x+=dx;y+=dy;time.sleep(.2)
            frame=pixels()
            assert roi(frame,250+x,180+y,1665+x,930+y)==reference, 'Client pixels changed during drag'
    finally:
        button(False)
    move(100,500);screenshot('drag-stable')
print('QEMU Files: repeated hover leaves unrelated text exact; 12 drag frames preserve all client pixels PASS')
