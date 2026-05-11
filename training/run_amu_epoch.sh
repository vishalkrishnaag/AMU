#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEFAULT_CPUS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
AMUS="${1:-$DEFAULT_CPUS}"
THREADS="${2:-$DEFAULT_CPUS}"

cd "$ROOT"
g++ -std=c++17 -O2 -pthread training/trainer.cpp -o training/trainer.out
training/trainer.out "$AMUS" "$THREADS"
