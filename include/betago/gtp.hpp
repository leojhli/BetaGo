#pragma once

#include "runner.hpp"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace betago {
class GtpFailure : public std::runtime_error {
public:
    GtpFailure(std::string code, std::string command, std::string diagnostics);
    const std::string code, command, diagnostics;
    Json to_json() const;
};
class GtpResignation : public std::runtime_error {
public:
    explicit GtpResignation(std::string command);
    const std::string command;
};

std::string move_to_gtp(const Move& move, int size);
Move move_from_gtp(std::string_view text, int size);

struct GtpResponse {
    bool success;
    std::uint64_t id;
    std::string body;
};
// Strict numbered responses, completed by a blank LF or CRLF line. Buffers
// at most 1 MiB; feed accepts arbitrary fragments, including split CRLF.
class GtpResponseParser {
public:
    void feed(std::string_view fragment);
    std::optional<GtpResponse> take(std::uint64_t expected_id);
    bool empty() const { return buffer_.empty(); }
private:
    std::string buffer_;
};

struct ExternalGtpFile {
    std::string role;
    std::filesystem::path path;
};
// Profile JSON: schema_version=1, kind="external_gtp_profile", executable,
// arguments:[strings], adapter:"generic"|"katago", display_name,
// optional working_directory, notes, reference_label, seed_documentation,
// seeded_stochastic (default false),
// timeouts:{startup_ms,command_ms,shutdown_ms}, files:[{role,path}].
// Relative paths resolve against the profile's directory. Arguments are
// literal argv entries; only {seed} is substituted, without invoking a shell.
// KataGo -model/-human-model/-config paths resolve against the profile directory
// and must appear in files. KataGo requires explicit -model and -config inputs.
struct ExternalGtpConfiguration {
    std::filesystem::path executable, working_directory, profile_path;
    std::vector<std::string> arguments;
    std::string adapter = "generic", display_name, notes, reference_label, seed_documentation;
    int startup_ms = 120000, command_ms = 30000, shutdown_ms = 2000;
    std::vector<ExternalGtpFile> files;
    bool declares_seeded_stochastic = false;
    Json declared_file_fingerprints = nullptr;
    void validate() const;
    Json to_json() const;
    bool seeded_stochastic() const;
};
ExternalGtpConfiguration load_external_gtp_profile(const std::filesystem::path& path);
// Read a fresh identity for drift checking; loaded profiles retain their
// original snapshot in declared_file_fingerprints.
Json external_gtp_identity(const ExternalGtpConfiguration& configuration);

class GtpProcess;
class ExternalGtpSession {
public:
    ExternalGtpSession(ExternalGtpConfiguration configuration, std::int64_t seed, int size, double komi);
    ~ExternalGtpSession();
    ExternalGtpSession(const ExternalGtpSession&) = delete;
    ExternalGtpSession& operator=(const ExternalGtpSession&) = delete;
    void start();
    Move genmove(const GameState& state);
    void accepted_move(int color, const Move& move, bool own_move);
    Json shutdown() noexcept;
    Json metadata() const;
private:
    ExternalGtpConfiguration configuration_;
    std::int64_t seed_;
    int size_;
    double komi_;
    std::unique_ptr<GtpProcess> process_;
    GtpResponseParser parser_;
    std::uint64_t next_id_ = 1;
    std::string stdout_tail_;
    bool stdout_truncated_ = false;
    Json metadata_ = Json::object();
    bool started_ = false, closed_ = false;
    std::optional<Move> pending_own_move_;
    int pending_own_color_ = 0;
    std::string request(const std::string& command, std::chrono::steady_clock::time_point deadline);
    std::string request(const std::string& command);
};
} // namespace betago
