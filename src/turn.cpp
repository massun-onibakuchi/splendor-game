#include "splendor/turn.hpp"

#include "splendor/detail/payment.hpp"
#include "splendor/detail/transition.hpp"
#include "splendor/detail/turn.hpp"

#include <algorithm>
#include <charconv>
#include <compare>
#include <stdexcept>
#include <string_view>

namespace splendor {
namespace {

using detail::TokenCounts;

constexpr std::size_t kGoldIndex = static_cast<std::size_t>(Color::Gold);

ValidationResult invalid(std::string message) {
    return ValidationResult{false, std::move(message)};
}

void require_valid(const ValidationResult& validation, const char* what) {
    if (!validation) throw std::logic_error(std::string(what) + ": " + validation.message);
}

constexpr std::string_view kMainKeyPrefix = "main|";

// A choice's place in its set's ascending key order, computed from its fields without
// formatting the key: comparing two ranks as words orders the choices exactly as
// comparing their keys' bytes does, and equal ranks mean equal keys.
//
// Within one choice set a family fixes which optional key fields appear and the zone,
// and every count a key spells -- a delta, payment or return per colour, a tier, slot,
// seat or noble slot -- is one digit, so keys of one family compare field by field: a
// negative delta's '-' sorts before every digit, so `-1 < -2 < ... < -9 < 0 < ... < 9`;
// a CLEANUP's absent noble `-` sorts before every slot. Only a card id has several
// digits, and it ends its key, so its prefix sorts first.
struct ChoiceRank {
    std::uint64_t high{};
    std::uint64_t low{};
    auto operator<=>(const ChoiceRank&) const = default;
};

// The byte order of `candidate_family_name`: no name is a prefix of another, so
// families compare by name before any field.
constexpr std::uint64_t family_rank(const CandidateFamily family) {
    switch (family) {
        case CandidateFamily::BuyReserved: return 0;
        case CandidateFamily::BuyVisible: return 1;
        case CandidateFamily::PassForced: return 2;
        case CandidateFamily::ReserveBlind: return 3;
        case CandidateFamily::ReserveVisible: return 4;
        case CandidateFamily::Take: return 5;
    }
    return 6;
}

// A one-digit signed decimal in byte order: '-' (0x2d) precedes '0', then the digit.
constexpr std::uint64_t signed_digit_rank(const int value) {
    return value < 0 ? static_cast<std::uint64_t>(-value - 1) : static_cast<std::uint64_t>(9 + value);
}

// A decimal of up to three digits that ends its key, in byte order: each digit one
// above its value and a missing digit zero, so `1 < 12 < 5`.
std::uint64_t final_decimal_rank(const unsigned value) {
    if (value > 999) throw std::logic_error("a key's final number exceeds three digits");
    std::array<char, 3> digits{};
    const auto end = std::to_chars(digits.data(), digits.data() + digits.size(), value).ptr;
    std::uint64_t rank = 0;
    for (const char* digit = digits.data(); digit != digits.data() + digits.size(); ++digit) {
        rank = (rank << 4) | (digit < end ? static_cast<std::uint64_t>(*digit - '0' + 1) : 0);
    }
    return rank;
}

// `main|<family>|d=..|p=..|z=<zone>|t=|s=|o=|r=|c=`: the family, the six signed deltas,
// the six payments, then the optional fields the family carries (zone and presence
// are the family's).
ChoiceRank main_rank(const CandidateDescriptor& primary) {
    ChoiceRank rank;
    rank.high = family_rank(primary.family);
    for (const std::int8_t delta : primary.token_delta) {
        rank.high = (rank.high << 5) | signed_digit_rank(delta);
    }
    for (const std::uint8_t paid : primary.payment) rank.high = (rank.high << 4) | paid;
    const auto digit = [](const auto& field) { return std::uint64_t{field.value_or(0)}; };
    rank.low = (digit(primary.source_tier) << 28) | (digit(primary.source_slot) << 24)
        | (digit(primary.reserve_owner) << 20) | (digit(primary.reserve_slot) << 16)
        | (primary.target_card_id ? final_decimal_rank(*primary.target_card_id) : 0);
    return rank;
}

// `cleanup|d=<six returned counts>|n=<slot or ->`.
template <typename Count>
ChoiceRank cleanup_rank(
    const std::array<Count, kTokenColorCount>& returned,
    const std::optional<std::uint8_t> noble_slot) {
    ChoiceRank rank;
    for (const Count count : returned) rank.high = (rank.high << 4) | static_cast<std::uint64_t>(count);
    rank.low = noble_slot ? std::uint64_t{*noble_slot} + 1 : 0;
    return rank;
}

// The ranks of one choice set in generation order. A thread reuses one instance, so its
// buffer keeps its capacity across choice sets.
class ChoiceOrder {
public:
    struct Entry {
        ChoiceRank rank;
        std::size_t index;
    };

    void clear() { entries_.clear(); }
    void add(const ChoiceRank rank) { entries_.push_back(Entry{rank, entries_.size()}); }

    // One entry per distinct rank, in ascending key order, naming the generation index
    // of a choice with that key. Equal keys describe equal choices, so this is the
    // choice set sorted by key and deduplicated.
    const std::vector<Entry>& distinct() {
        std::sort(entries_.begin(), entries_.end(), [](const Entry& lhs, const Entry& rhs) {
            return lhs.rank < rhs.rank;
        });
        entries_.erase(
            std::unique(
                entries_.begin(), entries_.end(),
                [](const Entry& lhs, const Entry& rhs) { return lhs.rank == rhs.rank; }),
            entries_.end());
        return entries_;
    }

private:
    std::vector<Entry> entries_;
};

// Per-thread working storage of choice generation. Search generates the choices of every
// expanded decision, so each set reuses these buffers instead of allocating its own.
struct ChoiceScratch {
    ChoiceOrder order;
    std::vector<CandidateDescriptor> primaries;
    // The take family's normal-form net deltas.
    std::vector<detail::TokenDelta> deltas;
    std::vector<TokenCounts> discards;
    std::vector<std::array<std::uint8_t, kTokenColorCount>> payments;
    std::vector<std::optional<std::uint8_t>> nobles;
    std::vector<std::uint8_t> eligible;

    static ChoiceScratch& local() {
        thread_local ChoiceScratch scratch;
        return scratch;
    }
};

void append_number(std::string& output, const unsigned value) {
    std::array<char, 4> digits;
    const auto result = std::to_chars(digits.data(), digits.data() + digits.size(), value);
    output.append(digits.data(), result.ptr);
}

// `cleanup|d=<six returned counts>|n=<slot or ->`.
template <typename Count>
void append_cleanup_key(
    std::string& key,
    const std::array<Count, kTokenColorCount>& returned,
    const std::optional<std::uint8_t> noble_slot) {
    key += "cleanup|d=";
    for (std::size_t color = 0; color < returned.size(); ++color) {
        if (color != 0) key.push_back(',');
        append_number(key, static_cast<std::uint8_t>(returned[color]));
    }
    key += "|n=";
    if (noble_slot) {
        append_number(key, *noble_slot);
    } else {
        key.push_back('-');
    }
}

// Nobles eligible once a primary's permanent bonus applies, computed once per decision.
// No primary changes the visible nobles, and only a purchase changes the bonuses, by one
// of its card's colour, so one set per bonus colour plus the unchanged set covers every
// primary.
class EligibleNobles {
public:
    EligibleNobles(const State& state, const Catalog& catalog) : catalog_(catalog) {
        const std::array<std::uint8_t, kGemColorCount>& held =
            state.players[state.current_player].bonuses;
        for (std::size_t bought = 0; bought <= kGemColorCount; ++bought) {
            std::array<std::uint8_t, kGemColorCount> bonuses = held;
            if (bought < kGemColorCount) ++bonuses[bought];
            detail::append_eligible_nobles(state, catalog, bonuses, sets_[bought]);
        }
    }

    const std::vector<std::uint8_t>& of(const CandidateDescriptor& primary) const {
        // Exhaustive on purpose: a new primary family must state whether it changes
        // the bonuses that noble eligibility reads.
        switch (primary.family) {
            case CandidateFamily::BuyVisible:
            case CandidateFamily::BuyReserved:
                return sets_[static_cast<std::size_t>(
                    catalog_.card(*primary.target_card_id).bonus)];
            case CandidateFamily::Take:
            case CandidateFamily::ReserveVisible:
            case CandidateFamily::ReserveBlind:
            case CandidateFamily::PassForced:
                break;
        }
        return sets_[kGemColorCount];
    }

private:
    const Catalog& catalog_;
    std::array<std::vector<std::uint8_t>, kGemColorCount + 1> sets_;
};

// Overwrites every field of a MAIN choice. A choice reused from an earlier set keeps its
// vector capacity, so refilling it does not allocate.
void assign_main_choice(
    TurnChoice& choice,
    const EligibleNobles& eligible,
    const CandidateDescriptor& primary) {
    choice.phase = DecisionPhase::Main;
    choice.primary = primary;
    choice.eligible_noble_slots = eligible.of(primary);
    choice.returned = {};
    choice.noble_slot.reset();
}

// One family-consistent primary per effect: its meaning never depends on the
// deck, the cleanup multiplicity, or whether the turn ends the game.
void main_choices(
    const State& state,
    const Catalog& catalog,
    std::vector<TurnChoice>& choices) {
    const PlayerState& player = state.players[state.current_player];
    ChoiceScratch& scratch = ChoiceScratch::local();
    std::vector<CandidateDescriptor>& primaries = scratch.primaries;
    ChoiceOrder& order = scratch.order;
    primaries.clear();
    order.clear();
    const auto emit = [&](CandidateDescriptor primary) {
        order.add(main_rank(primary));
        primaries.push_back(std::move(primary));
    };

    // A take is its gain/give normal form: one primary per net delta, its gain the
    // positive part and its return the negative part, so no cleanup return follows it
    // and no two traces of one afterstate are two choices.
    std::vector<detail::TokenDelta>& deltas = scratch.deltas;
    detail::normal_form_takes(state, deltas);
    for (const detail::TokenDelta& delta : deltas) {
        CandidateDescriptor primary;
        primary.family = CandidateFamily::Take;
        primary.token_delta = delta;
        emit(std::move(primary));
    }

    // A reservation commits exactly its mandatory gold; any overflow return is
    // cleanup, chosen after the draw. A blind reservation names a tier whose deck
    // still has cards; its card is the tier's unseen top card.
    if (reserve_count(player) < kReserveSlotsPerPlayer) {
        const bool gold_available = state.bank_tokens[kGoldIndex] != 0;
        for (std::size_t index = 0; index < state.market_card_ids.size(); ++index) {
            if (!state.market_card_ids[index]) continue;
            CandidateDescriptor primary;
            primary.family = CandidateFamily::ReserveVisible;
            primary.source_zone = SourceZone::Market;
            primary.source_tier = static_cast<std::uint8_t>(index / kMarketSlotsPerTier + 1);
            primary.source_slot = static_cast<std::uint8_t>(index % kMarketSlotsPerTier);
            primary.target_card_id = *state.market_card_ids[index];
            if (gold_available) primary.token_delta[kGoldIndex] = 1;
            emit(std::move(primary));
        }
        const TierCounts counts = tier_counts(state, catalog);
        for (std::uint8_t tier = 1; tier <= kTierCount; ++tier) {
            if (counts.deck[tier - 1] == 0) continue;
            CandidateDescriptor primary;
            primary.family = CandidateFamily::ReserveBlind;
            primary.source_zone = SourceZone::Deck;
            primary.source_tier = tier;
            if (gold_available) primary.token_delta[kGoldIndex] = 1;
            emit(std::move(primary));
        }
    }

    // A purchase commits its complete payment before any refill.
    std::vector<std::array<std::uint8_t, kTokenColorCount>>& payments = scratch.payments;
    const auto emit_buys = [&](
        const CandidateFamily family,
        const SourceZone zone,
        const std::uint16_t card_id,
        const std::optional<std::uint8_t> tier,
        const std::optional<std::uint8_t> market_slot,
        const std::optional<std::uint8_t> reserve_slot) {
        detail::payment_variants(player, catalog.card(card_id), payments);
        for (const auto& payment : payments) {
            CandidateDescriptor primary;
            primary.family = family;
            primary.source_zone = zone;
            primary.source_tier = tier;
            primary.source_slot = market_slot;
            primary.target_card_id = card_id;
            primary.payment = payment;
            if (reserve_slot) {
                primary.reserve_owner = state.current_player;
                primary.reserve_slot = reserve_slot;
            }
            emit(std::move(primary));
        }
    };
    for (std::size_t index = 0; index < state.market_card_ids.size(); ++index) {
        if (!state.market_card_ids[index]) continue;
        emit_buys(
            CandidateFamily::BuyVisible, SourceZone::Market, *state.market_card_ids[index],
            static_cast<std::uint8_t>(index / kMarketSlotsPerTier + 1),
            static_cast<std::uint8_t>(index % kMarketSlotsPerTier), std::nullopt);
    }
    for (std::size_t slot = 0; slot < player.reserves.size(); ++slot) {
        const std::optional<Reserve>& reserve = player.reserves[slot];
        if (!reserve) continue;
        if (!reserve->card_id) throw std::logic_error("the actor's reserve carries no identity");
        const std::uint16_t card_id = *reserve->card_id;
        emit_buys(
            CandidateFamily::BuyReserved, SourceZone::SelfReserve, card_id,
            reserve->tier, std::nullopt, static_cast<std::uint8_t>(slot));
    }

    if (primaries.empty()) {
        CandidateDescriptor pass;
        pass.family = CandidateFamily::PassForced;
        emit(std::move(pass));
    }
    const std::vector<ChoiceOrder::Entry>& sorted = order.distinct();
    const EligibleNobles eligible(state, catalog);
    choices.resize(sorted.size());
    for (std::size_t row = 0; row < sorted.size(); ++row) {
        assign_main_choice(choices[row], eligible, primaries[sorted[row].index]);
    }
}

void require_phase(const Decision& decision, const TurnChoice& choice, const DecisionPhase phase) {
    if (decision.phase != phase || choice.phase != phase) {
        throw std::invalid_argument("turn choice belongs to another phase");
    }
}

// Applies a cleanup's return and noble, then the end-of-turn adjudication with exactly
// one actor transition.
std::optional<TerminalFacts> finish_cleanup(
    State& state,
    const std::array<std::uint8_t, kTokenColorCount>& returned,
    const std::optional<std::uint8_t> noble_slot,
    const std::uint32_t max_game_plies,
    const Catalog& catalog) {
    const std::uint8_t seat = state.current_player;
    std::array<std::int8_t, kTokenColorCount> delta{};
    for (std::size_t color = 0; color < kTokenColorCount; ++color) {
        delta[color] = -static_cast<std::int8_t>(returned[color]);
    }
    apply_token_delta(state, seat, delta);
    claim_noble(state, catalog, seat, noble_slot);
    return detail::finish_turn_state(state, max_game_plies);
}

// Applies the cleanup and completes the turn at its next stable MAIN decision.
TurnStep complete_turn(
    State state,
    const std::array<std::uint8_t, kTokenColorCount>& returned,
    const std::optional<std::uint8_t> noble_slot,
    const std::uint32_t max_game_plies,
    const Catalog& catalog) {
    TurnStep step;
    step.terminal = finish_cleanup(state, returned, noble_slot, max_game_plies, catalog);
    step.decision.state = std::move(state);
    step.decision.phase = DecisionPhase::Main;
    return step;
}

}  // namespace

ValidationResult validate_decision(
    const Decision& decision,
    const std::uint32_t max_game_plies,
    const Catalog& catalog) {
    const State& state = decision.state;
    const auto validated = [&](const ValidationResult& result) {
        if (!result) return result;
        // The actor always knows its own reserves: a decision offered to a seat whose
        // reserve identities are missing has no authoritative choice set.
        if (state.current_player < kPlayerCount && !knows_own_reserves(state, state.current_player)) {
            return invalid("the acting seat's reserve identities are unknown");
        }
        return result;
    };
    switch (decision.phase) {
        case DecisionPhase::Main:
            return validated(validate_state(state, max_game_plies, catalog));
        case DecisionPhase::Cleanup: {
            if (const ValidationResult validation = validated(validate_state(
                    state, max_game_plies, catalog, true, StateBoundary::Intermediate));
                !validation) {
                return validation;
            }
            // The draw is resolved before any cleanup is chosen.
            if (const ValidationResult pending = detail::validate_resolved_draws(state, catalog);
                !pending) {
                return pending;
            }
            if (detail::cleanup_support(state, catalog).count < 2) {
                return invalid("a CLEANUP decision needs more than one joint cleanup");
            }
            return ValidationResult{true, {}};
        }
    }
    return invalid("unknown decision phase");
}

PlayerView observe(const Decision& decision, const std::uint8_t observer) {
    if (observed_by(decision.state, observer)) return PlayerView(&decision, std::nullopt, observer);
    return PlayerView(
        nullptr, Decision{observe(decision.state, observer), decision.phase}, observer);
}

PlayerView observe(Decision&& decision, const std::uint8_t observer) {
    if (observed_by(decision.state, observer)) {
        return PlayerView(nullptr, std::move(decision), observer);
    }
    return PlayerView(
        nullptr, Decision{observe(decision.state, observer), decision.phase}, observer);
}

PlayerView reveal(const Decision& decision) {
    if (!omniscient(decision.state)) {
        throw std::invalid_argument("reveal needs an omniscient decision");
    }
    return PlayerView(&decision, std::nullopt, decision.state.current_player);
}

PlayerView reveal(Decision&& decision) {
    if (!omniscient(decision.state)) {
        throw std::invalid_argument("reveal needs an omniscient decision");
    }
    const std::uint8_t actor = decision.state.current_player;
    return PlayerView(nullptr, std::move(decision), actor);
}

std::vector<TurnChoice> decision_choices(
    const Decision& decision,
    const std::uint32_t max_game_plies,
    const Catalog& catalog) {
    require_valid(validate_decision(decision, max_game_plies, catalog), "invalid decision");
    std::vector<TurnChoice> choices;
    detail::legal_choices(decision, catalog, choices);
    return choices;
}

MainOutcome apply_main(
    const Decision& decision,
    const TurnChoice& choice,
    const std::uint32_t max_game_plies,
    const Catalog& catalog) {
    require_phase(decision, choice, DecisionPhase::Main);
    MainOutcome outcome;
    outcome.state = decision.state;
    if (const std::optional<OwedDraw> owed =
            detail::apply_primary_effect(outcome.state, catalog, choice.primary)) {
        outcome.draw = owed;
        for (const std::uint16_t id : outcome.state.unknown_card_ids) {
            if (catalog.card(id).tier == owed->tier) outcome.outcome_card_ids.push_back(id);
        }
    }
    // Returns read token counts and eligibility reads bonuses; no drawn card changes
    // either, so the cleanup support is known before the draw.
    const detail::CleanupSupport cleanup = detail::cleanup_support(outcome.state, catalog);
    if (cleanup.count == 0) throw std::logic_error("a primary effect has no cleanup");
    outcome.cleanup_count = cleanup.count;
    if (cleanup.count == 1) {
        // A forced cleanup's terminal facts read prestige, purchases and the ply,
        // never the market or a reserve's identity, so an ending turn is adjudicated
        // before its draw; a public refill is then left undrawn. A continuing turn's
        // adjudication is discarded unread, so it is built in per-thread storage whose
        // buffers each copy reuses.
        thread_local State finished;
        finished = outcome.state;
        if (const std::optional<TerminalFacts> terminal = finish_cleanup(
                finished, cleanup.forced_return, cleanup.forced_noble, max_game_plies,
                catalog)) {
            std::swap(outcome.state, finished);
            outcome.terminal = terminal;
            if (outcome.draw && outcome.draw->kind == DrawKind::Refill) {
                outcome.draw.reset();
                outcome.outcome_card_ids.clear();
            }
        }
    }
    return outcome;
}

namespace {

// `resolve_main` from the outcome's state, owned by the caller.
TurnStep resolve_state(
    State state,
    const MainOutcome& outcome,
    const std::optional<std::uint16_t> drawn_card,
    const std::uint32_t max_game_plies,
    const Catalog& catalog) {
    if (drawn_card.has_value() != outcome.draw.has_value()) {
        throw std::invalid_argument("a drawn card is required exactly when a draw is owed");
    }
    if (drawn_card) detail::place_draw(state, catalog, *outcome.draw, *drawn_card);
    if (outcome.terminal) {
        TurnStep step;
        step.decision = Decision{std::move(state), DecisionPhase::Main};
        step.terminal = outcome.terminal;
        return step;
    }
    const detail::CleanupSupport cleanup = detail::cleanup_support(state, catalog);
    if (cleanup.count != outcome.cleanup_count) {
        throw std::logic_error("the draw changed the cleanup support");
    }
    if (cleanup.count == 1) {
        return complete_turn(
            std::move(state), cleanup.forced_return, cleanup.forced_noble, max_game_plies,
            catalog);
    }
    TurnStep step;
    step.decision.state = std::move(state);
    step.decision.phase = DecisionPhase::Cleanup;
    return step;
}

}  // namespace

TurnStep resolve_main(
    const MainOutcome& outcome,
    const std::optional<std::uint16_t> drawn_card,
    const std::uint32_t max_game_plies,
    const Catalog& catalog) {
    return resolve_state(outcome.state, outcome, drawn_card, max_game_plies, catalog);
}

TurnStep resolve_main(
    MainOutcome&& outcome,
    const std::optional<std::uint16_t> drawn_card,
    const std::uint32_t max_game_plies,
    const Catalog& catalog) {
    // The parameter takes the state before the body runs, which reads only the rest.
    return resolve_state(std::move(outcome.state), outcome, drawn_card, max_game_plies, catalog);
}

TurnStep apply_cleanup(
    const Decision& decision,
    const TurnChoice& choice,
    const std::uint32_t max_game_plies,
    const Catalog& catalog) {
    require_phase(decision, choice, DecisionPhase::Cleanup);
    return complete_turn(
        decision.state, choice.returned, choice.noble_slot, max_game_plies, catalog);
}

std::string_view decision_phase_name(const DecisionPhase phase) {
    return phase == DecisionPhase::Main ? "main" : "cleanup";
}

std::string choice_key(const TurnChoice& choice) {
    std::string key;
    detail::append_choice_key(key, choice);
    return key;
}

const TurnChoice& find_choice(
    const std::vector<TurnChoice>& choices,
    const std::string_view action_key) {
    const auto found = std::partition_point(
        choices.begin(), choices.end(),
        [&](const TurnChoice& choice) { return choice_key(choice) < action_key; });
    if (found == choices.end() || choice_key(*found) != action_key) {
        throw std::invalid_argument(
            "action key is not a legal choice of this decision: " + std::string(action_key));
    }
    return *found;
}

LeafPath leaf_path(const State& root, const CandidateDescriptor& leaf) {
    CandidateDescriptor primary = leaf;
    primary.chosen_noble_slot.reset();
    std::array<std::uint8_t, kTokenColorCount> returned{};
    // Exhaustive on purpose: a new family must state its own split here rather than
    // inherit "no return".
    switch (leaf.family) {
        case CandidateFamily::ReserveVisible: {
            // A visible reserve takes the available gold before any return, so the
            // leaf's net delta splits back into that take and its return.
            std::array<std::int8_t, kTokenColorCount> take{};
            if (root.bank_tokens[kGoldIndex] != 0) take[kGoldIndex] = 1;
            for (std::size_t color = 0; color < kTokenColorCount; ++color) {
                const int discard = take[color] - leaf.token_delta[color];
                if (discard < 0) throw std::logic_error("reserve leaf delta exceeds its take");
                returned[color] = static_cast<std::uint8_t>(discard);
            }
            primary.token_delta = take;
            break;
        }
        case CandidateFamily::ReserveBlind:
            // A leaf fixes its return before the draw; a blind reservation's owner
            // chooses its return after seeing the card, so no leaf blind reserves.
            throw std::invalid_argument("a blind reservation has no complete effective leaf");
        case CandidateFamily::Take:
        case CandidateFamily::PassForced:
        case CandidateFamily::BuyVisible:
        case CandidateFamily::BuyReserved:
            break;
    }
    LeafPath path;
    path.main_key = kMainKeyPrefix;
    detail::append_action_key(path.main_key, primary);
    append_cleanup_key(path.cleanup_key, returned, leaf.chosen_noble_slot);
    return path;
}

namespace detail {

namespace {

void fill_cleanup_choices(
    const State& state,
    const Catalog& catalog,
    std::vector<TurnChoice>& choices) {
    static const CandidateDescriptor kNoPrimary{};
    const PlayerState& player = state.players[state.current_player];
    ChoiceScratch& scratch = ChoiceScratch::local();
    std::vector<TokenCounts>& returns = scratch.discards;
    discard_variants(player, TokenCounts{}, returns);
    std::vector<std::optional<std::uint8_t>>& nobles = scratch.nobles;
    noble_choices(state, catalog, player.bonuses, nobles);
    ChoiceOrder& order = scratch.order;
    order.clear();
    for (const TokenCounts& variant : returns) {
        for (const auto noble : nobles) order.add(cleanup_rank(variant, noble));
    }
    const std::vector<ChoiceOrder::Entry>& sorted = order.distinct();
    choices.resize(sorted.size());
    for (std::size_t row = 0; row < sorted.size(); ++row) {
        // Every field is overwritten; a reused choice keeps its capacity.
        TurnChoice& choice = choices[row];
        const std::size_t index = sorted[row].index;
        const TokenCounts& variant = returns[index / nobles.size()];
        choice.phase = DecisionPhase::Cleanup;
        choice.primary = kNoPrimary;
        choice.eligible_noble_slots.clear();
        for (std::size_t color = 0; color < kTokenColorCount; ++color) {
            choice.returned[color] = static_cast<std::uint8_t>(variant[color]);
        }
        choice.noble_slot = nobles[index % nobles.size()];
    }
}

}  // namespace

std::vector<TurnChoice> cleanup_choices(const State& state, const Catalog& catalog) {
    std::vector<TurnChoice> choices;
    fill_cleanup_choices(state, catalog, choices);
    return choices;
}

CleanupSupport cleanup_support(const State& state, const Catalog& catalog) {
    const PlayerState& player = state.players[state.current_player];
    ChoiceScratch& scratch = ChoiceScratch::local();
    discard_variants(player, TokenCounts{}, scratch.discards);
    scratch.eligible.clear();
    append_eligible_nobles(state, catalog, player.bonuses, scratch.eligible);
    CleanupSupport support;
    support.count = scratch.discards.size() * std::max<std::size_t>(1, scratch.eligible.size());
    if (support.count == 1) {
        for (std::size_t color = 0; color < kTokenColorCount; ++color) {
            support.forced_return[color] = static_cast<std::uint8_t>(scratch.discards.front()[color]);
        }
        if (!scratch.eligible.empty()) support.forced_noble = scratch.eligible.front();
    }
    return support;
}

void legal_choices(
    const Decision& decision,
    const Catalog& catalog,
    std::vector<TurnChoice>& choices) {
    switch (decision.phase) {
        case DecisionPhase::Main:
            main_choices(decision.state, catalog, choices);
            return;
        case DecisionPhase::Cleanup:
            fill_cleanup_choices(decision.state, catalog, choices);
            return;
    }
    throw std::invalid_argument("unknown decision phase");
}

void append_choice_key(std::string& output, const TurnChoice& choice) {
    switch (choice.phase) {
        case DecisionPhase::Main:
            output += kMainKeyPrefix;
            append_action_key(output, choice.primary);
            return;
        case DecisionPhase::Cleanup:
            append_cleanup_key(output, choice.returned, choice.noble_slot);
            return;
    }
    throw std::invalid_argument("unknown decision phase");
}

void token_takes(const State& state, std::vector<TokenCounts>& takes) {
    takes.clear();
    std::array<std::size_t, kGemColorCount> colors{};
    std::size_t available = 0;
    for (std::size_t color = 0; color < kGemColorCount; ++color) {
        if (state.bank_tokens[color] != 0) colors[available++] = color;
    }
    if (available > 3) {
        // Every three distinct available colours, in lexicographic index order.
        for (std::size_t first = 0; first < available; ++first) {
            for (std::size_t second = first + 1; second < available; ++second) {
                for (std::size_t third = second + 1; third < available; ++third) {
                    TokenCounts take{};
                    take[colors[first]] = 1;
                    take[colors[second]] = 1;
                    take[colors[third]] = 1;
                    takes.push_back(take);
                }
            }
        }
    } else if (available != 0) {
        // Three or fewer available colours: the one take of each of them.
        TokenCounts take{};
        for (std::size_t index = 0; index < available; ++index) take[colors[index]] = 1;
        takes.push_back(take);
    }
    for (std::size_t color = 0; color < kGemColorCount; ++color) {
        if (state.bank_tokens[color] < 4) continue;
        TokenCounts take{};
        take[color] = 2;
        takes.push_back(take);
    }
}

void normal_form_takes(const State& state, std::vector<TokenDelta>& deltas) {
    deltas.clear();
    thread_local std::vector<TokenCounts> takes;
    thread_local std::vector<TokenCounts> returns;
    token_takes(state, takes);
    // A gain holds at most two of each gem colour: one base-3 digit per colour.
    constexpr std::size_t kGainCodes = 243;
    const auto code = [](const TokenCounts& gain) {
        std::size_t value = 0;
        for (std::size_t color = kGemColorCount; color-- > 0;) {
            value = 3 * value + static_cast<std::size_t>(gain[color]);
        }
        return value;
    };
    std::array<bool, kGainCodes> official{};
    std::array<bool, kGainCodes> seen{};
    for (const TokenCounts& take : takes) official[code(take)] = true;
    const PlayerState& player = state.players[state.current_player];
    int held = 0;
    for (const std::uint8_t count : player.tokens) held += count;
    for (const TokenCounts& take : takes) {
        // The take's colours and counts: three ones, fewer when fewer colours are
        // available, or one two.
        std::array<std::size_t, kGemColorCount> colors{};
        std::size_t used = 0;
        for (std::size_t color = 0; color < kGemColorCount; ++color) {
            if (take[color] != 0) colors[used++] = color;
        }
        // Every sub-multiset: an odometer over each colour's count.
        TokenCounts gain{};
        while (true) {
            const std::size_t gain_code = code(gain);
            if (!seen[gain_code]) {
                seen[gain_code] = true;
                int size = 0;
                for (std::size_t color = 0; color < kGemColorCount; ++color) size += gain[color];
                if (official[gain_code] || held + size >= 10) {
                    TokenCounts outside{};
                    std::array<int, kTokenColorCount + 1> room{};
                    for (std::size_t color = kTokenColorCount; color-- > 0;) {
                        outside[color] = gain[color] == 0 ? player.tokens[color] : 0;
                        room[color] = room[color + 1] + outside[color];
                    }
                    TokenCounts current{};
                    returns.clear();
                    append_discards(outside, room, 0, std::max(0, held + size - 10), current, returns);
                    for (const TokenCounts& given : returns) {
                        TokenDelta delta{};
                        for (std::size_t color = 0; color < kTokenColorCount; ++color) {
                            delta[color] = static_cast<std::int8_t>(gain[color] - given[color]);
                        }
                        deltas.push_back(delta);
                    }
                }
            }
            std::size_t digit = 0;
            while (digit < used && gain[colors[digit]] == take[colors[digit]]) {
                gain[colors[digit++]] = 0;
            }
            if (digit == used) break;
            ++gain[colors[digit]];
        }
    }
}

std::optional<OwedDraw> apply_primary_effect(
    State& state,
    const Catalog& catalog,
    const CandidateDescriptor& primary) {
    if (primary.chosen_noble_slot) {
        throw std::logic_error("a primary effect carries no noble");
    }
    const std::uint8_t seat = state.current_player;
    apply_token_delta(state, seat, primary.token_delta);
    PlayerState& player = state.players[seat];
    std::optional<OwedDraw> owed;
    switch (primary.family) {
        case CandidateFamily::Take:
        case CandidateFamily::PassForced:
            break;
        case CandidateFamily::ReserveVisible: {
            const std::size_t index = market_index(*primary.source_tier, *primary.source_slot);
            if (state.market_card_ids[index] != primary.target_card_id) {
                throw std::logic_error("reserve target changed");
            }
            const auto destination = first_empty_reserve(player);
            if (!destination) throw std::logic_error("reserve capacity exhausted");
            player.reserves[*destination] = Reserve{*primary.source_tier, primary.target_card_id, false};
            state.market_card_ids[index].reset();
            if (tier_counts(state, catalog).deck[*primary.source_tier - 1] != 0) {
                owed = OwedDraw{DrawKind::Refill, *primary.source_tier, *primary.source_slot, seat};
            }
            break;
        }
        case CandidateFamily::ReserveBlind: {
            // The deck must still hold a card after accounting for every concealed
            // reservation the state cannot name; the new reservation is one of them
            // until its draw resolves.
            if (tier_counts(state, catalog).deck[*primary.source_tier - 1] == 0) {
                throw std::logic_error("blind reserve of an empty deck");
            }
            const auto destination = first_empty_reserve(player);
            if (!destination) throw std::logic_error("reserve capacity exhausted");
            player.reserves[*destination] = Reserve{*primary.source_tier, std::nullopt, true};
            owed = OwedDraw{DrawKind::BlindReserve, *primary.source_tier, *destination, seat};
            break;
        }
        case CandidateFamily::BuyVisible:
        case CandidateFamily::BuyReserved: {
            apply_payment(state, seat, primary.payment);
            if (primary.family == CandidateFamily::BuyVisible) {
                const std::size_t index =
                    market_index(*primary.source_tier, *primary.source_slot);
                if (state.market_card_ids[index] != primary.target_card_id) {
                    throw std::logic_error("visible buy target changed");
                }
                state.market_card_ids[index].reset();
                if (tier_counts(state, catalog).deck[*primary.source_tier - 1] != 0) {
                    owed = OwedDraw{DrawKind::Refill, *primary.source_tier, *primary.source_slot, seat};
                }
            } else {
                if (primary.reserve_owner != seat
                    || !player.reserves[*primary.reserve_slot]
                    || player.reserves[*primary.reserve_slot]->card_id
                        != primary.target_card_id) {
                    throw std::logic_error("reserved buy target changed");
                }
                player.reserves[*primary.reserve_slot].reset();
            }
            const Card& card = catalog.card(*primary.target_card_id);
            ++player.bonuses[static_cast<std::size_t>(card.bonus)];
            player.prestige += card.prestige;
            ++player.purchased_cards;
            break;
        }
    }
    return owed;
}

void place_draw(
    State& state,
    const Catalog& catalog,
    const OwedDraw& owed,
    const std::uint16_t card_id) {
    const auto found = std::lower_bound(
        state.unknown_card_ids.begin(), state.unknown_card_ids.end(), card_id);
    if (found == state.unknown_card_ids.end() || *found != card_id
        || catalog.card(card_id).tier != owed.tier) {
        throw std::invalid_argument("card is not a draw outcome of the owed tier");
    }
    switch (owed.kind) {
        case DrawKind::Refill: {
            std::optional<std::uint16_t>& slot =
                state.market_card_ids[market_index(owed.tier, owed.slot)];
            if (slot) throw std::logic_error("owed market slot is already filled");
            slot = card_id;
            break;
        }
        case DrawKind::BlindReserve: {
            std::optional<Reserve>& reserve = state.players[owed.owner].reserves[owed.slot];
            if (!reserve || reserve->card_id || !reserve->concealed || reserve->tier != owed.tier) {
                throw std::logic_error("owed blind reservation is not awaiting its draw");
            }
            reserve->card_id = card_id;
            break;
        }
    }
    state.unknown_card_ids.erase(found);
}

std::optional<TerminalFacts> finish_turn_state(
    State& state,
    const std::uint32_t max_game_plies) {
    const std::uint8_t seat = state.current_player;
    if (!state.endgame_triggered && state.players[seat].prestige >= kTargetPrestige) {
        state.endgame_triggered = true;
        state.endgame_trigger_player = seat;
    }
    ++state.game_ply;
    state.current_player =
        static_cast<std::uint8_t>((state.starting_player + state.game_ply) % kPlayerCount);
    return terminal_facts(state, max_game_plies);
}

ValidationResult validate_resolved_draws(const State& state, const Catalog& catalog) {
    const TierCounts counts = tier_counts(state, catalog);
    for (std::size_t tier = 0; tier < kTierCount; ++tier) {
        const bool gap = std::any_of(
            state.market_card_ids.begin() + tier * kMarketSlotsPerTier,
            state.market_card_ids.begin() + (tier + 1) * kMarketSlotsPerTier,
            [](const auto& value) { return !value.has_value(); });
        if (gap && counts.deck[tier] != 0) {
            return invalid("a market slot is vacant with no pending draw");
        }
    }
    const PlayerState& actor = state.players[state.current_player];
    if (std::any_of(actor.reserves.begin(), actor.reserves.end(), [](const auto& reserve) {
            return reserve && !reserve->card_id;
        })) {
        return invalid("the actor's blind reservation awaits its draw");
    }
    return ValidationResult{true, {}};
}

}  // namespace detail

}  // namespace splendor
