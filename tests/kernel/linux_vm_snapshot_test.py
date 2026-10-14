"""Actual Linux mmap and simple VM operation blocks use the shared VM pin."""
import ast
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
tree = ast.parse((root / 'tests/kernel/syscall_vm_snapshot_test.py').read_text())
fixture = next(ast.literal_eval(n.value) for n in tree.body if isinstance(n, ast.Assign)
               and any(isinstance(t, ast.Name) and t.id == 'fixture' for t in n.targets))
fixture = fixture[:fixture.index('@HELPER@')]
internal = (root / 'kernel/process/internal.h').read_text()
helper = internal[internal.index('static inline vm_space_t *process_vm_reference('):]
linux = (root / 'kernel/syscall/linux.c').read_text()
start = linux.index('    vm_space_t *space = process_vm_reference(process);', linux.index('static int64_t linux_mmap('))
mapping = linux[start:linux.index('#if OS_DEBUG_SERIAL', start)]
start = linux.index('case LINUX_SYS_MPROTECT:')
start = linux.index('{', start) + 1
protect = linux[start:linux.index('        if (status != K_OK', start)]
start = linux.index('case LINUX_SYS_MUNMAP: {')
unmap = linux[start:linux.index('    case LINUX_SYS_BRK:', start)]
unmap = unmap[unmap.index('{')+1:unmap.rindex('}')]
start = linux.index('case LINUX_SYS_MADVISE: {')
start = linux.index('        vm_space_t *space = process_vm_reference(process);', start)
advise = linux[start:linux.index('        if (status != K_OK', start)]
extra = r'''
#define PAGE_SIZE 4096ULL
#define K_OK 0
#define LINUX_PROT_READ 1U
#define LINUX_PROT_WRITE 2U
#define LINUX_PROT_EXEC 4U
#define VM_PROT_EXEC 4U
#define K_EBUSY -16
#define LINUX_EEXIST -17
#define LINUX_MAP_FIXED 0x10U
#define LINUX_MAP_FIXED_NOREPLACE 0x100000U
typedef struct {uint64_t rdi,rsi,rdx;} frame_t;
static unsigned replacements;
static size_t protect_size = 4096;
static int vm_protect_allow_rwx(vm_space_t *s,vaddr_t a,size_t n,uint32_t p) {
 assert(a==0x10000 && n==protect_size && p==(VM_PROT_USER|VM_PROT_READ));
 return operation(s);
}
static int vm_map_object_replace(vm_space_t *s,vm_object_t *o,vaddr_t *a,uint64_t off,size_t n,uint32_t prot,uint32_t flags) {
 ++replacements;return vm_map_object(s,o,a,off,n,prot,flags);
}
'''
functions = helper + '\n'
functions += 'static int mmap_probe(process_t *process,uint64_t flags,vm_object_t *object) {int status;vaddr_t address=0;uint64_t map_size=4096;uint32_t vm_prot=VM_PROT_USER|VM_PROT_READ,vm_flags=VM_MAP_PRIVATE;\n' + mapping + '\nreturn status;}\n'
functions += 'static int protect_probe(process_t *process,frame_t *frame) {\n' + protect + '\nreturn status;}\n'
functions += 'static int unmap_probe(process_t *process,frame_t *frame) {\n' + unmap + '\n}\n'
functions += 'static int advise_probe(process_t *process,frame_t *frame) {\n' + advise + '\nreturn status;}\n'
checks = r'''
static void setup(void) {held=detach=false;destroyed=operations=object_puts=replacements=0;backend_status=0;process.vm=expected=allocate();}
static void finish(void) {if(process.vm)vm_space_put(process.vm);process.vm=NULL;assert(destroyed==1 && !held);}
int main(void) {
 int (*calls[])(process_t *,frame_t *)={protect_probe,unmap_probe,advise_probe};frame_t frame={.rdi=0x10000,.rsi=4096,.rdx=1};
 for(unsigned i=0;i<3;++i)for(unsigned mode=0;mode<4;++mode) {
  setup();detach=mode&1;backend_status=(mode&2)?-12:0;
  assert(calls[i](&process,&frame)==backend_status && operations==1);finish();
 }
 for(unsigned i=0;i<3;++i) {
  setup();vm_space_put(process.vm);process.vm=NULL;
  assert(calls[i](&process,&frame)==K_EINVAL && operations==1);finish();
 }
 uint64_t lengths[]={1,4095,4096,4122,8192};
 for(unsigned i=0;i<5;++i) {
  setup();frame.rsi=lengths[i];protect_size=(lengths[i]+4095)&~4095ULL;
  assert(protect_probe(&process,&frame)==0 && operations==1);finish();
 }
 setup();frame.rsi=0;
 assert(protect_probe(&process,&frame)==0 && !operations);finish();
 setup();frame.rdi=0x10001;frame.rsi=0;
 assert(protect_probe(&process,&frame)==K_EINVAL && !operations);finish();
 setup();frame.rdi=0x10000;frame.rsi=UINT64_MAX;
 assert(protect_probe(&process,&frame)==K_EINVAL && !operations);finish();
 protect_size=4096;frame.rsi=4096;
 uint64_t flags[]={0,LINUX_MAP_FIXED,LINUX_MAP_FIXED_NOREPLACE};
 int statuses[]={0,-12,K_EBUSY};
 for(unsigned f=0;f<3;++f)for(unsigned e=0;e<3;++e)for(unsigned d=0;d<2;++d) {
  setup();detach=d;backend_status=statuses[e];vm_object_t object={.refs=1};
  int wanted=flags[f]==LINUX_MAP_FIXED_NOREPLACE && statuses[e]==K_EBUSY?LINUX_EEXIST:statuses[e];
  assert(mmap_probe(&process,flags[f],&object)==wanted);
  assert(object_puts==1 && !object.refs && operations==1 && replacements==(flags[f]==LINUX_MAP_FIXED));finish();
 }
 return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    def run(name, body):
        path = directory / (name + '.c')
        path.write_text(fixture + extra + body + checks)
        binary = directory / name
        subprocess.run(['cc', '-std=c11', '-fsanitize=address,undefined', '-fno-pie',
                        '-no-pie', '-I'+str(root/'include'), str(path), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], capture_output=True, text=True)
    result = run('linux_pin', functions)
    assert result.returncode == 0, result.stderr
    result = run('unrounded_mprotect', functions.replace('(size_t)rounded, prot)',
                                                        '(size_t)frame->rsi, prot)'))
    assert result.returncode != 0, 'non-page-aligned lengths must reach the VM rounded'
    result = run('borrowed', functions.replace('if (space != 0) vm_space_get(space);', ''))
    assert result.returncode != 0 and 'heap-use-after-free' in result.stderr, result.stderr
print('Linux VM snapshots: mmap/fixed/noreplace error translation and protect/unmap/advise success/failure/exit/NULL cases passed; missing reference reproduces UAF')
