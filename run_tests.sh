#!/usr/bin/env bash
# Build, load the module, run the automated tests, unload, show kernel log.
# Run from the project root:  sudo ./tests/run_tests.sh
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ $EUID -ne 0 ]]; then echo "Run with sudo (insmod needs root)"; exit 1; fi

make
lsmod | grep -q '^mychardev ' && rmmod mychardev
insmod mychardev.ko
trap 'rmmod mychardev 2>/dev/null || true; echo "--- dmesg ---"; dmesg | tail -n 8' EXIT

for _ in $(seq 20); do [[ -c /dev/mychardev ]] && break; sleep 0.1; done
[[ -c /dev/mychardev ]] || { echo "/dev/mychardev was not created"; exit 1; }

./test_app --test
