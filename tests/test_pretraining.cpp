#include "betago/pretraining.hpp"
#include "betago/dataset.hpp"
#include "betago/selfplay.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

namespace {
using namespace betago;
void check(bool value, const std::string& message = "Unexpected expert pretraining result") {
    if (!value) throw std::runtime_error(message);
}
void close(double actual, double expected, double tolerance = 1e-10) {
    check(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, "Expert metric differs");
}
template<class Function> void rejects(Function function) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Invalid expert input was accepted");
}
struct Suite {
    int passed = 0, failed = 0;
    template<class Function> void test(const char* name, Function function) {
        try { function(); ++passed; std::cout << "PASS PRETRAINING " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL PRETRAINING " << name << ": " << error.what() << '\n'; }
    }
};
std::filesystem::path directory() {
    static unsigned counter = 0;
    const auto path = std::filesystem::absolute(std::filesystem::path("results/pretraining_tests") /
        ("run-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++counter)));
    check(std::filesystem::create_directories(path));
    return path;
}
std::string bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary); check(static_cast<bool>(stream));
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
Json read(const std::filesystem::path& path) { return Json::parse(bytes(path)); }
ExpertGame game(std::vector<Move> moves, int winner = BLACK, std::string source = "test-expert") {
    return {GameState::new_game(3, .5), std::move(moves), winner, std::move(source), Json::object()};
}
std::vector<ExpertGame> corpus() {
    return {game({Point{0, 0}, Point{2, 2}, PASS, PASS}, BLACK, "diagonal"),
        game({Point{0, 0}, Point{0, 1}, PASS, PASS}, WHITE, "adjacent"),
        game({PASS, Point{1, 1}, PASS, PASS}, EMPTY, "pass-and-draw")};
}
PretrainingSettings settings() {
    PretrainingSettings result;
    result.epochs = 3; result.batch_size = 8; result.validation_fraction = .34;
    result.network = {3, 1, 2}; result.optimizer = {.03, .7, .0001}; result.seed = -42;
    return result;
}
Point transform(Point p, int n, int symmetry) {
    switch (symmetry) {
    case 0: return p;
    case 1: return {p.column, n - 1 - p.row};
    case 2: return {n - 1 - p.row, n - 1 - p.column};
    case 3: return {n - 1 - p.column, p.row};
    case 4: return {p.row, n - 1 - p.column};
    case 5: return {n - 1 - p.column, n - 1 - p.row};
    case 6: return {n - 1 - p.row, p.column};
    default: return {p.column, p.row};
    }
}
Board transform(const Board& input, int symmetry) {
    const int n = static_cast<int>(input.size()); Board result(n, std::vector<int>(n));
    for (int row = 0; row < n; ++row) for (int column = 0; column < n; ++column) {
        const auto target = transform({row, column}, n, symmetry); result[target.row][target.column] = input[row][column];
    }
    return result;
}
bool encoded_equal(const EncodedPosition& first, const EncodedPosition& second) {
    return first.board_size == second.board_size && first.features == second.features &&
        first.legal_actions == second.legal_actions && first.terminal == second.terminal;
}
} // namespace

int run_pretraining_tests() {
    Suite suite;
    suite.test("expert moves use reported outcome from each player perspective including pass and draw", [] {
        const auto examples = expert_examples(corpus().front());
        check(examples.size() == 4);
        for (std::size_t i = 0; i < examples.size(); ++i) close(examples[i].value, i % 2 == 0 ? 1.0 : -1.0);
        check(examples[2].policy[9] == 1 && examples[3].policy[9] == 1);
        check(examples[3].input.features[3 * 9] == 1);
        for (const auto& sample : expert_examples(corpus().back())) close(sample.value, 0);
    });
    suite.test("all D4 augmentations preserve exact encoded legality ko history pass komi and outcome", [] {
        Board cells{{0, 1, 2, 0, 0}, {1, 2, 0, 2, 0}, {0, 1, 2, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}};
        const auto state = GameState(cells, BLACK, 2.5).play(Point{1, 2});
        ExpertGame expert{state, {PASS, Point{4, 0}, PASS, PASS}, WHITE, "ko-fixture", Json::object()};
        const auto original = expert_examples(expert).front();
        check(original.input.features[2 * 25 + 6] == 1 && !original.input.legal_actions[6]);
        for (int symmetry = 0; symmetry < 8; ++symmetry) {
            const auto sample = augment_expert_example(original, symmetry);
            const auto expected = encode_position(GameState(transform(state.board(), symmetry), state.to_play(), state.komi(),
                state.consecutive_passes(), transform(*state.previous_board(), symmetry)));
            check(encoded_equal(sample.input, expected));
            check(sample.policy[25] == 1 && sample.value == original.value);
            const auto ko = transform({1, 1}, 5, symmetry);
            check(sample.input.features[2 * 25 + ko.row * 5 + ko.column] == 1);
        }
        const auto placement = expert_examples(corpus().front()).front();
        for (int symmetry = 0; symmetry < 8; ++symmetry) {
            const auto sample = augment_expert_example(placement, symmetry);
            const auto target = transform({0, 0}, 3, symmetry);
            check(sample.policy[target.row * 3 + target.column] == 1);
        }
        rejects([&] { augment_expert_example(original, -1); });
        rejects([&] { augment_expert_example(original, 8); });
    });
    suite.test("whole game splits exclude exact and mirrored clones before augmentation", [] {
        auto games = corpus();
        auto duplicate = games.front(); duplicate.source = "another-download"; duplicate.metadata = {{"comment", "same game"}};
        games.push_back(duplicate);
        games.push_back(game({Point{0, 2}, Point{2, 0}, PASS, PASS}, BLACK, "mirrored-copy"));
        const auto output = directory();
        const auto report = pretrain_expert_games(games, settings(), std::nullopt, output);
        check(report.at("received_games") == 5 && report.at("unique_games") == 3 && report.at("duplicate_count") == 2);
        std::set<std::string> train_ids, validation_ids, group_ids;
        for (const auto& record : report.at("split").at("training_games")) {
            train_ids.insert(record.at("id").get<std::string>()); check(group_ids.insert(record.at("split_identity").get<std::string>()).second);
            check(record.at("examples") == record.at("moves").get<std::size_t>() * 8);
        }
        for (const auto& record : report.at("split").at("validation_games")) {
            validation_ids.insert(record.at("id").get<std::string>()); check(!train_ids.contains(record.at("id").get<std::string>()));
            check(group_ids.insert(record.at("split_identity").get<std::string>()).second);
            check(record.at("examples") == record.at("moves"));
        }
        check(train_ids.size() == 1 && validation_ids.size() == 2);
        const auto training = load_dataset(output / "train.json"), validation = load_dataset(output / "validation.json");
        check(training.size() == 32 && validation.size() == 8);
        check(read(output / "train.json").at("provenance").at("kind") == "expert_games");
        for (const auto& sample : training) sample.validate();
        for (const auto& sample : validation) sample.validate();
        auto restored = PolicyValueNetwork::load(output / "model.json");
        SelfPlaySettings play; play.board_size = 3; play.komi = .5; play.max_moves = 2; play.search.simulations = 1;
        play.temperature = 0; play.root_uniform_mix = 0;
        const auto generated = run_self_play(std::make_shared<const PolicyValueNetwork>(restored), play, 7);
        check(generated.record.at("moves").size() == 2);
    });
    suite.test("checkpoint architecture optimizer history and source bytes remain intact and selection uses heldout minimum", [] {
        const auto output = directory(), source_dir = directory();
        const auto source = source_dir / "source.json";
        PolicyValueNetwork original({3, 2, 3}, 13);
        original.train(); original.train_batch(expert_examples(corpus().front()), {.01, .5, 0}); original.train(false);
        original.save(source); const auto original_bytes = bytes(source);
        auto options = settings(); options.augment = false; options.epochs = 5;
        options.optimizer.learning_rate = .6;
        int observed_epochs = 0;
        const auto report = pretrain_expert_games(corpus(), options, source, output, [&](int epoch, const Json& current) {
            check(epoch == ++observed_epochs && current.at("epoch") == epoch);
        });
        check(bytes(source) == original_bytes && report.at("initial_training_steps") == 1 && observed_epochs == 5);
        check(report.at("settings").at("network").at("channels") == 2 && report.at("settings").at("network").at("value_hidden") == 3);
        double minimum = report.at("initial").at("validation").at("combined_loss"); int selected = 0;
        for (const auto& current : report.at("history")) {
            const double loss = current.at("validation").at("combined_loss");
            const bool better = loss < minimum; check(current.at("selected") == better);
            if (better) { minimum = loss; selected = current.at("epoch"); }
        }
        check(report.at("selected_epoch") == selected); close(report.at("best").at("validation").at("combined_loss"), minimum);
        const auto chosen = PolicyValueNetwork::load(output / "model.json"), final = PolicyValueNetwork::load(output / "final.json");
        check(chosen.training_steps() == report.at("selected_training_steps") && final.training_steps() == report.at("final_training_steps"));
        const auto heldout = load_dataset(output / "validation.json");
        const auto actual = chosen.evaluate_batch(heldout); close(actual.policy + actual.value, minimum);
        if (selected == 0) check(chosen.parameters() == original.parameters() && chosen.velocity() == original.velocity());
        check(read(output / "report.json") == report);
    });
    suite.test("invalid counts sizes komi labels illegal histories and existing outputs fail before dataset writes", [] {
        const auto output = directory(); auto options = settings();
        rejects([&] { pretrain_expert_games({corpus().front()}, options, std::nullopt, output); });
        auto duplicate = corpus().front(); duplicate.winner = WHITE;
        rejects([&] { pretrain_expert_games({corpus().front(), duplicate}, options, std::nullopt, output); });
        auto wrong = corpus(); wrong.back().initial_state = GameState::new_game(3, 1.5);
        rejects([&] { pretrain_expert_games(wrong, options, std::nullopt, output); });
        wrong = corpus(); wrong.back().initial_state = GameState::new_game(5, .5);
        rejects([&] { pretrain_expert_games(wrong, options, std::nullopt, output); });
        wrong = corpus(); wrong.front().moves[1] = wrong.front().moves[0];
        rejects([&] { pretrain_expert_games(wrong, options, std::nullopt, output); });
        options.validation_fraction = 1; rejects([&] { pretrain_expert_games(corpus(), options, std::nullopt, output); });
        check(std::filesystem::is_empty(output), "Invalid preflight wrote training artifacts");
        const auto source_dir = directory(); PolicyValueNetwork({5, 1, 1}, 0).save(source_dir / "wrong-size.json");
        rejects([&] { pretrain_expert_games(corpus(), settings(), source_dir / "wrong-size.json", output); });
        check(std::filesystem::is_empty(output));
        std::ofstream(output / "model.json") << "preserve";
        rejects([&] { pretrain_expert_games(corpus(), settings(), std::nullopt, output); });
        check(bytes(output / "model.json") == "preserve");
    });
    suite.test("interrupted progress preserves completed epoch report and a loadable selected model", [] {
        const auto output = directory();
        rejects([&] {
            pretrain_expert_games(corpus(), settings(), std::nullopt, output, [](int epoch, const Json&) {
                if (epoch == 1) throw std::runtime_error("Simulated training interruption");
            });
        });
        const auto report = read(output / "report.json");
        check(report.at("status") == "running" && report.at("epochs_completed") == 1 && report.at("history").size() == 1);
        check(report.at("final_model").is_null() && !std::filesystem::exists(output / "final.json"));
        const auto selected = PolicyValueNetwork::load(output / "model.json");
        check(selected.training_steps() == report.at("selected_training_steps"));
        const auto heldout = load_dataset(output / "validation.json");
        const auto actual = selected.evaluate_batch(heldout);
        close(actual.policy + actual.value, report.at("best").at("validation").at("combined_loss"));
    });
    std::cout << "PRETRAINING: " << suite.passed << " passed, " << suite.failed << " failed\n";
    return suite.failed;
}
