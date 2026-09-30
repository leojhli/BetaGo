#include "betago/dataset.hpp"
#include "betago/network.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <string>

namespace {
using namespace betago;

void neural_check(bool condition, const std::string& message = "Unexpected result") {
    if (!condition) throw std::runtime_error(message);
}
void neural_close(double actual, double expected, double tolerance = 1e-10,
                  const std::string& message = "Unexpected number") {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance)
        throw std::runtime_error(message + ": actual=" + std::to_string(actual) +
                                 " expected=" + std::to_string(expected));
}
template<class Function> void neural_rejects(Function function) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Invalid input was accepted");
}
struct NeuralSuite {
    int passed = 0, failed = 0;
    template<class Function> void test(const char* name, Function function) {
        try { function(); ++passed; std::cout << "PASS NEURAL " << name << '\n'; }
        catch (const std::exception& error) {
            ++failed; std::cerr << "FAIL NEURAL " << name << ": " << error.what() << '\n';
        }
    }
};
GameState neural_position(std::initializer_list<std::string> rows, int player = BLACK,
                          double komi = 0, int passes = 0) {
    Board board;
    for (const auto& row : rows) {
        std::vector<int> cells;
        for (char cell : row) cells.push_back(cell == 'X' ? BLACK : cell == 'O' ? WHITE : EMPTY);
        board.push_back(std::move(cells));
    }
    return GameState(std::move(board), player, komi, passes);
}
double plane(const EncodedPosition& input, int channel, int row, int column) {
    const auto area = static_cast<std::size_t>(input.board_size * input.board_size);
    return input.features.at(static_cast<std::size_t>(channel) * area +
                             static_cast<std::size_t>(row * input.board_size + column));
}
bool encoded_equal(const EncodedPosition& first, const EncodedPosition& second) {
    return first.board_size == second.board_size && first.features == second.features &&
           first.legal_actions == second.legal_actions && first.terminal == second.terminal;
}
TrainingExample example(const GameState& state, Move action, double value) {
    auto input = encode_position(state);
    std::vector<double> target(input.legal_actions.size(), 0.0);
    target.at(static_cast<std::size_t>(action_index(action, state.size()))) = 1.0;
    TrainingExample result{std::move(input), std::move(target), value};
    result.validate();
    return result;
}
std::vector<TrainingExample> small_examples() {
    auto first = GameState::new_game(3, 0);
    auto second = first.play(Point{0, 0});
    auto third = second.play(Point{2, 2});
    return {example(first, Point{0, 0}, 1), example(second, Point{0, 2}, -1),
            example(third, Point{1, 1}, 1)};
}
const ParameterBlock& block(const PolicyValueNetwork& network, const std::string& name) {
    const auto& blocks = network.parameter_blocks();
    auto found = std::find_if(blocks.begin(), blocks.end(),
                              [&](const ParameterBlock& candidate) { return candidate.name == name; });
    neural_check(found != blocks.end(), "Missing parameter block " + name);
    return *found;
}
void same_prediction(const Prediction& first, const Prediction& second) {
    neural_check(first.logits == second.logits && first.policy == second.policy &&
                 first.value == second.value, "Predictions differ");
}
Json state_json(const GameState& state) {
    return {{"board", state.board()}, {"to_play", state.to_play()}, {"komi", state.komi()},
            {"consecutive_passes", state.consecutive_passes()},
            {"previous_board", state.previous_board() ? Json(*state.previous_board()) : Json(nullptr)}};
}
Json single_dataset(const GameState& state, Move action, double value = 0) {
    Json data = make_demo_dataset();
    auto sample = example(state, action, value);
    data["examples"] = Json::array({{{"state", state_json(state)},
                                     {"policy", sample.policy}, {"value", value}}});
    return data;
}
Json read_json(const std::filesystem::path& path) {
    std::ifstream input(path); neural_check(static_cast<bool>(input), "Cannot read test checkpoint");
    Json data; input >> data; return data;
}
void write_json(const std::filesystem::path& path, const Json& data) {
    std::ofstream output(path); neural_check(static_cast<bool>(output), "Cannot write test checkpoint");
    output << data.dump(2); neural_check(static_cast<bool>(output), "Test checkpoint write failed");
}
} // namespace

int run_neural_tests() {
    NeuralSuite suite;
    suite.test("seven CHW planes use the current player's stone perspective", [] {
        auto black = neural_position({"X.O", ".OX", "..."}, BLACK, 2.5);
        auto white = GameState(black.board(), WHITE, 2.5);
        auto a = encode_position(black), b = encode_position(white);
        neural_check(FEATURE_CHANNELS == 7 && a.features.size() == 63 && a.legal_actions.size() == 10);
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) {
            neural_close(plane(a, 0, r, c), black.at({r, c}) == BLACK ? 1 : 0);
            neural_close(plane(a, 1, r, c), black.at({r, c}) == WHITE ? 1 : 0);
            neural_close(plane(b, 0, r, c), plane(a, 1, r, c));
            neural_close(plane(b, 1, r, c), plane(a, 0, r, c));
            neural_close(plane(a, 2, r, c), 0);
            neural_close(plane(a, 3, r, c), 0);
            neural_close(plane(a, 4, r, c), 0);
            neural_close(plane(a, 5, r, c), -std::tanh(2.5 / 9));
            neural_close(plane(b, 5, r, c), std::tanh(2.5 / 9));
        }
    });
    suite.test("legal masks include pass and match captures suicide and ko rules", [] {
        auto ko = neural_position({".XO..", "XO.O.", ".XO..", ".....", "....."})
                      .play(Point{1, 2});
        std::vector<GameState> states = {
            GameState::new_game(9), neural_position({".O.", "O.O", ".O."}),
            neural_position({"XOX", "O.O", "XOX"}), ko,
            ko.play(Point{4, 4}).play(Point{4, 0}),
            ko.play(PASS).play(Point{4, 0}), GameState::new_game(1)};
        for (const auto& state : states) {
            auto encoded = encode_position(state); auto legal = state.legal_moves();
            for (int index = 0; index <= state.size() * state.size(); ++index) {
                bool expected = std::find(legal.begin(), legal.end(), action_from_index(index, state.size())) != legal.end();
                neural_check(encoded.legal_actions.at(static_cast<std::size_t>(index)) == expected);
                if (index < state.size() * state.size())
                    neural_close(plane(encoded, 6, index / state.size(), index % state.size()), expected ? 1 : 0);
            }
            neural_check(encoded.legal_actions.back(), "Nonterminal pass must be legal");
        }
        neural_check(!encode_position(states[1]).legal_actions.at(4), "Suicide must be masked");
        neural_check(encode_position(states[2]).legal_actions.at(4), "Capture before suicide must stay legal");
        neural_close(plane(encode_position(ko), 2, 1, 1), 1);
        neural_close(plane(encode_position(states[4]), 2, 1, 1), 0);
        neural_close(plane(encode_position(states[5]), 2, 1, 1), 0);
        neural_check(encode_position(states[4]).legal_actions.at(6) &&
                     encode_position(states[5]).legal_actions.at(6), "Delayed recapture must become legal");
    });
    suite.test("one pass and terminal flags distinguish identical board positions", [] {
        auto start = GameState::new_game(3, 7.5);
        auto once = encode_position(start.play(PASS));
        auto twice = encode_position(start.play(PASS).play(PASS));
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) {
            neural_close(plane(once, 3, r, c), 1); neural_close(plane(once, 4, r, c), 0);
            neural_close(plane(twice, 3, r, c), 0); neural_close(plane(twice, 4, r, c), 1);
            neural_close(plane(twice, 6, r, c), 0);
        }
        neural_check(!once.terminal && twice.terminal);
        neural_check(std::none_of(twice.legal_actions.begin(), twice.legal_actions.end(), [](bool legal) { return legal; }));
    });
    suite.test("signed komi normalization remains bounded even for extreme finite komi", [] {
        for (double komi : {0.0, -7.5, 7.5, -1e300, 1e300}) {
            auto state = GameState::new_game(3, komi);
            auto black = encode_position(state), white = encode_position(GameState(state.board(), WHITE, komi));
            for (int index = 0; index < 9; ++index) {
                double actual = plane(black, 5, index / 3, index % 3);
                neural_check(std::isfinite(actual) && actual >= -1 && actual <= 1);
                neural_close(actual, -std::tanh(komi / 9));
                neural_close(actual, -plane(white, 5, index / 3, index % 3));
            }
        }
    });
    suite.test("action indexing covers every row-major point and final pass", [] {
        for (int size : {1, 2, 3, 7, 9, 19}) {
            for (int r = 0; r < size; ++r) for (int c = 0; c < size; ++c) {
                int index = action_index(Point{r, c}, size);
                neural_check(index == r * size + c && action_from_index(index, size) == Move(Point{r, c}));
            }
            neural_check(action_index(PASS, size) == size * size && action_from_index(size * size, size) == PASS);
            neural_rejects([&] { action_from_index(-1, size); });
            neural_rejects([&] { action_from_index(size * size + 1, size); });
            neural_rejects([&] { action_index(Point{size, 0}, size); });
            neural_rejects([&] { action_index(Point{0, -1}, size); });
        }
        neural_check(encode_position(GameState::new_game()).legal_actions.size() == 82);
        neural_rejects([] { action_index(PASS, 0); });
        neural_rejects([] { action_from_index(0, -1); });
    });
    suite.test("batch packing is NCHW and preserves every input including ko history", [] {
        auto state = neural_position({".XO..", "XO.O.", ".XO..", ".....", "....."}).play(Point{1, 2});
        const auto original_state = state;
        std::vector<EncodedPosition> inputs{encode_position(state), encode_position(state.play(PASS))};
        const auto original_inputs = inputs;
        auto packed = pack_features(inputs);
        neural_check(packed.samples == 2 && packed.channels == 7 && packed.height == 5 && packed.width == 5);
        neural_check(packed.values.size() == 350);
        neural_check(std::equal(inputs[0].features.begin(), inputs[0].features.end(), packed.values.begin()));
        neural_check(std::equal(inputs[1].features.begin(), inputs[1].features.end(), packed.values.begin() + 175));
        neural_check(state == original_state && encoded_equal(inputs[0], original_inputs[0]) &&
                     encoded_equal(inputs[1], original_inputs[1]));
        neural_rejects([] { pack_features({}); });
        neural_rejects([] { pack_features({encode_position(GameState::new_game(3)), encode_position(GameState::new_game(9))}); });
    });
    suite.test("encoded shapes and nonfinite features are rejected", [] {
        auto valid = encode_position(GameState::new_game(3));
        auto invalid = valid; invalid.features.pop_back(); neural_rejects([&] { invalid.validate(); });
        invalid = valid; invalid.legal_actions.pop_back(); neural_rejects([&] { invalid.validate(); });
        invalid = valid; invalid.board_size = 0; neural_rejects([&] { invalid.validate(); });
        invalid = valid; invalid.features[0] = std::numeric_limits<double>::quiet_NaN();
        neural_rejects([&] { invalid.validate(); });
        invalid = valid; invalid.features[0] = std::numeric_limits<double>::infinity();
        neural_rejects([&] { pack_features({invalid}); });
    });
    suite.test("encoded binary planes constant flags stone occupancy and action masks stay consistent", [] {
        const auto valid = encode_position(GameState::new_game(3));
        for (int mutation = 0; mutation < 8; ++mutation) {
            auto bad = valid;
            switch (mutation) {
                case 0: bad.features[0] = 0.5; break;
                case 1: bad.features[0] = 1; bad.features[9] = 1; break;
                case 2: bad.features[27] = 1; break;
                case 3: bad.features[45] = 0.1; break;
                case 4: bad.features[54] = 0; break;
                case 5: bad.legal_actions.back() = false; break;
                case 6: bad.terminal = true; break;
                case 7: bad.features[18] = 1; break;
            }
            neural_rejects([&] { bad.validate(); });
        }
        TrainingExample terminal{encode_position(GameState::new_game(3).play(PASS).play(PASS)),
                                 std::vector<double>(10, 0), 0};
        neural_rejects([&] { terminal.validate(); });
    });
    suite.test("scripted 9 by 9 dataset labels the legal action before it is played", [] {
        auto data = make_demo_dataset(); auto samples = dataset_from_json(data);
        const std::vector<Move> moves = {Point{0, 1}, Point{1, 1}, Point{1, 0}, Point{8, 8},
                                       Point{2, 1}, Point{8, 7}, Point{1, 2}, PASS, PASS};
        const double komi = data.at("examples").at(0).at("state").at("komi").get<double>();
        auto final = GameState::new_game(9, komi); for (auto move : moves) final = final.play(move);
        neural_check(final.is_terminal() && final.winner().has_value() && samples.size() == moves.size());
        auto state = GameState::new_game(9, komi);
        for (std::size_t i = 0; i < moves.size(); ++i) {
            auto expected = encode_position(state); const auto& sample = samples[i];
            neural_check(encoded_equal(sample.input, expected), "Target was encoded after its action");
            int target = action_index(moves[i], 9);
            neural_check(sample.input.legal_actions[static_cast<std::size_t>(target)]);
            for (std::size_t action = 0; action < sample.policy.size(); ++action)
                neural_close(sample.policy[action], action == static_cast<std::size_t>(target) ? 1 : 0);
            neural_close(sample.value, *final.winner() == state.to_play() ? 1 : -1);
            state = state.play(moves[i]);
        }
        neural_check(state == final);
    });
    suite.test("dataset rejects malformed schema perspective shape and empty or mixed samples", [] {
        const auto valid = make_demo_dataset();
        for (auto mutation : {0, 1, 2, 3, 4, 5, 6, 7}) {
            auto bad = valid;
            switch (mutation) {
                case 0: bad["schema_version"] = 99; break;
                case 1: bad["feature_schema"] = "obsolete"; break;
                case 2: bad["value_perspective"] = "black"; break;
                case 3: bad["examples"] = Json::array(); break;
                case 4: bad["examples"][0]["state"]["board"][0] = Json::array({0, 0}); break;
                case 5: bad["examples"][0]["policy"] = Json::array({1}); break;
                case 6: bad["examples"][0].erase("value"); break;
                case 7: bad["examples"].push_back(single_dataset(GameState::new_game(3), PASS)["examples"][0]); break;
            }
            neural_rejects([&] { dataset_from_json(bad); });
        }
    });
    suite.test("dataset rejects targets on occupied suicide and immediate ko actions", [] {
        std::vector<std::pair<GameState, Move>> illegal = {
            {GameState::new_game(3).play(Point{0, 0}), Point{0, 0}},
            {neural_position({".O.", "O.O", ".O."}), Point{1, 1}},
            {neural_position({".XO..", "XO.O.", ".XO..", ".....", "....."}).play(Point{1, 2}), Point{1, 1}}
        };
        for (const auto& [state, action] : illegal) {
            auto data = single_dataset(state, PASS);
            std::vector<double> target(static_cast<std::size_t>(state.size() * state.size() + 1), 0);
            target[static_cast<std::size_t>(action_index(action, state.size()))] = 1;
            data["examples"][0]["policy"] = target;
            neural_rejects([&] { dataset_from_json(data); });
        }
    });
    suite.test("training targets require normalized legal finite policies and bounded values", [] {
        const auto valid = example(GameState::new_game(3), PASS, 0);
        for (int mutation = 0; mutation < 7; ++mutation) {
            auto bad = valid;
            switch (mutation) {
                case 0: bad.policy[0] = -0.1; bad.policy.back() = 1.1; break;
                case 1: bad.policy.back() = 0; break;
                case 2: bad.policy[0] = 1; break;
                case 3: bad.policy[0] = std::numeric_limits<double>::quiet_NaN(); break;
                case 4: bad.value = 1.01; break;
                case 5: bad.value = -1.01; break;
                case 6: bad.value = std::numeric_limits<double>::infinity(); break;
            }
            neural_rejects([&] { bad.validate(); });
        }
        auto data = single_dataset(GameState::new_game(3), PASS); data["examples"][0]["value"] = 2;
        neural_rejects([&] { dataset_from_json(data); });
    });

    suite.test("CNN produces finite 82-action logits legal probabilities and bounded value", [] {
        PolicyValueNetwork network({}, 11);
        auto input = encode_position(GameState::new_game()); auto prediction = network.predict(input);
        neural_check(prediction.logits.size() == 82 && prediction.policy.size() == 82);
        double sum = 0;
        for (std::size_t i = 0; i < prediction.policy.size(); ++i) {
            neural_check(std::isfinite(prediction.logits[i]) && std::isfinite(prediction.policy[i]) && prediction.policy[i] >= 0);
            if (!input.legal_actions[i]) neural_close(prediction.policy[i], 0);
            sum += prediction.policy[i];
        }
        neural_close(sum, 1); neural_check(std::isfinite(prediction.value) && std::abs(prediction.value) <= 1);
    });
    suite.test("CNN masks suicide occupied and ko moves and terminal policy has no actions", [] {
        for (const auto& state : {neural_position({".O.", "O.O", ".O."}),
                                  GameState::new_game(3).play(Point{0, 0}),
                                  GameState::new_game(3).play(PASS).play(PASS)}) {
            PolicyValueNetwork network({3, 2, 3}, 2); auto input = encode_position(state);
            auto prediction = network.predict(input); double sum = 0;
            for (std::size_t i = 0; i < input.legal_actions.size(); ++i) {
                if (!input.legal_actions[i]) neural_close(prediction.policy[i], 0);
                sum += prediction.policy[i];
            }
            neural_close(sum, input.terminal ? 0 : 1);
            neural_check(std::isfinite(prediction.value) && std::abs(prediction.value) <= 1);
        }
        auto ko = neural_position({".XO..", "XO.O.", ".XO..", ".....", "....."}).play(Point{1, 2});
        PolicyValueNetwork network({5, 2, 3}, 2);
        neural_close(network.predict(encode_position(ko)).policy[6], 0);
    });
    suite.test("network seeds reproduce initialization and batch predictions equal single predictions", [] {
        PolicyValueNetwork first({3, 2, 3}, 40), second({3, 2, 3}, 40), other({3, 2, 3}, 41);
        neural_check(first.parameters() == second.parameters() && first.parameters() != other.parameters());
        std::vector<EncodedPosition> inputs; for (const auto& sample : small_examples()) inputs.push_back(sample.input);
        auto predictions = first.predict_batch(inputs); neural_check(predictions.size() == inputs.size());
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            same_prediction(predictions[i], first.predict(inputs[i])); same_prediction(predictions[i], second.predict(inputs[i]));
        }
    });
    suite.test("both heads and every shared convolution parameter pass central finite differences", [] {
        PolicyValueNetwork network({3, 2, 3}, 4); const auto batch = small_examples();
        auto parameters = network.parameters();
        // Positive shared activations keep +/- epsilon on the same ReLU branch.
        for (std::size_t i = 0; i < parameters.size(); ++i)
            parameters[i] = 0.025 * (2 + std::sin(static_cast<double>(i) * 0.53));
        network.set_parameters(parameters);
        constexpr double l2 = 0.07, epsilon = 1e-5;
        auto analytical = network.loss_and_gradient(batch, l2);
        neural_check(analytical.gradient.size() == parameters.size());
        for (const auto& section : network.parameter_blocks()) {
            double magnitude = 0;
            for (std::size_t local = 0; local < section.count; ++local) {
                const auto index = section.offset + local;
                auto perturbed = parameters; perturbed[index] += epsilon; network.set_parameters(perturbed);
                const double plus = network.evaluate_batch(batch, l2).total;
                perturbed[index] -= 2 * epsilon; network.set_parameters(perturbed);
                const double minus = network.evaluate_batch(batch, l2).total;
                const double numerical = (plus - minus) / (2 * epsilon);
                const double expected = analytical.gradient[index];
                const double tolerance = 2e-6 + 2e-4 * std::max(std::abs(expected), std::abs(numerical));
                neural_close(numerical, expected, tolerance, "Gradient " + section.name + "[" + std::to_string(local) + "]");
                magnitude += std::abs(expected - l2 * parameters[index]);
            }
            neural_check(magnitude > 1e-7, "Unexercised gradient block " + section.name);
        }
        network.set_parameters(parameters);
    });
    suite.test("policy and value gradients both reach the shared convolution and add correctly", [] {
        PolicyValueNetwork network({3, 2, 3}, 4); auto combined = small_examples();
        auto parameters = network.parameters();
        for (std::size_t i = 0; i < parameters.size(); ++i)
            parameters[i] = 0.025 * (2 + std::sin(static_cast<double>(i) * 0.53));
        network.set_parameters(parameters);
        auto policy_only = combined, value_only = combined;
        for (std::size_t i = 0; i < combined.size(); ++i) {
            auto prediction = network.predict(combined[i].input);
            policy_only[i].value = prediction.value;
            value_only[i].policy = prediction.policy;
        }
        auto both = network.loss_and_gradient(combined), policy = network.loss_and_gradient(policy_only),
             value = network.loss_and_gradient(value_only);
        for (std::size_t i = 0; i < both.gradient.size(); ++i)
            neural_close(both.gradient[i], policy.gradient[i] + value.gradient[i]);
        const auto& convolution = block(network, "conv_weight");
        double policy_magnitude = 0, value_magnitude = 0;
        for (std::size_t i = convolution.offset; i < convolution.offset + convolution.count; ++i) {
            policy_magnitude += std::abs(policy.gradient[i]); value_magnitude += std::abs(value.gradient[i]);
        }
        neural_check(policy_magnitude > 1e-7 && value_magnitude > 1e-7,
                     "Both heads must train shared convolution weights");
    });
    suite.test("losses and data gradients average over a batch while L2 is counted once", [] {
        PolicyValueNetwork network({3, 2, 3}, 12); auto batch = small_examples();
        auto first = network.loss_and_gradient(batch, 0);
        auto doubled = batch; doubled.insert(doubled.end(), batch.begin(), batch.end());
        auto repeated = network.loss_and_gradient(doubled, 0);
        neural_close(first.loss.total, repeated.loss.total);
        for (std::size_t i = 0; i < first.gradient.size(); ++i) neural_close(first.gradient[i], repeated.gradient[i]);
        const double l2 = 0.13; auto regularized = network.loss_and_gradient(batch, l2);
        double norm = 0;
        for (std::size_t i = 0; i < first.gradient.size(); ++i) {
            norm += network.parameters()[i] * network.parameters()[i];
            neural_close(regularized.gradient[i] - first.gradient[i], l2 * network.parameters()[i]);
        }
        neural_close(regularized.loss.regularization, 0.5 * l2 * norm);
        neural_close(regularized.loss.total, first.loss.total + regularized.loss.regularization);
        auto twice = network.loss_and_gradient(doubled, l2);
        neural_close(twice.loss.regularization, regularized.loss.regularization);
    });
    suite.test("stable log-sum-exp gives finite cross entropy and gradient with extreme logits", [] {
        PolicyValueNetwork network({3, 2, 3}, 0); auto parameters = network.parameters();
        std::fill(parameters.begin(), parameters.end(), 0);
        const auto offset = block(network, "policy_bias").offset;
        parameters[offset] = 1000; parameters[offset + 1] = -1000;
        network.set_parameters(parameters);
        std::vector<TrainingExample> batch{example(GameState::new_game(3, 0), Point{0, 1}, 0)};
        auto result = network.loss_and_gradient(batch);
        neural_close(result.loss.policy, 2000, 1e-9);
        neural_check(std::isfinite(result.loss.total));
        for (double gradient : result.gradient) neural_check(std::isfinite(gradient));
        neural_close(result.gradient[offset], 1); neural_close(result.gradient[offset + 1], -1);
        auto prediction = network.predict(batch[0].input);
        neural_close(std::accumulate(prediction.policy.begin(), prediction.policy.end(), 0.0), 1);
    });
    suite.test("accepted near-unit target mass retains exact cross entropy gradient and shift invariance", [] {
        PolicyValueNetwork network({3, 2, 3}, 5);
        auto sample = example(GameState::new_game(3, 0), PASS, 0);
        sample.policy.back() = 1 + 5e-9; sample.validate();
        const auto prediction = network.predict(sample.input);
        const auto gradients = network.loss_and_gradient({sample});
        const auto& bias = block(network, "policy_bias");
        const double mass = std::accumulate(sample.policy.begin(), sample.policy.end(), 0.0);
        double sum = 0;
        for (std::size_t action = 0; action < sample.policy.size(); ++action) {
            const double gradient = gradients.gradient[bias.offset + action];
            neural_close(gradient, prediction.policy[action] * mass - sample.policy[action], 1e-12);
            sum += gradient;
        }
        neural_close(sum, 0, 1e-12, "Adding the same bias to every legal logit cannot change cross entropy");
    });
    suite.test("evaluation and gradients preserve state and training requires explicit train mode", [] {
        PolicyValueNetwork network({3, 2, 3}, 9); auto batch = small_examples();
        auto parameters = network.parameters(), velocity = network.velocity(); auto original = batch;
        neural_check(!network.training()); auto before = network.predict(batch[0].input);
        network.evaluate_batch(batch); network.loss_and_gradient(batch);
        neural_rejects([&] { network.train_batch(batch); });
        neural_check(network.parameters() == parameters && network.velocity() == velocity && network.training_steps() == 0);
        network.train(); same_prediction(before, network.predict(batch[0].input));
        network.train(false); same_prediction(before, network.predict(batch[0].input));
        for (std::size_t i = 0; i < batch.size(); ++i)
            neural_check(encoded_equal(batch[i].input, original[i].input) && batch[i].policy == original[i].policy && batch[i].value == original[i].value);
    });
    suite.test("SGD momentum performs the hand-computed updates and retains gradient history", [] {
        PolicyValueNetwork network({3, 2, 3}, 6); auto batch = small_examples();
        const OptimizerSettings optimizer{0.03, 0.7, 0.04}; network.train();
        auto initial = network.parameters(); auto gradient = network.loss_and_gradient(batch, optimizer.l2);
        auto reported = network.train_batch(batch, optimizer);
        neural_close(reported.total, gradient.loss.total);
        for (std::size_t i = 0; i < initial.size(); ++i) {
            neural_close(network.velocity()[i], gradient.gradient[i]);
            neural_close(network.parameters()[i], initial[i] - optimizer.learning_rate * gradient.gradient[i]);
        }
        auto parameters = network.parameters(), velocity = network.velocity();
        gradient = network.loss_and_gradient(batch, optimizer.l2); network.train_batch(batch, optimizer);
        for (std::size_t i = 0; i < parameters.size(); ++i) {
            double expected = optimizer.momentum * velocity[i] + gradient.gradient[i];
            neural_close(network.velocity()[i], expected);
            neural_close(network.parameters()[i], parameters[i] - optimizer.learning_rate * expected);
        }
        neural_check(network.training_steps() == 2 && network.last_optimizer() == optimizer);
    });
    suite.test("invalid settings parameters batches and optimizer leave a trained model unchanged", [] {
        const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
        for (auto settings : {NetworkSettings{0, 2, 3}, NetworkSettings{3, 0, 3}, NetworkSettings{3, 2, 0}})
            neural_rejects([&] { PolicyValueNetwork network(settings); });
        PolicyValueNetwork network({3, 2, 3}, 7); auto batch = small_examples(); network.train(); network.train_batch(batch);
        auto parameters = network.parameters(), velocity = network.velocity(); const auto steps = network.training_steps();
        const auto last = network.last_optimizer();
        for (auto optimizer : {OptimizerSettings{0, 0, 0}, OptimizerSettings{-1, 0, 0},
                               OptimizerSettings{nan, 0, 0}, OptimizerSettings{inf, 0, 0},
                               OptimizerSettings{0.1, -0.1, 0}, OptimizerSettings{0.1, 1, 0},
                               OptimizerSettings{0.1, inf, 0}, OptimizerSettings{0.1, 0, -1}})
            neural_rejects([&] { network.train_batch(batch, optimizer); });
        neural_rejects([&] { network.train_batch({}); });
        auto invalid_batch = batch; invalid_batch[1].value = nan;
        neural_rejects([&] { network.train_batch(invalid_batch); });
        neural_rejects([&] { network.predict(encode_position(GameState::new_game(9))); });
        neural_rejects([&] { network.predict_batch({}); });
        neural_rejects([&] { network.evaluate_batch(batch, -0.1); });
        auto invalid_parameters = parameters; invalid_parameters.pop_back();
        neural_rejects([&] { network.set_parameters(invalid_parameters); });
        invalid_parameters = parameters; invalid_parameters[0] = inf;
        neural_rejects([&] { network.set_parameters(invalid_parameters); });
        neural_check(network.parameters() == parameters && network.velocity() == velocity &&
                     network.training_steps() == steps && network.last_optimizer() == last);
    });
    suite.test("fixed tiny positions can be memorized by both policy and value heads", [] {
        PolicyValueNetwork network({3, 4, 8}, 3); const auto batch = small_examples();
        const double initial = network.evaluate_batch(batch).total; network.train();
        for (int step = 0; step < 800; ++step) network.train_batch(batch, {0.05, 0.8, 0});
        network.train(false); auto final = network.evaluate_batch(batch);
        neural_check(final.total < initial * 0.1 && final.policy < 0.05 && final.value < 0.02,
                     "Tiny fixed-data overfit failed: loss=" + std::to_string(final.total));
        for (const auto& sample : batch) {
            auto prediction = network.predict(sample.input);
            auto target = static_cast<std::size_t>(std::max_element(sample.policy.begin(), sample.policy.end()) - sample.policy.begin());
            auto chosen = static_cast<std::size_t>(std::max_element(prediction.policy.begin(), prediction.policy.end()) - prediction.policy.begin());
            neural_check(chosen == target && prediction.policy[target] > 0.9);
            neural_check(prediction.value * sample.value > 0 && std::abs(prediction.value - sample.value) < 0.15);
        }
    });
    suite.test("a nonfinite proposed optimizer update is rejected without partial mutation", [] {
        PolicyValueNetwork network({3, 2, 3}, 1); auto parameters = network.parameters();
        std::fill(parameters.begin(), parameters.end(), 100); network.set_parameters(parameters); network.train();
        auto velocity = network.velocity(); auto batch = small_examples();
        neural_rejects([&] { network.train_batch(batch, {1e308, 0.9, 0}); });
        neural_check(network.parameters() == parameters && network.velocity() == velocity &&
                     network.training_steps() == 0 && !network.last_optimizer().has_value());
    });
    suite.test("checkpoint preserves exact predictions parameters optimizer state and next update", [] {
        const auto directory = std::filesystem::path("results") / "neural_tests";
        std::filesystem::create_directories(directory); const auto path = directory / "roundtrip.json";
        PolicyValueNetwork network({3, 2, 3}, -13); const auto batch = small_examples();
        const OptimizerSettings optimizer{0.03, 0.8, 0.001}; network.train();
        network.train_batch(batch, optimizer); network.train_batch(batch, optimizer); network.save(path);
        auto loaded = PolicyValueNetwork::load(path);
        neural_check(loaded.settings() == network.settings() && loaded.initialization_seed() == -13 &&
                     loaded.training() == network.training() && loaded.training_steps() == 2 &&
                     loaded.parameters() == network.parameters() && loaded.velocity() == network.velocity() &&
                     loaded.last_optimizer() == network.last_optimizer());
        for (const auto& sample : batch) same_prediction(network.predict(sample.input), loaded.predict(sample.input));
        auto first = network.train_batch(batch, optimizer), second = loaded.train_batch(batch, optimizer);
        neural_check(first.total == second.total && network.parameters() == loaded.parameters() &&
                     network.velocity() == loaded.velocity() && network.training_steps() == loaded.training_steps());
        loaded.train(false); loaded.save(path); auto evaluated = PolicyValueNetwork::load(path);
        neural_check(!evaluated.training()); same_prediction(loaded.predict(batch[0].input), evaluated.predict(batch[0].input));
    });
    suite.test("checkpoint rejects schema feature shape nonfinite and inconsistent optimizer data", [] {
        const auto directory = std::filesystem::path("results") / "neural_tests";
        std::filesystem::create_directories(directory); const auto good_path = directory / "valid.json";
        const auto bad_path = directory / "invalid.json"; PolicyValueNetwork network({3, 2, 3}, 2);
        network.train(); network.train_batch(small_examples()); network.save(good_path); const auto valid = read_json(good_path);
        for (int mutation = 0; mutation < 9; ++mutation) {
            auto bad = valid;
            switch (mutation) {
                case 0: bad["schema_version"] = 20; break;
                case 1: bad["architecture"] = "wrong"; break;
                case 2: bad["feature_schema"] = "wrong"; break;
                case 3: bad["parameter_blocks"][0]["shape"][0] = 100; break;
                case 4: bad["parameters"].erase(bad["parameters"].begin()); break;
                case 5: bad["parameters"][0] = nullptr; break; // Nonfinite JSON doubles serialize as null.
                case 6: bad["velocity"][0] = "nan"; break;
                case 7: bad["mode"] = "sometimes"; break;
                case 8: bad["last_optimizer"]["momentum"] = 1.5; break;
            }
            write_json(bad_path, bad); neural_rejects([&] { PolicyValueNetwork::load(bad_path); });
        }
        neural_rejects([&] { PolicyValueNetwork::load(directory / "missing.json"); });
    });
    std::cout << "NEURAL: " << suite.passed << " passed, " << suite.failed << " failed\n";
    return suite.failed;
}
