"""Visual Go board. Run python play.py, or python play.py --random --seed 10."""

import argparse
import tkinter as tk
import math
import random
from tkinter import ttk

from agents import RandomAgent
from game import BLACK, EMPTY, PASS, WHITE, GameState, IllegalMove


class GoWindow:
    MARGIN = 52
    SPACING = 58
    DEMO = ((0, 1), (1, 1), (1, 0), (8, 8), (2, 1), (8, 7), (1, 2))

    def __init__(self, root: tk.Tk, *, seed=0, max_moves=500, delay_ms=500):
        if max_moves < 1 or delay_ms < 1:
            raise ValueError("Move limit and delay must be positive")
        self.root = root
        self.state = GameState.new()
        self.history = []
        self.last_move = None
        self.demo_job = None
        self.demo_index = 0
        self.random_job = None
        self.random_agents = {}
        self.random_truncated = False
        self.seed = seed
        self.max_moves = max_moves
        self.delay_ms = delay_ms
        root.title("BetaGo - 9x9 Go")
        root.resizable(False, False)
        root.configure(background="#202321")
        root.protocol("WM_DELETE_WINDOW", self.close)

        style = ttk.Style(root)
        style.theme_use("clam")
        style.configure("TFrame", background="#202321")
        style.configure("TLabel", background="#202321", foreground="#ebe7dc")
        style.configure("TButton", background="#363d36", foreground="#f4efe4",
                        borderwidth=0, padding=(12, 9), font=("Segoe UI", 10))
        style.map("TButton", background=[("active", "#485448"), ("disabled", "#292d29")],
                  foreground=[("disabled", "#747a72")])
        panel = ttk.Frame(root, padding=20)
        panel.pack(fill="both", expand=True)
        ttk.Label(panel, text="BetaGo", font=("Segoe UI", 24, "bold")).pack(anchor="w")
        self.mode = tk.StringVar(value="9 x 9   /   Local play   /   Komi 7.5")
        ttk.Label(panel, textvariable=self.mode,
                  foreground="#aeb7aa").pack(anchor="w", pady=(0, 16))
        side = 2 * self.MARGIN + 8 * self.SPACING
        self.canvas = tk.Canvas(panel, width=side, height=side,
                                background="#dbb374", highlightthickness=0)
        self.canvas.pack()
        self.draw_board(side)
        self.canvas.bind("<Button-1>", self.click)
        self.status = tk.StringVar()
        ttk.Label(panel, textvariable=self.status, font=("Segoe UI", 12, "bold")).pack(anchor="w", pady=(12, 3))
        self.notice = tk.StringVar(value="Click an intersection to place a stone.")
        ttk.Label(panel, textvariable=self.notice, wraplength=540).pack(anchor="w")
        buttons = ttk.Frame(panel)
        buttons.pack(fill="x", pady=(12, 0))
        self.pass_button = ttk.Button(buttons, text="Pass", command=lambda: self.play(PASS))
        self.pass_button.pack(side="left")
        self.undo_button = ttk.Button(buttons, text="Undo", command=self.undo)
        self.undo_button.pack(side="left", padx=6)
        ttk.Button(buttons, text="New game", command=self.new_game).pack(side="left")
        self.demo_button = ttk.Button(buttons, text="Watch capture demo", command=self.toggle_demo)
        self.demo_button.pack(side="right")
        watch_controls = ttk.Frame(panel)
        watch_controls.pack(fill="x", pady=(8, 0))
        self.random_button = ttk.Button(watch_controls, text="Watch random game",
                                        command=self.toggle_random)
        self.random_button.pack(side="left")
        ttk.Label(panel, text="Finish captures before passing. Remaining stones count toward area.",
                  wraplength=540).pack(anchor="w", pady=(12, 0))
        self.draw()

    def draw_board(self, side):
        """Draw the fixed wood and grid once, underneath the moving stones."""
        canvas = self.canvas
        # Deterministic grain keeps the surface stable between moves.
        grain = random.Random(19)
        for x in range(side):
            tone = 4 * math.sin(x / 43) + 2 * math.sin(x / 13)
            color = self.rgb(210 + tone, 170 + tone, 105 + tone)
            canvas.create_line(x, 0, x, side, fill=color)
        for _ in range(240):
            x = grain.uniform(-12, side + 12)
            phase = grain.uniform(0, math.tau)
            amplitude = grain.uniform(1, 5)
            points = []
            for y in range(-20, side + 30, 20):
                points.extend((x + amplitude * math.sin(y / 95 + phase)
                               + 1.4 * math.sin(y / 37 + phase), y))
            canvas.create_line(*points, smooth=True,
                               fill=grain.choice(("#cba365", "#d8b174", "#d4ad6e", "#cfa768")),
                               width=grain.choice((1, 1, 2)))
        canvas.create_rectangle(2, 2, side - 2, side - 2, outline="#b48a4b", width=4)
        start = self.MARGIN
        end = start + 8 * self.SPACING
        for index in range(9):
            offset = start + index * self.SPACING
            canvas.create_line(start, offset, end, offset, fill="#3d301e")
            canvas.create_line(offset, start, offset, end, fill="#3d301e")
            for edge in (23, side - 23):
                canvas.create_text(offset, edge, text="ABCDEFGHJ"[index],
                                   fill="#494536", font=("Segoe UI", 13, "bold"))
                canvas.create_text(edge, offset, text=str(9 - index),
                                   fill="#494536", font=("Segoe UI", 13, "bold"))
        for row, column in ((2, 2), (2, 6), (4, 4), (6, 2), (6, 6)):
            x, y = start + column * self.SPACING, start + row * self.SPACING
            canvas.create_oval(x - 3, y - 3, x + 3, y + 3, fill="#30291e", outline="")

    @staticmethod
    def rgb(red, green, blue):
        return "#{:02x}{:02x}{:02x}".format(
            *(max(0, min(255, round(channel))) for channel in (red, green, blue)))

    def draw_stone(self, x, y, color):
        """Layer soft shadows and offset shading to suggest rounded stones."""
        canvas = self.canvas
        radius = 27
        for spread, shade in ((4, "#bd965c"), (2, "#a88450"), (0, "#876b43")):
            canvas.create_oval(x - radius - spread + 3, y - radius - spread + 4,
                               x + radius + spread + 3, y + radius + spread + 4,
                               fill=shade, outline="", tags="stones")
        base = (23, 24, 25) if color == BLACK else (177, 180, 185)
        highlight = (66, 66, 65) if color == BLACK else (247, 248, 247)
        for step in range(28):
            fraction = step / 27
            r = radius * (1 - 0.91 * fraction)
            offset = -6 * fraction
            shade = self.rgb(*(a + (b - a) * fraction ** 0.65
                               for a, b in zip(base, highlight)))
            canvas.create_oval(x + offset - r, y + offset - r,
                               x + offset + r, y + offset + r,
                               fill=shade, outline="", tags="stones")

    def draw(self):
        canvas = self.canvas
        canvas.delete("stones")
        start = self.MARGIN
        for row in range(9):
            for column in range(9):
                color = self.state.at((row, column))
                if color == EMPTY:
                    continue
                x, y = start + column * self.SPACING, start + row * self.SPACING
                self.draw_stone(x, y, color)
                if (row, column) == self.last_move:
                    canvas.create_oval(x - 10, y - 10, x + 10, y + 10,
                                       outline="#ffffff" if color == BLACK else "#333634",
                                       width=2, tags="stones")
        if self.state.is_terminal():
            score = self.state.score()
            winner = self.state.winner()
            result = "Draw" if winner is None else ("Black wins" if winner == BLACK else "White wins")
            self.status.set(f"{result}  |  Black {score.black:g} - White {score.white:g}")
        elif self.random_truncated:
            self.status.set(f"Truncated at {len(self.history)} moves  |  No final score")
        else:
            player = "Black" if self.state.to_play == BLACK else "White"
            self.status.set(f"{player} to play  |  Move {len(self.history) + 1}  |  Passes {self.state.consecutive_passes}/2")
        watching = self.demo_job is not None or self.random_job is not None
        self.pass_button.configure(state="disabled" if self.state.is_terminal() or watching else "normal")
        self.undo_button.configure(state="normal" if self.history else "disabled")

    def click(self, event):
        if self.demo_job is not None or self.random_job is not None:
            self.notice.set("Stop playback to play your own moves.")
            return
        column = round((event.x - self.MARGIN) / self.SPACING)
        row = round((event.y - self.MARGIN) / self.SPACING)
        if not (0 <= row < 9 and 0 <= column < 9):
            return
        x, y = self.MARGIN + column * self.SPACING, self.MARGIN + row * self.SPACING
        if abs(event.x - x) <= 23 and abs(event.y - y) <= 23:
            self.play((row, column))

    def play(self, move):
        try:
            successor = self.state.play(move)
        except IllegalMove as error:
            self.notice.set(str(error))
            return
        opponent = WHITE if self.state.to_play == BLACK else BLACK
        captured = sum(row.count(opponent) for row in self.state.board) - sum(
            row.count(opponent) for row in successor.board)
        self.history.append((self.state, self.last_move))
        self.state = successor
        self.random_truncated = False
        self.last_move = move
        if self.state.is_terminal():
            self.notice.set("Game over after two passes. Undo to resume, or start a new game.")
        elif move is PASS:
            self.notice.set("Passed. Another pass will end the game.")
        elif captured:
            self.notice.set(f"Captured {captured} stone(s). The empty intersections can be played again.")
        elif self.random_job is not None:
            self.notice.set("Random agents are choosing legal moves, including pass.")
        else:
            self.notice.set("Click an intersection to place a stone.")
        self.draw()

    def stop_demo(self):
        if self.demo_job is not None:
            self.root.after_cancel(self.demo_job)
            self.demo_job = None
        self.demo_button.configure(text="Watch capture demo")

    def new_game(self):
        self.stop_demo()
        self.stop_random()
        self.state = GameState.new()
        self.history.clear()
        self.last_move = None
        self.random_truncated = False
        self.notice.set("Click an intersection to place a stone.")
        self.draw()

    def undo(self):
        self.stop_demo()
        self.stop_random()
        self.random_truncated = False
        if self.history:
            self.state, self.last_move = self.history.pop()
            self.notice.set("Move undone.")
        self.draw()

    def toggle_demo(self):
        if self.demo_job is not None:
            self.stop_demo()
            self.notice.set("Demo stopped. You can continue playing this position.")
            self.draw()
            return
        self.new_game()
        self.demo_index = 0
        self.demo_button.configure(text="Stop demo")
        self.notice.set("Watch Black surround the white stone near the upper-left corner.")
        self.demo_job = self.root.after(900, self.demo_step)
        self.draw()

    def demo_step(self):
        self.play(self.DEMO[self.demo_index])
        self.demo_index += 1
        if self.demo_index == len(self.DEMO):
            self.demo_job = None
            self.demo_button.configure(text="Watch capture demo")
            self.notice.set("Capture! White's surrounded stone was removed. Continue playing or start a new game.")
        else:
            self.demo_job = self.root.after(900, self.demo_step)
        self.draw()

    def stop_random(self):
        if self.random_job is not None:
            self.root.after_cancel(self.random_job)
            self.random_job = None
        self.random_button.configure(text="Watch random game")
        self.mode.set("9 x 9   /   Local play   /   Komi 7.5")

    def toggle_random(self):
        if self.random_job is not None:
            self.stop_random()
            self.notice.set("Random game stopped. You can continue playing this position.")
            self.draw()
            return
        self.new_game()
        self.random_agents = {BLACK: RandomAgent(self.seed), WHITE: RandomAgent(self.seed + 1)}
        self.mode.set(f"9 x 9   /   Random vs random   /   Seed {self.seed}   /   Komi 7.5")
        self.random_button.configure(text="Stop random game")
        self.notice.set("Watch the random agents play. Stop playback to take over.")
        self.random_job = self.root.after(self.delay_ms, self.random_step)
        self.draw()

    def random_step(self):
        """Advance one legal action, then let Tkinter handle input and drawing."""
        move = self.random_agents[self.state.to_play].choose_move(self.state)
        self.play(move)
        if self.state.is_terminal():
            self.stop_random()
            self.mode.set("9 x 9   /   Random game finished   /   Komi 7.5")
        elif len(self.history) >= self.max_moves:
            self.stop_random()
            self.random_truncated = True
            self.mode.set("9 x 9   /   Random game truncated   /   Komi 7.5")
            self.notice.set("Move limit reached. No final score. Continue playing or start a new game.")
        else:
            self.random_job = self.root.after(self.delay_ms, self.random_step)
        self.draw()

    def close(self):
        self.stop_demo()
        self.stop_random()
        self.root.destroy()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--random", action="store_true", help="Start watching random agents immediately")
    parser.add_argument("--seed", type=int, default=0, help="Black's seed; White uses seed + 1")
    parser.add_argument("--max-moves", type=int, default=500)
    parser.add_argument("--delay-ms", type=int, default=500, help="Delay between random moves in milliseconds")
    args = parser.parse_args(argv)
    if args.max_moves < 1 or args.delay_ms < 1:
        parser.error("Move limit and delay must be positive")
    root = tk.Tk()
    window = GoWindow(root, seed=args.seed, max_moves=args.max_moves, delay_ms=args.delay_ms)
    if args.random:
        window.toggle_random()
    root.mainloop()


if __name__ == "__main__":
    main()
