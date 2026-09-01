#!/usr/bin/env bash
set -euo pipefail

log_path="${1:-${STAGE_LOG:-${BUILD:-build}/qemu-serial.log}}"
contracts_path="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/roadmap-stage-contracts.tsv"

if [[ ! -f "$log_path" ]]; then
    echo "stage verification: log not found: $log_path" >&2
    exit 2
fi
if [[ ! -f "$contracts_path" ]]; then
    echo "stage verification: contract file not found: $contracts_path" >&2
    exit 2
fi

# These are the stable boot/runtime boundaries.  Fine-grained USER_RUNTIME
# trace records are intentionally not required here; their purpose is to
# locate a failure inside the user image, while these markers define the
# acceptance boundary for a bootable debug image.
required_markers=(
    'LITEOS_KERNEL_OK'
    'LITEOS_CPU_INIT_OK'
    'LITEOS_MM_OK'
    'LITEOS_BUDDY_RANDOM_OK'
    'LITEOS_CONTEXT_SWITCH_OK'
    'LITEOS_DRIVER_OK'
    'LITEOS_LITEFS_OK'
    'LITEOS_NET_MANAGER_OK'
    'LITEOS_DISPLAY_CORE_OK'
    'LITEOS_USER_INIT_STARTED SERVICES=6'
    'LITEOS_LIBC_TEST_OK'
    'LITEOS_USER_RUNTIME_OK'
    'LITEOS_USER_RUNTIME_SUBTESTS_OK'
    'LITEOS_USERMODE_OK'
    'LITEOS_WINDOW_SERVER_KERNEL_OK'
    'LITEOS_WINDOW_WORKER_OK'
)

required_stages=(
    'LITEOS_STAGE phase=BOOT step=5 value=1'
    'LITEOS_STAGE phase=CPU step=5 value=1'
    'LITEOS_STAGE phase=MEMORY step=5 value=1'
    'LITEOS_STAGE phase=MEMORY step=14 value=2048'
    'LITEOS_STAGE phase=SCHED step=5 value=1'
    'LITEOS_STAGE phase=DRIVER step=5 value=1'
    'LITEOS_STAGE phase=STORAGE step=5 value=1'
    'LITEOS_STAGE phase=NETWORK step=5 value=1'
    'LITEOS_STAGE phase=DISPLAY step=5 value=1'
    'LITEOS_STAGE phase=USER_RUNTIME step=5 value=1'
    'LITEOS_STAGE phase=DESKTOP step=4 value=1'

    # Specification phases 0..18 have runtime self-test boundaries.  Phase
    # 19/20 are deliberately reported as pending until the release gates are
    # implemented; their presence still makes the stop point unambiguous.
    'LITEOS_STAGE phase=SPEC_P0_REPOSITORY step=5 value=1'
    'LITEOS_STAGE phase=SPEC_P1_CPU step=5 value=1'
    'LITEOS_STAGE phase=SPEC_P2_MEMORY step=5 value=1'
    'LITEOS_STAGE phase=SPEC_P3_SLAB step=5 value=1'
    'LITEOS_STAGE phase=SPEC_P4_SMP step=5 value=1'
    'LITEOS_STAGE phase=SPEC_P5_SCHEDULER step=5 value=1'
    'LITEOS_STAGE phase=SPEC_P6_VM step=5 value=1'
    'LITEOS_STAGE phase=SPEC_P7_SYSCALL step=5 value=1'
    'LITEOS_STAGE phase=SPEC_P8_OBJECT step=5 value=1'
    'LITEOS_STAGE phase=SPEC_P9_DEVICE step=5 value=1'
    'LITEOS_STAGE phase=SPEC_P10_STORAGE step=5 value=1'
    'LITEOS_STAGE phase=SPEC_P11_USB step=5 value=1'
    'LITEOS_STAGE phase=SPEC_P12_NETWORK step=5 value=1'
    'LITEOS_STAGE phase=SPEC_P13_GPU step=5 value=1'
    'LITEOS_STAGE phase=SPEC_P14_WINDOW step=5 value=1'
    'LITEOS_STAGE phase=SPEC_P15_AUDIO_BT step=5 value=1'
    'LITEOS_STAGE phase=SPEC_P16_SERVICES step=5 value=1'
    'LITEOS_STAGE phase=SPEC_P17_POWER step=5 value=1'
    'LITEOS_STAGE phase=SPEC_P18_HARDENING step=5 value=1'
    'LITEOS_STAGE phase=SPEC_P19_ABI step=15 value=0'
    'LITEOS_STAGE phase=SPEC_P20_OS1 step=15 value=0'

    # Refactoring roadmap phases 0..9.  Phase 0 remains pending until the
    # host-side benchmark baseline and regression gate are recorded; its
    # in-kernel metric boundary still makes the stop point exact.
    'LITEOS_STAGE phase=REFACTOR_P0_BASELINE step=15 value=0'
    'LITEOS_STAGE phase=REFACTOR_P0_BASELINE step=16 value=1'
    'LITEOS_STAGE phase=REFACTOR_P1_HEADERS step=5 value=1'
    'LITEOS_STAGE phase=REFACTOR_P2_PRIMITIVES step=5 value=1'
    'LITEOS_STAGE phase=REFACTOR_P2_PRIMITIVES step=16 value=7'
    'LITEOS_STAGE phase=REFACTOR_P3_BOOT step=15 value=0'
    'LITEOS_STAGE phase=REFACTOR_P3_BOOT step=16 value=1'
    'LITEOS_STAGE phase=REFACTOR_P3_BOOT step=16 value=2'
    'LITEOS_STAGE phase=REFACTOR_P3_BOOT step=16 value=3'
    'LITEOS_STAGE phase=REFACTOR_P3_BOOT step=16 value=4'
    'LITEOS_STAGE phase=REFACTOR_P3_BOOT step=16 value=5'
    'LITEOS_STAGE phase=REFACTOR_P3_BOOT step=16 value=6'
    'LITEOS_STAGE phase=REFACTOR_P3_BOOT step=16 value=7'
    'LITEOS_STAGE phase=REFACTOR_P3_BOOT step=16 value=8'
    'LITEOS_STAGE phase=REFACTOR_P3_BOOT step=16 value=9'
    'LITEOS_STAGE phase=REFACTOR_P3_BOOT step=16 value=10'
    'LITEOS_STAGE phase=REFACTOR_P4_SCHEDULER step=1 value=0'
    'LITEOS_STAGE phase=REFACTOR_P4_SCHEDULER step=16 value=1'
    'LITEOS_STAGE phase=REFACTOR_P4_SCHEDULER step=16 value=2'
    'LITEOS_STAGE phase=REFACTOR_P4_SCHEDULER step=16 value=3'
    'LITEOS_STAGE phase=REFACTOR_P4_SCHEDULER step=16 value=4'
    'LITEOS_STAGE phase=REFACTOR_P4_SCHEDULER step=16 value=5'
    'LITEOS_STAGE phase=REFACTOR_P4_SCHEDULER step=16 value=6'
    'LITEOS_STAGE phase=REFACTOR_P5_PROCESS step=1 value=0'
    'LITEOS_STAGE phase=REFACTOR_P5_PROCESS step=16 value=1'
    'LITEOS_STAGE phase=REFACTOR_P5_PROCESS step=16 value=2'
    'LITEOS_STAGE phase=REFACTOR_P5_PROCESS step=16 value=3'
    'LITEOS_STAGE phase=REFACTOR_P5_PROCESS step=16 value=4'
    'LITEOS_STAGE phase=REFACTOR_P5_PROCESS step=16 value=5'
    'LITEOS_STAGE phase=REFACTOR_P4_SCHEDULER step=15 value=0'
    'LITEOS_STAGE phase=REFACTOR_P5_PROCESS step=15 value=0'
    'LITEOS_STAGE phase=REFACTOR_P6_MM step=1 value=0'
    'LITEOS_STAGE phase=REFACTOR_P6_MM step=16 value=1'
    'LITEOS_STAGE phase=REFACTOR_P6_MM step=16 value=2'
    'LITEOS_STAGE phase=REFACTOR_P6_MM step=16 value=3'
    'LITEOS_STAGE phase=REFACTOR_P6_MM step=16 value=4'
    'LITEOS_STAGE phase=REFACTOR_P6_MM step=16 value=5'
    'LITEOS_STAGE phase=REFACTOR_P6_MM step=16 value=6'
    'LITEOS_STAGE phase=REFACTOR_P6_MM step=16 value=7'
    'LITEOS_STAGE phase=REFACTOR_P6_MM step=15 value=0'
    'LITEOS_STAGE phase=REFACTOR_P7A_GRAPHICS step=1 value=0'
    'LITEOS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=1'
    'LITEOS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=2'
    'LITEOS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=3'
    'LITEOS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=4'
    'LITEOS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=5'
    'LITEOS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=6'
    'LITEOS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=7'
    'LITEOS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=8'
    'LITEOS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=9'
    'LITEOS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=10'
    'LITEOS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=11'
    'LITEOS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=12'
    'LITEOS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=13'
    'LITEOS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=14'
    'LITEOS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=15'
    'LITEOS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=16'
    'LITEOS_STAGE phase=REFACTOR_P7A_GRAPHICS step=15 value=0'
    'LITEOS_STAGE phase=REFACTOR_P7B_COMPOSITOR step=15 value=0'
    'LITEOS_STAGE phase=REFACTOR_P7B_COMPOSITOR step=16 value=1'
    'LITEOS_STAGE phase=REFACTOR_P7B_COMPOSITOR step=16 value=2'
    'LITEOS_STAGE phase=REFACTOR_P7B_COMPOSITOR step=16 value=3'
    'LITEOS_STAGE phase=REFACTOR_P7B_COMPOSITOR step=16 value=4'
    'LITEOS_STAGE phase=REFACTOR_P7B_COMPOSITOR step=16 value=5'
    'LITEOS_STAGE phase=REFACTOR_P7B_COMPOSITOR step=16 value=6'
    'LITEOS_STAGE phase=REFACTOR_P7B_COMPOSITOR step=16 value=7'
    'LITEOS_STAGE phase=REFACTOR_P7B_COMPOSITOR step=16 value=8'
    'LITEOS_STAGE phase=REFACTOR_P7B_COMPOSITOR step=16 value=9'
    'LITEOS_STAGE phase=REFACTOR_P7B_COMPOSITOR step=16 value=10'
    'LITEOS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=1'
    'LITEOS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=2'
    'LITEOS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=3'
    'LITEOS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=4'
    'LITEOS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=5'
    'LITEOS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=6'
    'LITEOS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=7'
    'LITEOS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=8'
    'LITEOS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=9'
    'LITEOS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=10'
    'LITEOS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=11'
    'LITEOS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=12'
    'LITEOS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=13'
    'LITEOS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=14'
    'LITEOS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=15'
    'LITEOS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=16'
    'LITEOS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=17'
    'LITEOS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=18'
    'LITEOS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=19'
    'LITEOS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=20'
    'LITEOS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=21'
    'LITEOS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=22'
    'LITEOS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=23'
    'LITEOS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=24'
    'LITEOS_STAGE phase=REFACTOR_P8_DRIVERS step=15 value=0'
    'LITEOS_STAGE phase=REFACTOR_P9_CLEANUP step=15 value=0'
)

missing=()
for marker in "${required_markers[@]}"; do
    if ! grep -Fq -- "$marker" "$log_path"; then
        missing+=("$marker")
    fi
done
for marker in "${required_stages[@]}"; do
    if ! grep -Fq -- "$marker" "$log_path"; then
        missing+=("$marker")
    fi
done

# Keep the detailed operational checks above, but make the phase list itself
# come from the roadmap manifest.  This prevents a newly added phase from
# being accepted by the static checker while disappearing from stage-sanity.
while IFS='|' read -r family phase serial_name owner_source; do
    [[ -n "$family" && "${family:0:1}" != "#" ]] || continue
    if ! grep -Fq -- "LITEOS_STAGE phase=$serial_name " "$log_path"; then
        missing+=("LITEOS_STAGE phase=$serial_name")
    fi
done < "$contracts_path"

if grep -Fq -- 'LITEOS_STAGE_FAIL' "$log_path"; then
    echo "stage verification: explicit stage failure found in $log_path" >&2
    grep -F -- 'LITEOS_STAGE_FAIL' "$log_path" >&2 || true
    exit 1
fi

# A stage without a source location is not actionable in a debugger.  Keep
# the complete log on disk, but report only the offending records here.
unlocated_stages=$(grep -E '^LITEOS_STAGE(_FAIL)? phase=' "$log_path" |
    grep -Ev ' loc=kernel/[^[:space:]]+:[0-9]+([[:space:]]|$)' || true)
if [[ -n "$unlocated_stages" ]]; then
    echo "stage verification: unlocated stage record found in $log_path" >&2
    printf '%s\n' "$unlocated_stages" >&2
    exit 1
fi

# A syntactically valid location is not enough: a stale path or an out-of-date
# line number makes a debug build impossible to navigate.  Resolve every
# location against the current source tree and require the recorded source
# line to contain the stage publication call (or its small helper wrapper).
# Failure helpers carry the call-site through a local *_fail macro, so accept
# that wrapper spelling as well.
stage_locations=$(grep -E '^LITEOS_STAGE(_FAIL)? phase=' "$log_path" |
    sed -n 's/.* loc=\([^[:space:]]*\).*/\1/p' | sort -u)
location_errors=0
while IFS= read -r location; do
    [[ -n "$location" ]] || continue
    case "$location" in
        kernel/*:[1-9][0-9]*) ;;
        *)
            printf 'stage verification: invalid source location: %s\n' \
                "$location" >&2
            location_errors=$((location_errors + 1))
            continue
            ;;
    esac

    source_path="${location%:*}"
    source_line="${location##*:}"
    if [[ ! -f "$source_path" ]]; then
        printf 'stage verification: source file not found for %s\n' \
            "$location" >&2
        location_errors=$((location_errors + 1))
        continue
    fi

    source_line_count=$(wc -l < "$source_path")
    if ((source_line > source_line_count)); then
        printf 'stage verification: source line out of range: %s\n' \
            "$location" >&2
        location_errors=$((location_errors + 1))
        continue
    fi

    source_text=$(sed -n "${source_line}p" "$source_path" || true)
    if [[ "$source_text" != *liteos_debug_stage* &&
          "$source_text" != *liteos_debug_trace_stage* &&
          ! "$source_text" =~ [[:alnum:]_]+_fail[[:space:]]*\( ]]; then
        printf 'stage verification: source line is not a stage call: %s\n' \
            "$location" >&2
        location_errors=$((location_errors + 1))
    elif [[ "$source_text" != *LITEOS_DEBUG_PHASE_* &&
            ! "$source_text" =~ [[:alnum:]_]+_fail[[:space:]]*\( ]]; then
        printf 'stage verification: source line is a shared stage helper, not a call site: %s\n' \
            "$location" >&2
        location_errors=$((location_errors + 1))
    fi
done <<< "$stage_locations"

if ((location_errors != 0)); then
    exit 1
fi

if ((${#missing[@]} != 0)); then
    echo "stage verification: missing markers in $log_path" >&2
    printf '  %s\n' "${missing[@]}" >&2
    diagnostics=$(grep -Ein \
        'fail|error|fatal|panic|abort|invalid|cannot|undefined|assert|timeout|fault|halt' \
        "$log_path" | tail -n 24 || true)
    if [[ -n "$diagnostics" ]]; then
        printf '%s\n' "$diagnostics" >&2
    else
        echo "no explicit failure line; full log: $log_path" >&2
    fi
    exit 1
fi

sequence_error=''
if ! sequence_error=$(awk '
/LITEOS_STAGE phase=/ {
    for (i = 1; i <= NF; ++i) {
        current = $i
        sub(/^seq=/, "", current)
        sub(/\r$/, "", current)
        if (current !~ /^[0-9]+$/) continue
        if (count != 0 && (current + 0) <= (previous + 0)) {
            printf "sequence is not strictly increasing: %s after %s\n", current, previous
            bad = 1
        }
        previous = current
        ++count
    }
}
END {
    if (count == 0) {
        print "no sequenced LITEOS_STAGE records found"
        bad = 1
    }
    exit bad ? 1 : 0
}
' "$log_path"); then
    echo "stage verification: invalid stage sequence in $log_path" >&2
    printf '%s\n' "$sequence_error" >&2
    exit 1
fi

stage_count=$(grep -Fc -- 'LITEOS_STAGE phase=' "$log_path" || true)
if [[ "${STAGE_VERIFY_VERBOSE:-0}" == "1" ]]; then
    echo "QEMU stage verification: OK ($log_path, $stage_count stage records)"
fi
