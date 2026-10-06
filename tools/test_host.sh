#!/bin/sh
# Offline x86 host tests, all under ASan + UBSan. Run from anywhere; nothing touches a device.
#   tools/test_host.sh              unit tests + addin integration test
#   SKIP_INSTALL=1 tools/test_host.sh   without the installer test (which needs mpc-vst-plugins next to this repo)
set -eu
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
B=build/host
mkdir -p "$B"
CF="-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -g -O1 -fno-omit-frame-pointer"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
CORE="src/midi.c src/config.c"
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1

for t in midi config; do
  $CC $CF $SAN "tests/test_$t.c" $CORE -o "$B/t_$t"
  "$B/t_$t"
done

# Integration: fake libasound, the addin preloaded, a test binary named MPC.
$CC $CF $SAN -fPIC -shared tests/fake_libs.c -o "$B/libfake.so"
$CC $CF $SAN -fPIC -shared -fvisibility=hidden -DMPCSF_TEST_HOOKS -pthread \
  src/addin.c src/log.c $CORE -ldl -o "$B/libmpc_surface.so"
mkdir -p "$B/bin"
$CC $CF $SAN tests/fake_mpc.c -L"$B" -lfake -Wl,-rpath,"$PWD/$B" -ldl -o "$B/bin/MPC"
cp "$B/bin/MPC" "$B/bin/not-mpc"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
printf 'log=%s\n' "$tmp/surface.log" > "$tmp/surface.conf"
# ASan's runtime must come first in the preload list.
ASANLIB=$($CC -print-file-name=libasan.so)
run() { MPC_SURFACE_CONF="$tmp/surface.conf" LD_PRELOAD="$ASANLIB:$PWD/$B/libmpc_surface.so" "$@"; }
run "$B/bin/MPC" "$tmp/surface.log" || { cat "$tmp/surface.log" 2>/dev/null; exit 1; }
run "$B/bin/not-mpc" "$tmp/none.log" inert
# Preloaded into an unrelated program, the library must stay out of the way.
run /bin/true
echo "--- addin log"
cat "$tmp/surface.log"
# Installed layout: no MPC_SURFACE_CONF; surface.conf is read from the .so's folder and log=auto writes there.
mkdir -p "$tmp/addin"
cp "$B/libmpc_surface.so" "$tmp/addin/"
cp etc/surface.conf.example "$tmp/addin/surface.conf"
LD_PRELOAD="$ASANLIB:$tmp/addin/libmpc_surface.so" "$B/bin/MPC" "$tmp/addin/surface.log" ||
  { cat "$tmp/addin/surface.log" 2>/dev/null; exit 1; }
grep -q "config $tmp/addin/surface.conf" "$tmp/addin/surface.log" && ! grep -q "unreadable\|problem" "$tmp/addin/surface.log" ||
  { echo "FAIL: settings and log next to the .so"; cat "$tmp/addin/surface.log" 2>/dev/null; exit 1; }
echo "ok   settings and log next to the .so"
# enabled=0: a plain pass-through
printf 'enabled=0\nlog=%s\n' "$tmp/off.log" > "$tmp/surface.conf"
run "$B/bin/MPC" "$tmp/none.log" inert
echo "ok   enabled=0 is inert"
if [ "${SKIP_INSTALL:-0}" != 1 ]; then tools/test_install.sh; fi
echo "all host tests passed"
