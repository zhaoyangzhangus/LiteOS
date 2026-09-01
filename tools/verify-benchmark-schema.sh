#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
root_dir="$(CDPATH= cd -- "$script_dir/.." && pwd)"
cd "$root_dir"

metrics_path="$script_dir/roadmap-benchmark-metrics.tsv"
if (($# == 1)); then
    storage=auto
    log_path="$1"
elif (($# == 2)); then
    storage="$1"
    log_path="$2"
else
    printf 'usage: %s [usb|nvme|auto] qemu-serial.log\n' "$0" >&2
    exit 2
fi

if [[ "$storage" != usb && "$storage" != nvme && "$storage" != auto ]]; then
    printf 'benchmark schema verification: invalid storage profile: %s\n' \
        "$storage" >&2
    exit 2
fi
if [[ ! -f "$metrics_path" || ! -f "$log_path" ]]; then
    printf 'benchmark schema verification: missing manifest or log: %s, %s\n' \
        "$metrics_path" "$log_path" >&2
    exit 2
fi

if [[ "$storage" == auto ]]; then
    if grep -Fq -- 'LITEOS_ROOT_SOURCE=NVME' "$log_path" ||
       grep -Fq -- 'LITEOS_NVME_HW_OK' "$log_path"; then
        storage=nvme
    else
        storage=usb
    fi
fi

observed_metrics=$(awk '
function field(key, i, value) {
    for (i = 1; i <= NF; ++i) {
        if (substr($i, 1, length(key) + 1) == key "=") {
            value = substr($i, length(key) + 2)
            sub(/\r$/, "", value)
            return value
        }
    }
    return ""
}
($1 == "LITEOS_BENCH" || $1 == "LITEOS_BENCH_VALUE") {
    name = field("name")
    if (name != "") print name
}' "$log_path" | sort -u)

missing=()
required_count=0
while IFS='|' read -r metric profile; do
    [[ -n "$metric" && "${metric:0:1}" != "#" ]] || continue
    if [[ "$profile" == nvme && "$storage" != nvme ]]; then
        continue
    fi
    required_count=$((required_count + 1))
    if ! grep -Fxq -- "$metric" <<< "$observed_metrics"; then
        missing+=("$metric")
    fi
done < "$metrics_path"

if ((${#missing[@]} != 0)); then
    printf 'benchmark schema verification: missing %d/%d required metrics (%s profile)\n' \
        "${#missing[@]}" "$required_count" "$storage" >&2
    printf '  %s\n' "${missing[@]}" >&2
    exit 1
fi

printf 'benchmark schema verification: OK (%d required metrics, %s profile)\n' \
    "$required_count" "$storage"
