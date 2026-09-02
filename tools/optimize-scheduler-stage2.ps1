param(
    [switch]$ShowDiff = $true
)

$ErrorActionPreference = "Stop"

function Get-RepoRoot {
    $root = (& git rev-parse --show-toplevel 2>$null)
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($root)) {
        throw "Run this script inside the LiteOS git repository."
    }
    return [System.IO.Path]::GetFullPath($root.Trim())
}

function Get-Eol {
    param([string]$Text)
    if ($Text.Contains("`r`n")) { return "`r`n" }
    return "`n"
}

function Normalize-Eol {
    param(
        [string]$Text,
        [string]$Eol
    )
    return (($Text -replace "`r`n", "`n") -replace "`r", "`n") -replace "`n", $Eol
}

function Get-FunctionRange {
    param(
        [string]$Source,
        [string]$Signature
    )

    $start = $Source.IndexOf($Signature)
    if ($start -lt 0) {
        throw "Function not found: $Signature"
    }

    $brace = $Source.IndexOf("{", $start)
    if ($brace -lt 0) {
        throw "Opening brace not found: $Signature"
    }

    $depth = 0
    for ($i = $brace; $i -lt $Source.Length; $i++) {
        $c = $Source[$i]
        if ($c -eq '{') {
            $depth++
        } elseif ($c -eq '}') {
            $depth--
            if ($depth -eq 0) {
                return @{
                    Start = $start
                    End   = $i + 1
                }
            }
        }
    }

    throw "Closing brace not found: $Signature"
}

function Replace-WholeFunction {
    param(
        [string]$Source,
        [string]$Signature,
        [string]$Replacement,
        [string]$Eol
    )

    $range = Get-FunctionRange -Source $Source -Signature $Signature
    $replacementFixed = Normalize-Eol -Text $Replacement -Eol $Eol

    return $Source.Substring(0, $range.Start) +
           $replacementFixed +
           $Source.Substring($range.End)
}

function Replace-InFunction {
    param(
        [string]$Source,
        [string]$Signature,
        [string]$Old,
        [string]$New
    )

    $range = Get-FunctionRange -Source $Source -Signature $Signature
    $prefix = $Source.Substring(0, $range.Start)
    $function = $Source.Substring($range.Start, $range.End - $range.Start)
    $suffix = $Source.Substring($range.End)

    if ($function.Contains($New)) {
        return $Source
    }

    $index = $function.IndexOf($Old)
    if ($index -lt 0) {
        throw "Expected statement not found in $Signature : $Old"
    }

    if ($function.IndexOf($Old, $index + $Old.Length) -ge 0) {
        throw "Multiple matching statements in $Signature : $Old"
    }

    $function =
        $function.Substring(0, $index) +
        $New +
        $function.Substring($index + $Old.Length)

    return $prefix + $function + $suffix
}

$Root = Get-RepoRoot
$CorePath = Join-Path $Root "kernel\sched\core.c"
$SmpPath  = Join-Path $Root "kernel\sched\smp.c"

foreach ($path in @($CorePath, $SmpPath)) {
    if (-not (Test-Path $path)) {
        throw "Missing file: $path"
    }
}

$Core = [System.IO.File]::ReadAllText($CorePath)
$Smp  = [System.IO.File]::ReadAllText($SmpPath)

$CoreEol = Get-Eol $Core
$SmpEol  = Get-Eol $Smp

$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
Copy-Item $CorePath "$CorePath.sched-stage2-$timestamp.bak" -Force
Copy-Item $SmpPath  "$SmpPath.sched-stage2-$timestamp.bak" -Force

# ---------------------------------------------------------------------
# 0. Repair the two unlock mismatches from the previous optimization.
#    This section is idempotent.
# ---------------------------------------------------------------------

$Core = Replace-InFunction `
    -Source $Core `
    -Signature "void sched_wake(" `
    -Old "scheduler_unlock_irq_disabled(&cpu->queue.lock);" `
    -New "scheduler_unlock(&cpu->queue.lock, queue_flags);"

$Core = Replace-InFunction `
    -Source $Core `
    -Signature "void schedule(" `
    -Old "scheduler_unlock(&cpu->queue.lock, queue_flags);" `
    -New "scheduler_unlock_irq_disabled(&cpu->queue.lock);"

# ---------------------------------------------------------------------
# 1. Turn the runqueue spinlock from pure TAS into TTAS.
#
# Old contention loop:
#     LOCK XCHG
#     LOCK XCHG
#     LOCK XCHG ...
#
# New contention loop:
#     LOCK XCHG once
#     ordinary cached loads + PAUSE while busy
#     retry LOCK XCHG only after the lock looks free
# ---------------------------------------------------------------------

if (-not $Core.Contains("scheduler_spin_lock_raw")) {
    $signature = "uint64_t scheduler_lock("
    $insert = $Core.IndexOf($signature)
    if ($insert -lt 0) {
        throw "Cannot locate scheduler_lock()"
    }

    $helper = @'
static void scheduler_spin_lock_raw(spinlock_t *lock) {
    for (;;) {
        if (atomic_exchange_explicit(&lock->state, 1U,
                                     memory_order_acquire) == 0U) {
            return;
        }

        while (atomic_load_explicit(&lock->state,
                                    memory_order_relaxed) != 0U) {
            __asm__ volatile ("pause");
        }
    }
}

'@
    $helper = Normalize-Eol -Text $helper -Eol $CoreEol
    $Core = $Core.Substring(0, $insert) + $helper + $Core.Substring($insert)
}

$Core = Replace-WholeFunction `
    -Source $Core `
    -Signature "uint64_t scheduler_lock(" `
    -Eol $CoreEol `
    -Replacement @'
uint64_t scheduler_lock(spinlock_t *lock) {
    uint64_t flags = scheduler_irq_save();
    scheduler_spin_lock_raw(lock);
    return flags;
}
'@

$Core = Replace-WholeFunction `
    -Source $Core `
    -Signature "static void scheduler_lock_irq_disabled(" `
    -Eol $CoreEol `
    -Replacement @'
static void scheduler_lock_irq_disabled(spinlock_t *lock) {
    scheduler_spin_lock_raw(lock);
}
'@

# ---------------------------------------------------------------------
# 2. IPI acknowledgement is single-writer per CPU slot.
#    Remove LOCK XADD from every scheduler IPI.
# ---------------------------------------------------------------------

$ipiRange = Get-FunctionRange -Source $Smp -Signature "void x86_smp_ipi_interrupt("
$ipiPrefix = $Smp.Substring(0, $ipiRange.Start)
$ipiBody = $Smp.Substring($ipiRange.Start, $ipiRange.End - $ipiRange.Start)
$ipiSuffix = $Smp.Substring($ipiRange.End)

if (-not $ipiBody.Contains("uint64_t acknowledgements =")) {
    $oldAck = "atomic_fetch_add_explicit(&g_ipi_acknowledgements[cpu_index], 1U," +
              $SmpEol +
              "                                  memory_order_release);"

    if (-not $ipiBody.Contains($oldAck)) {
        throw "Cannot locate IPI acknowledgement increment."
    }

    $newAck = @'
uint64_t acknowledgements =
            atomic_load_explicit(&g_ipi_acknowledgements[cpu_index],
                                 memory_order_relaxed);
        atomic_store_explicit(&g_ipi_acknowledgements[cpu_index],
                              acknowledgements + 1U,
                              memory_order_release);
'@
    $newAck = Normalize-Eol -Text $newAck -Eol $SmpEol
    $ipiBody = $ipiBody.Replace($oldAck, $newAck)
    $Smp = $ipiPrefix + $ipiBody + $ipiSuffix
}

# ---------------------------------------------------------------------
# 3. Duplicate reschedule requests should normally be only an atomic load.
#    Only the first producer that flips false -> true executes LOCK CMPXCHG
#    and rings the APIC doorbell.
# ---------------------------------------------------------------------

$Smp = Replace-WholeFunction `
    -Source $Smp `
    -Signature "bool x86_smp_request_reschedule(" `
    -Eol $SmpEol `
    -Replacement @'
bool x86_smp_request_reschedule(uint32_t cpu_index) {
    const x86_acpi_platform_t *platform = x86_acpi_platform();
    if (platform == 0 || cpu_index >= platform->cpu_count ||
        !x86_smp_cpu_online(cpu_index)) return false;

    /*
     * Most wake bursts arrive while a request is already pending.
     * Avoid an unconditional LOCK XCHG in that common case.
     *
     * The producer has already published its runqueue update before reaching
     * this function. If the target concurrently consumes the old request, it
     * will schedule after that publication; otherwise the CAS below installs
     * a new request and sends the IPI.
     */
    if (atomic_load_explicit(&g_reschedule_pending[cpu_index],
                             memory_order_relaxed)) {
        return true;
    }

    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &g_reschedule_pending[cpu_index],
            &expected,
            true,
            memory_order_release,
            memory_order_relaxed)) {
        return true;
    }

    for (uint32_t attempt = 0; attempt < 4U; ++attempt) {
        if (liteos_lapic_send_fixed(platform->cpus[cpu_index].apic_id,
                                    X86_SMP_IPI_VECTOR)) {
            return true;
        }
        __asm__ volatile ("pause");
    }

    /* pending remains set; the target timer/idle polling is the fallback. */
    return false;
}
'@

# ---------------------------------------------------------------------
# 4. Idle loops call take_reschedule_request() very frequently.
#    Do not execute LOCK XCHG(false) when the flag is already false.
# ---------------------------------------------------------------------

$Smp = Replace-WholeFunction `
    -Source $Smp `
    -Signature "bool x86_smp_take_reschedule_request(" `
    -Eol $SmpEol `
    -Replacement @'
bool x86_smp_take_reschedule_request(void) {
    uint32_t cpu_index = x86_current_cpu_index();
    if (cpu_index >= g_discovered_count || cpu_index >= MAX_CPUS) return false;

    /*
     * Fast path for the overwhelmingly common idle-poll case.
     * A producer that races after this load still sends an IPI, and the
     * runnable snapshot is checked by the idle loop as a second condition.
     */
    if (!atomic_load_explicit(&g_reschedule_pending[cpu_index],
                              memory_order_relaxed)) {
        return false;
    }

    return atomic_exchange_explicit(&g_reschedule_pending[cpu_index], false,
                                    memory_order_acq_rel);
}
'@

$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($CorePath, $Core, $Utf8NoBom)
[System.IO.File]::WriteAllText($SmpPath, $Smp, $Utf8NoBom)

Write-Host ""
Write-Host "Scheduler stage-2 optimization applied."
Write-Host ""

git diff --check -- kernel/sched/core.c kernel/sched/smp.c
if ($LASTEXITCODE -ne 0) {
    throw "git diff --check failed"
}

if ($ShowDiff) {
    git diff -- kernel/sched/core.c kernel/sched/smp.c
}

Write-Host ""
Write-Host "Recommended validation:"
Write-Host "  .\tools\build-windows.ps1"
Write-Host "  .\tools\run-qemu-auto.ps1"
