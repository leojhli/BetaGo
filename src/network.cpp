#include "betago/network.hpp"
#include "betago/random.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <limits>
#include <nlohmann/json.hpp>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace betago {
namespace {
using Json = nlohmann::json;
constexpr const char* ARCHITECTURE = "conv_relu_policy_value_v1";
enum Block : std::size_t {
    CONV_WEIGHT, CONV_BIAS, POLICY_WEIGHT, POLICY_BIAS,
    VALUE_WEIGHT, VALUE_BIAS, OUTPUT_WEIGHT, OUTPUT_BIAS
};

void require_finite(double value, const char* context) {
    if (!std::isfinite(value)) throw std::runtime_error(std::string(context) + " is not finite");
}
void finite_vector(const std::vector<double>& values, const char* context) {
    for (double value : values) {
        if (!std::isfinite(value)) throw std::invalid_argument(std::string(context) + " must be finite");
    }
}

// JSON integer fields must actually be integers; 3.0 and booleans are not
// accepted as dimensions. Bounds are checked before allocating parameters.
std::uint64_t unsigned_integer(const Json& value, const char* name) {
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    if (value.is_number_integer()) {
        auto result = value.get<std::int64_t>();
        if (result >= 0) return static_cast<std::uint64_t>(result);
    }
    throw std::invalid_argument(std::string(name) + " must be a nonnegative integer");
}
int dimension(const Json& value, const char* name, int maximum) {
    auto result = unsigned_integer(value, name);
    if (result < 1 || result > static_cast<std::uint64_t>(maximum))
        throw std::invalid_argument(std::string(name) + " is out of range");
    return static_cast<int>(result);
}
double number(const Json& value, const char* name) {
    if (!value.is_number()) throw std::invalid_argument(std::string(name) + " must be a number");
    double result = value.get<double>();
    if (!std::isfinite(result)) throw std::invalid_argument(std::string(name) + " must be finite");
    return result;
}
std::vector<double> checkpoint_vector(const Json& value, std::size_t expected, const char* name) {
    if (!value.is_array() || value.size() != expected)
        throw std::invalid_argument(std::string(name) + " has the wrong parameter count");
    std::vector<double> result;
    result.reserve(expected);
    for (const auto& item : value) result.push_back(number(item, name));
    return result;
}
Json optimizer_json(const OptimizerSettings& settings) {
    return {{"learning_rate", settings.learning_rate}, {"momentum", settings.momentum}, {"l2", settings.l2}};
}
OptimizerSettings read_optimizer(const Json& document) {
    OptimizerSettings result{number(document.at("learning_rate"), "learning_rate"),
                             number(document.at("momentum"), "momentum"),
                             number(document.at("l2"), "l2")};
    result.validate();
    return result;
}
} // namespace

void NetworkSettings::validate() const {
    if (board_size < 1 || board_size > 19) throw std::invalid_argument("network board_size must be between 1 and 19");
    if (channels < 1 || channels > 32) throw std::invalid_argument("network channels must be between 1 and 32");
    if (value_hidden < 1 || value_hidden > 128) throw std::invalid_argument("network value_hidden must be between 1 and 128");
}

void OptimizerSettings::validate() const {
    if (!std::isfinite(learning_rate) || learning_rate <= 0)
        throw std::invalid_argument("learning_rate must be finite and positive");
    if (!std::isfinite(momentum) || momentum < 0 || momentum >= 1)
        throw std::invalid_argument("momentum must be finite and in [0, 1)");
    if (!std::isfinite(l2) || l2 < 0) throw std::invalid_argument("l2 must be finite and nonnegative");
}

void PolicyValueNetwork::make_layout() {
    const std::size_t area = static_cast<std::size_t>(settings_.board_size * settings_.board_size);
    const std::size_t channels = static_cast<std::size_t>(settings_.channels);
    const std::size_t hidden = static_cast<std::size_t>(settings_.value_hidden);
    const std::size_t flattened = channels * area;
    std::size_t offset = 0;
    auto add = [&](std::string name, std::vector<std::size_t> shape) {
        std::size_t count = std::accumulate(shape.begin(), shape.end(), std::size_t{1}, std::multiplies<>());
        blocks_.push_back({std::move(name), offset, std::move(shape), count});
        offset += count;
    };
    blocks_.clear();
    add("conv_weight", {channels, FEATURE_CHANNELS, 3, 3});
    add("conv_bias", {channels});
    add("policy_weight", {area + 1, flattened});
    add("policy_bias", {area + 1});
    add("value_hidden_weight", {hidden, flattened});
    add("value_hidden_bias", {hidden});
    add("value_output_weight", {1, hidden});
    add("value_output_bias", {1});
    parameters_.assign(offset, 0.0);
    velocity_.assign(offset, 0.0);
}

PolicyValueNetwork::PolicyValueNetwork(NetworkSettings settings, std::int64_t seed)
    : settings_(settings), initialization_seed_(seed) {
    settings_.validate();
    make_layout();
    Random random(seed);
    const int area = settings_.board_size * settings_.board_size;
    auto initialize = [&](Block block, int fan_in) {
        // He uniform keeps typical ReLU activations near the input scale.
        const double limit = std::sqrt(6.0 / fan_in);
        const auto& layout = blocks_[block];
        for (std::size_t i = 0; i < layout.count; ++i)
            parameters_[layout.offset + i] = random.uniform(-limit, limit);
    };
    initialize(CONV_WEIGHT, FEATURE_CHANNELS * 9);
    initialize(POLICY_WEIGHT, settings_.channels * area);
    initialize(VALUE_WEIGHT, settings_.channels * area);
    initialize(OUTPUT_WEIGHT, settings_.value_hidden);
    // Small positive ReLU biases reduce inactive units in tiny training sets.
    for (Block block : {CONV_BIAS, VALUE_BIAS}) {
        const auto& layout = blocks_[block];
        std::fill_n(parameters_.begin() + static_cast<std::ptrdiff_t>(layout.offset), layout.count, 0.01);
    }
}

void PolicyValueNetwork::set_parameters(std::vector<double> parameters) {
    if (parameters.size() != parameters_.size()) throw std::invalid_argument("wrong network parameter count");
    finite_vector(parameters, "network parameters");
    parameters_ = std::move(parameters);
    std::fill(velocity_.begin(), velocity_.end(), 0.0);
    training_steps_ = 0;
    last_optimizer_.reset();
}

PolicyValueNetwork::ForwardCache PolicyValueNetwork::forward(const EncodedPosition& input) const {
    input.validate();
    if (input.board_size != settings_.board_size) throw std::invalid_argument("input board size does not match network");
    const int size = settings_.board_size;
    const int area = size * size;
    const int flattened = settings_.channels * area;
    const int actions = area + 1;
    ForwardCache cache;
    cache.trunk_pre.resize(flattened);
    cache.trunk.resize(flattened);
    cache.value_pre.resize(settings_.value_hidden);
    cache.value_hidden.resize(settings_.value_hidden);
    cache.prediction.logits.resize(actions);
    cache.prediction.policy.assign(actions, 0.0);

    // Correlation convention: weights[output][input][kernel row][kernel col].
    // Positions outside the board contribute zero (one-cell same padding).
    for (int output = 0; output < settings_.channels; ++output) {
        for (int row = 0; row < size; ++row) {
            for (int column = 0; column < size; ++column) {
                double sum = parameters_[blocks_[CONV_BIAS].offset + output];
                for (int channel = 0; channel < FEATURE_CHANNELS; ++channel) {
                    for (int kr = 0; kr < 3; ++kr) {
                        const int source_row = row + kr - 1;
                        if (source_row < 0 || source_row >= size) continue;
                        for (int kc = 0; kc < 3; ++kc) {
                            const int source_column = column + kc - 1;
                            if (source_column < 0 || source_column >= size) continue;
                            const std::size_t weight = blocks_[CONV_WEIGHT].offset +
                                ((output * FEATURE_CHANNELS + channel) * 3 + kr) * 3 + kc;
                            sum += parameters_[weight] * input.features[channel * area + source_row * size + source_column];
                        }
                    }
                }
                require_finite(sum, "convolution activation");
                const int index = output * area + row * size + column;
                cache.trunk_pre[index] = sum;
                cache.trunk[index] = std::max(0.0, sum);
            }
        }
    }

    for (int action = 0; action < actions; ++action) {
        double sum = parameters_[blocks_[POLICY_BIAS].offset + action];
        const std::size_t weights = blocks_[POLICY_WEIGHT].offset + action * flattened;
        for (int i = 0; i < flattened; ++i) sum += parameters_[weights + i] * cache.trunk[i];
        require_finite(sum, "policy logit");
        cache.prediction.logits[action] = sum;
    }
    if (!input.terminal) {
        double maximum = -std::numeric_limits<double>::infinity();
        for (int action = 0; action < actions; ++action)
            if (input.legal_actions[action]) maximum = std::max(maximum, cache.prediction.logits[action]);
        double denominator = 0.0;
        for (int action = 0; action < actions; ++action) {
            if (input.legal_actions[action]) {
                cache.prediction.policy[action] = std::exp(cache.prediction.logits[action] - maximum);
                denominator += cache.prediction.policy[action];
            }
        }
        require_finite(denominator, "policy normalization");
        if (denominator <= 0) throw std::runtime_error("policy has no legal probability mass");
        for (double& probability : cache.prediction.policy) probability /= denominator;
    }

    for (int hidden = 0; hidden < settings_.value_hidden; ++hidden) {
        double sum = parameters_[blocks_[VALUE_BIAS].offset + hidden];
        const std::size_t weights = blocks_[VALUE_WEIGHT].offset + hidden * flattened;
        for (int i = 0; i < flattened; ++i) sum += parameters_[weights + i] * cache.trunk[i];
        require_finite(sum, "value hidden activation");
        cache.value_pre[hidden] = sum;
        cache.value_hidden[hidden] = std::max(0.0, sum);
    }
    double value = parameters_[blocks_[OUTPUT_BIAS].offset];
    for (int hidden = 0; hidden < settings_.value_hidden; ++hidden)
        value += parameters_[blocks_[OUTPUT_WEIGHT].offset + hidden] * cache.value_hidden[hidden];
    require_finite(value, "value output activation");
    cache.prediction.value = std::tanh(value);
    return cache;
}

Prediction PolicyValueNetwork::predict(const EncodedPosition& input) const {
    return forward(input).prediction;
}

std::vector<Prediction> PolicyValueNetwork::predict_batch(const std::vector<EncodedPosition>& inputs) const {
    if (inputs.empty()) throw std::invalid_argument("prediction batch must not be empty");
    std::vector<Prediction> predictions;
    predictions.reserve(inputs.size());
    for (const auto& input : inputs) predictions.push_back(predict(input));
    return predictions;
}

GradientResult PolicyValueNetwork::objective(const std::vector<TrainingExample>& batch,
                                           double l2, bool gradients) const {
    if (batch.empty()) throw std::invalid_argument("training batch must not be empty");
    if (!std::isfinite(l2) || l2 < 0) throw std::invalid_argument("l2 must be finite and nonnegative");
    for (const auto& example : batch) {
        example.validate();
        if (example.input.board_size != settings_.board_size)
            throw std::invalid_argument("training board size does not match network");
    }
    const int size = settings_.board_size;
    const int area = size * size;
    const int flattened = settings_.channels * area;
    const int actions = area + 1;
    const double scale = 1.0 / static_cast<double>(batch.size());
    GradientResult result;
    if (gradients) result.gradient.assign(parameters_.size(), 0.0);

    for (const auto& example : batch) {
        const auto cache = forward(example.input);
        // Log-softmax is computed directly rather than taking log(policy),
        // which remains stable when a legal probability underflows to zero.
        double maximum = -std::numeric_limits<double>::infinity();
        for (int action = 0; action < actions; ++action)
            if (example.input.legal_actions[action]) maximum = std::max(maximum, cache.prediction.logits[action]);
        double exponential_sum = 0.0;
        for (int action = 0; action < actions; ++action)
            if (example.input.legal_actions[action]) exponential_sum += std::exp(cache.prediction.logits[action] - maximum);
        const double log_sum = std::log(exponential_sum);
        const double target_mass = std::accumulate(example.policy.begin(), example.policy.end(), 0.0);
        for (int action = 0; action < actions; ++action) {
            if (example.policy[action] > 0)
                result.loss.policy -= scale * example.policy[action] *
                    (cache.prediction.logits[action] - maximum - log_sum);
        }
        const double error = cache.prediction.value - example.value;
        result.loss.value += scale * error * error;
        if (!gradients) continue;

        std::vector<double> trunk_gradient(flattened, 0.0);
        // Exact CE derivative for accepted target sums within normalization
        // tolerance: (masked softmax * target mass - target) / batch size.
        for (int action = 0; action < actions; ++action) {
            if (!example.input.legal_actions[action]) continue;
            const double delta = scale * (cache.prediction.policy[action] * target_mass - example.policy[action]);
            result.gradient[blocks_[POLICY_BIAS].offset + action] += delta;
            const std::size_t weights = blocks_[POLICY_WEIGHT].offset + action * flattened;
            for (int i = 0; i < flattened; ++i) {
                result.gradient[weights + i] += delta * cache.trunk[i];
                trunk_gradient[i] += delta * parameters_[weights + i];
            }
        }

        // Mean-squared value loss passes through tanh and the hidden ReLU.
        const double value_delta = scale * 2.0 * error *
            (1.0 - cache.prediction.value * cache.prediction.value);
        result.gradient[blocks_[OUTPUT_BIAS].offset] += value_delta;
        for (int hidden = 0; hidden < settings_.value_hidden; ++hidden) {
            const std::size_t output_weight = blocks_[OUTPUT_WEIGHT].offset + hidden;
            result.gradient[output_weight] += value_delta * cache.value_hidden[hidden];
            if (cache.value_pre[hidden] <= 0) continue;
            const double delta = value_delta * parameters_[output_weight];
            result.gradient[blocks_[VALUE_BIAS].offset + hidden] += delta;
            const std::size_t weights = blocks_[VALUE_WEIGHT].offset + hidden * flattened;
            for (int i = 0; i < flattened; ++i) {
                result.gradient[weights + i] += delta * cache.trunk[i];
                trunk_gradient[i] += delta * parameters_[weights + i];
            }
        }

        // Both heads contribute to the same trunk gradient before the
        // convolution derivative. The ReLU derivative at zero is defined as 0.
        for (int output = 0; output < settings_.channels; ++output) {
            for (int row = 0; row < size; ++row) {
                for (int column = 0; column < size; ++column) {
                    const int index = output * area + row * size + column;
                    if (cache.trunk_pre[index] <= 0) continue;
                    const double delta = trunk_gradient[index];
                    result.gradient[blocks_[CONV_BIAS].offset + output] += delta;
                    for (int channel = 0; channel < FEATURE_CHANNELS; ++channel) {
                        for (int kr = 0; kr < 3; ++kr) {
                            const int source_row = row + kr - 1;
                            if (source_row < 0 || source_row >= size) continue;
                            for (int kc = 0; kc < 3; ++kc) {
                                const int source_column = column + kc - 1;
                                if (source_column < 0 || source_column >= size) continue;
                                const std::size_t weight = blocks_[CONV_WEIGHT].offset +
                                    ((output * FEATURE_CHANNELS + channel) * 3 + kr) * 3 + kc;
                                result.gradient[weight] += delta *
                                    example.input.features[channel * area + source_row * size + source_column];
                            }
                        }
                    }
                }
            }
        }
    }

    // This baseline explicitly regularizes biases as well as weights. L2 is
    // separate from the batch mean, so changing batch size does not rescale it.
    if (l2 > 0) {
        for (std::size_t i = 0; i < parameters_.size(); ++i) {
            result.loss.regularization += (0.5 * l2 * parameters_[i]) * parameters_[i];
            if (gradients) result.gradient[i] += l2 * parameters_[i];
        }
    }
    result.loss.total = result.loss.policy + result.loss.value + result.loss.regularization;
    require_finite(result.loss.policy, "policy loss");
    require_finite(result.loss.value, "value loss");
    require_finite(result.loss.regularization, "regularization loss");
    require_finite(result.loss.total, "total loss");
    if (gradients) finite_vector(result.gradient, "network gradient");
    return result;
}

GradientResult PolicyValueNetwork::loss_and_gradient(const std::vector<TrainingExample>& batch, double l2) const {
    return objective(batch, l2, true);
}

LossMetrics PolicyValueNetwork::evaluate_batch(const std::vector<TrainingExample>& batch, double l2) const {
    return objective(batch, l2, false).loss;
}

LossMetrics PolicyValueNetwork::train_batch(const std::vector<TrainingExample>& batch, OptimizerSettings settings) {
    if (!training_) throw std::logic_error("train_batch requires train mode; call train(true)");
    settings.validate();
    if (training_steps_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("network training step counter is full");
    auto result = loss_and_gradient(batch, settings.l2);
    std::vector<double> next_parameters(parameters_.size()), next_velocity(parameters_.size());
    for (std::size_t i = 0; i < parameters_.size(); ++i) {
        next_velocity[i] = settings.momentum * velocity_[i] + result.gradient[i];
        next_parameters[i] = parameters_[i] - settings.learning_rate * next_velocity[i];
    }
    finite_vector(next_velocity, "optimizer velocity");
    finite_vector(next_parameters, "updated parameters");
    parameters_.swap(next_parameters);
    velocity_.swap(next_velocity);
    ++training_steps_;
    last_optimizer_ = settings;
    return result.loss;
}

void PolicyValueNetwork::save(const std::filesystem::path& path) const {
    if (path.empty()) throw std::invalid_argument("checkpoint path must not be empty");
    Json layouts = Json::array();
    for (const auto& block : blocks_)
        layouts.push_back({{"name", block.name}, {"offset", block.offset}, {"shape", block.shape}, {"count", block.count}});
    Json document = {
        {"schema_version", 1}, {"architecture", ARCHITECTURE}, {"feature_schema", FEATURE_SCHEMA},
        {"settings", {{"board_size", settings_.board_size}, {"channels", settings_.channels}, {"value_hidden", settings_.value_hidden}}},
        {"initialization_seed", initialization_seed_}, {"mode", training_ ? "train" : "eval"},
        {"training_steps", training_steps_}, {"parameter_blocks", std::move(layouts)},
        {"parameters", parameters_}, {"velocity", velocity_},
        {"last_optimizer", last_optimizer_ ? optimizer_json(*last_optimizer_) : Json(nullptr)}
    };
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot open checkpoint for writing: " + path.string());
    output << document.dump(2) << '\n';
    output.close();
    if (!output) throw std::runtime_error("cannot write checkpoint: " + path.string());
}

PolicyValueNetwork PolicyValueNetwork::load(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open checkpoint: " + path.string());
    try {
        const Json document = Json::parse(input);
        if (!document.is_object() || unsigned_integer(document.at("schema_version"), "schema_version") != 1)
            throw std::invalid_argument("unsupported checkpoint schema");
        if (document.at("architecture") != ARCHITECTURE) throw std::invalid_argument("unsupported checkpoint architecture");
        if (document.at("feature_schema") != FEATURE_SCHEMA) throw std::invalid_argument("checkpoint feature schema mismatch");
        const auto& settings_json = document.at("settings");
        NetworkSettings settings{dimension(settings_json.at("board_size"), "board_size", 19),
                                 dimension(settings_json.at("channels"), "channels", 32),
                                 dimension(settings_json.at("value_hidden"), "value_hidden", 128)};
        const auto& seed_json = document.at("initialization_seed");
        if (!seed_json.is_number_integer() || (seed_json.is_number_unsigned() &&
            seed_json.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
            throw std::invalid_argument("initialization_seed must be a signed 64-bit integer");
        PolicyValueNetwork result(settings, seed_json.get<std::int64_t>());
        const auto& mode = document.at("mode");
        if (mode == "train") result.training_ = true;
        else if (mode == "eval") result.training_ = false;
        else throw std::invalid_argument("checkpoint mode must be train or eval");

        const auto& layouts = document.at("parameter_blocks");
        if (!layouts.is_array() || layouts.size() != result.blocks_.size())
            throw std::invalid_argument("checkpoint parameter layout mismatch");
        for (std::size_t i = 0; i < result.blocks_.size(); ++i) {
            const auto& saved = layouts[i];
            const auto& expected = result.blocks_[i];
            if (saved.at("name") != expected.name ||
                unsigned_integer(saved.at("offset"), "offset") != expected.offset ||
                unsigned_integer(saved.at("count"), "count") != expected.count)
                throw std::invalid_argument("checkpoint parameter layout mismatch");
            const auto& shape = saved.at("shape");
            if (!shape.is_array() || shape.size() != expected.shape.size())
                throw std::invalid_argument("checkpoint parameter shape mismatch");
            for (std::size_t axis = 0; axis < expected.shape.size(); ++axis)
                if (unsigned_integer(shape[axis], "shape") != expected.shape[axis])
                    throw std::invalid_argument("checkpoint parameter shape mismatch");
        }
        result.parameters_ = checkpoint_vector(document.at("parameters"), result.parameter_count(), "parameters");
        result.velocity_ = checkpoint_vector(document.at("velocity"), result.parameter_count(), "velocity");
        result.training_steps_ = unsigned_integer(document.at("training_steps"), "training_steps");
        const auto& optimizer = document.at("last_optimizer");
        if (!optimizer.is_null()) result.last_optimizer_ = read_optimizer(optimizer);
        if (result.training_steps_ > 0 && !result.last_optimizer_)
            throw std::invalid_argument("trained checkpoint is missing optimizer settings");
        if (result.training_steps_ == 0 && (result.last_optimizer_ ||
            std::any_of(result.velocity_.begin(), result.velocity_.end(), [](double x) { return x != 0.0; })))
            throw std::invalid_argument("untrained checkpoint has optimizer history");
        return result;
    } catch (const nlohmann::json::exception& error) {
        throw std::invalid_argument(std::string("invalid checkpoint: ") + error.what());
    }
}
} // namespace betago
