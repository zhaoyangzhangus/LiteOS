#!/usr/bin/env bash
set -euo pipefail

exception_file=${1:-kernel/arch/x86_64/irq/exception.c}
fault_file=${2:-kernel/mm/fault.c}

grep -q 'frame->error_code & 1U' "$exception_file" || {
  echo "UAccess diagnostics must filter present-page protection faults" >&2
  exit 1
}

grep -q '!handled_fault' "$exception_file" || {
  echo "UAccess diagnostics must skip faults resolved by the VM" >&2
  exit 1
}

grep -q 'fault->cpu_error & 1U' "$fault_file" || {
  echo "VM fault diagnostics must filter present-page protection faults" >&2
  exit 1
}

echo "expected UAccess protection diagnostics: OK"
