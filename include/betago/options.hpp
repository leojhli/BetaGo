#pragma once
#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

namespace betago {
class Options {
public:
    Options(int argc, char** argv, std::set<std::string> valued, std::set<std::string> flags) {
        for (int i = 1; i < argc; ++i) {
            std::string key = argv[i];
            if (values_.contains(key)) throw std::invalid_argument("Repeated option " + key);
            if (flags.contains(key)) values_[key] = "";
            else if (valued.contains(key)) {
                if (++i == argc || valued.contains(argv[i]) || flags.contains(argv[i]))
                    throw std::invalid_argument("Missing value for " + key);
                values_[key] = argv[i];
            } else throw std::invalid_argument("Unknown option " + key);
        }
    }
    bool has(const std::string& key) const { return values_.contains(key); }
    std::string text(const std::string& key, std::string fallback = "") const {
        auto found = values_.find(key);
        return found == values_.end() ? fallback : found->second;
    }
    template<class Integer> Integer integer(const std::string& key, Integer fallback) const {
        if (!has(key)) return fallback;
        auto value = text(key);
        Integer number;
        auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
        if (error != std::errc{} || end != value.data() + value.size())
            throw std::invalid_argument("Invalid integer for " + key);
        return number;
    }
    double real(const std::string& key, double fallback) const {
        if (!has(key)) return fallback;
        auto value = text(key);
        std::size_t used = 0;
        double number;
        try { number = std::stod(value, &used); }
        catch (const std::invalid_argument&) { throw std::invalid_argument("Invalid number for " + key); }
        catch (const std::out_of_range&) { throw std::invalid_argument("Invalid number for " + key); }
        if (used != value.size() || !std::isfinite(number)) throw std::invalid_argument("Invalid number for " + key);
        return number;
    }
private:
    std::map<std::string, std::string> values_;
};
} // namespace betago
