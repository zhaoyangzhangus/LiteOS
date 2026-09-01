#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(CDPATH= cd -- "$script_dir/.." && pwd)"
cd "$repo_dir"

build_dir="${BUILD:-build}"
seconds="${QEMU_MATRIX_SECONDS:-60}"
cpu_text="${QEMU_MATRIX_CPUS:-1 2 4 8}"
accel_mode="${QEMU_MATRIX_ACCEL:-auto}"
read -r -a cpus <<< "$cpu_text"

if ! [[ "$seconds" =~ ^[0-9]+$ ]] || ((seconds < 1)); then
    echo "QEMU matrix: QEMU_MATRIX_SECONDS must be a positive integer" >&2
    exit 2
fi
if ((${#cpus[@]} == 0)); then
    echo "QEMU matrix: QEMU_MATRIX_CPUS must not be empty" >&2
    exit 2
fi
if [[ "$accel_mode" != auto && "$accel_mode" != kvm && "$accel_mode" != tcg ]]; then
    echo "QEMU matrix: QEMU_MATRIX_ACCEL must be auto, kvm, or tcg" >&2
    exit 2
fi

qemu_accel_args=()
case "$accel_mode" in
    tcg)
        qemu_accel_args+=(--no-kvm)
        actual_accel='tcg'
        ;;
    kvm)
        if [[ ! -r /dev/kvm || ! -w /dev/kvm ]]; then
            echo "QEMU matrix: KVM requested but /dev/kvm is unavailable" >&2
            exit 2
        fi
        actual_accel='kvm'
        ;;
    auto)
        if [[ -r /dev/kvm && -w /dev/kvm ]]; then
            actual_accel='kvm'
        else
            qemu_accel_args+=(--no-kvm)
            actual_accel='tcg'
        fi
        ;;
esac

for cpu in "${cpus[@]}"; do
    if ! [[ "$cpu" =~ ^[0-9]+$ ]] || ((cpu < 1)); then
        echo "QEMU matrix: invalid CPU count: $cpu" >&2
        exit 2
    fi

    if [[ "${LITEOS_VERBOSE:-0}" == "1" ]]; then
        echo "QEMU matrix: running ${cpu} CPU(s), accel=${actual_accel}"
    fi
    BUILD="$build_dir" ./run-qemu.sh \
        --headless \
        --cpu "$cpu" \
        --seconds "$seconds" \
        "${qemu_accel_args[@]}" \
        --no-build \
        --usb-storage

    cp -- "$build_dir/qemu-serial.log" \
        "$build_dir/qemu-serial-${cpu}.log"
    ./tools/verify-qemu-stages.sh \
        "$build_dir/qemu-serial-${cpu}.log"
done

if [[ "${LITEOS_VERBOSE:-0}" == "1" ]]; then
    echo "QEMU matrix: all CPU counts passed"
fi
