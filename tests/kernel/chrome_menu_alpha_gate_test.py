"""Menu gates must reject both a text page and the historical black border."""
import ast
from pathlib import Path

root = Path(__file__).resolve().parents[2]
tree = ast.parse((root/'tools/chromium/qmp-app-menu-test.py').read_text())
functions = [node for node in tree.body if isinstance(node, ast.FunctionDef)
             and node.name in ('menu_is_painted', 'menu_has_black_border')]
namespace = {}
exec(compile(ast.Module(body=functions, type_ignores=[]), 'menu-gates', 'exec'), namespace)
width, height = 1920, 1080
pixels = bytearray(bytes([239,239,239]) * width * height)
def rectangle(x0,y0,x1,y1,color):
    row = bytes(color) * (x1-x0)
    for y in range(y0,y1):
        pixels[(y*width+x0)*3:(y*width+x1)*3] = row

# The normal example.com page is light gray, with text in the menu ROI.
rectangle(600,400,670,500,(20,20,20))
assert not namespace['menu_is_painted'](width,pixels)
# Complete white panel and distributed menu text; shadow reveals gray below.
rectangle(557,90,945,806,(255,255,255))
for y in range(205,645,10):
    rectangle(600,y,630,y+2,(20,20,20))
assert namespace['menu_is_painted'](width,pixels)
assert not namespace['menu_has_black_border'](width,pixels)
# Old compositor discarded alpha in the same transparent margin.
rectangle(533,78,556,842,(0,0,0))
assert namespace['menu_is_painted'](width,pixels)
assert namespace['menu_has_black_border'](width,pixels)
print('Menu gates: gray text baseline, painted panel, transparent shadow and old opaque black border passed')
