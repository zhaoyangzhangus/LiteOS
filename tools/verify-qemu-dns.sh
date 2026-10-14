#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 /path/to/qemu-network.pcap" >&2
    exit 2
fi

capture="$1"
if [[ ! -s "$capture" ]]; then
    echo "DNS check: capture is missing or empty: $capture" >&2
    exit 1
fi
if ! command -v tcpdump >/dev/null 2>&1; then
    echo "DNS check: tcpdump is required to inspect the QEMU capture" >&2
    exit 1
fi

packets="$(tcpdump -nn -vv -r "$capture" 'udp port 53' 2>/dev/null)"
if ! grep -Eq '[0-9]+\+ A\? example\.com\.' <<<"$packets"; then
    echo "DNS check: no A query for example.com was observed" >&2
    exit 1
fi
if ! grep -Eq '[0-9]+ [0-9]+/[0-9]+/[0-9]+ A [0-9]+\.' <<<"$packets"; then
    echo "DNS check: no IPv4 A answer was observed" >&2
    exit 1
fi

echo "DNS check passed: QEMU captured an example.com A query and IPv4 answer"
