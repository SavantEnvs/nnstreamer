#!/usr/bin/env bash
# mayhem/build.sh — nnstreamer: sanitized meson build of the GStreamer plugins + sub-plugins for the
# fuzz harnesses (mayhem/build-fuzz), the harnesses themselves, and a normal-flags build with the
# upstream gtest suite (mayhem/build-test) that mayhem/test.sh runs. Re-runnable and offline
# (all deps come from apt in the image).
set -euo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
export CC CXX
SRC="${SRC:-/mayhem}"
cd "$SRC"
M="$SRC/mayhem"
FB="$M/build-fuzz"
TB="$M/build-test"

# Disable optional frameworks that would otherwise be auto-detected; the build only needs apt deps.
COMMON_OPTS=(-Dwerror=false -Dpython3-support=disabled -Dlua-support=disabled
  -Dgrpc-support=disabled -Dmqtt-support=disabled -Dnnstreamer-edge-support=disabled
  -Dml-agent-support=disabled)

# Instrument the shared libraries/plugins with SanCov too (the harness links libFuzzer), so the
# fuzzer gets coverage from the plugin code it loads.
COV=""
[ -n "$LIB_FUZZING_ENGINE" ] && COV="-fsanitize=fuzzer-no-link"
FCFLAGS="$SANITIZER_FLAGS $COV $DEBUG_FLAGS"

configure() { # <builddir> <c_args> <link_args> <extra opts...>
  local d="$1" cargs="$2" largs="$3"; shift 3
  if [ -f "$d/build.ninja" ]; then
    meson configure "$d" -Dc_args="$cargs" -Dcpp_args="$cargs" -Dc_link_args="$largs" -Dcpp_link_args="$largs" "$@"
  else
    meson setup "$d" -Dbuildtype=plain -Dc_args="$cargs" -Dcpp_args="$cargs" \
      -Dc_link_args="$largs" -Dcpp_link_args="$largs" "$@"
  fi
}

# 1) Sanitized build (undefined sanitizer symbols in the .so are resolved from the harness exe).
# ORC is off here: its JIT maps generated code from a temp file, which fails (SEGV) where /tmp is
# read-only (as under Mayhem), and the plain C kernels it replaces are the ones worth instrumenting.
configure "$FB" "$FCFLAGS" "$SANITIZER_FLAGS $COV" -Db_lundef=false -Denable-test=false \
  -Dorcc-support=disabled "${COMMON_OPTS[@]}"
ninja -C "$FB" -j"$MAYHEM_JOBS"

# 2) Harnesses (fuzzer + standalone reproducer each). Every harness links the nnstreamer plugin
# libraries and sub-plugins it drives directly (DT_NEEDED + rpath), so they are loaded, and the
# sub-plugins registered by their constructors, before LLVMFuzzerInitialize runs; the harness then
# hands the plugin files to GStreamer explicitly (no registry scan).
GST_CFLAGS="$(pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 glib-2.0)"
GST_LIBS="$(pkg-config --libs gstreamer-1.0 gstreamer-app-1.0 glib-2.0)"
GST_SYS_PLUGIN_DIR="$(pkg-config --variable=pluginsdir gstreamer-1.0)"
NNS_INC="-I$SRC/gst/nnstreamer/include -I$FB/gst/nnstreamer/include -I$FB/gst/nnstreamer -I$FB"
NNS_SO=("$FB/gst/nnstreamer/libnnstreamer.so" "$FB/gst/nnstreamer/libnnstreamer-single.so"
  "$FB/gst/datarepo/libgstdatarepo.so"
  "$FB"/ext/nnstreamer/tensor_decoder/libnnstreamer_decoder_*.so
  "$FB"/ext/nnstreamer/tensor_converter/libnnstreamer_converter_*.so)
NNS_RPATH="$FB/gst/nnstreamer:$FB/gst/datarepo:$FB/ext/nnstreamer/tensor_decoder:$FB/ext/nnstreamer/tensor_converter:$FB/ext/nnstreamer/extra"
NNS_LIB="-Wl,--no-as-needed ${NNS_SO[*]} -Wl,--as-needed -Wl,-rpath,$NNS_RPATH"
$CXX $SANITIZER_FLAGS $DEBUG_FLAGS -c "$M/lsan_off.cc" -o "$FB/lsan_off.o"
$CC $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o "$FB/standalone_main.o"
for t in converter_flexible converter_flatbuf converter_protobuf converter_flexbuf \
         sparse_dec decoder transform datareposrc elements filter caps converter_raw; do
  $CC $SANITIZER_FLAGS $DEBUG_FLAGS -DFUZZ_${t^^} -DNNS_FUZZ_BUILD="\"$FB\"" \
    -DGST_SYS_PLUGIN_DIR="\"$GST_SYS_PLUGIN_DIR\"" $GST_CFLAGS $NNS_INC \
    -c "$M/harnesses/fuzz_pipeline.c" -o "$FB/fuzz_$t.o"
  $CXX $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE "$FB/fuzz_$t.o" "$FB/lsan_off.o" \
    $NNS_LIB $GST_LIBS -o "$M/fuzz_$t"
  $CXX $SANITIZER_FLAGS $DEBUG_FLAGS "$FB/standalone_main.o" "$FB/fuzz_$t.o" "$FB/lsan_off.o" \
    $NNS_LIB $GST_LIBS -o "$M/fuzz_$t-standalone"
done

# 3) Normal-flags build with the upstream gtest unit tests (the functional oracle).
configure "$TB" "$COVERAGE_FLAGS" "$COVERAGE_FLAGS" -Denable-test=true "${COMMON_OPTS[@]}"
ninja -C "$TB" -j"$MAYHEM_JOBS"
