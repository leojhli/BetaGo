#include "betago/teacher.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

namespace {
using namespace betago;

void check(bool condition, const std::string& message = "Unexpected teacher result") {
    if (!condition) throw std::runtime_error(message);
}

template<class Function> void rejects(Function function) {
    try { function(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Invalid teacher settings were accepted");
}

struct Suite {
    int passed = 0, failed = 0;
    template<class Function> void test(const char* name, Function function) {
        try { function(); ++passed; std::cout << "PASS teacher " << name << '\n'; }
        catch (const std::exception& error) {
            ++failed; std::cerr << "FAIL teacher " << name << ": " << error.what() << '\n';
        }
    }
};

std::filesystem::path file(const std::string& name) {
    return std::filesystem::absolute(std::filesystem::path("results/teacher_tests") / name);
}

ExternalGtpConfiguration configuration(const std::filesystem::path& executable,
                                     const std::string& name, const std::string& mode = "normal",
                                     const std::string& moves = "pass") {
    std::filesystem::create_directories(file("."));
    std::ofstream log(file(name + ".jsonl"), std::ios::trunc);
    check(static_cast<bool>(log), "Cannot initialize teacher test log");
    ExternalGtpConfiguration result;
    result.executable = std::filesystem::absolute(executable);
    result.working_directory = file(".");
    result.arguments = {"--mode", mode, "--moves", moves, "--log", file(name + ".jsonl").string(),
                        "--seed", "{seed}"};
    result.display_name = "Fake teacher";
    result.startup_ms = 2000; result.command_ms = 500; result.shutdown_ms = 100;
    return result;
}

TeacherSettings small_settings(int games = 1, int limit = 10, std::int64_t seed = 0) {
    return TeacherSettings{3, 0.5, games, limit, seed};
}

std::vector<Json> events(const std::string& name) {
    std::ifstream stream(file(name + ".jsonl"));
    check(static_cast<bool>(stream), "Cannot read teacher test log");
    std::vector<Json> result;
    std::string line;
    while (std::getline(stream, line)) if (!line.empty()) result.push_back(Json::parse(line));
    return result;
}

void check_prefix(const Json& attempt) {
    const auto moves = moves_from_json(attempt.at("moves"));
    auto state = replay_moves(moves, attempt.at("size"), attempt.at("komi"));
    check(moves.size() == attempt.at("game_length").get<std::size_t>());
    if (attempt.at("status") == "completed") {
        check(state.is_terminal());
        check(attempt.at("winner") == (state.winner() ? Json(*state.winner()) : Json(nullptr)));
    } else {
        check(attempt.at("score").is_null() && attempt.at("winner").is_null());
    }
    check(attempt.at("elapsed_seconds").get<double>() >= 0);
}
} // namespace

int run_teacher_tests(const std::filesystem::path& fake_executable) {
    Suite suite;
    suite.test("settings reject invalid sizes limits nonfinite komi and seed overflow", [] {
        TeacherSettings settings;
        settings.validate();
        for (int size : {0, 20, 26}) {
            auto invalid = settings; invalid.board_size = size; rejects([&] { invalid.validate(); });
        }
        for (int count : {0, -1}) {
            auto invalid = settings; invalid.games = count; rejects([&] { invalid.validate(); });
            invalid = settings; invalid.max_moves = count; rejects([&] { invalid.validate(); });
        }
        auto excessive = settings; excessive.max_moves = 200001;
        rejects([&] { excessive.validate(); });
        auto invalid = settings; invalid.komi = std::numeric_limits<double>::infinity();
        rejects([&] { invalid.validate(); });
        invalid.komi = std::numeric_limits<double>::quiet_NaN(); rejects([&] { invalid.validate(); });
        settings.games = 2; settings.seed = std::numeric_limits<std::int64_t>::max() - 1;
        settings.validate();
        ++settings.seed; rejects([&] { settings.validate(); });
        settings.games = 1; settings.validate();
        settings.seed = std::numeric_limits<std::int64_t>::min(); settings.games = 10; settings.validate();
    });

    suite.test("one session controls both colors without echoing its own moves", [&] {
        auto config = configuration(fake_executable, "both_colors", "normal", "A1,B1,pass,pass");
        int initial_callbacks = 0, accepted_callbacks = 0, snapshots = 0;
        auto corpus = generate_teacher_games(config, small_settings(2, 4, -7),
            [&](int game, const GameState& state, Move move, int number) {
                check(game == 0 || game == 1);
                if (number == 0) { ++initial_callbacks; check(state == GameState::new_game(3, .5)); check(!move); }
                else {
                    ++accepted_callbacks;
                    check(number >= 1 && number <= 4);
                    check(state.to_play() == (number % 2 ? WHITE : BLACK));
                }
            }, [&](const Json& current) {
                ++snapshots;
                check(current.at("attempts").size() == static_cast<std::size_t>(snapshots));
                check(current.at("games").size() == static_cast<std::size_t>(snapshots));
            });
        check(initial_callbacks == 2 && accepted_callbacks == 8 && snapshots == 2);
        check(corpus.at("summary").at("completed") == 2);
        check(corpus.at("games").size() == 2);
        int startups = 0, genmoves = 0, plays = 0, clears = 0, quits = 0;
        std::vector<std::int64_t> seeds;
        for (const auto& event : events("both_colors")) {
            if (event.at("event") == "startup") { ++startups; seeds.push_back(event.at("seed").get<std::int64_t>()); }
            if (event.at("event") != "command") continue;
            auto command = event.at("command");
            genmoves += command == "genmove"; plays += command == "play";
            clears += command == "clear_board"; quits += command == "quit";
        }
        check(startups == 2 && genmoves == 8 && plays == 0 && clears == 2 && quits == 2);
        check(seeds == std::vector<std::int64_t>({-7, -6}));
        for (const auto& attempt : corpus.at("attempts")) check_prefix(attempt);
        for (const auto& record : corpus.at("games")) {
            auto game = expert_game_from_json(record);
            auto final = replay_moves(game.moves, game.initial_state.size(), game.initial_state.komi());
            check(final.is_terminal());
            check(game.winner == final.winner().value_or(EMPTY));
            check(game.metadata.at("provenance") == "teacher_self_play");
            check(game.metadata.at("teacher_session").at("rules_verification") == "unverified_generic");
        }
        check(corpus.at("metadata").at("professional_game_data") == false);
        check(corpus.at("metadata").at("rank_calibration") == false);
    });

    suite.test("failure retains legal prefix and later seed starts a fresh successful game", [&] {
        auto config = configuration(fake_executable, "continue", "error-seed-17");
        int snapshots = 0;
        auto corpus = generate_teacher_games(config, small_settings(2, 10, 17), {},
            [&](const Json& current) {
                ++snapshots;
                check(current.at("attempts").size() == static_cast<std::size_t>(snapshots));
            });
        check(snapshots == 2 && corpus.at("games").size() == 1);
        check(corpus.at("summary").at("attempted") == 2);
        check(corpus.at("summary").at("failures") == 1 && corpus.at("summary").at("completed") == 1);
        const auto& failed = corpus.at("attempts").at(0);
        check(failed.at("status") == "failed" && failed.at("game_length") == 1);
        check(failed.at("failure").at("code") == "gtp_rejection");
        check(failed.at("game_id").is_null());
        check(corpus.at("attempts").at(1).at("seed") == 18);
        for (const auto& attempt : corpus.at("attempts")) check_prefix(attempt);
    });

    suite.test("resignation and move cutoff provide no expert labels or scores", [&] {
        auto resigned = generate_teacher_games(configuration(fake_executable, "resignation", "normal", "A1,resign"), small_settings());
        check(resigned.at("games").empty());
        const auto& attempt = resigned.at("attempts").at(0);
        check(attempt.at("status") == "resigned" && attempt.at("game_length") == 1);
        check(attempt.at("decisions").size() == 2);
        check(attempt.at("decisions").at(1).at("accepted") == false);
        check(attempt.at("decisions").at(1).at("response") == "resign");
        check(resigned.at("summary").at("resignations") == 1);
        check_prefix(attempt);
        auto truncated = generate_teacher_games(configuration(fake_executable, "truncation"), small_settings(1, 1));
        check(truncated.at("games").empty());
        check(truncated.at("attempts").at(0).at("status") == "truncated");
        check(truncated.at("summary").at("truncations") == 1);
        check_prefix(truncated.at("attempts").at(0));
    });

    suite.test("illegal teacher reply is excluded rather than repaired or replaced", [&] {
        auto corpus = generate_teacher_games(configuration(fake_executable, "illegal", "illegal"), small_settings());
        check(corpus.at("games").empty());
        const auto& attempt = corpus.at("attempts").at(0);
        check(attempt.at("status") == "failed" && attempt.at("moves").empty());
        check(attempt.at("decisions").size() == 1 && attempt.at("decisions").at(0).at("accepted") == false);
        check(attempt.at("failure").at("code") == "malformed_response");
        check_prefix(attempt);
    });

    suite.test("cleanup warning retains otherwise completed two-pass game", [&] {
        auto corpus = generate_teacher_games(configuration(fake_executable, "cleanup", "no-quit"), small_settings(1, 2));
        check(corpus.at("games").size() == 1);
        const auto& attempt = corpus.at("attempts").at(0);
        check(attempt.at("status") == "completed" && !attempt.at("cleanup_warnings").empty());
        check(attempt.at("teacher_session").at("closed") == true);
        check_prefix(attempt);
    });

    suite.test("startup failures produce snapshots and do not abort later attempts", [&] {
        auto config = configuration(fake_executable, "missing");
        config.executable = file("missing_teacher_executable.exe");
        int snapshots = 0;
        auto corpus = generate_teacher_games(config, small_settings(2), {}, [&](const Json&) { ++snapshots; });
        check(snapshots == 2 && corpus.at("summary").at("failures") == 2 && corpus.at("games").empty());
        for (const auto& attempt : corpus.at("attempts")) {
            check(attempt.at("moves").empty());
            check(attempt.at("failure").at("code") == "launch_failure");
            check_prefix(attempt);
        }
    });

    suite.test("observer failures propagate rather than becoming engine failures", [&] {
        auto config = configuration(fake_executable, "observer");
        bool caught = false;
        try {
            generate_teacher_games(config, small_settings(2),
                [](int, const GameState&, Move, int number) {
                    if (number == 1) throw std::runtime_error("observer write failed");
                });
        } catch (const std::runtime_error& error) { caught = std::string(error.what()) == "observer write failed"; }
        check(caught);
        caught = false;
        try {
            generate_teacher_games(config, small_settings(2), {},
                [](const Json&) { throw std::runtime_error("snapshot write failed"); });
        } catch (const std::runtime_error& error) { caught = std::string(error.what()) == "snapshot write failed"; }
        check(caught);
    });

    suite.test("teacher inputs are frozen across attempts and changed files are rejected", [&] {
        auto config = configuration(fake_executable, "drift");
        const auto artifact = file("drift declared artifact.txt");
        {
            std::ofstream output(artifact, std::ios::trunc);
            output << "initial teacher input\n";
            check(static_cast<bool>(output));
        }
        config.files.push_back({"test_input", artifact});
        auto corpus = generate_teacher_games(config, small_settings(2, 2), {}, [&](const Json& current) {
            if (current.at("attempts").size() == 1) {
                std::ofstream output(artifact, std::ios::trunc);
                output << "changed teacher input\n";
                check(static_cast<bool>(output));
            }
        });
        check(corpus.at("games").size() == 1 && corpus.at("summary").at("failures") == 1);
        check(corpus.at("attempts").at(1).at("failure").at("code") == "launch_failure");
        check(corpus.at("attempts").at(1).at("moves").empty());
        check(!corpus.at("metadata").at("teacher_configuration").at("declared_file_fingerprints").is_null());
    });

    std::cout << suite.passed << " teacher tests passed, " << suite.failed << " failed\n";
    return suite.failed;
}
