#!/usr/bin/env bash
# Builds every third-party dependency Tube3DS needs into ./deps, without root
# and without touching /opt/devkitpro:
#
#   deps/prefix   static 3DS libs + headers: zlib, mbedtls, curl, ffmpeg
#   deps/tools    host tools for packaging the .cia: makerom, bannertool
#
# Only devkitARM + libctru + citro2d/citro3d (the stock devkitPro 3DS install)
# are required. Re-running is cheap: finished steps are skipped via stamp files.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="$ROOT/deps"
SRC="$DEPS/src"
PREFIX="$DEPS/prefix"
TOOLS="$DEPS/tools"
PATCHES="$ROOT/scripts/patches"
JOBS="$( (sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4) )"

: "${DEVKITPRO:=/opt/devkitpro}"
: "${DEVKITARM:=$DEVKITPRO/devkitARM}"
export DEVKITPRO DEVKITARM
export PATH="$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH"

ZLIB_VER=1.3.1
MBEDTLS_VER=2.28.8
CURL_VER=8.4.0
FFMPEG_VER=7.1.1

# Same flags devkitPro's 3dsvars.sh uses for its portlibs.
ARCH_FLAGS="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft"
export CFLAGS="$ARCH_FLAGS -O2 -mword-relocations -ffunction-sections -fdata-sections"
export CXXFLAGS="$CFLAGS"
export CPPFLAGS="-D__3DS__ -I$DEVKITPRO/libctru/include -I$PREFIX/include"
export LDFLAGS="$ARCH_FLAGS -L$DEVKITPRO/libctru/lib -L$PREFIX/lib -specs=3dsx.specs"
export CC=arm-none-eabi-gcc CXX=arm-none-eabi-g++ AR=arm-none-eabi-ar RANLIB=arm-none-eabi-ranlib

mkdir -p "$SRC" "$PREFIX/include" "$PREFIX/lib" "$TOOLS/bin"

log()  { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
done_() { [ -f "$DEPS/.stamp-$1" ]; }
mark()  { touch "$DEPS/.stamp-$1"; }

fetch() { # url dest
  [ -f "$2" ] && return 0
  curl -fL --retry 3 -o "$2.part" "$1"
  mv "$2.part" "$2"
}

if ! command -v arm-none-eabi-gcc >/dev/null; then
  echo "devkitARM not found. Install devkitPro (https://devkitpro.org/wiki/Getting_Started) with the 3ds-dev group." >&2
  exit 1
fi

# ---------------------------------------------------------------- zlib
if ! done_ zlib; then
  log "zlib $ZLIB_VER"
  cd "$SRC"
  fetch "https://zlib.net/fossils/zlib-$ZLIB_VER.tar.gz" "zlib-$ZLIB_VER.tar.gz"
  rm -rf "zlib-$ZLIB_VER" && tar -xf "zlib-$ZLIB_VER.tar.gz" && cd "zlib-$ZLIB_VER"
  CHOST=arm-none-eabi ./configure --prefix="$PREFIX" --static
  make -j"$JOBS" libz.a
  make install-libs install-headers 2>/dev/null || { cp libz.a "$PREFIX/lib/"; cp zlib.h zconf.h "$PREFIX/include/"; }
  mark zlib
fi

# ---------------------------------------------------------------- mbedtls
if ! done_ mbedtls; then
  log "mbedtls $MBEDTLS_VER"
  cd "$SRC"
  fetch "https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/v$MBEDTLS_VER.tar.gz" "mbedtls-$MBEDTLS_VER.tar.gz"
  rm -rf "mbedtls-$MBEDTLS_VER" && tar -xf "mbedtls-$MBEDTLS_VER.tar.gz" && cd "mbedtls-$MBEDTLS_VER"
  patch -Np1 -i "$PATCHES/mbedtls-$MBEDTLS_VER.patch"
  python3 scripts/config.py set MBEDTLS_ENTROPY_HARDWARE_ALT
  python3 scripts/config.py set MBEDTLS_NO_PLATFORM_ENTROPY
  python3 scripts/config.py set MBEDTLS_CMAC_C
  python3 scripts/config.py unset MBEDTLS_SELF_TEST
  python3 scripts/config.py unset MBEDTLS_TIMING_C
  make -C library -j"$JOBS" CC="$CC" AR="$AR" \
    CFLAGS="$CFLAGS $CPPFLAGS -I../include" static
  cp library/libmbedtls.a library/libmbedx509.a library/libmbedcrypto.a "$PREFIX/lib/"
  rm -rf "$PREFIX/include/mbedtls" && cp -R include/mbedtls "$PREFIX/include/"
  mark mbedtls
fi

# ---------------------------------------------------------------- curl
if ! done_ curl; then
  log "curl $CURL_VER"
  cd "$SRC"
  fetch "https://curl.se/download/curl-$CURL_VER.tar.xz" "curl-$CURL_VER.tar.xz"
  rm -rf "curl-$CURL_VER" && tar -xf "curl-$CURL_VER.tar.xz" && cd "curl-$CURL_VER"
  patch -Np1 -i "$PATCHES/curl-$CURL_VER.patch"
  ./configure CFLAGS="$CFLAGS" CPPFLAGS="$CPPFLAGS" LDFLAGS="$LDFLAGS" LIBS="-lctru" \
    --prefix="$PREFIX" --host=arm-none-eabi \
    --disable-shared --enable-static \
    --disable-ipv6 --disable-unix-sockets --disable-threaded-resolver \
    --disable-manual --disable-pthreads --disable-socketpair --disable-ntlm-wb \
    --disable-ftp --disable-file --disable-ldap --disable-ldaps --disable-rtsp \
    --disable-dict --disable-telnet --disable-tftp --disable-pop3 --disable-imap \
    --disable-smb --disable-smtp --disable-gopher --disable-mqtt --disable-docs \
    --without-libpsl --without-brotli --without-zstd --without-nghttp2 --without-libidn2 \
    --with-zlib="$PREFIX" --with-mbedtls="$PREFIX" \
    --without-ca-bundle --without-ca-path
  make -C lib -j"$JOBS"
  make -C lib install
  make -C include install
  mark curl
fi

# ---------------------------------------------------------------- ffmpeg
# Minimal build: H.264 + AAC + Opus decoders, MP4 + WebM demuxers. No network,
# no swscale/swresample (we convert YUV->RGB and float->s16 ourselves).
if ! done_ ffmpeg; then
  log "ffmpeg $FFMPEG_VER"
  cd "$SRC"
  fetch "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VER.tar.xz" "ffmpeg-$FFMPEG_VER.tar.xz"
  rm -rf "ffmpeg-$FFMPEG_VER" && tar -xf "ffmpeg-$FFMPEG_VER.tar.xz" && cd "ffmpeg-$FFMPEG_VER"
  # Upstream bug: h2645_sei.o (needed by H.264) references aom_film_grain.o,
  # which is only listed for HEVC. Without HEVC enabled the link fails.
  sed -i.bak 's/^OBJS-$(CONFIG_H264_SEI)                += h264_sei.o h2645_sei.o$/& aom_film_grain.o/' libavcodec/Makefile
  grep -q 'h2645_sei.o aom_film_grain.o$' libavcodec/Makefile || { echo "ffmpeg Makefile patch failed" >&2; exit 1; }
  # GCC 14+ turns these into hard errors; ffmpeg 7.1 trips them on arm-none-eabi
  # (int32_t is 'long' there).
  FFMPEG_WNO="-Wno-incompatible-pointer-types -Wno-int-conversion -Wno-implicit-function-declaration"
  # ffmpeg's configure also reads CFLAGS/LDFLAGS from the environment; passing
  # them twice duplicates -specs=3dsx.specs, which gcc rejects.
  env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS ./configure \
    --prefix="$PREFIX" \
    --enable-cross-compile --cross-prefix=arm-none-eabi- \
    --arch=arm --cpu=armv6k --target-os=none \
    --extra-cflags="$ARCH_FLAGS -mword-relocations -ffunction-sections -fdata-sections $CPPFLAGS $FFMPEG_WNO" \
    --extra-ldflags="$LDFLAGS" --extra-libs="-lctru -lm" \
    --pkg-config=false \
    --disable-shared --enable-static \
    --disable-programs --disable-doc --disable-debug \
    --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
    --disable-network --disable-pthreads --disable-w32threads --disable-os2threads \
    --disable-iconv --disable-bzlib --disable-lzma --disable-xlib --disable-sdl2 \
    --disable-runtime-cpudetect --disable-neon --enable-vfp --enable-armv6 --disable-armv6t2 \
    --disable-everything \
    --enable-zlib \
    --enable-decoder=h264,aac,opus \
    --enable-demuxer=mov,matroska \
    --enable-parser=h264,aac,opus
  env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS make -j"$JOBS"
  env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS make install
  mark ffmpeg
fi

# ---------------------------------------------------------------- host tools
HOSTENV=(env -u CC -u CXX -u AR -u RANLIB -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS)

if ! done_ makerom; then
  log "makerom (host)"
  cd "$SRC"
  [ -d Project_CTR ] || git clone --depth 1 https://github.com/3DSGuy/Project_CTR.git
  cd Project_CTR/makerom
  "${HOSTENV[@]}" make deps -j"$JOBS"
  "${HOSTENV[@]}" make -j"$JOBS"
  cp "$(find bin -type f -name makerom | head -1)" "$TOOLS/bin/makerom"
  mark makerom
fi

if ! done_ bannertool; then
  log "bannertool (host)"
  cd "$SRC"
  [ -d bannertool/buildtools/.git ] || [ -f bannertool/buildtools/make_base ] || {
    rm -rf bannertool
    git clone --depth 1 --recursive --shallow-submodules https://github.com/diasurgical/bannertool.git
  }
  cd bannertool
  # Its buildtools makefiles reject arm64 hosts; the sources are plain C/C++ with
  # no dependencies, so just compile them directly.
  mkdir -p hostbuild
  for f in source/pc/*.c; do
    "${HOSTENV[@]}" cc -O2 -w -c "$f" -o "hostbuild/$(basename "$f" .c).o"
  done
  "${HOSTENV[@]}" c++ -O2 -w -std=c++11 -Isource -Isource/3ds -Isource/pc \
    -DVERSION_MAJOR=1 -DVERSION_MINOR=2 -DVERSION_MICRO=0 \
    source/*.cpp source/3ds/*.cpp source/pc/*.cpp hostbuild/*.o -o "$TOOLS/bin/bannertool"
  mark bannertool
fi

log "All dependencies ready in $DEPS"
