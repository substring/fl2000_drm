#!/bin/bash
# FL2000 VGA first-picture test — run from a TTY with the graphical session stopped:
#   sudo systemctl isolate multi-user.target   (from COSMIC; kills the desktop!)
#   <log in at the text console>
#   cd ~/src/fl2000/fl2000_drm && sudo ./tty_test.sh
#
# Everything is logged to ~/src/fl2000/tty-test.log for later analysis.
# The test pattern stays up until you press Enter (or 5 minutes). Photograph the
# monitor while it shows. Afterwards the module is LEFT LOADED so that
#   sudo systemctl isolate graphical.target
# lets the compositor discover the connector (stage 4 of the plan).

LOG=/home/rroberts/src/fl2000/tty-test.log
DIR=/home/rroberts/src/fl2000/fl2000_drm

exec > >(tee "$LOG") 2>&1

say() { echo; echo "=== $(date +%T) $* ==="; }

say "environment"
uname -r
lsmod | grep -E "^fl2000|^drm_dma" || echo "fl2000 not loaded (good)"

say "load module (vga_mode=1)"
modprobe drm_dma_helper
dmesg -C
insmod "$DIR/fl2000.ko" vga_mode=1 dyndbg=+p || { echo "INSMOD FAILED rc=$?"; dmesg; exit 1; }
sleep 3

say "dmesg after load"
dmesg

say "connector state (validates 0x805C detect)"
for f in status enabled modes; do
	echo "--- $f ---"
	timeout 20 cat /sys/class/drm/card*-VGA-1/$f 2>&1
done

say "modetest connector listing"
CONN=$(timeout 30 modetest -M fl2000_drm -c 2>/dev/null | awk '/VGA/ {print $1; exit}')
timeout 30 modetest -M fl2000_drm -c 2>&1 | head -25
echo "connector id: ${CONN:-NOT FOUND}"

if [ -z "$CONN" ]; then
	echo "No VGA connector found by modetest - stopping. Module left loaded."
	exit 1
fi

say "starting 1024x768 SMPTE test pattern (connector $CONN)"
# sleep keeps modetest's stdin open so the pattern persists; Enter below ends it
sleep 300 | modetest -M fl2000_drm -s "$CONN":1024x768 -P 2>/dev/null &
MTPID=$!
sleep 5

say "dmesg after modeset"
dmesg | tail -40

say "0x50 DDC rescan WHILE streaming (sync-dependent DDC theory)"
BUS=$(for c in /sys/bus/i2c/devices/i2c-*; do n=$(cat $c/name 2>/dev/null); case "$n" in *FL2000*) basename "$c" | sed 's/i2c-//';; esac; done)
echo "FL2000 bus: i2c-${BUS:-NOT FOUND}"
ACKS=0
for i in $(seq 1 20); do
	r=$(timeout 5 i2ctransfer -y -f "$BUS" w1@0x50 0x00 r1 2>/dev/null) && { ACKS=$((ACKS+1)); echo "poll $i: ACK $r"; }
	sleep 0.5
done
echo "DDC polls: 20, ACKs: $ACKS"

say "PHOTOGRAPH THE MONITOR NOW - press Enter when done (auto-continues in 5 min)"
read -t 300 -r _

kill $MTPID 2>/dev/null
wait 2>/dev/null

say "final dmesg tail"
dmesg | tail -25

say "done - module left loaded; now run: sudo systemctl isolate graphical.target"
