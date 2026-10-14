#!/usr/bin/env bash
set -euo pipefail

qemu_launcher="${1:-}"
if [[ -z "$qemu_launcher" || ! -f "$qemu_launcher" ]]; then
    echo "usage: $0 /path/to/run-qemu.sh" >&2
    exit 2
fi

net_guard_line="$(grep -n -m1 'if \[\[ -n "\$net_dump" \]\]' "$qemu_launcher" | cut -d: -f1 || true)"
netdev_line="$(grep -n -F -m1 ' -netdev "user,id=vm-net,host=10.0.2.2"' "$qemu_launcher" | cut -d: -f1 || true)"
nic_line="$(grep -n -m1 ' -device "e1000e,netdev=vm-net' "$qemu_launcher" | cut -d: -f1 || true)"

if [[ -z "$net_guard_line" || -z "$netdev_line" || -z "$nic_line" ||
      "$netdev_line" -ge "$net_guard_line" || "$nic_line" -ge "$net_guard_line" ]]; then
    echo "QEMU must attach e1000e with the explicit host-loopback alias by default; --net-dump may only add a filter" >&2
    exit 1
fi
if grep -q 'guestfwd=.*cmd:socat' "$qemu_launcher"; then
    echo "Default proxy must use the host alias, not the subprocess relay" >&2
    exit 1
fi

if grep -q ' -net none' "$qemu_launcher"; then
    echo "QEMU must not disable the default network when --net-dump is absent" >&2
    exit 1
fi

echo "QEMU default network: e1000e user-mode NIC, independent NAT sockets via host alias 10.0.2.2"
