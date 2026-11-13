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
    cppclass TurnTerminal:
        string reason
        int winner_seat
        vector[int] final_prestige
    cppclass TurnTraceStep:
        uint32_t game_ply
        uint8_t actor
        bint forced
        string action_key
        string choice
        optional[uint8_t] refill_slot
        optional[uint16_t] refill_card
        optional[uint8_t] blind_draw_tier
    cppclass NativeTurnEpisode "splendor::TurnEpisode":
        NativeTurnEpisode(const NativeCatalog&, uint32_t, uint64_t, uint64_t) except +
        uint32_t max_game_plies()
        string phase() except +
        uint32_t game_ply()
        uint8_t current_player()
        uint64_t decision_seed() except +
        vector[string] choices() except +
        bint is_terminal()
        TurnTerminal terminal() except +
        TurnTerminal timeout_result(uint8_t) except +
        optional[TurnTerminal] adjudicate_timeout(const GameClock&, double) except +
        void select(const string&) except +
        void select(size_t) except +
        string observe(uint8_t) except +
        string choice_details() except +
        string token_takes() except +
        vector[TurnTraceStep] drain_trace() except +

cdef extern from "splendor/replay.hpp" namespace "splendor":
    cppclass TraceReplay:
        TraceReplay(vector[TurnTraceStep]) except +
        void verify(const vector[TurnTraceStep]&) except +
        void finish() except +

# A validated native time control: `kind` `turn` or `bank`, one allowance per seat.
cdef TimeControl time_control(str kind, allowance_ms, forfeit_on_timeout=*) except *

cdef class Catalog:
    cdef NativeCatalog* _catalog

cdef class Episode:
    cdef NativeTurnEpisode* _episode
    cdef readonly Catalog catalog
    cdef TraceReplay* _replay

cdef class Clock:
    cdef GameClock* _clock
