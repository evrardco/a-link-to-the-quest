#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Initialise the submodules after cloning: every submodule in .gitmodules, shallow (one commit each),
# and openvr trimmed to what the build uses (headers, plus lib/ and bin/linux64 for monadolink_tune)
# instead of ~500 MB with Valve's samples. Submodules that are already initialised are left alone,
# so running it again never touches local work (e.g. commits on external/monado's branch).
#
# Usage: ./init.sh                 everything: the driver's dependencies and Monado (external/monado,
#                                  the matching Quest Link runtime, ~60 MB shallow)
#        ./init.sh --no-monado     only what building the driver needs
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

skip_monado=false
case "${1:-}" in
"") ;;
--no-monado) skip_monado=true ;;
*)
	echo "Usage: $0 [--no-monado]" >&2
	exit 2
	;;
esac

# Sparse checkouts: Git keeps these per clone (.gitmodules can't hold them), so they're set here.
sparse_paths() {
	case "$1" in
	external/openvr) echo "headers lib/linux64 bin/linux64" ;;
	esac
}

mapfile -t paths < <(git config -f .gitmodules --get-regexp '^submodule\..*\.path$' | awk '{print $2}')
for path in "${paths[@]}"; do
	if [ "$path" = external/monado ] && $skip_monado; then
		echo "skip   $path (--no-monado)"
		continue
	fi
	# `git submodule status` marks an uninitialised submodule with a leading '-'.
	if [[ "$(git submodule status -- "$path")" == -* ]]; then
		echo "fetch  $path"
		git submodule update --init --depth 1 -- "$path"
	else
		echo "ok     $path (already initialised, left as is)"
	fi
	sparse=$(sparse_paths "$path")
	if [ -n "$sparse" ]; then
		# shellcheck disable=SC2086 # the paths are space-separated on purpose
		git -C "$path" sparse-checkout set $sparse
		echo "       $path: sparse checkout ($sparse)"
	fi
done
echo "Submodules ready. Build: cmake -S . -B build -G Ninja && ninja -C build"
