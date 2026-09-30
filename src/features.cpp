#include "betago/features.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace betago {
namespace {
void validate_size(int size) {
    if (size < 1 || size > 19)
        throw std::invalid_argument("Neural board size must be between 1 and 19");
}
bool binary(double value) { return value == 0.0 || value == 1.0; }
} // namespace

void EncodedPosition::validate() const {
    validate_size(board_size);
    const std::size_t area = static_cast<std::size_t>(board_size * board_size);
    if (features.size() != FEATURE_CHANNELS * area || legal_actions.size() != area + 1)
        throw std::invalid_argument("Encoded feature or action shape is invalid");
    auto at = [&](int channel, std::size_t point) { return features[channel * area + point]; };
    for (int channel = 0; channel < FEATURE_CHANNELS; ++channel)
        for (std::size_t point = 0; point < area; ++point) {
            const double value = at(channel, point);
            if (!std::isfinite(value) ||
                (channel == 5 ? value < -1.0 || value > 1.0 : !binary(value)))
                throw std::invalid_argument("Encoded feature values are invalid");
        }
    for (int channel : {3, 4, 5})
        for (std::size_t point = 1; point < area; ++point)
            if (at(channel, point) != at(channel, 0))
                throw std::invalid_argument("Position-wide feature planes must be constant");
    if ((at(4, 0) == 1.0) != terminal || (terminal && at(3, 0) != 0.0))
        throw std::invalid_argument("Encoded pass and terminal flags disagree");
    if (legal_actions[area] == terminal)
        throw std::invalid_argument("Pass legality disagrees with terminal status");
    for (std::size_t point = 0; point < area; ++point) {
        const bool occupied = at(0, point) != 0.0 || at(1, point) != 0.0;
        if (at(0, point) + at(1, point) > 1.0)
            throw std::invalid_argument("Encoded stone planes overlap");
        if ((at(6, point) == 1.0) != legal_actions[point] ||
            (legal_actions[point] && (occupied || terminal || at(2, point) == 1.0)))
            throw std::invalid_argument("Encoded placement legality disagrees with feature planes");
        if (at(2, point) == 1.0 && (occupied || terminal))
            throw std::invalid_argument("Encoded ko points must be empty and nonterminal");
    }
}

EncodedPosition encode_position(const GameState& state) {
    validate_size(state.size());
    const int area = state.size() * state.size();
    EncodedPosition result{state.size(), std::vector<double>(FEATURE_CHANNELS * area, 0.0),
                           std::vector<bool>(area + 1, false), state.is_terminal()};
    const int opponent = state.to_play() == BLACK ? WHITE : BLACK;
    const double signed_komi = (state.to_play() == WHITE ? state.komi() : -state.komi());
    const double scaled_komi = std::tanh(signed_komi / area);
    for (int row = 0; row < state.size(); ++row)
        for (int column = 0; column < state.size(); ++column) {
            const int point = row * state.size() + column;
            const Point move{row, column};
            const int color = state.at(move);
            result.features[point] = color == state.to_play() ? 1.0 : 0.0;
            result.features[area + point] = color == opponent ? 1.0 : 0.0;
            result.features[3 * area + point] = state.consecutive_passes() == 1 ? 1.0 : 0.0;
            result.features[4 * area + point] = state.is_terminal() ? 1.0 : 0.0;
            result.features[5 * area + point] = scaled_komi;
            if (!state.is_terminal() && color == EMPTY) {
                try {
                    state.play(move);
                    result.legal_actions[point] = true;
                    result.features[6 * area + point] = 1.0;
                } catch (const KoViolation&) {
                    result.features[2 * area + point] = 1.0;
                } catch (const IllegalMove&) {
                    // A placement can also be forbidden by suicide.
                }
            }
        }
    result.legal_actions[area] = !state.is_terminal();
    result.validate();
    return result;
}

int action_index(Move move, int size) {
    validate_size(size);
    if (!move) return size * size;
    if (move->row < 0 || move->column < 0 || move->row >= size || move->column >= size)
        throw std::invalid_argument("Action point is outside the board");
    return move->row * size + move->column;
}

Move action_from_index(int index, int size) {
    validate_size(size);
    if (index < 0 || index > size * size)
        throw std::invalid_argument("Action index is outside the board and pass range");
    if (index == size * size) return PASS;
    return Point{index / size, index % size};
}

InputBatch pack_features(const std::vector<EncodedPosition>& positions) {
    if (positions.empty() || positions.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::invalid_argument("Feature batch must have a valid nonzero sample count");
    positions.front().validate();
    const int size = positions.front().board_size;
    const std::size_t sample_values = static_cast<std::size_t>(FEATURE_CHANNELS * size * size);
    if (positions.size() > std::vector<double>().max_size() / sample_values)
        throw std::invalid_argument("Feature batch is too large");
    InputBatch result{static_cast<int>(positions.size()), FEATURE_CHANNELS, size, size, {}};
    result.values.reserve(positions.size() * sample_values);
    for (const auto& position : positions) {
        position.validate();
        if (position.board_size != size)
            throw std::invalid_argument("Feature batch must use one board size");
        result.values.insert(result.values.end(), position.features.begin(), position.features.end());
    }
    return result;
}

void TrainingExample::validate() const {
    input.validate();
    if (input.terminal)
        throw std::invalid_argument("Training policy examples must be nonterminal");
    if (policy.size() != input.legal_actions.size())
        throw std::invalid_argument("Training policy has the wrong action shape");
    double total = 0.0;
    for (std::size_t action = 0; action < policy.size(); ++action) {
        const double probability = policy[action];
        if (!std::isfinite(probability) || probability < 0.0 ||
            (!input.legal_actions[action] && probability != 0.0))
            throw std::invalid_argument("Training policy must assign finite nonnegative mass only to legal moves");
        total += probability;
    }
    if (!std::isfinite(total) || std::abs(total - 1.0) > 1e-8)
        throw std::invalid_argument("Training policy probabilities must sum to one");
    if (!std::isfinite(value) || value < -1.0 || value > 1.0)
        throw std::invalid_argument("Training value must be finite and between minus one and one");
}
} // namespace betago
