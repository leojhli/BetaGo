#include "betago/training_live.hpp"
#include <atomic>
#include <cmath>
#include <fstream>
#include <limits>
#include <random>
#include <set>
#include <sstream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace betago {
namespace {
constexpr std::size_t max_snapshot_bytes = 1024 * 1024;

void fields(const Json& object, std::initializer_list<const char*> required,
            std::initializer_list<const char*> optional, const char* name) {
    if (!object.is_object()) throw std::invalid_argument(std::string(name) + " must be an object");
    std::set<std::string> allowed;
    for (const auto* key : required) {
        if (!object.contains(key)) throw std::invalid_argument(std::string(name) + " is missing " + key);
        allowed.insert(key);
    }
    for (const auto* key : optional) allowed.insert(key);
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!allowed.contains(it.key())) throw std::invalid_argument(std::string(name) + " has unknown field " + it.key());
}

std::uint64_t natural(const Json& value, std::uint64_t maximum, const char* name) {
    if (!value.is_number_integer() ||
        (!value.is_number_unsigned() && value.get<std::int64_t>() < 0))
        throw std::invalid_argument(std::string(name) + " must be a nonnegative integer");
    const auto result = value.get<std::uint64_t>();
    if (result > maximum) throw std::invalid_argument(std::string(name) + " is out of range");
    return result;
}

std::string bounded_text(const Json& value, std::size_t maximum, const char* name, bool allow_empty = false) {
    if (!value.is_string()) throw std::invalid_argument(std::string(name) + " must be text");
    auto text = value.get<std::string>();
    if ((!allow_empty && text.empty()) || text.size() > maximum || text.find('\0') != std::string::npos)
        throw std::invalid_argument(std::string(name) + " is invalid");
    return text;
}

Board board(const Json& value, int expected_size = 0) {
    if (!value.is_array() || value.empty() || value.size() > 19 ||
        (expected_size != 0 && value.size() != static_cast<std::size_t>(expected_size)))
        throw std::invalid_argument("Live board size must be between 1 and 19");
    Board result;
    result.reserve(value.size());
    for (const auto& row : value) {
        if (!row.is_array() || row.size() != value.size()) throw std::invalid_argument("Live board must be square");
        std::vector<int> cells;
        cells.reserve(row.size());
        for (const auto& cell : row) cells.push_back(static_cast<int>(natural(cell, WHITE, "Board cell")));
        result.push_back(std::move(cells));
    }
    return result;
}

void validate_progress(const Json& progress) {
    fields(progress, {"phase", "message"},
           {"iteration", "game", "games_total", "update", "updates_total", "policy_loss", "value_loss", "total_loss", "loss"},
           "Live progress");
    const auto phase = bounded_text(progress.at("phase"), 32, "Phase");
    static const std::set<std::string> phases{"self_play", "training", "evaluation", "iteration_complete", "finished", "error"};
    if (!phases.contains(phase)) throw std::invalid_argument("Unknown live training phase");
    bounded_text(progress.at("message"), 16 * 1024, "Message", true);
    for (const auto* key : {"iteration", "game", "games_total", "update", "updates_total"})
        if (progress.contains(key)) natural(progress.at(key), std::numeric_limits<int>::max(), key);
    for (const auto* key : {"policy_loss", "value_loss", "total_loss", "loss"}) if (progress.contains(key)) {
        const auto& number = progress.at(key);
        if (!number.is_number() || !std::isfinite(number.get<double>()) || number.get<double>() < 0)
            throw std::invalid_argument(std::string(key) + " must be a finite nonnegative number");
    }
    if (progress.contains("game") && progress.contains("games_total") && progress.at("game") > progress.at("games_total"))
        throw std::invalid_argument("Live game number exceeds its total");
    if (progress.contains("update") && progress.contains("updates_total") && progress.at("update") > progress.at("updates_total"))
        throw std::invalid_argument("Live update number exceeds its total");
}

std::string session_id() {
    static std::atomic<std::uint64_t> runs{0};
    const auto time = std::chrono::system_clock::now().time_since_epoch().count();
    std::random_device random;
    std::ostringstream output;
    output << std::hex << time << '-' << random() << '-' << random() << '-' << runs.fetch_add(1);
    return output.str();
}

std::string read_bytes(const std::filesystem::path& path) {
    std::string bytes;
#ifdef _WIN32
    // Atomic replacement must remain possible while the viewer has the old
    // file open. Standard ifstream sharing does not guarantee this on Windows.
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot open training feed " + path.string() + " (Windows error " + std::to_string(GetLastError()) + ")");
    struct Close { HANDLE handle; ~Close() { CloseHandle(handle); } } close{handle};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size)) throw std::runtime_error("Cannot measure training feed");
    if (size.QuadPart < 0 || size.QuadPart > static_cast<LONGLONG>(max_snapshot_bytes))
        throw std::invalid_argument("Training feed exceeds 1 MiB");
    bytes.reserve(static_cast<std::size_t>(size.QuadPart));
    char buffer[64 * 1024];
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(handle, buffer, sizeof(buffer), &read, nullptr)) throw std::runtime_error("Cannot read training feed");
        if (read == 0) break;
        if (bytes.size() + read > max_snapshot_bytes) throw std::invalid_argument("Training feed exceeds 1 MiB");
        bytes.append(buffer, read);
    }
#else
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot open training feed " + path.string());
    char buffer[64 * 1024];
    while (stream.read(buffer, sizeof(buffer)) || stream.gcount() != 0) {
        const auto count = static_cast<std::size_t>(stream.gcount());
        if (bytes.size() + count > max_snapshot_bytes) throw std::invalid_argument("Training feed exceeds 1 MiB");
        bytes.append(buffer, count);
    }
    if (stream.bad()) throw std::runtime_error("Cannot read training feed");
#endif
    return bytes;
}

void protect_existing(const std::filesystem::path& path) {
    if (std::filesystem::exists(path)) (void)read_training_live(path);
}
} // namespace

TrainingLiveSnapshot parse_training_live(const Json& data) {
    fields(data, {"schema_version", "kind", "session_id", "sequence", "updated_at_ms", "move_number", "last_move", "state", "progress"}, {}, "Training feed");
    if (natural(data.at("schema_version"), 1, "Schema version") != 1 || data.at("kind") != "training_live")
        throw std::invalid_argument("Expected schema 1 training_live feed");
    const auto id = bounded_text(data.at("session_id"), 128, "Session ID");
    const auto sequence = natural(data.at("sequence"), std::numeric_limits<std::uint64_t>::max(), "Sequence");
    if (sequence == 0) throw std::invalid_argument("Live sequence must be positive");
    const auto time = natural(data.at("updated_at_ms"), std::numeric_limits<std::int64_t>::max(), "Update time");
    const auto move_number = natural(data.at("move_number"), std::numeric_limits<int>::max(), "Move number");
    const auto& state = data.at("state");
    fields(state, {"board", "komi", "to_play", "consecutive_passes", "previous_board"}, {}, "Live state");
    auto cells = board(state.at("board"));
    const int size = static_cast<int>(cells.size());
    const auto player = natural(state.at("to_play"), WHITE, "Player");
    if (player != BLACK && player != WHITE) throw std::invalid_argument("Live player must be black or white");
    const auto passes = natural(state.at("consecutive_passes"), 2, "Consecutive passes");
    if (!state.at("komi").is_number() || !std::isfinite(state.at("komi").get<double>()))
        throw std::invalid_argument("Live komi must be finite");
    const auto komi = state.at("komi").get<double>();
    std::optional<Board> previous;
    if (!state.at("previous_board").is_null()) previous = board(state.at("previous_board"), size);
    Move last_move;
    const auto& move = data.at("last_move");
    if (!move.is_null()) {
        if (!move.is_array() || move.size() != 2) throw std::invalid_argument("Live move must be [row,column] or null");
        const auto row = natural(move.at(0), size - 1, "Move row");
        const auto column = natural(move.at(1), size - 1, "Move column");
        last_move = Point{static_cast<int>(row), static_cast<int>(column)};
    }
    validate_progress(data.at("progress"));
    return {GameState(std::move(cells), static_cast<int>(player), komi, static_cast<int>(passes), std::move(previous)),
            last_move, static_cast<int>(move_number), data.at("progress"), id, sequence, static_cast<std::int64_t>(time)};
}

TrainingLiveSnapshot read_training_live(const std::filesystem::path& path) {
    return parse_training_live(Json::parse(read_bytes(path)));
}

TrainingLiveWriter::TrainingLiveWriter(std::filesystem::path path)
    : path_(std::move(path)), session_id_(session_id()) {
    if (path_.empty() || path_.filename().empty()) throw std::invalid_argument("Training feed must name a file");
    protect_existing(path_);
}

void TrainingLiveWriter::publish(const GameState& state, Move last_move, int move_number, const Json& progress) {
    if (sequence_ == std::numeric_limits<std::uint64_t>::max()) throw std::runtime_error("Training feed sequence exhausted");
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    Json data{{"schema_version", 1}, {"kind", "training_live"}, {"session_id", session_id_},
              {"sequence", sequence_ + 1}, {"updated_at_ms", milliseconds}, {"move_number", move_number},
              {"last_move", last_move ? Json::array({last_move->row, last_move->column}) : Json(nullptr)},
              {"state", {{"board", state.board()}, {"komi", state.komi()}, {"to_play", state.to_play()},
                         {"consecutive_passes", state.consecutive_passes()},
                         {"previous_board", state.previous_board() ? Json(*state.previous_board()) : Json(nullptr)}}},
              {"progress", progress}};
    (void)parse_training_live(data);
    if (data.dump().size() + 1 > max_snapshot_bytes) throw std::invalid_argument("Training feed exceeds 1 MiB");
    protect_existing(path_);
    save_records_atomic(path_, data);
    ++sequence_;
}
} // namespace betago
