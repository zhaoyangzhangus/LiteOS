#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(CDPATH= cd -- "$script_dir/.." && pwd)"
cd "$repo_root"

build_dir="${BUILD:-build-refactor}"
runs="${BENCHMARK_RUNS:-5}"
cpu_count="${BENCHMARK_CPU:-4}"
seconds="${BENCHMARK_SECONDS:-90}"
storage_mode="${BENCHMARK_STORAGE:-usb}"
accel_mode="${BENCHMARK_ACCEL:-auto}"
baseline_path="${BENCHMARK_BASELINE:-}"
compare_path=""
gate_percent="${BENCHMARK_GATE_PERCENT:-1}"
no_build=0

fail() {
    printf 'refactor benchmark failed: %s\n' "$*" >&2
    exit 1
}

show_failures() {
    local path
    for path in "$@"; do
        [[ -f "$path" ]] || continue
        grep -Ein 'fail|error|fatal|panic|abort|invalid|cannot|undefined|assert|timeout' \
            "$path" >&2 || true
    done
}

usage() {
    cat >&2 <<'EOF'
用法：./tools/refactor-benchmark.sh [--runs N] [--cpu N]
     [--seconds N] [--storage usb|nvme] [--build PATH]
     [--baseline PATH] [--compare PATH] [--accel auto|kvm|tcg]
     [--gate PERCENT] [--no-build]
EOF
}

while (($# > 0)); do
    case "$1" in
        --runs|--cpu|--seconds|--storage|--build|--baseline|--compare|--accel|--gate)
            (($# >= 2)) || fail "$1 需要参数"
            case "$1" in
                --runs) runs="$2" ;;
                --cpu) cpu_count="$2" ;;
                --seconds) seconds="$2" ;;
                --storage) storage_mode="$2" ;;
                --build) build_dir="$2" ;;
                --baseline) baseline_path="$2" ;;
                --compare) compare_path="$2" ;;
                --accel) accel_mode="$2" ;;
                --gate) gate_percent="$2" ;;
            esac
            shift 2
            ;;
        --no-build)
            no_build=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage
            fail "未知参数：$1"
            ;;
    esac
done

[[ "$runs" =~ ^[1-9][0-9]*$ ]] || fail "runs 无效：$runs"
[[ "$cpu_count" =~ ^[1-9][0-9]*$ ]] || fail "cpu 无效：$cpu_count"
[[ "$seconds" =~ ^[1-9][0-9]*$ ]] || fail "seconds 无效：$seconds"
[[ "$storage_mode" == usb || "$storage_mode" == nvme ]] ||
    fail "storage 无效：$storage_mode（只能是 usb 或 nvme）"
[[ "$accel_mode" == auto || "$accel_mode" == kvm || "$accel_mode" == tcg ]] ||
    fail "accel 无效：$accel_mode（只能是 auto、kvm 或 tcg）"
[[ "$gate_percent" =~ ^[0-9]+([.][0-9]+)?$ ]] || fail "gate 无效：$gate_percent"
[[ -n "$baseline_path" ]] || baseline_path="$build_dir/refactor-baseline.tsv"
[[ -z "$compare_path" || "$baseline_path" != "$compare_path" ]] ||
    fail '--baseline 不能覆盖 --compare'

mkdir -p "$build_dir"
benchmark_dir="$build_dir/refactor-benchmark"
mkdir -p "$benchmark_dir"
raw_path="$benchmark_dir/raw.tsv"
metrics_path="$benchmark_dir/metrics.txt"
rm -f -- "$raw_path" "$metrics_path"

qemu_storage_args=()
if [[ "$storage_mode" == usb ]]; then
    qemu_storage_args+=(--usb-storage)
else
    qemu_storage_args+=(--nvme-root)
fi
benchmark_profile=""
qemu_accel_args=()
benchmark_accel=""
case "$accel_mode" in
    tcg)
        qemu_accel_args+=(--no-kvm)
        benchmark_accel='tcg'
        ;;
    kvm)
        [[ -r /dev/kvm && -w /dev/kvm ]] ||
            fail '请求 KVM，但 /dev/kvm 不可用'
        benchmark_accel='kvm'
        ;;
    auto)
        if [[ -r /dev/kvm && -w /dev/kvm ]]; then
            benchmark_accel='kvm'
        else
            qemu_accel_args+=(--no-kvm)
            benchmark_accel='tcg'
        fi
        ;;
esac

if ((no_build == 0)); then
    if ! BUILD="$build_dir" make -f GNUmakefile debug-image \
            >"$build_dir/qemu-build.log" \
            2>"$build_dir/qemu-build-stderr.log"; then
        show_failures "$build_dir/qemu-build.log" "$build_dir/qemu-build-stderr.log"
        fail "构建失败，完整日志：$build_dir/qemu-build.log"
    fi
fi
[[ -f "$build_dir/esp/EFI/LITEOS/kernel.elf" ]] ||
    fail "找不到 $build_dir/esp/EFI/LITEOS/kernel.elf"

kernel_elf_path="$build_dir/esp/EFI/LITEOS/kernel.elf"

parse_log() {
    awk '
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
    {
        sub(/\r$/, "", $0)
        if ($1 == "LITEOS_BENCH" || $1 == "LITEOS_BENCH_VALUE") {
            name = field("name"); cycles = field("cycles")
            if (cycles == "") cycles = field("value")
            if (name != "" && cycles ~ /^[0-9]+$/) print name "\t" cycles
        } else if ($1 == "LITEOS_PERF_KMALLOC_OK") {
            for (i = 2; i <= NF; ++i) {
                split($i, pair, "=")
                if (pair[2] !~ /^[0-9]+$/) continue
                if (pair[1] == "MIN") print "kmalloc.min_tsc\t" pair[2]
                if (pair[1] == "MEDIAN") print "kmalloc.median_tsc\t" pair[2]
                if (pair[1] == "P95") print "kmalloc.p95_tsc\t" pair[2]
                if (pair[1] == "AVG") print "kmalloc.average_tsc\t" pair[2]
                if (pair[1] == "MAX") print "kmalloc.max_tsc\t" pair[2]
            }
        }
    }' "$1"
}

for ((run = 1; run <= runs; ++run)); do
    stdout_path="$benchmark_dir/qemu-$run.stdout.log"
    stderr_path="$benchmark_dir/qemu-$run.stderr.log"
    serial_path="$benchmark_dir/qemu-serial-$run.log"
    if ! BUILD="$build_dir" ./run-qemu.sh --headless --cpu "$cpu_count" \
            --seconds "$seconds" --no-build \
            "${qemu_accel_args[@]}" \
            "${qemu_storage_args[@]}" \
            >"$stdout_path" 2>"$stderr_path"; then
        show_failures "$stdout_path" "$stderr_path" "$build_dir/qemu-stderr.log"
        fail "QEMU 第 $run 轮失败，完整日志：$build_dir/qemu-serial.log"
    fi
    cp -- "$build_dir/qemu-serial.log" "$serial_path"
    if ! ./tools/verify-debug-locations.sh "$kernel_elf_path" "$serial_path" \
            >"$benchmark_dir/verify-$run.log" 2>&1; then
        cat "$benchmark_dir/verify-$run.log" >&2
        fail "QEMU 第 $run 轮阶段校验失败"
    fi
    if ! ./tools/verify-benchmark-schema.sh "$storage_mode" "$serial_path" \
            >"$benchmark_dir/schema-$run.log" 2>&1; then
        cat "$benchmark_dir/schema-$run.log" >&2
        fail "QEMU benchmark schema verification failed for round $run"
    fi
    for required_marker in \
        'LITEOS_USER_RUNTIME_OK' \
        'LITEOS_USER_RUNTIME_SUBTESTS_OK' \
        'LITEOS_USERMODE_OK' \
        'LITEOS_STAGE phase=USER_RUNTIME step=5 value=1'; do
        grep -Fq "$required_marker" "$serial_path" || {
            show_failures "$serial_path"
            fail "QEMU round $run missing runtime success marker: $required_marker"
        }
    done
    if grep -Eq 'LITEOS_USER_RUNTIME_(FAIL_STAGE|EXEC_WARN)|LITEOS_STAGE_FAIL' \
            "$serial_path"; then
        show_failures "$serial_path"
        fail "QEMU round $run contains runtime failure marker"
    fi
    grep -Fq 'LITEOS_STAGE phase=REFACTOR_P0_BASELINE step=16 value=1' \
        "$serial_path" || fail "QEMU 第 $run 轮缺少 P0 基准记录"
    run_profile=""
    if grep -Fq 'LITEOS_USB_MSC_FOUND' "$serial_path"; then
        run_profile='usb-msc'
    elif grep -Fq 'LITEOS_NVME_HW_OK' "$serial_path"; then
        run_profile='nvme-hardware'
    else
        fail "QEMU 第 $run 轮缺少可识别的存储工作负载标记"
    fi
    if [[ -z "$benchmark_profile" ]]; then
        benchmark_profile="$run_profile"
    elif [[ "$benchmark_profile" != "$run_profile" ]]; then
        fail "QEMU 各轮存储工作负载不一致：$benchmark_profile/$run_profile"
    fi
    record_count=0
    while IFS=$'\t' read -r metric value; do
        [[ -n "$metric" && "$value" =~ ^[0-9]+$ ]] || continue
        printf '%s\t%s\t%s\n' "$metric" "$run" "$value" >>"$raw_path"
        ((record_count += 1))
    done < <(parse_log "$serial_path")
    ((record_count > 0)) || fail "QEMU 第 $run 轮没有基准记录"
done

cut -f1 "$raw_path" | sort -u >"$metrics_path"
[[ -s "$metrics_path" ]] || fail '没有可统计的基准指标'
while IFS= read -r metric; do
    count=$(awk -F '\t' -v metric="$metric" '$1 == metric {++count} END {print count + 0}' "$raw_path")
    ((count == runs)) || fail "指标 $metric 只有 $count/$runs 轮记录"
done <"$metrics_path"

mkdir -p "$(dirname -- "$baseline_path")"
baseline_tmp="$baseline_path.tmp.$$"
trap 'rm -f -- "$baseline_tmp"' EXIT
{
    printf '# LiteOS refactor benchmark baseline v1\n'
    printf '# runs=%s cpu=%s seconds=%s\n' "$runs" "$cpu_count" "$seconds"
    printf '# storage=%s profile=%s accel=%s\n' \
        "$storage_mode" "$benchmark_profile" "$benchmark_accel"
    printf 'metric\tcount\tmin\tmedian\tp95\tmax\n'
    while IFS= read -r metric; do
        stats=$(awk -F '\t' -v metric="$metric" '
            $1 == metric { values[++count] = $3 }
            END {
                for (i = 1; i <= count; ++i)
                    for (j = i + 1; j <= count; ++j)
                        if (values[j] < values[i]) {
                            swap = values[i]; values[i] = values[j]; values[j] = swap
                        }
                if (count == 0) exit 1
                p95 = int((count * 95 + 99) / 100)
                if (count % 2) median = values[(count + 1) / 2]
                else median = int((values[count / 2] + values[count / 2 + 1]) / 2)
                printf "%d\t%d\t%d\t%d\t%d", count, values[1], median,
                       values[p95], values[count]
            }' "$raw_path") || fail "无法统计指标 $metric"
        printf '%s\t%s\n' "$metric" "$stats"
    done <"$metrics_path"
} >"$baseline_tmp"
mv -- "$baseline_tmp" "$baseline_path"
trap - EXIT

if [[ -n "$compare_path" ]]; then
    [[ -f "$compare_path" ]] || fail "找不到比较基线：$compare_path"

    metadata_value() {
        local key="$1"
        local path="$2"
        awk -v wanted="$key" '
            $1 == "#" {
                for (i = 2; i <= NF; ++i) {
                    split($i, pair, "=")
                    if (pair[1] == wanted) {
                        print substr($i, length(wanted) + 2)
                        exit
                    }
                }
            }' "$path"
    }

    for metadata_key in runs cpu seconds storage profile accel; do
        current_metadata=$(metadata_value "$metadata_key" "$baseline_path")
        baseline_metadata=$(metadata_value "$metadata_key" "$compare_path")
        [[ -n "$current_metadata" ]] ||
            fail "当前基线缺少元数据：$metadata_key"
        [[ -n "$baseline_metadata" ]] ||
            fail "比较基线缺少元数据：$metadata_key；请用同一启动介质重新生成，禁止跨 USB/NVMe 比较"
        [[ "$current_metadata" == "$baseline_metadata" ]] ||
            fail "基线元数据不匹配：$metadata_key 当前=$current_metadata 比较=$baseline_metadata"
    done

    regression=0
    while IFS=$'\t' read -r metric count minimum median p95 maximum; do
        [[ -z "$metric" || "$metric" == \#* || "$metric" == metric ]] && continue
        baseline_values=$(awk -F '\t' -v metric="$metric" '$1 == metric {print $4 "\t" $5; exit}' "$compare_path")
        if [[ -z "$baseline_values" ]]; then
            printf 'refactor benchmark failed: 基线缺少指标 %s\n' "$metric" >&2
            regression=1
            continue
        fi
        IFS=$'\t' read -r baseline_median baseline_p95 <<<"$baseline_values"
        if awk -v current="$median" -v baseline="$baseline_median" -v gate="$gate_percent" \
                'BEGIN { exit !(baseline > 0 && current > baseline * (1 + gate / 100)) }'; then
            printf 'refactor benchmark failed: %s median %s > %s (+%s%%)\n' \
                "$metric" "$median" "$baseline_median" "$gate_percent" >&2
            regression=1
        fi
        if awk -v current="$p95" -v baseline="$baseline_p95" -v gate="$gate_percent" \
                'BEGIN { exit !(baseline > 0 && current > baseline * (1 + gate / 100)) }'; then
            printf 'refactor benchmark failed: %s p95 %s > %s (+%s%%)\n' \
                "$metric" "$p95" "$baseline_p95" "$gate_percent" >&2
            regression=1
        fi
    done <"$baseline_path"
    ((regression == 0)) || fail "性能回归超过 ${gate_percent}%"
fi
