# Native declarations for downstream extensions: `cimport` these to read an `Episode`'s or
# `Clock`'s native object directly, without converting game values to Python objects.
# Link the same shared `splendor_game` library (`splendor_game.NATIVE_PREFIX`).
from libcpp.string cimport string
from libcpp.vector cimport vector
from libcpp.optional cimport optional
from libcpp cimport bool as cppbool
from libc.stdint cimport uint8_t, uint16_t, uint32_t, uint64_t, int64_t

cdef extern from "splendor/state.hpp" namespace "splendor":
    cppclass ValidationResult:
        bint valid
        string message

cdef extern from "splendor/clock.hpp" namespace "splendor":
    cdef enum class ClockKind:
        Turn
        Bank
    cppclass TimeControl:
        TimeControl() except +
        ClockKind kind
        vector[uint64_t] allowance_ms
        vector[cppbool] forfeit_on_timeout
    ValidationResult validate_time_control(const TimeControl&) except +
    cppclass GameClock:
        GameClock(TimeControl) except +
        void run(uint8_t, uint32_t, double) except +
        void stop(double)
        void record(uint8_t, uint32_t, uint64_t) except +
        bint running()
        optional[uint8_t] actor()
        int64_t used_ms(double)
        int64_t limit_ms() except +
        bint expired(double) except +
        bint loses_on_time(uint8_t) except +
        int64_t remaining_ms(uint8_t, double) except +

cdef extern from "splendor/catalog.hpp" namespace "splendor":
    cppclass NativeCatalog "splendor::Catalog":
        const string& contract()
        const string& payload_sha256()

cdef extern from "splendor/episode.hpp" namespace "splendor":
    cppclass NativeTurnEpisode "splendor::TurnEpisode":
        NativeTurnEpisode(const NativeCatalog&, uint8_t, uint32_t, uint64_t, uint64_t) except +
        uint32_t max_game_plies()
        uint8_t player_count()
        uint32_t game_ply()
        uint8_t current_player()
        uint64_t decision_seed() except +
        bint is_terminal()
        void select(size_t) except +

# A validated native time control: `kind` `turn` or `bank`, one allowance per seat.
cdef TimeControl time_control(str kind, allowance_ms, forfeit_on_timeout=*) except *

cdef class Catalog:
    cdef NativeCatalog* _catalog

# An `Episode` or `Clock` owns its native object, or views one that `_owner` keeps alive.
# Read-only views are what an agent receives of a host's episode and clock.
cdef class Episode:
    cdef NativeTurnEpisode* _episode
    cdef readonly Catalog catalog
    cdef object _owner

cdef class Clock:
    cdef GameClock* _clock
    cdef object _owner

cdef Episode episode_view(const NativeTurnEpisode* episode, Catalog catalog, object owner)
cdef Clock clock_view(const GameClock* clock, object owner)
