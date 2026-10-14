"""Exercise renamed Chrome validation tools and their input/negative cases."""
from pathlib import Path
import ast
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools'
names = ('runtime-trace', 'memory-profiler', 'webui-mode')
for name in names:
    assert not (TOOLS / ('verify-' + 'liteos-' + name + '.sh')).exists()


def run(name, arguments, expected):
    result = subprocess.run(['bash',str(TOOLS / ('verify-chrome-' + name + '.sh')),*map(str,arguments)],
                            capture_output=True,text=True)
    assert result.returncode == expected, (name,arguments,result.stdout,result.stderr)
    if expected == 0:
        assert 'Chrome ' in result.stdout and 'LiteOS' not in result.stdout
    if expected == 2:
        assert 'usage:' in result.stderr


with tempfile.TemporaryDirectory(prefix='chrome-validation-tools-') as temporary:
    root = Path(temporary)
    source = root / 'source file.cc'; libc = root / 'syscall.c'
    source.write_text('fixture'); libc.write_text('fixture')
    for name in names:
        arity = 2 if name == 'runtime-trace' else 1
        run(name,[],2)
        run(name,[root/'missing']*arity,2)
        run(name,[source]*(arity+1),2)
        run(name,[source]*arity,1)
    guard = '#if defined(__NATIVE_OS__) && defined(OS_STARTUP_TRACE)'
    dependency = guard + '\ntrace();\n#else\n(void)phase;\n(void)name;\n#endif\n'
    libc_text = 'OS_RUNTIME_TRACE\ng_libc_service_trace_drop_chunks\n'
    source.write_text(dependency); libc.write_text(libc_text)
    run('runtime-trace',[source,libc],0)
    run('runtime-trace',[source],2)
    for token in (guard,'(void)phase;','(void)name;'):
        source.write_text(dependency.replace(token,'')); run('runtime-trace',[source,libc],1)
    source.write_text(dependency)
    for token in ('OS_RUNTIME_TRACE','g_libc_service_trace_drop_chunks'):
        libc.write_text(libc_text.replace(token,'')); run('runtime-trace',[source,libc],1)
    resident = '''std::optional<size_t> ProcessMemoryDump::CountResidentBytes(void *p) {
#if defined(__NATIVE_OS__)
return std::nullopt;
#else
return probe(p);
#endif
}
std::optional<size_t> ProcessMemoryDump::CountResidentBytesInSharedMemory(void *p) {
return std::nullopt;
}
'''
    source.write_text(resident); run('memory-profiler',[source],0)
    source.write_text(resident.replace('#if defined(__NATIVE_OS__)','#if defined(OTHER_PLATFORM)'))
    run('memory-profiler',[source],1)
    source.write_text(resident.replace('return std::nullopt;','return probe(p);'))
    run('memory-profiler',[source],1)
    source.write_text(resident.replace('return std::nullopt;', 'return probe(p);', 1)
                     .replace('return probe(p);\n#endif', 'return std::nullopt;\n#endif'))
    run('memory-profiler',[source],1)
    source.write_text(resident.replace('#if defined(__NATIVE_OS__)', '// #if defined(__NATIVE_OS__)'))
    run('memory-profiler',[source],1)
    helpers = ('ReloadButton','HomeButton','BatterySaverButton','AppMenuButton',
               'BackForwardButton','PinnedToolbarActions','ExtensionsContainer',
               'SplitTabsButton','AvatarButton','MediaButton',
               'PerformanceInterventionButton','LocationBar','Toolbar')
    declaration = 'bool IsNativeGuestOzonePlatform() {\nreturn true;\n}\n'
    bodies = [f'bool IsWebUI{name}Enabled() {{\nif (IsNativeGuestOzonePlatform()) return false;\nreturn true;\n}}\n' for name in helpers]
    source.write_text(declaration + ''.join(bodies)); run('webui-mode',[source],0)
    source.write_text(''.join(bodies)); run('webui-mode',[source],1)
    for i in range(len(bodies)):
        source.write_text(declaration + ''.join(bodies[:i]+bodies[i+1:]))
        run('webui-mode',[source],1)
        broken = bodies.copy(); broken[i]=broken[i].replace('IsNativeGuestOzonePlatform()','OtherPlatform()')
        source.write_text(declaration + ''.join(broken)); run('webui-mode',[source],1)
tree = ast.parse((TOOLS/'chromium/serial-startup-timing.py').read_text())
assignment = next(node for node in tree.body if isinstance(node, ast.Assign)
                  and any(isinstance(target, ast.Name) and target.id == 'pattern'
                          for target in node.targets))
pattern = re.compile(ast.literal_eval(assignment.value.args[0]))
for namespace in ('OS', 'LITEOS'):
    for category in ('CHROME_STARTUP', 'PROFILE', 'CONTENT_INIT',
                     'BROWSER_PROCESS', 'RENDER_PROCESS'):
        marker = f'{namespace}_{category}_INIT_BEGIN'.encode()
        for record in (marker, marker+b'\r', b'[12:34] '+marker+b' status=0'):
            expected = [marker] if namespace == 'OS' else []
            assert [match.group() for match in pattern.finditer(record)] == expected
        for record in (b'NOT_'+marker, b'prefix'+marker, marker+b'lowercase'):
            assert not pattern.search(record), record
assert not pattern.search(b'OS_UNKNOWN_INIT')
print('CHROME_VALIDATION_TOOLS_TEST_OK: neutral startup markers, retired namespace rejection and token boundaries')
