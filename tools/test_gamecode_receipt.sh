#!/bin/sh
set -eu

sdk=.deps/ps5/ps5-payload-sdk/target/include
crypto=build/ps5/importer/host/source/deps/libmbedtls/include
mkdir -p build
clang-18 -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror -Wno-unused-function \
    -ffunction-sections -fdata-sections -idirafter "$sdk" -I"$crypto" \
    tools/test_gamecode_receipt.c -Wl,--gc-sections -o build/gamecode-receipt-test
build/gamecode-receipt-test
