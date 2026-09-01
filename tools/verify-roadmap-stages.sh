#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
root_dir="$(CDPATH= cd -- "$script_dir/.." && pwd)"
cd "$root_dir"

contracts_path="$script_dir/roadmap-stage-contracts.tsv"
build_dir="${BUILD:-build-refactor}"
static_only=0

usage() {
    cat >&2 <<'EOF'
usage: tools/verify-roadmap-stages.sh [--static]
       tools/verify-roadmap-stages.sh [kernel.elf] [qemu-serial.log]

--static checks the source-level stage contract without requiring a build or
QEMU log.  The two-argument form also runs the ELF/DWARF and serial-stage
location checks for the complete debug image.
EOF
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    usage
    exit 0
fi
if [[ "${1:-}" == "--static" ]]; then
    static_only=1
    shift
    if (($# != 0)); then
        usage
        exit 2
    fi
elif (($# > 2)); then
    usage
    exit 2
fi

elf_path="${1:-$build_dir/esp/EFI/LITEOS/kernel.elf}"
log_path="${2:-$build_dir/qemu-serial.log}"

if [[ ! -f "$contracts_path" ]]; then
    printf 'roadmap stage verification: contract file not found: %s\n' \
        "$contracts_path" >&2
    exit 2
fi

failures=0
contract_count=0
declare -A seen_contracts=()
for required in \
    include/kernel/debug_stage.h \
    kernel/debug_stage.c; do
    if [[ ! -f "$required" ]]; then
        printf 'roadmap stage verification: required file not found: %s\n' \
            "$required" >&2
        failures=$((failures + 1))
    fi
done

while IFS='|' read -r family phase serial_name owner_source; do
    [[ -n "$family" && "${family:0:1}" != "#" ]] || continue
    contract_count=$((contract_count + 1))

    if [[ "$family" != SPEC && "$family" != REFACTOR ]]; then
        printf 'roadmap stage verification: invalid family: %s\n' \
            "$family" >&2
        failures=$((failures + 1))
        continue
    fi
    contract_key="$family:$phase"
    if [[ -n "${seen_contracts[$contract_key]:-}" ]]; then
        printf 'roadmap stage verification: duplicate contract: %s\n' \
            "$contract_key" >&2
        failures=$((failures + 1))
    fi
    seen_contracts[$contract_key]=1
    if [[ ! "$phase" =~ ^[0-9]+$ && "$family" == SPEC ]]; then
        printf 'roadmap stage verification: invalid specification phase: %s\n' \
            "$phase" >&2
        failures=$((failures + 1))
        continue
    fi
    if [[ "$family" == REFACTOR && ! "$phase" =~ ^(0|[1-6]|7A|7B|8|9)$ ]]; then
        printf 'roadmap stage verification: invalid refactor phase: %s\n' \
            "$phase" >&2
        failures=$((failures + 1))
        continue
    fi
    if [[ "$owner_source" != kernel/* ||
          ! -f "$owner_source" ]]; then
        printf 'roadmap stage verification: Owner source not found for %s: %s\n' \
            "$serial_name" "$owner_source" >&2
        failures=$((failures + 1))
        continue
    fi

    symbol="LITEOS_DEBUG_PHASE_${family}_${phase}"
    if ! grep -Fq -- "$symbol" include/kernel/debug_stage.h; then
        printf 'roadmap stage verification: enum missing for %s: %s\n' \
            "$serial_name" "$symbol" >&2
        failures=$((failures + 1))
    fi
    if ! grep -Fq -- "\"$serial_name\"" kernel/debug_stage.c; then
        printf 'roadmap stage verification: serial name missing for %s\n' \
            "$serial_name" >&2
        failures=$((failures + 1))
    fi

    # Match a location-aware stage macro invocation in the declared Owner,
    # rather than accepting a bare enum mention in a comment or header.
    call_pattern="liteos_debug_(stage|trace_stage)(_[[:alnum:]_]+)?[[:space:]]*\\(${symbol}"
    if ! grep -Eq -- "$call_pattern" "$owner_source"; then
        printf 'roadmap stage verification: no publication call for %s in %s\n' \
            "$serial_name" "$owner_source" >&2
        failures=$((failures + 1))
    fi
done < "$contracts_path"

if ((contract_count != 32)); then
    printf 'roadmap stage verification: expected 32 contracts, found %d\n' \
        "$contract_count" >&2
    failures=$((failures + 1))
fi

if ((failures != 0)); then
    printf 'roadmap stage verification: source contract failed (%d issue(s))\n' \
        "$failures" >&2
    exit 1
fi

if ((static_only != 0)); then
    printf 'roadmap stage verification: source contract OK (%d phases)\n' \
        "$contract_count"
    exit 0
fi

if [[ ! -f "$elf_path" || ! -f "$log_path" ]]; then
    printf 'roadmap stage verification: ELF/log required for runtime mode: %s, %s\n' \
        "$elf_path" "$log_path" >&2
    exit 2
fi

# Keep runtime coverage driven by the same manifest as the source contract.
# This prevents a newly added phase from becoming statically locatable while
# silently disappearing from the QEMU log.  The detailed verifier below still
# checks the exact boot sequence and every record's source line.
runtime_contract_errors=0
while IFS='|' read -r family phase serial_name owner_source; do
    [[ -n "$family" && "${family:0:1}" != "#" ]] || continue
    phase_records=$(grep -F "LITEOS_STAGE phase=$serial_name " "$log_path" || true)
    if [[ -z "$phase_records" ]]; then
        printf 'roadmap stage verification: runtime phase missing: %s\n' \
            "$serial_name" >&2
        runtime_contract_errors=$((runtime_contract_errors + 1))
        continue
    fi
    if ! printf '%s\n' "$phase_records" |
        grep -Eq ' loc=kernel/[^[:space:]]+:[0-9]+([[:space:]]|$)'; then
        printf 'roadmap stage verification: runtime phase has no source location: %s\n' \
            "$serial_name" >&2
        runtime_contract_errors=$((runtime_contract_errors + 1))
    fi
    # The manifest names the first debugger entry point for the phase.  Require
    # that file to appear in the runtime record set as well; a location in an
    # unrelated helper is useful for tracing, but cannot replace the Owner
    # boundary declared by the roadmap.
    owner_records=$(printf '%s\n' "$phase_records" |
        grep -F " loc=${owner_source}:" || true)
    if [[ -z "$owner_records" ]]; then
        printf 'roadmap stage verification: runtime phase has no Owner location: %s (%s)\n' \
            "$serial_name" "$owner_source" >&2
        runtime_contract_errors=$((runtime_contract_errors + 1))
    fi
done < "$contracts_path"

if ((runtime_contract_errors != 0)); then
    printf 'roadmap stage verification: runtime contract failed (%d issue(s))\n' \
        "$runtime_contract_errors" >&2
    exit 1
fi

printf 'roadmap stage verification: runtime contract OK (%d phases)\n' \
    "$contract_count"

"$script_dir/verify-debug-locations.sh" "$elf_path" "$log_path"
printf 'roadmap stage verification: complete contract OK (%d phases)\n' \
    "$contract_count"
