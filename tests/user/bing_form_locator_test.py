"""Exercise the real QMP probe locator without operating a running guest."""
import ast
from pathlib import Path

root = Path(__file__).resolve().parents[2]
source = ast.parse((root/'tools/chromium/qmp-bing-search-probe.py').read_text())
function = next(node for node in source.body
                if isinstance(node, ast.FunctionDef) and node.name == 'find_search_band')
namespace = {}
exec(compile(ast.Module(body=[function], type_ignores=[]), '<real-locator>', 'exec'), namespace)
locate = namespace['find_search_band']
width = 1920

def frame(bands, background=(20, 80, 140)):
    pixels = bytearray(bytes(background) * (width*1080))
    row = bytes((255, 255, 255)) * 500
    for top, bottom in bands:
        for y in range(top, bottom):
            offset = (y*width+200)*3
            pixels[offset:offset+len(row)] = row
    return pixels

for top in (292, 335, 342):
    assert locate(width, frame([(top, top+50)])) == (top, top+50)
with_placeholder = frame([(291, 343)])
for y in range(313, 321):
    offset = (y*width+200)*3
    with_placeholder[offset:offset+80*3] = bytes((80, 80, 80))*80
assert locate(width, with_placeholder) == (291, 343)
for bands, background in (([], (20, 80, 140)),
                          ([(292, 342), (380, 430)], (20, 80, 140)),
                          ([(292, 300)], (20, 80, 140)),
                          ([], (255, 255, 255))):
    try:
        locate(width, frame(bands, background))
    except RuntimeError:
        pass
    else:
        raise AssertionError('Absent/ambiguous/non-home form must fail without clicking')
print('Bing form locator: banner-height changes, absence, ambiguity and all-white page passed')
