#!/usr/bin/env bash
# Run LiteOS with the Linux QEMU/OVMF stack inside WSL.
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$script_dir"

make_cmd="${MAKE:-make}"
qemu_path="${QEMU_PATH:-$(command -v qemu-system-x86_64 || true)}"
firmware="${OVMF_CODE:-}"
build_dir="${BUILD:-build}"
debug=0
no_build=0
headless=0
usb_nested=0
usb_storage=0
monitor_socket=""
keep_open=0
no_kvm=0
net_dump=""
seconds=10
cpu_count=4
gdb_port=1234
debug_level=2

usage() {
    cat <<'EOF'
用法：./run-qemu.sh [选项]

选项：
  --debug             启动 GDB stub，监听 127.0.0.1:1234
  --gdb-port N        修改 GDB 端口
  --headless          不显示图形窗口
  --usb-nested        测试 xHCI -> Hub -> Hub -> Mouse
  --usb-storage       测试 xHCI USB Mass Storage U盘
  --monitor-socket P  在 Unix socket P 开启 HMP monitor
  --keep-open         普通运行模式下持续运行，Ctrl-C 退出
  --seconds N         普通运行模式最多运行 N 秒（默认 10）
  --no-build          使用现有 build/esp，不重新编译
  --debug-level N     编译 DEBUG 等级（默认 2）
  --cpu N             vCPU 数量（默认 4）
  --no-kvm            强制使用软件模拟
  --net-dump PATH     将 QEMU 虚拟网卡报文保存到 PATH（诊断用）
  --qemu PATH         指定 Linux QEMU 路径
  --firmware PATH     指定 OVMF_CODE 固件路径
  -h, --help          显示帮助
EOF
}

while (($# > 0)); do
    case "$1" in
        --debug) debug=1; shift ;;
        --gdb-port)
            (($# >= 2)) || { echo "--gdb-port 需要参数" >&2; exit 2; }
            gdb_port="$2"; shift 2 ;;
        --headless) headless=1; shift ;;
        --usb-nested) usb_nested=1; shift ;;
        --usb-storage) usb_storage=1; shift ;;
        --monitor-socket)
            (($# >= 2)) || {
                echo "--monitor-socket 需要参数" >&2
                exit 2
            }
            monitor_socket="$2"
            shift 2
            ;;

        --keep-open) keep_open=1; shift ;;
        --seconds)
            (($# >= 2)) || { echo "--seconds 需要参数" >&2; exit 2; }
            seconds="$2"; shift 2 ;;
        --no-build) no_build=1; shift ;;
        --debug-level)
            (($# >= 2)) || { echo "--debug-level 需要参数" >&2; exit 2; }
            debug_level="$2"; shift 2 ;;
        --cpu)
            (($# >= 2)) || { echo "--cpu 需要参数" >&2; exit 2; }
            cpu_count="$2"; shift 2 ;;
        --no-kvm) no_kvm=1; shift ;;
        --net-dump)
            (($# >= 2)) || { echo "--net-dump 需要参数" >&2; exit 2; }
            net_dump="$2"; shift 2 ;;
        --qemu)
            (($# >= 2)) || { echo "--qemu 需要参数" >&2; exit 2; }
            qemu_path="$2"; shift 2 ;;
        --firmware)
            (($# >= 2)) || { echo "--firmware 需要参数" >&2; exit 2; }
            firmware="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "未知参数：$1" >&2; usage >&2; exit 2 ;;
    esac
done

# B10B-3 HMP HOTPLUG MONITOR
if [[ -n "$monitor_socket" &&
      "$monitor_socket" != /* ]]; then
    monitor_socket="$script_dir/$monitor_socket"
fi

if ((usb_nested && headless)); then
    echo "--usb-nested 需要图形窗口以产生鼠标输入" >&2
    exit 2
fi
if ((usb_nested && usb_storage)); then
    echo "--usb-nested 与 --usb-storage 请分开测试" >&2
    exit 2
fi

[[ -n "$qemu_path" && -x "$qemu_path" ]] || {
    echo "找不到 Linux QEMU，请安装 qemu-system-x86 或使用 --qemu PATH" >&2
    exit 1
}

if [[ -z "$firmware" ]]; then
    for candidate in /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/edk2/ovmf/OVMF_CODE.fd /usr/share/qemu/edk2-x86_64-code.fd; do
        if [[ -f "$candidate" ]]; then firmware="$candidate"; break; fi
    done
fi
[[ -f "$firmware" ]] || {
    echo "找不到 OVMF_CODE 固件，请安装 ovmf 或使用 --firmware PATH" >&2
    exit 1
}

if ((no_build == 0)); then
    "$make_cmd" -f GNUmakefile esp DEBUG="$debug_level"
fi
[[ -f "$build_dir/esp/EFI/LITEOS/kernel.elf" ]] || {
    echo "找不到 $build_dir/esp/EFI/LITEOS/kernel.elf，请先构建镜像" >&2
    exit 1
}

# The kernel VFS self-tests create temporary entries at the ESP root.  QEMU
# 10's vvfat backend can leave those entries visible on the host after the
# guest removes them; on the next boot they make the mkdir/create checks fail
# (or trigger a vvfat assertion).  Quarantine every known self-test artifact
# before constructing the synthetic FAT image.  Moving keeps the evidence
# recoverable while making the boot directory deterministic.
stale_dir="$build_dir/qemu-stale"
mkdir -p "$stale_dir"
for stale_entry in \
    "$build_dir/esp/vfs-api" \
    "$build_dir/esp/vfs-filemap-test"* \
    "$build_dir/esp/etc/vfs-filemap-test"*; do
    [[ -e "$stale_entry" ]] || continue
    stale_name="$(basename -- "$stale_entry")"
    stale_target="$stale_dir/"$stale_name".$$"
    mv -- "$stale_entry" "$stale_target"
    echo "Quarantined stale VFS artifact: $stale_target"
done

mkdir -p "$build_dir"
qemu_args=(
    -machine q35
    -m 4096M
    -cpu qemu64
    -smp "$cpu_count"
    -drive "if=pflash,format=raw,readonly=on,file=$firmware"
    -drive "if=none,id=liteos-nvme0,format=raw,file=fat:rw:$build_dir/esp"
    -device "nvme,drive=liteos-nvme0,serial=liteos-system,bootindex=1"
    -boot order=c
    -serial "file:$build_dir/qemu-serial.log"
    -no-reboot
    -no-shutdown
    -d guest_errors
    -D "$build_dir/qemu.log"
)

if [[ -n "$monitor_socket" ]]; then
    mkdir -p "$(dirname -- "$monitor_socket")"
    rm -f "$monitor_socket"

    qemu_args+=(
        -monitor
        "unix:$monitor_socket,server=on,wait=off"
    )
else
    qemu_args+=(
        -monitor
        none
    )
fi


if ((no_kvm == 0)) && [[ -r /dev/kvm && -w /dev/kvm ]]; then
    qemu_args+=( -accel kvm )
    echo 'QEMU accelerator: KVM'
else
    qemu_args+=( -accel tcg,thread=multi )
    echo 'QEMU accelerator: TCG (no usable /dev/kvm)'
fi

if ((headless)); then
    qemu_args+=( -display none )
else
    qemu_args+=( -display gtk,zoom-to-fit=off,grab-on-hover=on )

    if ((usb_nested)); then
        #
        # B10B-2 NESTED USB HUB TOPOLOGY
        #
        # Physical topology:
        #
        #   xHCI root port 1
        #        |
        #        +-- Hub #1
        #              |
        #              +-- port 1: Hub #2
        #                          |
        #                          +-- port 1: Mouse
        #
        # xHCI route strings expected by LiteOS:
        #
        #   Hub #1 : 0x00000
        #   Hub #2 : 0x00001
        #   Mouse  : 0x00011
        #
        qemu_args+=(
            -device "qemu-xhci,id=liteos-xhci,msix=on"
            -device "usb-hub,id=liteos-hub1,bus=liteos-xhci.0,port=1"
            -device "usb-hub,id=liteos-hub2,bus=liteos-xhci.0,port=1.1"
            -device "usb-mouse,id=liteos-nested-mouse,bus=liteos-xhci.0,port=1.1.1"
        )
    elif ((usb_storage == 0)); then
        qemu_args+=(
            -device "qemu-xhci,id=liteos-xhci,msix=on"
            -device "usb-kbd,bus=liteos-xhci.0"
            -device "usb-mouse,bus=liteos-xhci.0"
        )
    fi
fi

if ((usb_storage)); then
    usb_storage_image="$build_dir/usb-storage.img"
    if [[ ! -f "$usb_storage_image" ]]; then
        truncate -s 64M "$usb_storage_image"
        if command -v mkfs.fat >/dev/null 2>&1; then
            mkfs.fat -F 32 -n LITEOSUSB "$usb_storage_image" >/dev/null
        fi
    fi
    qemu_args+=(
        -drive "if=none,id=liteos-usb-stick,format=raw,file=$usb_storage_image"
        -device "qemu-xhci,id=liteos-xhci,msix=on"
        -device "usb-storage,id=liteos-usb-storage,drive=liteos-usb-stick,bus=liteos-xhci.0,port=1"
    )
fi
if ((debug)); then qemu_args+=( -gdb "tcp::${gdb_port}" ); fi
if [[ -n "$net_dump" ]]; then
    # Give the diagnostic filter a named user-net backend.  `-net dump` was
    # removed from newer QEMU builds and cannot attach to the implicit NIC.
    qemu_args+=(
        -net none
        -netdev "user,id=liteos-net"
        -device "e1000e,netdev=liteos-net,mac=52:54:00:12:34:56"
        -object "filter-dump,id=liteos-net-dump,netdev=liteos-net,file=$net_dump"
    )
fi

stdout_log="$build_dir/qemu-stdout.log"
stderr_log="$build_dir/qemu-stderr.log"
if ((debug)); then
    pid_file="$build_dir/qemu-debug.pid"
    if [[ -f "$pid_file" ]]; then
        old_pid="$(cat "$pid_file" 2>/dev/null || true)"
        if [[ "$old_pid" =~ ^[0-9]+$ ]] && kill -0 "$old_pid" 2>/dev/null; then
            old_command="$(ps -p "$old_pid" -o comm= 2>/dev/null | tr -d '[:space:]')"
            case "$old_command" in
                qemu-system-*)
                    kill "$old_pid" 2>/dev/null || true
                    for _ in $(seq 1 20); do
                        kill -0 "$old_pid" 2>/dev/null || break
                        sleep 0.1
                    done
                    kill -9 "$old_pid" 2>/dev/null || true
                    ;;
                *)
                    echo "build/qemu-debug.pid 指向非 QEMU 进程 $old_pid ($old_command)，请手动处理" >&2
                    exit 1
                    ;;
            esac
        fi
        rm -f "$pid_file"
    fi
    echo 'QEMU_DEBUG_START'
    "$qemu_path" "${qemu_args[@]}" >"$stdout_log" 2>"$stderr_log" &
    qemu_pid=$!
    echo "$qemu_pid" >"$pid_file"
    cleanup_debug() {
        if kill -0 "$qemu_pid" 2>/dev/null; then kill "$qemu_pid" 2>/dev/null || true; wait "$qemu_pid" 2>/dev/null || true; fi
        rm -f "$pid_file"
    }
    trap cleanup_debug EXIT INT TERM
    ready=0
    for _ in $(seq 1 150); do
        if ! kill -0 "$qemu_pid" 2>/dev/null; then
            echo "QEMU 在 GDB 端口就绪前退出，请查看 $stderr_log" >&2
            exit 1
        fi
        if (exec 3<>"/dev/tcp/127.0.0.1/${gdb_port}") 2>/dev/null; then
            exec 3>&-
            ready=1
            break
        fi
        sleep 0.1
    done
    ((ready)) || { echo "QEMU GDB 端口 ${gdb_port} 未就绪" >&2; exit 1; }
    echo 'QEMU_DEBUG_READY'
    wait "$qemu_pid"
else
    run_status=0
    if ((keep_open)); then
        "$qemu_path" "${qemu_args[@]}" >"$stdout_log" 2>"$stderr_log" || run_status=$?
    else
        timeout --foreground --signal=TERM "${seconds}s" "$qemu_path" "${qemu_args[@]}" >"$stdout_log" 2>"$stderr_log" || run_status=$?
        if ((run_status == 124)); then run_status=0; fi
    fi
    if ((run_status != 0)); then
        echo "QEMU 退出码：$run_status，请查看 $stderr_log" >&2
        exit "$run_status"
    fi
    [[ -f "$build_dir/qemu-serial.log" ]] && cat "$build_dir/qemu-serial.log"
fi
