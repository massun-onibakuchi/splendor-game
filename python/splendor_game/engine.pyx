# distutils: language = c++
"""Game execution, legal choices and match clocks over `splendor::game`, with the
`splendor::play` renderings a host shows."""
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
    static std::string splendor_game_contract(std::uint8_t players) {
        return std::string(splendor::game_contract(players));
    }
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
    string splendor_game_contract(uint8_t) except +
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

cdef extern from "splendor/contracts.hpp" namespace "splendor":
    const uint8_t kMinPlayers
    const uint8_t kMaxPlayers

cdef extern from *:
    """
    #include "splendor/episode.hpp"
    #include "splendor/choice_columns.hpp"
    #include "splendor/replay.hpp"
    #include "splendor/play/render.hpp"
    // Presentation shims: keys and JSON renderings derive from the typed episode.
    static void splendor_choice_columns(const splendor::TurnEpisode& episode,
                                        splendor::ChoiceColumns& out) {
        splendor::write_choice_columns(episode.legal_choices(), out);
    }
    static std::size_t splendor_choice_count(const splendor::TurnEpisode& episode) {
        return episode.legal_choices().size();
    }
    static std::string splendor_phase(const splendor::TurnEpisode& episode) {
        if (episode.is_terminal()) return "terminal";
        return std::string(splendor::decision_phase_name(episode.world().phase));
    }
    static std::vector<std::string> splendor_choice_keys(const splendor::TurnEpisode& episode) {
        std::vector<std::string> keys;
        keys.reserve(episode.legal_choices().size());
        for (const splendor::TurnChoice& choice : episode.legal_choices()) {
            keys.push_back(splendor::choice_key(choice));
        }
        return keys;
    }
    static void splendor_select_key(splendor::TurnEpisode& episode, const std::string& key) {
        episode.select(std::string_view(key));
    }
    static std::string splendor_observe(const splendor::TurnEpisode& episode, std::uint8_t observer) {
        return splendor::play::board_json(episode.view(observer), episode.catalog(), episode.terminal());
    }
    static std::string splendor_choice_details(const splendor::TurnEpisode& episode) {
        return splendor::play::choices_json(episode.legal_choices(), episode.world().state);
    }
    static std::string splendor_token_takes(const splendor::TurnEpisode& episode) {
        if (episode.is_terminal()) return "[]";
        return splendor::play::token_takes_json(episode.world());
    }
    static std::string splendor_result(const splendor::TurnEpisode& episode) {
        if (!episode.terminal()) throw std::logic_error("episode is not terminal");
        return splendor::play::result_json(splendor::match_result(*episode.terminal()));
    }
    static std::string splendor_timeout_result(const splendor::TurnEpisode& episode, std::uint8_t loser) {
        return splendor::play::result_json(splendor::timeout_result(episode.world().state, loser));
    }
    static std::string splendor_adjudicate_timeout(
            const splendor::TurnEpisode& episode, const splendor::GameClock& clock, double now_ms) {
        const auto result = episode.adjudicate_timeout(clock, now_ms);
        return result ? splendor::play::result_json(*result) : std::string();
    }
    // One drained step for the host log: its key and choice rendering, verified first
    // against a replay's persisted facts when one is active.
    struct SplendorTraceRow {
        std::uint32_t game_ply{};
        std::uint8_t actor{};
        bool forced{};
        std::string action_key;
        std::string choice;
        std::optional<std::uint8_t> refill_slot;
        std::optional<std::uint16_t> refill_card;
        std::optional<std::uint8_t> blind_draw_tier;
    };
    static std::vector<SplendorTraceRow> splendor_drain_trace(
            splendor::TurnEpisode& episode, splendor::TraceReplay* replay) {
        const std::vector<splendor::TurnTraceStep> steps = episode.drain_trace();
        if (replay) replay->verify(steps);
        std::vector<SplendorTraceRow> rows;
        rows.reserve(steps.size());
        for (const splendor::TurnTraceStep& step : steps) {
            rows.push_back({step.game_ply, step.actor, step.forced, splendor::choice_key(step.choice),
                            splendor::play::choice_json(step.choice, step.noble_ids),
                            step.refill_slot, step.refill_card, step.blind_draw_tier});
        }
        return rows;
    }
    """
    void splendor_choice_columns(const NativeTurnEpisode&, ChoiceColumns&) except +
    size_t splendor_choice_count(const NativeTurnEpisode&)
    string splendor_phase(const NativeTurnEpisode&) except +
    vector[string] splendor_choice_keys(const NativeTurnEpisode&) except +
    void splendor_select_key(NativeTurnEpisode&, const string&) except +
    string splendor_observe(const NativeTurnEpisode&, uint8_t) except +
    string splendor_choice_details(const NativeTurnEpisode&) except +
    string splendor_token_takes(const NativeTurnEpisode&) except +
    string splendor_result(const NativeTurnEpisode&) except +
    string splendor_timeout_result(const NativeTurnEpisode&, uint8_t) except +
    string splendor_adjudicate_timeout(const NativeTurnEpisode&, const GameClock&, double) except +
    cppclass SplendorTraceRow:
        uint32_t game_ply
        uint8_t actor
        bint forced
        string action_key
        string choice
        optional[uint8_t] refill_slot
        optional[uint16_t] refill_card
        optional[uint8_t] blind_draw_tier
    vector[SplendorTraceRow] splendor_drain_trace(NativeTurnEpisode&, TraceReplay*) except +

# The seat counts a game may have; each count's rules are a game of their own.
MIN_PLAYERS = kMinPlayers
MAX_PLAYERS = kMaxPlayers
GAME_CONTRACTS = {
    players: splendor_game_contract(players).decode("ascii")
    for players in range(MIN_PLAYERS, MAX_PLAYERS + 1)
}
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

    def __cinit__(self, Catalog catalog not None, uint8_t player_count, uint32_t max_game_plies,
                  uint64_t setup_seed, uint64_t episode_seed):
        self.catalog = catalog
        self._episode = new NativeTurnEpisode(
            catalog._catalog[0], player_count, max_game_plies, setup_seed, episode_seed)

    def __dealloc__(self):
        del self._episode
        del self._replay

    @property
    def max_game_plies(self):
        return self._episode.max_game_plies()

    @property
    def player_count(self):
        return self._episode.player_count()

    def phase(self):
        """`main`, `cleanup` or `terminal`."""
        return splendor_phase(self._episode[0]).decode("ascii")

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
        return tuple(key.decode("ascii") for key in splendor_choice_keys(self._episode[0]))

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
        """The finished game's result as JSON text."""
        return splendor_result(self._episode[0]).decode("ascii")

    def timeout_result(self, uint8_t loser):
        """The result were `loser` to time out at the pending decision, as JSON text."""
        return splendor_timeout_result(self._episode[0], loser).decode("ascii")

    def adjudicate_timeout(self, Clock clock not None, double now_ms):
        """The pending actor's timeout under `clock` as result JSON text, or `None`."""
        cdef string result = splendor_adjudicate_timeout(self._episode[0], clock._clock[0], now_ms)
        return result.decode("ascii") if result.size() else None

    def select(self, str action_key):
        """Apply the legal choice with this key; any other key is rejected unchanged."""
        splendor_select_key(self._episode[0], action_key.encode("ascii"))

    def select_index(self, size_t index):
        """Apply the legal choice at this position of choice order."""
        self._episode.select(index)

    def observe(self, uint8_t observer):
        """The current decision as `observer` sees it, as JSON text."""
        return splendor_observe(self._episode[0], observer).decode("ascii")

    def choice_details(self):
        """The legal choices as JSON text, public referents resolved, in key order."""
        return splendor_choice_details(self._episode[0]).decode("ascii")

    def token_takes(self):
        """A MAIN decision's official gem takes before any return, as JSON text."""
        return splendor_token_takes(self._episode[0]).decode("ascii")

    def drain_trace(self):
        """Every choice applied since the previous drain, forced ones included.

        `choice` is the JSON text `choice_details` gave the choice at its decision.
        """
        cdef vector[SplendorTraceRow] steps = splendor_drain_trace(self._episode[0], self._replay)
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
        cdef vector[RecordedStep] expected
        cdef RecordedStep step
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
