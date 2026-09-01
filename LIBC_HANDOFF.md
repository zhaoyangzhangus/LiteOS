# LiteOS libc / 内核 ABI 交接文档

更新日期：2026-08-27

## 1. 交接结论

当前树已经具备一个可在 LiteOS 用户态运行的 freestanding C 运行时和较大的
ISO C/POSIX 接口子集。`/sbin/libc-test` 可以通过 LiteOS 动态加载器启动，执行
分配、字符串、stdio、文件、管道、虚拟内存、网络地址、线程、信号、浮点和
C11 threads 测试。

这里的“完整 libc”应理解为当前目标程序所需的 libc 基础面，而不是已经通过
全部 POSIX、C 标准库和 OS 1.0 兼容性认证。新增接口必须同时补齐公共头文件、
实现、内核 ABI（若需要）和运行时测试，不能只增加导出符号。

## 2. 目录和所有权

| 组件 | 位置 | 说明 |
| --- | --- | --- |
| 公共 libc 头文件 | `user/libc/include/` | `stdio.h`、`stdlib.h`、`unistd.h`、`pthread.h`、网络、宽字符等；编译时通过 `-Iuser/libc/include` 优先使用 |
| libc 实现 | `user/libc/*.c` | 内存/字符串、分配器、stdio、时间、目录、进程、线程、socket、DNS、宽字符、数学等 |
| CRT | `user/libc/crt_start.S` | 解析 LiteOS 初始栈，调用 `main(argc, argv)`，最终进入 `exit/_exit` |
| 非 C 汇编 ABI | `user/libc/setjmp.S`、`signal_restorer.S` | `setjmp/longjmp` 和用户信号返回路径 |
| 动态加载器 | `user/runtime/ld.c` | 处理 `PT_INTERP`、`DT_NEEDED`、x86_64 RELA 和权限收紧 |
| libc 测试 | `user/runtime/libc_test.c` | `/sbin/libc-test` 的运行时回归入口 |
| UAPI | `include/uapi/*.h` | 版本化请求头、系统调用号、文件/管道/内存/进程/时间/信号/socket 结构 |
| syscall 实现 | `kernel/syscall/*.c` | 用户指针检查、状态码转换和各类系统调用 Owner |
| 句柄/资源 | `kernel/object/handle.c`、`kernel/process/*` | 内核 handle 生命周期、进程复制、exec 和回收 |

`user/libc/include/liteos/libc.h` 是实现之间共享的内部伞形头，不应被当作
宿主 libc 头文件替代品。内核代码使用 `include/` 下的内核/UAPI 头，不要从
旧的已删除兼容目录恢复头文件。

## 3. 已实现的主要接口面

- ISO C 内存和字符串：`memcpy`、`memmove`、`memset`、比较/搜索、复制、分词、
  错误字符串，以及常用 GNU/BSD 扩展（`memccpy`、`memmem`、`strlcpy` 等）。
- 分配和转换：`malloc/calloc/realloc`、对齐分配、`atof`/`strto*`、`qsort/bsearch`、
  环境变量、`atexit`、临时文件名、`realpath`。
- stdio：缓冲 `FILE`、格式化输入输出、`fopen/fdopen`、宽字符流、`getline`/
  `getdelim`、unlocked 变体、64 位 offset 变体。
- 文件和进程：`open*`、`read/write`、`pread/pwrite`、vector I/O、`stat*`、目录、
  `fcntl/ioctl`、`dup*`、`pipe*`、`fork`、`exec*`、`wait*`、路径配置。
- 内存和时间：`mmap/munmap/mprotect/msync/madvise`、`clock_*`、`nanosleep`、
  `settimeofday`、`sysconf`。
- 网络：IPv4/IPv6 地址转换、`poll/select`、基础 socket、`sendmsg/recvmsg`、
  数字和本地 DNS `getaddrinfo/getnameinfo`，以及现有网卡协议路径。
- 并发：pthread 创建/回收、互斥量、条件变量、键、一次性初始化、rwlock、
  barrier、spin lock、属性 guard size；C11 `threads.h` 的 thread/mutex/cond/
  once/TSS 基础接口。
- 语言和数学：ctype、locale 的 C locale、UTF-8 宽字符/`uchar`、setjmp，以及
  主要 math/complex/fenv 入口。实现存在不等于标准完整兼容，精度和异常语义
  仍需扩大测试，详见已知边界。

## 4. ABI 约定

### 用户态句柄、fd 和 CLOEXEC

libc 维护 fd 到内核 `os_handle_t` 的表；内核 handle 才是资源所有者。普通文件、
pipe 和 socket 的元数据必须在 fd 复用、`dup*`、`close` 时同步清理。`open*` 创建
新槽位时要重置旧的 pipe/socket 标记，避免复用 fd 后把普通文件误判为 pipe 或
socket。引用归零时由 `release_resource()` 清理 handle、flags、路径和类型状态。

`os_handle_t` 是 64 位值，低 32 位为表索引，高 32 位为 generation，零值表示
无效句柄；不要把它截断成 libc 的 `int fd` 或持久化为文件描述符。

内核已经支持逐 handle 的 `HANDLE_FLAG_CLOEXEC`，并为 `open`、`pipe2`、`socket`
和 `accept4` 的创建路径写入该标志；`OS_SYS_HANDLE_DUP/GET_FLAGS/SET_FLAGS`
也已经定义。相关实现位于 `include/uapi/abi.h`、`include/uapi/syscall.h`、
`include/kernel/handle.h`、`kernel/object/handle.c`、`kernel/syscall/handles.c`
和 `kernel/process/exec.c`。当前 libc 仍把 `FD_CLOEXEC` 主要保存在 fd 表的
`descriptor_flags` 中，`fcntl(F_SETFD)`、`dup`、`dup2`、`dup3` 和 `F_DUPFD*`
尚未全部通过上述 handle ABI 同步。因此 exec 关闭语义尚未冻结，不能把当前
本地 `F_GETFD` 检查描述成完整的 POSIX CLOEXEC 兼容。

### 系统调用请求

原始 x86_64 syscall ABI 使用 `RAX` 传系统调用号，`RDI`、`RSI`、`RDX`、`R10`、
`R8`、`R9` 传六个参数，`RAX` 返回有符号结果，`RCX` 和 `R11` 由入口破坏。
当前 `OS_SYSCALL_ABI_VERSION` 为 1。

系统调用请求结构以 `os_versioned_header_t` 开头，调用方填写 `size`、`version`
和 `flags`；内核先校验版本、结构大小和用户地址，再转换负的 `kstatus_t` 为
libc `errno`。并非所有 UAPI 结构都是请求结构，endpoint、info 和返回值结构
是否带 header 以各自的 UAPI 定义为准。新增字段只能追加，改变已有字段布局或
枚举值前必须同步更新 `tools/abi_sanity.c` 以及所有静态断言。

### 进程、fork 和 exec

`fork` 复制地址空间为 COW，继承可继承句柄；`waitpid` 负责父进程读取并回收退出
状态。`execve` 由 `kernel/process/exec.c` 加载新 ELF、重建用户栈，并在 syscall
返回边界切换到新入口。libc CRT 只依赖 LiteOS 初始栈协议，不链接宿主 CRT。

`FD_CLOEXEC` 的内核 handle 标志和 exec 时关闭语义正在独立审查中。当前测试只
覆盖创建路径和 libc 本地 `F_GETFD`/`F_SETFD`、`dup3(O_CLOEXEC)` 的结果；还没有
覆盖 `fork` 后执行新映像并逐 fd 检查的运行时测试。在该审查完成前，不要把
“exec 后 fd 继承”写成已冻结 ABI。

### 线程和 errno

每个用户线程通过 FS-base 线程上下文持有独立 `errno`；pthread 创建时必须初始化
该上下文。由 libc 自分配的线程栈可以包含低端 guard 页，内核通过
`user_stack_owned` 记录并回收；调用方提供外部栈时按 POSIX 忽略 guard 属性。
process-shared pthread 对象和完整 rwlock 所有者追踪尚未实现。

### 动态链接

用户 ET_DYN 使用 `/lib/ld-liteos.so.1` 作为 `PT_INTERP`，并通过
`DT_NEEDED=libliteosc.so.1` 解析 libc。loader/libc 必须保持无外部 NEEDED、无未解析
动态符号；新增导出时同时修改 `tools/verify-libc.sh` 和 `tools/verify-libc.ps1`。

内核 `kernel/console_printf.c` 的格式化输出与用户 `stdio` 是两条独立路径。
Debug2 构建中的 `OS_SYS_DEBUG_WRITE` 将用户诊断写到 COM1，避免把 libc 测试输出
刷到桌面 framebuffer；修改其中任一路径时都要分别验证串口日志和 `FILE` 缓冲行为。

## 5. 推荐验证流程

在当前工作树很脏、包含大量既有重构删除的前提下，禁止使用 `git reset`、
`git checkout` 或广泛清理。Windows 原生环境需要 w64devkit 的 `make.exe`、
`x86_64-elf-gcc` 和 `x86_64-elf-objdump`。为 libc 工作使用独立 BUILD 目录，
并在 UAPI 或公共头变更后强制全量重编：

```powershell
make.exe -B -f GNUmakefile BUILD=build-handoff DEBUG=2 LITEOS_DEBUG_SERIAL=1 build-handoff/elf/libc-test.elf
make.exe -B -f GNUmakefile BUILD=build-handoff DEBUG=2 LITEOS_DEBUG_SERIAL=1 abi-sanity libc-header-sanity libc-sanity
make.exe -B -f GNUmakefile BUILD=build-handoff DEBUG=2 LITEOS_DEBUG_SERIAL=1 esp
git diff --check
```

WSL/Linux 使用同一组目标，将 `make.exe` 换成 `make`；不要复用旧 BUILD 目录
来推断头文件改动已经生效。

通过标准：

1. `abi-sanity`、`libc-header-sanity`、`libc-sanity` 均成功。
2. 运行 `/sbin/libc-test` 时 QEMU 串口出现 `LITEOS_LIBC_TEST_OK`，且没有
   `PANIC`、`FAULT` 或 `TIMEOUT`；完整 ESP/native matrix 还应出现
   `LITEOS_USER_RUNTIME_OK`、`LITEOS_WINDOW_OK` 和 `LITEOS_NET_DHCP_OK`。
3. 动态检查确认 loader/libc 无外部依赖，测试 ELF 有 LiteOS `PT_INTERP`、
   `DT_NEEDED` 和 `JUMP_SLOT`。
4. 修改 UAPI 后重新编译所有依赖目标，不要只运行宿主机头文件编译。

本次交接前已在 Windows w64devkit 环境成功执行以下最小链（`build-handoff` 为
独立临时构建目录，可按需替换）：

```powershell
make.exe -B -f GNUmakefile BUILD=build-handoff `
  DEBUG=2 LITEOS_DEBUG_SERIAL=1 build-handoff/elf/libc-test.elf
make.exe -B -f GNUmakefile BUILD=build-handoff `
  DEBUG=2 LITEOS_DEBUG_SERIAL=1 abi-sanity libc-header-sanity libc-sanity
```

以上两条命令分别生成测试 ELF，并运行 ABI、头文件和动态 libc sanity。完整
ESP 集成使用上一段的 `esp` 命令单独验证。

这条链确认了新 `atof()` 的 ELF 编译、头文件 sanity、动态导出检查和
`JUMP_SLOT`。它不证明完整 `esp` 集成或 CLOEXEC exec 继承语义；并行 ABI 改动
收敛后应重新执行完整 `esp` 构建。

本轮随后执行了 `make.exe -B ... esp`，内核、loader、libc、桌面程序和 wget
均完成链接，输出 `ESP image prepared at build-handoff/esp`。这只证明构建闭环，
不替代 QEMU 运行和 CLOEXEC exec 继承测试。

Windows 目标会自动调用 `tools/verify-libc.ps1`；在 WSL/Linux 下对应目标调用
`tools/verify-libc.sh`。两者必须保持同一组导出符号和 ELF 约束，不要只运行其中
一个脚本来判断 ABI 已闭环。

需要定位多 CPU 或 debug 变化时，再运行现有的 native QEMU matrix；矩阵通过不
等于硬件恢复、长时稳定性、性能基线或 OS 1.0 验收完成。

## 6. 已知边界和下一步

当前不能宣称以下能力已经完整兼容：

- `system()` 仍返回 `ENOSYS`，不要用 `/sbin/gshell` 冒充标准 shell。
- CLOEXEC 的创建标志已经进入内核 ABI，但 libc 的 `fcntl`/`dup*` 同步和 exec
  后逐 fd 运行时验证尚未完成。
- 完整 DNS 配置/搜索域、`dlopen/dlsym`、完整 socket option ABI 尚未冻结。
- 跨进程信号、同步异常信号、stop/continue 调度尚未完成。
- process-shared pthread、rwlock 所有者追踪和真实 guard-page fault 运行测试仍缺。
- locale 目前以 C locale 为主；数学、fenv、复数和 Unicode 需要继续补充边界/精度测试。
- 物理设备恢复、长时网络/存储测试、性能回归基线、发布 ABI 和 OS 1.0 验收仍是
  更高层的未完成门槛。

建议接手顺序：

1. 先查看 `git status --short` 和本文件的最新审计消息，确认并行代理没有修改
   同一文件。
2. 完成并验证 `FD_CLOEXEC`/exec 继承语义，再将测试加入 `libc_test.c`。
3. 根据标准 libc 审计报告选择一个可由现有内核 ABI 支撑的接口族，按“头文件、
   实现、UAPI、syscall、运行时测试、导出检查”的顺序闭环。
4. 每个接口族通过后，在 `REFACTOR_STATUS.md` 追加一段带日期的事实记录，并
   保留未实现边界，避免使用“完整 POSIX”之类过宽表述。

## 7. 交接时的工作树注意事项

本次重构留下了大量用户已有的删除、重命名和未跟踪文件；这些不属于 libc 变更，
接手者不得擅自恢复或删除。只提交明确属于 libc/ABI 的文件，并在提交前用路径
过滤检查：

当前最需要继续审查的 CLOEXEC diff 集中在 `include/uapi/abi.h`、
`include/uapi/syscall.h`、`include/kernel/handle.h`、`kernel/object/handle.c`、
`kernel/syscall/handles.c`、`kernel/process/exec.c`、`user/libc/syscall.c` 和
`user/runtime/libc_test.c`；这些文件之间的语义尚未形成发布 ABI。

```powershell
git status --short -- LIBC_HANDOFF.md REFACTOR_STATUS.md user/libc user/runtime include/uapi kernel/syscall kernel/object/handle.c kernel/process/exec.c tools
```

若构建失败，先记录完整命令、首个错误和 BUILD 目录，再检查是否是并行代理留下的
半成品；不要以清理整个工作树作为首选修复手段。
