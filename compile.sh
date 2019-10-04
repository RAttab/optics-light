#! /usr/bin/env bash

set -o errexit -o nounset -o pipefail -o xtrace

: ${PREFIX:="."}
: ${INSTALL:="."}
: ${OPTICS_LOG:=""}

declare -a SRC
SRC=( optics
      poller
      backend_stdout
      backend_carbon
      backend_rest
      utils/utils
      utils/crest/crest )

declare -a TEST
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

declare -a BENCH
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

CFLAGS="-ggdb -O3 -march=native -pipe -std=gnu11 -D_GNU_SOURCE -pthread"
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

export CMOCKA_TEST_ABORT=1

if [[ $OPTICS_LOG = 1 ]]; then
    CFLAGS="$CFLAGS -DOPTICS_LOG"
elif [[ $OPTICS_LOG = ring ]]; then
    CFLAGS="$CFLAGS -DOPTICS_LOG -DOPTICS_LOG_RING=1"
fi

OBJ=""
for src in "${SRC[@]}"; do
    mkdir -p $(dirname $src)
    "$CC" -c -o "$src.o" "${PREFIX}/src/$src.c" $CFLAGS
    OBJ="$OBJ $src.o"
done
"$AR" rcs "$LIB" $OBJ

"$CC" -c -o test.o "${PREFIX}/test/test.c" $CFLAGS
TEST_DEPS="test.o $LIB $DEPS -lcmocka"

"$CC" -c -o bench.o "${PREFIX}/test/bench.c" $CFLAGS
BENCH_DEPS="bench.o test.o $LIB $DEPS -lcmocka"

"$CC" -c -o example "${PREFIX}/test/example.c" $DEPS $CFLAGS


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

test_name() {
    echo "$1" | sed -r "s/[a-z]*_([a-z_]*)/\1/"
}

do_test() {
    if [[ $1 =~ test_.* ]]; then
        TEST=( $(test_name "$1") )
    fi

    for test in "${TEST[@]}"; do
        "$CC" -o "test_${test}" "${PREFIX}/test/${test}_test.c" $CFLAGS $TEST_DEPS
    done

    for test in "${TEST[@]}"; do
        "./test_${test}"
    done
}

do_valgrind() {
    if [[ $1 =~ valgrind_.* ]]; then
        TEST=( $(test_name "$1") )
    fi

    for test in "${TEST[@]}"; do
        "$CC" -o "test_${test}" "${PREFIX}/test/${test}_test.c" $CFLAGS $TEST_DEPS
    done

    for test in "${TEST[@]}"; do
        valgrind --leak-check=full --track-origins=yes "./test_${test}"
    done
}

do_bench() {
    if [[ $1 =~ bench_.* ]]; then
        BENCH=( $(test_name "$1") )
    fi

    for bench in "${BENCH[@]}"; do
        "$CC" -o "bench_${bench}" "${PREFIX}/test/${bench}_bench.c" $CFLAGS $BENCH_DEPS
    done

    for bench in "${BENCH[@]}"; do
        "./bench_${bench}"
    done
}


while [[ $# -gt 0 ]]; do
    arg=$1
    case $arg in
        install) do_install ;;

        test) do_test all ;;
        test_*) do_test $arg ;;

        valgrind) do_valgrind all ;;
        valgrind_*) do_valgrind $arg ;;

        bench) do_bench all ;;
        bench_*) do_bench $arg ;;

        *)
            echo "unknown argument '$arg'"
            exit 1
           ;;
    esac

    shift
done
