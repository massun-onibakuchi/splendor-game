# distutils: language = c++
"""Game execution, legal choices and match clocks; linked only to `splendor::game`."""
from libcpp.string cimport string
from libcpp.vector cimport vector
from libcpp.optional cimport optional
from libcpp.utility cimport move
from libc.stdint cimport int8_t, uint8_t, uint16_t, uint32_t, uint64_t
from libc.string cimport memcpy

from pathlib import Path

cdef extern from *:
    """
    #include "splendor/action_tree.hpp"
    #include "splendor/candidate.hpp"
    #include "splendor/contracts.hpp"
    #include "splendor/catalog.hpp"
    #include "splendor/seed.hpp"
    #include "splendor/turn.hpp"
    static splendor::Catalog* splendor_load_catalog(const std::string& path) {
        return new splendor::Catalog(splendor::Catalog::load(path));
    }
    static std::string splendor_game_contract() { return std::string(splendor::kGameContract); }
    static std::string splendor_catalog_contract() { return std::string(splendor::kCatalogContract); }
    static std::string splendor_staged_contract() { return std::string(splendor::kStagedActionContract); }
    static std::string splendor_seed_contract() { return std::string(splendor::kSeedContract); }
    static std::string splendor_catalog_sha256() { return std::string(splendor::kCatalogPayloadSha256); }
    static std::uint64_t splendor_derive64(std::uint64_t root, const std::string& domain,
                                           const std::vector<std::uint64_t>& coordinates) {
        return splendor::derive64(root, domain, coordinates);
    }
    // Names of the native enum codes, indexed by code.
    static std::vector<std::string> splendor_phase_names() {
        return {std::string(splendor::decision_phase_name(splendor::DecisionPhase::Main)),
                std::string(splendor::decision_phase_name(splendor::DecisionPhase::Cleanup))};
    }
    static std::vector<std::string> splendor_action_family_names() {
        std::vector<std::string> names;
        for (std::size_t code = 0; code < splendor::kActionFamilyCount; ++code) {
            names.emplace_back(splendor::action_family_name(static_cast<splendor::ActionFamily>(code)));
        }
        return names;
    }
    static std::vector<std::string> splendor_primary_family_names() {
        std::vector<std::string> names;
        for (int code = 0; code <= static_cast<int>(splendor::CandidateFamily::PassForced); ++code) {
            names.emplace_back(splendor::candidate_family_name(static_cast<splendor::CandidateFamily>(code)));
        }
        return names;
    }
    """
    NativeCatalog* splendor_load_catalog(const string&) except +
    string splendor_game_contract()
    string splendor_catalog_contract()
    string splendor_staged_contract()
    string splendor_seed_contract()
    string splendor_catalog_sha256()
    uint64_t splendor_derive64(uint64_t, const string&, const vector[uint64_t]&) except +
    vector[string] splendor_phase_names() except +
    vector[string] splendor_action_family_names() except +
    vector[string] splendor_primary_family_names() except +

cdef extern from "splendor/choice_columns.hpp" namespace "splendor":
    cppclass ChoiceColumns:
        size_t rows
        vector[uint8_t] phase
        vector[uint8_t] family
        vector[uint16_t] intent
        vector[uint8_t] null_take
        vector[uint8_t] primary
        vector[int8_t] token_delta
        vector[uint8_t] payment
        vector[uint8_t] tier
        vector[uint8_t] market_slot
        vector[uint8_t] reserve_slot
        vector[uint16_t] card
        vector[uint8_t] eligible_nobles
        vector[uint8_t] returned
        vector[uint8_t] noble_slot
    const uint8_t kNoChoiceField
    const uint16_t kNoChoiceCard
    const size_t kTokenColorCount

cdef extern from *:
    """
    #include "splendor/episode.hpp"
    #include "splendor/choice_columns.hpp"
    static void splendor_choice_columns(const splendor::TurnEpisode& episode,
                                        splendor::ChoiceColumns& out) {
        splendor::write_choice_columns(episode.legal_choices(), out);
    }
    static std::size_t splendor_choice_count(const splendor::TurnEpisode& episode) {
        return episode.legal_choices().size();
    }
    """
    void splendor_choice_columns(const NativeTurnEpisode&, ChoiceColumns&) except +
    size_t splendor_choice_count(const NativeTurnEpisode&)

GAME_CONTRACT = splendor_game_contract().decode("ascii")
CATALOG_CONTRACT = splendor_catalog_contract().decode("ascii")
STAGED_ACTION_CONTRACT = splendor_staged_contract().decode("ascii")
SEED_CONTRACT = splendor_seed_contract().decode("ascii")
CATALOG_SHA256 = splendor_catalog_sha256().decode("ascii")

# The installed native package: headers, the shared library, the `SplendorGame` CMake
# package and the standard catalog.
NATIVE_PREFIX = Path(__file__).resolve().parent / "_core"
STANDARD_CATALOG = NATIVE_PREFIX / "share/splendor-game/standard-catalog-v1/catalog.json"

# Names of the codes in `choice_table` columns (`phase`, `family`, `primary`), by code.
PHASES = tuple(name.decode("ascii") for name in splendor_phase_names())
ACTION_FAMILIES = tuple(name.decode("ascii") for name in splendor_action_family_names())
PRIMARY_FAMILIES = tuple(name.decode("ascii") for name in splendor_primary_family_names())
# The value of an absent `choice_table` field.
NO_FIELD = kNoChoiceField
NO_CARD = kNoChoiceCard


def derive64(root, str domain, coordinates=()):
    """The native seed law: one derived uint64 seed per `(root, domain, coordinates)`."""
    cdef uint64_t seed = root
    cdef vector[uint64_t] native
    for coordinate in coordinates:
        native.push_back(coordinate)
    return splendor_derive64(seed, domain.encode("utf-8"), native)


cdef uint64_t _uint(value, object bits, str name) except? 0:
    # `bits` stays a Python int: the bound `1 << 32` overflows a C int.
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value < (1 << bits):
        raise ValueError(f"{name} must be a uint{bits}")
    return value


cdef TimeControl time_control(str kind, allowance_ms, forfeit_on_timeout=()) except *:
    cdef TimeControl control
    if kind == "turn":
        control.kind = ClockKind.Turn
    elif kind == "bank":
        control.kind = ClockKind.Bank
    else:
        raise ValueError("time control kind must be turn or bank")
    for milliseconds in allowance_ms:
        control.allowance_ms.push_back(milliseconds)
    for forfeit in forfeit_on_timeout:
        control.forfeit_on_timeout.push_back(forfeit)
    cdef ValidationResult result = validate_time_control(control)
    if not result.valid:
        raise ValueError(result.message.decode("utf-8"))
    return control


cdef object _column(const void* data, size_t count, str fmt, size_t itemsize, tuple shape):
    """One native column copied into a typed, shaped memoryview."""
    cdef bytearray storage = bytearray(count * itemsize)
    if count:
        memcpy(<char*>storage, data, count * itemsize)
    return memoryview(storage).cast(fmt, shape)


cdef dict _terminal(const TurnTerminal& record):
    return {
        "reason": record.reason.decode("ascii"),
        "winner_seat": None if record.winner_seat < 0 else record.winner_seat,
        "final_prestige": list(record.final_prestige),
    }


cdef class Catalog:
    """An immutable card and noble catalog, shared by any number of episodes. Any catalog
    of the standard shape loads; pin the one you play by `payload_sha256`."""

    def __cinit__(self, path):
        self._catalog = splendor_load_catalog(str(path).encode("utf-8"))

    def __dealloc__(self):
        del self._catalog

    @staticmethod
    def standard():
        """The standard catalog shipped with this package."""
        return Catalog(STANDARD_CATALOG)

    @property
    def contract(self):
        """The contract name the catalog declares."""
        return self._catalog.contract().decode("ascii")

    @property
    def payload_sha256(self):
        """SHA-256 of the catalog's canonical payload: its identity."""
        return self._catalog.payload_sha256().decode("ascii")


cdef class Episode:
    """One game on the canonical staged-action turn.

    Only decisions with more than one legal choice are exposed; forced decisions, owed
    draws and turn completion are applied natively. The episode keeps its catalog alive.
    """

    def __cinit__(self, Catalog catalog not None, uint32_t max_game_plies,
                  uint64_t setup_seed, uint64_t episode_seed):
        self.catalog = catalog
        self._episode = new NativeTurnEpisode(
            catalog._catalog[0], max_game_plies, setup_seed, episode_seed)

    def __dealloc__(self):
        del self._episode
        del self._replay

    @property
    def max_game_plies(self):
        return self._episode.max_game_plies()

    def phase(self):
        """`main`, `cleanup` or `terminal`."""
        return self._episode.phase().decode("ascii")

    def game_ply(self):
        return self._episode.game_ply()

    def current_player(self):
        return self._episode.current_player()

    def decision_seed(self):
        """The acting seat's seed for this decision under the native seed law."""
        return self._episode.decision_seed()

    def choice_count(self):
        return splendor_choice_count(self._episode[0])

    def choices(self):
        """The legal choices' keys, in choice order."""
        return tuple(key.decode("ascii") for key in self._episode.choices())

    def choice_table(self):
        """The legal choices as typed columns in choice order (`splendor/choice_columns.hpp`).

        Scalar columns have one row per choice; `token_delta`, `payment` and `returned`
        are `(rows, 6)`. Absent fields hold `NO_FIELD` (`card`: `NO_CARD`). Every column
        is a typed buffer: `numpy.asarray` wraps it without copying, `tolist()` reads it.
        A finished episode has no decision and raises `ValueError`.
        """
        if self._episode.is_terminal():
            raise ValueError("a finished episode has no decision")
        cdef ChoiceColumns columns
        splendor_choice_columns(self._episode[0], columns)
        cdef size_t rows = columns.rows
        cdef tuple wide = (rows, kTokenColorCount)
        return {
            "phase": _column(columns.phase.data(), rows, "B", 1, (rows,)),
            "family": _column(columns.family.data(), rows, "B", 1, (rows,)),
            "intent": _column(columns.intent.data(), rows, "H", 2, (rows,)),
            "null_take": _column(columns.null_take.data(), rows, "B", 1, (rows,)),
            "primary": _column(columns.primary.data(), rows, "B", 1, (rows,)),
            "token_delta": _column(columns.token_delta.data(), rows * kTokenColorCount, "b", 1, wide),
            "payment": _column(columns.payment.data(), rows * kTokenColorCount, "B", 1, wide),
            "tier": _column(columns.tier.data(), rows, "B", 1, (rows,)),
            "market_slot": _column(columns.market_slot.data(), rows, "B", 1, (rows,)),
            "reserve_slot": _column(columns.reserve_slot.data(), rows, "B", 1, (rows,)),
            "card": _column(columns.card.data(), rows, "H", 2, (rows,)),
            "eligible_nobles": _column(columns.eligible_nobles.data(), rows, "B", 1, (rows,)),
            "returned": _column(columns.returned.data(), rows * kTokenColorCount, "B", 1, wide),
            "noble_slot": _column(columns.noble_slot.data(), rows, "B", 1, (rows,)),
        }

    def is_terminal(self):
        return bool(self._episode.is_terminal())

    def terminal(self):
        return _terminal(self._episode.terminal())

    def timeout_result(self, uint8_t loser):
        return _terminal(self._episode.timeout_result(loser))

    def adjudicate_timeout(self, Clock clock not None, double now_ms):
        cdef optional[TurnTerminal] result = self._episode.adjudicate_timeout(clock._clock[0], now_ms)
        if not result.has_value():
            return None
        return _terminal(result.value())

    def select(self, str action_key):
        """Apply the legal choice with this key; any other key is rejected unchanged."""
        cdef string key = action_key.encode("ascii")
        self._episode.select(key)

    def select_index(self, size_t index):
        """Apply the legal choice at this position of choice order."""
        self._episode.select(index)

    def observe(self, uint8_t observer):
        """The current decision as `observer` sees it, as JSON text."""
        return self._episode.observe(observer).decode("ascii")

    def choice_details(self):
        """The legal choices as JSON text, public referents resolved, in key order."""
        return self._episode.choice_details().decode("ascii")

    def token_takes(self):
        """A MAIN decision's official gem takes before any return, as JSON text."""
        return self._episode.token_takes().decode("ascii")

    def drain_trace(self):
        """Every choice applied since the previous drain, forced ones included.

        `choice` is the JSON text `choice_details` gave the choice at its decision.
        """
        cdef vector[TurnTraceStep] steps = self._episode.drain_trace()
        if self._replay != NULL:
            self._replay.verify(steps)
        return [
            {
                "game_ply": step.game_ply,
                "actor": step.actor,
                "forced": bool(step.forced),
                "action_key": step.action_key.decode("ascii"),
                "choice": step.choice.decode("ascii"),
                "refill_slot": step.refill_slot.value() if step.refill_slot.has_value() else None,
                "refill_card": step.refill_card.value() if step.refill_card.has_value() else None,
                "blind_draw_tier": (
                    step.blind_draw_tier.value() if step.blind_draw_tier.has_value() else None
                ),
            }
            for step in steps
        ]

    def expect_trace(self, rows):
        """Parse persisted trace facts once, before draining any generated trace: every
        later drain is verified against them, and `finish_replay` requires all of them."""
        cdef vector[TurnTraceStep] expected
        cdef TurnTraceStep step
        if self._replay != NULL:
            raise ValueError("a replay is already active")
        for row in rows:
            step.game_ply = <uint32_t>_uint(row["game_ply"], 32, "game_ply")
            step.actor = <uint8_t>_uint(row["actor"], 8, "actor")
            if type(row["forced"]) is not bool:
                raise ValueError("forced must be boolean")
            step.forced = row["forced"]
            if type(row["action_key"]) is not str:
                raise ValueError("action_key must be a string")
            step.action_key = row["action_key"].encode("ascii")
            step.refill_slot.reset()
            step.refill_card.reset()
            step.blind_draw_tier.reset()
            if row["refill_slot"] is not None:
                step.refill_slot = <uint8_t>_uint(row["refill_slot"], 8, "refill_slot")
            if row["refill_card"] is not None:
                step.refill_card = <uint16_t>_uint(row["refill_card"], 16, "refill_card")
            expected.push_back(move(step))
        self._replay = new TraceReplay(move(expected))

    def finish_replay(self):
        if self._replay == NULL:
            raise ValueError("no replay is active")
        self._replay.finish()
        del self._replay
        self._replay = NULL


cdef class Clock:
    """One match's native clock (`splendor::GameClock`): `kind` `turn` or `bank`, one
    allowance in milliseconds per seat. Times are the caller's monotonic milliseconds."""

    def __cinit__(self, str kind, allowance_ms, forfeit_on_timeout=()):
        self._clock = new GameClock(time_control(kind, allowance_ms, forfeit_on_timeout))

    def __dealloc__(self):
        del self._clock

    def run(self, uint8_t seat, uint32_t game_ply, double now_ms):
        self._clock.run(seat, game_ply, now_ms)

    def stop(self, double now_ms):
        self._clock.stop(now_ms)

    def record(self, uint8_t seat, uint32_t game_ply, uint64_t used_ms):
        """Adopt a logged decision's turn-cumulative use while stopped."""
        self._clock.record(seat, game_ply, used_ms)

    def running(self):
        return self._clock.running()

    def actor(self):
        cdef optional[uint8_t] seat = self._clock.actor()
        return seat.value() if seat.has_value() else None

    def used_ms(self, double now_ms):
        return self._clock.used_ms(now_ms)

    def limit_ms(self):
        return self._clock.limit_ms()

    def expired(self, double now_ms):
        return self._clock.expired(now_ms)

    def loses_on_time(self, uint8_t seat):
        return self._clock.loses_on_time(seat)

    def remaining_ms(self, uint8_t seat, double now_ms):
        return self._clock.remaining_ms(seat, now_ms)
