#include "betago/dataset.hpp"
#include "betago/network.hpp"
#include "betago/options.hpp"
#include "betago/random.hpp"
#include "build_info.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>

namespace {
using namespace betago;

Json loss_json(const LossMetrics& loss) {
    return {{"policy_cross_entropy", loss.policy}, {"value_mse", loss.value},
            {"regularization", loss.regularization}, {"total", loss.total}};
}

Json fit_report(const PolicyValueNetwork& network, const std::vector<TrainingExample>& examples, double l2) {
    auto loss = network.evaluate_batch(examples, l2);
    double entropy = 0, max_kl = 0, absolute_error = 0, max_error = 0;
    std::size_t correct = 0;
    Json predictions = Json::array();
    for (const auto& example : examples) {
        auto prediction = network.predict(example.input);
        auto choice = std::max_element(prediction.policy.begin(), prediction.policy.end()) - prediction.policy.begin();
        auto target = std::max_element(example.policy.begin(), example.policy.end()) - example.policy.begin();
        correct += choice == target;
        double target_entropy = 0, cross_entropy = 0;
        double largest = -std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < prediction.logits.size(); ++i)
            if (example.input.legal_actions[i]) largest = std::max(largest, prediction.logits[i]);
        double denominator = 0;
        for (std::size_t i = 0; i < prediction.logits.size(); ++i)
            if (example.input.legal_actions[i]) denominator += std::exp(prediction.logits[i] - largest);
        double log_denominator = std::log(denominator);
        for (std::size_t i = 0; i < example.policy.size(); ++i) if (example.policy[i] > 0) {
            target_entropy -= example.policy[i] * std::log(example.policy[i]);
            cross_entropy += example.policy[i] * ((largest - prediction.logits[i]) + log_denominator);
        }
        entropy += target_entropy;
        max_kl = std::max(max_kl, cross_entropy - target_entropy);
        double error = std::abs(prediction.value - example.value);
        absolute_error += error; max_error = std::max(max_error, error);
        predictions.push_back({{"target_action", target}, {"predicted_action", choice},
            {"target_action_probability", prediction.policy[static_cast<std::size_t>(target)]},
            {"target_value", example.value}, {"predicted_value", prediction.value}});
    }
    double count = static_cast<double>(examples.size());
    Json report = {{"loss", loss_json(loss)}, {"policy_kl", std::max(0.0, loss.policy - entropy / count)},
        {"max_policy_kl", std::max(0.0, max_kl)}, {"policy_top1_accuracy", correct / count},
        {"value_mean_absolute_error", absolute_error / count}, {"max_value_error", max_error},
        {"predictions", predictions}};
    // KL measures matching the target distribution, including soft targets.
    // These thresholds prove memorization of this dataset, not playing strength.
    report["fits_training_set"] = report.at("policy_kl").get<double>() <= 0.05 && max_kl <= 0.15 &&
        loss.value <= 0.01 && max_error <= 0.2;
    return report;
}

bool same_path(const std::string& a, const std::string& b) {
    auto first = std::filesystem::weakly_canonical(std::filesystem::absolute(a));
    auto second = std::filesystem::weakly_canonical(std::filesystem::absolute(b));
    std::error_code error;
    if (std::filesystem::equivalent(first, second, error)) return true;
#ifdef _WIN32
    // Windows paths ignore case. Canonical parents also resolve directory
    // links before comparing output paths that have not been created yet.
    auto first_text = first.native(), second_text = second.native();
    auto lower = [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); };
    std::transform(first_text.begin(), first_text.end(), first_text.begin(), lower);
    std::transform(second_text.begin(), second_text.end(), second_text.begin(), lower);
    return first_text == second_text;
#else
    return first == second;
#endif
}

void show_prediction(const PolicyValueNetwork& network, const EncodedPosition& position) {
    auto prediction = network.predict(position);
    std::cout << "Value for the player to move: " << prediction.value << '\n';
    std::vector<int> actions;
    for (std::size_t i = 0; i < position.legal_actions.size(); ++i)
        if (position.legal_actions[i]) actions.push_back(static_cast<int>(i));
    std::stable_sort(actions.begin(), actions.end(), [&](int a, int b) { return prediction.policy[a] > prediction.policy[b]; });
    std::cout << "Most likely legal actions (zero-based row, column):\n";
    for (std::size_t i = 0; i < std::min<std::size_t>(5, actions.size()); ++i) {
        auto move = action_from_index(actions[i], position.board_size);
        std::cout << "  ";
        if (move) std::cout << '(' << move->row << ", " << move->column << ')';
        else std::cout << "pass";
        std::cout << ": " << prediction.policy[actions[i]] << '\n';
    }
}
}

int main(int argc, char** argv) {
    using namespace betago;
    try {
        Options args(argc, argv, {"--make-demo", "--train", "--inspect", "--resume", "--output", "--report",
            "--epochs", "--batch-size", "--learning-rate", "--momentum", "--l2", "--seed", "--channels",
            "--value-hidden", "--log-every", "--position", "--example", "--komi"}, {"--help", "--verify-fit"});
        if (args.has("--help")) {
            std::cout << "BetaGo CPU policy/value network\n"
                      << "network.exe --make-demo results/neural_demo.json\n"
                      << "network.exe --train results/neural_demo.json [--epochs 500] [--batch-size N]\n"
                      << "            [--learning-rate 0.02] [--momentum 0.9] [--l2 0] [--seed 0]\n"
                      << "            [--channels 8] [--value-hidden 16] [--log-every 50] [--verify-fit]\n"
                      << "            [--output results/policy_value.json] [--report results/neural_training.json]\n"
                      << "            [--resume results/policy_value.json]\n"
                      << "network.exe --inspect results/policy_value.json [--position dataset.json] [--example 0]\n"
                      << "Without --position, inspect an empty board at --komi 7.5.\n"
                      << "The demo checks memorization; it does not measure playing strength.\n";
            return 0;
        }
        if (int(args.has("--make-demo")) + int(args.has("--train")) + int(args.has("--inspect")) != 1)
            throw std::invalid_argument("Choose one of --make-demo, --train, or --inspect");
        for (const auto* key : {"--resume", "--output", "--report", "--epochs", "--batch-size", "--learning-rate",
                               "--momentum", "--l2", "--seed", "--channels", "--value-hidden", "--log-every", "--verify-fit"})
            if (args.has(key) && !args.has("--train")) throw std::invalid_argument(std::string(key) + " requires --train");
        for (const auto* key : {"--position", "--example", "--komi"})
            if (args.has(key) && !args.has("--inspect")) throw std::invalid_argument(std::string(key) + " requires --inspect");
        if (args.has("--example") && !args.has("--position"))
            throw std::invalid_argument("--example requires --position");
        if (args.has("--komi") && args.has("--position"))
            throw std::invalid_argument("The dataset supplies komi; omit --komi with --position");
        if (args.has("--make-demo")) {
            auto data = make_demo_dataset();
            save_records(args.text("--make-demo"), data);
            std::cout << "Saved " << data.at("examples").size() << " scripted 9x9 training examples to "
                      << args.text("--make-demo") << '\n';
            return 0;
        }
        std::cout << std::fixed << std::setprecision(6);
        if (args.has("--inspect")) {
            auto network = PolicyValueNetwork::load(args.text("--inspect"));
            network.train(false);
            std::cout << "Loaded " << network.settings().board_size << 'x' << network.settings().board_size << ", "
                      << network.parameter_count() << " parameters, " << network.training_steps() << " updates\n";
            if (args.has("--position")) {
                auto examples = load_dataset(args.text("--position"));
                int index = args.integer("--example", 0);
                if (index < 0 || static_cast<std::size_t>(index) >= examples.size())
                    throw std::invalid_argument("Example index is out of bounds");
                show_prediction(network, examples[index].input);
                std::cout << "Scripted target value: " << examples[index].value << '\n';
            } else show_prediction(network, encode_position(GameState::new_game(network.settings().board_size, args.real("--komi", 7.5))));
            return 0;
        }
        if (args.has("--resume") && (args.has("--channels") || args.has("--value-hidden")))
            throw std::invalid_argument("A resumed checkpoint supplies its architecture; omit --channels and --value-hidden");
        int epochs = args.integer("--epochs", 500), log_every = args.integer("--log-every", 50);
        if (epochs < 1 || log_every < 1) throw std::invalid_argument("Epoch count and logging interval must be positive");
        OptimizerSettings optimizer{args.real("--learning-rate", 0.02), args.real("--momentum", 0.9), args.real("--l2", 0)};
        optimizer.validate();
        auto examples = load_dataset(args.text("--train"));
        if (examples.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            throw std::invalid_argument("Dataset has too many examples");
        int requested_batch = args.integer("--batch-size", static_cast<int>(examples.size()));
        if (requested_batch < 1) throw std::invalid_argument("Batch size must be positive");
        int batch_size = std::min(requested_batch, static_cast<int>(examples.size()));
        auto seed = args.integer<std::int64_t>("--seed", 0);
        NetworkSettings settings{examples.front().input.board_size, args.integer("--channels", 8), args.integer("--value-hidden", 16)};
        auto network = args.has("--resume") ? PolicyValueNetwork::load(args.text("--resume")) : PolicyValueNetwork(settings, seed);
        if (network.settings().board_size != settings.board_size) throw std::invalid_argument("Dataset size disagrees with checkpoint");
        auto output = args.text("--output", "results/policy_value.json"), report_path = args.text("--report", "results/neural_training.json");
        if (same_path(output, report_path) || same_path(output, args.text("--train")) || same_path(report_path, args.text("--train")) ||
            (args.has("--resume") && same_path(report_path, args.text("--resume"))))
            throw std::invalid_argument("Dataset, checkpoint, and report must use separate paths");
        auto initial = fit_report(network, examples, optimizer.l2);
        auto initial_steps = network.training_steps();
        network.train();
        std::vector<std::size_t> order(examples.size()); std::iota(order.begin(), order.end(), 0);
        Random shuffle(seed);
        Json history = Json::array();
        std::cout << "Training " << examples.size() << " positions in batches of " << batch_size << ", "
                  << network.parameter_count() << " parameters on CPU\n";
        auto started = std::chrono::steady_clock::now();
        for (int epoch = 0; epoch < epochs; ++epoch) {
            if (static_cast<std::size_t>(batch_size) < examples.size())
                for (std::size_t i = order.size(); i > 1; --i) std::swap(order[i - 1], order[shuffle.below(i)]);
            for (std::size_t begin = 0; begin < order.size(); begin += static_cast<std::size_t>(batch_size)) {
                std::vector<TrainingExample> batch;
                auto end = std::min(order.size(), begin + static_cast<std::size_t>(batch_size));
                for (std::size_t i = begin; i < end; ++i) batch.push_back(examples[order[i]]);
                network.train_batch(batch, optimizer);
            }
            if (epoch == 0 || (epoch + 1) % log_every == 0 || epoch + 1 == epochs) {
                auto fit = fit_report(network, examples, optimizer.l2);
                std::cout << "Epoch " << epoch + 1 << ": policy KL " << fit.at("policy_kl").get<double>()
                          << "; value MSE " << fit.at("loss").at("value_mse").get<double>() << '\n' << std::flush;
                history.push_back({{"epoch", epoch + 1}, {"steps", network.training_steps()}, {"metrics", fit}});
            }
        }
        double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        network.train(false);
        auto final = fit_report(network, examples, optimizer.l2);
        network.save(output);
        Json report = {{"schema_version", 1}, {"kind", "policy_value_training"}, {"feature_schema", FEATURE_SCHEMA},
            {"dataset", args.text("--train")}, {"checkpoint", output},
            {"resume_checkpoint", args.has("--resume") ? Json(args.text("--resume")) : Json(nullptr)},
            {"settings", {{"board_size", network.settings().board_size}, {"channels", network.settings().channels},
                {"value_hidden", network.settings().value_hidden}, {"seed", seed}, {"epochs", epochs}, {"batch_size", batch_size},
                {"learning_rate", optimizer.learning_rate}, {"momentum", optimizer.momentum}, {"l2", optimizer.l2}}},
            {"num_examples", examples.size()}, {"parameter_count", network.parameter_count()},
            {"initial_training_steps", initial_steps}, {"final_training_steps", network.training_steps()},
            {"elapsed_seconds", elapsed}, {"initial", initial}, {"final", final}, {"history", history},
            {"build", {{"git_revision", build_info::git_available ? Json(build_info::revision) : Json(nullptr)},
                {"git_dirty", build_info::git_available ? Json(build_info::dirty) : Json(nullptr)},
                {"source_sha256", build_info::source_sha256}, {"compiler", build_info::compiler}, {"profile", BETAGO_BUILD_PROFILE}}}};
        save_records(report_path, report);
        std::cout << "Final policy accuracy: " << final.at("policy_top1_accuracy").get<double>()
                  << "; value MSE: " << final.at("loss").at("value_mse").get<double>() << '\n';
        std::cout << "Saved " << output << " and " << report_path << '\n';
        if (args.has("--verify-fit") && !final.at("fits_training_set").get<bool>()) {
            std::cerr << "Training-set fit check failed. Inspect the saved losses and targets.\n";
            return 2;
        }
        if (args.has("--verify-fit")) std::cout << "Training-set fit check passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
