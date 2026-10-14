"""Dry-run the real host test entrypoint; missing tools must not hide errors."""
from pathlib import Path
import subprocess

root=Path(__file__).resolve().parents[2]
base=['make','-f','tools/host-tests.mk','-n','host-regression-test']
result=subprocess.run(base,cwd=root,text=True,capture_output=True,timeout=10)
assert result.returncode==0,result.stderr
assert 'cc -O2 -Wall -Wextra -Werror -fsanitize=address,undefined' in result.stdout
assert '-o build/message-box-test.exe' in result.stdout
assert '-o /message-box-test.exe' not in result.stdout
for assignment,message in (('HOSTCC=','HOSTCC must not be empty'),('BUILD=','BUILD must not be empty')):
    result=subprocess.run([*base,assignment],cwd=root,text=True,capture_output=True,timeout=10)
    assert result.returncode!=0 and message in result.stderr,(assignment,result)
result=subprocess.run([*base,'HOSTCC=custom-compiler'],cwd=root,text=True,capture_output=True,timeout=10)
assert result.returncode==0 and 'custom-compiler -O2' in result.stdout,result
print('Host test entrypoint: standalone defaults, canonical output, overrides and empty-variable rejection passed')
