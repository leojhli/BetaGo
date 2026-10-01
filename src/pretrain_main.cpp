#include "betago/options.hpp"
#include "betago/pretraining.hpp"
#include "betago/teacher.hpp"
#include "betago/training_live.hpp"
#include "build_info.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

namespace {
using namespace betago;
using Path = std::filesystem::path;

std::string read_bytes(const Path& path, std::size_t limit) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::invalid_argument("Cannot open " + path.string());
    std::string result;
    char buffer[65536];
    while (stream.read(buffer, sizeof(buffer)) || stream.gcount()) {
        if (result.size() + static_cast<std::size_t>(stream.gcount()) > limit)
            throw std::invalid_argument("Input exceeds size limit: " + path.string());
        result.append(buffer, static_cast<std::size_t>(stream.gcount()));
    }
    if (stream.bad()) throw std::runtime_error("Cannot read " + path.string());
    return result;
}
std::string fingerprint(std::string_view bytes) {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned char byte : bytes) { hash ^= byte; hash *= UINT64_C(1099511628211); }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}
Json metadata() {
    const auto* machine = std::getenv("COMPUTERNAME");
    if (!machine) machine = std::getenv("HOSTNAME");
    const auto* cpu = std::getenv("PROCESSOR_IDENTIFIER");
    return {{"build", {{"revision", build_info::revision}, {"source_sha256", build_info::source_sha256},
        {"dirty", build_info::dirty}, {"compiler", build_info::compiler}, {"flags", build_info::flags},
        {"built_at_utc", build_info::built_at_utc}}},
        {"machine", {{"name", machine ? Json(machine) : Json(nullptr)},
        {"processor", cpu ? Json(cpu) : Json(nullptr)}, {"hardware_threads", std::thread::hardware_concurrency()}}}};
}

class LiveFeed {
public:
    void enable(const Path& path, const GameState& state) {
        writer_.emplace(path); state_ = state;
    }
    void publish(const GameState& state, Move move, int count, const Json& progress) noexcept {
        state_ = state; move_ = move; count_ = count;
        stage(progress);
    }
    void stage(const Json& progress) noexcept {
        if (!writer_ || !state_) return;
        try { writer_->publish(*state_, move_, count_, progress); }
        catch (const std::exception& error) {
            std::cerr << "Live board feed disabled: " << error.what() << '\n'; writer_.reset();
        }
    }
private:
    std::optional<TrainingLiveWriter> writer_;
    std::optional<GameState> state_;
    Move move_;
    int count_ = 0;
};

void help() {
    std::cout << "Generate teacher games, import SGF, or pretrain a policy/value network (C++).\n"
        << "pretrain.exe --teacher-profile profiles/katago.json --games 20 --output results/teacher [--live]\n"
        << "pretrain.exe --import-sgf DIRECTORY_OR_FILE --size 9 --output results/imported [--live]\n"
        << "pretrain.exe --train results/teacher/corpus.json --output results/pretrained [--checkpoint MODEL]\n"
        << "  [--epochs 20 --batch-size 32 --validation-fraction 0.2 --learning-rate 0.01]\n"
        << "  [--seed 0 --no-augment --live --live-file PATH]\n"
        << "Teacher options: --size 9 --komi 7.5 --max-moves 400. A new process controls both colors.\n"
        << "Only completed teacher games train; SGF needs explicit SZ, KM and a known result.\n"
        << "Whole games are held out; at least two unique games are required. model.json selects the\n"
        << "best held-out epoch (possibly the initial checkpoint). Start a new selfplay run from it.\n"
        << "Exit 2 retains an incomplete corpus; 1 means invalid input or an I/O error.\n";
}
void check_options(const Options& args, const std::vector<std::string>& all,
                   const std::vector<std::string>& allowed) {
    for (const auto& option : all)
        if (args.has(option) && std::find(allowed.begin(), allowed.end(), option) == allowed.end())
            throw std::invalid_argument(option + " does not apply to this mode");
}
std::vector<Path> sgf_paths(const Path& input) {
    std::vector<Path> paths;
    if (std::filesystem::is_regular_file(input)) paths.push_back(input);
    else if (std::filesystem::is_directory(input)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(input)) {
            auto extension = entry.path().extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
                return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 'a' - 'A') : static_cast<char>(c);
            });
            if (entry.is_regular_file() && extension == ".sgf") paths.push_back(entry.path());
            if (paths.size() > 50000) throw std::invalid_argument("Import is limited to 50000 SGF files per corpus");
        }
    } else throw std::invalid_argument("SGF input is not a file or directory");
    std::sort(paths.begin(), paths.end());
    if (paths.empty()) throw std::invalid_argument("No SGF files found");
    return paths;
}
} // namespace

int main(int argc, char** argv) {
    LiveFeed live;
    try {
        Options args(argc, argv, {"--teacher-profile", "--import-sgf", "--train", "--output", "--checkpoint",
            "--size", "--komi", "--games", "--max-moves", "--seed", "--epochs", "--batch-size",
            "--validation-fraction", "--learning-rate", "--momentum", "--l2", "--channels", "--value-hidden", "--live-file"},
            {"--help", "--live", "--no-augment"});
        if (args.has("--help")) { help(); return 0; }
        const int modes = args.has("--teacher-profile") + args.has("--import-sgf") + args.has("--train");
        if (modes != 1) throw std::invalid_argument("Choose exactly one of --teacher-profile, --import-sgf, --train");
        if (!args.has("--output") || args.text("--output").empty()) throw std::invalid_argument("Provide a new --output directory");
        if (args.has("--live-file") && (!args.has("--live") || args.text("--live-file").empty()))
            throw std::invalid_argument("--live-file requires --live and a nonempty path");
        const auto output = std::filesystem::absolute(args.text("--output"));
        if (std::filesystem::exists(output)) throw std::invalid_argument("Output directory already exists; choose a new --output");
        const int size = args.integer("--size", 9);
        if (size < 1 || size > 19) throw std::invalid_argument("Board size must be between 1 and 19");
        const std::vector<std::string> mode_options{"--checkpoint", "--komi", "--games", "--max-moves", "--epochs",
            "--batch-size", "--validation-fraction", "--learning-rate", "--momentum", "--l2", "--channels", "--value-hidden", "--no-augment"};
        const auto live_path = args.has("--live-file") ? Path(args.text("--live-file")) : output / "live.json";
        const auto build = metadata();

        if (args.has("--teacher-profile")) {
            check_options(args, mode_options, {"--komi", "--games", "--max-moves"});
            const auto configuration = load_external_gtp_profile(args.text("--teacher-profile"));
            TeacherSettings settings;
            settings.board_size = size; settings.komi = args.real("--komi", size == 9 ? 7.5 : 0.5);
            settings.games = args.integer("--games", 20); settings.max_moves = args.integer("--max-moves", 400);
            settings.seed = args.integer<std::int64_t>("--seed", 0); settings.validate();
            if (!std::filesystem::create_directories(output)) throw std::invalid_argument("Output was created by another process");
            if (args.has("--live")) live.enable(live_path, GameState::new_game(size, settings.komi));
            std::size_t exported = 0;
            auto save = [&](const Json& data) {
                auto snapshot = data; snapshot["build_machine"] = build;
                save_records_atomic(output / "corpus.json", snapshot);
                for (; exported < data.at("games").size(); ++exported) {
                    std::ostringstream filename; filename << "game-" << std::setw(6) << std::setfill('0') << exported + 1 << ".sgf";
                    std::ofstream stream(output / filename.str(), std::ios::binary);
                    stream << expert_game_sgf(expert_game_from_json(data.at("games")[exported]));
                    stream.flush(); if (!stream) throw std::runtime_error("Cannot export teacher SGF");
                }
                const auto& summary = data.at("summary");
                std::cout << "Teacher attempts " << summary.at("attempted") << '/' << settings.games
                    << ", completed " << summary.at("completed") << ", failures " << summary.at("failures")
                    << ", resignations " << summary.at("resignations") << ", truncated " << summary.at("truncations") << '\n' << std::flush;
            };
            const auto corpus = generate_teacher_games(configuration, settings,
                [&](int game, const GameState& state, Move move, int count) {
                    live.publish(state, move, count, {{"phase", "self_play"}, {"message", "External teacher game"},
                        {"game", game + 1}, {"games_total", settings.games}});
                    if (count % 20 == 0) std::cout << "Teacher game " << game + 1 << '/' << settings.games << ", move " << count << '\n' << std::flush;
                }, save);
            const auto& summary = corpus.at("summary");
            const bool incomplete = summary.at("failures") != 0 || summary.at("resignations") != 0 || summary.at("truncations") != 0;
            live.stage({{"phase", "finished"}, {"message", incomplete ? "Teacher corpus saved with excluded attempts; review corpus.json" : "Teacher corpus saved; ready for supervised pretraining"}});
            std::cout << "Corpus: " << (output / "corpus.json").string() << '\n';
            return incomplete ? 2 : 0;
        }

        if (args.has("--import-sgf")) {
            check_options(args, mode_options, {});
            const auto paths = sgf_paths(std::filesystem::absolute(args.text("--import-sgf")));
            if (!std::filesystem::create_directories(output)) throw std::invalid_argument("Output was created by another process");
            Json corpus{{"schema_version", 1}, {"kind", "expert_game_corpus"}, {"board_size", size},
                {"metadata", {{"provenance", "user_supplied_sgf"}, {"professional_status", "unverified"},
                {"rules_note", "Moves checked under BetaGo simple ko and forbidden suicide; values use the reported SGF outcome, not local final-board scoring."}}},
                {"games", Json::array()}, {"files", Json::array()}, {"rejected_files", Json::array()}, {"build_machine", build}};
            if (args.has("--live")) live.enable(live_path, GameState::new_game(size));
            for (const auto& path : paths) {
                try {
                    const auto bytes = read_bytes(path, 32 * 1024 * 1024);
                    auto games = parse_sgf(bytes, size, path.string());
                    for (const auto& game : games) corpus["games"].push_back(expert_game_json(game));
                    corpus["files"].push_back({{"path", path.string()}, {"fingerprint_fnv1a64", fingerprint(bytes)}, {"games", games.size()}});
                } catch (const std::invalid_argument& error) {
                    corpus["rejected_files"].push_back({{"path", path.string()}, {"reason", error.what()}});
                    std::cerr << "Excluded " << path.string() << ": " << error.what() << '\n';
                }
                save_records_atomic(output / "corpus.json", corpus);
            }
            const bool incomplete = !corpus["rejected_files"].empty() || corpus["games"].empty();
            live.stage({{"phase", "finished"}, {"message", "SGF import saved; inspect corpus.json before training"}});
            std::cout << "Imported " << corpus["games"].size() << " games; rejected " << corpus["rejected_files"].size()
                      << " files. Corpus: " << (output / "corpus.json").string() << '\n';
            return incomplete ? 2 : 0;
        }

        check_options(args, mode_options, {"--checkpoint", "--epochs", "--batch-size", "--validation-fraction",
            "--learning-rate", "--momentum", "--l2", "--channels", "--value-hidden", "--no-augment"});
        if (args.has("--checkpoint") && (args.text("--checkpoint").empty() || args.has("--channels") || args.has("--value-hidden")))
            throw std::invalid_argument("A checkpoint supplies its architecture; omit --channels and --value-hidden");
        const auto corpus_path = std::filesystem::absolute(args.text("--train"));
        const auto bytes = read_bytes(corpus_path, 256 * 1024 * 1024);
        const auto corpus = Json::parse(bytes);
        if (!corpus.is_object() || corpus.value("schema_version", 0) != 1 || corpus.value("kind", "") != "expert_game_corpus" ||
            !corpus.contains("games") || !corpus.at("games").is_array())
            throw std::invalid_argument("Expected a schema 1 expert_game_corpus");
        std::vector<ExpertGame> games;
        for (const auto& record : corpus.at("games")) games.push_back(expert_game_from_json(record));
        PretrainingSettings settings;
        settings.network = {size, args.integer("--channels", 8), args.integer("--value-hidden", 16)};
        settings.epochs = args.integer("--epochs", 20); settings.batch_size = args.integer("--batch-size", 32);
        settings.validation_fraction = args.real("--validation-fraction", 0.2);
        settings.seed = args.integer<std::int64_t>("--seed", 0); settings.augment = !args.has("--no-augment");
        settings.optimizer = {args.real("--learning-rate", 0.01), args.real("--momentum", 0.9), args.real("--l2", 0.0001)};
        settings.validate();
        if (games.empty()) throw std::invalid_argument("Corpus contains no accepted games");
        if (!std::filesystem::create_directories(output)) throw std::invalid_argument("Output was created by another process");
        if (args.has("--live")) {
            live.enable(live_path, games.front().initial_state);
            live.stage({{"phase", "training"}, {"message", "Preparing whole-game split and supervised targets"}});
        }
        const auto checkpoint = args.has("--checkpoint") ? std::optional<Path>(args.text("--checkpoint")) : std::nullopt;
        const Json input_identity{{"path", corpus_path.string()}, {"fingerprint_fnv1a64", fingerprint(bytes)},
            {"metadata", corpus.value("metadata", Json::object())}};
        auto report = pretrain_expert_games(games, settings, checkpoint, output, [&](int epoch, const Json& metrics) {
            auto snapshot = Json::parse(read_bytes(output / "report.json", 32 * 1024 * 1024));
            snapshot["build_machine"] = build; snapshot["input_corpus"] = input_identity;
            save_records_atomic(output / "report.json", snapshot);
            std::cout << "Supervised epoch " << epoch << '/' << settings.epochs << ": " << metrics.dump() << '\n' << std::flush;
            live.stage({{"phase", "training"}, {"message", "Supervised pretraining epoch " + std::to_string(epoch)},
                {"update", epoch}, {"updates_total", settings.epochs},
                {"policy_loss", metrics.at("training").at("policy_cross_entropy")},
                {"value_loss", metrics.at("training").at("value_mse")},
                {"total_loss", metrics.at("training").at("combined_loss")}});
        });
        report["build_machine"] = build;
        report["input_corpus"] = input_identity;
        save_records_atomic(output / "report.json", report);
        live.stage({{"phase", "finished"}, {"message", "Pretraining finished; model.json selects the best held-out epoch"}});
        std::cout << "Selected checkpoint: " << (output / "model.json").string() << "\nReport: " << (output / "report.json").string()
            << "\nStart a NEW selfplay run with --checkpoint pointing to model.json. Held-out loss is not a human rank estimate.\n";
        return 0;
    } catch (const std::exception& error) {
        live.stage({{"phase", "error"}, {"message", error.what()}});
        std::cerr << "pretrain: " << error.what() << '\n'; return 1;
    }
}
