# Clang P2996 local toolchain

`debug-p2996-clang` expects the P2996 Clang install to be selected by the
environment. The preset uses `clang++` and `clang-scan-deps` by name, and uses
`CLANG_P2996_ROOT` only to locate the matching libc++ libraries and module
manifest.

For a local install under `~/.local/opt/clang-p2996`:

```sh
export CLANG_P2996_ROOT="$HOME/.local/opt/clang-p2996"
export PATH="$CLANG_P2996_ROOT/bin:$PATH"
export LD_LIBRARY_PATH="$CLANG_P2996_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
```

Then configure and build normally:

```sh
cmake --preset debug-p2996-clang
cmake --build --preset debug-p2996-clang --target conflux_json_reflect_tests
./scripts/run-ctest.sh --test-dir /tmp/gcc-16/debug-p2996-clang -R '^conflux_json_reflect_tests$' --output-on-failure
```

Quick checks:

```sh
command -v clang++
command -v clang-scan-deps
clang++ -print-resource-dir
clang++ -print-file-name=libc++.so
```

`clang++` and `clang-scan-deps` should resolve inside `$CLANG_P2996_ROOT/bin`.
