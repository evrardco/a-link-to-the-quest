#!/usr/bin/env bash
# Host: probe Monado for 10 s (wear the headset and move your head), then copy SteamVR's last log.
# Writes <project>/probe.txt and <project>/vrserver.txt.
cd "$(dirname "$0")/../.."
echo "Probing for 10 s: put the headset on and look around..."
monadolink/scripts/probe.sh 10
cp ~/.local/share/Steam/logs/vrserver.txt vrserver.txt && echo "copied vrserver.txt"
