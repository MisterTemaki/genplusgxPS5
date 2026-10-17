#!/usr/bin/env bash
# Genesis Plus GX PS5: the link arguments for RADV (the GPU build), from PS5_Vulkan's own recipe
# (tools/radv-link.sh in github.com/mihawk-99/PS5_Vulkan): the driver archive linked whole, the payload SDK fork's
# platform layer bound to the libc names no system module exports, its libc++ for RADV's compiler, Clang's builtins,
# the allocations moved to the platform's heap in direct memory. Printed one argument per line.
#
#   radv_link_args.sh <PS5_Vulkan checkout> <SDK root> <RADV archive>
#
# SPDX-License-Identifier: MIT (this script); PS5_Vulkan's recipe is GPL-3.0-or-later.
set -euo pipefail
root=$1 sdk=$2 archive=$3
# shellcheck source=/dev/null
source "$root/tools/radv-link.sh"
radv_link_recipe "$root" "$sdk" "$archive" >&2 || exit 2
printf '%s\n' "${radv_linker_script[@]}" "${radv_link_flags[@]}" "${radv_link_inputs[@]}"
