#include "splendor/play/match.hpp"
#include "splendor/play/render.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>

namespace {

using namespace splendor;
using namespace splendor::play;

const Catalog& catalog() {
    static const Catalog loaded = Catalog::load(SPLENDOR_GAME_CATALOG);
    return loaded;
}

MatchHost host(const std::uint32_t max_game_plies = 160) {
    return MatchHost{"test-match-log", max_game_plies, Json::parse(R"({"search":"scripted"})"),
                     {Actor{"A", "a-id", Json::parse(R"({"label":"A"})")},
                      Actor{"B", "b-id", Json::parse(R"({"label":"B"})")}}};
}

Json agent(const std::string& name, const bool ponder = false) {
    return Json(Json::Object{{"kind", "agent"}, {"actor", name}, {"ponder", ponder}});
}

Json human() { return Json::parse(R"({"kind":"human"})"); }

MatchSettings settings(Json::Array seats, const std::string& time_control = R"({"kind":"turn","human_seconds":60,"agent_seconds":5,"timeout":"flag"})",
                       const bool show_analysis = true) {
    return parse_settings(Json(Json::Object{{"seats", Json(std::move(seats))},
                                            {"time_control", Json::parse(time_control)},
                                            {"show_analysis", show_analysis}, {"seed", "1234"}}),
                          host());
}

Json state(Match& match, const std::uint8_t perspective, const double now) {
    return Json::parse(match.state(perspective, std::nullopt, now));
}

// Plays the pending agent move with its first legal choice, as a search would select.
void play_agent_first(Match& match, const double now, const std::string& report = R"({"note":1})") {
    const std::optional<Job> job = match.job();
    ASSERT_TRUE(job && job->kind == JobKind::Move);
    match.play_agent(job->id, 0, report, now);
}

TEST(JsonValue, ParsesAndDumpsExactly) {
    const Json value = Json::parse(R"( {"a":[1,-2,18446744073709551615,0.25,true,null],"s":"q\"\u00e9\n"} )");
    EXPECT_EQ(value.at("a").as_array("a")[2].as_uint("big"), 18446744073709551615ULL);
    EXPECT_EQ(value.at("s").as_string("s"), "q\"\xc3\xa9\n");
    EXPECT_EQ(Json::parse(value.dump()), value);
    EXPECT_EQ(Json::parse(R"({"x":1,"y":2})"), Json::parse(R"({"y":2.0,"x":1})"));
    EXPECT_THROW(Json::parse(R"({"a":1,"a":2})"), std::invalid_argument);
    EXPECT_THROW(Json::parse("[1] 2"), std::invalid_argument);
    EXPECT_THROW(Json::parse("01"), std::invalid_argument);
    EXPECT_THROW(Json(-1).as_uint("x"), std::invalid_argument);
}

TEST(MatchHost, SettingsSeatAnyKindsAtEverySupportedCount) {
    EXPECT_EQ(settings({human(), agent("A")}).seats[0].name, "Human");
    const MatchSettings two_humans = settings({human(), human()});
    EXPECT_EQ(two_humans.seats[1].name, "Human 2");
    const MatchSettings same_actor = settings({agent("A"), human(), agent("A"), agent("B")});
    std::vector<std::string> names;
    for (const Seat& seat : same_actor.seats) names.push_back(seat.name);
    EXPECT_EQ(names, (std::vector<std::string>{"A #1", "Human", "A #2", "B"}));
    EXPECT_EQ(settings({agent("A"), agent("B"), human(), human()}).seats.size(), 4U);
    EXPECT_THROW(settings({human()}), std::invalid_argument);
    EXPECT_THROW(settings({human(), Json::parse(R"({"kind":"robot"})")}), std::invalid_argument);
    EXPECT_THROW(settings({human(), agent("unoffered")}), std::invalid_argument);
    EXPECT_THROW(settings({human(), agent("A")}, R"({"kind":"turn","human_seconds":0,"agent_seconds":5,"timeout":"flag"})"),
                 std::invalid_argument);
}

// A human and an agent alternate: the human submits keys at its revision, the agent's move
// arrives as a selection index from the job the match scheduled.
TEST(MatchHost, HumanAndAgentAlternateThroughJobsAndKeys) {
    Match match(catalog(), host(), settings({human(), agent("A")}), 1'700'000'000'000ULL);
    EXPECT_FALSE(match.job());
    EXPECT_FALSE(match.take_unsaved());  // a never-started match has no log
    match.start(0);
    EXPECT_TRUE(match.take_unsaved());
    EXPECT_FALSE(match.job());  // the human moves first and the agent does not ponder
    Json view = state(match, 0, 10);
    ASSERT_FALSE(view.at("choices").as_array("choices").empty());
    EXPECT_TRUE(state(match, 1, 10).at("choices").as_array("choices").empty());
    const std::string key = view.at("choices").as_array("choices")[0].at("key").as_string("key");
    EXPECT_THROW(match.play(key, 5, 20), std::invalid_argument);  // stale revision
    match.play(key, 0, 20);
    const std::optional<Job> job = match.job();
    ASSERT_TRUE(job);
    EXPECT_EQ(job->kind, JobKind::Move);
    EXPECT_EQ(job->seat, 1);
    match.publish(job->id, R"({"root_visits":3})", 3);
    view = state(match, 0, 30);
    EXPECT_EQ(view.at("thinking").at("live").at("simulations").as_uint("sims"), 3U);
    EXPECT_THROW(match.play(key, 1, 40), std::invalid_argument);  // not a human's turn
    play_agent_first(match, 50);
    view = state(match, 0, 60);
    const Json::Array& log = view.at("log").as_array("log");
    const auto agent_move = std::find_if(log.begin(), log.end(), [](const Json& step) {
        return step.at("actor").as_uint("actor") == 1 && !step.at("forced").as_bool("forced");
    });
    ASSERT_NE(agent_move, log.end());
    EXPECT_EQ(agent_move->at("search").at("simulations").as_uint("sims"), 3U);
    EXPECT_EQ(agent_move->at("search").at("note").as_uint("note"), 1U);
    EXPECT_EQ(view.at("history").as_array("history").size(), 3U);
    EXPECT_EQ(view.at("revision").as_uint("revision"), 2U);
}

// Without humans the agents play each other to the end, and the log restores to the same
// finished game.
TEST(MatchHost, AgentsPlayEachOtherAndTheLogReplays) {
    Match match(catalog(), host(20), settings({agent("A"), agent("B")}), 1'700'000'000'000ULL);
    match.start(0);
    double now = 1;
    while (match.status() == MatchStatus::Running) play_agent_first(match, now++);
    EXPECT_EQ(match.status(), MatchStatus::Finished);
    const Json log = Json::parse(match.archive_log(now));
    EXPECT_EQ(log.at("result").at("reason").as_string("reason"), "max_game_plies");
    EXPECT_TRUE(log.at("result").at("winners").as_array("winners").empty());
    const std::unique_ptr<Match> restored = Match::restore(catalog(), host(20), log, now);
    EXPECT_EQ(restored->status(), MatchStatus::Finished);
    EXPECT_EQ(Json::parse(restored->archive_log(now)).at("steps"), log.at("steps"));
    EXPECT_THROW(Match::restore(catalog(), host(30), log, now), std::invalid_argument);
    MatchHost other = host(20);
    other.actors.pop_back();
    EXPECT_THROW(Match::restore(catalog(), other, log, now), std::invalid_argument);
    Json forged = log;
    forged.as_object("log");
    Json steps = log.at("steps");
    steps.as_array("steps")[0].set("actor", Json(1));
    forged.set("steps", steps);
    EXPECT_THROW(Match::restore(catalog(), host(20), forged, now), std::invalid_argument);
}

// Two humans share the board: each sees choices only from its own seat's perspective.
TEST(MatchHost, HumansTakeTurnsFromTheirOwnPerspectives) {
    Match match(catalog(), host(), settings({human(), human()}), 1'700'000'000'000ULL);
    match.start(0);
    for (std::uint64_t revision = 0; revision < 6; ++revision) {
        const std::uint8_t actor = match.episode().current_player();
        const auto other = static_cast<std::uint8_t>(1 - actor);
        EXPECT_TRUE(state(match, other, 1).at("choices").as_array("choices").empty());
        const Json view = state(match, actor, 1);
        match.play(view.at("choices").as_array("choices")[0].at("key").as_string("key"), revision, 2);
        EXPECT_FALSE(match.job());
    }
}

TEST(MatchHost, AnExpiredBankLosesForTheTimedOutSeatAlone) {
    Match match(catalog(), host(), settings({human(), agent("A")}, R"({"kind":"bank","seconds":1})"),
                1'700'000'000'000ULL);
    match.start(0);
    EXPECT_EQ(state(match, 0, 999).at("status").as_string("status"), "running");
    const Json view = state(match, 0, 1000);
    EXPECT_EQ(view.at("status").as_string("status"), "finished");
    EXPECT_EQ(view.at("result").at("reason").as_string("reason"), "timeout");
    EXPECT_EQ(view.at("result").at("winners"), Json::parse("[1]"));
    EXPECT_EQ(view.at("result").at("timed_out_seat").as_uint("seat"), 0U);
}

// A log's timeout must be its restored clock's own: less pending use is a forged result.
TEST(MatchHost, ATimeoutRestoresOnlyFromAnExpiredClock) {
    Match match(catalog(), host(), settings({human(), agent("A")}, R"({"kind":"bank","seconds":1})"),
                1'700'000'000'000ULL);
    match.start(0);
    static_cast<void>(match.state(0, std::nullopt, 1000));
    Json log = Json::parse(match.archive_log(1000));
    ASSERT_EQ(log.at("result").at("reason").as_string("reason"), "timeout");
    EXPECT_EQ(Match::restore(catalog(), host(), log, 5000)->status(), MatchStatus::Finished);
    log.set("pending_elapsed_ms", Json(std::uint64_t{10}));
    EXPECT_THROW(static_cast<void>(Match::restore(catalog(), host(), log, 5000)), std::invalid_argument);
}

// A malformed agent report leaves its move due, so the host can report the failure.
TEST(MatchHost, AMalformedAgentReportFailsItsSearch) {
    Match match(catalog(), host(), settings({agent("A"), human()}), 1'700'000'000'000ULL);
    match.start(0);
    const std::optional<Job> job = match.job();
    ASSERT_TRUE(job);
    EXPECT_ANY_THROW(match.play_agent(job->id, 0, "[]", 10));
    ASSERT_TRUE(match.job());
    match.fail(job->id, "bad report", 10);
    EXPECT_EQ(match.status(), MatchStatus::Paused);
}

// A paused game restarts paused at the same decision with the pending turn's use.
TEST(MatchHost, APausedLogRestartsAtItsPendingDecision) {
    Match match(catalog(), host(), settings({human(), agent("A")}), 1'700'000'000'000ULL);
    match.start(0);
    const std::string key =
        state(match, 0, 0).at("choices").as_array("choices")[0].at("key").as_string("key");
    match.play(key, 0, 1500);
    play_agent_first(match, 2000);
    match.pause(2700);
    const Json log = Json::parse(match.archive_log(2700));
    EXPECT_EQ(log.at("pending_elapsed_ms").as_uint("pending"), 700U);
    const std::unique_ptr<Match> restored = Match::restore(catalog(), host(), log, 10'000);
    EXPECT_EQ(restored->status(), MatchStatus::Paused);
    EXPECT_EQ(Json::parse(restored->archive_log(10'000)).at("steps"), log.at("steps"));
    EXPECT_EQ(restored->clock().used_ms(10'000), 700);
    EXPECT_EQ(restored->episode().world(), match.episode().world());
}

// An agent that holds a card another seat cannot identify reports its own knowledge: other
// seats see only that a report exists until the game is over.
TEST(MatchHost, ConcealedAgentReportsAreWithheldFromOtherSeats) {
    Match match(catalog(), host(), settings({agent("A"), human()}), 1'700'000'000'000ULL);
    match.start(0);
    double now = 1;
    for (int turn = 0; turn < 40 && match.status() == MatchStatus::Running; ++turn) {
        if (const std::optional<Job> job = match.job()) {
            if (job->concealed) {
                match.publish(job->id, R"({"secret":true})", 1);
                const Json hidden = state(match, 1, now).at("thinking").at("live");
                EXPECT_TRUE(hidden.at("concealed").as_bool("concealed"));
                EXPECT_FALSE(hidden.find("secret"));
                EXPECT_TRUE(state(match, 0, now).at("thinking").at("live").find("secret"));
                match.play_agent(job->id, 0, R"({"secret":true})", now);
                const auto search = [](const Json& log) {
                    const Json::Array& steps = log.at("steps").as_array("steps");
                    const auto move = std::find_if(steps.rbegin(), steps.rend(),
                        [](const Json& step) { return !step.at("search").is_null(); });
                    return move->at("search");
                };
                EXPECT_FALSE(search(Json::parse(match.export_log(1, now))).find("secret"));
                EXPECT_TRUE(search(Json::parse(match.export_log(0, now))).find("secret"));
                EXPECT_TRUE(search(Json::parse(match.archive_log(now))).find("secret"));
                return;
            }
            // Reserve blind whenever possible, so the agent soon holds a concealed card.
            const auto& choices = match.episode().legal_choices();
            const auto blind = std::find_if(choices.begin(), choices.end(), is_blind_reservation);
            const std::size_t index = blind == choices.end() ? 0 : static_cast<std::size_t>(blind - choices.begin());
            match.play_agent(job->id, index, "{}", now++);
        } else {
            const Json view = state(match, 1, now);
            match.play(view.at("choices").as_array("choices")[0].at("key").as_string("key"),
                       view.at("revision").as_uint("revision"), now++);
        }
    }
    FAIL() << "the agent never searched holding a concealed card";
}

TEST(MatchHost, ArchiveCountsOrdinaryAgentResultsAgainstHumans) {
    Match match(catalog(), host(), settings({human(), agent("A")}, R"({"kind":"bank","seconds":1})", false),
                1'700'000'000'000ULL);
    match.start(0);
    static_cast<void>(match.state(0, std::nullopt, 5000));
    const Json summary = log_summary(Json::parse(match.archive_log(5000)), "test-match-log");
    EXPECT_FALSE(summary.at("ordinary").as_bool("ordinary"));  // a timeout is not ordinary
    EXPECT_THROW(log_summary(Json::parse(match.archive_log(5000)), "other"), std::invalid_argument);
    Json ordinary = summary;
    ordinary.set("ordinary", Json(true));
    const Json records = archive_records({summary, ordinary});
    ASSERT_EQ(records.as_array("records").size(), 1U);
    EXPECT_EQ(records.as_array("records")[0].at("agent_wins").as_uint("wins"), 1U);
    EXPECT_EQ(records.as_array("records")[0].at("other_finished").as_uint("other"), 1U);
}

}  // namespace
