/* Guest-only QA workload: a syscall-free loop also exercises safe SIGKILL
 * at the timer return boundary. Never run this executable on the host. */
__attribute__((noreturn)) void _start(void) {
    for (;;) __asm__ volatile ("pause" : : : "memory");
}
