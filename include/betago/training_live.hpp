#pragma once

#include "runner.hpp"
#include <cstdint>

namespace betago {
// A viewer reads one complete position at a time. This is an observation feed,
// not a checkpoint or a resumable training record.
struct TrainingLiveSnapshot {
    GameState state;
    Move last_move;
    int move_number;
    Json progress;
    std::string session_id;
    std::uint64_t sequence;
    std::int64_t updated_at_ms;
};

class TrainingLiveWriter {
public:
    explicit TrainingLiveWriter(std::filesystem::path path);
    void publish(const GameState& state, Move last_move, int move_number, const Json& progress);

private:
    std::filesystem::path path_;
    std::string session_id_;
    std::uint64_t sequence_ = 0;
};

TrainingLiveSnapshot parse_training_live(const Json& data);
TrainingLiveSnapshot read_training_live(const std::filesystem::path& path);
} // namespace betago
