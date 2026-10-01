#include "betago/training_live.hpp"
#include <atomic>
#include <fstream>
#include <iostream>
#include <limits>
#include <thread>

namespace {
using namespace betago;
void check(bool condition, const std::string& message = "Unexpected result") {
    if (!condition) throw std::runtime_error(message);
}
template<class Function> void rejects(Function function) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Invalid training snapshot was accepted");
}
struct Suite {
    int passed = 0, failed = 0;
    template<class Function> void test(const char* name, Function function) {
        try { function(); ++passed; std::cout << "PASS training live " << name << '\n'; }
        catch (const std::exception& error) {
            ++failed; std::cerr << "FAIL training live " << name << ": " << error.what() << '\n';
        }
    }
};
std::filesystem::path file(const std::string& name) {
    auto path = std::filesystem::absolute(std::filesystem::path("results/training_live_tests") / name);
    std::filesystem::create_directories(path.parent_path());
    return path;
}
void fresh(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove(path, error);
    check(!error, "Cannot reset training feed test artifact");
}
void write(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << contents;
    check(static_cast<bool>(stream), "Cannot write training feed test artifact");
}
std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
Json progress(const char* phase = "self_play") {
    return {{"phase", phase}, {"message", "Watching accepted moves"},
            {"iteration", 3}, {"game", 1}, {"games_total", 4}};
}
Json fixture() {
    const auto state = GameState::new_game(3);
    return {{"schema_version", 1}, {"kind", "training_live"}, {"session_id", "fixture"},
            {"sequence", 1}, {"updated_at_ms", 1750000000000LL}, {"move_number", 0}, {"last_move", nullptr},
            {"state", {{"board", state.board()}, {"komi", state.komi()}, {"to_play", BLACK},
                       {"consecutive_passes", 0}, {"previous_board", nullptr}}},
            {"progress", progress()}};
}
} // namespace

int run_training_live_tests() {
    Suite suite;
    suite.test("captures and immutable simple-ko history survive file round trip", [] {
        const auto path = file("ko live.json"); fresh(path);
        Board cells{{0, 1, 2, 0, 0}, {1, 2, 0, 2, 0}, {0, 1, 2, 0, 0},
                    {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}};
        const auto before = GameState(cells, BLACK, 7.5);
        const auto captured = before.play(Point{1, 2});
        TrainingLiveWriter writer(path);
        writer.publish(captured, Point{1, 2}, 1, progress());
        const auto restored = read_training_live(path);
        check(restored.state == captured && restored.state.previous_board() == before.board());
        check(restored.last_move == Point{1, 2} && restored.move_number == 1 && restored.sequence == 1);
        check(restored.updated_at_ms > 0 && restored.progress == progress());
        check(captured.at({1, 1}) == EMPTY && before.at({1, 1}) == WHITE);
        rejects([&] { restored.state.play(Point{1, 1}); });
    });
    suite.test("pass and completed terminal position retain all state", [] {
        const auto path = file("passes.json"); fresh(path);
        TrainingLiveWriter writer(path);
        const auto state = GameState::new_game(9).play(PASS).play(PASS);
        writer.publish(state, PASS, 2, progress("finished"));
        const auto restored = read_training_live(path);
        check(restored.state == state && restored.state.is_terminal() && !restored.last_move);
        check(restored.state.to_play() == BLACK && restored.state.consecutive_passes() == 2);
    });
    suite.test("sequence is monotonic and a fresh writer has a new session", [] {
        const auto path = file("sessions.json"); fresh(path);
        TrainingLiveWriter writer(path);
        writer.publish(GameState::new_game(3), PASS, 0, progress());
        const auto first = read_training_live(path);
        writer.publish(GameState::new_game(3).play(Point{0, 0}), Point{0, 0}, 1, progress());
        const auto second = read_training_live(path);
        check(first.session_id == second.session_id && first.sequence == 1 && second.sequence == 2);
        TrainingLiveWriter next(path);
        next.publish(GameState::new_game(3), PASS, 0, progress());
        const auto third = read_training_live(path);
        check(third.session_id != second.session_id && third.sequence == 1);
    });
    suite.test("numeric types and ranges are checked without coercion", [] {
        for (const auto& [pointer, value] : std::vector<std::pair<std::string, Json>>{
                {"/schema_version", true}, {"/sequence", 0}, {"/sequence", -1}, {"/sequence", 1.5},
                {"/updated_at_ms", std::numeric_limits<std::uint64_t>::max()}, {"/move_number", -1},
                {"/move_number", std::numeric_limits<std::uint64_t>::max()}, {"/state/to_play", 0},
                {"/state/to_play", 2.0}, {"/state/consecutive_passes", 3}, {"/state/board/0/0", true},
                {"/state/board/0/0", 3}, {"/state/komi", "7.5"},
                {"/state/komi", std::numeric_limits<double>::infinity()},
                {"/progress/game", -1}, {"/progress/game", true},
                {"/progress/loss", -0.2}, {"/progress/value_loss", "1"},
                {"/progress/total_loss", std::numeric_limits<double>::quiet_NaN()}}) {
            auto data = fixture(); data[Json::json_pointer(pointer)] = value;
            rejects([&] { (void)parse_training_live(data); });
        }
    });
    suite.test("board, move, history and progress shape are validated", [] {
        for (const auto& [pointer, value] : std::vector<std::pair<std::string, Json>>{
                {"/kind", "arena"}, {"/session_id", ""}, {"/state/board", Json::array()},
                {"/state/board/0", Json::array({0, 0})}, {"/state/board", Board(20, std::vector<int>(20))},
                {"/state/previous_board", Board(2, std::vector<int>(2))},
                {"/last_move", Json::array({3, 0})}, {"/last_move", Json::array({0, -1})},
                {"/last_move", Json::array({0})}, {"/last_move", "pass"},
                {"/progress/phase", "paused"}, {"/progress/message", false},
                {"/progress/game", 5}, {"/progress/unknown", 0},
                {"/progress/message", std::string(16 * 1024 + 1, 'x')}}) {
            auto data = fixture(); data[Json::json_pointer(pointer)] = value;
            rejects([&] { (void)parse_training_live(data); });
        }
        auto data = fixture(); data.erase("move_number");
        rejects([&] { (void)parse_training_live(data); });
        data = fixture(); data["unexpected"] = 1;
        rejects([&] { (void)parse_training_live(data); });
        data = fixture(); data["progress"]["update"] = 2; data["progress"]["updates_total"] = 1;
        rejects([&] { (void)parse_training_live(data); });
    });
    suite.test("training losses and update progress are retained", [] {
        auto data = fixture();
        data["progress"] = {{"phase", "training"}, {"message", "Updating the network"},
                            {"update", 2}, {"updates_total", 4}, {"policy_loss", 0.8},
                            {"value_loss", 0.2}, {"total_loss", 1.0}};
        check(parse_training_live(data).progress == data.at("progress"));
    });
    suite.test("unrelated and malformed files are preserved", [] {
        const auto path = file("protected.json");
        for (const std::string& contents : {std::string("{\"kind\":\"checkpoint\"}"), std::string("not json")}) {
            write(path, contents);
            rejects([&] { TrainingLiveWriter writer(path); });
            check(read(path) == contents);
        }
        fresh(path);
        TrainingLiveWriter writer(path);
        writer.publish(GameState::new_game(3), PASS, 0, progress());
        write(path, "{\"kind\":\"arena\"}");
        rejects([&] { writer.publish(GameState::new_game(3), PASS, 0, progress()); });
        check(read(path) == "{\"kind\":\"arena\"}");
    });
    suite.test("oversized, missing and fragmented feeds are rejected", [] {
        const auto path = file("oversized.json");
        write(path, std::string(1024 * 1024 + 1, ' '));
        rejects([&] { (void)read_training_live(path); });
        rejects([&] { TrainingLiveWriter writer(path); });
        check(std::filesystem::file_size(path) == 1024 * 1024 + 1);
        write(path, "{\"schema_version\":1,");
        rejects([&] { (void)read_training_live(path); });
        fresh(path);
        rejects([&] { (void)read_training_live(path); });
        rejects([] { TrainingLiveWriter writer(std::filesystem::path{}); });
    });
    suite.test("rejected publication preserves the last complete feed and sequence", [] {
        const auto path = file("invalid publication.json"); fresh(path);
        TrainingLiveWriter writer(path);
        writer.publish(GameState::new_game(3), PASS, 0, progress());
        const auto initial = read(path);
        rejects([&] { writer.publish(GameState::new_game(3), Point{3, 0}, 1, progress()); });
        check(read(path) == initial);
        writer.publish(GameState::new_game(3), PASS, 0, progress("training"));
        check(read_training_live(path).sequence == 2);
    });
    suite.test("concurrent readers see complete snapshots during atomic replacement", [] {
        const auto path = file("atomic read.json"); fresh(path);
        TrainingLiveWriter writer(path);
        writer.publish(GameState::new_game(3), PASS, 0, progress());
        std::atomic<bool> finished{false}, reader_started{false};
        std::atomic<int> failures{0}, reads{0};
        std::thread reader([&] {
            while (!finished.load()) {
                try {
                    const auto snapshot = read_training_live(path);
                    if (snapshot.state.size() != 3 || snapshot.sequence == 0) ++failures;
                    ++reads;
                } catch (const std::exception&) { ++failures; }
                reader_started = true;
                std::this_thread::yield();
            }
        });
        while (!reader_started.load()) std::this_thread::yield();
        try {
            for (int index = 0; index < 40; ++index) {
                const auto state = GameState::new_game(3).play(Point{index % 3, (index / 3) % 3});
                writer.publish(state, Point{index % 3, (index / 3) % 3}, 1, progress());
            }
        } catch (...) { finished = true; reader.join(); throw; }
        finished = true; reader.join();
        check(reads > 0 && failures == 0, "Reader observed a partial snapshot or blocked replacement");
        check(read_training_live(path).sequence == 41);
    });
    std::cout << suite.passed << " training live tests passed, " << suite.failed << " failed\n";
    return suite.failed;
}
