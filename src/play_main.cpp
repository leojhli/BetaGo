#include "betago/options.hpp"
#include "betago/window.hpp"
#include <iostream>

int main(int argc, char** argv) {
    using namespace betago;
    try {
        Options args(argc, argv, {"--seed", "--max-moves", "--delay-ms", "--dump-canvas", "--simulations", "--rollout-limit", "--exploration", "--checkpoint", "--c-puct", "--watch-training"},
            {"--random", "--mcts", "--watch-mcts", "--neural", "--watch-neural", "--help", "--self-test"});
        if (args.has("--help")) {
            std::cout << "BetaGo visual 9x9 board\nplay.exe [--random | --mcts | --watch-mcts | --neural | --watch-neural] [--seed 0]\n"
                      << "         [--max-moves 500] [--delay-ms 500] [--simulations 128]\n"
                      << "         [--rollout-limit 200] [--exploration 1.4142135623730951]\n"
                      << "         [--checkpoint results/policy_value.json] [--c-puct 1.5]\n"
                      << "play.exe --watch-training results/selfplay/live.json\n"
                      << "--mcts: play Black against MCTS White; --watch-mcts: MCTS Black vs random White\n"
                      << "--neural and --watch-neural use a required 9x9 checkpoint with PUCT search.\n"
                      << "A checkpoint without a startup flag enables the neural MCTS buttons in local play.\n"
                      << "--watch-training follows real training updates. Pause view and closing the board leave training running.\n";
            return 0;
        }
        int limit = args.integer("--max-moves", 500), delay = args.integer("--delay-ms", 500);
        if (limit < 1 || delay < 1) throw std::invalid_argument("Move limit and delay must be positive");
        if (int(args.has("--random")) + int(args.has("--mcts")) + int(args.has("--watch-mcts")) +
            int(args.has("--neural")) + int(args.has("--watch-neural")) + int(args.has("--watch-training")) > 1)
            throw std::invalid_argument("Choose one startup mode: --random, --mcts, --watch-mcts, --neural, --watch-neural, or --watch-training");
        if (args.has("--watch-training") && (args.has("--checkpoint") || args.has("--self-test") || args.has("--dump-canvas") ||
            args.has("--simulations") || args.has("--rollout-limit") || args.has("--exploration") || args.has("--c-puct")))
            throw std::invalid_argument("--watch-training cannot use a checkpoint, local search options, or GUI diagnostics");
        if ((args.has("--neural") || args.has("--watch-neural")) && !args.has("--checkpoint"))
            throw std::invalid_argument("Neural MCTS requires --checkpoint");
        if (args.has("--checkpoint") && (args.has("--mcts") || args.has("--watch-mcts")))
            throw std::invalid_argument("Use --neural or --watch-neural with --checkpoint");
        if (args.has("--checkpoint") && (args.has("--rollout-limit") || args.has("--exploration")))
            throw std::invalid_argument("Neural MCTS uses --c-puct, not --rollout-limit or --exploration");
        if (args.has("--c-puct") && !args.has("--checkpoint"))
            throw std::invalid_argument("--c-puct requires --checkpoint");
        MctsSettings settings{args.integer("--simulations", 128), args.real("--exploration", 1.4142135623730951), args.integer("--rollout-limit", 200)};
        settings.validate();
        NeuralMctsSettings neural_settings{settings.simulations, args.real("--c-puct", 1.5)};
        std::shared_ptr<const PolicyValueNetwork> network;
        if (args.has("--checkpoint")) {
            neural_settings.validate();
            auto loaded = std::make_shared<PolicyValueNetwork>(PolicyValueNetwork::load(args.text("--checkpoint")));
            if (loaded->settings().board_size != 9)
                throw std::invalid_argument("The visual board requires a 9x9 checkpoint");
            loaded->train(false);
            network = std::move(loaded);
        }
        GoWindow window(args.integer<std::int64_t>("--seed", 0), limit, delay, settings, std::move(network), neural_settings);
        if (args.has("--self-test")) window.self_test();
        else if (args.has("--dump-canvas")) window.dump_canvas(args.text("--dump-canvas"));
        else {
            if (args.has("--watch-training")) window.start_training_watch(args.text("--watch-training"));
            else if (args.has("--random")) window.start_random();
            else if (args.has("--mcts")) window.start_mcts();
            else if (args.has("--watch-mcts")) window.start_mcts(true);
            else if (args.has("--neural")) window.start_mcts();
            else if (args.has("--watch-neural")) window.start_mcts(true);
            window.run();
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
