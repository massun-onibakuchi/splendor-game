# distutils: language = c++
"""The native match host (`splendor::play`): JSON text in and out, nothing per choice."""
from libcpp cimport bool as cppbool
from libcpp.optional cimport optional
from libcpp.string cimport string
from libcpp.vector cimport vector
from libc.stdint cimport int64_t, uint8_t, uint64_t

from splendor_game.engine cimport (
    Catalog, Clock, Episode, GameClock, NativeCatalog, NativeTurnEpisode, clock_view, episode_view,
)

cdef extern from *:
    """
    #include "splendor/play/match.hpp"
    #include "splendor/play/render.hpp"

    namespace play_shim {
    using splendor::play::Json;
    using splendor::play::Match;

    static Match* create(const splendor::Catalog& catalog, const std::string& host,
                         const std::string& settings, std::uint64_t unix_ms) {
        const splendor::play::MatchHost resolved = splendor::play::parse_host(Json::parse(host));
        return new Match(catalog, resolved, splendor::play::parse_settings(Json::parse(settings), resolved),
                         unix_ms);
    }
    static Match* restore(const splendor::Catalog& catalog, const std::string& host,
                          const std::string& log, double now_ms) {
        return Match::restore(catalog, splendor::play::parse_host(Json::parse(host)), Json::parse(log), now_ms)
            .release();
    }
    // The pending job: kind is 0 move, 1 ponder, 2 review; position is -1 unless reviewing.
    struct Job {
        bool due{};
        std::uint64_t id{};
        std::uint8_t kind{};
        std::uint8_t seat{};
        std::uint8_t root_actor{};
        std::uint64_t revision{};
        std::int64_t position{-1};
        bool concealed{};
    };
    static Job job(const Match& match) {
        Job out;
        if (const auto& job = match.job()) {
            out = {true, job->id, static_cast<std::uint8_t>(job->kind), job->seat, job->root_actor,
                   job->revision, job->position ? static_cast<std::int64_t>(*job->position) : -1,
                   job->concealed};
        }
        return out;
    }
    static void play(Match& match, const std::string& key, std::uint64_t revision, double now_ms) {
        match.play(key, revision, now_ms);
    }
    static void play_agent(Match& match, std::uint64_t job, std::size_t choice,
                           const std::optional<std::string>& report, double now_ms) {
        match.play_agent(job, choice, report ? std::optional<std::string_view>(*report) : std::nullopt, now_ms);
    }
    static void analyze(Match& match, std::int64_t position) {
        match.analyze(position < 0 ? std::nullopt : std::optional<std::size_t>(position));
    }
    static std::string state(Match& match, std::uint8_t perspective, std::int64_t since, double now_ms) {
        return match.state(perspective, since < 0 ? std::nullopt : std::optional<std::uint64_t>(since), now_ms);
    }
    struct Seat {
        std::uint8_t kind{};
        std::string identity;
    };
    static std::vector<Seat> seats(const Match& match) {
        std::vector<Seat> out;
        for (const auto& seat : match.seats()) out.push_back({static_cast<std::uint8_t>(seat.kind), seat.identity});
        return out;
    }
    static std::string catalog_view(const splendor::Catalog& catalog) {
        return splendor::play::catalog_json(catalog);
    }
    using SharedEpisode = std::shared_ptr<const splendor::TurnEpisode>;
    static SharedEpisode job_episode(const Match& match) { return match.job_episode(); }
    static std::string summary(const std::string& log, const std::string& log_contract) {
        return splendor::play::log_summary(Json::parse(log), log_contract).dump();
    }
    static std::string records(const std::string& summaries) {
        return splendor::play::archive_records(Json::parse(summaries).as_array("summaries")).dump();
    }
    }  // namespace play_shim
    """
    cppclass NativeMatch "splendor::play::Match":
        void start(double) except +
        void pause(double) except +
        void resume(double) except +
        void close(double) except +
        void publish(uint64_t, const string&, uint64_t) except +
        void finish(uint64_t, double) except +
        void fail(uint64_t, const string&, double) except +
        string export_log(uint8_t, double) except +
        string export_text(uint8_t, double) except +
        string archive_log(double) except +
        cppbool take_unsaved()
        const NativeTurnEpisode& episode()
        const GameClock& clock()
        const string& game_id()
        uint8_t default_perspective()
    cppclass SharedEpisode "play_shim::SharedEpisode":
        const NativeTurnEpisode* get()
    cppclass ShimJob "play_shim::Job":
        cppbool due
        uint64_t id
        uint8_t kind
        uint8_t seat
        uint8_t root_actor
        uint64_t revision
        int64_t position
        cppbool concealed
    cppclass ShimSeat "play_shim::Seat":
        uint8_t kind
        string identity
    NativeMatch* shim_create "play_shim::create"(const NativeCatalog&, const string&, const string&, uint64_t) except +
    NativeMatch* shim_restore "play_shim::restore"(const NativeCatalog&, const string&, const string&, double) except +
    ShimJob shim_job "play_shim::job"(const NativeMatch&)
    SharedEpisode shim_job_episode "play_shim::job_episode"(const NativeMatch&) except +
    void shim_play "play_shim::play"(NativeMatch&, const string&, uint64_t, double) except +
    void shim_play_agent "play_shim::play_agent"(NativeMatch&, uint64_t, size_t, const optional[string]&, double) except +
    void shim_analyze "play_shim::analyze"(NativeMatch&, int64_t) except +
    string shim_state "play_shim::state"(NativeMatch&, uint8_t, int64_t, double) except +
    vector[ShimSeat] shim_seats "play_shim::seats"(const NativeMatch&) except +
    string shim_catalog_view "play_shim::catalog_view"(const NativeCatalog&) except +
    string shim_summary "play_shim::summary"(const string&, const string&) except +
    string shim_records "play_shim::records"(const string&) except +


_JOB_KINDS = ("move", "ponder", "review")
_SEAT_KINDS = ("human", "agent")


def catalog_view(Catalog catalog not None):
    """The catalog's display records, as JSON text."""
    return shim_catalog_view(catalog._catalog[0]).decode("utf-8")


def log_summary(str log, str log_contract):
    """An archived log's listing entry, as JSON text."""
    return shim_summary(log.encode("utf-8"), log_contract.encode("utf-8")).decode("utf-8")


def archive_records(str summaries):
    """Each agent identity's record against humans, from a JSON array of summaries."""
    return shim_records(summaries.encode("utf-8")).decode("utf-8")


cdef class _SharedEpisode:
    cdef SharedEpisode episode


cdef class NativeJob:
    """The job a host should run for an agent seat (`splendor::play::Job`)."""
    cdef readonly uint64_t id
    cdef readonly str kind
    cdef readonly int seat
    cdef readonly int root_actor
    cdef readonly uint64_t revision
    cdef readonly object position
    cdef readonly bint concealed


cdef class HostMatch:
    """One `splendor::play::Match`. Not thread-safe: the host serializes every call. Episode
    and clock views stay valid while this object lives and are read-only."""
    cdef NativeMatch* _match
    cdef readonly Catalog catalog

    def __dealloc__(self):
        del self._match

    @staticmethod
    def create(Catalog catalog not None, str host, str settings, uint64_t unix_ms):
        cdef HostMatch match = HostMatch.__new__(HostMatch)
        match.catalog = catalog
        match._match = shim_create(catalog._catalog[0], host.encode("utf-8"), settings.encode("utf-8"), unix_ms)
        return match

    @staticmethod
    def restore(Catalog catalog not None, str host, str log, double now_ms):
        cdef HostMatch match = HostMatch.__new__(HostMatch)
        match.catalog = catalog
        match._match = shim_restore(catalog._catalog[0], host.encode("utf-8"), log.encode("utf-8"), now_ms)
        return match

    def seats(self):
        """Each seat's `(kind, identity)`, in seat order."""
        return tuple(
            (_SEAT_KINDS[seat.kind], seat.identity.decode("utf-8")) for seat in shim_seats(self._match[0])
        )

    @property
    def game_id(self):
        return self._match.game_id().decode("utf-8")

    @property
    def default_perspective(self):
        return self._match.default_perspective()

    def episode(self):
        return episode_view(&self._match.episode(), self.catalog, self)

    def clock(self):
        return clock_view(&self._match.clock(), self)

    def job(self):
        cdef ShimJob native = shim_job(self._match[0])
        if not native.due:
            return None
        cdef NativeJob job = NativeJob.__new__(NativeJob)
        job.id = native.id
        job.kind = _JOB_KINDS[native.kind]
        job.seat = native.seat
        job.root_actor = native.root_actor
        job.revision = native.revision
        job.position = None if native.position < 0 else native.position
        job.concealed = native.concealed
        return job

    def job_episode(self):
        # The view shares the episode: a review position's outlives the next analysis.
        cdef _SharedEpisode owner = _SharedEpisode.__new__(_SharedEpisode)
        owner.episode = shim_job_episode(self._match[0])
        return episode_view(owner.episode.get(), self.catalog, owner)

    def start(self, double now_ms):
        self._match.start(now_ms)

    def pause(self, double now_ms):
        self._match.pause(now_ms)

    def resume(self, double now_ms):
        self._match.resume(now_ms)

    def close(self, double now_ms):
        self._match.close(now_ms)

    def play(self, str key, uint64_t revision, double now_ms):
        shim_play(self._match[0], key.encode("ascii"), revision, now_ms)

    def analyze(self, position):
        if position is None:
            shim_analyze(self._match[0], -1)
        elif type(position) is not int or position < 0:
            raise ValueError("no such position")
        else:
            shim_analyze(self._match[0], position)

    def publish(self, uint64_t job, str report, uint64_t simulations):
        self._match.publish(job, report.encode("utf-8"), simulations)

    def finish(self, uint64_t job, double now_ms):
        self._match.finish(job, now_ms)

    def play_agent(self, uint64_t job, size_t choice, report, double now_ms):
        cdef optional[string] native
        if report is not None:
            native = <string>(<str>report).encode("utf-8")
        shim_play_agent(self._match[0], job, choice, native, now_ms)

    def fail(self, uint64_t job, str error, double now_ms):
        self._match.fail(job, error.encode("utf-8"), now_ms)

    def state(self, uint8_t perspective, since, double now_ms):
        """The board's state JSON for `perspective`; with `since` the current version, only
        the moving parts."""
        cdef int64_t known = -1 if since is None else since
        return shim_state(self._match[0], perspective, known, now_ms)

    def export_log(self, uint8_t perspective, double now_ms):
        return self._match.export_log(perspective, now_ms).decode("utf-8")

    def export_text(self, uint8_t perspective, double now_ms):
        return self._match.export_text(perspective, now_ms).decode("utf-8")

    def archive_log(self, double now_ms):
        return self._match.archive_log(now_ms).decode("utf-8")

    def take_unsaved(self):
        return self._match.take_unsaved()
