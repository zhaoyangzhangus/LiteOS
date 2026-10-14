#!/usr/bin/env bash
# Only features with no extended register state beyond FXSAVE64 are allowed.
# KVM exposes the intersection across all host CPUs; TCG retains the baseline.
qemu_select_cpu_model() {
    local cpu_info="$1" accelerated="$2" chrome="$3"
    local model='qemu64,rdrand=on,xsave=off,avx=off,avx2=off'
    if [[ "$accelerated" == 1 && "$chrome" == 1 && -r "$cpu_info" ]]; then
        local host_flag qemu_flag
        for host_flag in ssse3 sse4_1 sse4_2 popcnt; do
            if awk -v wanted="$host_flag" '
                /^flags[[:space:]]*:/ {
                    cpus++; found=0
                    for (i=3; i<=NF; i++) if ($i==wanted) found=1
                    if (!found) missing=1
                }
                END { exit !(cpus>0 && !missing) }
            ' "$cpu_info"; then
                qemu_flag="${host_flag//_/.}"
                model+=",${qemu_flag}=on"
            fi
        done
    fi
    printf '%s\n' "$model"
}
