#include "betago/options.hpp"
#include "betago/window.hpp"
#include <iostream>

int main(int argc, char** argv) {
    using namespace betago;
    try {
        Options args(argc, argv, {"--seed", "--max-moves", "--delay-ms", "--dump-canvas", "--simulations", "--rollout-limit", "--exploration"},
            {"--random", "--mcts", "--watch-mcts", "--help", "--self-test"});
        if (args.has("--help")) {
            std::cout << "BetaGo visual 9x9 board\nplay.exe [--random | --mcts | --watch-mcts] [--seed 0]\n"
                      << "         [--max-moves 500] [--delay-ms 500] [--simulations 128]\n"
                      << "         [--rollout-limit 200] [--exploration 1.4142135623730951]\n"
                      << "--mcts: play Black against MCTS White; --watch-mcts: MCTS Black vs random White\n";
            return 0;
        }
        int limit = args.integer("--max-moves", 500), delay = args.integer("--delay-ms", 500);
        if (limit < 1 || delay < 1) throw std::invalid_argument("Move limit and delay must be positive");
        if (int(args.has("--random")) + int(args.has("--mcts")) + int(args.has("--watch-mcts")) > 1)
            throw std::invalid_argument("Choose one of --random, --mcts, or --watch-mcts");
        MctsSettings settings{args.integer("--simulations", 128), args.real("--exploration", 1.4142135623730951), args.integer("--rollout-limit", 200)};
        settings.validate();
        GoWindow window(args.integer<std::int64_t>("--seed", 0), limit, delay, settings);
        if (args.has("--self-test")) window.self_test();
        else if (args.has("--dump-canvas")) window.dump_canvas(args.text("--dump-canvas"));
        else {
            if (args.has("--random")) window.start_random();
            else if (args.has("--mcts")) window.start_mcts();
            else if (args.has("--watch-mcts")) window.start_mcts(true);
            window.run();
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
