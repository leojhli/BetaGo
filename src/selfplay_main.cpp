#include "betago/arena.hpp"
#include "betago/options.hpp"
#include "betago/replay.hpp"
#include "betago/selfplay.hpp"
#include "betago/training_live.hpp"
#include "build_info.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace {
using namespace betago;
using Path = std::filesystem::path;

Json read_json(const Path& path) {
    std::ifstream input(path);
    if (!input) throw std::invalid_argument("Cannot open " + path.string());
    return Json::parse(input);
}
std::int64_t integer(const Json& value) {
    if (!value.is_number_integer() || (value.is_number_unsigned() &&
        value.get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX)))
        throw std::invalid_argument("Run settings require signed 64-bit integers");
    return value.get<std::int64_t>();
}
int small_integer(const Json& value) {
    auto number = integer(value);
    if (number < INT_MIN || number > INT_MAX) throw std::invalid_argument("Run integer is out of range");
    return static_cast<int>(number);
}
double real(const Json& value) {
    if (!value.is_number()) throw std::invalid_argument("Run settings require numeric values");
    auto number = value.get<double>();
    if (!std::isfinite(number)) throw std::invalid_argument("Run number must be finite");
    return number;
}
std::wstring normalized(const Path& path) {
    auto text = std::filesystem::weakly_canonical(path).generic_wstring();
#ifdef _WIN32
    for (auto& c : text) c = static_cast<wchar_t>(std::towlower(c));
#endif
    return text;
}
Path owned_path(const Path& root, const Json& value) {
    if (!value.is_string()) throw std::invalid_argument("Run artifact paths must be strings");
    Path relative(value.get<std::string>());
    if (relative.empty() || relative.has_root_path()) throw std::invalid_argument("Run paths must be relative");
    for (const auto& part : relative)
        if (part == "..") throw std::invalid_argument("Run paths cannot leave their directory");
    auto prefix = normalized(root) + L"/";
    if (!normalized(root / relative).starts_with(prefix))
        throw std::invalid_argument("Run artifact resolves outside its directory");
    return root / relative;
}

// Replacing the manifest is the commit point. Versioned checkpoints and replay
// snapshots are never overwritten. An interrupted attempt can be rerun safely.
void replace_file(const Path& temporary, const Path& target) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot commit " + target.string());
#else
    std::filesystem::rename(temporary, target);
#endif
}
void commit_json(const Path& path, const Json& data) {
    Path temporary = path; temporary += ".tmp";
    save_records(temporary, data);
    replace_file(temporary, path);
}
void refresh_best(const Path& root, const Path& checkpoint) {
    auto temporary = owned_path(root, "best.json.tmp");
    auto target = owned_path(root, "best.json");
    std::filesystem::copy_file(checkpoint, temporary, std::filesystem::copy_options::overwrite_existing);
    replace_file(temporary, target);
}
std::string fingerprint(const Path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::invalid_argument("Cannot read " + path.string());
    std::uint64_t hash = UINT64_C(14695981039346656037);
    char c;
    while (input.get(c)) { hash ^= static_cast<unsigned char>(c); hash *= UINT64_C(1099511628211); }
    if (input.bad()) throw std::runtime_error("Cannot read artifact bytes");
    std::ostringstream output;
    output << std::hex << std::setw(16) << std::setfill('0') << hash;
    return output.str();
}
std::int64_t iteration_seed(std::int64_t base, int iteration, std::uint64_t stream) {
    // SplitMix64 arithmetic is deliberately unsigned, including negative seeds.
    auto number = static_cast<std::uint64_t>(base) +
        static_cast<std::uint64_t>(iteration) * UINT64_C(0x9e3779b97f4a7c15) + stream;
    number = (number ^ (number >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    number = (number ^ (number >> 27)) * UINT64_C(0x94d049bb133111eb);
    return std::bit_cast<std::int64_t>(number ^ (number >> 31));
}
Json metadata() {
    const auto* hostname = std::getenv("COMPUTERNAME");
    if (!hostname) hostname = std::getenv("HOSTNAME");
    const auto* cpu = std::getenv("PROCESSOR_IDENTIFIER");
#ifdef _WIN32
    const char* os = "Windows";
#elif defined(__APPLE__)
    const char* os = "macOS";
#elif defined(__linux__)
    const char* os = "Linux";
#else
    const char* os = "unknown";
#endif
#if defined(__x86_64__) || defined(_M_X64)
    const char* architecture = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    const char* architecture = "arm64";
#else
    const char* architecture = "unknown";
#endif
    std::time_t now = std::time(nullptr);
    std::ostringstream timestamp;
    if (const auto* utc = std::gmtime(&now)) timestamp << std::put_time(utc, "%Y-%m-%dT%H:%M:%SZ");
    return {{"recorded_at_utc", timestamp.str()}, {"machine", {{"hostname", hostname ? Json(hostname) : Json(nullptr)},
        {"os", os}, {"architecture", architecture},
        {"cpu_identifier", cpu ? Json(cpu) : Json(nullptr)}, {"hardware_threads", std::thread::hardware_concurrency()}}},
        {"build", {{"git_revision", build_info::git_available ? Json(build_info::revision) : Json(nullptr)},
        {"git_dirty", build_info::git_available ? Json(build_info::dirty) : Json(nullptr)},
        {"source_sha256", build_info::source_sha256}, {"built_at_utc", build_info::built_at_utc},
        {"compiler", build_info::compiler}, {"profile", BETAGO_BUILD_PROFILE}, {"flags", build_info::flags}, {"cpp_standard", 20}}}};
}

struct LoopSettings {
    SelfPlaySettings play;
    TrainingSettings training;
    NetworkSettings network;
    int games = 4, capacity = 100, evaluation_simulations = 64, evaluation_pairs = 1, evaluation_max_moves = 400;
    double promotion_score = 0.55;
    std::int64_t seed = 0;
    void validate() const {
        play.validate(); training.validate(); network.validate();
        if (network.board_size != play.board_size) throw std::invalid_argument("Network and self-play sizes differ");
        if (games < 1 || capacity < 1 || evaluation_pairs < 1 || evaluation_pairs > INT_MAX / 2 || evaluation_max_moves < 1)
            throw std::invalid_argument("Game counts, replay capacity and evaluation move limit must be positive");
        NeuralMctsSettings{evaluation_simulations, play.search.c_puct}.validate();
        if (!std::isfinite(promotion_score) || promotion_score < 0.5 || promotion_score > 1)
            throw std::invalid_argument("Promotion score must be between 0.5 and 1");
    }
    Json to_json() const {
        return {{"self_play", play.to_json()}, {"training", training.to_json()},
            {"network", {{"board_size", network.board_size}, {"channels", network.channels}, {"value_hidden", network.value_hidden}}},
            {"games", games}, {"replay_capacity", capacity}, {"evaluation_simulations", evaluation_simulations},
            {"evaluation_pairs", evaluation_pairs}, {"evaluation_max_moves", evaluation_max_moves},
            {"promotion_score", promotion_score}, {"seed", seed}};
    }
    static LoopSettings from_json(const Json& data) {
        LoopSettings settings;
        const auto& play = data.at("self_play"); const auto& training = data.at("training");
        const auto& network = data.at("network");
        settings.play.board_size = small_integer(play.at("board_size")); settings.play.komi = real(play.at("komi"));
        settings.play.max_moves = small_integer(play.at("max_moves"));
        settings.play.search = {small_integer(play.at("search").at("simulations")), real(play.at("search").at("c_puct"))};
        settings.play.temperature = real(play.at("temperature")); settings.play.temperature_moves = small_integer(play.at("temperature_moves"));
        settings.play.root_uniform_mix = real(play.at("root_uniform_mix"));
        settings.training.updates = small_integer(training.at("updates")); settings.training.batch_size = small_integer(training.at("batch_size"));
        settings.training.optimizer = {real(training.at("learning_rate")), real(training.at("momentum")), real(training.at("l2"))};
        settings.network = {small_integer(network.at("board_size")), small_integer(network.at("channels")), small_integer(network.at("value_hidden"))};
        settings.games = small_integer(data.at("games")); settings.capacity = small_integer(data.at("replay_capacity"));
        settings.evaluation_simulations = small_integer(data.at("evaluation_simulations"));
        settings.evaluation_pairs = small_integer(data.at("evaluation_pairs")); settings.evaluation_max_moves = small_integer(data.at("evaluation_max_moves"));
        settings.promotion_score = real(data.at("promotion_score")); settings.seed = integer(data.at("seed"));
        settings.validate(); return settings;
    }
};
std::string attempt_directory(const Path& root, int iteration) {
    for (int attempt = 1; attempt < INT_MAX; ++attempt) {
        std::ostringstream name;
        name << "iteration-" << std::setw(6) << std::setfill('0') << iteration << "-attempt-" << attempt;
        auto path = owned_path(root, name.str());
        if (std::filesystem::create_directory(path)) return name.str();
    }
    throw std::runtime_error("Cannot allocate iteration directory");
}
class LiveFeed {
public:
    bool enabled() const { return static_cast<bool>(writer_); }
    void enable(const Path& root) noexcept {
        try { writer_ = std::make_unique<TrainingLiveWriter>(owned_path(root, "live.json")); }
        catch (const std::exception& error) { disable(error.what()); }
        catch (...) { disable("Unknown live-feed initialization error"); }
    }
    void publish(const GameState& state, Move last_move, int move_number, const Json& progress) noexcept {
        if (!writer_) return;
        try {
            state_ = state; last_move_ = last_move; move_number_ = move_number;
            writer_->publish(state, last_move, move_number, progress);
        } catch (const std::exception& error) { disable(error.what()); }
        catch (...) { disable("Unknown live-feed write error"); }
    }
    void stage(const Json& progress) noexcept {
        if (!writer_ || !state_) return;
        try { writer_->publish(*state_, last_move_, move_number_, progress); }
        catch (const std::exception& error) { disable(error.what()); }
        catch (...) { disable("Unknown live-feed write error"); }
    }
private:
    std::unique_ptr<TrainingLiveWriter> writer_;
    std::optional<GameState> state_;
    Move last_move_;
    int move_number_ = 0;
    void disable(const char* reason) noexcept {
        writer_.reset();
        try { std::cerr << "Warning: live display disabled: " << reason << '\n'; } catch (...) {}
    }
};
void help() {
    std::cout << "BetaGo CPU self-play training loop\n"
        << "selfplay.exe --output results/selfplay [--iterations 1] [--checkpoint MODEL]\n"
        << "  --size 9 --komi 7.5 --seed 0 --games 4 --max-moves 200\n"
        << "  --simulations 64 --c-puct 1.5 --temperature 1 --temperature-moves 20 --root-uniform-mix 0.25\n"
        << "  --replay-games 100 --updates 100 --batch-size 32 --learning-rate 0.01 --momentum 0.9 --l2 0.0001\n"
        << "  --channels 8 --value-hidden 16 --eval-simulations 64 --eval-pairs 1 --eval-max-moves 400\n"
        << "  --promotion-score 0.55 [--live]\n"
        << "selfplay.exe --resume results/selfplay [--iterations 1] [--live]\n"
        << "New runs require a new directory. Resume restores all saved settings, replay and incumbent.\n"
        << "Completed games only provide value labels. Exit 2 means an iteration had no trainable replay.\n"
        << "best.json is the accepted model; every candidate and evaluation is kept.\n"
        << "--live writes live.json with accepted moves and training progress for the board viewer.\n"
        << "Deterministic evaluation repeats the same color games across seed pairs; its gate is heuristic.\n";
}
} // namespace

int main(int argc, char** argv) {
    LiveFeed live;
    try {
        Options args(argc, argv, {"--output", "--resume", "--checkpoint", "--iterations", "--size", "--komi", "--seed", "--games",
            "--max-moves", "--simulations", "--c-puct", "--temperature", "--temperature-moves", "--root-uniform-mix",
            "--replay-games", "--updates", "--batch-size", "--learning-rate", "--momentum", "--l2", "--channels", "--value-hidden",
            "--eval-simulations", "--eval-pairs", "--eval-max-moves", "--promotion-score"}, {"--help", "--live"});
        if (args.has("--help")) { help(); return 0; }
        int iterations = args.integer("--iterations", 1);
        if (iterations < 1) throw std::invalid_argument("Iterations must be positive");
        LoopSettings settings;
        Json manifest;
        Path root;
        if (args.has("--resume")) {
            for (const auto* option : {"--output", "--checkpoint", "--size", "--komi", "--seed", "--games", "--max-moves",
                "--simulations", "--c-puct", "--temperature", "--temperature-moves", "--root-uniform-mix", "--replay-games",
                "--updates", "--batch-size", "--learning-rate", "--momentum", "--l2", "--channels", "--value-hidden",
                "--eval-simulations", "--eval-pairs", "--eval-max-moves", "--promotion-score"})
                if (args.has(option)) throw std::invalid_argument(std::string(option) + " cannot override a resumed run; start a new run with --checkpoint");
            if (args.text("--resume").empty()) throw std::invalid_argument("Resume directory cannot be empty");
            root = std::filesystem::absolute(args.text("--resume"));
            manifest = read_json(owned_path(root, "run.json"));
            if (integer(manifest.at("schema_version")) != 1 || manifest.at("kind") != "self_play_training_run")
                throw std::invalid_argument("Unsupported training run");
            settings = LoopSettings::from_json(manifest.at("settings"));
        } else {
            if (args.text("--output", "results/selfplay").empty()) throw std::invalid_argument("Output directory cannot be empty");
            root = std::filesystem::absolute(args.text("--output", "results/selfplay"));
            if (std::filesystem::exists(root)) throw std::invalid_argument("Output directory already exists; use --resume or a new --output");
            if (args.has("--checkpoint") && args.text("--checkpoint").empty()) throw std::invalid_argument("Checkpoint path cannot be empty");
            if (args.has("--checkpoint") && (args.has("--channels") || args.has("--value-hidden")))
                throw std::invalid_argument("A checkpoint supplies its architecture; omit --channels and --value-hidden");
            auto initial = args.has("--checkpoint") ? std::optional<PolicyValueNetwork>(PolicyValueNetwork::load(args.text("--checkpoint"))) : std::nullopt;
            settings.play.board_size = args.integer("--size", initial ? initial->settings().board_size : 9);
            settings.play.komi = args.real("--komi", settings.play.board_size == 9 ? 7.5 : 0.5);
            settings.play.max_moves = args.integer("--max-moves", 200);
            settings.play.search = {args.integer("--simulations", 64), args.real("--c-puct", 1.5)};
            settings.play.temperature = args.real("--temperature", 1);
            settings.play.temperature_moves = args.integer("--temperature-moves", 20);
            settings.play.root_uniform_mix = args.real("--root-uniform-mix", 0.25);
            settings.training = {args.integer("--updates", 100), args.integer("--batch-size", 32),
                {args.real("--learning-rate", 0.01), args.real("--momentum", 0.9), args.real("--l2", 0.0001)}};
            settings.seed = args.integer<std::int64_t>("--seed", 0);
            settings.network = initial ? initial->settings() : NetworkSettings{settings.play.board_size, args.integer("--channels", 8), args.integer("--value-hidden", 16)};
            settings.games = args.integer("--games", 4); settings.capacity = args.integer("--replay-games", 100);
            settings.evaluation_simulations = args.integer("--eval-simulations", 64);
            settings.evaluation_pairs = args.integer("--eval-pairs", 1); settings.evaluation_max_moves = args.integer("--eval-max-moves", 400);
            settings.promotion_score = args.real("--promotion-score", 0.55);
            settings.validate();
            if (!initial) initial.emplace(settings.network, settings.seed);
            initial->train(false);
            if (!std::filesystem::create_directories(root))
                throw std::invalid_argument("Output directory was created by another process; choose a new run directory");
            initial->save(root / "initial.json");
            ReplayBuffer(settings.capacity, settings.play.board_size, settings.play.komi).save(root / "replay-initial.json");
            manifest = {{"schema_version", 1}, {"kind", "self_play_training_run"}, {"settings", settings.to_json()},
                {"seed_derivation", "splitmix64(base + iteration*0x9e3779b97f4a7c15 + stream); game=i, training=0x100000000, arena=0x200000000 (62-bit bounded)"},
                {"initial_checkpoint_source", args.has("--checkpoint") ? Json(std::filesystem::absolute(args.text("--checkpoint")).string()) : Json(nullptr)},
                {"next_iteration", 1}, {"incumbent", "initial.json"}, {"replay", "replay-initial.json"},
                {"incumbent_fingerprint_fnv1a64", fingerprint(root / "initial.json")},
                {"replay_fingerprint_fnv1a64", fingerprint(root / "replay-initial.json")}, {"history", Json::array()}};
            commit_json(root / "run.json", manifest);
        }
        int first = small_integer(manifest.at("next_iteration"));
        if (first < 1 || iterations > INT_MAX - first || !manifest.at("history").is_array() ||
            manifest.at("history").size() != static_cast<std::size_t>(first - 1))
            throw std::invalid_argument("Invalid iteration counter or history");
        auto incumbent_path = owned_path(root, manifest.at("incumbent"));
        auto replay_path = owned_path(root, manifest.at("replay"));
        if (fingerprint(incumbent_path) != manifest.at("incumbent_fingerprint_fnv1a64").get<std::string>() ||
            fingerprint(replay_path) != manifest.at("replay_fingerprint_fnv1a64").get<std::string>())
            throw std::invalid_argument("Committed checkpoint/replay was changed; start a new run rather than resume modified artifacts");
        auto incumbent = PolicyValueNetwork::load(incumbent_path);
        if (incumbent.settings() != settings.network) throw std::invalid_argument("Run architecture disagrees with its incumbent");
        auto replay = ReplayBuffer::load(replay_path);
        if (replay.capacity() != settings.capacity || replay.board_size() != settings.play.board_size || replay.komi() != settings.play.komi)
            throw std::invalid_argument("Run replay settings disagree");
        refresh_best(root, incumbent_path);
        if (args.has("--live")) {
            live.enable(root);
            live.publish(GameState::new_game(settings.play.board_size, settings.play.komi), PASS, 0,
                {{"phase", "self_play"}, {"message", "Preparing BetaGo self-play (both colors)"}, {"iteration", first},
                 {"game", 0}, {"games_total", settings.games}});
        }
        std::cout << std::fixed << std::setprecision(5);
        bool skipped = false;
        for (int iteration = first; iteration < first + iterations; ++iteration) {
            const auto directory = attempt_directory(root, iteration);
            const auto path = owned_path(root, directory);
            auto started = std::chrono::steady_clock::now();
            std::cout << "Iteration " << iteration << ": self-play from " << manifest.at("incumbent") << '\n' << std::flush;
            Json games = Json::array(); int completed = 0;
            auto frozen = std::make_shared<const PolicyValueNetwork>(incumbent);
            for (int game = 0; game < settings.games; ++game) {
                SelfPlayMoveProgress live_moves;
                if (live.enabled()) {
                    live.publish(GameState::new_game(settings.play.board_size, settings.play.komi), PASS, 0,
                        {{"phase", "self_play"}, {"message", "BetaGo plays both Black and White"}, {"iteration", iteration},
                         {"game", game + 1}, {"games_total", settings.games}});
                    live_moves = [&](int move_number, const GameState& state, Move move) {
                        if (live.enabled()) live.publish(state, move, move_number,
                            {{"phase", "self_play"}, {"message", "BetaGo plays both Black and White"}, {"iteration", iteration},
                             {"game", game + 1}, {"games_total", settings.games}});
                    };
                }
                auto result = run_self_play(frozen, settings.play, iteration_seed(settings.seed, iteration, static_cast<std::uint64_t>(game)),
                    [&](int move, const GameState&, const SearchStatistics&) {
                        if (move % 25 == 0) std::cout << "  Game " << game + 1 << ": move " << move << '\n' << std::flush;
                    }, live_moves);
                if (live.enabled()) live.stage({{"phase", "self_play"},
                    {"message", result.record.at("termination_reason") == "two_passes" ? "BetaGo self-play completed (both colors)" : "BetaGo self-play truncated (both colors)"},
                    {"iteration", iteration}, {"game", game + 1}, {"games_total", settings.games}});
                if (replay.add(result.record)) ++completed;
                games.push_back(std::move(result.record));
                save_records(path / "games.json", {{"schema_version", 1}, {"kind", "self_play_batch"}, {"metadata", metadata()}, {"games", games}});
                std::cout << "  Game " << game + 1 << ": " << games.back().at("termination_reason") << ", "
                    << games.back().at("game_length") << " moves\n" << std::flush;
            }
            replay.save(path / "replay.json");
            if (completed > 0) save_records(path / "examples.json", self_play_dataset(games));
            Json report = {{"schema_version", 1}, {"kind", "self_play_iteration"}, {"iteration", iteration}, {"metadata", metadata()},
                {"settings", settings.to_json()}, {"incumbent_before", manifest.at("incumbent")}, {"completed_self_play_games", completed},
                {"truncated_self_play_games", settings.games - completed}, {"replay_games", replay.game_count()},
                {"replay_examples", replay.examples().size()}, {"promoted", false},
                {"examples", completed > 0 ? Json(directory + "/examples.json") : Json(nullptr)}};
            if (replay.game_count() == 0) {
                skipped = true;
                report["status"] = "skipped_no_completed_games";
                std::cout << "  No completed games: no value labels or training update.\n";
            } else {
                auto candidate = incumbent;
                if (live.enabled()) live.stage({{"phase", "training"}, {"message", "Training BetaGo candidate; no game in progress"},
                    {"iteration", iteration}, {"update", 0}, {"updates_total", settings.training.updates}});
                report["training"] = train_candidate(candidate, replay, settings.training,
                    iteration_seed(settings.seed, iteration, UINT64_C(0x100000000)),
                    [&](int update, const LossMetrics& loss) {
                        if (live.enabled()) live.stage({{"phase", "training"}, {"message", "Training BetaGo candidate; no game in progress"},
                            {"iteration", iteration}, {"update", update}, {"updates_total", settings.training.updates},
                            {"policy_loss", loss.policy}, {"value_loss", loss.value}, {"total_loss", loss.total}});
                        if (update == 1 || update % 25 == 0 || update == settings.training.updates)
                            std::cout << "  Update " << update << ": CE " << loss.policy << ", value MSE " << loss.value << '\n' << std::flush;
                    });
                candidate.save(path / "candidate.json");
                ArenaSettings arena;
                arena.a = {"neural-mcts", {}, (path / "candidate.json").string(), {settings.evaluation_simulations, settings.play.search.c_puct}};
                arena.b = {"neural-mcts", {}, incumbent_path.string(), arena.a.neural};
                arena.pairs = settings.evaluation_pairs; arena.size = settings.play.board_size; arena.komi = settings.play.komi;
                arena.max_moves = settings.evaluation_max_moves;
                // Reserve ample positive headroom for arena's pair seed arithmetic.
                arena.seed = static_cast<std::int64_t>(static_cast<std::uint64_t>(iteration_seed(settings.seed, iteration, UINT64_C(0x200000000))) & UINT64_C(0x3fffffffffffffff));
                ArenaMoveProgress live_evaluation;
                // Arena pairs alternate colors, starting with candidate A as Black.
                const auto matchup = [](int game) -> std::string {
                    return game % 2 == 0 ? "Black: BetaGo candidate | White: BetaGo accepted"
                                         : "Black: BetaGo accepted | White: BetaGo candidate";
                };
                if (live.enabled()) {
                    live.stage({{"phase", "evaluation"}, {"message", "Preparing BetaGo candidate vs accepted model"},
                        {"iteration", iteration}, {"game", 0}, {"games_total", settings.evaluation_pairs * 2}});
                    live_evaluation = [&](int game, const GameState& state, Move move, int move_number) {
                        if (live.enabled()) live.publish(state, move, move_number,
                            {{"phase", "evaluation"}, {"message", matchup(game)}, {"iteration", iteration},
                             {"game", game + 1}, {"games_total", settings.evaluation_pairs * 2}});
                    };
                }
                auto evaluation = run_arena(arena, metadata(), [&](int game, const Json& record) {
                    if (live.enabled()) live.stage({{"phase", "evaluation"},
                        {"message", matchup(game) + (record.at("termination_reason") == "two_passes" ? " - completed" : " - truncated")},
                        {"iteration", iteration}, {"game", game + 1}, {"games_total", settings.evaluation_pairs * 2}});
                    std::cout << "  Evaluation " << game + 1 << ": " << record.at("termination_reason") << '\n' << std::flush;
                }, {}, {}, live_evaluation);
                // PUCT from an empty board is deterministic. Repeating a color
                // pairing adds no independent samples, so suppress such bounds.
                evaluation["evaluation_sampling"] = {{"deterministic", true}, {"independent_seed_pairs", false},
                    {"distinct_color_matchups", 2}, {"note", "Additional seed pairs repeat the same two color games; score gate is heuristic, not a strength-confidence test."}};
                for (const char* agent : {"a", "b"}) {
                    evaluation["summary"]["agents"][agent]["paired_win_rate_95"] = nullptr;
                    evaluation["summary"]["agents"][agent]["paired_score_rate_95"] = nullptr;
                }
                save_records(path / "arena.json", evaluation);
                auto score = evaluation.at("summary").at("agents").at("a").at("score_rate");
                bool complete = evaluation.at("summary").at("truncated_games") == 0;
                bool promoted = complete && !score.is_null() && score.get<double>() >= settings.promotion_score;
                report["status"] = "trained_and_evaluated"; report["candidate"] = directory + "/candidate.json";
                report["evaluation"] = directory + "/arena.json"; report["candidate_score_rate"] = score;
                report["promotion_gate"] = {{"threshold", settings.promotion_score}, {"requires_all_games_complete", true},
                    {"all_games_complete", complete}, {"strength_claim", false}};
                report["promoted"] = promoted;
                if (promoted) {
                    incumbent = std::move(candidate); incumbent_path = path / "candidate.json";
                    manifest["incumbent"] = directory + "/candidate.json";
                }
                std::cout << "  Candidate score " << score << ": " << (promoted ? "accepted" : "kept for inspection; incumbent retained") << '\n';
            }
            report["elapsed_seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            report["incumbent_after"] = manifest.at("incumbent");
            save_records(path / "report.json", report);
            manifest["history"].push_back({{"iteration", iteration}, {"report", directory + "/report.json"},
                {"status", report.at("status")}, {"promoted", report.at("promoted")}});
            manifest["next_iteration"] = iteration + 1; manifest["replay"] = directory + "/replay.json";
            manifest["incumbent_fingerprint_fnv1a64"] = fingerprint(incumbent_path);
            manifest["replay_fingerprint_fnv1a64"] = fingerprint(path / "replay.json");
            commit_json(owned_path(root, "run.json"), manifest);
            refresh_best(root, incumbent_path);
            if (live.enabled()) live.stage({{"phase", "iteration_complete"},
                {"message", report.at("status") == "skipped_no_completed_games" ? "Iteration skipped: no completed games" :
                    report.at("promoted").get<bool>() ? "Iteration complete: candidate accepted" : "Iteration complete: incumbent retained"},
                {"iteration", iteration}});
        }
        if (live.enabled()) live.stage({{"phase", "finished"}, {"message", "Training run finished"},
            {"iteration", first + iterations - 1}});
        std::cout << "Saved run to " << root.string() << "; accepted model: " << (root / "best.json").string() << '\n';
        return skipped ? 2 : 0;
    } catch (const std::exception& error) {
        try { if (live.enabled()) live.stage({{"phase", "error"}, {"message", error.what()}}); } catch (...) {}
        std::cerr << "Error: " << error.what() << '\n'; return 1;
    }
}
