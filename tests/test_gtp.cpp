#include "betago/arena.hpp"
#include "betago/gtp.hpp"
#include "betago/gtp_process.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
using namespace betago;
using Clock = std::chrono::steady_clock;
void check(bool condition, const std::string& message = "Unexpected result") {
    if (!condition) throw std::runtime_error(message);
}
template<class Function> void rejects(Function function) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Invalid GTP input was accepted");
}
template<class Function> void fails(Function function, const std::string& code) {
    try { function(); } catch (const GtpFailure& error) {
        check(error.code == code, "Expected GTP failure " + code + ", got " + error.code); return;
    }
    throw std::runtime_error("Expected typed GTP failure " + code);
}
struct Suite {
    int passed = 0, failed = 0;
    template<class Function> void test(const char* name, Function function) {
        try { function(); ++passed; std::cout << "PASS GTP " << name << '\n'; }
        catch (const std::exception& error) {
            ++failed; std::cerr << "FAIL GTP " << name << ": " << error.what() << '\n';
        }
    }
};
std::filesystem::path file(const std::string& name) {
    return std::filesystem::absolute(std::filesystem::path("results/gtp_tests") / name);
}
void write(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    check(static_cast<bool>(stream), "Cannot write GTP test artifact"); stream << text;
    check(static_cast<bool>(stream), "Failed writing GTP test artifact");
}
std::vector<Json> events(const std::filesystem::path& path) {
    std::ifstream input(path); check(static_cast<bool>(input), "Cannot read fake engine event log");
    std::vector<Json> result; std::string line;
    while (std::getline(input, line)) if (!line.empty()) result.push_back(Json::parse(line));
    return result;
}
ExternalGtpConfiguration configuration(const std::filesystem::path& executable, const std::string& mode,
                                     const std::string& name, const std::string& moves = "pass") {
    ExternalGtpConfiguration config;
    config.executable = std::filesystem::absolute(executable);
    config.arguments = {"--mode", mode, "--log", file(name + ".jsonl").string(),
                        "--moves", moves, "--seed", "{seed}"};
    config.adapter = "generic"; config.display_name = "Fake Go engine";
    config.startup_ms = 1500; config.command_ms = 400; config.shutdown_ms = 100;
    config.working_directory = file(".");
    write(file(name + ".jsonl"), "");
    return config;
}
struct Process {
    std::unique_ptr<GtpProcess> value;
    GtpResponseParser parser;
    std::uint64_t next_id = 1;
    explicit Process(const std::filesystem::path& executable, std::vector<std::string> arguments = {})
        : value(make_gtp_process(std::filesystem::absolute(executable), arguments, file("."))) {}
    ~Process() { if (value) value->close(Clock::now() + std::chrono::milliseconds(100)); }
    GtpResponse request(const std::string& command, int timeout_ms = 1000) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        const auto id = next_id++;
        value->write(std::to_string(id) + " " + command + "\n", deadline);
        for (;;) {
            if (auto response = parser.take(id)) return *response;
            parser.feed(value->read(deadline));
        }
    }
    std::optional<std::string> quit(int timeout_ms = 300) {
        check(request("quit").success);
        return value->close(Clock::now() + std::chrono::milliseconds(timeout_ms));
    }
};
void same_move(const Move& first, const Move& second) { check(first == second, "GTP coordinate round-trip changed a move"); }
Json profile(const std::filesystem::path& executable, const std::string& mode = "normal") {
    return {{"schema_version", 1}, {"kind", "external_gtp_profile"}, {"display_name", "Fake engine test"},
            {"executable", std::filesystem::absolute(executable).string()},
            {"arguments", Json::array({"--mode", mode, "--seed", "{seed}"})}, {"adapter", "generic"},
            {"timeouts", {{"startup_ms", 1500}, {"command_ms", 400}, {"shutdown_ms", 100}}}};
}
bool stopped(std::uint64_t process) {
#ifdef _WIN32
    HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(process));
    if (!handle) return true;
    const bool done = WaitForSingleObject(handle, 0) == WAIT_OBJECT_0; CloseHandle(handle); return done;
#else
    (void)process; return true;
#endif
}
AgentConfiguration external_agent(const ExternalGtpConfiguration& configuration) {
    AgentConfiguration result; result.kind = "external-gtp"; result.external_gtp = configuration; return result;
}
ArenaSettings small_arena(const ExternalGtpConfiguration& config, int pairs = 1) {
    ArenaSettings settings; settings.a = external_agent(config); settings.b.kind = "random";
    settings.size = 1; settings.komi = .5; settings.max_moves = 2; settings.pairs = pairs; settings.seed = 17;
    return settings;
}
ExternalGtpConfiguration katago_configuration(const std::filesystem::path& executable, const std::string& mode,
                                             const std::string& name) {
    auto config = configuration(executable, mode, name);
    const auto model = file(name + " model.bin"), settings = file(name + " config.cfg");
    write(model, "mock model artifact\n"); write(settings, "mock configuration artifact\n");
    config.adapter = "katago"; config.files = {{"model", model}, {"config", settings}};
    config.arguments.insert(config.arguments.end(), {"gtp", "-model", model.string(), "-config", settings.string(),
        "-override-config", "maxVisits=8,allowResignation=true,ponderingEnabled=true,delayMoveScale=1,delayMoveMax=1"});
    return config;
}
} // namespace

int run_gtp_tests(const std::filesystem::path& fake_exe) {
    Suite suite;
    std::filesystem::create_directories(file("."));
    suite.test("coordinates reverse rows skip I and preserve pass on board sizes 1 through 25", [] {
        for (int size = 1; size <= 25; ++size) {
            same_move(move_from_gtp(move_to_gtp(PASS, size), size), PASS);
            for (int row = 0; row < size; ++row) for (int column = 0; column < size; ++column) {
                const Move move = Point{row, column}; const auto vertex = move_to_gtp(move, size);
                check(vertex.front() != 'I'); same_move(move_from_gtp(vertex, size), move);
                auto lower = vertex;
                for (char& value : lower) if (value >= 'A' && value <= 'Z') value = static_cast<char>(value - 'A' + 'a');
                same_move(move_from_gtp(lower, size), move);
            }
        }
        check(move_to_gtp(Point{0, 0}, 9) == "A9" && move_to_gtp(Point{8, 8}, 9) == "J1");
        for (const auto& value : {"I1", "A0", "A10", "Z1", "A1 extra", "", "resign", "A-1"})
            rejects([&] { move_from_gtp(value, 9); });
        rejects([] { move_to_gtp(Point{-1, 0}, 9); }); rejects([] { move_to_gtp(PASS, 0); });
    });
    suite.test("numbered response parser handles byte fragments split CRLF and multiple lines", [] {
        GtpResponseParser parser;
        const std::string bytes = "=12 first line\r\nsecond line\r\n\r\n";
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            parser.feed(bytes.substr(index, 1));
            if (index + 1 < bytes.size()) check(!parser.take(12));
        }
        const auto response = parser.take(12);
        check(response && response->success && response->id == 12 && response->body == "first line\nsecond line");
        check(parser.empty());
        parser.feed("?13 rejected\n\n"); const auto error = parser.take(13);
        check(error && !error->success && error->body == "rejected");
        const std::string utf8 = "caf\xc3\xa9";
        parser.feed("=14 " + utf8 + "\n\n");
        const auto unicode = parser.take(14); check(unicode && unicode->body == utf8);
    });
    suite.test("response parser rejects missing wrong overflowing ids malformed frames and oversized buffers", [] {
        for (const auto& bytes : {"= pass\n\n", "=18446744073709551616 pass\n\n",
                                 "hello\n\n", "=1 pass\rbroken\n\n", "=1x pass\n\n"}) {
            fails([&] { GtpResponseParser parser; parser.feed(bytes); parser.take(1); }, "malformed_response");
        }
        fails([] { GtpResponseParser parser; parser.feed("=2 pass\n\n"); parser.take(1); }, "wrong_response_id");
        fails([] { GtpResponseParser parser; parser.feed(std::string(1024 * 1024 + 1, 'x')); }, "malformed_response");
        fails([] {
            GtpResponseParser parser; parser.feed("=1 " + std::string(1, static_cast<char>(0xff)) + "\n\n");
            parser.take(1);
        }, "malformed_response");
        GtpResponseParser pending; pending.feed("=1 pass\n"); check(!pending.take(1));
    });
    suite.test("profile validation resolves relative paths freezes artifact identities and rejects invalid fields", [&] {
        const auto profile_path = file("profile with spaces.json");
        const auto artifact = file("model artifact.bin"); write(artifact, "model placeholder\n");
        auto data = profile(fake_exe);
        data["executable"] = std::filesystem::relative(std::filesystem::absolute(fake_exe), profile_path.parent_path()).string();
        data["working_directory"] = ".";
        data["files"] = Json::array({{{"role", "model"}, {"path", artifact.filename().string()}}});
        data["seeded_stochastic"] = true; write(profile_path, data.dump(2));
        const auto loaded = load_external_gtp_profile(profile_path);
        check(std::filesystem::equivalent(loaded.executable, fake_exe) && !loaded.seeded_stochastic());
        check(loaded.declared_file_fingerprints == external_gtp_identity(loaded));
        write(artifact, "changed artifact\n"); check(loaded.declared_file_fingerprints != external_gtp_identity(loaded));
        auto documented = data; documented["seed_documentation"] = "Fake engine test receives --seed as a literal integer argument.";
        write(file("documented profile.json"), documented.dump());
        check(load_external_gtp_profile(file("documented profile.json")).seeded_stochastic());
        documented["arguments"] = Json::array({"--mode", "normal"});
        write(file("documented profile.json"), documented.dump());
        check(!load_external_gtp_profile(file("documented profile.json")).seeded_stochastic());
        for (const auto& key : {"schema_version", "executable", "adapter", "arguments", "display_name"}) {
            auto bad = data; bad.erase(key); write(file("invalid profile.json"), bad.dump());
            rejects([&] { load_external_gtp_profile(file("invalid profile.json")); });
        }
        for (const auto& key : {"startup_ms", "command_ms", "shutdown_ms"}) {
            auto bad = data; bad["timeouts"][key] = 0; write(file("invalid profile.json"), bad.dump());
            rejects([&] { load_external_gtp_profile(file("invalid profile.json")); });
        }
        auto bad = data; bad["arguments"] = Json::array({17}); write(file("invalid profile.json"), bad.dump());
        rejects([&] { load_external_gtp_profile(file("invalid profile.json")); });
        for (int mutation = 0; mutation < 3; ++mutation) {
            auto unknown = data;
            if (mutation == 0) unknown["startup_command"] = "not an allowed field";
            if (mutation == 1) unknown["timeouts"]["extra"] = 1;
            if (mutation == 2) unknown["files"][0]["extra"] = "not an allowed field";
            write(file("unknown profile.json"), unknown.dump()); rejects([&] { load_external_gtp_profile(file("unknown profile.json")); });
        }
        const auto elsewhere = file("different working directory"); std::filesystem::create_directories(elsewhere);
        auto relative = data; relative["working_directory"] = elsewhere.string();
        relative["arguments"].push_back("--tag"); relative["arguments"].push_back(artifact.filename().string());
        write(file("relative argv profile.json"), relative.dump());
        const auto bound = load_external_gtp_profile(file("relative argv profile.json"));
        check(std::filesystem::equivalent(std::filesystem::path(bound.arguments.back()), artifact) &&
              std::filesystem::equivalent(bound.working_directory, elsewhere), "Declared relative argv path changed meaning with engine working directory");
    });
#ifdef _WIN32
    suite.test("native process preserves literal argv spaces quotes and trailing slashes", [&] {
        const auto log = file("literal argv.jsonl"); write(log, "");
        const std::string tag = "spaces ; literal \"quoted\" path\\tail\\";
        Process process(fake_exe, {"--log", log.string(), "--tag", tag});
        check(process.request("name").body == "Fake Go engine"); check(!process.quit());
        const auto logged = events(log); check(!logged.empty() && logged.front().at("tag") == tag);
    });
    suite.test("native transport parses fragmented LF and CRLF replies and drains bounded stderr", [&] {
        Process process(fake_exe);
        check(process.request("test_fragment").body == "first line\nsecond line");
        check(process.request("test_crlf").body == "first line\nsecond line");
        check(process.request("test_stderr", 3000).body == "stderr drained");
        check(!process.value->stderr_text().empty() && process.value->stderr_text().size() <= 64 * 1024,
              "Stderr diagnostics were absent or unbounded");
        const auto error = process.request("test_error"); check(!error.success && error.body.find("rejected") != std::string::npos);
        check(!process.quit());
    });
    suite.test("native transport faults and deadlines terminate each owned process", [&] {
        for (const auto& command : {"test_wrong_id", "test_missing_id", "test_malformed", "test_oversize", "test_exit", "test_hang"}) {
            Process process(fake_exe);
            check(process.request("name").success);
            const std::string token = command;
            const auto code = token == "test_wrong_id" ? "wrong_response_id" : token == "test_hang" ? "timeout" :
                              token == "test_exit" ? "unexpected_exit" : "malformed_response";
            fails([&] { process.request(command, token == "test_oversize" ? 2000 : 150); }, code);
            process.value->close(Clock::now() + std::chrono::milliseconds(100));
        }
        Process blocked(fake_exe, {"--mode", "startup-hang"});
        fails([&] { blocked.value->write(std::string(256 * 1024, 'x'), Clock::now() + std::chrono::milliseconds(150)); }, "timeout");
        blocked.value->close(Clock::now() + std::chrono::milliseconds(100));
    });
    suite.test("session synchronization sends opponent moves exactly once and never repeats own genmove", [&] {
        auto config = configuration(fake_exe, "fragment", "synchronization", "A1,pass");
        ExternalGtpSession session(config, -71, 3, .5); session.start();
        GameState state = GameState::new_game(3, .5);
        const auto own = session.genmove(state); check(own == Move(Point{2, 0}));
        session.accepted_move(BLACK, own, true); state = state.play(own);
        const Move opponent = Point{0, 2}; session.accepted_move(WHITE, opponent, false); state = state.play(opponent);
        const auto pass = session.genmove(state); check(!pass);
        session.accepted_move(BLACK, pass, true); state = state.play(pass);
        session.accepted_move(WHITE, PASS, false); state = state.play(PASS); check(state.is_terminal());
        session.shutdown();
        const auto logged = events(file("synchronization.jsonl")); int plays = 0, genmoves = 0, clears = 0;
        for (const auto& item : logged) if (item.at("event") == "command") {
            const auto command = item.at("command");
            plays += command == "play"; genmoves += command == "genmove"; clears += command == "clear_board";
        }
        check(plays == 2 && genmoves == 2 && clears == 1);
        check(logged.front().at("seed") == -71);
        const auto metadata = session.metadata();
        check(metadata.at("seed") == -71 && metadata.at("board_size") == 3 && metadata.at("komi") == .5 && metadata.at("started") == true);
        check(metadata.at("engine_name") == "Fake Go engine" && metadata.at("engine_version") == "test-1.0");
        check(metadata.at("actual_arguments").get<std::vector<std::string>>().back() == "-71");
        check(metadata.at("configuration").at("executable").is_string() && metadata.at("process").is_object());
        check(metadata.at("engine_seed") == -71 && metadata.at("seed_status") == "argument_substitution");
    });
    suite.test("unconfigured external seeds are recorded explicitly without uncertainty claims", [&] {
        auto config = configuration(fake_exe, "normal", "seed-unconfigured"); config.arguments.resize(config.arguments.size() - 2);
        ExternalGtpSession engine(config, 37, 1, .5); engine.start();
        const auto metadata = engine.metadata();
        check(metadata.at("seed") == 37 && metadata.at("engine_seed").is_null() && metadata.at("seed_status") == "unconfigured" &&
              metadata.at("seeded_stochastic") == false && metadata.at("seed_integration").at("eligible") == false);
        check(metadata.at("effective_rules").is_null() && metadata.at("rules_verification").is_string() && metadata.at("rules_note").is_string());
        engine.shutdown(); const auto closed = engine.metadata();
        check(closed.at("startup_seconds").get<double>() >= 0 && closed.at("shutdown_seconds").get<double>() >= 0);
        check(events(file("seed-unconfigured.jsonl")).front().at("seed").is_null());
    });
    suite.test("KataGo adapter verifies effective rules komi parameters models and forced startup overrides", [&] {
        auto config = katago_configuration(fake_exe, "normal", "kata-positive");
        ExternalGtpSession session(config, 31, 3, .5); session.start();
        const auto data = session.metadata();
        check(data.at("effective_rules").at("ko") == "SIMPLE" && data.at("effective_rules").at("scoring") == "AREA" &&
              data.at("effective_rules").at("suicide") == false && data.at("effective_komi") == .5);
        check(data.at("effective_parameters").at("allowResignation") == false &&
              data.at("effective_parameters").at("ponderingEnabled") == false &&
              data.at("effective_parameters").at("delayMoveScale") == 0 && data.at("effective_parameters").at("delayMoveMax") == 0);
        check(data.at("models").is_array() && data.at("models")[0].at("name") == "fake-model");
        const auto arguments = data.at("actual_arguments").get<std::vector<std::string>>();
        const auto flag = std::find(arguments.begin(), arguments.end(), "-override-config");
        check(flag != arguments.end() && std::count(arguments.begin(), arguments.end(), "-override-config") == 1 && flag + 1 != arguments.end());
        check((flag + 1)->rfind("allowResignation=false") > (flag + 1)->find("allowResignation=true"));
        session.shutdown();
        auto undeclared = config; undeclared.files.erase(undeclared.files.begin()); rejects([&] { undeclared.validate(); });
        for (const auto& mode : {"kata-bad-rules", "kata-bad-komi", "kata-bad-params", "kata-bad-models"}) {
            auto bad = katago_configuration(fake_exe, mode, std::string("kata-") + mode);
            ExternalGtpSession engine(bad, 0, 3, .5);
            fails([&] { engine.start(); }, std::string(mode) == "kata-bad-models" ? "malformed_response" : "unsupported_capability");
            engine.shutdown();
        }
        auto bad_ack = katago_configuration(fake_exe, "bad-rules-ack", "kata-bad-rules-ack");
        ExternalGtpSession engine(bad_ack, 0, 3, .5); fails([&] { engine.start(); }, "malformed_response"); engine.shutdown();
    });
    suite.test("startup rejects empty engine names and nonempty board setup acknowledgements", [&] {
        for (const auto& mode : {"empty-name", "bad-setup-ack"}) {
            auto config = configuration(fake_exe, mode, std::string("startup-") + mode);
            ExternalGtpSession engine(config, 0, 3, .5);
            fails([&] { engine.start(); }, "malformed_response"); engine.shutdown();
        }
    });
    suite.test("missing executable becomes a typed launch failure and arena continues all attempts", [&] {
        auto config = configuration(fake_exe, "normal", "missing-executable"); config.executable = file("does not exist.exe");
        ExternalGtpSession session(config, 0, 1, .5); fails([&] { session.start(); }, "launch_failure"); session.shutdown();
        const auto data = run_arena(small_arena(config, 2));
        check(data.at("games").size() == 4 && data.at("summary").at("failed_games") == 4);
        for (const auto& game : data.at("games")) {
            check(game.at("failure").at("code") == "launch_failure" && game.at("game_length") == 0 && game.at("score").is_null());
            check(game.at("external_sessions").at("a").at("seed") == game.at("a_seed"));
        }
        save_records_atomic(file("missing executable arena.json"), data); check(load_records(file("missing executable arena.json")) == data);
    });
    suite.test("declared artifact drift prevents launch and retains the prepared identity", [&] {
        auto config = configuration(fake_exe, "normal", "artifact-drift");
        const auto artifact = file("drift model.bin"); write(artifact, "first bytes\n");
        config.files = {{"model", artifact}}; config.declared_file_fingerprints = external_gtp_identity(config);
        write(artifact, "different bytes\n");
        ExternalGtpSession session(config, 0, 1, .5); fails([&] { session.start(); }, "launch_failure"); session.shutdown();
        check(events(file("artifact-drift.jsonl")).empty(), "Engine was launched after its declared artifact changed");
    });
    suite.test("startup failures rejected responses resignation and invalid vertices remain distinct failures", [&] {
        for (const auto& mode : {"startup-exit", "startup-hang"}) {
            auto config = configuration(fake_exe, mode, std::string("startup-") + mode); config.startup_ms = 200;
            ExternalGtpSession session(config, 0, 3, .5);
            fails([&] { session.start(); }, std::string(mode) == "startup-hang" ? "timeout" : "unexpected_exit"); session.shutdown();
        }
        for (const auto& mode : {"error", "wrong-id", "missing-id", "malformed", "oversize", "hang-genmove", "illegal"}) {
            auto config = configuration(fake_exe, mode, std::string("session-") + mode);
            ExternalGtpSession session(config, 0, 3, .5); session.start();
            const std::string token = mode;
            const auto code = token == "error" ? "gtp_rejection" : token == "wrong-id" ? "wrong_response_id" :
                              token == "hang-genmove" ? "timeout" : "malformed_response";
            fails([&] { session.genmove(GameState::new_game(3, .5)); }, code); session.shutdown();
        }
        auto config = configuration(fake_exe, "resign", "session-resign");
        ExternalGtpSession session(config, 0, 3, .5); session.start(); bool resignation = false;
        try { session.genmove(GameState::new_game(3, .5)); } catch (const GtpResignation&) { resignation = true; }
        check(resignation, "Resignation was not explicitly distinguished from pass or engine failure"); session.shutdown();
    });
    suite.test("syntactically valid occupied suicide and immediate ko moves fail local legality", [&] {
        auto attempt = [&](const std::string& mode, int size, const std::vector<Move>& prefix) {
            auto config = configuration(fake_exe, mode, "local-" + mode);
            ExternalGtpSession engine(config, 0, size, .5); engine.start();
            GameState state = GameState::new_game(size, .5);
            for (Move move : prefix) { engine.accepted_move(state.to_play(), move, false); state = state.play(move); }
            fails([&] { engine.genmove(state); }, "illegal_move"); engine.shutdown();
        };
        attempt("occupied", 3, {Point{2, 0}});
        attempt("suicide", 3, {Point{2, 0}, Point{2, 1}, Point{2, 2}, Point{1, 0},
                                Point{0, 0}, Point{0, 1}, Point{0, 2}, Point{1, 2}});
        attempt("ko", 5, {Point{0, 1}, Point{0, 2}, Point{1, 0}, Point{1, 1}, Point{2, 1},
                           Point{1, 3}, PASS, Point{2, 2}, Point{1, 2}});
    });
    suite.test("unexpected parent exit is detected even when a child inherits response pipes", [&] {
        const auto child = file("inherited child pid.txt"); write(child, "");
        auto config = configuration(fake_exe, "exit-child", "exit-inherited-child");
        config.arguments.insert(config.arguments.end(), {"--child-pid", child.string()});
        ExternalGtpSession engine(config, 0, 3, .5); engine.start();
        std::ifstream input(child); std::uint64_t child_pid = 0; input >> child_pid; check(child_pid > 0);
        fails([&] { engine.genmove(GameState::new_game(3, .5)); }, "unexpected_exit"); engine.shutdown();
        const auto deadline = Clock::now() + std::chrono::milliseconds(500);
        while (!stopped(child_pid) && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        check(stopped(child_pid), "Inherited-pipe child survived cleanup after its parent exited");
    });
    suite.test("shutdown deadlines report forced cleanup and owned child trees stop", [&] {
        { auto config = configuration(fake_exe, "hang-quit", "cleanup-hang");
          ExternalGtpSession session(config, 0, 1, .5); session.start();
          const auto warning = session.shutdown(); check(!warning.is_null() && !warning.empty()); }
#ifdef _WIN32
        const auto child = file("child pid.txt"); write(child, "");
        auto config = configuration(fake_exe, "spawn-child", "cleanup-tree");
        config.arguments.insert(config.arguments.end(), {"--child-pid", child.string()});
        ExternalGtpSession session(config, 0, 1, .5); session.start();
        std::ifstream input(child); std::uint64_t child_pid = 0; input >> child_pid; check(child_pid > 0);
        session.shutdown();
        const auto deadline = Clock::now() + std::chrono::milliseconds(500);
        while (!stopped(child_pid) && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        check(stopped(child_pid), "Owned fake-engine child survived session cleanup");
#endif
    });
    suite.test("external arena starts a fresh process per color game and preserves identity seeds", [&] {
        auto config = configuration(fake_exe, "normal", "arena-paired");
        const auto data = run_arena(small_arena(config, 2));
        check(data.at("schema_version") == 2 && data.at("games").size() == 4);
        check(data.at("summary").at("completed_games") == 4 && data.at("summary").at("complete_pairs") == 2);
        check(data.at("summary").at("agents").at("a").at("paired_score_rate_95").is_null(),
              "Undocumented external randomness was treated as independent seed-pair uncertainty");
        std::set<std::uint64_t> processes; std::vector<std::int64_t> seeds;
        for (const auto& item : events(file("arena-paired.jsonl"))) if (item.at("event") == "startup") {
            processes.insert(item.at("pid").get<std::uint64_t>()); seeds.push_back(item.at("seed").get<std::int64_t>());
        }
        check(processes.size() == 4 && seeds == std::vector<std::int64_t>{17, 17, 19, 19});
        for (std::size_t index = 0; index < data.at("games").size(); ++index) {
            const auto& game = data.at("games")[index];
            check(game.at("status") == "completed" && game.at("game_length") == 2 && game.at("failure").is_null());
            check(game.at("black_agent") == (index % 2 == 0 ? "a" : "b"));
            check(game.at("external_sessions").at("a").at("seed") == game.at("a_seed"));
        }
        save_records_atomic(file("paired arena.json"), data);
        const auto logged_count = events(file("arena-paired.jsonl")).size();
        check(load_records(file("paired arena.json")) == data);
        check(events(file("arena-paired.jsonl")).size() == logged_count, "Offline replay launched an external engine");
    });
    suite.test("failed external attempts retain legal prefixes persist incrementally and continue later pairs", [&] {
        auto settings = small_arena(configuration(fake_exe, "error", "arena-failed"), 2);
        std::size_t snapshots = 0;
        const auto data = run_arena(settings, Json::object(), {}, {}, [&](const Json& report) {
            ++snapshots; check(report.at("games").size() == snapshots);
            save_records_atomic(file("incremental failures.json"), report);
        });
        check(snapshots == 4 && data.at("summary").at("failed_games") == 4 && data.at("summary").at("completed_games") == 0);
        check(data.at("summary").at("agents").at("a").at("score_rate").is_null());
        for (std::size_t index = 0; index < data.at("games").size(); ++index) {
            const auto& game = data.at("games")[index];
            check(game.at("status") == "failed" && game.at("score").is_null() && game.at("winner").is_null());
            check(game.at("failure").at("actor") == "a" && game.at("failure").at("code") == "gtp_rejection");
            check(game.at("game_length") == (index % 2 == 0 ? 0 : 1));
        }
        check(load_records(file("incremental failures.json")) == data);
        for (const auto& field : {"winner", "game_length", "a_seed", "status"}) {
            auto corrupt = data;
            if (std::string(field) == "status") corrupt["games"][0][field] = "completed";
            else corrupt["games"][0][field] = 99;
            save_records_atomic(file("corrupt arena.json"), corrupt);
            rejects([&] { load_records(file("corrupt arena.json")); });
        }
    });
    suite.test("invalid UTF-8 engine diagnostics preserve failed attempts and atomic offline replay", [&] {
        auto settings = small_arena(configuration(fake_exe, "binary-diagnostics", "arena-binary"), 2);
        std::size_t snapshots = 0;
        const auto saved_path = file("binary diagnostic arena.json");
        const auto data = run_arena(settings, Json::object(), {}, {}, [&](const Json& report) {
            ++snapshots; check(report.at("games").size() == snapshots);
            save_records_atomic(saved_path, report);
        });
        check(snapshots == 4 && data.at("summary").at("failed_games") == 4 &&
              data.at("summary").at("completed_games") == 0 && data.at("summary").at("complete_pairs") == 0);
        check(data.at("summary").at("agents").at("a").at("score_rate").is_null());
        check(data.at("diagnostics_encoding").is_string());
        for (const auto& game : data.at("games")) {
            check(game.at("status") == "failed" && game.at("score").is_null() && game.at("winner").is_null());
            check(game.at("failure").at("code") == "malformed_response" && game.at("failure").at("actor") == "a");
            check(game.at("failure").at("diagnostics").get<std::string>().find(static_cast<char>(0xff)) != std::string::npos,
                  "Fake engine did not exercise invalid UTF-8 stdout diagnostics");
        }
        rejects([&] { data.dump(); });
        const auto sanitized = Json::parse(data.dump(2, ' ', false, Json::error_handler_t::replace));
        const auto event_count = events(file("arena-binary.jsonl")).size();
        const auto loaded = load_records(saved_path); check(loaded == sanitized);
        check(loaded.at("games")[0].at("failure").at("diagnostics").get<std::string>().find("\xef\xbf\xbd") != std::string::npos);
        check(events(file("arena-binary.jsonl")).size() == event_count, "Offline replay launched an external engine");
    });
    suite.test("resignation and truncation remain unscored and excluded from complete-pair bounds", [&] {
        const auto resigned = run_arena(small_arena(configuration(fake_exe, "resign", "arena-resigned")));
        check(resigned.at("summary").at("resigned_games") == 2 && resigned.at("summary").at("completed_games") == 0);
        for (const auto& game : resigned.at("games"))
            check(game.at("status") == "resigned" && game.at("resigned_by") == "a" &&
                  game.at("score").is_null() && game.at("winner").is_null());
        for (const auto* identity : {"a", "b"}) {
            const auto& totals = resigned.at("summary").at("agents").at(identity);
            check(totals.at("wins") == 0 && totals.at("losses") == 0 && totals.at("draws") == 0 &&
                  totals.at("score_rate").is_null() && totals.at("paired_score_rate_95").is_null());
        }
        save_records_atomic(file("resignation arena.json"), resigned); check(load_records(file("resignation arena.json")) == resigned);
        auto settings = small_arena(configuration(fake_exe, "normal", "arena-truncated")); settings.max_moves = 1;
        const auto truncated = run_arena(settings);
        check(truncated.at("summary").at("truncated_games") == 2 && truncated.at("summary").at("complete_pairs") == 0);
        for (const auto& game : truncated.at("games")) check(game.at("score").is_null() && game.at("winner").is_null());
        save_records_atomic(file("truncated arena.json"), truncated); check(load_records(file("truncated arena.json")) == truncated);
    });
    suite.test("cleanup warnings preserve valid scored outcomes while recording forced shutdown", [&] {
        const auto data = run_arena(small_arena(configuration(fake_exe, "hang-quit", "arena-cleanup")));
        check(data.at("summary").at("completed_games") == 2 && data.at("summary").at("failed_games") == 0);
        for (const auto& game : data.at("games")) {
            check(game.at("status") == "completed" && !game.at("score").is_null() && !game.at("cleanup_warnings").empty());
            check(game.at("cleanup_warnings")[0].at("actor") == "a");
        }
        save_records_atomic(file("cleanup arena.json"), data); check(load_records(file("cleanup arena.json")) == data);
    });
    suite.test("decision failures after accepted placements preserve prefixes and do not halt later scored pairs", [&] {
        auto settings = small_arena(configuration(fake_exe, "error-seed-17", "arena-prefix", "A1"), 2);
        settings.size = 3; settings.max_moves = 6;
        int factory_calls = 0;
        ArenaAgentFactory factory = [&](const AgentConfiguration& config, std::int64_t) -> ArenaAgent {
            check(config.kind == "random", "Builtin callback factory bypassed an external engine session"); ++factory_calls;
            return [calls = 0](const GameState& state) mutable -> ArenaDecision {
                ++calls;
                if (calls > 1) return {PASS, std::nullopt};
                return {state.legal_moves().front(), std::nullopt};
            };
        };
        const auto data = run_arena(settings, Json::object(), {}, factory);
        check(factory_calls == 4);
        check(data.at("summary").at("failed_games") == 2 && data.at("summary").at("completed_games") == 2 &&
              data.at("summary").at("complete_pairs") == 1);
        check(data.at("games")[0].at("game_length") == 2 && data.at("games")[1].at("game_length") == 3);
        for (int index : {0, 1}) {
            const auto& game = data.at("games")[index];
            check(game.at("decisions").size() == game.at("moves").size() + 1 &&
                  game.at("decisions").back().at("accepted") == false);
            check(game.at("failure").at("code") == "gtp_rejection" && game.at("score").is_null());
        }
        save_records_atomic(file("prefix arena.json"), data); check(load_records(file("prefix arena.json")) == data);
        auto corrupt = data; corrupt["games"][0]["decisions"][0]["accepted"] = false;
        save_records_atomic(file("bad prefix.json"), corrupt); rejects([&] { load_records(file("bad prefix.json")); });
    });
    suite.test("failed final synchronization retains a terminal legal prefix without awarding an outcome", [&] {
        const auto data = run_arena(small_arena(configuration(fake_exe, "reject-terminal-play", "arena-final-sync")));
        const auto& failed = data.at("games")[0];
        check(failed.at("status") == "failed" && failed.at("game_length") == 2 && failed.at("score").is_null() && failed.at("winner").is_null());
        check(replay_moves(moves_from_json(failed.at("moves")), 1, .5).is_terminal());
        check(failed.at("failure").at("actor") == "a" && failed.at("failure").at("code") == "gtp_rejection" &&
              failed.at("failure").at("command").get<std::string>().starts_with("play "));
        check(failed.at("decisions").size() == 2 && failed.at("decisions").back().at("accepted") == true);
        check(data.at("summary").at("failed_games") == 1 && data.at("summary").at("completed_games") == 1 &&
              data.at("summary").at("complete_pairs") == 0);
        save_records_atomic(file("final sync arena.json"), data); check(load_records(file("final sync arena.json")) == data);
    });
#else
    suite.test("unsupported native launching reports a typed Windows platform failure", [&] {
        fails([&] { make_gtp_process(std::filesystem::absolute(fake_exe), {}, file(".")); }, "launch_failure");
    });
#endif
    std::cout << suite.passed << " GTP passed, " << suite.failed << " failed\n";
    return suite.failed;
}
