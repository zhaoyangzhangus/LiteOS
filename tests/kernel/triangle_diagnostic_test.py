"""Production triangle diagnostics and shared GPU log-record matching contract."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'tools/nvidia/phase11_triangle_test.c').read_text()
assert 'LITEOS_' not in source
functions=source[source.index('static void p11_emit('):source.index('static int p11_contains_nvidia(')]
functions=functions.replace('write(', 'capture_write(')
code=r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static char output[256];
static unsigned calls,mode;
static size_t used;
static ssize_t capture_write(int fd,const void *text,size_t length) {
    assert(fd==STDOUT_FILENO && text && length);++calls;
    if (mode==1 && calls==1) {errno=EINTR;return -1;}
    if (mode==3) return 0;
    if (mode==4) {errno=EIO;return -1;}
    if (mode==5) {errno=EAGAIN;return -1;}
    if (mode==6) return (ssize_t)length+1;
    if (mode==2 && length>3) length=3;
    assert(used+length<sizeof(output));memcpy(output+used,text,length);
    used+=length;output[used]=0;return length;
}
static void reset(unsigned m) {memset(output,0,sizeof(output));used=calls=0;mode=m;errno=0;}
''' + functions + r'''
int main(void) {
    const uint32_t values[]={0,1,9,10,99,100,999999999,1000000000,UINT32_MAX};
    for (unsigned i=0;i<sizeof(values)/sizeof(*values);++i) {
        char expected[64],number[32];
        snprintf(number,sizeof(number),"%u\n",values[i]);
        snprintf(expected,sizeof(expected),"OS_P11_VALUE=%u\n",values[i]);
        for (unsigned m=0;m<3;++m) {
            reset(m);p11_emit_u32("OS_P11_VALUE=",values[i]);
            assert(!strcmp(output,expected));
            unsigned want=m==2?(strlen("OS_P11_VALUE=")+2)/3+(strlen(number)+2)/3:2+(m==1);
            assert(calls==want);
        }
        reset(0);p11_emit_u32(NULL,values[i]);assert(!strcmp(output,number)&&calls==1);
    }
    uint32_t value=17;
    for (unsigned i=0;i<10000;++i) {
        value=value*1664525U+1013904223U;
        char expected[32];snprintf(expected,sizeof(expected),"%u\n",value);
        reset(0);p11_emit_u32("",value);assert(!strcmp(output,expected)&&calls==1);
    }
    reset(0);p11_emit(NULL);p11_emit("");assert(!calls&&!used);
    reset(0);p11_emit_value("OS_P11_GL_VENDOR=","NVIDIA");
    assert(!strcmp(output,"OS_P11_GL_VENDOR=NVIDIA\n")&&calls==3);
    reset(0);p11_emit_value(NULL,NULL);assert(!strcmp(output,"\n")&&calls==1);
    for (unsigned m=3;m<=6;++m) {
        reset(m);p11_emit("OS_P11_FAIL\n");assert(calls==1&&!used);
    }
}
'''
with tempfile.TemporaryDirectory(prefix='triangle-diagnostics-') as directory:
    binary=Path(directory)/'test'
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',str(binary)],input=code,text=True,check=True)
    subprocess.run([str(binary)],check=True)
for bridge in (0,1):
    for headers in ([], ['-I',str(ROOT/'user/libc/include')]):
        subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsyntax-only',
                        f'-DP11_WINDOW_BRIDGE={bridge}','-I',str(ROOT/'include'),
                        *headers,str(ROOT/'tools/nvidia/phase11_triangle_test.c')],check=True)

helper=ROOT/'tools/nvidia/log-records.ps1'
text=helper.read_text()
assert '[regex]::Escape($Marker)' in text and '[string]::IsNullOrEmpty($Marker)' in text
prefix=re.search(r"return \$Text -match \('([^']+)'",text).group(1)
suffix=re.search(r"else \{ '([^']+)' \}",text).group(1)
cases=[]
for marker in ('OS_CUBE_PASS','OS_P11_PASS','OS_P12_PASS','OS_P12_RENDER_PASS frames=2','OS_P11_GL_VENDOR='):
    value='NVIDIA' if marker.endswith('=') else ''
    for before in ('','[12:03:04] ','\t'):
        cases.append((before+marker+value+'\r\n',marker,True))
    for before in ('LITE','NOT_','prefix'):
        cases.append((before+marker+value+'\n',marker,False))
    if not marker.endswith('='):
        cases.append((marker+'0\n',marker,False))
        cases.append((marker+'_EXTRA\n',marker,False))
cases.extend((('OS_P11_GL_VENDOR=anything','OS_P11_GL_VENDOR=anything',True),
              ('OS_P11_PASS','',False)))
for log,marker,expected in cases:
    actual=bool(marker) and bool(re.search(prefix+re.escape(marker)+('' if marker.endswith('=') else suffix),log,re.I))
    assert actual==expected,(log,marker)
for name in ('cube_demo_gate.ps1','phase11_gate.ps1','phase12_gate.ps1','phase12_build.ps1'):
    consumer=(ROOT/'tools/nvidia'/name).read_text()
    assert ". (Join-Path $PSScriptRoot 'log-records.ps1')" in consumer,name
    assert 'function Test-LogRecord' not in consumer,name
    assert 'LITEOS_P11' not in consumer and 'LITEOS_P12' not in consumer,name
    if name.endswith('gate.ps1'):
        assert 'Test-LogRecord $realText $requiredMarker' in consumer,name
pwsh=shutil.which('pwsh')
if pwsh:
    def quote(value):
        return "'"+str(value).replace("'", "''")+"'"
    for log,marker,expected in cases:
        command=(f'. {quote(helper)}; '
                 f'if ((Test-LogRecord {quote(log)} {quote(marker)}) '
                 f'-ne ${str(expected).lower()}) {{ exit 1 }}')
        subprocess.run([pwsh,'-NoProfile','-Command',command],check=True)
    print('PowerShell actual shared matcher cases passed')
else:
    print('PowerShell unavailable: extracted regex contract and consumer wiring checked; PowerShell execution not verified')
print('Triangle diagnostics: exact uint32 formatting, short writes/EINTR/error termination, four full-source header/bridge modes passed')
