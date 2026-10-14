#!/usr/bin/env python3
"""Test production fault IRQ boundary with privileged instructions mocked."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
source = (root / "kernel/arch/x86_64/irq/exception.c").read_text()
helper = source[source.index("static bool exception_resolve_page_fault("):
                source.index("void x86_interrupt_dispatch(")]
for instruction in ("sti", "cli"):
    helper = helper.replace(
        '__asm__ volatile ("' + instruction + '" : : : "memory")',
        'mock_' + instruction + '()')
mock = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint64_t vaddr_t;
typedef struct { uint64_t rflags, error_code; } arch_trap_frame_t;
static bool irq, expected_irq, expected_user, expected_uaccess, result;
static bool syscall_active, preempt_disabled, tlb_active;
static bool x86_syscall_active_fast(void) { return syscall_active; }
static bool sched_preempt_disabled(void) { return preempt_disabled; }
static bool x86_tlb_shootdown_active(void) { return tlb_active; }
static unsigned enables, disables, calls;
static void mock_sti(void) { assert(!irq); irq=true; ++enables; }
static void mock_cli(void) { assert(irq); irq=false; ++disables; }
static bool vm_handle_current_fault(vaddr_t address, uint32_t error,
                                    bool user, bool uaccess) {
    assert(address==0x12345000 && error==7);
    assert(irq==expected_irq && user==expected_user && uaccess==expected_uaccess);
    ++calls; return result;
}
'''
cases = r'''
int main(void) {
    for (unsigned user=0;user<2;++user)
    for (unsigned saved_if=0;saved_if<2;++saved_if)
    for (unsigned uaccess=0;uaccess<2;++uaccess)
    for (unsigned active=0;active<2;++active)
    for (unsigned locked=0;locked<2;++locked)
    for (unsigned tlb=0;tlb<2;++tlb)
    for (unsigned success=0;success<2;++success) {
        arch_trap_frame_t frame={saved_if ? 0x202 : 2,7};
        irq=false; enables=disables=calls=0;
        syscall_active=active; preempt_disabled=locked; tlb_active=tlb;
        expected_irq=saved_if && (user || (uaccess && active && !locked && !tlb));
        expected_user=user; expected_uaccess=uaccess; result=success;
        assert(exception_resolve_page_fault(&frame,0x12345000,user,uaccess)==result);
        assert(!irq && calls==1);
        assert(enables==(unsigned)expected_irq && disables==enables);
    }
}
'''
with tempfile.TemporaryDirectory(prefix="system-fault-irq-test-") as directory:
    binary = str(pathlib.Path(directory) / "test")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                    "-x", "c", "-", "-o", binary],
                   input=mock + helper + cases, text=True, check=True)
    subprocess.run([binary], check=True)
print("page-fault IRQ boundary: 128 cases passed; only unlocked syscall UAccess restores saved IF")
