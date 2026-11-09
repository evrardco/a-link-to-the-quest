#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host: save the last hour of the user journal (where vrserver's stderr usually goes when Steam is
# started from the desktop) to <project>/journal.txt, plus a short summary of Monado/OpenXR lines.
cd "$(dirname "$0")/../.."
journalctl --user --since "-60min" --no-pager -o short-precise > journal.txt 2>&1
echo "$(wc -l < journal.txt) lines in $PWD/journal.txt"
grep -n -i "xrt\|ipc\|monado\|openxr\|oxr_" journal.txt | tail -20
