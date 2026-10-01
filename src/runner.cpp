#include "betago/runner.hpp"
#include <atomic>
#include <climits>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace betago {
std::string GameResult::termination_reason() const {
    return final_state.is_terminal() ? "two_passes" : "move_limit";
}
std::optional<Score> GameResult::score() const {
    if (!final_state.is_terminal()) return std::nullopt;
    return final_state.score();
}
std::optional<int> GameResult::winner() const {
    return final_state.is_terminal() ? final_state.winner() : std::nullopt;
}
Json GameResult::to_json() const {
    Json actions = Json::array();
    for (auto move : moves) {
        if (move) actions.push_back({move->row, move->column});
        else actions.push_back(nullptr);
    }
    Json points = nullptr;
    if (auto s = score()) points = {{"black", s->black}, {"white", s->white}};
    Json victor = nullptr;
    if (auto w = winner()) victor = *w;
    return {{"size", final_state.size()}, {"komi", final_state.komi()},
            {"max_moves", max_moves}, {"moves", actions}, {"game_length", moves.size()},
            {"elapsed_seconds", elapsed_seconds}, {"termination_reason", termination_reason()},
            {"score", points}, {"winner", victor}};
}

GameResult run_game(Agent black, Agent white, int size, double komi, int max_moves,
                    const AcceptedMoveObserver& observer) {
    if (max_moves < 1) throw std::invalid_argument("Move limit must be a positive integer");
    GameState state = GameState::new_game(size, komi);
    std::vector<Move> moves;
    auto started = std::chrono::steady_clock::now();
    for (int index = 0; index < max_moves; ++index) {
        Move move = (state.to_play() == BLACK ? black : white)(state);
        auto next = state.play(move);
        moves.push_back(move);
        if (observer) observer(state, move, next);
        state = std::move(next);
        if (state.is_terminal()) break;
    }
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return {std::move(state), std::move(moves), max_moves, elapsed};
}

GameState replay_moves(const std::vector<Move>& moves, int size, double komi) {
    GameState state = GameState::new_game(size, komi);
    for (auto move : moves) state = state.play(move);
    return state;
}

std::vector<Move> moves_from_json(const Json& actions) {
    if (!actions.is_array()) throw std::invalid_argument("Moves must be an array");
    std::vector<Move> moves;
    for (const auto& action : actions) {
        if (action.is_null()) moves.push_back(PASS);
        else {
            if (!action.is_array() || action.size() != 2 ||
                !action[0].is_number_integer() || !action[1].is_number_integer())
                throw std::invalid_argument("Placement must contain two integer coordinates");
            auto row = action[0].get<std::int64_t>(), column = action[1].get<std::int64_t>();
            if (row < 0 || column < 0 || row > INT_MAX || column > INT_MAX)
                throw std::invalid_argument("Coordinates are out of bounds");
            moves.push_back(Point{static_cast<int>(row), static_cast<int>(column)});
        }
    }
    return moves;
}

Json load_records(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) throw std::runtime_error("Cannot open " + path.string());
    Json data = Json::parse(stream);
    if (!data.is_object() || !data.contains("schema_version") || !data["schema_version"].is_number_integer())
        throw std::invalid_argument("Unsupported game record format");
    if (data.at("schema_version") != 1 && data.at("schema_version") != 2)
        throw std::invalid_argument("Unsupported game record format");
    const int schema = data.at("schema_version").get<int>();
    const bool external = schema == 2;
    if (external && data.value("mode", std::string()) != "arena")
        throw std::invalid_argument("Schema two is reserved for external arena records");
    if (external) {
        const auto& agents = data.at("agents");
        if (!agents.is_object() || !agents.contains("a") || !agents.contains("b"))
            throw std::invalid_argument("External arena must describe both agents");
        int external_count = 0;
        for (const auto* name : {"a", "b"}) {
            const auto& kind = agents.at(name).at("kind");
            if (kind == "external-gtp") ++external_count;
            else if (kind != "random" && kind != "mcts" && kind != "policy" && kind != "neural-mcts")
                throw std::invalid_argument("Unknown recorded arena agent kind");
        }
        if (external_count != 1) throw std::invalid_argument("External arena must describe one external opponent");
        if (data.at("run_status") != "running" && data.at("run_status") != "finished")
            throw std::invalid_argument("Invalid external arena run status");
        if (!data.at("uncertainty_eligible").is_boolean())
            throw std::invalid_argument("Invalid uncertainty eligibility flag");
    }
    const auto& records = data.at("games");
    if (!records.is_array() || records.empty())
        throw std::invalid_argument("Game record must contain at least one game");
    std::size_t record_index = 0;
    for (const auto& record : records) {
        auto integer = [&](const char* key) {
            const auto& value = record.at(key);
            if (!value.is_number_integer()) throw std::invalid_argument("Expected an integer setting");
            auto number = value.get<std::int64_t>();
            if (number < 1 || number > INT_MAX) throw std::invalid_argument("Invalid recorded setting");
            return static_cast<int>(number);
        };
        int size = integer("size"), limit = integer("max_moves");
        double komi = record.at("komi").get<double>();
        auto moves = moves_from_json(record.at("moves"));
        auto state = replay_moves(moves, size, komi);
        if (moves.size() > static_cast<std::size_t>(limit) ||
            (!external && !state.is_terminal() && moves.size() != static_cast<std::size_t>(limit)))
            throw std::invalid_argument("Invalid recorded move limit");
        double elapsed = record.at("elapsed_seconds").get<double>();
        if (!std::isfinite(elapsed) || elapsed < 0) throw std::invalid_argument("Invalid runtime");
        Json expected = GameResult{state, moves, limit, elapsed}.to_json();
        if (external) {
            auto actor = [](const Json& value) {
                if (value != "a" && value != "b") throw std::invalid_argument("Invalid external arena actor");
                return value.get<std::string>();
            };
            const auto black = actor(record.at("black_agent")), white = actor(record.at("white_agent"));
            if (black == white) throw std::invalid_argument("Arena game needs two identities");
            if (!record.at("agent_kinds").is_object() || !record.at("uncertainty_eligible").is_boolean() ||
                record.at("uncertainty_eligible") != data.at("uncertainty_eligible"))
                throw std::invalid_argument("Attempt agent kinds or uncertainty metadata disagree");
            for (const auto* name : {"a", "b"})
                if (record.at("agent_kinds").at(name) != data.at("agents").at(name).at("kind"))
                    throw std::invalid_argument("Attempt agent kinds disagree with prepared agents");
            const auto& status = record.at("status");
            auto external_actor = [&](const Json& value) {
                const auto name = actor(value);
                if (data.at("agents").at(name).at("kind") != "external-gtp")
                    throw std::invalid_argument("Protocol failure or resignation must belong to the external agent");
                return name;
            };
            if (status == "completed") {
                if (!state.is_terminal() || !record.at("failure").is_null() || !record.at("resigned_by").is_null())
                    throw std::invalid_argument("Completed attempt needs two passes without a failure");
            } else if (status == "truncated") {
                if (state.is_terminal() || moves.size() != static_cast<std::size_t>(limit) ||
                    !record.at("failure").is_null() || !record.at("resigned_by").is_null())
                    throw std::invalid_argument("Truncated attempt must reach its move limit");
            } else if (status == "failed" || status == "resigned") {
                expected["score"] = expected["winner"] = nullptr;
                expected["termination_reason"] = status == "failed" ? "failure" : "resignation";
                if (status == "failed") {
                    const auto& failure = record.at("failure");
                    if (!failure.is_object() || !record.at("resigned_by").is_null())
                        throw std::invalid_argument("Failed attempt needs failure details only");
                    external_actor(failure.at("actor"));
                    for (const auto* field : {"code", "command", "diagnostics", "message"})
                        if (!failure.at(field).is_string() || (std::string(field) == "code" && failure.at(field) == ""))
                            throw std::invalid_argument("Malformed external failure details");
                    const auto& code = failure.at("code");
                    if (code != "launch_failure" && code != "unexpected_exit" && code != "timeout" &&
                        code != "malformed_response" && code != "wrong_response_id" && code != "gtp_rejection" &&
                        code != "unsupported_capability" && code != "illegal_move")
                        throw std::invalid_argument("Unsupported external failure code");
                } else {
                    const auto name = external_actor(record.at("resigned_by"));
                    if (name != (state.to_play() == BLACK ? black : white))
                        throw std::invalid_argument("Resigning actor must be the next player to move");
                    if (state.is_terminal() || moves.size() == static_cast<std::size_t>(limit) || !record.at("failure").is_null())
                        throw std::invalid_argument("Resignation must precede a scored or truncated result");
                }
            } else throw std::invalid_argument("Unsupported external arena attempt status");
            if (!record.at("cleanup_warnings").is_array() || !record.at("external_sessions").is_object())
                throw std::invalid_argument("Malformed external arena session metadata");
            for (const auto& warning : record.at("cleanup_warnings")) {
                if (!warning.is_object()) throw std::invalid_argument("Cleanup warnings must be objects");
                actor(warning.at("actor"));
            }
            for (const auto& entry : record.at("external_sessions").items()) {
                actor(Json(entry.key()));
                if (!entry.value().is_object()) throw std::invalid_argument("Session metadata must be an object");
            }
            const auto& decisions = record.at("decisions");
            if (!decisions.is_array() || decisions.size() < moves.size() || decisions.size() > moves.size() + 1)
                throw std::invalid_argument("Attempt decisions disagree with its legal move prefix");
            if (status == "resigned" && decisions.size() != moves.size() + 1)
                throw std::invalid_argument("Resignation must retain its unaccepted genmove decision");
            if (decisions.size() > moves.size() && (state.is_terminal() || moves.size() == static_cast<std::size_t>(limit)))
                throw std::invalid_argument("An ended game cannot have an additional decision");
            for (std::size_t index = 0; index < decisions.size(); ++index) {
                const auto& decision = decisions[index];
                const int color = index % 2 == 0 ? BLACK : WHITE;
                if (!decision.at("move_number").is_number_integer() || decision.at("move_number") != index + 1 ||
                    !decision.at("player").is_number_integer() || decision.at("player") != color ||
                    actor(decision.at("agent")) != (color == BLACK ? black : white) ||
                    !decision.at("accepted").is_boolean() || decision.at("accepted").get<bool>() != (index < moves.size()))
                    throw std::invalid_argument("Attempt decision identity or acceptance disagrees with replay");
                const double decision_seconds = decision.at("elapsed_seconds").get<double>();
                if (!std::isfinite(decision_seconds) || decision_seconds < 0)
                    throw std::invalid_argument("Invalid decision runtime");
                if (!decision.at("external").is_boolean() || decision.at("external").get<bool>() !=
                    (data.at("agents").at(decision.at("agent").get<std::string>()).at("kind") == "external-gtp"))
                    throw std::invalid_argument("Decision external metadata disagrees with its agent");
                if (index < moves.size() && moves_from_json(Json::array({decision.at("move")})).front() != moves[index])
                    throw std::invalid_argument("Accepted decision move disagrees with replay");
                if (index < moves.size() && decision.at("event") != "move")
                    throw std::invalid_argument("Accepted decisions must contain moves");
                if (index == moves.size() && status != "failed" && status != "resigned")
                    throw std::invalid_argument("Only failed or resigned attempts have unaccepted decisions");
                if (index == moves.size()) {
                    if (status == "resigned") {
                        if (decision.at("event") != "resignation" || decision.at("agent") != record.at("resigned_by"))
                            throw std::invalid_argument("Resignation decision disagrees with its actor");
                    } else if (decision.at("agent") != record.at("failure").at("actor") ||
                               (decision.at("event") != "failure" && decision.at("event") != "move") ||
                               (decision.at("event") == "move" && record.at("failure").at("code") != "illegal_move"))
                        throw std::invalid_argument("Unaccepted decision disagrees with its protocol failure");
                }
            }
            const auto& settings = data.at("settings");
            for (const auto* field : {"size", "komi", "max_moves"})
                if (settings.at(field) != record.at(field))
                    throw std::invalid_argument("Attempt settings disagree with arena settings");
            if (!record.at("pair_index").is_number_integer() || record.at("pair_index").get<std::int64_t>() < 0 ||
                !record.at("game_in_pair").is_number_integer() ||
                (record.at("game_in_pair") != 1 && record.at("game_in_pair") != 2))
                throw std::invalid_argument("Invalid external arena pair index");
            const auto pair = record.at("pair_index").get<std::int64_t>();
            const auto in_pair = record.at("game_in_pair").get<int>();
            if (pair != static_cast<std::int64_t>(record_index / 2) || in_pair != static_cast<int>(record_index % 2 + 1))
                throw std::invalid_argument("Attempt records must preserve their chronological color-pair schedule");
            if ((in_pair != 1 && in_pair != 2) || black != (in_pair == 1 ? "a" : "b") ||
                !settings.at("pairs").is_number_integer() || pair >= settings.at("pairs").get<std::int64_t>())
                throw std::invalid_argument("External arena pair must exchange colors");
            if (!data.at("seed").is_number_integer() ||
                (data.at("seed").is_number_unsigned() && data.at("seed").get<std::uint64_t>() >
                                                        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
                throw std::invalid_argument("Invalid recorded seed");
            const auto seed = data.at("seed").get<std::int64_t>();
            if (pair > (std::numeric_limits<std::int64_t>::max() - 1) / 2 ||
                seed > std::numeric_limits<std::int64_t>::max() - (2 * pair + 1))
                throw std::invalid_argument("External arena seed overflow");
            const auto a_seed = seed + 2 * pair, b_seed = a_seed + 1;
            for (const auto& [key, value] : {std::pair{"a_seed", a_seed}, {"b_seed", b_seed},
                                          {"black_seed", in_pair == 1 ? a_seed : b_seed},
                                          {"white_seed", in_pair == 1 ? b_seed : a_seed}})
                if (!record.at(key).is_number_integer() || record.at(key) != value)
                    throw std::invalid_argument("Recorded identity seed disagrees with color pairing");
        }
        for (auto key : {"game_length", "termination_reason", "score", "winner"})
            if (record.at(key) != expected.at(key))
                throw std::invalid_argument(std::string("Recorded ") + key + " disagrees with replay");
        ++record_index;
    }
    if (external) {
        const auto& pairs = data.at("settings").at("pairs");
        if (!pairs.is_number_integer() || pairs.get<std::int64_t>() < 1 ||
            pairs.get<std::int64_t>() > INT_MAX / 2 || records.size() > static_cast<std::size_t>(pairs.get<std::int64_t>() * 2) ||
            (data.at("run_status") == "finished" && records.size() != static_cast<std::size_t>(pairs.get<std::int64_t>() * 2)))
            throw std::invalid_argument("Arena run status disagrees with its scheduled attempts");
    }
    return data;
}

std::string describe(const Json& record) {
    std::ostringstream out;
    out << record.at("game_length").get<int>() << " moves | ";
    if (record.value("status", std::string()) == "failed")
        out << "failed: " << record.at("failure").at("actor").get<std::string>() << ' '
            << record.at("failure").at("code").get<std::string>() << "; unscored";
    else if (record.value("status", std::string()) == "resigned")
        out << record.at("resigned_by").get<std::string>() << " resigned; unscored";
    else if (record.at("termination_reason") == "move_limit") out << "truncated at move limit; no final score";
    else {
        const auto& winner = record.at("winner");
        out << (winner.is_null() ? "draw" : winner == BLACK ? "Black wins" : "White wins");
        out << ", Black " << record.at("score").at("black").get<double>()
            << " / White " << record.at("score").at("white").get<double>();
    }
    out << " | " << std::fixed << std::setprecision(3) << record.at("elapsed_seconds").get<double>() << 's';
    return out.str();
}

void save_records(const std::filesystem::path& path, const Json& data) {
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path);
    if (!stream || !(stream << data.dump(2) << '\n')) throw std::runtime_error("Cannot write " + path.string());
}

void save_records_atomic(const std::filesystem::path& path, const Json& data) {
    if (path.empty() || path.filename().empty()) throw std::invalid_argument("Output must name a file");
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    static std::atomic<std::uint64_t> sequence{0};
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    auto temporary = path;
    temporary += ".tmp-" + std::to_string(tick) + "-" + std::to_string(sequence.fetch_add(1));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); }
    } cleanup{temporary};
    {
        std::ofstream stream(temporary, std::ios::binary);
        // Native engine diagnostics may use a Windows code page. Preserve the
        // report even when those byte strings are not valid UTF-8.
        if (!stream || !(stream << data.dump(2, ' ', false, Json::error_handler_t::replace) << '\n'))
            throw std::runtime_error("Cannot write " + temporary.string());
        stream.flush();
        if (!stream) throw std::runtime_error("Cannot finish writing " + temporary.string());
        stream.close();
        if (!stream) throw std::runtime_error("Cannot close " + temporary.string());
    }
#ifdef _WIN32
    // Readers, antivirus and the sync provider can briefly hold a file during
    // replacement. Keep the old complete document available while retrying.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    for (;;) {
        if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) break;
        const auto error = GetLastError();
        if ((error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION) ||
            std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Cannot atomically replace " + path.string() +
                                     " (Windows error " + std::to_string(error) + ")");
        Sleep(5);
    }
#else
    std::filesystem::rename(temporary, path);
#endif
}
} // namespace betago
