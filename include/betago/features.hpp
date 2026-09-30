#pragma once

#include "state.hpp"
#include <vector>

namespace betago {
inline constexpr int FEATURE_CHANNELS = 7;
inline constexpr char FEATURE_SCHEMA[] =
    "go_relative_chw_v1_current_opponent_ko_one_pass_terminal_signed_komi_legal";

// Each channel is a row-major board plane. Actions use the same point order,
// with pass at board_size * board_size.
struct EncodedPosition {
    int board_size;
    std::vector<double> features;
    std::vector<bool> legal_actions;
    bool terminal;
    void validate() const;
};

EncodedPosition encode_position(const GameState& state);
int action_index(Move move, int board_size);
Move action_from_index(int index, int board_size);

struct InputBatch {
    int samples, channels, height, width;
    std::vector<double> values;
};
InputBatch pack_features(const std::vector<EncodedPosition>& positions);

struct TrainingExample {
    EncodedPosition input;
    std::vector<double> policy;
    double value;
    void validate() const;
};
} // namespace betago
