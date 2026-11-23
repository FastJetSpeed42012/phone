#!/bin/sh
# Build the shell for the host with stubbed TI libraries and run a script.
# usage: tests/run_host.sh tests/scripts/basic.txt
set -e
cd "$(dirname "$0")/.."
mkdir -p tests/build
cc -std=gnu11 -DHOST -g -O1 -Wall -Wno-unused-function -fsanitize=address,undefined \
   -Itests/host/include src/*.c tests/host/stubs.c -o tests/build/cmd_host
exec tests/build/cmd_host "$@"
