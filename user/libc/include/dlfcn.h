#pragma once

/* Dynamic-loader surface used by Chromium's POSIX code.  System currently
 * resolves its own shared objects through the process loader; these values
 * keep the standard interface source-compatible while the remaining loader
 * entry points are added to the libc ABI. */
#define RTLD_LAZY   0x0001
#define RTLD_NOW    0x0002
#define RTLD_LOCAL  0x0000
#define RTLD_GLOBAL 0x0100
#define RTLD_NOLOAD 0x0004
#define RTLD_NODELETE 0x1000
#define RTLD_DEEPBIND 0x0008
#define RTLD_DEFAULT ((void *)0)
#define RTLD_NEXT    ((void *)-1L)

typedef struct {
    const char *dli_fname;
    void *dli_fbase;
    const char *dli_sname;
    void *dli_saddr;
} Dl_info;

#ifdef __cplusplus
extern "C" {
#endif
void *dlopen(const char *file, int mode);
int dlclose(void *handle);
void *dlsym(void *handle, const char *name);
const char *dlerror(void);
int dladdr(const void *address, Dl_info *info);
#ifdef __cplusplus
}
#endif
