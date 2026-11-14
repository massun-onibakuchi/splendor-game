#include "splendor/play/match.hpp"

#include "splendor/play/render.hpp"
#include "splendor/seed.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace splendor::play {
namespace {

[[noreturn]] void reject(const std::string& message) { throw std::invalid_argument(message); }

bool safe_identity(const std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](const char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.'
            || c == '_' || c == '-';
    });
}

std::string_view kind_name(const SeatKind kind) { return kind == SeatKind::Human ? "human" : "agent"; }

std::string_view job_name(const JobKind kind) {
    switch (kind) {
        case JobKind::Move: return "move";
        case JobKind::Ponder: return "ponder";
        case JobKind::Review: return "review";
    }
    return {};
}

std::string_view status_name(const MatchStatus status) {
    switch (status) {
        case MatchStatus::Ready: return "ready";
        case MatchStatus::Running: return "running";
        case MatchStatus::Paused: return "paused";
        case MatchStatus::Finished: return "finished";
    }
    return {};
}

Json parse_time_control(const Json& raw) {
    const std::string& kind = raw.at("kind").as_string("time_control kind");
    if (kind == "bank") {
        raw.closed({"kind", "seconds"}, "bank time control");
        return Json(Json::Object{{"kind", "bank"}, {"seconds", raw.at("seconds").as_uint("seconds", 32, true)}});
    }
    if (kind != "turn") reject("time_control kind must be turn or bank");
    raw.closed({"kind", "human_seconds", "agent_seconds", "timeout"}, "turn time control");
    const std::string& timeout = raw.at("timeout").as_string("timeout");
    if (timeout != "flag" && timeout != "forfeit") reject("timeout must be flag or forfeit");
    return Json(Json::Object{
        {"kind", "turn"},
        {"human_seconds", raw.at("human_seconds").as_uint("human_seconds", 32, true)},
        {"agent_seconds", raw.at("agent_seconds").as_uint("agent_seconds", 32, true)},
        {"timeout", timeout},
    });
}

// Seat-indexed allowances: one bank each, or each kind's turn allowance; only a human's
// per-turn overtime can forfeit, and a bank always does.
TimeControl native_time_control(const MatchSettings& settings) {
    const Json& raw = settings.time_control;
    TimeControl control;
    const bool bank = raw.at("kind").as_string("kind") == "bank";
    control.kind = bank ? ClockKind::Bank : ClockKind::Turn;
    for (const Seat& seat : settings.seats) {
        const bool human = seat.kind == SeatKind::Human;
        const std::uint64_t seconds = bank ? raw.at("seconds").as_uint("seconds")
            : raw.at(human ? "human_seconds" : "agent_seconds").as_uint("seconds");
        control.allowance_ms.push_back(seconds * 1000);
        control.forfeit_on_timeout.push_back(!bank && human && raw.at("timeout").as_string("timeout") == "forfeit");
    }
    if (const ValidationResult result = validate_time_control(control); !result) reject(result.message);
    return control;
}

std::string time_control_text(const Json& control) {
    if (control.at("kind").as_string("kind") == "bank") {
        return "bank " + std::to_string(control.at("seconds").as_uint("seconds"))
            + " s each, no increment, out of time loses";
    }
    return "human " + std::to_string(control.at("human_seconds").as_uint("seconds")) + " s, agent "
        + std::to_string(control.at("agent_seconds").as_uint("seconds")) + " s per turn, timeout "
        + control.at("timeout").as_string("timeout");
}

// UTC from Unix milliseconds (proleptic Gregorian), without the C library's shared state.
struct Utc {
    std::string iso;      // 2026-11-14T11:35:02Z
    std::string compact;  // 20261114T113502123Z
};

Utc utc(const std::uint64_t unix_ms) {
    const std::int64_t seconds = static_cast<std::int64_t>(unix_ms / 1000);
    const std::int64_t days = seconds / 86400;
    const std::int64_t of_day = seconds % 86400;
    // Howard Hinnant's civil_from_days.
    const std::int64_t z = days + 719468;
    const std::int64_t era = z / 146097;
    const std::int64_t doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const std::int64_t day = doy - (153 * mp + 2) / 5 + 1;
    const std::int64_t month = mp < 10 ? mp + 3 : mp - 9;
    const std::int64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);
    char iso[32];
    char compact[32];
    std::snprintf(iso, sizeof iso, "%04lld-%02lld-%02lldT%02lld:%02lld:%02lldZ", static_cast<long long>(year),
                  static_cast<long long>(month), static_cast<long long>(day),
                  static_cast<long long>(of_day / 3600), static_cast<long long>(of_day / 60 % 60),
                  static_cast<long long>(of_day % 60));
    std::snprintf(compact, sizeof compact, "%04lld%02lld%02lldT%02lld%02lld%02lld%03lluZ",
                  static_cast<long long>(year), static_cast<long long>(month), static_cast<long long>(day),
                  static_cast<long long>(of_day / 3600), static_cast<long long>(of_day / 60 % 60),
                  static_cast<long long>(of_day % 60), static_cast<unsigned long long>(unix_ms % 1000));
    return {iso, compact};
}

std::string game_id_of(const MatchSettings& settings, const std::string& compact) {
    std::string id = compact;
    for (const Seat& seat : settings.seats) id += '-' + seat.identity;
    return id + '-' + std::to_string(settings.seed);
}

Json optional_json(const std::optional<std::int64_t>& value) { return value ? Json(*value) : Json(); }

// `{...}` member writers for the state JSON, which embeds pre-rendered boards verbatim.
void raw_member(std::string& out, const std::string_view key, const std::string_view raw) {
    if (out.size() > 1) out += ',';
    append_quoted(out, key);
    out += ':';
    out += raw;
}
void member(std::string& out, const std::string_view key, const Json& value) {
    if (out.size() > 1) out += ',';
    append_quoted(out, key);
    out += ':';
    value.dump_to(out);
}

}  // namespace

MatchSettings parse_settings(const Json& request, const MatchHost& host) {
    request.closed({"seats", "time_control", "show_analysis", "seed"}, "match settings");
    MatchSettings settings;
    const Json::Array& seats = request.at("seats").as_array("seats");
    if (seats.size() < kMinPlayers || seats.size() > kMaxPlayers) reject("a match seats two to four players");
    for (const Json& raw : seats) {
        const std::string& kind = raw.at("kind").as_string("seat kind");
        Seat seat;
        if (kind == "human") {
            raw.closed({"kind"}, "human seat");
            seat.kind = SeatKind::Human;
            seat.name = "Human";
            seat.identity = "human";
        } else if (kind == "agent") {
            raw.closed({"kind", "actor", "ponder"}, "agent seat");
            const std::string& name = raw.at("actor").as_string("actor");
            const auto offered = std::find_if(host.actors.begin(), host.actors.end(),
                [&name](const Actor& actor) { return actor.name == name; });
            if (offered == host.actors.end()) reject("unknown actor: " + name);
            seat.kind = SeatKind::Agent;
            seat.name = offered->name;
            seat.identity = offered->identity;
            seat.actor = offered->summary;
            seat.ponder = raw.at("ponder").as_bool("ponder");
            if (!safe_identity(seat.identity)) reject("an actor identity is [A-Za-z0-9._-]+");
        } else {
            reject("seat kind must be human or agent");
        }
        settings.seats.push_back(std::move(seat));
    }
    // Every seat's name is unique: a name several seats would carry is numbered in seat
    // order, "Human 1", "Human 2" and "A #1", "A #2" (actor names often end in digits).
    std::vector<std::string> names;
    for (const Seat& seat : settings.seats) names.push_back(seat.name);
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (std::count(names.begin(), names.end(), names[index]) == 1) continue;
        Seat& seat = settings.seats[index];
        const auto order = std::count(names.begin(), names.begin() + static_cast<std::ptrdiff_t>(index), names[index]) + 1;
        seat.name += (seat.kind == SeatKind::Human ? " " : " #") + std::to_string(order);
    }
    const Json& seed = request.at("seed");
    // A browser cannot carry every uint64 as a JSON number; a decimal string is exact.
    if (seed.is_string()) {
        const std::string& text = seed.as_string("seed");
        if (text.empty() || text.size() > 20
            || !std::all_of(text.begin(), text.end(), [](const char c) { return c >= '0' && c <= '9'; })) {
            reject("seed must be a uint64");
        }
        settings.seed = Json::parse(text).as_uint("seed");
    } else {
        settings.seed = seed.as_uint("seed");
    }
    settings.time_control = parse_time_control(request.at("time_control"));
    settings.show_analysis = request.at("show_analysis").as_bool("show_analysis");
    native_time_control(settings);
    return settings;
}

Match::Match(const Catalog& catalog, MatchHost host, MatchSettings settings, std::string game_id,
             std::string started_at)
    : catalog_(catalog),
      host_(std::move(host)),
      settings_(std::move(settings)),
      game_id_(std::move(game_id)),
      started_at_(std::move(started_at)),
      setup_seed_(derive64(settings_.seed, kSetupSeedDomain)),
      episode_seed_(derive64(settings_.seed, kEpisodeSeedDomain)),
      episode_(std::make_shared<TurnEpisode>(
          catalog_, static_cast<std::uint8_t>(settings_.seats.size()), host_.max_game_plies, setup_seed_,
          episode_seed_)),
      clock_(native_time_control(settings_)),
      rendered_(settings_.seats.size()),
      move_work_(settings_.seats.size()),
      ponder_work_(settings_.seats.size()) {
    nobles_.resize(settings_.seats.size());
    purchases_.resize(settings_.seats.size());
}

Match::Match(const Catalog& catalog, MatchHost host, MatchSettings settings, const std::uint64_t unix_ms)
    : Match(catalog, std::move(host), std::move(settings), {}, {}) {
    const Utc now = utc(unix_ms);
    started_at_ = now.iso;
    game_id_ = game_id_of(settings_, now.compact);
    begin();
}

void Match::begin() {
    record({}, Json(), Json(), false);
    history_.push_back(Snapshot{episode_->world(), episode_->terminal(), nobles_, purchases_});
}

std::unique_ptr<Match> Match::restore(
    const Catalog& catalog, MatchHost host, const Json& log, const double now_ms) {
    if (log.at("log_contract").as_string("log_contract") != host.log_contract) {
        reject("not a " + host.log_contract + " log");
    }
    if (log.at("log_schema").as_string("log_schema") != kMatchLogSchema) {
        reject("not a " + std::string(kMatchLogSchema) + " log");
    }
    // Resume identity: the restarted game continues under the logged ply cap, which decides
    // when it ends. Rules and catalog need no comparison: replay requires the keys to
    // regenerate the log's own steps.
    if (log.at("max_game_plies").as_uint("max_game_plies", 32) != host.max_game_plies) {
        reject("the log was played under a different ply cap");
    }
    // A logged agent restarts as the offered actor of the same identity.
    Json seats = Json::array();
    for (const Json& logged : log.at("seats").as_array("seats")) {
        if (logged.at("kind").as_string("seat kind") == "human") {
            seats.push(Json(Json::Object{{"kind", "human"}}));
            continue;
        }
        const std::string& identity = logged.at("identity").as_string("identity");
        const auto offered = std::find_if(host.actors.begin(), host.actors.end(),
            [&identity](const Actor& actor) { return actor.identity == identity; });
        if (offered == host.actors.end()) reject("this host does not offer the actor " + identity);
        seats.push(Json(Json::Object{
            {"kind", "agent"}, {"actor", Json(offered->name)}, {"ponder", logged.at("ponder")}}));
    }
    MatchSettings settings = parse_settings(Json(Json::Object{
        {"seats", std::move(seats)}, {"time_control", log.at("time_control")},
        {"show_analysis", log.at("show_analysis")}, {"seed", log.at("seed")}}), host);
    std::unique_ptr<Match> match(new Match(
        catalog, std::move(host), std::move(settings), log.at("game_id").as_string("game_id"),
        log.at("started_at").as_string("started_at")));
    if (!safe_identity(match->game_id_)) reject("invalid game id");
    std::vector<RecordedStep> expected;
    for (const Json& step : log.at("steps").as_array("steps")) {
        RecordedStep fact;
        fact.game_ply = static_cast<std::uint32_t>(step.at("game_ply").as_uint("game_ply", 32));
        fact.actor = static_cast<std::uint8_t>(step.at("actor").as_uint("actor", 8));
        fact.forced = step.at("forced").as_bool("forced");
        fact.action_key = step.at("action_key").as_string("action_key");
        if (!step.at("refill_slot").is_null()) {
            fact.refill_slot = static_cast<std::uint8_t>(step.at("refill_slot").as_uint("refill_slot", 8));
        }
        if (!step.at("refill_card").is_null()) {
            fact.refill_card = static_cast<std::uint16_t>(step.at("refill_card").as_uint("refill_card", 16));
        }
        expected.push_back(std::move(fact));
    }
    match->replay_.emplace(std::move(expected));
    match->begin();
    match->replay(log, now_ms);
    return match;
}

void Match::replay(const Json& log, const double now_ms) {
    const Json::Array& logged = log.at("steps").as_array("steps");
    for (std::size_t index = 0; index < logged.size(); ++index) {
        if (logged[index].at("forced").as_bool("forced")) continue;
        const std::string& key = logged[index].at("action_key").as_string("action_key");
        const auto& choices = episode_->legal_choices();
        if (std::none_of(choices.begin(), choices.end(),
                         [&key](const TurnChoice& choice) { return choice_key(choice) == key; })) {
            reject("log step " + std::to_string(index + 1) + " is not legal at its position");
        }
        apply_choice({}, key, {}, Json(), Json(), false);
    }
    // Every native trace batch was verified as it was recorded.
    replay_->finish();
    replay_.reset();
    for (std::size_t index = 0; index < steps_.size(); ++index) {
        Json& mine = steps_[index];
        const Json& theirs = logged[index];
        for (const std::string_view name : {"elapsed_ms", "overtime", "search", "ponder"}) {
            mine.set(name, theirs.at(name));
        }
        if (!mine.at("forced").as_bool("forced")) {
            // Each decision's turn-cumulative time rebuilds the banks.
            clock_.record(
                static_cast<std::uint8_t>(mine.at("actor").as_uint("actor", 8)),
                static_cast<std::uint32_t>(mine.at("game_ply").as_uint("game_ply", 32)),
                theirs.at("elapsed_ms").as_uint("elapsed_ms"));
        }
        if (!mine.at("search").is_null()) last_search_ = mine.at("search");
    }
    pauses_ = log.at("pauses").as_uint("pauses");
    const Json& result = log.at("result");
    if (episode_->is_terminal()) {
        const Json final_result = Json::parse(result_value());
        if (!result.is_null() && !(result == final_result)) reject("the log's result is not its replayed game's");
        status_ = MatchStatus::Finished;
        result_ = final_result;
        touch();
        return;
    }
    // Align with the pending turn, then adopt its time since the last decision too.
    const std::uint8_t actor = episode_->current_player();
    clock_.run(actor, episode_->game_ply(), now_ms);
    clock_.stop(now_ms);
    const std::uint64_t pending = log.at("pending_elapsed_ms").as_uint("pending_elapsed_ms");
    if (static_cast<std::int64_t>(pending) < clock_.used_ms(now_ms)) {
        reject("pending time precedes the turn's last logged decision");
    }
    clock_.record(actor, episode_->game_ply(), pending);
    if (!result.is_null()) {
        // An unfinished game can only have ended on the clock, at its pending decision:
        // the restored clock itself must adjudicate that timeout.
        const std::optional<MatchResult> timeout = episode_->adjudicate_timeout(clock_, now_ms);
        const Json expected = timeout ? Json::parse(play::result_json(*timeout)) : Json();
        if (!timeout || !(result == expected)) reject("the log's result is not its replayed game's");
        status_ = MatchStatus::Finished;
        result_ = expected;
    } else {
        status_ = MatchStatus::Paused;
    }
    touch();
}

// Commands ---------------------------------------------------------------------

void Match::start(const double now_ms) {
    if (status_ != MatchStatus::Ready) reject("the match has already started");
    status_ = MatchStatus::Running;
    advance(now_ms);
}

void Match::pause(const double now_ms) {
    expire(now_ms);
    if (status_ != MatchStatus::Running) reject("only a running match can pause");
    halt();
    clock_.stop(now_ms);
    status_ = MatchStatus::Paused;
    ++pauses_;
    touch();
}

void Match::resume(const double now_ms) {
    if (status_ != MatchStatus::Paused) reject("only a paused match can resume");
    status_ = MatchStatus::Running;
    error_.clear();
    advance(now_ms);
}

void Match::close(const double now_ms) {
    halt();
    clock_.stop(now_ms);
    expire(now_ms);
    if (status_ == MatchStatus::Running) status_ = MatchStatus::Paused;
    touch();
}

void Match::play(const std::string_view action_key, const std::uint64_t submitted, const double now_ms) {
    expire(now_ms);
    if (status_ != MatchStatus::Running) reject("the match is not running");
    // Decision identity, not metadata: action keys recur across turns (the same take is
    // often legal again), so a repeated or late confirmation never reaches a later decision.
    if (submitted != revision()) reject("the decision has changed since this action was composed");
    const std::uint8_t actor = episode_->current_player();
    if (settings_.seats[actor].kind != SeatKind::Human) reject("it is not a human seat's turn");
    // A pondering agent's view of this decision is recorded with the move.
    Json ponder = !live_.is_null() && live_.at("kind").as_string("kind") == "ponder" ? live_ : Json();
    // Only a human's per-turn allowance can be overrun and played on (`flag`).
    const bool overtime = clock_.expired(now_ms);
    std::string key(action_key);
    const auto& choices = episode_->legal_choices();
    const bool legal = std::any_of(
        choices.begin(), choices.end(), [&](const TurnChoice& choice) { return choice_key(choice) == key; });
    if (!legal) reject("illegal action for this decision");
    halt();
    apply_choice({}, key, decided(now_ms), Json(), std::move(ponder), overtime);
    advance(now_ms);
}

void Match::analyze(const std::optional<std::size_t> position) {
    if (status_ != MatchStatus::Finished) reject("positions are analyzed once the game is over");
    if (position && *position >= history_.size()) reject("no such position");
    halt();
    review_ = Json();
    review_work_ = 0;
    if (!position) {
        touch();
        return;
    }
    // A fresh episode replayed to the snapshot: the live one cannot rewind.
    review_episode_ = std::make_shared<TurnEpisode>(
        catalog_, static_cast<std::uint8_t>(settings_.seats.size()), host_.max_game_plies, setup_seed_,
        episode_seed_);
    for (const Json& step : steps_) {
        if (!step.at("forced").as_bool("forced") && step.at("history_index").as_uint("history_index") <= *position) {
            review_episode_->select(std::string_view(step.at("action_key").as_string("action_key")));
        }
    }
    if (!review_episode_->is_terminal()) {
        // The root's actor reviews its own decision when it is an agent; otherwise the
        // first agent seat does, and a game without agents has no analysis.
        const std::uint8_t actor = review_episode_->current_player();
        std::optional<std::uint8_t> seat;
        if (settings_.seats[actor].kind == SeatKind::Agent) seat = actor;
        for (std::uint8_t other = 0; !seat && other < settings_.seats.size(); ++other) {
            if (settings_.seats[other].kind == SeatKind::Agent) seat = other;
        }
        if (seat) launch(JobKind::Review, *seat, 0, position);
    }
    touch();
}

// Jobs ---------------------------------------------------------------------------

std::shared_ptr<const TurnEpisode> Match::job_episode() const {
    if (!job_) throw std::logic_error("no search is due");
    return job_->kind == JobKind::Review ? review_episode_ : episode_;
}

void Match::publish(const std::uint64_t job, const std::string_view report, const std::uint64_t simulations) {
    if (!job_ || job_->id != job) {
        // A stale search's work still counts toward its seat's next move.
        return;
    }
    Json value = Json::parse(report);
    value.as_object("an agent report");
    value.set("kind", Json(job_->kind == JobKind::Review
        ? (job_->root_actor == job_->seat ? "move" : "ponder") : job_name(job_->kind)));
    value.set("seat", Json(job_->seat));
    value.set("actor", Json(job_->root_actor));
    value.set("concealed", Json(job_->concealed));
    switch (job_->kind) {
        case JobKind::Review:
            review_work_ += simulations;
            value.set("position", Json(static_cast<std::uint64_t>(*job_->position)));
            value.set("simulations", Json(review_work_));
            review_ = std::move(value);
            break;
        case JobKind::Ponder:
            ponder_work_[job_->seat] += simulations;
            value.set("simulations", Json(ponder_work_[job_->seat]));
            live_ = std::move(value);
            break;
        case JobKind::Move:
            move_work_[job_->seat] += simulations;
            value.set("simulations", Json(move_work_[job_->seat]));
            live_ = std::move(value);
            break;
    }
}

void Match::finish(const std::uint64_t job, const double now_ms) {
    if (!job_ || job_->id != job) return;
    // Completing root readiness past a bank's end loses on time, unplayed.
    expire(now_ms);
    if (!job_ || job_->id != job) return;
    if (job_->kind == JobKind::Move) throw std::logic_error("a move search ends with its selection");
    job_.reset();
    thinking_since_.reset();
    touch();
}

void Match::play_agent(
    const std::uint64_t job, const std::size_t choice, const std::string_view report, const double now_ms) {
    if (!job_ || job_->id != job) return;
    expire(now_ms);
    if (!job_ || job_->id != job) return;
    if (job_->kind != JobKind::Move || job_->revision != revision()) {
        throw std::logic_error("only a current move search selects");
    }
    const Job finished = *job_;
    if (choice >= episode_->legal_choices().size()) reject("agent selection is not a legal choice");
    // A malformed report fails before the job ends, so the host can still report the failure.
    Json search = Json::parse(report);
    search.as_object("an agent report");
    job_.reset();
    thinking_since_.reset();
    const std::int64_t elapsed = decided(now_ms);
    search.set("kind", Json("move"));
    search.set("seat", Json(finished.seat));
    search.set("actor", Json(finished.root_actor));
    search.set("concealed", Json(finished.concealed));
    // This decision's own simulations and those pondered before it; `root_visits` includes
    // any the agent's retained tree inherited.
    search.set("simulations", Json(move_work_[finished.seat]));
    search.set("pondered_simulations", Json(ponder_work_[finished.seat]));
    search.set("think_ms", Json(elapsed));
    move_work_[finished.seat] = 0;
    ponder_work_[finished.seat] = 0;
    last_search_ = search;
    apply_choice(choice, {}, elapsed, std::move(search), Json(), false);
    advance(now_ms);
}

void Match::fail(const std::uint64_t job, const std::string_view error, const double now_ms) {
    if (!job_ || job_->id != job) return;
    // A turn that ran out before the failure was lost on time, not paused.
    expire(now_ms);
    if (!job_ || job_->id != job) return;
    halt();
    error_ = "agent search failed: " + std::string(error);
    if (status_ == MatchStatus::Running) {
        clock_.stop(now_ms);
        status_ = MatchStatus::Paused;
    }
    touch();
}

// Internals ----------------------------------------------------------------------

void Match::touch() {
    ++version_;
    unsaved_ = unsaved_ || status_ != MatchStatus::Ready;
}

bool Match::take_unsaved() noexcept { return std::exchange(unsaved_, false); }

std::uint8_t Match::default_perspective() const noexcept {
    for (std::uint8_t seat = 0; seat < settings_.seats.size(); ++seat) {
        if (settings_.seats[seat].kind == SeatKind::Human) return seat;
    }
    return 0;
}

void Match::expire(const double now_ms) {
    if (status_ != MatchStatus::Running) return;
    const std::optional<MatchResult> result = episode_->adjudicate_timeout(clock_, now_ms);
    if (!result) return;
    halt();
    clock_.stop(now_ms);
    status_ = MatchStatus::Finished;
    result_ = Json::parse(play::result_json(*result));
    touch();
}

void Match::halt() {
    job_.reset();
    thinking_since_.reset();
}

void Match::advance(const double now_ms) {
    if (episode_->is_terminal()) {
        halt();
        clock_.stop(now_ms);
        status_ = MatchStatus::Finished;
        result_ = Json::parse(result_value());
        touch();
        return;
    }
    const std::uint8_t actor = episode_->current_player();
    // One turn's time covers its MAIN and CLEANUP decisions.
    clock_.run(actor, episode_->game_ply(), now_ms);
    if (!job_) {
        if (settings_.seats[actor].kind == SeatKind::Agent) {
            launch(JobKind::Move, actor, now_ms);
        } else {
            // The next pondering agent in turn order searches the human's decision.
            const auto seats = static_cast<std::uint8_t>(settings_.seats.size());
            for (std::uint8_t offset = 1; offset < seats; ++offset) {
                const auto seat = static_cast<std::uint8_t>((actor + offset) % seats);
                if (settings_.seats[seat].kind == SeatKind::Agent && settings_.seats[seat].ponder) {
                    launch(JobKind::Ponder, seat, now_ms);
                    break;
                }
            }
        }
    }
    touch();
}

bool Match::concealed_actor(const State& state, const std::uint8_t seat) const {
    return std::any_of(state.players[seat].reserves.begin(), state.players[seat].reserves.end(),
        [](const auto& reserve) { return reserve && reserve->concealed && reserve->card_id; });
}

void Match::launch(
    const JobKind kind, const std::uint8_t seat, const double now_ms, const std::optional<std::size_t> position) {
    const TurnEpisode& episode = kind == JobKind::Review ? *review_episode_ : *episode_;
    const std::uint8_t actor = episode.current_player();
    job_ = Job{next_job_++, kind, seat, actor, revision(), position,
               concealed_actor(episode.world().state, actor)};
    // A move's thinking time runs from now, so draining a previous search spends its own.
    if (kind == JobKind::Move) thinking_since_ = now_ms;
}

std::int64_t Match::decided(const double now_ms) {
    clock_.stop(now_ms);
    return clock_.used_ms(now_ms);
}

void Match::apply_choice(
    const std::optional<std::size_t> index, const std::string_view key,
    const std::optional<std::int64_t> elapsed_ms, Json search, Json ponder, const bool overtime) {
    if (index) {
        episode_->select(*index);
    } else {
        episode_->select(key);
    }
    record(elapsed_ms, std::move(search), std::move(ponder), overtime);
    history_.push_back(Snapshot{episode_->world(), episode_->terminal(), nobles_, purchases_});
    live_ = Json();
    touch();
}

// Logs a select's steps: the chosen one first, then any forced ones it caused.
void Match::record(
    const std::optional<std::int64_t> elapsed_ms, Json search, Json ponder, const bool overtime) {
    const std::vector<TurnTraceStep> trace = episode_->drain_trace();
    if (replay_) replay_->verify(trace);
    for (std::size_t position = 0; position < trace.size(); ++position) {
        const TurnTraceStep& step = trace[position];
        const TurnChoice& choice = step.choice;
        if (choice.phase == DecisionPhase::Main
            && (choice.primary.family == PrimaryFamily::BuyVisible
                || choice.primary.family == PrimaryFamily::BuyReserved)) {
            purchases_[step.actor].push_back(*choice.primary.target_card_id);
        }
        if (choice.phase == DecisionPhase::Cleanup && choice.noble_slot && step.noble_ids[*choice.noble_slot]) {
            nobles_[step.actor].push_back(*step.noble_ids[*choice.noble_slot]);
        }
        const bool chosen = position == 0 && !step.forced;
        std::string text = choice_text(choice, catalog_, step.noble_ids);
        if (step.refill_card) text += "; market refilled with card #" + std::to_string(*step.refill_card);
        steps_.push_back(Json(Json::Object{
            {"index", Json(static_cast<std::uint64_t>(steps_.size() + 1))},
            {"game_ply", Json(step.game_ply)},
            {"actor", Json(step.actor)},
            {"phase", Json(decision_phase_name(choice.phase))},
            {"action_key", Json(choice_key(choice))},
            {"forced", Json(step.forced)},
            {"choice", Json::parse(choice_json(choice, step.noble_ids))},
            {"refill_slot", step.refill_slot ? Json(*step.refill_slot) : Json()},
            {"refill_card", step.refill_card ? Json(*step.refill_card) : Json()},
            {"text", Json(std::move(text))},
            {"elapsed_ms", chosen ? optional_json(elapsed_ms) : Json()},
            {"overtime", Json(chosen && overtime)},
            {"search", chosen ? search : Json()},
            {"ponder", chosen ? ponder : Json()},
            {"history_index", Json(static_cast<std::uint64_t>(history_.size()))},
        }));
    }
}

std::string Match::result_value() const {
    return play::result_json(match_result(*episode_->terminal()));
}

// Views --------------------------------------------------------------------------

bool Match::analysis_visible() const noexcept {
    return settings_.show_analysis || status_ == MatchStatus::Finished;
}

// A report made from a seat's knowledge of a card another seat could not identify is only
// its presence to other seats until the game is finished.
Json Match::visible_report(const Json& report, const std::uint8_t perspective) const {
    // A report restored from a player's export may already be withheld: it has no actor.
    const Json* actor = report.is_null() ? nullptr : report.find("actor");
    if (!actor || status_ == MatchStatus::Finished || !report.at("concealed").as_bool("concealed")
        || actor->as_uint("actor", 8) == perspective) {
        return report;
    }
    return Json(Json::Object{{"kind", report.at("kind")}, {"seat", report.at("seat")}, {"concealed", true}});
}

Json Match::visible_step(const Json& step, const std::uint8_t perspective) const {
    Json visible = step;
    if (!analysis_visible()) {
        visible.set("search", Json());
        visible.set("ponder", Json());
    } else {
        visible.set("search", visible_report(step.at("search"), perspective));
        visible.set("ponder", visible_report(step.at("ponder"), perspective));
    }
    return visible;
}

Json Match::settings_value() const {
    return Json(Json::Object{
        {"game_id", Json(game_id_)},
        {"started_at", Json(started_at_)},
        {"seed", Json(std::to_string(settings_.seed))},
        {"setup_seed", Json(std::to_string(setup_seed_))},
        {"episode_seed", Json(std::to_string(episode_seed_))},
        {"time_control", settings_.time_control},
        {"show_analysis", Json(settings_.show_analysis)},
    });
}

Json Match::seats_value() const {
    Json seats = Json::array();
    for (std::uint8_t index = 0; index < settings_.seats.size(); ++index) {
        const Seat& seat = settings_.seats[index];
        seats.push(Json(Json::Object{
            {"seat", Json(index)},
            {"kind", Json(kind_name(seat.kind))},
            {"name", Json(seat.name)},
            {"identity", Json(seat.identity)},
            {"actor", seat.actor},
            {"ponder", Json(seat.ponder)},
        }));
    }
    return seats;
}

// The moving clock a client animates: every seat's time left now, and whose turn runs it.
Json Match::clock_value(const double now_ms) const {
    Json remaining = Json::array();
    for (std::uint8_t seat = 0; seat < settings_.seats.size(); ++seat) {
        remaining.push(Json(static_cast<std::int64_t>(clock_.remaining_ms(seat, now_ms))));
    }
    const std::optional<std::uint8_t> actor = clock_.actor();
    return Json(Json::Object{
        {"kind", settings_.time_control.at("kind")},
        {"actor", actor ? Json(*actor) : Json()},
        {"running", Json(clock_.running())},
        {"remaining_ms", std::move(remaining)},
    });
}

Json Match::thinking_value(const std::uint8_t perspective, const double now_ms) const {
    return Json(Json::Object{
        {"active", Json(thinking_since_.has_value())},
        {"seat", job_ ? Json(job_->seat) : Json()},
        {"kind", job_ ? Json(job_name(job_->kind)) : Json()},
        {"elapsed_ms", thinking_since_ ? Json(static_cast<std::int64_t>(std::llround(now_ms - *thinking_since_)))
                                       : Json()},
        // The agent's view so far of the undecided decision, its own or the pondered one.
        {"live", analysis_visible() ? visible_report(live_, perspective) : Json()},
        // A finished game's analysis of a reviewed position.
        {"review", visible_report(review_, perspective)},
    });
}

const std::string& Match::snapshot_json(const std::uint8_t perspective, const std::size_t position) {
    std::vector<std::string>& rendered = rendered_[perspective];
    while (rendered.size() <= position) {
        const Snapshot& snapshot = history_[rendered.size()];
        const auto seats = [](const std::vector<std::vector<std::uint16_t>>& lists) {
            Json out = Json::array();
            for (const auto& list : lists) {
                Json ids = Json::array();
                for (const std::uint16_t id : list) ids.push(Json(id));
                out.push(std::move(ids));
            }
            return out;
        };
        std::string out = "{";
        raw_member(out, "board", board_json(observe(snapshot.world, perspective), catalog_, snapshot.terminal));
        member(out, "nobles", seats(snapshot.nobles));
        member(out, "purchases", seats(snapshot.purchases));
        rendered.push_back(out + '}');
    }
    return rendered[position];
}

std::string Match::state(
    const std::uint8_t perspective, const std::optional<std::uint64_t> since, const double now_ms) {
    if (perspective >= settings_.seats.size()) reject("no such seat");
    expire(now_ms);
    std::string out = "{";
    member(out, "version", Json(version_));
    member(out, "clock", clock_value(now_ms));
    member(out, "thinking", thinking_value(perspective, now_ms));
    // Unchanged since the client's version: only the moving parts.
    if (since && *since == version_) return out + '}';
    const std::uint8_t actor = episode_->current_player();
    const bool to_move = status_ == MatchStatus::Running && !episode_->is_terminal() && actor == perspective
        && settings_.seats[actor].kind == SeatKind::Human;
    member(out, "revision", Json(revision()));
    member(out, "status", Json(status_name(status_)));
    member(out, "error", error_.empty() ? Json() : Json(error_));
    member(out, "perspective", Json(perspective));
    member(out, "settings", settings_value());
    member(out, "seats", seats_value());
    member(out, "conditions", host_.conditions);
    member(out, "max_game_plies", Json(host_.max_game_plies));
    raw_member(out, "board", board_json(episode_->view(perspective), catalog_, episode_->terminal()));
    raw_member(out, "choices", to_move ? choices_json(episode_->legal_choices(), episode_->world().state)
                                   : std::string("[]"));
    raw_member(out, "takes", to_move ? token_takes_json(episode_->world()) : std::string("[]"));
    member(out, "last_search", analysis_visible() ? visible_report(last_search_, perspective) : Json());
    Json log = Json::array();
    for (const Json& step : steps_) log.push(visible_step(step, perspective));
    member(out, "log", log);
    std::string history = "[";
    for (std::size_t position = 0; position < history_.size(); ++position) {
        if (position) history += ',';
        history += snapshot_json(perspective, position);
    }
    raw_member(out, "history", history + ']');
    member(out, "pauses", Json(pauses_));
    member(out, "result", result_);
    return out + '}';
}

Json Match::log_value(const std::optional<std::uint8_t> perspective, const double now_ms) const {
    Json steps = Json::array();
    for (const Json& step : steps_) steps.push(perspective ? visible_step(step, *perspective) : step);
    Json log(Json::Object{
        {"log_contract", Json(host_.log_contract)},
        {"log_schema", Json(kMatchLogSchema)},
        {"game_contract", Json(game_contract(static_cast<std::uint8_t>(settings_.seats.size())))},
        {"candidate_contract", Json(kStagedActionContract)},
        {"catalog", Json(Json::Object{
            {"contract", Json(catalog_.contract())}, {"payload_sha256", Json(catalog_.payload_sha256())}})},
        {"max_game_plies", Json(host_.max_game_plies)},
        {"seed_domains", Json(Json::Object{
            {"setup_seed", Json(kSetupSeedDomain)}, {"episode_seed", Json(kEpisodeSeedDomain)}})},
    });
    const Json settings = settings_value();
    for (const auto& [key, value] : settings.as_object("settings")) log.set(key, value);
    log.set("seats", seats_value());
    log.set("conditions", host_.conditions);
    log.set("pauses", Json(pauses_));
    log.set("steps", std::move(steps));
    log.set("pending_elapsed_ms", episode_->is_terminal() ? Json() : Json(clock_.used_ms(now_ms)));
    log.set("result", result_);
    return log;
}

std::string Match::export_log(const std::uint8_t perspective, const double now_ms) {
    if (perspective >= settings_.seats.size()) reject("no such seat");
    return log_value(perspective, now_ms).dump();
}

std::string Match::archive_log(const double now_ms) { return log_value(std::nullopt, now_ms).dump(); }

std::string Match::export_text(const std::uint8_t perspective, const double now_ms) {
    const Json log = log_value(perspective, now_ms);
    std::vector<std::string> names;
    for (const Seat& seat : settings_.seats) names.push_back(seat.name);
    std::string title;
    for (std::size_t seat = 0; seat < names.size(); ++seat) title += (seat ? " vs " : "") + names[seat];
    std::string out = "Splendor: " + title + "\n";
    out += "seed " + std::to_string(settings_.seed) + " (setup " + std::to_string(setup_seed_) + ", episode "
        + std::to_string(episode_seed_) + "); " + time_control_text(settings_.time_control)
        + "; analysis shown during play: " + (settings_.show_analysis ? "yes" : "no") + "; pauses "
        + std::to_string(pauses_) + "\n";
    for (const Json& step : log.at("steps").as_array("steps")) {
        const Json& choice = step.at("choice");
        const bool forced = step.at("forced").as_bool("forced");
        if (forced && choice.at("phase").as_string("phase") == "cleanup") {
            const auto& returned = choice.at("returned").as_array("returned");
            const bool any = std::any_of(returned.begin(), returned.end(),
                [](const Json& count) { return count.as_uint("count") != 0; });
            if (!any && choice.at("noble").is_null()) continue;
        }
        std::string note = forced ? " (forced)" : "";
        if (step.at("overtime").as_bool("overtime")) note += " (over time)";
        out += "Turn " + std::to_string(step.at("game_ply").as_uint("game_ply") + 1) + ' '
            + names[step.at("actor").as_uint("actor")] + ": " + step.at("text").as_string("text") + note + "\n";
    }
    if (!result_.is_null()) {
        const auto& winners = result_.at("winners").as_array("winners");
        std::string outcome;
        if (winners.empty()) {
            outcome = "no winner";
        } else {
            for (std::size_t index = 0; index < winners.size(); ++index) {
                outcome += (index ? " and " : "") + names[winners[index].as_uint("winner")];
            }
            outcome += winners.size() == 1 ? " wins" : " share the win";
        }
        std::string prestige;
        for (const Json& value : result_.at("final_prestige").as_array("final_prestige")) {
            prestige += (prestige.empty() ? "" : "-") + std::to_string(value.as_uint("prestige"));
        }
        out += "Result: " + outcome + " (" + result_.at("reason").as_string("reason") + ", prestige " + prestige
            + ")\n";
    }
    return out;
}

// Archive ------------------------------------------------------------------------

Json log_summary(const Json& log, const std::string_view log_contract) {
    if (log.at("log_contract").as_string("log_contract") != log_contract) {
        reject("not a " + std::string(log_contract) + " log");
    }
    const Json& result = log.at("result");
    bool human = false;
    bool agent = false;
    Json seats = Json::array();
    for (const Json& seat : log.at("seats").as_array("seats")) {
        const std::string& kind = seat.at("kind").as_string("kind");
        human = human || kind == "human";
        agent = agent || kind == "agent";
        seats.push(Json(Json::Object{
            {"seat", seat.at("seat")}, {"kind", Json(kind)}, {"name", seat.at("name")},
            {"identity", seat.at("identity")}, {"actor", seat.at("actor")}}));
    }
    std::uint64_t decisions = 0;
    for (const Json& step : log.at("steps").as_array("steps")) decisions += step.at("forced").as_bool("forced") ? 0 : 1;
    const bool show_analysis = log.at("show_analysis").as_bool("show_analysis");
    // Assisted games and timeout forfeits are not ordinary results for strength.
    const bool ordinary = !result.is_null() && human && agent && !show_analysis
        && result.at("reason").as_string("reason") != "timeout";
    return Json(Json::Object{
        {"game_id", log.at("game_id")},
        {"started_at", log.at("started_at")},
        {"seats", std::move(seats)},
        {"time_control", log.at("time_control")},
        {"show_analysis", Json(show_analysis)},
        {"decisions", Json(decisions)},
        {"result", result},
        {"ordinary", Json(ordinary)},
    });
}

Json archive_records(const Json::Array& summaries) {
    Json records = Json::array();
    const auto record_of = [&records](const Json& seat) -> Json& {
        const std::string& identity = seat.at("identity").as_string("identity");
        for (Json& record : records.as_array("records")) {
            if (record.at("identity").as_string("identity") == identity) return record;
        }
        records.push(Json(Json::Object{
            {"identity", Json(identity)}, {"name", seat.at("name")}, {"actor", seat.at("actor")},
            {"human_wins", Json(0)}, {"agent_wins", Json(0)}, {"draws", Json(0)},
            {"other_finished", Json(0)}, {"unfinished", Json(0)}}));
        return records.as_array("records").back();
    };
    const auto bump = [](Json& record, const std::string_view field) {
        record.set(field, Json(record.at(field).as_uint(field) + 1));
    };
    for (const Json& summary : summaries) {
        const Json::Array& seats = summary.at("seats").as_array("seats");
        const bool has_human = std::any_of(seats.begin(), seats.end(),
            [](const Json& seat) { return seat.at("kind").as_string("kind") == "human"; });
        if (!has_human) continue;
        const Json& result = summary.at("result");
        SeatMask winners = 0;
        SeatMask humans = 0;
        if (!result.is_null()) {
            for (const Json& winner : result.at("winners").as_array("winners")) {
                winners = static_cast<SeatMask>(winners | (1U << winner.as_uint("winner", 3)));
            }
        }
        for (const Json& seat : seats) {
            if (seat.at("kind").as_string("kind") == "human") {
                humans = static_cast<SeatMask>(humans | (1U << seat.at("seat").as_uint("seat", 3)));
            }
        }
        for (const Json& seat : seats) {
            if (seat.at("kind").as_string("kind") != "agent") continue;
            Json& record = record_of(seat);
            const auto index = seat.at("seat").as_uint("seat", 3);
            if (result.is_null()) {
                bump(record, "unfinished");
            } else if (!summary.at("ordinary").as_bool("ordinary")) {
                bump(record, "other_finished");
            } else if (contains(winners, index) && (winners & humans) == 0) {
                bump(record, "agent_wins");
            } else if (!contains(winners, index) && (winners & humans) != 0) {
                bump(record, "human_wins");
            } else {
                bump(record, "draws");
            }
        }
    }
    return records;
}

}  // namespace splendor::play
