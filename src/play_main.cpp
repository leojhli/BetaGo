#include "betago/options.hpp"
#include "betago/window.hpp"
#include <iostream>

int main(int argc, char** argv) {
    using namespace betago;
    try {
        Options args(argc, argv, {"--seed", "--max-moves", "--delay-ms", "--dump-canvas"}, {"--random", "--help", "--self-test"});
        if (args.has("--help")) {
            std::cout << "BetaGo visual 9x9 board\nplay.exe [--random] [--seed 0] [--max-moves 500] [--delay-ms 500]\n";
            return 0;
        }
        int limit = args.integer("--max-moves", 500), delay = args.integer("--delay-ms", 500);
        if (limit < 1 || delay < 1) throw std::invalid_argument("Move limit and delay must be positive");
        GoWindow window(args.integer<std::int64_t>("--seed", 0), limit, delay);
        if (args.has("--self-test")) window.self_test();
        else if (args.has("--dump-canvas")) window.dump_canvas(args.text("--dump-canvas"));
        else { if (args.has("--random")) window.start_random(); window.run(); }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
