#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace betago {
enum class ProfileWork : std::size_t {
    StatePlay, StateCopy, Group, LegalMoves, Encode, Forward, Objective,
    Normalize, Expand, Simulation, SelfPlay, ReplayValidation, CandidateTraining, Count
};
inline constexpr std::array<std::string_view, static_cast<std::size_t>(ProfileWork::Count)> PROFILE_NAMES{
    "state_play", "successor_board_copy", "group", "legal_moves", "encode_position",
    "network_forward", "network_objective", "normalize_priors", "expand",
    "simulation", "self_play", "replay_validation", "candidate_training"
};
struct ProfileMetric {
    std::uint64_t calls = 0;
    double inclusive_seconds = 0;
    double exclusive_seconds = 0;
};
class ProfileScope;
class ProfileSession;
inline thread_local ProfileSession* active_profile = nullptr;

// Profiling is opt-in and local to the calling thread. No clocks are read
// during ordinary searches. Sessions cannot overlap on the same thread.
class ProfileSession {
public:
    ProfileSession() : start_(std::chrono::steady_clock::now()) {
        if (active_profile) throw std::logic_error("profiling sessions cannot overlap");
        active_profile = this;
    }
    ~ProfileSession() { active_profile = nullptr; }
    ProfileSession(const ProfileSession&) = delete;
    ProfileSession& operator=(const ProfileSession&) = delete;
    const auto& metrics() const { return metrics_; }
    double elapsed_seconds() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    }
private:
    friend class ProfileScope;
    std::chrono::steady_clock::time_point start_;
    std::array<ProfileMetric, static_cast<std::size_t>(ProfileWork::Count)> metrics_{};
    ProfileScope* current_ = nullptr;
};

class ProfileScope {
public:
    explicit ProfileScope(ProfileWork work) : session_(active_profile), work_(work) {
        if (session_) {
            parent_ = session_->current_;
            session_->current_ = this;
            start_ = std::chrono::steady_clock::now();
        }
    }
    ~ProfileScope() {
        if (!session_) return;
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
        auto& metric = session_->metrics_[static_cast<std::size_t>(work_)];
        ++metric.calls;
        metric.inclusive_seconds += elapsed;
        metric.exclusive_seconds += elapsed - children_seconds_;
        if (parent_) parent_->children_seconds_ += elapsed;
        session_->current_ = parent_;
    }
    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;
private:
    ProfileSession* session_;
    ProfileWork work_;
    ProfileScope* parent_ = nullptr;
    std::chrono::steady_clock::time_point start_{};
    double children_seconds_ = 0;
};
} // namespace betago
