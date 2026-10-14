#pragma once

#include <kernel/base.h>
#include <kernel/spinlock.h>
#include <arch/x86_64/context.h>

struct file;
struct process;
struct thread;
/* Process registry owns lookup and pins the returned thread for the caller. */
struct thread *linux_thread_reference_by_tid(uint64_t tid);
uint64_t linux_thread_visible_tid(const struct thread *thread);
struct nv_frontend_file;
struct socket;
struct window_server_window;

/* Match the advertised Linux NOFILE budget. Chromium's databases, shared
 * images and IPC channels exceed 256 even with only a few live tabs. */
#define OS_LINUX_FD_LIMIT 1024U

enum linux_fd_kind {
    LINUX_FD_UNUSED = 0U,
    LINUX_FD_FILE = 1U,
    LINUX_FD_CONSOLE = 2U,
    LINUX_FD_NVIDIA = 3U,
    LINUX_FD_PROC = 4U,
    LINUX_FD_WINDOW = 5U,
    LINUX_FD_SOCKET = 6U,
    LINUX_FD_PIPE = 7U,
    LINUX_FD_EPOLL = 8U,
    LINUX_FD_EVENTFD = 9U,
    LINUX_FD_UNIX_SOCKET = 10U,
    LINUX_FD_NULL = 11U,
};

typedef struct linux_fd_entry {
    struct file *file;
    void *private_data;
    uint32_t flags;
    uint8_t kind;
    uint8_t reserved[3];
    struct linux_fd_description *description;
} linux_fd_entry_t;

typedef struct linux_fd_table {
    spinlock_t lock;
    linux_fd_entry_t entries[OS_LINUX_FD_LIMIT];
} linux_fd_table_t;

enum process_personality {
    OS_PROCESS_PERSONALITY_NATIVE = 0U,
    OS_PROCESS_PERSONALITY_LINUX_X86_64 = 1U,
};

void linux_fd_table_init(linux_fd_table_t *table);
void linux_fd_table_destroy(linux_fd_table_t *table);
kstatus_t linux_fd_table_clone(const linux_fd_table_t *source,
                               linux_fd_table_t *destination);
void linux_fd_table_close_on_exec(struct process *process);
/* Reset under the table lock, transferring old slots to caller ownership.
 * Release the snapshot after dropping process/table locks. */
void linux_fd_table_reset_stdio(linux_fd_table_t *table,
                                linux_fd_entry_t previous[3]);
void linux_fd_table_release_stdio(linux_fd_entry_t previous[3]);

/* Called by the process lifecycle code; this is separate from syscall
 * dispatch so clone/set_tid_address cleanup cannot become fake success. */
void linux_thread_exit_cleanup(struct thread *thread);

bool linux_process_is(struct process *process);
bool linux_elf_interpreter_path(const char *path);

int64_t linux_syscall_dispatch(arch_trap_frame_t *frame);

/* Phase 7 acceptance harness; only invoked by a Phase 7 test build. */
bool linux_phase7_realtest(void);

/* Phase 8 acceptance harness; only invoked by a Phase 8 test build. */
bool linux_phase8_realtest(void);

/* Phase 9 acceptance harness; only invoked by a Phase 9 test build. */
bool linux_phase9_realtest(void);

/* Phase 10 acceptance harness; only invoked by a Phase 10 test build. */
bool linux_phase10_realtest(void);

/* Phase 11 acceptance harness; only invoked by a Phase 11 test build. */
bool linux_phase11_realtest(void);

/* Phase 12 acceptance harness; only invoked by a Phase 12 test build. */
bool linux_phase12_realtest(void);

/* Phase 13 Linux event/epoll acceptance harness. */
bool linux_phase13_realtest(void);
