#!/bin/sh
# Собирает проверки Linux ABI обычным gcc хоста (динамически, glibc) и кладёт их в rootfs/bin
# вместе с библиотеками. Дальше: rm -f rootfs.tar; make rootfs.tar; make run, и в системе
# запустить abi_basic, abi_fork, abi_signal_pipe, abi_thread.
cd "$(dirname "$0")/../.." || exit 1
tmp=$(mktemp -d) || exit 1
for t in abi_basic abi_fork abi_signal_pipe abi_thread abi_net abi_net2 abi_ping abi_eventfd; do
    gcc -w -O2 -pthread tests/linux/$t.c -o "$tmp/$t" || exit 1
    ./tools/import_host.sh rootfs "$tmp/$t" >/dev/null
done
rm -rf "$tmp"
