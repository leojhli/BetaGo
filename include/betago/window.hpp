#pragma once
#include "random.hpp"
#include "tk.hpp"
#include <iosfwd>
#include <utility>

namespace betago {
class GoWindow {
public:
    GoWindow(std::int64_t seed = 0, int max_moves = 500, int delay_ms = 500);
    void start_random();
    void run() { tk_.loop(); }
    void self_test();
    void dump_canvas(const std::filesystem::path& path);
private:
    static constexpr int MARGIN = 52, SPACING = 58;
    static const std::vector<Point> DEMO;
    TkRuntime tk_;
    GameState state_ = GameState::new_game();
    std::vector<std::pair<GameState, Move>> history_;
    Move last_move_;
    std::string demo_job_, random_job_;
    std::size_t demo_index_ = 0;
    bool random_truncated_ = false;
    std::int64_t seed_;
    int max_moves_, delay_ms_;
    RandomAgent black_, white_;
    static int callback(void* data, Tcl_Interp*, int count, Tcl_Obj* const objects[]);
    void action(const std::vector<std::string>& args);
    void draw_board();
    void draw_stone(std::ostringstream& out, int x, int y, int color);
    void draw();
    void click(int x, int y);
    void play(Move move);
    void stop_demo();
    void stop_random();
    void new_game();
    void undo();
    void toggle_demo();
    void demo_step();
    void toggle_random();
    void random_step();
    void close();
    void notice(const std::string& message);
    void mode(const std::string& message);
    static std::string rgb(double red, double green, double blue);
};
} // namespace betago
