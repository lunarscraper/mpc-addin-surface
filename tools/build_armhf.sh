#!/bin/sh
# Build libmpc_surface.so for MPC OS / Force (armv7 hard-float; glibc <= 2.31, the oldest supported MPC OS) in a Debian
# bullseye container, then check the result: no symbol newer than GLIBC_2.31, no DT_NEEDED on libasound (it is
# resolved at run time from what MPC already loaded), and only the hooks exported.
# Needs Docker with arm/v7 emulation (qemu-user binfmt). Output: build/armhf/libmpc_surface.so, and build/package/
# (the .so, the default surface.conf and addin.manifest) for tools/release.sh.
set -eu
cd "$(dirname "$0")/.."
IMAGE=${IMAGE:-arm32v7/gcc:11-bullseye}
OUT=build/armhf
mkdir -p "$OUT"
SRCS="src/addin.c src/midi.c src/log.c src/config.c"

docker run --rm --platform linux/arm/v7 -u "$(id -u):$(id -g)" -v "$PWD:/w" -w /w "$IMAGE" sh -c "
  set -e
  gcc -std=c11 -D_GNU_SOURCE -O2 -g0 -fPIC -shared -fvisibility=hidden \
      -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard \
      -Wall -Wextra -Werror -Wl,--as-needed -Wl,-z,relro,-z,now -Wl,--no-undefined \
      $SRCS -ldl -lpthread -o $OUT/libmpc_surface.so
  strip --strip-unneeded $OUT/libmpc_surface.so
  readelf -d $OUT/libmpc_surface.so | grep NEEDED > $OUT/needed.txt
  objdump -T $OUT/libmpc_surface.so | grep -o 'GLIBC_[0-9.]*' | sort -uV > $OUT/glibc.txt
  objdump -T $OUT/libmpc_surface.so | awk '\$4 == \".text\" {print \$NF}' | sort > $OUT/exports.txt
"

echo "NEEDED:"; sed 's/^/  /' "$OUT/needed.txt"
if grep -Eq 'libasound' "$OUT/needed.txt"; then echo "FAIL: links libasound" >&2; exit 1; fi
max=$(tail -n1 "$OUT/glibc.txt")
echo "highest symbol version: $max"
if [ "$(printf '%s\nGLIBC_2.31\n' "$max" | sort -V | tail -n1)" != GLIBC_2.31 ]; then
  echo "FAIL: needs $max; the oldest supported MPC OS has glibc 2.31" >&2; exit 1
fi
echo "exports:"; sed 's/^/  /' "$OUT/exports.txt"
unexpected=$(grep -Ev '^snd_rawmidi_(open|close|read|nonblock|poll_descriptors|poll_descriptors_count|poll_descriptors_revents)$' "$OUT/exports.txt" || true)
if [ -n "$unexpected" ]; then echo "FAIL: unexpected exports: $unexpected" >&2; exit 1; fi
ls -l "$OUT/libmpc_surface.so"
echo "armhf build OK"

# The package: the addin's files, which tools/release.sh turns into the release zip (with the installer).
P=build/package
rm -rf "$P"; mkdir -p "$P"
cp "$OUT/libmpc_surface.so" addin.manifest "$P/"
cp etc/surface.conf.example "$P/surface.conf"
echo "package: $P/"; ls "$P"
