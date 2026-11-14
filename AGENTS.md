# AGENTS.md

Native Splendor engine (`include/`, `src/`, C++20), its Cython binding
(`python/splendor_game`) and the practical-play host (`python/splendor_play`).

## Environment

- C++: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j`, then
  `ctest --test-dir build --output-on-failure`.
- Python through `uv`: `uv sync`, `uv run pytest`. `uv sync --reinstall-package
  splendor-game` rebuilds the native library after C++ changes.

## Design stance

- **Independent.** Nothing here imports, links or names model, search, feature, training
  or experiment code. Consumers build on the engine.
- **Trusted fast paths** stay in `splendor/detail/` and trust only engine-produced inputs.
- **Fast paths stay native.** No Python object per choice or per state on any bulk path;
  expose buffers. Measure engine changes with `benchmarks/playout_benchmark.cpp`; a
  checksum change means play changed.
- **Every seat count.** Games of two to four seats share one state layout and one API;
  seat arithmetic reads the state's own count. Nothing assumes two seats.

## Docs

[`docs/specs/`](docs/specs/spec.md) holds the normative contracts, one per subsystem.
