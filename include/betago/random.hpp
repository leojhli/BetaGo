#pragma once
#include "state.hpp"
#include <array>
#include <cstdint>

namespace betago {
// MT19937 with the original integer seeding and sampling conventions. Keeping
// this explicit preserves existing seed sequences and the exact wood grain.
class Random {
public:
    explicit Random(std::int64_t seed = 0);
    std::uint32_t next_u32();
    double unit();
    double uniform(double low, double high) { return low + (high - low) * unit(); }
    std::size_t below(std::size_t count);
private:
    std::array<std::uint32_t, 624> state_{};
    std::size_t index_ = 624;
};

class RandomAgent {
public:
    explicit RandomAgent(std::int64_t seed = 0) : rng_(seed) {}
    Move choose_move(const GameState& state);
private:
    Random rng_;
};
} // namespace betago
