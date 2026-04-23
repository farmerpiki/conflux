#!/bin/bash

for preset in release-{gcc-stdcxx,clang-libcxx}
do
    (cmake --build build/${preset} || cmake --build build/${preset} --clean-first) && ./build/${preset}/benchmarks/conflux_benchmarks | tee benchmark-results/${preset}.txt
done
