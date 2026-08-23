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
        if grep -Eq '[[:space:]][A-Za-z]:/' "$dep_file"; then
            force_rebuild=1
            break
        fi
    done < <(find "$build_dir" -type f -name '*.d' -print)
    while IFS= read -r host_binary; do
        if [[ "$(head -c 2 "$host_binary" 2>/dev/null)" == 'MZ' ]]; then
            force_rebuild=1
            break
        fi
    done < <(find "$build_dir" -maxdepth 1 -type f -name '*.exe' -print)
fi

if ((force_rebuild)); then
    echo '检测到 Windows 生成的构建文件，清理生成的依赖并重新生成 WSL 产物'
    find "$build_dir" -type f -name '*.d' -delete
    set -- -B "$@"
fi

exec "$make_cmd" -f GNUmakefile "$@"
