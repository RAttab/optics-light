#! /usr/bin/env bash

set -o errexit -o nounset -o pipefail -o xtrace

: ${PREFIX:="."}

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
       crest_path
       crest )

BENCH=( timer
        htable
        lens
        lens_counter
        lens_dist
        lens_gauge
        lens_histo
        lens_quantile )

CC=${OTHERC:-gcc}

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
DEPS="-lbsd -llibmicrohttpd"

OBJ=""
for src in "${SRC[@]}"; do
    mkdir -p $(dirname $src)
    $CC -c -o "$src.o" "${PREFIX}/src/$src.c" $CFLAGS $DEPS
    OBJ="$OBJ $src.o"
done
ar rcs "$LIB" $OBJ

for test in "${TEST[@]}"; do
    $CC -o "test_${test}" "${PREFIX}/test/${test}_test.c" "$LIB" $CFLAGS $DEPS
done

do_install() {
    mv "$LIB" ./bin

}

do_test() {
    for test in "${TEST[@]}"; do
        "./test_${test}"
    done
}

do_valgrind() {
    for test in "${TEST[@]}"; do
        valgrind --leak-check=full --track-origins=yes "./test_${test}"
    done
}

do_bench() {
    for bench in "${BENCH[@]}"; do
        $CC -o "bench_${bench}" "${PREFIX}/test/${bench}_bench.c" "$LIB" $CFLAGS $DEPS
        "./bench_${bench}"
    done
}


while [[ $# -gt 0 ]]; do
    arg=$1
    case $arg in
        install)
            do_install && shift;;

        test)
            do_test && shift ;;

        valgrind)
            do_valgrind && shift ;;

        bench)
            do_bench && shift ;;

        *)
            echo "unknown argument '$arg'"
            exit 1
           ;;
    esac
done
