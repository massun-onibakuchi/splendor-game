# AGENTS.md

Native Splendor engine (`include/`, `src/`, C++20), its Cython binding
(`python/splendor_game`) and the practical-play host (`python/splendor_play`, stdlib only).

## Environment

- C++: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j`, then
  `ctest --test-dir build --output-on-failure`.
- Python through `uv`: `uv sync`, `uv run pytest`. `uv sync --reinstall-package
  splendor-game` rebuilds the native library after C++ changes.

## Design stance

- **Independent.** Nothing here imports, links or names model, search, feature, training
  or experiment code. Consumers build on the engine; the engine never anticipates one.
- **End state first.** No compatibility shims or deprecation paths unless requested.
- **Contracts are persisted.** `splendor-*-vN` identifiers name what consumers keep:
  change behavior only with a new version. The seed namespace is a build constant
  (`SPLENDOR_GAME_SEED_NAMESPACE`); never change the default.
- **Small surfaces.** Public headers and explicit Python exports; trusted fast paths stay
  in `splendor/detail/` and trust only engine-produced inputs.
- **Fast paths stay native.** No Python object per choice or per state on any bulk path;
  expose buffers. Measure engine changes with `benchmarks/playout_benchmark.cpp`; a
  checksum change means play changed.
- **Known extensions.** Two players today, three and four planned: seat arguments, not
  `1 - seat` arithmetic.

## Docs

`docs/specs/` holds the normative contracts, one per subsystem
([index](docs/specs/spec.md)). Change a spec when its contract changes, in the same change.
