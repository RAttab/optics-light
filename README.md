# Optics Light

Metrics gathering library for high-throughput and latency sensitive
services.

Main design goal is that recording a single should:

1. avoid sampling where possible
2. should not be more expensive then a single atomic operation (depends on metric
   type)
3. Keep the code relatively readable and maintenable

Simplification [Optics](github.com/RAttab/optics) by removing the external
polling agent which added too much operational and code complexity for very
little gain.

## Building

Dependencies:
- libbsd
- libdaemon
- libmicrohttpd


Building:
```
$ mkdir build
$ cd build
$ PREFIX=.. ../compile.sh
```

Testing:
```
$ cd build
$ PREFIX=.. ../compile.sh test valgrind bench

$ PREFIX=.. ../compile.sh test_<name>
$ PREFIX=.. ../compile.sh valgrind_<name>
$ PREFIX=.. ../compile.sh bench_<name>
```

### Usage

To log metrics refer to this [example](test/example.c) which describes the
basics of creating and logging metrics.

### Pre-emptive Nit-picking

* Assumes AMD64
* Not 100% POSIX-compliant
* Only Linux is supported
