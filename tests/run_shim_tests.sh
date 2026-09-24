#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
shim_test_dir=$(mktemp -d)
trap 'rm -rf "$shim_test_dir"' EXIT
shim_test_flags=(-std=c11 -Wall -Wextra -Werror -g)
if [[ ${SANITIZE:-0} == 1 ]]; then
    shim_test_flags+=(-fsanitize=address,undefined)
fi
# pkg-config output intentionally expands into compiler arguments.
# shellcheck disable=SC2046
"${CC:-cc}" "${shim_test_flags[@]}" tests/shim_lifecycle.c \
    $(pkg-config --cflags --libs libfabric) \
    -Wl,--wrap=fi_getinfo -Wl,--wrap=fi_dupinfo \
    -o "$shim_test_dir/lifecycle"
"$shim_test_dir/lifecycle"
# Build the real provider example; running it requires a configured RXE device.
# shellcheck disable=SC2046
"${CC:-cc}" "${shim_test_flags[@]}" -I naive \
    examples/shim_pingpong.c naive/fabric_shim.c \
    $(pkg-config --cflags --libs libfabric) -o "$shim_test_dir/pingpong"
echo 'shim pingpong example compiled'
