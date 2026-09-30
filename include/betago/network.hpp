#pragma once

#include "features.hpp"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace betago {
struct NetworkSettings {
    int board_size = 9;
    int channels = 8;
    int value_hidden = 16;
    void validate() const;
    bool operator==(const NetworkSettings&) const = default;
};

struct OptimizerSettings {
    double learning_rate = 0.02;
    double momentum = 0.9;
    double l2 = 0.0;
    void validate() const;
    bool operator==(const OptimizerSettings&) const = default;
};

struct Prediction {
    std::vector<double> logits;
    std::vector<double> policy;
    double value = 0.0;
};

struct LossMetrics {
    double policy = 0.0;
    double value = 0.0;
    double regularization = 0.0;
    double total = 0.0;
};

struct GradientResult {
    LossMetrics loss;
    std::vector<double> gradient;
};

struct ParameterBlock {
    std::string name;
    std::size_t offset;
    std::vector<std::size_t> shape;
    std::size_t count;
};

// A small, explicit CPU network: same-padded 3x3 convolution + ReLU, then
// separate policy-logit and hidden-ReLU/tanh value heads. There is no dropout
// or batch normalization, so train/eval modes have identical predictions.
class PolicyValueNetwork {
public:
    explicit PolicyValueNetwork(NetworkSettings settings = {}, std::int64_t seed = 0);
    const NetworkSettings& settings() const { return settings_; }
    std::int64_t initialization_seed() const { return initialization_seed_; }
    const std::vector<double>& parameters() const { return parameters_; }
    const std::vector<ParameterBlock>& parameter_blocks() const { return blocks_; }
    std::size_t parameter_count() const { return parameters_.size(); }
    // Replacing parameters starts a fresh optimizer history.
    void set_parameters(std::vector<double> parameters);

    Prediction predict(const EncodedPosition& input) const;
    std::vector<Prediction> predict_batch(const std::vector<EncodedPosition>& inputs) const;
    GradientResult loss_and_gradient(const std::vector<TrainingExample>& batch, double l2 = 0.0) const;
    LossMetrics evaluate_batch(const std::vector<TrainingExample>& batch, double l2 = 0.0) const;

    void train(bool enabled = true) { training_ = enabled; }
    bool training() const { return training_; }
    std::uint64_t training_steps() const { return training_steps_; }
    const std::vector<double>& velocity() const { return velocity_; }
    const std::optional<OptimizerSettings>& last_optimizer() const { return last_optimizer_; }
    // Returns the loss before the update. Parameters and optimizer history
    // change together only after the entire update has passed finite checks.
    LossMetrics train_batch(const std::vector<TrainingExample>& batch,
                            OptimizerSettings settings = {});

    void save(const std::filesystem::path& path) const;
    static PolicyValueNetwork load(const std::filesystem::path& path);

private:
    struct ForwardCache {
        std::vector<double> trunk_pre, trunk, value_pre, value_hidden;
        Prediction prediction;
    };
    NetworkSettings settings_;
    std::int64_t initialization_seed_;
    std::vector<ParameterBlock> blocks_;
    std::vector<double> parameters_, velocity_;
    bool training_ = false;
    std::uint64_t training_steps_ = 0;
    std::optional<OptimizerSettings> last_optimizer_;

    void make_layout();
    ForwardCache forward(const EncodedPosition& input) const;
    GradientResult objective(const std::vector<TrainingExample>& batch,
                             double l2, bool gradients) const;
};
} // namespace betago
