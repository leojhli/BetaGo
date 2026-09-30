#include "betago/random.hpp"
// MT19937 is based on the Matsumoto/Nishimura reference algorithm. Its BSD
// redistribution terms are retained in third_party/MT19937-LICENSE.txt.
#include <bit>
#include <limits>

namespace betago {
Random::Random(std::int64_t seed) {
    // Signed integer seeds use their absolute magnitude, split little-endian.
    std::uint64_t magnitude = seed < 0 ? 0ULL - static_cast<std::uint64_t>(seed) : seed;
    std::uint32_t keys[2] = {static_cast<std::uint32_t>(magnitude),
                            static_cast<std::uint32_t>(magnitude >> 32)};
    std::size_t key_count = keys[1] ? 2 : 1;
    state_[0] = 19650218U;
    for (std::size_t i = 1; i < state_.size(); ++i)
        state_[i] = 1812433253U * (state_[i - 1] ^ (state_[i - 1] >> 30)) + static_cast<std::uint32_t>(i);
    std::size_t i = 1, j = 0;
    for (std::size_t k = 0; k < state_.size(); ++k) {
        state_[i] = (state_[i] ^ ((state_[i - 1] ^ (state_[i - 1] >> 30)) * 1664525U))
                    + keys[j] + static_cast<std::uint32_t>(j);
        if (++i == state_.size()) { state_[0] = state_.back(); i = 1; }
        if (++j == key_count) j = 0;
    }
    for (std::size_t k = 1; k < state_.size(); ++k) {
        state_[i] = (state_[i] ^ ((state_[i - 1] ^ (state_[i - 1] >> 30)) * 1566083941U))
                    - static_cast<std::uint32_t>(i);
        if (++i == state_.size()) { state_[0] = state_.back(); i = 1; }
    }
    state_[0] = 0x80000000U;
}

std::uint32_t Random::next_u32() {
    if (index_ == state_.size()) {
        for (std::size_t i = 0; i < state_.size(); ++i) {
            std::uint32_t joined = (state_[i] & 0x80000000U) | (state_[(i + 1) % 624] & 0x7fffffffU);
            state_[i] = state_[(i + 397) % 624] ^ (joined >> 1) ^ ((joined & 1) ? 0x9908b0dfU : 0);
        }
        index_ = 0;
    }
    std::uint32_t value = state_[index_++];
    value ^= value >> 11;
    value ^= (value << 7) & 0x9d2c5680U;
    value ^= (value << 15) & 0xefc60000U;
    return value ^ (value >> 18);
}

double Random::unit() {
    auto high = next_u32() >> 5;
    auto low = next_u32() >> 6;
    return (high * 67108864.0 + low) / 9007199254740992.0;
}

std::size_t Random::below(std::size_t count) {
    if (!count || count > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("Random choice count must fit a positive 32-bit integer");
    int bits = std::bit_width(static_cast<std::uint32_t>(count));
    std::uint32_t value;
    do { value = next_u32() >> (32 - bits); } while (value >= count);
    return value;
}

Move RandomAgent::choose_move(const GameState& state) {
    auto moves = state.legal_moves();
    if (moves.empty()) throw std::invalid_argument("Cannot choose a move after the game ends");
    return moves[rng_.below(moves.size())];
}
} // namespace betago
