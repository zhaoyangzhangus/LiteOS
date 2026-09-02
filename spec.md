# LiteOS Scheduler Performance Optimization Tasks

目标：基于当前最新调度器 ownership/SPSC 架构，在不回退架构设计的前提下，解决当前 benchmark 回退：

```text
改善：
enqueue          -7.6%
wake             -6.6%
context_switch  -46.5%   # 暂不视为可信收益

回退：
schedule         +14.7%
block            +39.1%
wake_to_running   +8.4%
dequeue           +6.2%
```

核心原则：

```text
不要推翻 owner CPU + SPSC 架构。

当前主要问题不是 SPSC 本身，
而是太多跨 CPU bookkeeping 被塞进了普通 schedule() 热路径。
```

---

# P0 — 先修 benchmark

在继续优化前，先把 benchmark 改成有统计意义的测试。

## schedule

不要只测单次：

```c
start();
schedule();
stop();
```

改成：

```text
warmup
+
10000~100000 次
+
total cycles / operations
```

分别测试：

```text
schedule_same_thread
schedule_two_fair_threads
schedule_rt
schedule_with_remote_command
schedule_without_remote_command
```

---

## enqueue/dequeue

不要只测 300 个线程中的第 1 次。

分别测试：

```text
FAIR enqueue x 10000
FAIR dequeue x 10000

RT enqueue x 10000
RT dequeue x 10000
```

输出：

```text
min
median
average
p95
```

至少输出 average。

---

## wake

必须拆成：

```text
local_wake
remote_wake_submit
remote_wake_to_ready
remote_wake_to_running
```

现在的 `scheduler.wake` 基本是 local wake，不能代表 SPSC remote wake。

---

## context_switch

当前单次 raw round-trip 数据噪声过大。

改成：

```text
连续执行 10000~100000 次 context switch round-trip
```

再平均。

在 benchmark 修复前：

> 不把 context_switch -46.5% 视为真实优化结果。

---

# P1 — schedule() 不再无条件扫描所有 SPSC inbox

这是最高优先级。

当前普通：

```c
schedule()
```

会无条件：

```c
scheduler_command_drain_local(cpu_id, cpu);
```

而 drain 会：

```text
遍历所有 source CPU

primary ring:
    head
    tail

overflow ring:
    head
    tail
```

因此即使：

```text
没有任何 remote command
```

也会执行：

```text
O(NCPU)
```

的 cacheline / atomic load。

这是 `schedule +14.7%` 的第一嫌疑。

---

## 修改目标

普通 schedule：

```c
schedule()
{
    if (scheduler_commands_pending(cpu_id))
        scheduler_command_drain_local(cpu_id, cpu);

    ...
}
```

但：

```c
scheduler_commands_pending()
```

不能扫描所有 ring。

应增加 per-CPU：

```c
atomic_bool command_pending;
```

或者更适合单写架构的：

```c
uint32_t command_pending;
```

由 producer 在：

```text
empty -> non-empty
```

时发布。

consumer drain 完全部 command 后清除。

---

## 最终目标

普通 local schedule：

```text
1 个 pending load
```

而不是：

```text
扫描 NCPU 个 SPSC ring
```

---

# P2 — 避免 IPI handler -> schedule() 重复 drain

当前可能出现：

```text
IPI
 ↓
sched_handle_reschedule_request()
 ↓
scheduler_command_drain_local()
 ↓
schedule()
 ↓
scheduler_command_drain_local() AGAIN
```

这会把同一个 scheduler boundary 的 command fabric 扫两遍。

---

## 修改方案

让：

```c
scheduler_command_drain_local()
```

返回：

```c
uint32_t consumed;
```

或者 schedule 接受：

```c
schedule_flags
```

例如：

```c
schedule_internal(bool commands_already_drained);
```

IPI handler：

```c
drain();
schedule_internal(true);
```

普通路径：

```c
schedule_internal(false);
```

原则：

> 一个 scheduling boundary 最多 drain 一次。

---

# P3 — command drain 空路径必须极轻

当前：

```c
scheduler_command_release_drain()
```

在 schedule 中即使没有 release item，也可能执行：

```text
IRQ save
IRQ restore
```

或者进入额外循环。

应改成：

```c
if (release_head != NULL)
    scheduler_command_release_drain(...);
```

或者维护：

```c
release_pending
```

确保：

```text
empty path = 1 个 predictable branch
```

---

# P4 — schedule() 内 snapshot 只发布一次

当前：

```text
enqueue_local()
    -> publish snapshot

dequeue_local()
    -> publish snapshot

schedule()
    -> publish snapshot again
```

一次 schedule 可能发布 2~3 次 snapshot。

---

## 改成 raw/local 两层 API

例如：

```c
enqueue_local_raw(cpu, thread);
dequeue_local_raw(cpu, thread);
```

不 publish。

普通外部 local enqueue：

```c
enqueue_local()
{
    enqueue_local_raw();
    scheduler_publish_queue_snapshot();
}
```

schedule：

```c
schedule()
{
    ...
    enqueue_local_raw(current);
    ...
    dequeue_local_raw(next);
    ...
    scheduler_publish_queue_snapshot(cpu); // exactly once
}
```

---

## 目标

每个 schedule：

```text
最多一次 snapshot calculation
最多一次 snapshot store
```

---

# P5 — `sched_block_current()` 做 owner-local fast path

当前 block 路径存在重复：

```text
sched_block_current()
    ↓
已经知道 current CPU/current thread
    ↓
sched_publish_blocked()
    ↓
再次 current CPU lookup
再次 owner validation
再次 CPU available 检查
```

这是不必要的。

---

## 新增

```c
static inline bool sched_publish_blocked_local(
    uint32_t cpu_id,
    thread_t *thread);
```

假设调用者已经确认：

```text
thread == cpu->queue.current
thread->owner_cpu == cpu_id
```

只执行：

```c
if (state != THREAD_RUNNING)
    return false;

epoch++;

state = THREAD_BLOCKED;
```

---

# P6 — block_epoch 内存序优化

当前 block：

```text
block_epoch update
state -> BLOCKED
```

无需两个 release。

推荐：

```c
atomic_store_explicit(
    &thread->block_epoch,
    new_epoch,
    memory_order_relaxed);

atomic_store_explicit(
    &thread->state,
    THREAD_BLOCKED,
    memory_order_release);
```

remote wake：

```c
state acquire
```

即可看到 epoch publication。

原则：

```text
epoch = relaxed
state BLOCKED = release
remote state = acquire
```

---

# P7 — owner CPU 写 thread state 时减少 CAS

ownership 已经建立后，重新审计：

```text
thread->state
```

所有 mutation。

如果确认：

```text
只有 owner CPU 写
```

则：

```c
CAS(RUNNING, BLOCKED)
CAS(BLOCKED, READY)
CAS(READY, ...)
```

逐步替换成：

```text
owner-side load/check
+
plain/atomic store
```

remote CPU：

```text
禁止写 state
只发 scheduler command
```

保留 acquire reader 即可。

不要一次全部替换。

按：

```text
block
wake
enqueue
migration
exit
```

逐路径验证。

---

# P8 — local wake 不要给自己发 LAPIC IPI

当前 local wake 成功后如果仍然：

```c
x86_smp_request_reschedule(cpu_id);
```

且：

```text
cpu_id == caller_cpu
```

就可能产生 self IPI。

这不合理。

---

## 改成

per-CPU：

```c
bool need_resched;
```

local wake：

```c
cpu->need_resched = true;
```

remote wake：

```text
SPSC
+
IPI
```

---

## 但必须先确认这些路径消费 need_resched

```text
syscall exit
interrupt exit
scheduler boundary
preempt enable
idle loop
```

必须全部覆盖后才能删 self IPI。

---

# P9 — need_resched 成为核心调度条件

不要：

```text
timer tick == schedule()
```

应该：

```text
timer
 ↓
accounting
 ↓
policy decision
 ↓
need_resched = true/false
```

只有：

```text
need_resched
```

才进入 schedule。

---

## FAIR

如果：

```text
nr_running == 0
```

当前线程就是唯一 runnable：

```text
不 schedule
```

---

## RT

只有：

```text
更高优先级 thread
RR slice expired
policy requires preemption
```

才设置 need_resched。

---

# P10 — FAIR RB-tree 增加 cached leftmost

当前 FAIR 经常：

```c
rb_tree_first(&rq->fair_root);
```

沿左边遍历。

增加：

```c
rb_node_t *fair_leftmost;
```

或：

```c
thread_t *fair_leftmost;
```

---

## insert

如果：

```text
new vruntime < leftmost
```

更新 leftmost。

---

## remove

只有：

```text
removed == leftmost
```

才计算 successor/new leftmost。

---

## pick

直接：

```c
return rq->fair_leftmost;
```

目标：

```text
FAIR pick O(1)
```

保持 RB-tree insert/remove O(logN)。

---

# P11 — schedule fast path

最终普通 local schedule 应接近：

```c
schedule()
{
    irq_disable();

    if (unlikely(command_pending))
        drain_commands();

    current = cpu->current;

    if (current_should_requeue)
        enqueue_raw(current);

    next = pick_next_fast();

    if (next != current)
        dequeue_raw(next);

    publish_snapshot_once();

    if (next == current) {
        irq_restore();
        return;
    }

    context_switch();
}
```

---

## 重点增加 `next == current` fast path

如果最终：

```text
next == current
```

尽量不要继续：

```text
FPU switch bookkeeping
CR3 bookkeeping
TSS update
context switch preparation
```

直接返回。

---

# P12 — command fabric 不允许 O(NCPU) common scan

长期优化。

当前：

```text
destination CPU
遍历所有 source ring
```

即使增加 `command_pending`，真的有 command 时仍然 O(NCPU)。

进一步增加：

```text
active source bitmap
```

例如：

```c
uint64_t active_sources;
```

如果 CPU 数 <=64。

producer：

```text
ring empty -> non-empty
    ↓
set source bit
```

consumer：

```text
while bitmap:
    source = ctz(bitmap)
    drain source ring
```

但注意：

> 不要用一个所有 producer 竞争的全局 atomic bitmap，重新制造 MPSC cacheline contention。

更适合：

```text
每 source 一个 pending byte/cacheline-friendly flag
+
destination 维护 cached active list
```

或者保留 O(N) drain，仅保证：

```text
没有 command 时不扫描
```

先做 P1，之后测量再决定。

---

# P13 — SPSC ring entry 尽量紧凑

当前 command struct 如果过大，检查能否缩成：

```c
struct sched_cmd {
    uintptr_t thread;
    uint32_t generation;
    uint16_t op;
    uint16_t data;
};
```

优先控制到：

```text
16 bytes
```

甚至后期考虑：

```text
8 bytes
```

但：

> 不允许为了压结构破坏 generation/ownership correctness。

---

# P14 — ring head/tail cacheline 与 layout

确认：

```text
producer tail
consumer head
```

不在同 cacheline。

检查：

```c
_Static_assert(offsetof(...tail...) ...)
```

必要时：

```c
alignas(64)
```

但避免每个 8-entry ring 膨胀过大。

如果：

```text
MAX_CPU = 256
```

每 CPU pair 都做 128B header 会非常浪费。

应评估真实 CPU 上限。

---

# P15 — release queue 与 command refs 优化

ownership 重构后增加的：

```text
command reference
release queue
deferred release
```

不要让它们进入每次 schedule common path。

目标：

```text
只有真的 command ref -> 0
```

才进入 release handling。

空 schedule：

```text
0 release bookkeeping
```

---

# P16 — migration 不进普通 schedule common path

检查：

```text
migration_pending
migration_complete
owner transfer
```

是否每次 schedule 都检查。

如果是：

```text
unlikely()
```

并用：

```c
if (unlikely(cpu->migration_pending))
```

保护。

普通线程：

```text
0 migration work
```

---

# P17 — branch hint

对明确冷路径使用：

```c
likely()
unlikely()
```

例如：

```text
remote command pending
migration
thread DEAD
reaper
ring overflow
fallback
```

不要滥用。

schedule common path应是：

```text
running FAIR thread
无 remote command
无 migration
无 death
```

---

# P18 — struct scheduler_cpu 热冷分离

重新看：

```c
scheduler_cpu_t
```

把 schedule 每次访问的放前面：

```text
current
rq hot fields
nr_running
need_resched
fair_leftmost
snapshot
```

冷字段：

```text
reaper
migration diagnostics
command stats
overflow
debug counters
```

放后面甚至单独 cold struct。

目标：

```text
schedule hot fields 尽可能落在 1~2 cacheline
```

---

# P19 — thread_t scheduler hot fields聚合

将 schedule 高频字段尽量放一起：

```text
state
owner_cpu
sched_class
priority
flags
vruntime
rq linkage
```

避免：

```text
schedule()
```

访问 thread_t 多个远距离 cacheline。

block_epoch/migration/debug 等低频字段可以分离。

重新 benchmark `dequeue +6.2%`，因为这次 thread_t 变大很可能影响 cache locality。

---

# P20 — dequeue benchmark 后再决定算法改动

不要为了现在的：

```text
dequeue +6.2%
```

立即换 RB-tree。

先完成：

```text
P0 benchmark fix
P10 cached leftmost
P18/P19 cache layout
```

再测。

只有确认 RB erase 真的是热点，再做更深入优化。

---

# P21 — remote wake benchmark

新增真正的跨核测试：

```text
CPU0:
    wake thread owned by CPU1

CPU1:
    consume SPSC
    READY
    RUNNING
```

记录：

```text
submit cycles
IPI latency
queue wait
drain cycles
READY latency
RUNNING latency
```

必须区分：

```text
ring submit
IPI
scheduler
context switch
```

否则无法知道 remote wake 慢在哪里。

---

# P22 — scheduler telemetry 不进入 hot measurement

确认 telemetry：

```text
timestamp
emit
buffer
atomic counter
```

不污染被测范围。

正确：

```text
start
operation repeated N times
end

emit once
```

不要：

```text
每次 operation 都 emit
```

---

# 执行优先级

## 第一批：必须马上做

```text
P0  benchmark 修复
P1  schedule 不再无条件扫描 command rings
P2  消除重复 drain
P3  release empty fast-path
P4  snapshot 一次发布
P5  block owner-local fast path
P6  block_epoch 内存序优化
```

目标首先解决：

```text
schedule +14.7%
block    +39.1%
wake_to_running +8.4%
```

---

## 第二批

```text
P8  local wake 去 self IPI
P9  need_resched
P10 FAIR cached leftmost
P11 schedule fast path
P15 release bookkeeping cold path
P16 migration cold path
```

---

## 第三批

```text
P7  state CAS -> owner store
P12 command fabric进一步优化
P13 command压缩
P14 SPSC layout
P18 scheduler_cpu cache layout
P19 thread_t cache layout
P20 dequeue专项
P21 remote wake专项benchmark
```

---

# 阶段验收目标

第一阶段完成后，希望至少看到：

```text
schedule          < baseline
block             接近或低于 baseline
wake_to_running   接近 schedule 的改善
enqueue           保持当前 -7.6% 左右
wake              保持当前 -6.6% 或更好
```

如果：

```text
schedule 仍 > baseline
```

不要继续扩大 SPSC 架构，继续 profile schedule。

---

# 最重要的架构要求

不要因为 benchmark 回退重新引入：

```text
remote rq lock
remote thread state writer
global MPSC scheduler queue
```

当前 ownership/SPSC 方向保留。

现在需要做的是：

> 把 ownership 架构的管理成本从 common path 移到真正发生 remote event 的 cold path。

最终 common local schedule 应该几乎完全不知道：

```text
其他 CPU
SPSC
migration
command release
remote ownership
```

除非：

```text
pending flag != 0
```

才进入这些路径。