"""Check Windows and normalized paths at every experimental list input boundary."""
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT/'tools/nvidia'
count = 0
for name in ('phase8', 'phase9', 'phase10', 'phase11', 'phase12',
             'phase-w3', 'cube_desktop'):
    source = (TOOLS/f'{name}_build.ps1').read_text()
    loops = re.split(r'foreach \(\$row in @\(Get-Content -LiteralPath \$\w+LinkList\)\) \{', source)[1:]
    assert loops, name
    for loop in loops:
        pattern_match = re.search(r"\$row -match '([^']+)'", loop)
        assert pattern_match, name
        pattern = pattern_match[1]
        upstream = re.search(r'((?:build-realtest-elf|build/nvidia/[^/]+/system)/kernel)/', pattern)[1]
        prefix = loop[:pattern_match.start()]
        normalize = '$row = ConvertTo-ObjectListPath $row' in prefix
        for separator in ('/', '\\'):
            row = 'C:/workspace with spaces/' + upstream + '/graphics-compositor.o'
            row = row.replace('/', separator)
            value = row.replace('\\', '/') if normalize else row
            match = re.search(pattern, value)
            assert match and match[1] == 'graphics-compositor.o', (name, row, 'path missed')
        assert normalize, name
        data_guard = 'if (-not (Test-ObjectListData $row))' in prefix
        for row in ('# C:/workspace/' + upstream + '/entry.o',
                    '  # C:/workspace/' + upstream + '/entry.o',
                    '\t# C:/workspace/' + upstream + '/entry.o'):
            skipped = data_guard or ("$row[0] -eq '#'" in prefix and row.startswith('#'))
            assert skipped or not re.search(pattern, row), (name, row, 'comment selected as object')
        assert data_guard, (name, 'missing shared data classification')
        assert '$row[0]' not in prefix, name
        assert 'continue' in prefix[prefix.index('if (-not (Test-ObjectListData $row))'):], name
        count += 1
    assert ". (Join-Path $PSScriptRoot 'object-paths.ps1')" in source, name

helper = (TOOLS/'object-paths.ps1').read_text()
assert "function ConvertTo-ObjectListPath([string]$Path)" in helper
assert "function Test-ObjectListData([string]$Path)" in helper
assert "[string]::IsNullOrWhiteSpace($Path)" in helper
assert "if (-not (Test-ObjectListData $Path))" in helper
assert "$Path.TrimStart().StartsWith('#')" in helper
assert "return $Path.Replace('\\', '/')" in helper
assert ".Replace('\\\\', '/')" not in helper
fixtures = ('', '   ', '\t', '# comment \\ remains intact', '  # indented \\ comment',
            'C:\\workspace with spaces\\kernel\\entry.o',
            'C:/workspace with spaces/kernel/entry.o',
            '\\\\server\\share\\kernel\\entry.o', 'relative\\kernel\\entry.o')
pwsh = shutil.which('pwsh')
if pwsh:
    for path in fixtures:
        expected = path if not path or path.lstrip().startswith('#') else path.replace('\\', '/')
        literal = "'" + path.replace("'", "''") + "'"
        program = helper + '\nWrite-Output (ConvertTo-ObjectListPath ' + literal + ')\n'
        program += 'Write-Output ("DATA=" + [int](Test-ObjectListData ' + literal + '))\n'
        result = subprocess.run([pwsh, '-NoProfile', '-Command', '-'], input=program,
                                text=True, capture_output=True, check=True, timeout=10)
        assert expected in result.stdout.splitlines(), (path, result.stdout)
        is_data = bool(path.strip()) and not path.lstrip().startswith('#')
        assert 'DATA=' + str(int(is_data)) in result.stdout.splitlines(), (path, result.stdout)
print(f'Object list paths: {count} input boundaries accept Windows/normalized paths and spaces; '
      'blank/indented comments excluded before matching, original non-data rows retained; '
      + ('PowerShell helper fixtures passed' if pwsh else 'PowerShell unavailable: source contract checks only'))
