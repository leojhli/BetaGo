#include "betago/options.hpp"
#include "betago/profile.hpp"
#include "betago/selfplay.hpp"
#include "build_info.hpp"
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

namespace {
using namespace betago;
using Clock = std::chrono::steady_clock;
std::uint64_t sink = 0;
std::string fingerprint(const Json& value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char byte : value.dump()) { hash ^= byte; hash *= 1099511628211ULL; }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}
Json prediction_json(const Prediction& p) {
    return {{"logits", p.logits}, {"policy", p.policy}, {"value", p.value}};
}
Json loss_json(const LossMetrics& l) {
    return {{"policy", l.policy}, {"value", l.value}, {"regularization", l.regularization}, {"total", l.total}};
}
Json statistics_json(const SearchStatistics& s) {
    Json children = Json::array();
    for (const auto& child : s.children)
        children.push_back({{"move", child.move ? Json::array({child.move->row, child.move->column}) : Json(nullptr)},
            {"visits", child.visits}, {"value_sum", child.value_sum}, {"prior", child.prior}});
    return {{"simulations", s.simulations}, {"network", s.network_evaluations},
        {"terminal", s.terminal_evaluations}, {"children", std::move(children)}};
}
Json optimizer_history(const PolicyValueNetwork& network) {
    Json settings = nullptr;
    if (network.last_optimizer()) {
        const auto& o = *network.last_optimizer();
        settings = {{"learning_rate", o.learning_rate}, {"momentum", o.momentum}, {"l2", o.l2}};
    }
    return {{"velocity", fingerprint(network.velocity())}, {"training_steps", network.training_steps()},
        {"last_optimizer", std::move(settings)}};
}
Json profile_json(const ProfileSession& session) {
    Json rows = Json::array();
    for (std::size_t i = 0; i < PROFILE_NAMES.size(); ++i) {
        const auto& m = session.metrics()[i];
        rows.push_back({{"work", PROFILE_NAMES[i]}, {"calls", m.calls},
            {"inclusive_seconds", m.inclusive_seconds}, {"exclusive_seconds", m.exclusive_seconds}});
    }
    return {{"wall_seconds", session.elapsed_seconds()}, {"scopes", std::move(rows)}};
}
template<class Function> Json time_work(int repeats, std::size_t units, Function work) {
    work(); // Warm-up is excluded; all measured samples have profiling disabled.
    std::vector<double> samples;
    for (int i = 0; i < repeats; ++i) {
        const auto start = Clock::now();
        work();
        samples.push_back(std::chrono::duration<double>(Clock::now() - start).count());
    }
    auto ordered = samples;
    std::sort(ordered.begin(), ordered.end());
    const double median = ordered.size() % 2 ? ordered[ordered.size()/2] :
        (ordered[ordered.size()/2-1] + ordered[ordered.size()/2]) / 2;
    return {{"samples_seconds", samples}, {"median_seconds", median}, {"units_per_sample", units},
        {"units_per_second", median > 0 ? Json(units / median) : Json(nullptr)}};
}
std::vector<GameState> positions(int size) {
    std::vector<GameState> states{GameState::new_game(size)};
    RandomAgent random(12345);
    auto state = states.front();
    for (int ply = 1; ply <= size * size && !state.is_terminal(); ++ply) {
        state = state.play(random.choose_move(state));
        if (!state.is_terminal() && ply % std::max(1, size * size / 4) == 0) states.push_back(state);
    }
    return states;
}
void protect_output(const std::filesystem::path& output, const std::filesystem::path& input) {
    if (input.empty()) return;
    std::error_code error;
    if (std::filesystem::equivalent(output, input, error) && !error)
        throw std::invalid_argument("--output must not overwrite the input checkpoint");
    // weakly_canonical resolves existing parents and Windows path aliases.
    auto normalized = [](const auto& path) {
        auto text = std::filesystem::weakly_canonical(std::filesystem::absolute(path)).generic_wstring();
#if defined(_WIN32)
        for (auto& character : text) character = static_cast<wchar_t>(std::towlower(character));
#endif
        return text;
    };
    if (normalized(output) == normalized(input))
        throw std::invalid_argument("--output must not overwrite the input checkpoint");
}
}

int main(int argc, char** argv) {
    using namespace betago;
    try {
        Options args(argc, argv, {"--checkpoint", "--output", "--compare", "--repeats", "--simulations", "--selfplay-moves"}, {"--help"});
        if (args.has("--help")) {
            std::cout << "profile [--checkpoint model.json] [--output results/profile.json] [--repeats 5]\n"
                         "        [--simulations 64] [--selfplay-moves 32] [--compare earlier-profile.json]\n"
                         "Fixed seeded positions, separate untimed behavior fingerprints and opt-in scope profiling.\n";
            return 0;
        }
        const int repeats = args.integer("--repeats", 5);
        const int simulations = args.integer("--simulations", 64);
        const int moves = args.integer("--selfplay-moves", 32);
        if (repeats < 1 || repeats > 100 || moves < 1 || moves > 10000)
            throw std::invalid_argument("repeats must be 1..100 and selfplay-moves 1..10000");
        NeuralMctsSettings settings{simulations, 1.5}; settings.validate();
        const auto checkpoint = args.text("--checkpoint");
        const auto output = args.text("--output", "results/profile.json");
        protect_output(output, checkpoint);
        Json baseline;
        if (args.has("--compare")) {
            protect_output(output, args.text("--compare"));
            std::ifstream baseline_file(args.text("--compare"), std::ios::binary);
            if (!baseline_file) throw std::invalid_argument("cannot open comparison profile");
            baseline = Json::parse(baseline_file);
            if (baseline.at("kind") != "performance_profile" || baseline.at("schema_version") != 1)
                throw std::invalid_argument("comparison input must be a performance profile with schema 1");
        }
        auto network = std::make_shared<const PolicyValueNetwork>(checkpoint.empty() ?
            PolicyValueNetwork{} : PolicyValueNetwork::load(checkpoint));
        const int size = network->settings().board_size;
        const auto states = positions(size);
        std::vector<EncodedPosition> inputs;
        for (const auto& state : states) inputs.push_back(encode_position(state));
        std::vector<TrainingExample> batch;
        for (int i = 0; i < 32; ++i) {
            const auto& input = inputs[i % inputs.size()];
            std::vector<double> policy(input.legal_actions.size(), 0);
            const double mass = 1.0 / std::count(input.legal_actions.begin(), input.legal_actions.end(), true);
            for (std::size_t a = 0; a < policy.size(); ++a) if (input.legal_actions[a]) policy[a] = mass;
            batch.push_back({input, std::move(policy), i % 2 ? -1.0 : 1.0});
        }
        auto search_work = [&] {
            NeuralMctsAgent agent(network, settings);
            for (const auto& state : states) { agent.choose_move(state); sink += agent.last_search().simulations; }
        };
        SelfPlaySettings self_settings;
        self_settings.board_size = size; self_settings.max_moves = moves; self_settings.search = settings;
        auto self_work = [&] { auto game = run_self_play(network, self_settings, 101); sink += game.record.at("game_length").get<int>(); };
        Json timing;
        timing["state_copy"] = time_work(repeats, states.size() * 1000, [&] {
            for (int i = 0; i < 1000; ++i) for (const auto& state : states) { auto copy = state; sink += copy.board().size(); }
        });
        timing["groups"] = time_work(repeats, states.size() * 100, [&] {
            for (int i = 0; i < 100; ++i) for (const auto& state : states)
                for (int r = 0; r < size; ++r) for (int c = 0; c < size; ++c)
                    if (state.at({r,c}) != EMPTY) sink += state.group_and_liberties({r,c}).stones.size();
        });
        timing["encoding"] = time_work(repeats, states.size() * 100, [&] {
            for (int i = 0; i < 100; ++i) for (const auto& state : states) sink += encode_position(state).features.size();
        });
        timing["inference"] = time_work(repeats, inputs.size() * 100, [&] {
            for (int i = 0; i < 100; ++i) for (const auto& input : inputs) sink += network->predict(input).policy.size();
        });
        timing["training"] = time_work(repeats, 10, [&] {
            auto candidate = *network; candidate.train();
            for (int i = 0; i < 10; ++i) candidate.train_batch(batch, {0.01, 0.9, 0.0001});
            sink += candidate.training_steps();
        });
        timing["search"] = time_work(repeats, states.size() * simulations, search_work);
        timing["self_play"] = time_work(repeats, 1, self_work);
        Json profiles;
        { ProfileSession session; search_work(); profiles["search"] = profile_json(session); }
        { ProfileSession session; self_work(); profiles["self_play"] = profile_json(session); }
        { ProfileSession session; network->loss_and_gradient(batch, 0.0001); profiles["training_batch"] = profile_json(session); }

        Json predictions = Json::array(), searches = Json::array(), board_records = Json::array();
        NeuralMctsAgent agent(network, settings);
        for (std::size_t i = 0; i < states.size(); ++i) {
            predictions.push_back(prediction_json(network->predict(inputs[i])));
            agent.choose_move(states[i]); searches.push_back(statistics_json(agent.last_search()));
            board_records.push_back({{"board", states[i].board()}, {"to_play", states[i].to_play()},
                {"previous_board", states[i].previous_board() ? Json(*states[i].previous_board()) : Json(nullptr)},
                {"passes", states[i].consecutive_passes()}});
        }
        const auto gradient = network->loss_and_gradient(batch, 0.0001);
        auto candidate = *network; candidate.train();
        candidate.train_batch(batch, {0.01, 0.9, 0.0001}); candidate.train_batch(batch, {0.01, 0.9, 0.0001});
        auto game = run_self_play(network, self_settings, 101);
        // Search timestamps and measured elapsed seconds are intentionally excluded.
        Json game_behavior = {{"moves", game.record.at("moves")}, {"outcome", game.record.at("winner")},
            {"score", game.record.at("score")}};
        Json policies = Json::array();
        for (const auto& target : game.record.at("targets")) policies.push_back(target.at("policy"));
        game_behavior["policies"] = std::move(policies);
        const char* cpu = std::getenv("PROCESSOR_IDENTIFIER");
        const char* host = std::getenv("COMPUTERNAME");
        Json document = {{"schema_version", 1}, {"kind", "performance_profile"},
            {"build", {{"source_sha256", build_info::source_sha256}, {"compiler", build_info::compiler},
                {"flags", build_info::flags}, {"built_at_utc", build_info::built_at_utc}}},
            {"machine", {{"cpu", cpu ? Json(cpu) : Json(nullptr)}, {"hostname", host ? Json(host) : Json(nullptr)},
                {"hardware_threads", std::thread::hardware_concurrency()}}},
            {"workload", {{"checkpoint", checkpoint}, {"parameters", fingerprint(network->parameters())},
                {"board_size", size}, {"channels", network->settings().channels}, {"value_hidden", network->settings().value_hidden},
                {"positions", std::move(board_records)}, {"simulations", simulations}, {"c_puct", settings.c_puct},
                {"self_play_settings", self_settings.to_json()}, {"self_play_seed", 101}, {"repeats", repeats},
                {"training_batch_size", 32}, {"training_updates", 10}, {"optimizer", {{"learning_rate", 0.01}, {"momentum", 0.9}, {"l2", 0.0001}}}}},
            {"timing_without_profiling", timing}, {"profiles", profiles},
            {"behavior", {{"predictions", fingerprint(predictions)},
                {"loss_gradient", fingerprint(Json{{"loss", loss_json(gradient.loss)}, {"gradient", gradient.gradient}})},
                {"two_updates", fingerprint(Json{{"parameters", candidate.parameters()}, {"velocity", candidate.velocity()}})},
                {"searches", fingerprint(searches)}, {"self_play", fingerprint(game_behavior)}}},
            {"notes", "Synthetic uniform-policy training batch measures speed, not learning or playing strength. Group units count position sweeps. Inclusive scopes overlap; exclusive scopes exclude nested measured work. Profiling adds clock overhead; compare unprofiled medians with matching workloads."}};
        document["workload"]["optimizer_history"] = optimizer_history(*network);
        document["self_play_result"] = {{"moves", game.record.at("game_length")},
            {"termination_reason", game.record.at("termination_reason")}, {"training_examples", game.examples.size()}};
        document["benchmark_sink"] = sink;
        bool matched = true;
        if (!baseline.is_null()) {
            Json previous_work = baseline.at("workload"), current_work = document.at("workload");
            for (const auto* field : {"repeats", "checkpoint", "optimizer_history"}) {
                previous_work.erase(field); current_work.erase(field);
            }
            const bool workload_matches = previous_work == current_work;
            const bool behavior_matches = baseline.at("behavior") == document.at("behavior");
            const bool machine_matches = baseline.at("machine") == document.at("machine");
            const bool build_matches = baseline.at("build").at("compiler") == document.at("build").at("compiler") &&
                baseline.at("build").at("flags") == document.at("build").at("flags");
            const bool history_available = baseline.at("workload").contains("optimizer_history");
            const bool history_matches = !history_available ||
                baseline.at("workload").at("optimizer_history") == document.at("workload").at("optimizer_history");
            Json speedups;
            for (const auto& item : timing.items()) {
                const auto& before = baseline.at("timing_without_profiling").at(item.key());
                const double earlier = before.at("median_seconds").get<double>();
                const double now = item.value().at("median_seconds").get<double>();
                if (!std::isfinite(earlier) || earlier <= 0 || before.at("units_per_sample") != item.value().at("units_per_sample"))
                    throw std::invalid_argument("comparison has invalid timings or differing work counts");
                speedups[item.key()] = now > 0 ? Json(earlier / now) : Json(nullptr);
            }
            matched = workload_matches && behavior_matches && machine_matches && build_matches && history_matches;
            document["comparison"] = {{"baseline", args.text("--compare")}, {"matching_workload", workload_matches},
                {"matching_behavior", behavior_matches}, {"matching_machine", machine_matches},
                {"matching_compiler_flags", build_matches},
                {"matching_optimizer_history", history_available ? Json(history_matches) : Json(nullptr)},
                {"speedup", std::move(speedups)}};
            std::cout << "Behavior comparison: " << (behavior_matches ? "identical" : "DIFFERENT") << '\n';
        }
        save_records(output, document);
        for (const auto* work : {"search", "self_play", "training"})
            std::cout << work << ": " << timing[work]["median_seconds"] << " seconds (median)\n";
        std::cout << "Saved " << output << '\n';
        if (!matched) { std::cerr << "Comparison differs; inspect the saved workload, behavior and environment checks.\n"; return 1; }
        return 0;
    } catch (const std::exception& error) { std::cerr << "Error: " << error.what() << '\n'; return 2; }
}
