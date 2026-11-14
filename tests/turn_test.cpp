#include "splendor/turn.hpp"

#include "splendor/action_tree.hpp"
#include "splendor/detail/episode.hpp"
#include "splendor/detail/turn.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using splendor::Catalog;
using splendor::PrimaryFamily;
using splendor::Decision;
using splendor::DecisionPhase;
using splendor::MainOutcome;
using splendor::State;
using splendor::TurnChoice;
using splendor::TurnStep;

constexpr std::uint32_t kMaxGamePlies = 400;
constexpr std::size_t kGoldIndex = static_cast<std::size_t>(splendor::Color::Gold);

Catalog load_catalog() {
    return Catalog::load(
        SPLENDOR_GAME_CATALOG);
}

Decision main_decision(const State& state) { return Decision{state, DecisionPhase::Main}; }

std::vector<TurnChoice> choices_of(const Decision& decision, const Catalog& catalog) {
    return splendor::decision_choices(decision, kMaxGamePlies, catalog);
}

// The turn start at `plies` completed turns of a game that prefers `preferred` at every
// MAIN and takes the first cleanup, its draws made by the episode law.
State advanced_state(
    const Catalog& catalog,
    const std::uint64_t setup_seed,
    const std::uint32_t plies,
    const PrimaryFamily preferred = PrimaryFamily::ReserveVisible) {
    Decision decision = main_decision(splendor::initial_state(catalog, 2, kMaxGamePlies, setup_seed));
    const splendor::detail::EpisodeSeeds seeds(900 + setup_seed);
    std::vector<TurnChoice> choices;
    while (decision.state.game_ply < plies) {
        splendor::detail::legal_choices(decision, catalog, choices);
        auto chosen = std::find_if(choices.begin(), choices.end(), [&](const TurnChoice& choice) {
            return choice.phase == DecisionPhase::Main && choice.primary.family == preferred;
        });
        if (chosen == choices.end()) chosen = choices.begin();
        if (splendor::detail::play_choice(decision, *chosen, seeds, kMaxGamePlies, catalog)) {
            throw std::logic_error("the fixture game ended early");
        }
    }
    return decision.state;
}

// Ten held tokens and gold in the bank: a visible reserve overflows by its gold.
State ten_token_reserve_state(const Catalog& catalog) {
    State root = splendor::initial_state(catalog, 2, kMaxGamePlies, 5);
    splendor::PlayerState& actor = root.players[root.current_player];
    for (std::size_t color = 0; color < splendor::kGemColorCount; ++color) {
        actor.tokens[color] = 2;
        root.bank_tokens[color] = 2;
    }
    return root;
}

// The actor already meets the first `eligible` visible nobles; the third noble
// is the opponent's. `purchased` deck cards leave the inventory to conserve cards.
State noble_ready_root(
    const Catalog& catalog, const std::size_t eligible, const std::uint64_t setup_seed = 5) {
    State root = splendor::initial_state(catalog, 2, kMaxGamePlies, setup_seed);
    splendor::PlayerState& actor = root.players[root.current_player];
    std::size_t purchased = 0;
    for (std::size_t color = 0; color < splendor::kGemColorCount; ++color) {
        for (std::size_t slot = 0; slot < eligible; ++slot) {
            actor.bonuses[color] = std::max(
                actor.bonuses[color], catalog.noble(*root.noble_ids[slot]).requirements[color]);
        }
        purchased += actor.bonuses[color];
    }
    actor.purchased_cards = static_cast<std::uint8_t>(purchased);
    root.unknown_card_ids.erase(
        root.unknown_card_ids.begin(),
        root.unknown_card_ids.begin() + static_cast<std::ptrdiff_t>(purchased));
    root.noble_ids[2].reset();
    root.players[1 - root.current_player].noble_count = 1;
    return root;
}

TurnChoice find_main(
    const std::vector<TurnChoice>& choices,
    const PrimaryFamily family,
    const std::function<bool(const TurnChoice&)>& accept = [](const TurnChoice&) { return true; }) {
    const auto found = std::find_if(choices.begin(), choices.end(), [&](const TurnChoice& choice) {
        return choice.primary.family == family && accept(choice);
    });
    if (found == choices.end()) throw std::logic_error("fixture lacks the primary");
    return *found;
}

// Every MAIN choice of the turn start `state`, under every lawful draw and every cleanup
// that follows, completes its turn: the cleanup support is known before the draw, an
// exposed CLEANUP is the same actor's valid decision offering exactly that support, each
// successor is a valid turn start or terminal, and terminal facts never read the draw.
// Returns the number of (choice, draw, cleanup) turns completed.
std::size_t expect_complete_turns(const State& state, const Catalog& catalog) {
    const Decision root = main_decision(state);
    std::size_t completed = 0;
    for (const TurnChoice& main : choices_of(root, catalog)) {
        const MainOutcome outcome = splendor::apply_main(root, main, kMaxGamePlies, catalog);
        EXPECT_GE(outcome.cleanup_count, 1U);
        EXPECT_EQ(outcome.draw.has_value(), !outcome.outcome_card_ids.empty());
        std::vector<std::optional<std::uint16_t>> draws(
            outcome.outcome_card_ids.begin(), outcome.outcome_card_ids.end());
        if (draws.empty()) draws.emplace_back(std::nullopt);
        // The terminal facts of each cleanup under the first draw; every draw must agree.
        std::vector<std::optional<splendor::TerminalFacts>> endings;
        for (const auto card : draws) {
            const TurnStep step = splendor::resolve_main(outcome, card, kMaxGamePlies, catalog);
            std::vector<TurnStep> finished;
            if (step.decision.phase == DecisionPhase::Cleanup) {
                EXPECT_GT(outcome.cleanup_count, 1U);
                EXPECT_EQ(step.decision.state.current_player, state.current_player);
                EXPECT_TRUE(splendor::validate_decision(step.decision, kMaxGamePlies, catalog));
                const std::vector<TurnChoice> cleanups = choices_of(step.decision, catalog);
                EXPECT_EQ(cleanups.size(), outcome.cleanup_count);
                for (const TurnChoice& cleanup : cleanups) {
                    finished.push_back(
                        splendor::apply_cleanup(step.decision, cleanup, kMaxGamePlies, catalog));
                }
            } else {
                EXPECT_EQ(outcome.cleanup_count, 1U);
                finished.push_back(step);
            }
            const bool first_draw = endings.empty();
            for (std::size_t index = 0; index < finished.size(); ++index) {
                const TurnStep& done = finished[index];
                EXPECT_EQ(done.decision.phase, DecisionPhase::Main);
                if (!done.terminal) {
                    EXPECT_NE(done.decision.state.game_ply, state.game_ply);
                    EXPECT_TRUE(
                        splendor::validate_decision(done.decision, kMaxGamePlies, catalog));
                }
                if (first_draw) {
                    endings.push_back(done.terminal);
                } else if (index < endings.size()) {
                    EXPECT_EQ(done.terminal, endings[index]) << splendor::choice_key(main);
                }
                ++completed;
            }
            EXPECT_EQ(finished.size(), endings.size());
        }
    }
    return completed;
}

TEST(StagedTurn, EveryTurnCompletesToAValidSuccessor) {
    const Catalog catalog = load_catalog();
    for (const std::uint64_t seed : {2468U, 13U, 77U, 31U}) {
        for (const std::uint32_t plies : {0U, 6U, 14U, 22U}) {
            for (const PrimaryFamily preferred :
                 {PrimaryFamily::ReserveVisible, PrimaryFamily::Take}) {
                EXPECT_GT(
                    expect_complete_turns(
                        advanced_state(catalog, seed, plies, preferred), catalog),
                    0U);
            }
        }
    }
}

TEST(StagedTurn, ReserveCommitsItsGoldAndReturnsAfterTheRefill) {
    const Catalog catalog = load_catalog();
    const State state = ten_token_reserve_state(catalog);
    const Decision root = main_decision(state);
    const std::vector<TurnChoice> mains = choices_of(root, catalog);
    const TurnChoice& reserve = find_main(mains, PrimaryFamily::ReserveVisible);
    EXPECT_EQ(reserve.primary.token_delta[kGoldIndex], 1);
    EXPECT_TRUE(splendor::choice_key(reserve).starts_with("main|"));
    const MainOutcome outcome = splendor::apply_main(root, reserve, kMaxGamePlies, catalog);
    ASSERT_TRUE(outcome.awaiting_draw());
    ASSERT_GT(outcome.outcome_card_ids.size(), 1U);
    EXPECT_EQ(outcome.cleanup_count, splendor::kTokenColorCount);
    std::set<std::vector<std::uint16_t>> decks;
    for (const std::uint16_t card : outcome.outcome_card_ids) {
        const TurnStep step = splendor::resolve_main(outcome, card, kMaxGamePlies, catalog);
        ASSERT_EQ(step.decision.phase, DecisionPhase::Cleanup);
        const State& cleanup = step.decision.state;
        // The actual refill: the card is on the market and out of the inventory together,
        // and the unclipped eleven-token holding awaits its return.
        EXPECT_EQ(splendor::token_count(cleanup.players[state.current_player]), 11U);
        EXPECT_EQ(
            cleanup.market_card_ids[splendor::detail::market_index(outcome.draw->tier, outcome.draw->slot)],
            card);
        EXPECT_EQ(cleanup.unknown_card_ids.size() + 1, state.unknown_card_ids.size());
        decks.insert(cleanup.unknown_card_ids);
        const std::vector<TurnChoice> cleanups = choices_of(step.decision, catalog);
        ASSERT_EQ(cleanups.size(), splendor::kTokenColorCount);
        std::set<std::size_t> colors;
        for (const TurnChoice& choice : cleanups) {
            EXPECT_EQ(choice.phase, DecisionPhase::Cleanup);
            EXPECT_TRUE(splendor::choice_key(choice).starts_with("cleanup|d="));
            for (std::size_t color = 0; color < splendor::kTokenColorCount; ++color) {
                if (choice.returned[color]) colors.insert(color);
            }
        }
        EXPECT_EQ(colors.size(), splendor::kTokenColorCount);
    }
    EXPECT_EQ(decks.size(), outcome.outcome_card_ids.size());

    // Without gold in the bank the same family commits no token and owes no return, so
    // the turn completes once the refill is drawn.
    State no_gold = state;
    no_gold.players[1 - no_gold.current_player].tokens[kGoldIndex] = no_gold.bank_tokens[kGoldIndex];
    no_gold.bank_tokens[kGoldIndex] = 0;
    const Decision dry = main_decision(no_gold);
    const TurnChoice& plain = find_main(choices_of(dry, catalog), PrimaryFamily::ReserveVisible);
    for (const auto value : plain.primary.token_delta) EXPECT_EQ(value, 0);
    const MainOutcome forced = splendor::apply_main(dry, plain, kMaxGamePlies, catalog);
    EXPECT_EQ(forced.cleanup_count, 1U);
    const TurnStep next = splendor::resolve_main(
        forced, forced.outcome_card_ids.front(), kMaxGamePlies, catalog);
    EXPECT_EQ(next.decision.phase, DecisionPhase::Main);
    EXPECT_NE(next.decision.state.current_player, state.current_player);
}

// Mid-turn only the actor may hold more than ten tokens: every other seat ended its turn
// within the limit, which the sure-win proof's purchase releases rely on.
TEST(StagedTurn, OnlyTheActorMayExceedTheHoldLimitMidTurn) {
    const Catalog catalog = load_catalog();
    const Decision root = main_decision(ten_token_reserve_state(catalog));
    const TurnChoice reserve = find_main(choices_of(root, catalog), PrimaryFamily::ReserveVisible);
    const MainOutcome outcome = splendor::apply_main(root, reserve, kMaxGamePlies, catalog);
    const TurnStep step =
        splendor::resolve_main(outcome, outcome.outcome_card_ids.front(), kMaxGamePlies, catalog);
    ASSERT_EQ(step.decision.phase, DecisionPhase::Cleanup);
    ASSERT_EQ(splendor::token_count(step.decision.state.players[root.state.current_player]), 11U);
    EXPECT_TRUE(splendor::validate_decision(step.decision, kMaxGamePlies, catalog));

    State bystander = step.decision.state;
    std::swap(bystander.players[0].tokens, bystander.players[1].tokens);
    const splendor::ValidationResult result = splendor::validate_state(
        bystander, kMaxGamePlies, catalog, true, splendor::StateBoundary::Intermediate);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.message, "player exceeds the token limit");
}

// Takes are their gain/give normal form: on every hand and bank pattern token conservation
// allows at two players (each gem pile empty, one to three, or four or more), the take
// choices are exactly the net deltas of an official take followed by any complete return,
// each once, with the gain and the return in different colours (a smaller gain only when
// the hand ends at ten). The maxima are the action tree's documented bounds.
TEST(StagedTurn, TakesAreTheGainGiveNormalFormOfOfficialTraces) {
    using splendor::detail::TokenCounts;
    using splendor::detail::TokenDelta;
    std::vector<TokenCounts> takes;
    std::vector<TokenCounts> discards;
    std::vector<TokenDelta> normal;
    std::set<TokenDelta> traced;
    std::size_t pairs = 0;
    std::size_t most_deltas = 0;
    std::size_t most_gains = 0;
    std::size_t most_returns = 0;
    State state;
    TokenCounts hand{};
    const std::function<void(std::size_t, int)> hands = [&](const std::size_t color, const int left) {
        if (color < splendor::kTokenColorCount) {
            const int most = color == kGoldIndex ? splendor::kGoldSupply : splendor::gem_supply(2);
            for (hand[color] = 0; hand[color] <= std::min(most, left); ++hand[color]) {
                hands(color + 1, left - hand[color]);
            }
            hand[color] = 0;
            return;
        }
        splendor::PlayerState& actor = state.players[state.current_player];
        int held = 0;
        for (std::size_t c = 0; c < splendor::kTokenColorCount; ++c) {
            actor.tokens[c] = static_cast<std::uint8_t>(hand[c]);
            held += hand[c];
        }
        // Each gem pile is empty, holds one to three, or four or more, as the hand allows.
        std::array<int, splendor::kGemColorCount> pile{};
        const std::function<void(std::size_t)> banks = [&](const std::size_t gem) {
            if (gem < splendor::kGemColorCount) {
                for (const int count : {0, 1, 4}) {
                    if (hand[gem] + count > splendor::gem_supply(2)) continue;
                    pile[gem] = count;
                    banks(gem + 1);
                }
                return;
            }
            for (std::size_t c = 0; c < splendor::kGemColorCount; ++c) {
                state.bank_tokens[c] = static_cast<std::uint8_t>(pile[c]);
            }
            ++pairs;
            traced.clear();
            splendor::detail::token_takes(state, takes);
            for (const TokenCounts& take : takes) {
                splendor::detail::discard_variants(actor, take, discards);
                for (const TokenCounts& discard : discards) {
                    TokenDelta delta{};
                    for (std::size_t c = 0; c < splendor::kTokenColorCount; ++c) {
                        delta[c] = static_cast<std::int8_t>(take[c] - discard[c]);
                    }
                    traced.insert(delta);
                }
            }
            splendor::detail::normal_form_takes(state, normal);
            const std::set<TokenDelta> unique(normal.begin(), normal.end());
            ASSERT_EQ(unique.size(), normal.size()) << "a net delta has two descriptions";
            ASSERT_EQ(unique, traced) << "held " << held;
            std::map<TokenDelta, std::size_t> returns;
            for (const TokenDelta& delta : normal) {
                TokenDelta gain{};
                int gained = 0;
                int given = 0;
                for (std::size_t c = 0; c < splendor::kTokenColorCount; ++c) {
                    gain[c] = std::max<std::int8_t>(delta[c], 0);
                    gained += gain[c];
                    given -= std::min<std::int8_t>(delta[c], 0);
                }
                ASSERT_EQ(given, std::max(0, held + gained - 10));
                ++returns[gain];
            }
            most_deltas = std::max(most_deltas, normal.size());
            most_gains = std::max(most_gains, returns.size());
            for (const auto& [gain, count] : returns) most_returns = std::max(most_returns, count);
        };
        banks(0);
    };
    hands(0, 10);
    EXPECT_EQ(pairs, 303122U);
    EXPECT_EQ(most_deltas, 168U);
    EXPECT_EQ(most_gains, 29U);
    EXPECT_EQ(most_returns, 15U);
}

// The action tree: a take's intent is its gain (the positive part of its normal-form
// delta) and its returns the parameters; a purchase's intent is its card over every legal
// payment; a reservation is its own intent. Intents are numbered contiguously by first
// appearance and never span families. The null move is the take of the empty gain, legal
// at ten held tokens. A CLEANUP decision is one family whose every choice is its own intent.
TEST(StagedTurn, ActionTreeGroupsGainsAndPurchasesByIntent) {
    using splendor::ActionFamily;
    using splendor::ActionNode;
    const Catalog catalog = load_catalog();
    State state = splendor::initial_state(catalog, 2, kMaxGamePlies, 5);
    splendor::PlayerState& actor = state.players[state.current_player];
    // Ten tokens, three of them gold: smaller gains, the null and gold substitutions.
    for (std::size_t color = 0; color < splendor::kTokenColorCount; ++color) {
        const std::uint8_t taken = color == kGoldIndex ? 3 : (color < 2 ? 2 : 1);
        actor.tokens[color] += taken;
        state.bank_tokens[color] -= taken;
    }
    const std::vector<TurnChoice> mains = choices_of(main_decision(state), catalog);
    const std::vector<ActionNode> nodes = splendor::action_tree(mains);
    ASSERT_EQ(nodes.size(), mains.size());
    std::map<std::uint16_t, ActionFamily> family_of;
    std::map<std::uint16_t, std::size_t> members;
    std::map<std::array<std::int8_t, splendor::kTokenColorCount>, std::set<std::uint16_t>> gains;
    std::map<std::uint16_t, std::set<std::uint16_t>> cards;
    std::size_t nulls = 0;
    std::uint16_t next = 0;
    for (std::size_t index = 0; index < mains.size(); ++index) {
        const ActionNode& node = nodes[index];
        const splendor::Primary& primary = mains[index].primary;
        ASSERT_LE(node.intent, next);
        if (node.intent == next) ++next;
        ++members[node.intent];
        const auto [known, fresh] = family_of.emplace(node.intent, node.family);
        EXPECT_TRUE(fresh || known->second == node.family) << splendor::choice_key(mains[index]);
        EXPECT_EQ(node.null, splendor::is_null_take(primary));
        nulls += node.null;
        if (primary.family == PrimaryFamily::Take) {
            EXPECT_EQ(node.family, ActionFamily::Take);
            std::array<std::int8_t, splendor::kTokenColorCount> gain{};
            for (std::size_t color = 0; color < splendor::kTokenColorCount; ++color) {
                gain[color] = std::max<std::int8_t>(primary.token_delta[color], 0);
            }
            gains[gain].insert(node.intent);
        } else if (primary.family == PrimaryFamily::BuyVisible
                   || primary.family == PrimaryFamily::BuyReserved) {
            EXPECT_EQ(node.family, ActionFamily::Buy);
            cards[*primary.target_card_id].insert(node.intent);
        } else {
            EXPECT_EQ(node.family, ActionFamily::Reserve);
        }
    }
    EXPECT_EQ(nulls, 1U);
    std::set<std::uint16_t> shared;
    std::size_t multi_return_gains = 0;
    for (const auto& [gain, intents] : gains) {
        ASSERT_EQ(intents.size(), 1U) << "a gain spans several intents";
        EXPECT_TRUE(shared.insert(*intents.begin()).second);
        multi_return_gains += members[*intents.begin()] > 1;
    }
    std::size_t multi_payment_cards = 0;
    for (const auto& [card, intents] : cards) {
        ASSERT_EQ(intents.size(), 1U) << "card " << card << " spans several intents";
        EXPECT_TRUE(shared.insert(*intents.begin()).second) << "card " << card;
        multi_payment_cards += members[*intents.begin()] > 1;
    }
    for (const auto& [intent, family] : family_of) {
        if (family == ActionFamily::Reserve) EXPECT_EQ(members[intent], 1U);
    }
    ASSERT_GE(multi_return_gains, 1U) << "fixture offers no gain with several returns";
    ASSERT_GE(multi_payment_cards, 1U) << "fixture offers no multi-payment purchase";

    // A joint cleanup (returns × nobles): one family, every choice its own intent.
    State overflow = noble_ready_root(catalog, 2);
    for (std::size_t color = 0; color < splendor::kGemColorCount; ++color) {
        overflow.players[overflow.current_player].tokens[color] = 2;
        overflow.bank_tokens[color] = 2;
    }
    const Decision root = main_decision(overflow);
    const MainOutcome outcome = splendor::apply_main(
        root, find_main(choices_of(root, catalog), PrimaryFamily::ReserveVisible),
        kMaxGamePlies, catalog);
    const TurnStep step = splendor::resolve_main(
        outcome, outcome.outcome_card_ids.front(), kMaxGamePlies, catalog);
    const std::vector<TurnChoice> cleanups = choices_of(step.decision, catalog);
    ASSERT_GT(cleanups.size(), 1U);
    const std::vector<ActionNode> cleanup_nodes = splendor::action_tree(cleanups);
    for (std::size_t index = 0; index < cleanups.size(); ++index) {
        EXPECT_EQ(cleanup_nodes[index], (ActionNode{ActionFamily::Cleanup,
            static_cast<std::uint16_t>(index), false}));
    }
}

TEST(StagedTurn, TierCardCountChangesTheDrawNotThePrimary) {
    const Catalog catalog = load_catalog();
    const State base = ten_token_reserve_state(catalog);
    const auto tier_one = [](const TurnChoice& choice) {
        return choice.primary.source_tier == std::optional<std::uint8_t>{1};
    };
    const TurnChoice full = find_main(
        choices_of(main_decision(base), catalog), PrimaryFamily::ReserveVisible, tier_one);
    for (const std::size_t kept : {1U, 0U}) {
        // Tier-one inventory beyond `kept` becomes the opponent's purchases.
        State state = base;
        std::vector<std::uint16_t> tier_one_deck;
        for (const std::uint16_t id : state.unknown_card_ids) {
            if (catalog.card(id).tier == 1) tier_one_deck.push_back(id);
        }
        const std::size_t removed = tier_one_deck.size() - kept;
        for (std::size_t index = 0; index < removed; ++index) {
            state.unknown_card_ids.erase(std::find(
                state.unknown_card_ids.begin(), state.unknown_card_ids.end(), tier_one_deck[index]));
        }
        state.players[1 - state.current_player].purchased_cards = static_cast<std::uint8_t>(removed);
        state.players[1 - state.current_player].bonuses[0] = static_cast<std::uint8_t>(removed);
        const Decision root = main_decision(state);
        const TurnChoice& reserve =
            find_main(choices_of(root, catalog), PrimaryFamily::ReserveVisible, tier_one);
        EXPECT_EQ(reserve.primary, full.primary);
        const MainOutcome outcome = splendor::apply_main(root, reserve, kMaxGamePlies, catalog);
        EXPECT_EQ(outcome.outcome_card_ids.size(), kept);
        EXPECT_EQ(outcome.awaiting_draw(), kept != 0);
        // The one-card and the empty tier still leave the return discretionary.
        EXPECT_EQ(outcome.cleanup_count, splendor::kTokenColorCount);
        const std::optional<std::uint16_t> draw =
            kept ? std::optional<std::uint16_t>(outcome.outcome_card_ids.front()) : std::nullopt;
        EXPECT_EQ(
            splendor::resolve_main(outcome, draw, kMaxGamePlies, catalog).decision.phase,
            DecisionPhase::Cleanup);
        EXPECT_GT(expect_complete_turns(state, catalog), 0U);
    }
}

TEST(StagedTurn, MainExposesTheEligibleNobleSetAtEveryCardinality) {
    const Catalog catalog = load_catalog();
    for (const std::size_t eligible : {0U, 1U, 2U}) {
        const State state = noble_ready_root(catalog, eligible);
        const Decision root = main_decision(state);
        const std::vector<TurnChoice> mains = choices_of(root, catalog);
        // Carried-over eligibility: TAKE and RESERVE are not noble-free.
        for (const PrimaryFamily family :
             {PrimaryFamily::Take, PrimaryFamily::ReserveVisible}) {
            const TurnChoice& main = find_main(mains, family);
            EXPECT_EQ(static_cast<std::size_t>(std::popcount(main.eligible_nobles)), eligible);
        }
        EXPECT_GT(expect_complete_turns(state, catalog), 0U);
    }
}

TEST(StagedTurn, JointCleanupIsTheProductOfReturnsAndNobles) {
    const Catalog catalog = load_catalog();
    State state = noble_ready_root(catalog, 2);
    splendor::PlayerState& actor = state.players[state.current_player];
    for (std::size_t color = 0; color < splendor::kGemColorCount; ++color) {
        actor.tokens[color] = 2;
        state.bank_tokens[color] = 2;
    }
    const Decision root = main_decision(state);
    const TurnChoice& reserve = find_main(choices_of(root, catalog), PrimaryFamily::ReserveVisible);
    const MainOutcome outcome = splendor::apply_main(root, reserve, kMaxGamePlies, catalog);
    EXPECT_EQ(outcome.cleanup_count, splendor::kTokenColorCount * 2);
    const TurnStep step = splendor::resolve_main(
        outcome, outcome.outcome_card_ids.front(), kMaxGamePlies, catalog);
    const std::vector<TurnChoice> cleanups = choices_of(step.decision, catalog);
    std::set<std::optional<std::uint8_t>> nobles;
    for (const TurnChoice& cleanup : cleanups) nobles.insert(cleanup.noble_slot);
    EXPECT_EQ(nobles.size(), 2U);
    EXPECT_FALSE(nobles.contains(std::nullopt));
    // Return first, then the noble: one completed turn claims exactly one noble.
    const TurnStep done = splendor::apply_cleanup(step.decision, cleanups.back(), kMaxGamePlies, catalog);
    EXPECT_EQ(done.decision.state.players[state.current_player].noble_count, 1U);
    EXPECT_EQ(splendor::token_count(done.decision.state.players[state.current_player]), 10U);
    EXPECT_GT(expect_complete_turns(state, catalog), 0U);
}

// Every gem is held, all three reserves are full and nothing is affordable, so the
// only primary is the forced pass; two carried-over nobles still leave a discretionary
// cleanup, which an episode exposes after playing the pass automatically.
TEST(StagedTurn, ForcedMainIsFollowedByADiscretionaryCleanup) {
    const Catalog catalog = load_catalog();
    // A setup whose decks keep an unaffordable card of every tier for the swaps below.
    State state = noble_ready_root(catalog, 2, 1);
    splendor::PlayerState& actor = state.players[state.current_player];
    splendor::PlayerState& opponent = state.players[1 - state.current_player];
    actor.tokens = {4, 4, 2, 0, 0, 0};
    opponent.tokens = {0, 0, 2, 4, 4, 0};
    state.bank_tokens = {0, 0, 0, 0, 0, 5};
    const auto affordable = [&](const std::uint16_t id) {
        const splendor::Card& card = catalog.card(id);
        for (std::size_t color = 0; color < splendor::kGemColorCount; ++color) {
            if (card.cost[color] > actor.tokens[color] + actor.bonuses[color]) return false;
        }
        return true;
    };
    for (auto& slot : state.market_card_ids) {
        if (!slot || !affordable(*slot)) continue;
        const std::uint8_t tier = catalog.card(*slot).tier;
        const auto replacement = std::find_if(
            state.unknown_card_ids.begin(), state.unknown_card_ids.end(),
            [&](const std::uint16_t id) { return catalog.card(id).tier == tier && !affordable(id); });
        ASSERT_NE(replacement, state.unknown_card_ids.end());
        const std::uint16_t swapped = *slot;
        slot = *replacement;
        state.unknown_card_ids.erase(replacement);
        state.unknown_card_ids.insert(
            std::lower_bound(state.unknown_card_ids.begin(), state.unknown_card_ids.end(), swapped),
            swapped);
    }
    for (auto& reserved : actor.reserves) {
        const auto card = std::find_if(
            state.unknown_card_ids.begin(), state.unknown_card_ids.end(),
            [&](const std::uint16_t id) { return !affordable(id); });
        ASSERT_NE(card, state.unknown_card_ids.end());
        reserved = splendor::Reserve{catalog.card(*card).tier, *card, false};
        state.unknown_card_ids.erase(card);
    }
    ASSERT_TRUE(splendor::validate_state(state, kMaxGamePlies, catalog));
    const std::vector<TurnChoice> mains = choices_of(main_decision(state), catalog);
    ASSERT_EQ(mains.size(), 1U);
    EXPECT_EQ(mains.front().primary.family, PrimaryFamily::PassForced);

    Decision decision = main_decision(state);
    std::vector<TurnChoice> choices;
    const splendor::detail::EpisodeSeeds seeds(11);
    EXPECT_FALSE(splendor::detail::play_forced(decision, choices, seeds, kMaxGamePlies, catalog));
    EXPECT_EQ(decision.phase, DecisionPhase::Cleanup);
    EXPECT_EQ(choices.size(), 2U);
    EXPECT_GT(expect_complete_turns(state, catalog), 0U);
}

TEST(StagedTurn, ForeignAndStaleChoicesAreRejected) {
    const Catalog catalog = load_catalog();
    const State state = ten_token_reserve_state(catalog);
    const Decision root = main_decision(state);
    const std::vector<TurnChoice> mains = choices_of(root, catalog);
    const TurnChoice& reserve = find_main(mains, PrimaryFamily::ReserveVisible);
    const TurnChoice& take = find_main(mains, PrimaryFamily::Take);
    const MainOutcome outcome = splendor::apply_main(root, reserve, kMaxGamePlies, catalog);
    const TurnStep step = splendor::resolve_main(
        outcome, outcome.outcome_card_ids.front(), kMaxGamePlies, catalog);
    const std::vector<TurnChoice> cleanups = choices_of(step.decision, catalog);
    // Keys of another phase, a cleanup applied at MAIN, a primary applied at CLEANUP.
    EXPECT_THROW(splendor::find_choice(mains, splendor::choice_key(cleanups.front())), std::invalid_argument);
    EXPECT_THROW(splendor::find_choice(cleanups, splendor::choice_key(take)), std::invalid_argument);
    EXPECT_THROW(
        splendor::apply_main(root, cleanups.front(), kMaxGamePlies, catalog), std::invalid_argument);
    EXPECT_THROW(
        splendor::apply_cleanup(step.decision, take, kMaxGamePlies, catalog), std::invalid_argument);
    // A draw from another tier, a missing draw, and a draw where none is owed.
    std::uint16_t foreign = 0;
    for (const std::uint16_t id : state.unknown_card_ids) {
        if (catalog.card(id).tier != outcome.draw->tier) foreign = id;
    }
    EXPECT_THROW(splendor::resolve_main(outcome, foreign, kMaxGamePlies, catalog), std::invalid_argument);
    EXPECT_THROW(
        splendor::resolve_main(outcome, std::nullopt, kMaxGamePlies, catalog), std::invalid_argument);
    const MainOutcome taken = splendor::apply_main(root, take, kMaxGamePlies, catalog);
    EXPECT_THROW(
        splendor::resolve_main(taken, outcome.outcome_card_ids.front(), kMaxGamePlies, catalog),
        std::invalid_argument);
    // A CLEANUP decision owes no draw and offers a real choice.
    EXPECT_FALSE(splendor::validate_decision(Decision{outcome.state, DecisionPhase::Cleanup}, kMaxGamePlies, catalog));
    EXPECT_FALSE(splendor::validate_decision(Decision{state, DecisionPhase::Cleanup}, kMaxGamePlies, catalog));
}

// The closing seat's turn after the target was reached ends the game; the noble
// choice still moves prestige, so the turn exposes it after the refill and only the
// chosen cleanup adjudicates the game.
TEST(StagedTurn, EndingTurnKeepsItsDiscretionaryCleanup) {
    const Catalog catalog = load_catalog();
    State state = noble_ready_root(catalog, 2);
    state.game_ply = 1;
    state.current_player = static_cast<std::uint8_t>(1 - state.starting_player);
    std::swap(state.players[0], state.players[1]);
    state.endgame_triggered = true;
    state.endgame_trigger_player = state.starting_player;
    state.players[state.starting_player].prestige = 15;
    ASSERT_TRUE(splendor::validate_state(state, kMaxGamePlies, catalog));
    const Decision root = main_decision(state);
    const TurnChoice& reserve = find_main(choices_of(root, catalog), PrimaryFamily::ReserveVisible);
    const MainOutcome outcome = splendor::apply_main(root, reserve, kMaxGamePlies, catalog);
    EXPECT_FALSE(outcome.terminal.has_value());
    EXPECT_EQ(outcome.cleanup_count, 2U);
    const TurnStep step = splendor::resolve_main(
        outcome, outcome.outcome_card_ids.front(), kMaxGamePlies, catalog);
    ASSERT_EQ(step.decision.phase, DecisionPhase::Cleanup);
    for (const TurnChoice& cleanup : choices_of(step.decision, catalog)) {
        const TurnStep ended = splendor::apply_cleanup(step.decision, cleanup, kMaxGamePlies, catalog);
        ASSERT_TRUE(ended.terminal.has_value());
        EXPECT_EQ(ended.terminal->reason, splendor::TerminalReason::TargetScoreEqualTurns);
    }
    // A forced ending turn is adjudicated without drawing its refill.
    const TurnChoice& take = find_main(choices_of(root, catalog), PrimaryFamily::Take);
    State lone = state;
    lone.noble_ids[1].reset();
    lone.players[1 - lone.current_player].noble_count = 2;
    const Decision single = main_decision(lone);
    const MainOutcome forced = splendor::apply_main(
        single, splendor::find_choice(choices_of(single, catalog), splendor::choice_key(take)), kMaxGamePlies,
        catalog);
    EXPECT_TRUE(forced.terminal.has_value());
    EXPECT_FALSE(forced.awaiting_draw());
    EXPECT_GT(expect_complete_turns(state, catalog), 0U);
}

// A closing seat's blind reservation whose only cleanup ends the game still draws its
// card: the final world names it, so the owner can observe the final board, while a
// public refill of an ending turn stays undrawn. Both the equal-turn ending and the cap.
TEST(StagedTurn, EndingBlindReservationStillDrawsForItsOwner) {
    const Catalog catalog = load_catalog();
    State closing = splendor::initial_state(catalog, 2, kMaxGamePlies, 11);
    closing.game_ply = 1;
    closing.current_player = static_cast<std::uint8_t>(1 - closing.starting_player);
    closing.endgame_triggered = true;
    closing.endgame_trigger_player = closing.starting_player;
    closing.players[closing.starting_player].prestige = 15;
    State capped = splendor::initial_state(catalog, 2, 2, 11);
    capped.game_ply = 1;
    capped.current_player = static_cast<std::uint8_t>(1 - capped.starting_player);
    for (const auto& [state, plies] : {std::pair{closing, kMaxGamePlies}, std::pair{capped, 2U}}) {
        ASSERT_TRUE(splendor::validate_state(state, plies, catalog));
        const std::uint8_t owner = state.current_player;
        const Decision root = main_decision(state);
        const std::vector<TurnChoice> mains = splendor::decision_choices(root, plies, catalog);
        const TurnChoice& blind = find_main(mains, PrimaryFamily::ReserveBlind);
        const MainOutcome outcome = splendor::apply_main(root, blind, plies, catalog);
        ASSERT_TRUE(outcome.terminal.has_value());
        ASSERT_TRUE(outcome.awaiting_draw()) << "the ending turn still owes the private draw";
        EXPECT_EQ(outcome.draw->kind, splendor::DrawKind::BlindReserve);
        EXPECT_EQ(outcome.draw->owner, owner);
        const std::uint16_t card = outcome.outcome_card_ids.back();
        const TurnStep ended = splendor::resolve_main(outcome, card, plies, catalog);
        ASSERT_TRUE(ended.terminal.has_value());
        EXPECT_EQ(ended.terminal, outcome.terminal);
        const State& final_state = ended.decision.state;
        EXPECT_TRUE(splendor::omniscient(final_state));
        EXPECT_EQ(
            *final_state.players[owner].reserves[outcome.draw->slot],
            (splendor::Reserve{outcome.draw->tier, card, true}));
        // Both seats observe the final board; the owner alone sees the card.
        EXPECT_EQ(splendor::observe(final_state, owner).players[owner].reserves[0]->card_id, card);
        const State other = splendor::observe(final_state, static_cast<std::uint8_t>(1 - owner));
        EXPECT_FALSE(other.players[owner].reserves[0]->card_id.has_value());

        // The ending turn's public refill is not drawn.
        const TurnChoice& visible = find_main(mains, PrimaryFamily::ReserveVisible);
        const MainOutcome refill = splendor::apply_main(root, visible, plies, catalog);
        ASSERT_TRUE(refill.terminal.has_value());
        EXPECT_FALSE(refill.awaiting_draw());
        EXPECT_TRUE(splendor::omniscient(refill.state));
    }

    // The environment draws it too: the episode's final state is complete.
    Decision decision = main_decision(closing);
    const TurnChoice& blind = find_main(choices_of(decision, catalog), PrimaryFamily::ReserveBlind);
    const auto terminal = splendor::detail::play_choice(
        decision, blind, splendor::detail::EpisodeSeeds(5), kMaxGamePlies, catalog);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_TRUE(splendor::omniscient(decision.state));
    EXPECT_TRUE(splendor::knows_own_reserves(decision.state, closing.current_player));
}

// Whole games through the staged turn to their end: every decision is valid, every
// choice is legal, and every turn of every turn start completes to a valid successor.
TEST(StagedTurn, WholeGamesPlayToTerminalThroughStagedDecisions) {
    const Catalog catalog = load_catalog();
    std::size_t target_endings = 0;
    std::size_t cleanup_decisions = 0;
    for (const std::uint64_t seed : {3U, 17U, 29U, 41U}) {
        Decision decision = main_decision(splendor::initial_state(catalog, 2, kMaxGamePlies, seed));
        const splendor::detail::EpisodeSeeds seeds(seed);
        std::optional<splendor::TerminalFacts> terminal;
        for (std::uint32_t step = 0; step < 4 * kMaxGamePlies && !terminal; ++step) {
            ASSERT_TRUE(splendor::validate_decision(decision, kMaxGamePlies, catalog));
            if (decision.phase == DecisionPhase::Main) {
                EXPECT_GT(expect_complete_turns(decision.state, catalog), 0U);
            } else {
                ++cleanup_decisions;
            }
            const std::vector<TurnChoice> choices = choices_of(decision, catalog);
            ASSERT_FALSE(choices.empty());
            // Generation orders choices without formatting keys; the published keys
            // must still be strictly ascending bytes.
            for (std::size_t row = 1; row < choices.size(); ++row) {
                EXPECT_LT(splendor::choice_key(choices[row - 1]), splendor::choice_key(choices[row]));
            }
            // Prefer the most expensive purchase so games reach the target, then a blind
            // reservation so games draw privately and clean up after the reveal.
            const auto chosen = std::max_element(
                choices.begin(), choices.end(), [&](const TurnChoice& lhs, const TurnChoice& rhs) {
                    const auto score = [&](const TurnChoice& choice) {
                        const splendor::Primary& row = choice.primary;
                        const bool purchase = choice.phase == DecisionPhase::Main
                            && (row.family == PrimaryFamily::BuyVisible
                                || row.family == PrimaryFamily::BuyReserved);
                        if (purchase) return 100 + catalog.card(*row.target_card_id).prestige;
                        if (row.family == PrimaryFamily::ReserveBlind) return 3;
                        return row.family == PrimaryFamily::ReserveVisible ? 2 : 0;
                    };
                    return score(lhs) < score(rhs);
                });
            terminal = splendor::detail::play_choice(decision, *chosen, seeds, kMaxGamePlies, catalog);
        }
        ASSERT_TRUE(terminal.has_value());
        // Transitions do not rescan their outputs; the games check every decision above
        // and the final state here, an intermediate one when the ending turn left its
        // refill undrawn.
        const splendor::ValidationResult final_state = splendor::validate_state(
            decision.state, kMaxGamePlies, catalog, false, splendor::StateBoundary::Intermediate);
        EXPECT_TRUE(final_state) << final_state.message;
        target_endings += terminal->reason == splendor::TerminalReason::TargetScoreEqualTurns;
    }
    EXPECT_EQ(target_endings, 4U);
    EXPECT_GT(cleanup_decisions, 0U);
}

// A blind reservation owes a draw from the tier's unknown cards, each equiprobable;
// the drawn card reaches the owner's slot concealed, the owner sees it before the
// cleanup and any other seat sees only the tier, with the card still unknown to it.
TEST(StagedTurn, BlindReserveDrawsPrivatelyAndRevealsToItsOwnerOnly) {
    const Catalog catalog = load_catalog();
    const State state = ten_token_reserve_state(catalog);
    const std::uint8_t owner = state.current_player;
    const Decision root = main_decision(state);
    const std::vector<TurnChoice> mains = choices_of(root, catalog);
    for (std::uint8_t tier = 1; tier <= 3; ++tier) {
        const TurnChoice& blind = find_main(
            mains, PrimaryFamily::ReserveBlind,
            [&](const TurnChoice& choice) { return choice.primary.source_tier == tier; });
        EXPECT_EQ(blind.primary.token_delta[kGoldIndex], 1);
        EXPECT_FALSE(blind.primary.target_card_id.has_value());
        const MainOutcome outcome = splendor::apply_main(root, blind, kMaxGamePlies, catalog);
        ASSERT_TRUE(outcome.awaiting_draw());
        EXPECT_EQ(outcome.draw->kind, splendor::DrawKind::BlindReserve);
        EXPECT_EQ(outcome.draw->tier, tier);
        // The law: exactly the tier's unknown cards, as the complete state knows them.
        std::vector<std::uint16_t> expected;
        for (const std::uint16_t card : state.unknown_card_ids) {
            if (catalog.card(card).tier == tier) expected.push_back(card);
        }
        EXPECT_EQ(outcome.outcome_card_ids, expected);
        EXPECT_EQ(expected.size(), splendor::tier_counts(state, catalog).deck[tier - 1]);
        EXPECT_EQ(outcome.cleanup_count, splendor::kTokenColorCount) << "the gold overflows";
        // Before the draw the owed slot holds the tier and no identity.
        const splendor::PlayerState& pending = outcome.state.players[owner];
        ASSERT_TRUE(pending.reserves[outcome.draw->slot].has_value());
        EXPECT_FALSE(pending.reserves[outcome.draw->slot]->card_id.has_value());
        EXPECT_TRUE(pending.reserves[outcome.draw->slot]->concealed);

        const std::uint16_t card = expected[expected.size() / 2];
        const TurnStep step = splendor::resolve_main(outcome, card, kMaxGamePlies, catalog);
        ASSERT_EQ(step.decision.phase, DecisionPhase::Cleanup);
        const State& drawn = step.decision.state;
        const splendor::Reserve held = *drawn.players[owner].reserves[outcome.draw->slot];
        EXPECT_EQ(held, (splendor::Reserve{tier, card, true}));
        EXPECT_EQ(drawn.market_card_ids, state.market_card_ids) << "no market refill";
        EXPECT_EQ(drawn.unknown_card_ids.size() + 1, state.unknown_card_ids.size());
        EXPECT_EQ(
            std::find(drawn.unknown_card_ids.begin(), drawn.unknown_card_ids.end(), card),
            drawn.unknown_card_ids.end());

        // The owner's cleanup decision carries the identity; every other seat's
        // observation of the same decision keeps the card among its unknowns.
        const splendor::PlayerView own = splendor::observe_actor(step.decision);
        EXPECT_EQ(own.state().players[owner].reserves[outcome.draw->slot]->card_id, card);
        EXPECT_TRUE(splendor::knows_own_reserves(own.state(), owner));
        const State other = splendor::observe(drawn, static_cast<std::uint8_t>(1 - owner));
        const splendor::Reserve seen = *other.players[owner].reserves[outcome.draw->slot];
        EXPECT_EQ(seen, (splendor::Reserve{tier, std::nullopt, true}));
        EXPECT_NE(
            std::find(other.unknown_card_ids.begin(), other.unknown_card_ids.end(), card),
            other.unknown_card_ids.end());
        EXPECT_EQ(other.unknown_card_ids.size(), state.unknown_card_ids.size());
        EXPECT_FALSE(splendor::omniscient(other));
        // The owner's cleanup choices are the six returns, legal under its own view.
        const std::vector<TurnChoice> cleanups = choices_of(step.decision, catalog);
        EXPECT_EQ(cleanups.size(), splendor::kTokenColorCount);
        EXPECT_TRUE(splendor::validate_decision(step.decision, kMaxGamePlies, catalog));
    }

    // A seat's own view draws from its marginal law: the opponent's concealed card is a
    // possible outcome of the actor's blind draw, while the actor's own concealed card is not.
    State world = state;
    const std::uint8_t other = static_cast<std::uint8_t>(1 - owner);
    const std::uint16_t theirs = world.unknown_card_ids.front();
    const std::uint16_t mine = world.unknown_card_ids.back();
    world.players[other].reserves[0] = splendor::Reserve{catalog.card(theirs).tier, theirs, true};
    world.players[owner].reserves[0] = splendor::Reserve{catalog.card(mine).tier, mine, true};
    std::erase(world.unknown_card_ids, theirs);
    std::erase(world.unknown_card_ids, mine);
    const Decision viewed{splendor::observe(world, owner), DecisionPhase::Main};
    const TurnChoice& blind = find_main(
        choices_of(viewed, catalog), PrimaryFamily::ReserveBlind,
        [&](const TurnChoice& choice) { return choice.primary.source_tier == catalog.card(theirs).tier; });
    const MainOutcome marginal = splendor::apply_main(viewed, blind, kMaxGamePlies, catalog);
    const auto& law = marginal.outcome_card_ids;
    EXPECT_NE(std::find(law.begin(), law.end(), theirs), law.end());
    EXPECT_EQ(std::find(law.begin(), law.end(), mine), law.end());
    EXPECT_EQ(
        law.size(), splendor::tier_counts(world, catalog).deck[catalog.card(theirs).tier - 1] + 1);
}

// Two worlds differing only in a card another seat conceals are one and the same to the
// seat that cannot see it: its observation and its legal choices agree.
TEST(StagedTurn, HiddenWorldSwapLeavesTheOtherSeatsDecisionUnchanged) {
    const Catalog catalog = load_catalog();
    State left = advanced_state(catalog, 23, 6, PrimaryFamily::Take);
    const std::uint8_t actor = left.current_player;
    const std::uint8_t other = static_cast<std::uint8_t>(1 - actor);
    // Same tier, two identities: the only difference between the worlds.
    const std::uint16_t first = left.unknown_card_ids[0];
    const auto same_tier = std::find_if(
        left.unknown_card_ids.begin() + 1, left.unknown_card_ids.end(),
        [&](const std::uint16_t card) { return catalog.card(card).tier == catalog.card(first).tier; });
    ASSERT_NE(same_tier, left.unknown_card_ids.end());
    const std::uint16_t second = *same_tier;
    State right = left;
    left.players[other].reserves[0] = splendor::Reserve{catalog.card(first).tier, first, true};
    std::erase(left.unknown_card_ids, first);
    right.players[other].reserves[0] = splendor::Reserve{catalog.card(second).tier, second, true};
    std::erase(right.unknown_card_ids, second);
    ASSERT_NE(left, right);

    EXPECT_EQ(splendor::observe(left, actor), splendor::observe(right, actor));
    const Decision left_decision{left, DecisionPhase::Main};
    const Decision right_decision{right, DecisionPhase::Main};
    const splendor::PlayerView left_view = splendor::observe(left_decision, actor);
    const splendor::PlayerView right_view = splendor::observe(right_decision, actor);
    const std::vector<TurnChoice> left_choices = choices_of(left_view.decision(), catalog);
    const std::vector<TurnChoice> right_choices = choices_of(right_view.decision(), catalog);
    ASSERT_EQ(left_choices.size(), right_choices.size());
    for (std::size_t row = 0; row < left_choices.size(); ++row) {
        EXPECT_EQ(left_choices[row], right_choices[row]);
    }
    // The concealing seat's own views differ, by exactly that card.
    EXPECT_NE(splendor::observe(left, other), splendor::observe(right, other));

    // Drivers apply choices generated from the actor's view to the complete world, so
    // the two choice sets agree, for the seat that conceals and for the seat that cannot see.
    for (State world : {left, right}) {
        for (const std::uint8_t seat : {actor, other}) {
            world.current_player = seat;
            world.game_ply = seat == actor ? left.game_ply : left.game_ply + 1;
            const Decision truth{world, DecisionPhase::Main};
            const std::vector<TurnChoice> from_world = choices_of(truth, catalog);
            const std::vector<TurnChoice> from_view =
                choices_of(splendor::observe_actor(truth).decision(), catalog);
            ASSERT_EQ(from_world.size(), from_view.size());
            for (std::size_t row = 0; row < from_world.size(); ++row) {
                EXPECT_EQ(from_world[row], from_view[row]);
            }
        }
    }
}

// `reveal` keeps the other seat's concealed identity: the world is the critic's view. A
// view of a decision whose knowledge is not omniscient cannot be revealed.
TEST(StagedTurn, RevealKeepsTheWorldAndRefusesAProjection) {
    const Catalog catalog = load_catalog();
    State left = advanced_state(catalog, 23, 6, PrimaryFamily::Take);
    const std::uint8_t actor = left.current_player;
    const std::uint8_t other = static_cast<std::uint8_t>(1 - actor);
    const std::uint16_t first = left.unknown_card_ids[0];
    left.players[other].reserves[0] = splendor::Reserve{catalog.card(first).tier, first, true};
    std::erase(left.unknown_card_ids, first);
    const Decision left_decision{left, DecisionPhase::Main};

    const splendor::PlayerView revealed = splendor::reveal(left_decision);
    EXPECT_TRUE(revealed.acting());
    EXPECT_EQ(revealed.state(), left) << "no projection: the world is the view";
    const splendor::PlayerView seen = splendor::observe(left_decision, actor);
    EXPECT_FALSE(seen.state().players[other].reserves[0]->card_id.has_value());
    EXPECT_EQ(
        choices_of(seen.decision(), catalog), choices_of(revealed.decision(), catalog))
        << "the decision is still the actor's";

    // A projected decision has lost knowledge that `reveal` cannot manufacture.
    const Decision projected{splendor::observe(left, actor), DecisionPhase::Main};
    EXPECT_THROW(splendor::reveal(projected), std::invalid_argument);
    EXPECT_THROW(splendor::reveal(Decision{projected}), std::invalid_argument);
}

}  // namespace
