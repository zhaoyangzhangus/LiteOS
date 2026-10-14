"""Click the real menu's New tab command, then close the created tab by its X.

Requires the inspected 945x1060 browser at (10,10), with one initial tab.
Uses only the canonical guest. Retained body screenshots need visual review.
"""
import argparse
import ast
import json
from pathlib import Path
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('--cycles', type=int, default=3)
args = parser.parse_args()
if not 1 <= args.cycles <= 10:
    parser.error('cycles must be 1..10')
root = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
tree = ast.parse((root/'tools/chromium/qmp-app-menu-test.py').read_text())
gate = next(node for node in tree.body
            if isinstance(node, ast.FunctionDef) and node.name == 'menu_is_painted')
namespace = {}
exec(compile(ast.Module(body=[gate],type_ignores=[]), '<menu-paint-gate>', 'exec'),namespace)
menu_is_painted = namespace['menu_is_painted']
qmp = QMPClient()

def command(name,arguments=None):
    return qmp.command(name, arguments)

def relative(dx,dy):
    while dx or dy:
        x,y = max(-100,min(100,dx)),max(-100,min(100,dy))
        command('input-send-event',{'events':[
            {'type':'rel','data':{'axis':'x','value':x}},
            {'type':'rel','data':{'axis':'y','value':y}}]})
        dx -= x
        dy -= y
        time.sleep(.05)

def click():
    for down in (True,False):
        command('input-send-event',{'events':[
            {'type':'btn','data':{'button':'left','down':down}}]})
        if down:
            time.sleep(.04)

def capture(label):
    command('screendump',{'filename':str(root/f'build/chrome-menu-newtab-{label}.png'),
                         'format':'png'})

def header(width,pixels):
    return b''.join(pixels[(y*width+320)*3:(y*width+450)*3] for y in range(16,40))

def difference(a,b):
    return sum(abs(x-y)>20 for x,y in zip(a,b))/len(a)


assert command('query-status')['running']
command('send-key',{'keys':[{'type':'qcode','data':'esc'}],'hold-time':40})
time.sleep(.2)
relative(-2000,-1200)
relative(928,72)
results = []
with tempfile.TemporaryDirectory(prefix='system-menu-command-') as directory:
    filename = str(Path(directory)/'frame.ppm')
    def frame():
        command('screendump',{'filename':filename})
        with open(filename,'rb') as image:
            assert image.readline().strip()==b'P6'
            width,height = map(int,image.readline().split())
            assert (width,height)==(1920,1080)
            assert image.readline().strip()==b'255'
            pixels = image.read()
            assert len(pixels)==width*height*3
        return width,pixels

    def wait(predicate,label,seconds=10):
        start = time.monotonic()
        while time.monotonic()-start < seconds:
            width,pixels = frame()
            if predicate(width,pixels):
                return (time.monotonic()-start)*1000
            time.sleep(.025)
        capture(label+'-failure')
        raise RuntimeError(label+' did not pass within timeout')

    width,pixels = frame()
    assert not menu_is_painted(width,pixels), 'Menu still open at baseline'
    baseline = header(width,pixels)
    capture('baseline')
    for cycle in range(1,args.cycles+1):
        click()
        open_ms = wait(menu_is_painted,'open')
        relative(-278,44)  # Menu's actual New tab row at (650,116).
        capture('command-hover')
        start = time.monotonic()
        click()
        wait(lambda w,p:not menu_is_painted(w,p) and
             difference(baseline,header(w,p))>.05,'new-tab')
        create_ms = (time.monotonic()-start)*1000
        time.sleep(3)
        capture(f'{cycle}-body')
        relative(-141,-86)  # New tab's actual X at (509,30).
        start = time.monotonic()
        click()
        wait(lambda w,p:difference(baseline,header(w,p))<.01,'close-tab')
        close_ms = (time.monotonic()-start)*1000
        capture(f'{cycle}-restored')
        result = {'cycle':cycle,'menu_painted_ms':open_ms,
                  'command_to_tab_header_ms':create_ms,'close_to_restore_ms':close_ms}
        results.append(result)
        print(json.dumps(result),flush=True)
        relative(419,42)
report = {'measurements':results,
          'scope':'Actual menu New tab click and actual tab X; visually inspect body screenshots',
          'includes':'QMP delivery, held click, screenshot and polling overhead'}
(root/'build/chrome-menu-newtab-test.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report))
