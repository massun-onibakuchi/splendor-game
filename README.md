# splendor-game

Native Splendor (two players, blind reservation) for agents of any kind: a C++20 game
engine with staged decisions, legal-choice surfaces, chance, seat views, replay and match
clocks; Python bindings; and a practical-play host for human-versus-agent matches.

Contracts live in [docs/specs](docs/specs/spec.md).

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/splendor_game_benchmark 3000      # uniform playouts: staged, recorded, atomic

uv sync
uv run pytest
```

## C++

Add this directory, or install it and use the CMake package:

```cmake
add_subdirectory(splendor-game)            # SPLENDOR_GAME_CATALOG names the catalog
# or
find_package(SplendorGame CONFIG REQUIRED) # SplendorGame_CATALOG names the catalog
target_link_libraries(your_target PRIVATE splendor::game)
```

`splendor/episode.hpp` is the recorded game a host drives; `splendor/turn.hpp`,
`splendor/choice_columns.hpp` and `splendor/detail/*.hpp` are the surfaces and trusted
fast paths batched producers and searches build on.

Any catalog of the standard shape loads; its identity is its declared contract and payload
digest. Every seed derives from the seed law, whose domain-separation tag is the build
constant `SPLENDOR_GAME_SEED_NAMESPACE` (default `splendor-seed-derivation-v1`; the Python
build reads the same environment variable). Keep the default unless you must reproduce seeds
published under another tag.

## Python

```python
from splendor_game import Catalog, Episode, derive64

episode = Episode(Catalog.standard(), 160, derive64(1, "setup"), derive64(1, "episode"))
while not episode.is_terminal():
    table = episode.choice_table()          # typed columns, one row per legal choice
    episode.select_index(0)
print(episode.terminal())
```

Native extensions `cimport splendor_game.engine` and link the shared library under
`splendor_game.NATIVE_PREFIX`. `splendor_play` serves a human-versus-agent match:
`make_server(setup, agents, static=..., catalog_path=..., archive=Archive(...), ...)`.
