#!/usr/bin/env bash
# WSL entry point for the LiteOS cross build. The target artifacts are still
# PE/COFF because the UEFI loader and kernel link flow require MinGW, while
# host-side tools/tests are built with HOSTCC (normally the WSL gcc).
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
make_cmd="${MAKE:-make}"
cd "$script_dir"

build_dir="${BUILD:-build}"
force_rebuild=0
if [[ -d "$build_dir" ]]; then
    while IFS= read -r dep_file; do
        # A dependency file produced by a previous Windows build can contain
        # drive-letter paths.  It is not portable to WSL and must be rebuilt.
        # The tree migration also retired the old source roots; if a stale .d
        # file still names one of them, force the same clean rebuild.
        if grep -Eq '[[:space:]][A-Za-z]:/' "$dep_file" ||
           grep -Eq '(^|[[:space:]])(src|kernel|include|boot|user|tests|tools)/' "$dep_file"; then
            force_rebuild=1
            break
        fi
    done < <(find "$build_dir" -type f -name '*.d' -print)
    host_binaries=(
        "$build_dir/build-id.exe"
        "$build_dir/make-init-image.exe"
        "$build_dir/make-test-elf.exe"
    )
    check_all_host_binaries=0
    for build_target in "$@"; do
        case "$build_target" in
            test|header-sanity|abi-sanity|*-test)
                check_all_host_binaries=1
                break
                ;;
        esac
    done
    if ((check_all_host_binaries)); then
        host_binaries=("$build_dir"/*.exe)
    fi
    for host_binary in "${host_binaries[@]}"; do
        [[ -f "$host_binary" ]] || continue
        if [[ "$(head -c 2 "$host_binary" 2>/dev/null)" == 'MZ' ]]; then
            force_rebuild=1
            break
        fi
    done
fi

if ((force_rebuild)); then
    if [[ "${LITEOS_VERBOSE:-0}" == "1" ]]; then
        echo '检测到 Windows 生成的构建文件，清理生成的依赖并重新生成 WSL 产物'
    fi
    find "$build_dir" -type f -name '*.d' -delete
    set -- -B "$@"
fi

exec "$make_cmd" -f GNUmakefile "$@"
