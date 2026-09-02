# LiteOS Page Fault / Window Surface Performance Optimization Specification

## 1. 背景

当前现象：

```text
每次打开窗口
    ↓
出现短暂停顿 / 卡顿
    ↓
窗口显示后恢复正常
```

当前怀疑主要不是 compositor 本身，而是：

```text
窗口 surface 使用 demand paging
        ↓
第一次绘制/清屏
        ↓
大量连续 4 KiB page fault
        ↓
每个 fault 都执行较重 VM 路径
        ↓
形成 page-fault storm
```

当前窗口场景非常典型：

```text
创建窗口
    ↓
reserve 较大的虚拟 surface
    ↓
物理页暂未全部分配
    ↓
第一次 memset / clear / draw
    ↓
逐页 #PF
```

如果一个 surface 为：

```text
1920 × 1080 × 4 bytes
≈ 8 MiB
```

4 KiB page 数约为：

```text
8 MiB / 4 KiB
≈ 2048 faults
```

即使每次 fault 只有几微秒，也足以造成明显的一次性停顿。

---

# 2. 当前主要问题

当前 page fault 路径大致为：

```text
#PF
 ↓
vm_handle_fault()
 ↓
map_lock(vm_space)
 ↓
VMA RB-tree lookup
 ↓
x86_translate_page()
 ↓
global page-table lock
 ↓
backing object lookup
 ↓
page_alloc()
 ↓
zero 4 KiB
 ↓
kmalloc page metadata
 ↓
x86_map_page()
 ↓
global page-table lock AGAIN
 ↓
PTE walk/create
 ↓
RSS / generation update
 ↓
map_unlock()
```

主要问题：

```text
1 page = 1 exception
       + 1 VMA lookup
       + multiple locks
       + 1 physical allocation
       + 1 zero
       + metadata allocation
       + 1 PTE mapping
```

对稀疏匿名内存可以接受。

对窗口 framebuffer：

```text
连续
密集
顺序写入
```

非常不合适。

---

# 3. 总体目标

目标不是取消 demand paging。

而是将：

```text
逐页异常分配
```

优化成：

```text
窗口创建时适量预分配
+
page fault 时批量 fault-around
+
shared surface 使用密集 page table
+
缩短锁持有时间
+
减少重复 page-table walk
```

最终窗口创建应做到：

```text
virtual reserve 很便宜
current visible surface 快速 ready
resize 只增量 populate
首次绘制不产生大规模 #PF storm
```

---

# Phase 0 — 先建立 Page Fault Telemetry

在修改算法以前必须先测量。

## 0.1 增加 fault 分类计数

记录：

```text
page_fault_total

page_fault_user
page_fault_kernel

page_fault_not_present
page_fault_protection

page_fault_anon
page_fault_shared
page_fault_file
page_fault_cow
page_fault_stack

page_fault_window_surface
```

窗口 surface 最好给 VM object 增加明确 flag/type：

```text
VM_OBJECT_WINDOW_SURFACE
```

方便单独统计。

---

## 0.2 记录每个 fault 时间

使用 TSC：

```text
T0 = page fault entry
T1 = VMA lookup complete
T2 = backing page ready
T3 = PTE installed
T4 = page fault return
```

得到：

```text
A = VMA lookup
B = backing allocation
C = page-table mapping
D = remaining fault overhead
TOTAL
```

输出：

```text
average
p50
p90
p95
p99
max
```

不要每次 fault 打 log。

只统计。

窗口测试结束后统一输出。

---

## 0.3 统计一次窗口打开产生多少 fault

记录：

```text
window_create_begin_fault_count
window_first_present_fault_count
```

输出：

```text
window_surface_faults = delta
```

这是最重要的数据之一。

例如：

```text
Window 800x600:
shared faults = 469

Window 1920x1080:
shared faults = 2031
```

如果数量基本符合：

```text
surface_size / PAGE_SIZE
```

就可以确认存在 page-fault storm。

---

# Phase 1 — 做 A/B 验证

先不要大改 VM。

比较：

```text
resizable window
vs
fixed-size window
```

如果当前 resizable window reserve 更大的 surface：

```text
resizable
    ↓
更高首次 fault 数
    ↓
更明显卡顿
```

则进一步证明瓶颈来源。

同时测试：

```text
window created
但不 clear surface
```

和：

```text
window created
立即 memset whole visible buffer
```

如果后者才产生停顿：

> 基本确定是首次 surface page population。

---

# Phase 2 — Window Surface Initial Populate

这是第一项实际性能优化。

当前应该继续允许：

```text
reserve maximum virtual surface
```

例如：

```text
最大 resize 空间
```

但不要等待应用逐页 fault 当前窗口可见区域。

---

## 2.1 创建窗口时只 populate 当前尺寸

例如：

```text
maximum virtual size:
2560 × 1440 × 4
```

当前窗口：

```text
800 × 600 × 4
```

只分配：

```text
800 × 600 × 4
```

对应物理页。

其余仍保持：

```text
virtual reservation only
```

---

## 2.2 Populate 时使用批量页分配

不要：

```text
for each page:
    fault-like path
```

建议：

```text
surface_populate(
    object,
    page_start,
    page_count
);
```

内部：

```text
allocate N pages
initialize backing entries
map N pages
```

避免人为触发 page fault handler。

---

## 2.3 创建窗口完成后再 visible

顺序：

```text
window object create
 ↓
virtual map
 ↓
populate initial visible area
 ↓
surface clear
 ↓
publish/show window
```

而不是：

```text
publish window
 ↓
application starts drawing
 ↓
大量 page fault
 ↓
用户看见卡顿
```

---

# Phase 3 — Resize Incremental Populate

resize 时：

```text
old_size
    ↓
new_size
```

只 populate 新增加区域。

例如：

```text
800 × 600
    ↓
1200 × 800
```

不要重新处理整个 buffer。

只计算新覆盖 pages：

```text
new_pages - already_populated_pages
```

---

# Phase 4 — Fault-Around / Batch Fault

即使有 initial populate，普通 shared/anon memory 仍需要高效 fault。

当前：

```text
one #PF
    ↓
one 4 KiB page
```

改为：

```text
one #PF
    ↓
populate nearby pages
```

---

## 4.1 首先测试 batch size

建议：

```text
8 pages   = 32 KiB
16 pages  = 64 KiB
32 pages  = 128 KiB
64 pages  = 256 KiB
```

窗口 framebuffer 推荐先测：

```text
16
32
```

---

## 4.2 顺序访问优先向前 fault-around

例如 fault：

```text
page index = 100
```

可 populate：

```text
100 ... 115
```

而不是：

```text
92 ... 107
```

因为 framebuffer 第一次 clear 通常是向前顺序访问。

---

## 4.3 边界限制

batch 不能越过：

```text
VMA end
VM object size
permission boundary
guard page
```

---

## 4.4 已存在 page 直接跳过

batch 内：

```text
if page already populated:
    continue
```

不要重复分配。

---

# Phase 5 — Shared Surface Backing Store 改成 Dense Array

当前 shared anonymous backing 如果是：

```text
linked list of page nodes
```

例如：

```text
anon_page_node {
    page_index
    page
    next
}
```

那么窗口 surface 并不适合这种结构。

窗口 surface 是：

```text
固定最大页数
密集
连续 page index
```

应该直接：

```c
struct vm_dense_pages {
    page_t **pages;
    uint32_t page_count;
};
```

访问：

```c
page = pages[index];
```

O(1)。

---

# Phase 6 — 创建 Object 时一次分配 Page Pointer Table

例如最大 surface：

```text
2560 × 1440 × 4
≈ 14 MiB
≈ 3600 pages
```

pointer table：

```text
3600 × 8
≈ 28 KiB
```

非常小。

比：

```text
每个新 page
    ↓
kmalloc anon_page_node
```

更适合。

---

# Phase 7 — Window Surface 专用 VM Object

建议不要强迫窗口 framebuffer 与普通 sparse anonymous object 完全共用 backing 实现。

增加：

```text
VM_OBJECT_ANON_SPARSE
VM_OBJECT_SHARED_SPARSE

VM_OBJECT_DENSE_SHARED
VM_OBJECT_WINDOW_SURFACE
```

窗口 surface：

```text
dense page pointer array
+
batch populate
+
fault-around
```

普通 mmap 大型稀疏区域继续使用 sparse backing。

---

# Phase 8 — 物理页分配移出 Backing Spinlock

当前如果是：

```text
lock backing
 ↓
lookup
 ↓
page_alloc()
 ↓
zero page
 ↓
kmalloc metadata
 ↓
insert
 ↓
unlock
```

则 lock 持有时间过长。

改成 double-check。

---

## 8.1 推荐流程

```text
lock
 ↓
lookup(index)

if exists:
    return

unlock

allocate page
zero/init page
allocate metadata if needed

lock
 ↓
lookup(index) AGAIN

if another CPU inserted:
    unlock
    free temporary page
    return existing

insert new page
unlock
```

---

## 8.2 优点

spinlock critical section 从：

```text
page allocation
zeroing
kmalloc
list manipulation
```

缩成：

```text
lookup
insert
```

显著降低：

```text
preempt-off
IRQ/lock latency
cross-core contention
```

---

# Phase 9 — Dense Object 可完全避免 Metadata kmalloc

如果使用：

```c
page_t **pages;
```

则 fault 时：

```text
allocate page
pages[index] = page
```

完全没有：

```text
kmalloc anon_page_node
```

这是窗口路径很值得做的一项。

---

# Phase 10 — 合并 Translate + Map Page Table Walk

当前 page fault 可能执行：

```text
x86_translate_page()
    ↓
page-table lock
4-level walk
unlock
```

然后：

```text
x86_map_page()
    ↓
page-table lock
4-level walk AGAIN
create/map
unlock
```

这属于明显重复。

---

## 10.1 新增 Fault 专用接口

例如：

```c
x86_fault_map_page(
    vm_space_t *space,
    vaddr_t va,
    paddr_t pa,
    uint64_t flags,
    bool *already_present
);
```

一次：

```text
take PT lock
 ↓
walk
 ↓
if existing PTE:
    report existing
else:
    create intermediate tables
    install PTE
 ↓
unlock
```

---

## 10.2 目标

正常 not-present fault：

```text
one PT lock acquisition
one page-table walk
```

而不是：

```text
two locks
two walks
```

---

# Phase 11 — Batch Page Table Mapping

配合 fault-around：

```c
x86_map_pages(
    space,
    virtual_start,
    pages[],
    count,
    flags
);
```

一次 page-table lock 下安装多个连续 PTE。

例如：

```text
32-page fault-around
```

不要：

```text
32 × x86_map_page()
```

---

# Phase 12 — Page Table Lock 从 Global 改为 Per Address Space

这是后期优化。

如果当前：

```text
g_page_table_lock
```

保护所有 address spaces，

则：

```text
Process A fault
Process B fault
kernel map
window process fault
```

全部竞争一个 lock。

---

## 12.1 推荐

每个：

```c
vm_space_t
```

拥有：

```c
spinlock_t page_table_lock;
```

只保护：

```text
自己的 user page table
```

kernel shared page table 单独锁。

---

## 12.2 不要一开始做 hashed PT locks

先完成：

```text
global
    ↓
per-vm_space
```

通常已经能去掉绝大多数无关 contention。

只有 profile 表明单个地址空间 page-table lock 成热点以后再考虑：

```text
per-PML4
hashed page-table locks
```

---

# Phase 13 — 缩短 vm_space map_lock 持有范围

当前如果：

```text
map_lock
 ↓
VMA lookup
 ↓
backing allocation
 ↓
page zeroing
 ↓
page table mapping
 ↓
unlock
```

则锁粒度过大。

---

## 推荐流程

```text
map read lock
 ↓
lookup VMA
 ↓
validate permission
 ↓
pin/reference VM object
 ↓
capture:
    object
    offset
    protection
 ↓
unlock map

backing page lookup/allocation

map PTE

release object reference
```

---

# Phase 14 — VMA Lookup 本身暂时不要重写

当前如果已经使用 RB-tree：

```text
VMA lookup = O(logN)
```

这通常不是窗口卡顿的主要来源。

不要为了性能立即换：

```text
Maple Tree
interval tree
custom radix tree
```

先 profile。

只有：

```text
VMA lookup 占 fault latency 很高
```

才考虑替换。

---

# Phase 15 — Zero Page / Demand-Zero Optimization

窗口 framebuffer 第一次访问通常需要零初始化。

目前可能是：

```text
page_alloc(PAGE_ALLOC_ZERO)
```

意味着每页：

```text
4 KiB memset
```

---

## 15.1 如果窗口创建后必定 clear 全 buffer

那么：

> 内核先 zero page，应用马上再写满一次，相当于重复写内存。

这会浪费大量 bandwidth。

---

## 15.2 增加 no-zero populate 模式

仅在安全情况下允许：

```text
WINDOW_SURFACE_DISCARD_CONTENT
```

创建新的 window surface：

```text
physical page allocate
不清零
```

然后要求：

```text
surface 在暴露给用户/显示系统以前必须被完整 clear
```

---

## 安全要求

不能让应用读取到其他进程旧内存。

所以只能在：

```text
kernel guarantees full overwrite before user-readable
```

的路径使用。

如果无法严格保证：

> 继续 zero。

不要为了性能破坏内存隔离。

---

# Phase 16 — 可选 Shared Zero Page

对普通匿名只读零页可以：

```text
all fresh anon pages
    ↓
map shared global zero page read-only
```

第一次 write：

```text
write fault
 ↓
allocate private page
```

但对于窗口 framebuffer：

```text
马上会整块写入
```

通常收益不大，甚至增加一次额外 write fault。

所以：

> 不作为窗口优化重点。

---

# Phase 17 — 2 MiB Huge Page

窗口 buffer 足够大且连续时可探索：

```text
2 MiB huge page
```

但这是后期优化。

需要：

```text
2 MiB physical contiguous allocation
2 MiB alignment
surface layout compatibility
```

不要作为第一轮解决方案。

---

# Phase 18 — Window Surface Memory Pool

如果窗口频繁：

```text
create
destroy
create
destroy
```

可以建立：

```text
window surface page pool
```

缓存近期释放的 clean pages。

减少：

```text
buddy alloc
page metadata
zero path
```

---

## Pool 规则

只缓存：

```text
已安全清零
```

或者进入 pool 后统一清零。

不能跨进程泄漏内容。

---

# Phase 19 — Zeroing 批处理

如果必须 zero：

不要每次 page fault：

```text
allocate one
zero one
```

batch populate 时：

```text
allocate N pages
 ↓
zero N pages
```

允许：

```text
SIMD
rep stosq
non-temporal store
```

以后再 benchmark。

先保证 batch 架构。

---

# Phase 20 — Prefault API

为明确知道即将访问范围的内核/用户对象提供：

```c
vm_prefault(
    address,
    length,
    flags
);
```

或者内核内部：

```c
vm_populate_range(
    space,
    start,
    length
);
```

窗口创建直接调用：

```text
vm_populate_range(initial visible surface)
```

---

# Phase 21 — Window Resize 策略

resize 不要：

```text
resize
 ↓
第一次 repaint
 ↓
大量 page fault
```

应该：

```text
resize request
 ↓
compute newly visible pages
 ↓
populate range
 ↓
update geometry
 ↓
repaint
```

---

# Phase 22 — Surface Growth Granularity

不要每次 resize 只扩展精确像素对应的 pages。

可以按：

```text
64 KiB
256 KiB
1 MiB
```

增长。

例如：

```text
需要 +80 KiB
```

直接 populate：

```text
+256 KiB
```

减少连续 resize 时反复 fault/populate。

需要 benchmark 内存占用与 latency。

---

# Phase 23 — Page Fault Fast Path

最终普通匿名 not-present fault 应接近：

```text
#PF
 ↓
lookup VMA
 ↓
lookup backing
 ↓
page exists?
   ├─ yes -> map
   └─ no  -> allocate
 ↓
install PTE
 ↓
return
```

不要进入：

```text
复杂 generic object dispatch
重复页表查询
多个 global lock
多次 metadata allocation
跨 CPU synchronization
```

---

# Phase 24 — Window Surface Fast Path

最终窗口首次显示：

```text
window_create
 ↓
reserve max VA
 ↓
allocate current visible page batch
 ↓
map current range batch
 ↓
clear once
 ↓
publish window
```

理想情况下：

```text
first draw page faults ≈ 0
```

---

# Phase 25 — Benchmark 场景

必须建立以下测试。

## A. Fixed 800×600

```text
create
first clear
first present
```

记录：

```text
fault count
create latency
first draw latency
```

---

## B. Resizable 800×600

最大 surface 可设：

```text
2560×1440
```

但当前只显示：

```text
800×600
```

验证：

```text
物理页只分配当前区域
```

---

## C. 1920×1080

验证大 surface。

---

## D. 连续创建 10/100 个窗口

查看：

```text
allocator pressure
page pool
fault p95
```

---

## E. Resize stress

```text
640x480
 ↓
800x600
 ↓
1024x768
 ↓
1280x720
 ↓
1920x1080
```

测：

```text
resize latency
new faults
new pages allocated
```

---

# Phase 26 — 输出指标

每个窗口输出：

```text
window_create_us
surface_populate_us
first_clear_us
first_present_us

fault_count
shared_fault_count

pages_allocated
pages_prefaulted
pages_fault_around

page_fault_avg_ns
page_fault_p95_ns
page_fault_p99_ns
```

---

# Phase 27 — 第一轮优化执行顺序

第一轮只做：

```text
P0  telemetry

P1  确认窗口打开时 fault 数

P2  initial visible surface populate

P3  resize incremental populate

P4  fault-around 16/32 pages
```

然后 benchmark。

不要同时重构：

```text
page-table locks
VM object storage
VMA locking
huge page
```

---

# Phase 28 — 第二轮

如果第一次窗口卡顿明显下降，再做：

```text
P5  dense shared/window surface backing

P6  删除 per-page metadata kmalloc

P8  backing allocation 移出 spinlock

P10 translate+map 合并

P11 batch page table mapping
```

---

# Phase 29 — 第三轮

只有 profile 仍显示锁 contention 时：

```text
P12 global PT lock -> per-vm-space

P13 缩短 map_lock

P18 window page pool

P19 batch zero
```

---

# Phase 30 — 最后再考虑

```text
huge pages
zero page
advanced VMA tree
hashed PT locks
NUMA allocation
```

这些不是当前窗口卡顿的第一优先级。

---

# 31. 第一轮预期效果

当前：

```text
window first draw
    ↓
hundreds/thousands of #PF
```

第一轮后目标：

```text
window create
    ↓
populate current visible surface
```

因此：

```text
first draw shared faults
≈ 0
```

或者极少。

resize：

```text
只 fault/populate 新增区域
```

---

# 32. 架构原则

保留 demand paging。

但 demand paging 主要用于：

```text
未知访问模式
稀疏内存
普通 mmap
heap
stack
```

窗口 framebuffer 属于：

```text
已知会很快连续写入
```

应采用：

```text
eager initial population
+
incremental resize
+
fault-around fallback
```

而不是纯逐页 lazy allocation。

---

# 33. 禁止事项

不要：

1. 直接把所有预留 surface 全部物理分配。
2. 为了窗口性能关闭内存隔离。
3. 把所有匿名内存都改成 dense array。
4. 在持有 spinlock 时做大量 page_alloc/zero/kmalloc。
5. 一个 32-page batch 调用 32 次完整 x86_map_page。
6. 未 benchmark 就直接上 huge page。
7. 为解决卡顿重写整个 VMA subsystem。
8. 在每次 page fault 打串口日志。
9. 让 page fault 路径参与不必要的 scheduler/跨 CPU 消息。
10. 同一个 commit 同时修改所有 VM 子系统。

---

# 34. 最终期望架构

```text
                 VM
                  │
       ┌──────────┴──────────┐
       │                     │
 sparse anonymous       dense shared
       │                     │
 demand paging          window surface
       │                     │
  one/batch fault       initial populate
                             │
                        incremental resize
                             │
                         fault-around
```

窗口路径：

```text
Window Create
     │
     ├─ reserve maximum VA
     │
     ├─ create dense surface object
     │
     ├─ populate visible pages
     │
     ├─ batch map
     │
     └─ publish window
```

page fault 变成：

```text
正常 fallback
```

而不是：

```text
窗口初始化的主要内存分配机制
```

---

# 35. Codex 第一阶段任务

Codex 先只执行：

```text
1. 增加 page fault 分段 telemetry

2. 增加 window_surface fault counter

3. 测试打开一个窗口触发的 fault 数量

4. 为窗口当前可见 surface 实现
   vm_populate_range()

5. 创建窗口时 populate 当前 width × height

6. resize 时只 populate newly exposed pages

7. 增加 16/32-page fault-around

8. Windows build + QEMU

9. 输出优化前后：
   window create latency
   first draw latency
   fault count
   fault avg/p95/p99
```

第一阶段结束后先提交数据。

暂时不要继续：

```text
dense backing rewrite
global page-table lock rewrite
VMA lock rewrite
huge page
```

如果：

```text
window first fault count
```

和卡顿已经大幅下降，

再进入第二阶段。