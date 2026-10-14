"""Check actual fault-context and TLB report emitters without privileged I/O."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
exception = (ROOT / 'kernel/arch/x86_64/irq/exception.c').read_text()
tlb = (ROOT / 'kernel/arch/x86_64/mm/tlb.c').read_text()
for name in ('irq/exception.c', 'mm/page_table.c', 'mm/tlb.c'):
    assert 'liteos' not in (ROOT / 'kernel/arch/x86_64' / name).read_text().lower()
for name in ('core', 'scheduler'):
    assert 'LITEOS_TLB_' not in (ROOT / f'kernel/init/{name}.c').read_text()
def function(source, marker):
    start = source.index(marker)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]
functions = '\n'.join(function(tlb, marker) for marker in (
    'static void tlb_failure_text(', 'static void tlb_failure_hex(',
    'static void tlb_failure_report('))
functions += '\n' + '\n'.join(function(exception, marker) for marker in (
    'static void record_user_opcode(', 'static bool exception_process_is_chrome(',
    'static void record_user_context('))
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define __user
#define K_OK 0
typedef struct { uint64_t value; } paddr_t;
typedef uint64_t vaddr_t;
typedef struct { uint64_t pid; char name[16]; } process_t;
typedef struct { process_t *process; } thread_t;
typedef struct { uint64_t rsp, rbp, rax, rbx, rcx, rdx, rsi, rdi,
    r8, r9, r10, r11, r12, r13, r14, r15, cs, rip, vector; } arch_trap_frame_t;
static char output[4096];
static size_t used;
static bool have_cpu, readable = true, copy_fail;
static uint64_t stack_words[8];
static char message[64];
static uint8_t opcode[8] = {0x0f, 0x0b, 1, 2, 3, 4, 5, 6};
static void put(char ch) { assert(used + 1 < sizeof(output)); output[used++] = ch; output[used] = 0; }
static void exception_write(const char *text) { while (*text) put(*text++); }
static void exception_write_hex(uint64_t value) {
    char text[32]; snprintf(text, sizeof(text), "0x%016llx", (unsigned long long)value); exception_write(text);
}
static void kernel_serial_write(const char *text) { exception_write(text); }
static void kernel_serial_write_u32(uint32_t value) {
    char text[16]; snprintf(text, sizeof(text), "%u", value); exception_write(text);
}
static void tlb_failure_putc(char ch) { put(ch); }
static bool sched_debug_cpu(uint32_t cpu, uint32_t *state, uint64_t *tid, uint32_t *runnable) {
    assert(cpu == 3); *state = 2; *tid = UINT64_MAX; *runnable = 7; return have_cpu;
}
static void x86_page_table_debug_state(uint32_t *lock, uint32_t *owner, uint32_t *waiter, uint64_t *waits) {
    *lock = 1; *owner = 4; *waiter = 5; *waits = UINT64_MAX;
}
static bool inside(uintptr_t addr, size_t n, const void *base, size_t size) {
    uintptr_t start = (uintptr_t)base;
    return addr >= start && addr - start <= size && n <= size - (addr - start);
}
static bool x86_user_range_valid(const void *p, size_t n) {
    return readable && (inside((uintptr_t)p,n,stack_words,sizeof(stack_words)) ||
        inside((uintptr_t)p,n,message,sizeof(message)) || inside((uintptr_t)p,n,opcode,sizeof(opcode)));
}
static int copy_from_user(void *out, const void *in, size_t n) {
    if (copy_fail || !x86_user_range_valid(in,n)) return -1;
    memcpy(out,in,n); return K_OK;
}
static void clear(void) { used = 0; output[0] = 0; }
''' + functions + r'''
int main(void) {
    for (unsigned present = 0; present < 2; ++present) {
        clear(); have_cpu = present;
        tlb_failure_report("TIMEOUT", 1, 3, 9, UINT64_MAX, 0,
                           (paddr_t){UINT64_MAX}, 0x8000000000000000ULL);
        assert(strstr(output,"OS_TLB_TIMEOUT owner=0x0000000000000001 target=0x0000000000000003"));
        assert(strstr(output," gen=0xffffffffffffffff ack=0x0000000000000000"));
        assert(strstr(output," root=0xffffffffffffffff va=0x8000000000000000"));
        assert(!!strstr(output," target_tid=0xffffffffffffffff") == !!present);
        assert(strstr(output," pt_lock=0x0000000000000001 pt_owner=0x0000000000000004 pt_waiter=0x0000000000000005 pt_waits=0xffffffffffffffff\r\n"));
    }
    process_t process = {UINT64_MAX, "chrome"}; thread_t thread = {&process};
    arch_trap_frame_t frame = {0};
    frame.cs = 3; frame.rip = (uintptr_t)opcode; frame.rsp = (uintptr_t)stack_words;
    frame.r15 = (uintptr_t)message; frame.rax = UINT64_MAX; frame.vector = 6;
    strcpy(message, "fatal\nmessage"); stack_words[7] = UINT64_MAX;
    clear(); record_user_opcode(&frame);
    assert(!strcmp(output,"OS_REALTEST_OPCODE 15 11 1 2 3 4 5 6\r\n"));
    clear(); record_user_context(&frame, &thread);
    assert(strstr(output,"OS_USER_REGS pid=0xffffffffffffffff"));
    assert(strstr(output," rax=0xffffffffffffffff"));
    assert(strstr(output,"OS_USER_STACK rsp=") && strstr(output," 0xffffffffffffffff\r\n"));
    assert(strstr(output,"OS_CHROME_UD2_R15_TEXT ptr=") && strstr(output," text=fatal.message\r\n"));
    memset(message,'x',sizeof(message)); clear(); record_user_context(&frame,&thread);
    char *text = strstr(output," text="); assert(text && strlen(text + 6) == 66);
    frame.vector = 14; clear(); record_user_context(&frame,&thread);
    assert(!strstr(output,"OS_CHROME_UD2"));
    frame.vector = 6; strcpy(process.name,"other"); clear(); record_user_context(&frame,&thread);
    assert(!strstr(output,"OS_CHROME_UD2"));
    for (unsigned fail = 0; fail < 2; ++fail) {
        readable = fail != 0; copy_fail = fail != 0; clear();
        record_user_opcode(&frame); assert(!used);
        record_user_context(&frame,&thread);
        assert(strstr(output,"OS_USER_REGS") && !strstr(output,"OS_USER_STACK"));
    }
    clear(); record_user_opcode(NULL); record_user_context(NULL,NULL); assert(!used);
    puts("ARCH_DIAGNOSTIC_NAMES_TEST_OK");
}
'''
with tempfile.TemporaryDirectory(prefix='arch-diagnostics-') as tmp:
    fixture = Path(tmp) / 'test.c'; binary = Path(tmp) / 'test'
    fixture.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(fixture), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
