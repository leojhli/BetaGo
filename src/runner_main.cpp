#include "betago/options.hpp"
#include "betago/runner.hpp"
#include <iostream>
#include <limits>

int main(int argc, char** argv) {
    using namespace betago;
    try {
        Options args(argc, argv, {"--size", "--komi", "--games", "--seed", "--max-moves", "--output", "--replay"}, {"--help"});
        if (args.has("--help")) {
            std::cout << "BetaGo random game runner\n"
                      << "runner.exe [--size 9] [--komi 7.5] [--games 1] [--seed 0]\n"
                      << "           [--max-moves 500] [--output results/random_games.json]\n"
                      << "runner.exe --replay results/random_games.json\n";
            return 0;
        }
        if (args.has("--replay")) {
            auto data = load_records(args.text("--replay"));
            int index = 0;
            for (const auto& record : data.at("games")) {
                std::cout << "Game " << ++index << ": " << describe(record) << '\n';
                std::cout << replay_moves(moves_from_json(record.at("moves")), record.at("size"), record.at("komi")).to_string() << '\n';
            }
            return 0;
        }
        int games = args.integer("--games", 1);
        if (games < 1) throw std::invalid_argument("Number of games must be positive");
        auto seed = args.integer<std::int64_t>("--seed", 0);
        if (seed > std::numeric_limits<std::int64_t>::max() - (2LL * games - 1))
            throw std::invalid_argument("Batch seeds exceed signed 64-bit range");
        Json records = Json::array();
        int completed = 0;
        for (int index = 0; index < games; ++index) {
            auto black_seed = seed + 2LL * index, white_seed = black_seed + 1;
            RandomAgent black(black_seed), white(white_seed);
            auto result = run_game([&](const GameState& s) { return black.choose_move(s); },
                                   [&](const GameState& s) { return white.choose_move(s); },
                                   args.integer("--size", 9), args.real("--komi", 7.5), args.integer("--max-moves", 500));
            Json record = result.to_json();
            record["black_seed"] = black_seed;
            record["white_seed"] = white_seed;
            records.push_back(record);
            completed += result.final_state.is_terminal();
            std::cout << "Game " << index + 1 << ": " << describe(record) << '\n';
        }
        Json data = {{"schema_version", 1}, {"seed", seed},
            {"agents", {{"black", "random"}, {"white", "random"}}},
            {"rules", "simple ko, no suicide, area scoring, no dead-group adjudication"}, {"games", records}};
        auto output = args.text("--output", "results/random_games.json");
        save_records(output, data);
        std::cout << "Completed: " << completed << "; truncated: " << games - completed << "\nSaved " << output << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
