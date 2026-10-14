"""Startup body sampling follows actual geometry, not a saved 945px width."""
import ast
from pathlib import Path

root = Path(__file__).resolve().parents[2]
tree = ast.parse((root/'tools/chromium/qmp-startup-latency.py').read_text())
function = next(node for node in tree.body if isinstance(node, ast.FunctionDef)
                and node.name == 'browser_extent')
namespace = {}
exec(compile(ast.Module(body=[function], type_ignores=[]), '<extent>', 'exec'), namespace)
extent = namespace['browser_extent']
width = 1920

def scene(left, right):
    pixels = bytearray(bytes((5, 20, 60))*width*100)
    for y in (84, 86, 88, 90, 92):
        for x in range(left, right):
            pixels[(y*width+x)*3:(y*width+x)*3+3] = bytes((245, 245, 245))
    return pixels

assert extent(width, scene(10, 955)) == (10, 955)
assert extent(width, scene(0, 1920)) == (0, 1920)
assert extent(width, scene(0, 0)) is None
assert extent(width, scene(100, 200)) is None
focused = scene(10, 955)
for x in range(135, 865):
    focused[(90*width+x)*3:(90*width+x)*3+3] = bytes((0, 70, 255))
assert extent(width, focused) == (10, 955), 'Focused address border is not minimization'
print('Startup extent: normal/full-width, empty scene and tiny patch passed')

body_function = next(node for node in tree.body if isinstance(node, ast.FunctionDef)
                     and node.name == 'body_pixel_count')
exec(compile(ast.Module(body=[body_function], type_ignores=[]), '<body>', 'exec'), namespace)
body_count = namespace['body_pixel_count']
body = bytearray(bytes((255, 255, 255))*width*700)
assert body_count(width, body, 200, 750) == 0
for color in ((66, 133, 244), (234, 67, 53), (251, 188, 5), (52, 168, 83)):
    for y in range(400, 420):
        body[(y*width+350)*3:(y*width+450)*3] = bytes(color)*100
    assert body_count(width, body, 200, 750) == 2000, 'Colored logo must count as loaded content'
for color in ((220, 220, 220), (255, 255, 255), (20, 20, 20)):
    for y in range(400, 420):
        body[(y*width+350)*3:(y*width+450)*3] = bytes(color)*100
    assert body_count(width, body, 200, 750) == (2000 if color[0] == 20 else 0)
assert body_count(width, body[:width*100*3], 200, 750) == 0
print('Startup body: colored logo, dark text, blank/gray and short image passed')
