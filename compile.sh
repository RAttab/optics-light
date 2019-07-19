#! /usr/bin/env bash

set -o errexit -o nounset -o pipefail -o xtrace

: ${PREFIX:="."}
: ${INSTALL:="."}

declare -a SRC
SRC=( optics
      poller
      backend_stdout
      backend_carbon
      backend_rest
      utils/utils
      utils/crest/crest )

TEST=( timer
       htable
       buffer
       key
       lens
       lens_counter
       lens_dist
       lens_gauge
       lens_histo
       lens_quantile
       poller
       poller_lens
       backend_carbon
       backend_rest
       crest )

BENCH=( timer
        htable
        lens
        lens_counter
        lens_dist
        lens_gauge
        lens_histo
        lens_quantile )

PKG_CONFIGS=( optics optics_static )

CC=${OTHERC:-gcc}
AR=${OTHERC:-ar}

CFLAGS="-g -O3 -march=native -pipe -std=gnu11 -D_GNU_SOURCE -pthread"
CFLAGS="$CFLAGS -I${PREFIX}/src"

CFLAGS="$CFLAGS -Werror -Wall -Wextra"
CFLAGS="$CFLAGS -Wundef"
CFLAGS="$CFLAGS -Wcast-align"
CFLAGS="$CFLAGS -Wwrite-strings"
CFLAGS="$CFLAGS -Wunreachable-code"
CFLAGS="$CFLAGS -Wformat=2"
CFLAGS="$CFLAGS -Wswitch-enum"
CFLAGS="$CFLAGS -Wswitch-default"
CFLAGS="$CFLAGS -Winit-self"
CFLAGS="$CFLAGS -Wno-strict-aliasing"
CFLAGS="$CFLAGS -fno-strict-aliasing"
CFLAGS="$CFLAGS -Wno-implicit-fallthrough"

LIB="liboptics.a"
DEPS="-lbsd -lmicrohttpd"

OBJ=""
for src in "${SRC[@]}"; do
    mkdir -p $(dirname $src)
    "$CC" -c -o "$src.o" "${PREFIX}/src/$src.c" $CFLAGS
    OBJ="$OBJ $src.o"
done
"$AR" rcs "$LIB" $OBJ

$CC -c -o test.o "${PREFIX}/test/test.c" $CFLAGS
TEST_DEPS="test.o $LIB $DEPS -lcmocka"

for test in "${TEST[@]}"; do
    "$CC" -o "test_${test}" "${PREFIX}/test/${test}_test.c" $CFLAGS $TEST_DEPS
done

version() {
    git --git-dir "${PREFIX}" describe --tags --exact-match 2> /dev/null \
        || git --git-dir "${PREFIX}" symbolic-ref -q --short HEAD 2> /dev/null \
        || git --git-dir "${PREFIX}" rev-parse --short HEAD
}

do_install() {
    mv "$LIB" "${INSTALL}/bin"

    export pc_prefix="${INSTALL}"
    export pc_version="$(version)"
    for pc in "${SRC[@}"; do
        envsubst '$$pc_prefix' '$$pc_version' \
                 <"${PREFIX}/src/${pc}.pc.in" \
                 >"${INSTALL}/share/pkgconfig/${pc}.pc"
    done
}

do_test() {
    $CC -c -o test.o "${PREFIX}/test/test.c" $CFLAGS
    TEST_DEPS="test.o $LIB $DEPS -lcmocka"

    for test in "${TEST[@]}"; do
        "$CC" -o "test_${test}" "${PREFIX}/test/${test}_test.c" $CFLAGS $TEST_DEPS
        "./test_${test}"
    done
}

do_valgrind() {
    for test in "${TEST[@]}"; do
        valgrind --leak-check=full --track-origins=yes "./test_${test}"
    done
}

do_bench() {
    $CC -c -o bench.o "${PREFIX}/test/bench.c" $CFLAGS
    BENCH_DEPS="bench.o $LIB $DEPS"

    for bench in "${BENCH[@]}"; do
        "$CC" -o "bench_${bench}" "${PREFIX}/test/${bench}_bench.c" $CFLAGS $BENCH_DEPS
        "./bench_${bench}"
    done
}


while [[ $# -gt 0 ]]; do
    arg=$1
    case $arg in
        install) do_install ;;
        test) do_test ;;
        valgrind) do_valgrind ;;
        bench) do_bench ;;
        *)
            echo "unknown argument '$arg'"
            exit 1
           ;;
    esac

    shift
done
