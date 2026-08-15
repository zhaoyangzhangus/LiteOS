# Final User / Kernel ABI

## Raw x86_64 syscall ABI

```text
RAX = syscall number
RDI = arg0
RSI = arg1
RDX = arg2
R10 = arg3
R8  = arg4
R9  = arg5

RAX = result
RCX/R11 clobbered
```

Negative results are stable error codes. libc converts them to its public convention.

## Entry/return

```text
SYSCALL
→ swapgs/per-CPU state
→ switch to per-thread kernel stack
→ minimal syscall frame
→ O(1) table dispatch
→ return-to-user work
→ SYSRETQ if validated
→ otherwise IRETQ
```

Initial Ring3 entry always uses an IRETQ-compatible frame.

## User pointer rule

Range checks are only preliminary. All actual user memory access uses exception-safe UAccess routines backed by an exception-fixup table.

## Syscall namespace

The stable number ranges are declared in `include/uapi/syscall.h`.

Ranges are kept sparse so a subsystem can grow without renumbering another subsystem.

设备 UAPI 使用 `OS_SYS_DEVICE_OPEN` 和 `OS_SYS_DEVICE_CONTROL`：

```text
DEVICE_OPEN(os_device_open_t *)
DEVICE_CONTROL(handle, os_device_control_t *)
```

`DEVICE_OPEN` 只接受稳定的 `device_id` 和权限位并返回不透明句柄；
`DEVICE_CONTROL(QUERY)` 通过用户缓冲区返回 `os_device_info_t`，RESET/SET_POWER
分别需要对应的句柄权限。设备注销后，仍存在的句柄只会得到 `K_EDEVREMOVED`，
不会访问已从注册表移除的设备。

异步 I/O 使用 `IO_SUBMIT(os_io_submit_t *)` 和 `IO_CANCEL(request_id)`：

- `target` 是设备句柄，至少需要设备控制权限；`completion_port` 需要完成端口写权限。
- 提交时内核复制用户向量及写入数据到内核缓冲区；读请求完成时再通过 UAccess 复制回用户缓冲区，因此请求生命周期内不会直接持有可被 `munmap` 的用户指针。
- `IO_SUBMIT` 返回不透明 request id，完成项携带同一 id；请求完成或进程退出后 id 立即失效，取消只允许提交它的进程执行。

## Handle ABI

`VM_SHARE(os_vm_share_args_t *)` 创建匿名共享段并返回不透明 section 句柄。
随后把该句柄放入 `os_vm_map_args_t.object`，并使用 `OS_VM_SHARED` 映射到当前地址空间；
共享段不允许以 `OS_VM_PRIVATE` 映射。句柄关闭后，只要仍有地址空间映射，后备页仍保持有效，
最后一个映射和最后一个句柄释放后才回收共享段。

`os_handle_t` is opaque. Applications never interpret generation/index bits.

No kernel pointer, PA, IOVA, driver structure or page descriptor is exposed.

## Versioned structures

Every extensible UAPI input starts with:

```c
os_versioned_header_t {
    size;
    version;
    flags;
}
```

The kernel rejects structures smaller than the fields required by the requested version, ignores documented trailing extension bytes, and requires reserved fields to be zero where specified.

## Data transfer

`DEVICE_ENUMERATE(os_device_enumerate_t *)` 按紧凑索引返回公开的 `device_id`、class 和状态；输出只经过用户拷贝，不暴露 `device_t` 地址、物理地址或 IOVA。

```text
small arguments/results → usercopy
bulk persistent data    → mapping/shared object
async buffers           → pin/map for request lifetime
GPU data                → GPU allocation handles
```

## Display commit

### 图形 Shell 所需的显示与输入接口

`DISPLAY_GET_INFO(os_display_info_t *)` 返回输出 0 的宽度、高度、像素步长和用户态提交格式。用户程序据此申请 GPU allocation，不直接访问 GOP 物理地址。

`INPUT_READ(os_input_event_t *, timeout_ns)` 保留为 Window Server 的底层兼容入口；窗口管理器通常使用等价的 `WINDOW_INPUT_READ`。普通应用不能绕过 Window Server 直接消费统一输入队列。

`DISPLAY_COMMIT(os_display_commit_t *)` 只接受 GPU allocation 句柄作为扫描输出
缓冲区；当前软件 GOP 后端把提交的矩形放在输出左上角。`offset`、`stride`、`width`
和 `height` 由内核校验，用户态不能直接提交
物理地址或 framebuffer 地址。`wait_fence` 非零时，提交必须等待指定 fence 值；
`signal_fence` 为无效句柄时，内核创建 fence 并把句柄写回结构体。内核在 signal
fence 完成前保持 buffer 的对象引用和 pin，确保用户关闭句柄或释放映射不会破坏
正在显示的缓冲区。

### Window Server

窗口内容和窗口策略分离：应用使用 `WINDOW_CREATE(os_window_create_t *)` 创建窗口，
内核为它分配匿名共享内容缓冲并把映射地址写回；应用只绘制该缓冲区。第一个成功调用
`WINDOW_REGISTER_MANAGER` 的用户进程成为 Window Server，随后通过
`WINDOW_ENUMERATE`、`WINDOW_MAP`、`WINDOW_SET` 和 `WINDOW_FOCUS` 读取/修改窗口布局，
读取原始输入并用 `WINDOW_INPUT_DISPATCH` 把事件投递给焦点窗口。应用通过
`WINDOW_EVENT_READ` 接收属于自己的窗口事件。Window Server 把所有窗口内容合成到自己
的 GPU allocation，再调用 `DISPLAY_COMMIT`，因此应用不会直接提交屏幕帧，也不会负责
其他窗口的 z-order 或输入焦点。
