cmake_minimum_required(VERSION 3.20)
project(SplendorGame VERSION 0.1.0 LANGUAGES CXX)

# A consumer that adds this directory gets the library alone; a top-level build also
# builds its tests and benchmark and installs the CMake package.
string(COMPARE EQUAL "${CMAKE_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}" SPLENDOR_GAME_TOP_LEVEL)
option(SPLENDOR_GAME_BUILD_TESTS "Build the game tests and benchmark" ${SPLENDOR_GAME_TOP_LEVEL})
option(SPLENDOR_GAME_INSTALL "Install the SplendorGame CMake package" ${SPLENDOR_GAME_TOP_LEVEL})
# The seed law's domain-separation tag: every seed the engine derives depends on it. A
# downstream that already published seeds under another tag builds with that tag.
set(SPLENDOR_GAME_SEED_NAMESPACE "splendor-seed-derivation-v1" CACHE STRING
  "Domain-separation tag of the seed law (26-33 characters of [a-z0-9.-])")
string(LENGTH "${SPLENDOR_GAME_SEED_NAMESPACE}" SPLENDOR_GAME_SEED_NAMESPACE_LENGTH)
if(NOT SPLENDOR_GAME_SEED_NAMESPACE MATCHES "^[a-z0-9.-]+$"
   OR SPLENDOR_GAME_SEED_NAMESPACE_LENGTH LESS 26
   OR SPLENDOR_GAME_SEED_NAMESPACE_LENGTH GREATER 33)
  message(FATAL_ERROR
    "SPLENDOR_GAME_SEED_NAMESPACE must be 26-33 characters of [a-z0-9.-]: the seed "
    "preimage's fixed opening then ends within its first SHA-256 block")
endif()
set(SPLENDOR_GAME_GENERATED "${PROJECT_BINARY_DIR}/generated/include")
configure_file(cmake/config.hpp.in "${SPLENDOR_GAME_GENERATED}/splendor/config.hpp" @ONLY)

include(GNUInstallDirs)

add_library(splendor_game
  src/action_tree.cpp
  src/candidate.cpp
  src/candidate_generation.cpp
  src/catalog.cpp
  src/choice_columns.cpp
  src/clock.cpp
  src/episode.cpp
  src/game_episode.cpp
  src/match.cpp
  src/replay.cpp
  src/seed.cpp
  src/state.cpp
  src/transition.cpp
  src/turn.cpp)
add_library(splendor::game ALIAS splendor_game)
set_target_properties(splendor_game PROPERTIES EXPORT_NAME game)
target_compile_features(splendor_game PUBLIC cxx_std_20)
target_include_directories(splendor_game
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${SPLENDOR_GAME_GENERATED}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
target_compile_options(splendor_game PRIVATE -Wall -Wextra -Wpedantic)

# Hosts and boards: presentation over the game library. The game library itself has no
# JSON and no host.
add_library(splendor_play src/play/json.cpp src/play/match.cpp src/play/render.cpp)
add_library(splendor::play ALIAS splendor_play)
set_target_properties(splendor_play PROPERTIES EXPORT_NAME play)
target_link_libraries(splendor_play PUBLIC splendor_game)
# A shared play library finds the shared game library installed beside it.
set_target_properties(splendor_play PROPERTIES INSTALL_RPATH "$<IF:$<PLATFORM_ID:Darwin>,@loader_path,$ORIGIN>")
target_compile_options(splendor_play PRIVATE -Wall -Wextra -Wpedantic)

# The standard catalog that every consumer loads: in this source tree, or installed
# beside the package (`SplendorGame_CATALOG` after `find_package`).
set(SPLENDOR_GAME_CATALOG "${PROJECT_SOURCE_DIR}/data/standard-catalog-v1/catalog.json"
  CACHE INTERNAL "The standard Splendor catalog")

if(SPLENDOR_GAME_INSTALL)
  include(CMakePackageConfigHelpers)
  set(SPLENDOR_GAME_DATADIR "${CMAKE_INSTALL_DATADIR}/splendor-game")
  install(TARGETS splendor_game splendor_play EXPORT SplendorGameTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
  install(DIRECTORY include/ "${SPLENDOR_GAME_GENERATED}/" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(DIRECTORY data/ DESTINATION "${SPLENDOR_GAME_DATADIR}")
  install(FILES third_party/picosha2/LICENSE
    DESTINATION "${SPLENDOR_GAME_DATADIR}/licenses" RENAME picosha2.txt)
  install(EXPORT SplendorGameTargets FILE SplendorGameTargets.cmake NAMESPACE splendor::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/SplendorGame")
  configure_package_config_file(cmake/SplendorGameConfig.cmake.in
    "${PROJECT_BINARY_DIR}/SplendorGameConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/SplendorGame"
    PATH_VARS SPLENDOR_GAME_DATADIR)
  write_basic_package_version_file("${PROJECT_BINARY_DIR}/SplendorGameConfigVersion.cmake"
    COMPATIBILITY ExactVersion)
  install(FILES
    "${PROJECT_BINARY_DIR}/SplendorGameConfig.cmake"
    "${PROJECT_BINARY_DIR}/SplendorGameConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/SplendorGame")
endif()

if(NOT SPLENDOR_GAME_BUILD_TESTS)
  return()
endif()

enable_testing()
include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
  URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c)
FetchContent_MakeAvailable(googletest)

add_executable(splendor_game_tests
  tests/candidate_transition_test.cpp
  tests/catalog_seed_state_test.cpp
  tests/clock_test.cpp
  tests/match_host_test.cpp
  tests/match_test.cpp
  tests/play_json_test.cpp
  tests/player_count_test.cpp
  tests/turn_test.cpp)
target_link_libraries(splendor_game_tests PRIVATE splendor::play GTest::gtest_main)
target_include_directories(splendor_game_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
target_compile_definitions(splendor_game_tests PRIVATE
  SPLENDOR_GAME_CATALOG="${SPLENDOR_GAME_CATALOG}")
target_compile_options(splendor_game_tests PRIVATE -Wall -Wextra -Wpedantic)
include(GoogleTest)
gtest_discover_tests(splendor_game_tests)

add_executable(splendor_game_benchmark benchmarks/playout_benchmark.cpp)
target_link_libraries(splendor_game_benchmark PRIVATE splendor::game)
target_compile_definitions(splendor_game_benchmark PRIVATE
  SPLENDOR_GAME_CATALOG="${SPLENDOR_GAME_CATALOG}")
target_compile_options(splendor_game_benchmark PRIVATE -Wall -Wextra -Wpedantic)
