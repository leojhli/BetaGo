#include "betago/window.hpp"
#include "betago/runner.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
#include <sstream>

namespace betago {
const std::vector<Point> GoWindow::DEMO = {{0, 1}, {1, 1}, {1, 0}, {8, 8}, {2, 1}, {8, 7}, {1, 2}};
GoWindow::GoWindow(std::int64_t seed, int max_moves, int delay_ms, MctsSettings settings)
    : seed_(seed), max_moves_(max_moves), delay_ms_(delay_ms), black_(seed), white_(0), mcts_settings_(settings) {
    if (max_moves < 1 || delay_ms < 1) throw std::invalid_argument("Move limit and delay must be positive");
    if (seed == std::numeric_limits<std::int64_t>::max()) throw std::invalid_argument("White's seed exceeds signed 64-bit range");
    mcts_settings_.validate();
    tk_.command("betago", callback, this);
    tk_.eval(R"TK(
wm title . {BetaGo - 9x9 Go}
wm resizable . 0 0
. configure -background #202321
wm protocol . WM_DELETE_WINDOW {betago close}
ttk::style theme use clam
ttk::style configure TFrame -background #202321
ttk::style configure TLabel -background #202321 -foreground #ebe7dc
ttk::style configure TButton -background #363d36 -foreground #f4efe4 -borderwidth 0 -padding {12 9} -font {{Segoe UI} 10}
ttk::style map TButton -background {active #485448 disabled #292d29} -foreground {disabled #747a72}
ttk::frame .p -padding 20
pack .p -fill both -expand 1
ttk::label .p.title -text BetaGo -font {{Segoe UI} 24 bold}
pack .p.title -anchor w
set mode {9 x 9   /   Local play   /   Komi 7.5}
ttk::label .p.mode -textvariable mode -foreground #aeb7aa
pack .p.mode -anchor w -pady {0 16}
canvas .p.board -width 568 -height 568 -background #dbb374 -highlightthickness 0
pack .p.board
bind .p.board <Button-1> {betago click %x %y}
set status {}
ttk::label .p.status -textvariable status -font {{Segoe UI} 12 bold}
pack .p.status -anchor w -pady {12 3}
set notice {Click an intersection to place a stone.}
ttk::label .p.notice -textvariable notice -wraplength 540
pack .p.notice -anchor w
ttk::frame .p.buttons
pack .p.buttons -fill x -pady {12 0}
ttk::button .p.buttons.pass -text Pass -command {betago pass}
pack .p.buttons.pass -side left
ttk::button .p.buttons.undo -text Undo -command {betago undo}
pack .p.buttons.undo -side left -padx 6
ttk::button .p.buttons.new -text {New game} -command {betago new}
pack .p.buttons.new -side left
ttk::button .p.buttons.demo -text {Watch capture demo} -command {betago demo}
pack .p.buttons.demo -side right
ttk::frame .p.watch
pack .p.watch -fill x -pady {8 0}
ttk::button .p.watch.random -text {Watch random game} -command {betago random}
pack .p.watch.random -side left
ttk::button .p.watch.human -text {Play vs MCTS} -command {betago mcts}
pack .p.watch.human -side left -padx 6
ttk::button .p.watch.mcts -text {Watch MCTS vs random} -command {betago watch_mcts}
pack .p.watch.mcts -side left
ttk::label .p.footer -text {Finish captures before passing. Remaining stones count toward area.} -wraplength 540
pack .p.footer -anchor w -pady {12 0}
)TK");
    draw_board();
    draw();
}

int GoWindow::callback(void* data, Tcl_Interp*, int count, Tcl_Obj* const objects[]) {
    auto& window = *static_cast<GoWindow*>(data);
    try {
        std::vector<std::string> args;
        for (int i = 1; i < count; ++i) args.push_back(window.tk_.text(objects[i]));
        window.action(args);
        return TCL_OK;
    } catch (const std::exception& error) {
        window.tk_.set_result(error.what());
        return TCL_ERROR;
    }
}

void GoWindow::action(const std::vector<std::string>& args) {
    if (args.empty()) throw std::invalid_argument("Missing visual action");
    const auto& name = args[0];
    if (name == "click" && args.size() == 3) click(std::stoi(args[1]), std::stoi(args[2]));
    else if (name == "pass") {
        if (computer_turn()) notice("Wait for the computer's move, or stop MCTS to take over.");
        else play(PASS);
    }
    else if (name == "undo") undo();
    else if (name == "new") new_game();
    else if (name == "demo") toggle_demo();
    else if (name == "demo_step") demo_step();
    else if (name == "random") toggle_random();
    else if (name == "random_step") random_step();
    else if (name == "mcts") toggle_mcts(false);
    else if (name == "watch_mcts") toggle_mcts(true);
    else if (name == "mcts_step") mcts_step();
    else if (name == "close") close();
    else throw std::invalid_argument("Unknown visual action");
}

void GoWindow::notice(const std::string& text) { tk_.eval("set notice " + tcl_quote(text)); }
void GoWindow::mode(const std::string& text) { tk_.eval("set mode " + tcl_quote(text)); }
std::string GoWindow::rgb(double r, double g, double b) {
    std::ostringstream out;
    out << '#' << std::hex << std::setfill('0');
    for (double channel : {r, g, b}) out << std::setw(2) << std::clamp(static_cast<int>(std::nearbyint(channel)), 0, 255);
    return out.str();
}

void GoWindow::draw_board() {
    constexpr int side = 568;
    std::ostringstream out;
    out << std::setprecision(17);
    Random grain(19);
    for (int x = 0; x < side; ++x) {
        double tone = 4 * std::sin(x / 43.0) + 2 * std::sin(x / 13.0);
        out << ".p.board create line " << x << " 0 " << x << ' ' << side << " -fill " << rgb(210 + tone, 170 + tone, 105 + tone) << '\n';
    }
    const char* shades[] = {"#cba365", "#d8b174", "#d4ad6e", "#cfa768"};
    const int widths[] = {1, 1, 2};
    for (int i = 0; i < 240; ++i) {
        double x = grain.uniform(-12, side + 12);
        double phase = grain.uniform(0, 2 * std::numbers::pi);
        double amplitude = grain.uniform(1, 5);
        out << ".p.board create line";
        for (int y = -20; y < side + 30; y += 20)
            out << ' ' << x + amplitude * std::sin(y / 95.0 + phase) + 1.4 * std::sin(y / 37.0 + phase) << ' ' << y;
        auto shade = shades[grain.below(4)];
        int width = widths[grain.below(3)];
        out << " -smooth 1 -fill " << shade << " -width " << width << '\n';
    }
    out << ".p.board create rectangle 2 2 566 566 -outline #b48a4b -width 4\n";
    for (int index = 0; index < 9; ++index) {
        int offset = MARGIN + index * SPACING;
        out << ".p.board create line 52 " << offset << " 516 " << offset << " -fill #3d301e\n";
        out << ".p.board create line " << offset << " 52 " << offset << " 516 -fill #3d301e\n";
        for (int edge : {23, side - 23}) {
            out << ".p.board create text " << offset << ' ' << edge << " -text " << "ABCDEFGHJ"[index] << " -fill #494536 -font {{Segoe UI} 13 bold}\n";
            out << ".p.board create text " << edge << ' ' << offset << " -text " << 9 - index << " -fill #494536 -font {{Segoe UI} 13 bold}\n";
        }
    }
    for (Point p : {Point{2, 2}, Point{2, 6}, Point{4, 4}, Point{6, 2}, Point{6, 6}}) {
        int x = MARGIN + p.column * SPACING, y = MARGIN + p.row * SPACING;
        out << ".p.board create oval " << x - 3 << ' ' << y - 3 << ' ' << x + 3 << ' ' << y + 3 << " -fill #30291e -outline {}\n";
    }
    tk_.eval(out.str());
}

void GoWindow::draw_stone(std::ostringstream& out, int x, int y, int color) {
    constexpr int radius = 27;
    for (auto [spread, shade] : {std::pair{4, "#bd965c"}, {2, "#a88450"}, {0, "#876b43"}})
        out << ".p.board create oval " << x - radius - spread + 3 << ' ' << y - radius - spread + 4 << ' '
            << x + radius + spread + 3 << ' ' << y + radius + spread + 4 << " -fill " << shade << " -outline {} -tags stones\n";
    double base[3] = {23, 24, 25}, highlight[3] = {66, 66, 65};
    if (color == WHITE) { base[0] = 177; base[1] = 180; base[2] = 185; highlight[0] = 247; highlight[1] = 248; highlight[2] = 247; }
    for (int step = 0; step < 28; ++step) {
        double fraction = step / 27.0, r = radius * (1 - 0.91 * fraction), offset = -6 * fraction;
        double channels[3];
        for (int channel = 0; channel < 3; ++channel)
            channels[channel] = base[channel] + (highlight[channel] - base[channel]) * std::pow(fraction, 0.65);
        out << ".p.board create oval " << x + offset - r << ' ' << y + offset - r << ' ' << x + offset + r << ' ' << y + offset + r
            << " -fill " << rgb(channels[0], channels[1], channels[2]) << " -outline {} -tags stones\n";
    }
}

void GoWindow::draw() {
    std::ostringstream out;
    out << std::setprecision(17) << ".p.board delete stones\n";
    for (int r = 0; r < 9; ++r) for (int c = 0; c < 9; ++c) {
        int color = state_.at({r, c});
        if (color == EMPTY) continue;
        int x = MARGIN + c * SPACING, y = MARGIN + r * SPACING;
        draw_stone(out, x, y, color);
        if (last_move_ == Move(Point{r, c}))
            out << ".p.board create oval " << x - 10 << ' ' << y - 10 << ' ' << x + 10 << ' ' << y + 10
                << " -outline " << (color == BLACK ? "#ffffff" : "#333634") << " -width 2 -tags stones\n";
    }
    std::ostringstream status;
    if (state_.is_terminal()) {
        auto score = state_.score(); auto winner = state_.winner();
        status << (!winner ? "Draw" : *winner == BLACK ? "Black wins" : "White wins")
               << "  |  Black " << score.black << " - White " << score.white;
    } else if (random_truncated_) status << "Truncated at " << history_.size() << " moves  |  No final score";
    else status << (state_.to_play() == BLACK ? "Black" : "White") << " to play  |  Move " << history_.size() + 1
                << "  |  Passes " << state_.consecutive_passes() << "/2";
    out << "set status " << tcl_quote(status.str()) << '\n';
    bool watching = !demo_job_.empty() || !random_job_.empty() || computer_turn();
    out << ".p.buttons.pass configure -state " << (state_.is_terminal() || watching ? "disabled" : "normal") << '\n';
    out << ".p.buttons.undo configure -state " << (history_.empty() ? "disabled" : "normal") << '\n';
    tk_.eval(out.str());
}

void GoWindow::click(int x, int y) {
    if (!demo_job_.empty() || !random_job_.empty()) { notice("Stop playback to play your own moves."); return; }
    if (computer_turn()) { notice("Wait for the computer's move, or stop MCTS to take over."); return; }
    int column = static_cast<int>(std::nearbyint((x - MARGIN) / double(SPACING)));
    int row = static_cast<int>(std::nearbyint((y - MARGIN) / double(SPACING)));
    if (row < 0 || row >= 9 || column < 0 || column >= 9) return;
    if (std::abs(x - (MARGIN + column * SPACING)) <= 23 && std::abs(y - (MARGIN + row * SPACING)) <= 23)
        play(Point{row, column});
}

void GoWindow::play(Move move) {
    auto successor = state_;
    try { successor = state_.play(move); }
    catch (const IllegalMove& error) { notice(error.what()); return; }
    int opponent = state_.to_play() == BLACK ? WHITE : BLACK, captured = 0;
    for (int r = 0; r < 9; ++r) for (int c = 0; c < 9; ++c)
        captured += (state_.at({r, c}) == opponent) - (successor.at({r, c}) == opponent);
    history_.emplace_back(state_, last_move_);
    state_ = std::move(successor);
    random_truncated_ = false;
    last_move_ = move;
    if (state_.is_terminal()) notice("Game over after two passes. Undo to resume, or start a new game.");
    else if (!move) notice("Passed. Another pass will end the game.");
    else if (captured) notice("Captured " + std::to_string(captured) + " stone(s). The empty intersections can be played again.");
    else if (!random_job_.empty()) notice("Random agents are choosing legal moves, including pass.");
    else if (mcts_mode_) notice(mcts_watch_ ? "Watching MCTS (Black) vs random (White)." : "Your turn as Black. White uses MCTS.");
    else notice("Click an intersection to place a stone.");
    if (mcts_mode_) {
        if (state_.is_terminal()) {
            stop_mcts(); mode("9 x 9   /   MCTS game finished   /   Komi 7.5");
        } else if (mcts_watch_ && history_.size() >= static_cast<std::size_t>(max_moves_)) {
            stop_mcts(); random_truncated_ = true;
            mode("9 x 9   /   MCTS game truncated   /   Komi 7.5");
            notice("Move limit reached. No final score. Continue playing or start a new game.");
        } else schedule_mcts();
    }
    draw();
}

void GoWindow::stop_demo() {
    if (!demo_job_.empty()) { tk_.eval("after cancel " + tcl_quote(demo_job_)); demo_job_.clear(); }
    tk_.eval(".p.buttons.demo configure -text {Watch capture demo}");
}
void GoWindow::stop_random() {
    if (!random_job_.empty()) { tk_.eval("after cancel " + tcl_quote(random_job_)); random_job_.clear(); }
    tk_.eval(".p.watch.random configure -text {Watch random game}");
    mode("9 x 9   /   Local play   /   Komi 7.5");
}
void GoWindow::new_game() {
    stop_demo(); stop_random(); stop_mcts();
    state_ = GameState::new_game(); history_.clear(); last_move_.reset(); random_truncated_ = false;
    notice("Click an intersection to place a stone."); draw();
}
void GoWindow::undo() {
    stop_demo(); stop_random(); stop_mcts(); random_truncated_ = false;
    if (!history_.empty()) {
        state_ = history_.back().first; last_move_ = history_.back().second; history_.pop_back(); notice("Move undone.");
    }
    draw();
}
void GoWindow::toggle_demo() {
    if (!demo_job_.empty()) { stop_demo(); notice("Demo stopped. You can continue playing this position."); draw(); return; }
    new_game(); demo_index_ = 0;
    tk_.eval(".p.buttons.demo configure -text {Stop demo}");
    notice("Watch Black surround the white stone near the upper-left corner.");
    demo_job_ = tk_.eval("after 900 {betago demo_step}"); draw();
}
void GoWindow::demo_step() {
    play(DEMO.at(demo_index_++));
    if (demo_index_ == DEMO.size()) {
        demo_job_.clear(); tk_.eval(".p.buttons.demo configure -text {Watch capture demo}");
        notice("Capture! White's surrounded stone was removed. Continue playing or start a new game.");
    } else demo_job_ = tk_.eval("after 900 {betago demo_step}");
    draw();
}
void GoWindow::start_random() { toggle_random(); }
void GoWindow::toggle_random() {
    if (!random_job_.empty()) { stop_random(); notice("Random game stopped. You can continue playing this position."); draw(); return; }
    new_game(); black_ = RandomAgent(seed_); white_ = RandomAgent(seed_ + 1);
    mode("9 x 9   /   Random vs random   /   Seed " + std::to_string(seed_) + "   /   Komi 7.5");
    tk_.eval(".p.watch.random configure -text {Stop random game}");
    notice("Watch the random agents play. Stop playback to take over.");
    random_job_ = tk_.eval("after " + std::to_string(delay_ms_) + " {betago random_step}"); draw();
}
void GoWindow::random_step() {
    play((state_.to_play() == BLACK ? black_ : white_).choose_move(state_));
    if (state_.is_terminal()) { stop_random(); mode("9 x 9   /   Random game finished   /   Komi 7.5"); }
    else if (history_.size() >= static_cast<std::size_t>(max_moves_)) {
        stop_random(); random_truncated_ = true; mode("9 x 9   /   Random game truncated   /   Komi 7.5");
        notice("Move limit reached. No final score. Continue playing or start a new game.");
    } else random_job_ = tk_.eval("after " + std::to_string(delay_ms_) + " {betago random_step}");
    draw();
}
bool GoWindow::computer_turn() const {
    return mcts_mode_ && !state_.is_terminal() && (mcts_watch_ || state_.to_play() == WHITE);
}
void GoWindow::stop_mcts() {
    if (!mcts_job_.empty()) { tk_.eval("after cancel " + tcl_quote(mcts_job_)); mcts_job_.clear(); }
    // Searches borrow the agent's random generator, so destroy the search first.
    search_.reset(); mcts_agent_.reset(); mcts_mode_ = mcts_watch_ = false;
    tk_.eval(".p.watch.human configure -text {Play vs MCTS}; .p.watch.mcts configure -text {Watch MCTS vs random}");
    mode("9 x 9   /   Local play   /   Komi 7.5");
}
void GoWindow::start_mcts(bool watch) { toggle_mcts(watch); }
void GoWindow::toggle_mcts(bool watch) {
    if (mcts_mode_ && mcts_watch_ == watch) {
        stop_mcts(); notice("MCTS stopped. You can continue playing this position."); draw(); return;
    }
    new_game(); mcts_mode_ = true; mcts_watch_ = watch;
    mcts_agent_ = std::make_unique<MctsAgent>(mcts_settings_, watch ? seed_ : seed_ + 1);
    white_ = RandomAgent(seed_ + 1);
    mode(std::string("9 x 9   /   ") + (watch ? "MCTS vs random" : "You (Black) vs MCTS") + "   /   Komi 7.5");
    tk_.eval(watch ? ".p.watch.mcts configure -text {Stop MCTS game}" : ".p.watch.human configure -text {Stop MCTS}");
    notice(watch ? "Watching MCTS (Black) vs random (White)." : "Your turn as Black. White uses MCTS.");
    schedule_mcts(); draw();
}
void GoWindow::schedule_mcts() {
    if (computer_turn() && mcts_job_.empty())
        mcts_job_ = tk_.eval("after " + std::to_string(delay_ms_) + " {betago mcts_step}");
}
void GoWindow::mcts_step() {
    mcts_job_.clear();
    if (!computer_turn()) return;
    if (mcts_watch_ && state_.to_play() == WHITE) { play(white_.choose_move(state_)); return; }
    if (!search_) search_ = mcts_agent_->start_search(state_);
    // Yield to Tk after each bounded rollout, so reset, undo and stop work
    // throughout a search. The board is changed only when the budget is complete.
    search_->step();
    if (!search_->finished()) {
        auto stats = search_->statistics();
        notice("MCTS thinking: " + std::to_string(stats.simulations) + "/" + std::to_string(mcts_settings_.simulations) + " simulations.");
        mcts_job_ = tk_.eval("after 1 {betago mcts_step}"); return;
    }
    auto move = search_->best_move(); auto stats = search_->statistics(); search_.reset();
    play(move);
    if (mcts_mode_) notice("Last search: " + std::to_string(stats.simulations) + " simulations, " +
        std::to_string(stats.truncated_rollouts) + " rollouts cut off. " + (mcts_watch_ ? "Watching MCTS vs random." : "Your turn as Black."));
}
void GoWindow::close() { stop_demo(); stop_random(); stop_mcts(); tk_.eval("destroy ."); }

void GoWindow::self_test() {
    // Tk ignores mouse events for an unmapped canvas. Map it off-screen so the
    // test exercises the real event binding without displaying a test window.
    tk_.eval("wm geometry . +30000+30000; update");
    auto check = [](bool condition, const char* message) {
        if (!condition) throw std::runtime_error(std::string("GUI check failed: ") + message);
    };
    tk_.eval("event generate .p.board <Button-1> -x 52 -y 52");
    check(state_.at({0, 0}) == BLACK, "click placement");
    tk_.eval(".p.buttons.undo invoke");
    check(history_.empty(), "undo placement");
    tk_.eval(".p.buttons.pass invoke; .p.buttons.pass invoke");
    check(state_.is_terminal(), "two passes");
    check(tk_.eval(".p.buttons.pass cget -state") == "disabled", "terminal pass button");
    seed_ = 10; delay_ms_ = 1; max_moves_ = 500;
    tk_.eval(".p.watch.random invoke");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!random_job_.empty()) {
        tk_.eval("update");
        check(std::chrono::steady_clock::now() < deadline, "playback deadline");
    }
    RandomAgent black(10), white(11);
    auto expected = run_game([&](const GameState& s) { return black.choose_move(s); },
                             [&](const GameState& s) { return white.choose_move(s); });
    check(state_ == expected.final_state && history_.size() == expected.moves.size(), "seeded runner parity");
    max_moves_ = 1; tk_.eval(".p.watch.random invoke");
    while (!random_job_.empty()) tk_.eval("update");
    check(random_truncated_ && !state_.is_terminal(), "truncated playback");
    check(tk_.eval("set status").find("No final score") != std::string::npos, "truncation message");
    tk_.eval(".p.buttons.undo invoke");
    check(!random_truncated_ && history_.empty(), "undo truncation");
    delay_ms_ = 10000; max_moves_ = 500;
    tk_.eval(".p.watch.random invoke; .p.watch.random invoke");
    check(random_job_.empty(), "stop playback");
    tk_.eval(".p.watch.random invoke");
    tk_.eval("after cancel " + tcl_quote(random_job_));
    random_step(); // Undo becomes enabled once an actual move exists.
    tk_.eval(".p.buttons.undo invoke");
    check(random_job_.empty(), "undo stops playback");
    tk_.eval(".p.watch.random invoke; .p.buttons.new invoke");
    check(random_job_.empty() && history_.empty(), "reset stops playback");
    tk_.eval(".p.watch.random invoke; .p.buttons.demo invoke");
    check(random_job_.empty() && !demo_job_.empty(), "switch to capture demo");
    for (std::size_t i = 0; i < DEMO.size(); ++i) {
        tk_.eval("after cancel " + tcl_quote(demo_job_));
        demo_step();
    }
    check(state_.at({1, 1}) == EMPTY && demo_job_.empty(), "demo capture");
    tk_.eval(".p.buttons.demo invoke; .p.watch.random invoke");
    check(demo_job_.empty() && !random_job_.empty(), "switch to random game");
    stop_random();
    check(tk_.eval("after info").empty(), "cancel pending callbacks");
    mcts_settings_ = {4, 1.4142135623730951, 2}; delay_ms_ = 1;
    tk_.eval(".p.watch.human invoke");
    check(mcts_mode_ && !computer_turn() && mcts_job_.empty(), "human starts as Black");
    tk_.eval("event generate .p.board <Button-1> -x 52 -y 52");
    auto before_search = state_;
    check(computer_turn() && !mcts_job_.empty(), "human move starts White search");
    check(tk_.eval(".p.buttons.pass cget -state") == "disabled", "pass blocked while computer thinks");
    tk_.eval("event generate .p.board <Button-1> -x 110 -y 52");
    check(state_ == before_search, "click blocked while computer thinks");
    MctsAgent expected_mcts(mcts_settings_, seed_ + 1);
    auto expected_move = expected_mcts.choose_move(before_search);
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (computer_turn()) {
        tk_.eval("update");
        check(std::chrono::steady_clock::now() < deadline, "MCTS response deadline");
    }
    check(state_ == before_search.play(expected_move) && history_.size() == 2, "incremental GUI search matches seeded agent");
    check(!search_ && mcts_job_.empty(), "completed search releases tree and timer");
    tk_.eval(".p.watch.human invoke");
    check(!mcts_mode_ && history_.size() == 2, "stop MCTS retains position");
    // Cancel a partially explored tree through real controls. No result from
    // the old position may arrive after undo, reset or a mode switch.
    delay_ms_ = 10000;
    tk_.eval(".p.watch.human invoke; .p.buttons.pass invoke");
    tk_.eval("after cancel " + tcl_quote(mcts_job_)); mcts_step();
    check(search_ && search_->statistics().simulations == 1, "incremental simulation yields before commit");
    tk_.eval(".p.buttons.undo invoke");
    check(!mcts_mode_ && !search_ && mcts_job_.empty() && history_.empty(), "undo cancels partial search");
    tk_.eval(".p.watch.human invoke; .p.buttons.pass invoke");
    tk_.eval("after cancel " + tcl_quote(mcts_job_)); mcts_step();
    tk_.eval(".p.buttons.new invoke; update");
    check(!search_ && !mcts_mode_ && history_.empty(), "reset cancels partial search");
    tk_.eval(".p.watch.human invoke; .p.buttons.pass invoke");
    auto stopped_position = state_;
    tk_.eval("after cancel " + tcl_quote(mcts_job_)); mcts_step();
    tk_.eval(".p.watch.human invoke; update");
    check(!mcts_mode_ && !search_ && mcts_job_.empty() && state_ == stopped_position, "stop cancels partial search without committing a move");
    tk_.eval(".p.watch.human invoke; .p.buttons.pass invoke; .p.watch.random invoke");
    check(!mcts_mode_ && !search_ && mcts_job_.empty() && !random_job_.empty(), "random mode cancels MCTS");
    tk_.eval(".p.watch.mcts invoke");
    check(random_job_.empty() && mcts_mode_ && mcts_watch_, "MCTS mode cancels random playback");
    tk_.eval(".p.buttons.demo invoke");
    check(!mcts_mode_ && mcts_job_.empty() && !demo_job_.empty(), "demo cancels MCTS");
    max_moves_ = 2; delay_ms_ = 1;
    tk_.eval(".p.watch.mcts invoke");
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (mcts_mode_) {
        tk_.eval("update");
        check(std::chrono::steady_clock::now() < deadline, "MCTS watch deadline");
    }
    MctsAgent expected_black(mcts_settings_, seed_); RandomAgent expected_white(seed_ + 1);
    auto expected_watch = run_game([&](const GameState& s) { return expected_black.choose_move(s); },
                                  [&](const GameState& s) { return expected_white.choose_move(s); }, 9, 7.5, max_moves_);
    check(state_ == expected_watch.final_state && history_.size() == expected_watch.moves.size(), "MCTS watch runner parity");
    check(random_truncated_ && !state_.is_terminal(), "MCTS watch truncation");
    check(tk_.eval("set status").find("No final score") != std::string::npos, "MCTS watch truncation message");
    check(tk_.eval("after info").empty() && !search_, "MCTS cancels pending callbacks");
    delay_ms_ = 10000; tk_.eval(".p.watch.mcts invoke; .p.watch.mcts invoke");
    check(!mcts_mode_ && mcts_job_.empty(), "stop watched MCTS game");
    tk_.eval(".p.watch.human invoke");
    state_ = GameState(Board(9, std::vector<int>(9, BLACK)), WHITE, 7.5, 1);
    schedule_mcts(); draw();
    for (int i = 0; i < mcts_settings_.simulations; ++i) {
        tk_.eval("after cancel " + tcl_quote(mcts_job_)); mcts_step();
    }
    check(state_.is_terminal() && !mcts_mode_ && !search_ && mcts_job_.empty(), "MCTS second pass ends play and cancels search");
    check(tk_.eval("set status").find("Black wins") != std::string::npos, "MCTS terminal score displayed");
    close();
    std::cout << "GUI checks passed: clicks, passes, seeded playback, truncation, stop, undo, reset, demo, incremental MCTS, cancellation and close.\n";
}

void GoWindow::dump_canvas(const std::filesystem::path& path) {
    // Migration check: compare geometry, colors, fonts, and the full grain with
    // the original canvas. Kept as a development diagnostic, not a UI control.
    tk_.eval("wm withdraw .");
    play(Point{0, 0}); play(Point{1, 1});
    tk_.eval("update idletasks");
    std::cout << "C++ window requested size: " << tk_.eval("winfo reqwidth .") << ' ' << tk_.eval("winfo reqheight .") << '\n';
    auto list = tk_.eval(R"TK(
set canvas_dump {}
foreach id [.p.board find all] {
    set kind [.p.board type $id]
    set item [list $kind [.p.board coords $id]]
    foreach key {fill width outline text font smooth tags} {
        if {![catch {.p.board itemcget $id -$key} value]} {lappend item $key $value}
    }
    lappend canvas_dump $item
}
set canvas_dump
)TK");
    std::ofstream file(path);
    if (!file || !(file << list)) throw std::runtime_error("Cannot write canvas diagnostic");
    close();
}
} // namespace betago
