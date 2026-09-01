#!/usr/bin/env bash
set -euo pipefail

build_dir="${BUILD:-build-refactor}"
elf_path="${1:-$build_dir/esp/EFI/LITEOS/kernel.elf}"
log_path="${2:-$build_dir/qemu-serial.log}"
objdump_cmd="${OBJDUMP:-x86_64-w64-mingw32-objdump}"

if [[ ! -f "$elf_path" ]]; then
    printf 'debug location verification: ELF not found: %s\n' "$elf_path" >&2
    exit 2
fi
if [[ ! -f "$log_path" ]]; then
    printf 'debug location verification: stage log not found: %s\n' "$log_path" >&2
    exit 2
fi
if ! command -v "$objdump_cmd" >/dev/null 2>&1; then
    printf 'debug location verification: objdump not found: %s\n' "$objdump_cmd" >&2
    exit 2
fi

if ! "$objdump_cmd" -t "$elf_path" |
    grep -F 'liteos_debug_stage_at' >/dev/null; then
    printf 'debug location verification: stage symbol missing from %s\n' \
        "$elf_path" >&2
    exit 1
fi

# The kernel is linked as an ELF debug image even though its intermediate link
# object is PE/COFF.  Keep two representative source units in the check so a
# stripped or symbol-only image cannot accidentally pass the F5 contract.
for source in kernel/debug_stage.c kernel/init/early.c; do
    if ! "$objdump_cmd" --dwarf=info "$elf_path" |
        grep -F "$source" >/dev/null; then
        printf 'debug location verification: DWARF source missing: %s\n' \
            "$source" >&2
        exit 1
    fi
done

"$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/verify-qemu-stages.sh" \
    "$log_path"

# Benchmark records are another debugger-facing contract.  A timing value
# without its call site is useful for a graph, but not for explaining a
# regression in a refactor.  Keep this check separate from the stage verifier
# because a deliberately early boot-failure log may contain no benchmark yet.
benchmark_records=$(grep -E '^LITEOS_BENCH(_VALUE)? ' "$log_path" || true)
if [[ -n "$benchmark_records" ]]; then
    unlocated_benchmarks=$(printf '%s\n' "$benchmark_records" |
        grep -Ev '^LITEOS_BENCH(_VALUE)? name=[^[:space:]]+ (cycles|value)=[0-9]+ loc=kernel/[^[:space:]]+:[0-9]+([[:space:]]|$)' || true)
    if [[ -n "$unlocated_benchmarks" ]]; then
        printf 'debug location verification: unlocated benchmark record in %s\n' \
            "$log_path" >&2
        printf '%s\n' "$unlocated_benchmarks" >&2
        exit 1
    fi

    benchmark_locations=$(printf '%s\n' "$benchmark_records" |
        sed -n 's/.* loc=\(kernel\/[^[:space:]]*:[0-9][0-9]*\).*/\1/p' |
        sort -u)
    benchmark_location_errors=0
    while IFS= read -r location; do
        [[ -n "$location" ]] || continue
        source_path="${location%:*}"
        source_line="${location##*:}"
        if [[ ! -f "$source_path" ]]; then
            printf 'debug location verification: benchmark source not found: %s\n' \
                "$location" >&2
            benchmark_location_errors=$((benchmark_location_errors + 1))
            continue
        fi
        source_line_count=$(wc -l < "$source_path")
        if ((source_line > source_line_count)); then
            printf 'debug location verification: benchmark line out of range: %s\n' \
                "$location" >&2
            benchmark_location_errors=$((benchmark_location_errors + 1))
            continue
        fi
        source_text=$(sed -n "${source_line}p" "$source_path" || true)
        if [[ "$source_text" != *kernel_perf_emit_scope* &&
              "$source_text" != *kernel_perf_emit_value* ]]; then
            printf 'debug location verification: benchmark line is not a perf call: %s\n' \
                "$location" >&2
            benchmark_location_errors=$((benchmark_location_errors + 1))
        fi
    done <<< "$benchmark_locations"
    if ((benchmark_location_errors != 0)); then
        exit 1
    fi
fi

# The kmalloc summary predates the generic LITEOS_BENCH format, but it is
# still a benchmark record.  Require the same source location so a memory
# allocator regression can be opened at the report call site.
kmalloc_records=$(grep -E '^LITEOS_PERF_KMALLOC_OK ' "$log_path" || true)
if [[ -n "$kmalloc_records" ]]; then
    unlocated_kmalloc=$(printf '%s\n' "$kmalloc_records" |
        grep -Ev '^LITEOS_PERF_KMALLOC_OK .* loc=kernel/[^[:space:]]+:[0-9]+([[:space:]]|$)' || true)
    if [[ -n "$unlocated_kmalloc" ]]; then
        printf 'debug location verification: unlocated kmalloc report in %s\n' \
            "$log_path" >&2
        printf '%s\n' "$unlocated_kmalloc" >&2
        exit 1
    fi

    kmalloc_locations=$(printf '%s\n' "$kmalloc_records" |
        sed -n 's/.* loc=\(kernel\/[^[:space:]]*:[0-9][0-9]*\).*/\1/p' |
        sort -u)
    kmalloc_location_errors=0
    while IFS= read -r location; do
        [[ -n "$location" ]] || continue
        source_path="${location%:*}"
        source_line="${location##*:}"
        if [[ ! -f "$source_path" ]]; then
            printf 'debug location verification: kmalloc source not found: %s\n' \
                "$location" >&2
            kmalloc_location_errors=$((kmalloc_location_errors + 1))
            continue
        fi
        source_line_count=$(wc -l < "$source_path")
        if ((source_line > source_line_count)); then
            printf 'debug location verification: kmalloc line out of range: %s\n' \
                "$location" >&2
            kmalloc_location_errors=$((kmalloc_location_errors + 1))
            continue
        fi
        source_text=$(sed -n "${source_line}p" "$source_path" || true)
        if [[ "$source_text" != *kernel_perf_emit_report* ]]; then
            printf 'debug location verification: kmalloc line is not a perf report call: %s\n' \
                "$location" >&2
            kmalloc_location_errors=$((kmalloc_location_errors + 1))
        fi
    done <<< "$kmalloc_locations"
    if ((kmalloc_location_errors != 0)); then
        exit 1
    fi
fi

printf 'debug location verification: OK (%s, %s)\n' "$elf_path" "$log_path"
