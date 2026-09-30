#include "betago/selfplay.hpp"
#include "betago/replay.hpp"
#include "betago/arena.hpp"
#include "betago/dataset.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>

namespace {
using namespace betago;

void check(bool condition, const char* message = "Unexpected result") {
    if (!condition) throw std::runtime_error(message);
}
void close(double actual, double expected, double tolerance = 1e-10) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance)
        throw std::runtime_error("Expected " + std::to_string(expected) + ", got " + std::to_string(actual));
}
template<class Function> void rejects(Function function) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Invalid input was accepted");
}
struct Suite {
    int passed = 0, failed = 0;
    template<class Function> void test(const char* name, Function function) {
        try { function(); ++passed; std::cout << "PASS SELFPLAY " << name << '\n'; }
        catch (const std::exception& error) {
            ++failed; std::cerr << "FAIL SELFPLAY " << name << ": " << error.what() << '\n';
        }
    }
};

std::filesystem::path file(const char* name) {
    return std::filesystem::path("results/selfplay_tests") / name;
}
std::shared_ptr<PolicyValueNetwork> model(int size = 1, bool favor_pass = false) {
    auto result = std::make_shared<PolicyValueNetwork>(NetworkSettings{size, 1, 2}, 17);
    auto parameters = result->parameters(); std::fill(parameters.begin(), parameters.end(), 0.0);
    if (favor_pass) for (const auto& block : result->parameter_blocks())
        if (block.name == "policy_bias") parameters.at(block.offset + size * size) = 8.0;
    result->set_parameters(std::move(parameters));
    result->train(false);
    return result;
}
SelfPlaySettings tiny_settings(double komi = 0.5) {
    SelfPlaySettings settings;
    settings.board_size = 1; settings.komi = komi; settings.max_moves = 2;
    settings.search = {4, 1.5}; settings.temperature = 1;
    settings.temperature_moves = 20; settings.root_uniform_mix = 0;
    return settings;
}
SelfPlayGame one_game(std::int64_t seed = 0, double komi = 0.5) {
    return run_self_play(model(), tiny_settings(komi), seed);
}
Json without_timing(Json value) {
    if (value.is_object()) {
        value.erase("elapsed_seconds");
        for (auto& entry : value.items()) entry.value() = without_timing(std::move(entry.value()));
    } else if (value.is_array()) {
        for (auto& child : value) child = without_timing(std::move(child));
    }
    return value;
}
void same_examples(const std::vector<TrainingExample>& first, const std::vector<TrainingExample>& second) {
    check(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        check(first[i].input.board_size == second[i].input.board_size &&
              first[i].input.features == second[i].input.features &&
              first[i].input.legal_actions == second[i].input.legal_actions &&
              first[i].input.terminal == second[i].input.terminal &&
              first[i].policy == second[i].policy && first[i].value == second[i].value);
    }
}
Json raw_state(const GameState& state) {
    Json previous = state.previous_board() ? Json(*state.previous_board()) : Json(nullptr);
    return {{"board", state.board()}, {"to_play", state.to_play()}, {"komi", state.komi()},
            {"consecutive_passes", state.consecutive_passes()}, {"previous_board", previous}};
}

// A known legal sequence supplies exact history, including a simple-ko capture.
// Search targets below are complete legal visit vectors, independently of any
// particular learned model; validation must replay rather than trust the state.
Json recorded_sequence(int size, double komi, const std::vector<Move>& moves) {
    auto settings = tiny_settings(komi);
    settings.board_size = size; settings.max_moves = static_cast<int>(moves.size());
    settings.temperature = 0;
    const auto final = replay_moves(moves, size, komi);
    GameResult result{final, moves, settings.max_moves, 0};
    auto record = result.to_json();
    record["schema_version"] = 1; record["kind"] = "self_play_game";
    record["feature_schema"] = FEATURE_SCHEMA; record["value_perspective"] = "player_to_move";
    record["seed"] = 0; record["self_play_settings"] = settings.to_json();
    record["targets"] = Json::array();
    auto state = GameState::new_game(size, komi);
    for (const auto& move : moves) {
        auto visits = std::vector<int>(static_cast<std::size_t>(size * size + 1), 0);
        auto policy = std::vector<double>(visits.size(), 0);
        const auto selected = static_cast<std::size_t>(action_index(move, size));
        visits[selected] = settings.search.simulations; policy[selected] = 1;
        const bool terminal_child = state.play(move).is_terminal();
        Json target = {{"state", raw_state(state)}, {"policy", policy},
            {"search", {{"simulations", settings.search.simulations},
                        {"root_visits", settings.search.simulations}, {"action_visits", visits},
                        {"network_evaluations", terminal_child ? 1 : settings.search.simulations + 1},
                        {"terminal_evaluations", terminal_child ? settings.search.simulations : 0}}}};
        if (final.is_terminal()) {
            auto winner = final.winner();
            target["value"] = winner ? (*winner == state.to_play() ? 1.0 : -1.0) : 0.0;
        }
        record["targets"].push_back(std::move(target));
        state = state.play(move);
    }
    return record;
}
} // namespace

int run_selfplay_tests() {
    Suite suite;
    suite.test("visit targets normalize all root children and keep pass and illegal actions separate", [] {
        auto state = GameState::new_game(3, 0.5).play(Point{0, 0});
        SearchStatistics search; search.algorithm = "puct";
        search.simulations = search.root_visits = 5;
        for (const auto& move : state.legal_moves()) {
            int visits = move == Move(Point{0, 1}) ? 2 : (!move ? 3 : 0);
            search.children.push_back({move, visits, 0, 0});
        }
        const auto policy = visit_policy(state, search);
        close(std::accumulate(policy.begin(), policy.end(), 0.0), 1);
        close(policy[0], 0); close(policy[1], 0.4); close(policy.back(), 0.6);
        for (std::size_t i = 2; i + 1 < policy.size(); ++i) close(policy[i], 0);
        TrainingExample{encode_position(state), policy, -1}.validate();
        for (int mutation = 0; mutation < 7; ++mutation) {
            auto bad = search;
            if (mutation == 0) bad.root_visits = bad.simulations = 0;
            if (mutation == 1) bad.root_visits = 4;
            if (mutation == 2) bad.children[0].visits = -1;
            if (mutation == 3) bad.children.push_back(bad.children[0]);
            if (mutation == 4) bad.children.erase(bad.children.begin() + 1);
            if (mutation == 5) bad.children[0].move = Point{0, 0};
            if (mutation == 6) bad.children[0].visits = 1;
            rejects([&] { visit_policy(state, bad); });
        }
        rejects([&] { visit_policy(state.play(PASS).play(PASS), search); });
    });
    suite.test("policy sampling is seeded and rejects invalid distributions", [] {
        Random first(-11), second(-11);
        int seen[3]{};
        const std::vector<double> policy{0.1, 0.2, 0.7};
        for (int i = 0; i < 1000; ++i) {
            auto a = sample_policy(policy, first), b = sample_policy(policy, second);
            check(a == b && a < 3); ++seen[a];
        }
        check(seen[0] > 0 && seen[1] > 0 && seen[2] > seen[1]);
        for (const auto& bad : {std::vector<double>{}, std::vector<double>{0, 0},
                               std::vector<double>{1, -1},
                               std::vector<double>{std::numeric_limits<double>::quiet_NaN()}})
            rejects([&] { sample_policy(bad, first); });
    });
    suite.test("selfplay settings and mismatched models reject before doing work", [] {
        const auto valid = tiny_settings();
        for (int mutation = 0; mutation < 10; ++mutation) {
            auto bad = valid;
            if (mutation == 0) bad.board_size = 0;
            if (mutation == 1) bad.board_size = 20;
            if (mutation == 2) bad.max_moves = 0;
            if (mutation == 3) bad.search.simulations = 0;
            if (mutation == 4) bad.temperature = -1;
            if (mutation == 5) bad.temperature = std::numeric_limits<double>::infinity();
            if (mutation == 6) bad.temperature_moves = -1;
            if (mutation == 7) bad.root_uniform_mix = -0.1;
            if (mutation == 8) bad.root_uniform_mix = 1.1;
            if (mutation == 9) bad.komi = std::numeric_limits<double>::quiet_NaN();
            int decisions = 0;
            rejects([&] { run_self_play(model(), bad, 0,
                [&](int, const GameState&, const SearchStatistics&) { ++decisions; }); });
            check(decisions == 0);
        }
        rejects([&] { run_self_play(nullptr, valid, 0); });
        rejects([&] { run_self_play(model(2), valid, 0); });
    });
    suite.test("final win labels use the saved pre-action player perspective", [] {
        auto game = one_game(-31);
        check(game.record.at("moves") == Json::array({nullptr, nullptr}));
        check(game.record.at("termination_reason") == "two_passes" && game.record.at("winner") == WHITE);
        check(game.examples.size() == 2);
        close(game.examples[0].value, -1); close(game.examples[1].value, 1);
        for (const auto& example : game.examples) {
            check(!example.input.terminal); close(example.policy[0], 0); close(example.policy[1], 1);
            example.validate();
        }
        check(game.record.at("targets")[0].at("state").at("to_play") == BLACK);
        check(game.record.at("targets")[1].at("state").at("to_play") == WHITE);
        same_examples(game.examples, validate_self_play_game(game.record));
    });
    suite.test("draws are zero and truncated games yield no fabricated final targets", [] {
        auto draw = one_game(3, 0);
        check(draw.record.at("winner").is_null() && draw.examples.size() == 2);
        for (const auto& example : draw.examples) close(example.value, 0);
        auto settings = tiny_settings(0); settings.max_moves = 1;
        auto truncated = run_self_play(model(), settings, 2);
        check(truncated.record.at("termination_reason") == "move_limit" && truncated.examples.empty());
        check(truncated.record.at("score").is_null() && truncated.record.at("winner").is_null());
        check(!truncated.record.at("targets")[0].contains("value"));
        check(validate_self_play_game(truncated.record).empty());
        auto bad = truncated.record; bad["targets"][0]["value"] = 0;
        rejects([&] { validate_self_play_game(bad); });
        auto dataset = self_play_dataset(Json::array({draw.record, truncated.record}));
        same_examples(draw.examples, dataset_from_json(dataset));
    });
    suite.test("seeded exploratory games repeat exactly and preserve their frozen source", [] {
        auto network = model(3);
        auto settings = tiny_settings(); settings.board_size = 3; settings.max_moves = 20;
        settings.search.simulations = 8; settings.root_uniform_mix = 0.25;
        const auto parameters = network->parameters();
        auto first = run_self_play(network, settings, 73);
        auto repeated = run_self_play(network, settings, 73);
        auto changed = run_self_play(network, settings, 74);
        check(without_timing(first.record) == without_timing(repeated.record));
        same_examples(first.examples, repeated.examples);
        check(first.record.at("moves") != changed.record.at("moves"), "Exploration ignored game seed");
        check(network->parameters() == parameters && !network->training() && network->training_steps() == 0);
        validate_self_play_game(first.record);
    });
    suite.test("selfplay mixes root priors without changing ordinary neural evaluation", [] {
        auto network = model(2, true);
        auto settings = tiny_settings(); settings.board_size = 2; settings.search.simulations = 1;
        settings.temperature = 0; settings.root_uniform_mix = 0.25;
        int decisions = 0;
        run_self_play(network, settings, 0, [&](int, const GameState& state, const SearchStatistics& search) {
            const auto expected = neural_mcts_detail::normalize_priors(state, network->predict(encode_position(state)));
            const auto legal = state.legal_moves();
            for (const auto& child : search.children)
                close(child.prior, 0.75 * expected.at(action_index(child.move, state.size())) +
                                   0.25 / static_cast<double>(legal.size()));
            ++decisions;
        });
        check(decisions == 2);
        NeuralMctsAgent normal(network, {1, 1.5});
        const auto state = GameState::new_game(2, 0.5); normal.choose_move(state);
        const auto expected = neural_mcts_detail::normalize_priors(state, network->predict(encode_position(state)));
        for (const auto& child : normal.last_search().children)
            close(child.prior, expected.at(action_index(child.move, state.size())));
    });
    suite.test("temperature changes move sampling while targets remain raw search visits", [] {
        auto network = model(3);
        auto hot = tiny_settings(); hot.board_size = 3; hot.max_moves = 1;
        hot.search.simulations = 8; hot.temperature = 1; hot.temperature_moves = 1;
        auto cold = hot; cold.temperature = 0;
        auto no_early_sampling = hot; no_early_sampling.temperature_moves = 0;
        auto greedy = run_self_play(network, cold, 0);
        auto same_greedy = run_self_play(network, no_early_sampling, 73);
        check(greedy.record.at("moves") == same_greedy.record.at("moves"));
        bool found_different_move = false;
        for (int seed = 0; seed < 20; ++seed) {
            auto exploratory = run_self_play(network, hot, seed);
            check(exploratory.record.at("targets")[0].at("policy") == greedy.record.at("targets")[0].at("policy"));
            if (exploratory.record.at("moves") != greedy.record.at("moves")) found_different_move = true;
        }
        check(found_different_move, "Positive temperature never sampled a different visited move");
        const auto& target = greedy.record.at("targets")[0];
        const auto visits = target.at("search").at("action_visits").get<std::vector<int>>();
        const auto policy = target.at("policy").get<std::vector<double>>();
        check(std::count_if(policy.begin(), policy.end(), [](double p) { return p > 0; }) > 1);
        for (std::size_t i = 0; i < visits.size(); ++i) close(policy[i], visits[i] / 8.0);
    });
    suite.test("records replay exact board turn pass history labels and search work", [] {
        const auto valid = one_game().record;
        for (int mutation = 0; mutation < 19; ++mutation) {
            auto bad = valid;
            switch (mutation) {
                case 0: bad["targets"][0]["state"]["to_play"] = WHITE; break;
                case 1: bad["targets"][0]["state"]["previous_board"] = Board{{EMPTY}}; break;
                case 2: bad["targets"][1]["state"]["consecutive_passes"] = 0; break;
                case 3: bad["targets"][0]["policy"] = std::vector<double>{0.5, 0.5}; break;
                case 4: bad["targets"][0]["value"] = 1; break;
                case 5: bad["targets"][0]["search"]["simulations"] = 4.0; break;
                case 6: bad["targets"][0]["search"]["root_visits"] = 3; break;
                case 7: bad["targets"][0]["search"]["action_visits"] = std::vector<int>{1, 3}; break;
                case 8: bad["targets"][0]["search"]["network_evaluations"] = 0; break;
                case 9: bad["winner"] = BLACK; break;
                case 10: bad["score"] = Json::array({10, 0}); break;
                case 11: bad["termination_reason"] = "move_limit"; break;
                case 12: bad["moves"][0] = Json::array({0, 0}); break;
                case 13: bad["targets"].erase(bad["targets"].begin()); break;
                case 14: bad["self_play_settings"]["komi"] = 0; break;
                case 15: bad["seed"] = 0.0; break;
                case 16: bad["size"] = 1.5; break;
                case 17: bad["targets"][0]["state"]["board"][0][0] = 0.0; break;
                case 18: bad["feature_schema"] = "wrong"; break;
            }
            rejects([&] { validate_self_play_game(bad); });
        }
    });
    suite.test("simple ko and its expiry survive raw-state persistence", [] {
        const std::vector<Move> moves{Point{0, 1}, Point{0, 2}, Point{1, 0}, Point{1, 1},
            Point{2, 1}, Point{1, 3}, PASS, Point{2, 2}, Point{1, 2},
            PASS, Point{4, 0}, Point{1, 1}, PASS, PASS};
        const auto record = recorded_sequence(5, 0.5, moves);
        const auto examples = validate_self_play_game(record);
        check(examples.size() == moves.size());
        const auto recapture = static_cast<std::size_t>(action_index(Point{1, 1}, 5));
        check(!examples[9].input.legal_actions[recapture], "Immediate recapture allowed");
        check(examples[11].input.legal_actions[recapture], "Ko failed to expire");
        auto bad = record; bad["targets"][9]["state"]["previous_board"] = nullptr;
        rejects([&] { validate_self_play_game(bad); });
        auto dataset = self_play_dataset(Json::array({record}));
        same_examples(examples, dataset_from_json(dataset));
    });
    suite.test("recent replay keeps complete games in FIFO order and restores all examples", [] {
        ReplayBuffer buffer(2, 1, 0.5);
        auto first = one_game(10), second = one_game(11), third = one_game(12);
        check(buffer.add(first.record) && buffer.add(second.record));
        auto truncated_settings = tiny_settings(); truncated_settings.max_moves = 1;
        const auto truncated = run_self_play(model(), truncated_settings, 13);
        check(!buffer.add(truncated.record) && buffer.game_count() == 2);
        check(buffer.add(third.record) && buffer.game_count() == 2);
        auto data = buffer.to_json();
        check(data.at("games")[0].at("seed") == 11 && data.at("games")[1].at("seed") == 12);
        auto restored = ReplayBuffer::from_json(data);
        check(restored.capacity() == 2 && restored.board_size() == 1 && restored.komi() == 0.5);
        check(restored.to_json() == data);
        same_examples(buffer.examples(), restored.examples());
        buffer.save(file("replay.json"));
        auto loaded = ReplayBuffer::load(file("replay.json"));
        check(loaded.to_json() == data); same_examples(buffer.examples(), loaded.examples());
        check(buffer.examples().size() == 4);
    });
    suite.test("replay rejects rule mismatches malformed integers and corrupted histories", [] {
        ReplayBuffer buffer(2, 1, 0.5); buffer.add(one_game().record);
        const auto valid = buffer.to_json();
        for (int mutation = 0; mutation < 8; ++mutation) {
            auto bad = valid;
            switch (mutation) {
                case 0: bad["schema_version"] = 20; break;
                case 1: bad["capacity"] = 2.0; break;
                case 2: bad["board_size"] = 1.0; break;
                case 3: bad["komi"] = 0; break;
                case 4: bad["games"][0]["targets"][0]["value"] = 1; break;
                case 5: bad["games"] = "wrong"; break;
                case 6: bad["games"].push_back(valid.at("games")[0]); bad["games"].push_back(valid.at("games")[0]); break;
                case 7: bad["capacity"] = 0; break;
            }
            rejects([&] { ReplayBuffer::from_json(bad); });
        }
        rejects([&] { buffer.add(one_game(0, 0).record); });
        auto wrong_size = recorded_sequence(2, 0.5, {PASS, PASS});
        rejects([&] { buffer.add(wrong_size); });
        check(buffer.to_json() == valid, "Rejected game mutated replay");
        rejects([] { ReplayBuffer invalid(0, 1, 0.5); });
        rejects([] { ReplayBuffer invalid(2, 0, 0.5); });
        rejects([] { ReplayBuffer invalid(2, 1, std::numeric_limits<double>::infinity()); });
    });
    suite.test("minibatches sample with replacement reproducibly and may exceed replay size", [] {
        const auto examples = one_game().examples;
        Random first(9), second(9);
        auto a = sample_replay_batch(examples, 17, first);
        auto b = sample_replay_batch(examples, 17, second);
        check(a.size() == 17); same_examples(a, b);
        check(std::any_of(a.begin(), a.end(), [](const TrainingExample& e) { return e.value == -1; }));
        check(std::any_of(a.begin(), a.end(), [](const TrainingExample& e) { return e.value == 1; }));
        rejects([&] { sample_replay_batch({}, 1, first); });
        rejects([&] { sample_replay_batch(examples, 0, first); });
        rejects([&] { sample_replay_batch(examples, -1, first); });
    });
    suite.test("candidate updates reports and minibatch losses reproduce from their seed", [] {
        ReplayBuffer buffer(3, 1, 0.5); buffer.add(one_game().record);
        auto first = *model(), second = *model();
        TrainingSettings settings; settings.updates = 12; settings.batch_size = 3;
        settings.optimizer = {0.03, 0.8, 0.001};
        int updates = 0;
        const auto a = train_candidate(first, buffer, settings, -20, [&](int, const LossMetrics& loss) {
            check(first.training()); check(std::isfinite(loss.total)); ++updates;
        });
        const auto b = train_candidate(second, buffer, settings, -20);
        check(a == b && first.parameters() == second.parameters() && first.velocity() == second.velocity());
        check(updates == settings.updates && first.training_steps() == static_cast<std::uint64_t>(settings.updates));
        check(!first.training() && !second.training());
        auto changed = *model(); train_candidate(changed, buffer, settings, -21);
        check(changed.parameters() != first.parameters(), "Training sampler ignored its seed");
    });
    suite.test("trained checkpoints retain momentum for the next candidate iteration", [] {
        ReplayBuffer buffer(3, 1, 0.5); buffer.add(one_game().record);
        auto first = *model(); TrainingSettings settings;
        settings.updates = 3; settings.batch_size = 3; settings.optimizer = {0.03, 0.8, 0.001};
        train_candidate(first, buffer, settings, 10); first.save(file("candidate.json"));
        auto loaded = PolicyValueNetwork::load(file("candidate.json"));
        check(loaded.parameters() == first.parameters() && loaded.velocity() == first.velocity());
        const auto a = train_candidate(first, buffer, settings, 11);
        const auto b = train_candidate(loaded, buffer, settings, 11);
        check(a == b && loaded.parameters() == first.parameters() && loaded.velocity() == first.velocity());
        check(loaded.training_steps() == 6 && !loaded.training());
        auto reset = loaded; reset.set_parameters(reset.parameters());
        check(reset.training_steps() == 0 && !reset.last_optimizer());
        check(std::all_of(reset.velocity().begin(), reset.velocity().end(), [](double value) { return value == 0; }));
    });
    suite.test("training rejects invalid or empty inputs before mutating a candidate", [] {
        ReplayBuffer buffer(3, 1, 0.5); buffer.add(one_game().record);
        auto network = *model(); const auto parameters = network.parameters();
        const auto velocity = network.velocity();
        for (int mutation = 0; mutation < 5; ++mutation) {
            TrainingSettings bad;
            if (mutation == 0) bad.updates = 0;
            if (mutation == 1) bad.batch_size = 0;
            if (mutation == 2) bad.optimizer.learning_rate = 0;
            if (mutation == 3) bad.optimizer.momentum = 1;
            if (mutation == 4) bad.optimizer.l2 = -1;
            rejects([&] { train_candidate(network, buffer, bad, 0); });
        }
        ReplayBuffer empty(3, 1, 0.5);
        rejects([&] { train_candidate(network, empty, {}, 0); });
        ReplayBuffer mismatched(3, 2, 0.5); mismatched.add(recorded_sequence(2, 0.5, {PASS, PASS}));
        rejects([&] { train_candidate(network, mismatched, {}, 0); });
        check(network.parameters() == parameters && network.velocity() == velocity &&
              network.training_steps() == 0 && !network.training());
    });
    suite.test("one CPU iteration reaches a replay-trained checkpoint and paired arena", [] {
        auto incumbent = model(); incumbent->save(file("incumbent.json"));
        ReplayBuffer replay(2, 1, 0.5);
        check(replay.add(run_self_play(incumbent, tiny_settings(), 100).record));
        replay.save(file("end_to_end_replay.json"));
        auto candidate = PolicyValueNetwork::load(file("incumbent.json"));
        TrainingSettings settings; settings.updates = 4; settings.batch_size = 2;
        const auto report = train_candidate(candidate, ReplayBuffer::load(file("end_to_end_replay.json")), settings, 101);
        check(report.is_object() && candidate.training_steps() == 4);
        candidate.save(file("end_to_end_candidate.json"));
        ArenaSettings arena;
        arena.a = {"neural-mcts", {}, file("end_to_end_candidate.json").string(), {4, 1.5}};
        arena.b = {"neural-mcts", {}, file("incumbent.json").string(), {4, 1.5}};
        arena.pairs = 1; arena.size = 1; arena.komi = 0.5; arena.max_moves = 2; arena.seed = 102;
        const auto evaluated = run_arena(arena);
        check(evaluated.at("summary").at("complete_pairs") == 1);
        check(evaluated.at("games").size() == 2);
        for (const auto& game : evaluated.at("games")) check(game.at("termination_reason") == "two_passes");
        save_records(file("end_to_end_arena.json"), evaluated);
        check(load_records(file("end_to_end_arena.json")) == evaluated);
    });
    std::cout << "SELFPLAY: " << suite.passed << " passed, " << suite.failed << " failed\n";
    return suite.failed;
}
