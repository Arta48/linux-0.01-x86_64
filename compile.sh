#!/usr/bin/env bash
set -e

echo "=== Cleaning old build ==="
make clean

echo "=== Building Image ==="
make

echo "=== Launching QEMU ==="
make run
