#pragma once

#include "splendor/episode.hpp"
#include "splendor/replay.hpp"
#include "splendor/play/json.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// One practical match between seats of any kind (docs/specs/play.md): each seat is a human,
// who submits validated keys through the host's commands, or an agent, an offered actor the
// host language runs whenever the match schedules a job for its seat. An agent's move is a
// choice index, with a report only if it has one: the host asks nothing of how it decides.
// The match owns the game, its clock, turn order, the agents' job schedule, analysis
// visibility, the log and its replay; a host language supplies wall and monotonic time, runs
// agents and persists logs. It is not thread-safe: a host serializes every call.
namespace splendor::play {

// The log schema this host writes and restores; a composer persists logs under its own
// contract name for it (`MatchHost::log_contract`).
inline constexpr std::string_view kMatchLogSchema = "splendor-match-log-v1";
inline constexpr std::string_view kSetupSeedDomain = "match-setup";
inline constexpr std::string_view kEpisodeSeedDomain = "match-episode";

enum class SeatKind : std::uint8_t { Human, Agent };

// An agent a host offers: `identity` is stable across names and paths (archives group results
// by it) and `summary` is the actor's logged description.
struct Actor {
    std::string name;
    std::string identity;
    Json summary;
};

// What every match of one host shares: the actors it offers, and `conditions` logged
// verbatim (an agent's search configuration, for example).
struct MatchHost {
    std::string log_contract;
    std::uint32_t max_game_plies{};
    Json conditions;
    std::vector<Actor> actors;
};

// The host description a host language supplies as JSON: `{log_contract, max_game_plies,
// conditions, actors: [{name, identity, summary}]}`.
MatchHost parse_host(const Json& description);

// One participant: a human, or an offered actor. A pondering agent runs a job of its own
// while a human seat decides.
struct Seat {
    SeatKind kind{};
    std::string name;
    std::string identity;
    Json actor;
    bool ponder{};
};

// A match's conditions as the board chose them: `{seats: [{kind: "human"} | {kind: "agent",
// actor: <offered name>, ponder}], time_control: {kind: "turn", human_seconds, agent_seconds,
// timeout: "flag"|"forfeit"} | {kind: "bank", seconds}, show_analysis, seed}`. Seat 0 starts;
// the seat count is the game's.
struct MatchSettings {
    std::vector<Seat> seats;
    std::uint64_t seed{};
    Json time_control;
    bool show_analysis{};
};

MatchSettings parse_settings(const Json& request, const MatchHost& host);

enum class MatchStatus : std::uint8_t { Ready, Running, Paused, Finished };
enum class JobKind : std::uint8_t { Move, Ponder, Review };

// The work a host language should be running for an agent seat: its own move, its pondering
// of a human's decision, or a finished game's analysis of a reviewed position. `root_actor`
// is the job's decision's actor; `concealed` says that actor held a card another seat could
// not identify, so a report is that seat's knowledge alone.
struct Job {
    std::uint64_t id{};
    JobKind kind{};
    std::uint8_t seat{};
    std::uint8_t root_actor{};
    std::uint64_t revision{};
    std::optional<std::size_t> position;
    bool concealed{};
    bool operator==(const Job&) const = default;
};

class Match {
public:
    // `unix_ms` is the host's wall clock, naming the game and its UTC start.
    Match(const Catalog& catalog, MatchHost host, MatchSettings settings, std::uint64_t unix_ms);
    // A logged match rebuilt by replaying its keys: the game facts are regenerated and
    // verified, then logged clocks and reports are adopted. An unfinished match restarts
    // paused at its pending decision; a finished one reopens for review.
    static std::unique_ptr<Match> restore(
        const Catalog& catalog, MatchHost host, const Json& log, double now_ms);
    Match(const Match&) = delete;
    Match& operator=(const Match&) = delete;

    // Commands. `now_ms` is the host's monotonic clock.
    void start(double now_ms);
    void pause(double now_ms);
    void resume(double now_ms);
    // Stops any job and freezes the clock, adjudicating expiry first.
    void close(double now_ms);
    // The acting human's complete decision, composed at `revision`.
    void play(std::string_view action_key, std::uint64_t revision, double now_ms);
    // Analyzes a finished game's reviewed position; none stops the analysis.
    void analyze(std::optional<std::size_t> position);

    // The job the host language should run, if any, and the episode of its decision: shared,
    // so a holder keeps a review position's episode after the next analysis replaces it.
    const std::optional<Job>& job() const noexcept { return job_; }
    std::shared_ptr<const TurnEpisode> job_episode() const;
    // A job's progress for display: a JSON report object and the simulations (or other work
    // units) spent since the last one.
    void publish(std::uint64_t job, std::string_view report, std::uint64_t simulations);
    // A ponder or review job ended without a selection.
    void finish(std::uint64_t job, double now_ms);
    // A move job's selection: an index into the legal choices of the job's decision, which
    // is still pending while the job is current, with the agent's final JSON report object,
    // if it reports.
    void play_agent(
        std::uint64_t job, std::size_t choice, std::optional<std::string_view> report, double now_ms);
    void fail(std::uint64_t job, std::string_view error, double now_ms);

    // The board's state as seat `perspective` may see it; with `since` equal to the current
    // version, only the moving parts (clock and thinking).
    std::string state(std::uint8_t perspective, std::optional<std::uint64_t> since, double now_ms);
    // The replayable log as `perspective` may see it, and as text.
    std::string export_log(std::uint8_t perspective, double now_ms);
    std::string export_text(std::uint8_t perspective, double now_ms);
    // The full, unredacted log an archive keeps.
    std::string archive_log(double now_ms);
    // Whether the archive log changed since the previous call; a never-started match has none.
    bool take_unsaved() noexcept;

    const TurnEpisode& episode() const noexcept { return *episode_; }
    const GameClock& clock() const noexcept { return clock_; }
    const std::vector<Seat>& seats() const noexcept { return settings_.seats; }
    MatchStatus status() const noexcept { return status_; }
    std::uint64_t version() const noexcept { return version_; }
    const std::string& game_id() const noexcept { return game_id_; }
    // The first human seat, else seat 0: the board's default perspective.
    std::uint8_t default_perspective() const noexcept;

private:
    struct Snapshot {
        Decision world;
        std::optional<TerminalFacts> terminal;
        std::vector<std::vector<std::uint16_t>> nobles;
        std::vector<std::vector<std::uint16_t>> purchases;
    };

    Match(const Catalog& catalog, MatchHost host, MatchSettings settings, std::string game_id,
          std::string started_at);
    // Records the setup's steps and the first position.
    void begin();

    std::uint64_t revision() const noexcept { return history_.size() - 1; }
    bool analysis_visible() const noexcept;
    bool concealed_actor(const State& state, std::uint8_t seat) const;
    Json visible_report(const Json& report, std::uint8_t perspective) const;
    Json visible_step(const Json& step, std::uint8_t perspective) const;
    Json log_value(std::optional<std::uint8_t> perspective, double now_ms) const;
    Json settings_value() const;
    Json seats_value() const;
    Json clock_value(double now_ms) const;
    Json thinking_value(std::uint8_t perspective, double now_ms) const;
    const std::string& snapshot_json(std::uint8_t perspective, std::size_t position);
    std::string result_value() const;

    void touch();
    void expire(double now_ms);
    void halt();
    void advance(double now_ms);
    void launch(JobKind kind, std::uint8_t seat, double now_ms, std::optional<std::size_t> position = {});
    std::int64_t decided(double now_ms);
    // Plays a legal choice by index or key, then logs it and the position it leads to.
    void apply_choice(std::optional<std::size_t> index, std::string_view key,
                      std::optional<std::int64_t> elapsed_ms, Json search, Json ponder, bool overtime);
    void record(std::optional<std::int64_t> elapsed_ms, Json search, Json ponder, bool overtime);
    void replay(const Json& log, double now_ms);

    const Catalog& catalog_;
    MatchHost host_;
    MatchSettings settings_;
    std::string game_id_;
    std::string started_at_;
    std::uint64_t setup_seed_{};
    std::uint64_t episode_seed_{};
    std::shared_ptr<TurnEpisode> episode_;
    std::optional<TraceReplay> replay_;
    GameClock clock_;
    MatchStatus status_{MatchStatus::Ready};
    Json result_;
    std::string error_;
    std::uint64_t version_{};
    std::uint64_t pauses_{};
    bool unsaved_{};
    Json::Array steps_;
    std::vector<Snapshot> history_;
    // Each seat's claimed noble IDs and purchased card IDs so far.
    std::vector<std::vector<std::uint16_t>> nobles_;
    std::vector<std::vector<std::uint16_t>> purchases_;
    // Rendered history boards, by perspective then position; snapshots never change.
    std::vector<std::vector<std::string>> rendered_;
    std::optional<Job> job_;
    std::uint64_t next_job_{1};
    std::optional<double> thinking_since_;
    // Simulations each agent spent on the decision it moves at, across paused segments, and
    // pondering since its previous move.
    std::vector<std::uint64_t> move_work_;
    std::vector<std::uint64_t> ponder_work_;
    std::uint64_t review_work_{};
    Json live_;
    Json last_search_;
    Json review_;
    std::shared_ptr<TurnEpisode> review_episode_;
};

// An archived log as its listing shows it: `{game_id, started_at, seats, time_control,
// show_analysis, decisions, result, ordinary}`. Ordinary games are finished human-and-agent
// games without shown analysis or a timeout. Throws for a log of another contract.
Json log_summary(const Json& log, std::string_view log_contract);
// Each agent identity's ordinary record against human seats, from listed summaries:
// `[{identity, name, actor, human_wins, agent_wins, draws, other_finished, unfinished}]`.
Json archive_records(const Json::Array& summaries);

}  // namespace splendor::play
