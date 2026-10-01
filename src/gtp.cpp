#include "betago/gtp.hpp"
#include "betago/gtp_process.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>

namespace betago {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t MAX_RESPONSE_BYTES = 1024 * 1024;
constexpr const char* COLUMNS = "ABCDEFGHJKLMNOPQRSTUVWXYZ";

std::string trim(std::string_view text) {
    auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (!text.empty() && space(text.front())) text.remove_prefix(1);
    while (!text.empty() && space(text.back())) text.remove_suffix(1);
    return std::string(text);
}
std::string lower(std::string text) {
    for (char& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}
void require_size(int size) {
    if (size < 1 || size > 25) throw std::invalid_argument("Single-letter GTP coordinates require board size 1..25");
}
void valid_text(const std::string& text, const char* name, bool allow_empty = false) {
    if ((!allow_empty && text.empty()) || text.find('\0') != std::string::npos ||
        text.find('\r') != std::string::npos || text.find('\n') != std::string::npos)
        throw std::invalid_argument(std::string(name) + " must be a single nonempty text value without NUL or newlines");
}
std::string path_text(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}
std::filesystem::path resolved(const std::string& value, const std::filesystem::path& directory) {
    valid_text(value, "Profile path");
    auto path = std::filesystem::path(std::u8string(value.begin(), value.end()));
    if (path.is_relative()) path = directory / path;
    return std::filesystem::weakly_canonical(std::filesystem::absolute(path));
}
std::string string_field(const Json& document, const char* field) {
    if (!document.at(field).is_string()) throw std::invalid_argument(std::string(field) + " must be a string");
    return document.at(field).get<std::string>();
}
void allowed_keys(const Json& object, std::initializer_list<std::string_view> allowed, const char* context) {
    if (!object.is_object()) throw std::invalid_argument(std::string(context) + " must be an object");
    for (const auto& entry : object.items())
        if (std::find(allowed.begin(), allowed.end(), entry.key()) == allowed.end())
            throw std::invalid_argument(std::string("Unknown ") + context + " field: " + entry.key());
}
int timeout(const Json& value, const char* name) {
    if (!value.is_number_integer()) throw std::invalid_argument(std::string(name) + " must be an integer");
    const auto number = value.get<std::int64_t>();
    if (number < 1 || number > 3600000) throw std::invalid_argument(std::string(name) + " must be 1..3600000 milliseconds");
    return static_cast<int>(number);
}
Json fingerprint_file(const std::filesystem::path& path, const std::string& role) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::invalid_argument("Cannot read declared external-engine file: " + path_text(path));
    std::uint64_t hash = 14695981039346656037ULL, count = 0;
    char block[65536];
    while (input) {
        input.read(block, sizeof(block));
        const auto length = input.gcount();
        count += static_cast<std::uint64_t>(length);
        for (std::streamsize i = 0; i < length; ++i) {
            hash ^= static_cast<unsigned char>(block[i]); hash *= 1099511628211ULL;
        }
    }
    if (!input.eof()) throw std::invalid_argument("Cannot fingerprint external-engine file: " + path_text(path));
    std::ostringstream encoded;
    encoded << std::hex << std::setw(16) << std::setfill('0') << hash;
    return {{"role", role}, {"path", path_text(path)}, {"bytes", count}, {"fingerprint_fnv1a64", encoded.str()}};
}
std::map<std::string, std::string> override_values(const std::vector<std::string>& arguments) {
    std::map<std::string, std::string> result;
    for (std::size_t i = 0; i + 1 < arguments.size(); ++i) {
        if (arguments[i] != "-override-config" && arguments[i] != "--override-config") continue;
        std::istringstream values(arguments[++i]);
        std::string item;
        while (std::getline(values, item, ',')) {
            const auto equals = item.find('=');
            if (equals != std::string::npos) result[trim(std::string_view(item).substr(0, equals))] =
                trim(std::string_view(item).substr(equals + 1));
        }
    }
    return result;
}
bool katago_file_flag(const std::string& argument) {
    return argument == "-model" || argument == "--model" || argument == "-human-model" ||
        argument == "--human-model" || argument == "-config" || argument == "--config";
}
std::vector<std::string> substituted_arguments(const ExternalGtpConfiguration& configuration, std::int64_t seed) {
    auto arguments = configuration.arguments;
    const std::string replacement = std::to_string(seed);
    for (auto& argument : arguments) {
        std::size_t position = 0;
        while ((position = argument.find("{seed}", position)) != std::string::npos) {
            argument.replace(position, 6, replacement); position += replacement.size();
        }
    }
    if (configuration.adapter == "katago") {
        // These startup parameters have special handling in KataGo's GTP
        // implementation. Apply them on the command line, then verify them
        // through kata-get-param rather than assuming search params include them.
        // Current KataGo accepts repeated overrides. Consolidating them also
        // supports runtimes whose argument parser accepts a single occurrence.
        std::vector<std::string> consolidated;
        std::string overrides;
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            if (arguments[i] == "-override-config" || arguments[i] == "--override-config") {
                if (++i == arguments.size()) throw std::invalid_argument("KataGo override-config requires a value");
                if (!arguments[i].empty()) {
                    if (!overrides.empty()) overrides += ',';
                    overrides += arguments[i];
                }
            } else consolidated.push_back(arguments[i]);
        }
        if (!overrides.empty()) overrides += ',';
        overrides += "allowResignation=false,ponderingEnabled=false,delayMoveScale=0,delayMoveMax=0";
        consolidated.push_back("-override-config"); consolidated.push_back(std::move(overrides));
        arguments = std::move(consolidated);
    }
    return arguments;
}
Json parse_engine_json(const std::string& text, const std::string& command) {
    try { return Json::parse(text); }
    catch (const Json::exception& error) { throw GtpFailure("malformed_response", command,
        std::string(error.what()) + "; received body: " + text); }
}
std::string color_name(int color) {
    if (color != BLACK && color != WHITE) throw std::invalid_argument("Unknown GTP player color");
    return color == BLACK ? "B" : "W";
}
} // namespace

GtpFailure::GtpFailure(std::string failure_code, std::string failure_command, std::string details)
    : std::runtime_error(failure_code + (failure_command.empty() ? "" : " during " + failure_command) + ": " + details),
      code(std::move(failure_code)), command(std::move(failure_command)), diagnostics(std::move(details)) {}
Json GtpFailure::to_json() const { return {{"code", code}, {"command", command}, {"diagnostics", diagnostics}}; }
GtpResignation::GtpResignation(std::string request_command)
    : std::runtime_error("External GTP engine resigned"), command(std::move(request_command)) {}

std::string move_to_gtp(const Move& move, int size) {
    require_size(size);
    if (!move) return "pass";
    if (move->row < 0 || move->column < 0 || move->row >= size || move->column >= size)
        throw std::invalid_argument("GTP placement is outside the board");
    return std::string(1, COLUMNS[move->column]) + std::to_string(size - move->row);
}
Move move_from_gtp(std::string_view text, int size) {
    require_size(size);
    const auto token = trim(text);
    if (lower(token) == "pass") return PASS;
    if (token.size() < 2 || token.size() > 3) throw std::invalid_argument("GTP move must be a vertex or pass");
    char column = token.front();
    if (column >= 'a' && column <= 'z') column = static_cast<char>(column - 'a' + 'A');
    const auto found = std::string_view(COLUMNS).find(column);
    int row = 0;
    const auto parsed = std::from_chars(token.data() + 1, token.data() + token.size(), row);
    if (found == std::string_view::npos || found >= static_cast<std::size_t>(size) ||
        parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || row < 1 || row > size || token[1] == '0')
        throw std::invalid_argument("GTP vertex is outside the board or malformed");
    return Point{size - row, static_cast<int>(found)};
}

void GtpResponseParser::feed(std::string_view fragment) {
    if (fragment.find('\0') != std::string_view::npos || fragment.size() > MAX_RESPONSE_BYTES - buffer_.size())
        throw GtpFailure("malformed_response", "", "GTP response contains NUL or exceeds 1 MiB");
    buffer_.append(fragment);
}
std::optional<GtpResponse> GtpResponseParser::take(std::uint64_t expected_id) {
    std::size_t position = 0;
    std::vector<std::string_view> lines;
    while (true) {
        const auto end = buffer_.find('\n', position);
        if (end == std::string::npos) return std::nullopt;
        std::string_view line(buffer_.data() + position, end - position);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.find('\r') != std::string_view::npos)
            throw GtpFailure("malformed_response", "", "GTP response has a bare carriage return");
        position = end + 1;
        if (line.empty()) break;
        lines.push_back(line);
    }
    if (lines.empty()) throw GtpFailure("malformed_response", "", "GTP response has no numbered header");
    const auto header = lines.front();
    if (header.size() < 2 || (header.front() != '=' && header.front() != '?') || header[1] < '0' || header[1] > '9')
        throw GtpFailure("malformed_response", "", "GTP response must begin with =ID or ?ID");
    std::uint64_t id = 0;
    const auto parsed = std::from_chars(header.data() + 1, header.data() + header.size(), id);
    if (parsed.ec != std::errc{} || (parsed.ptr != header.data() + header.size() && *parsed.ptr != ' ' && *parsed.ptr != '\t'))
        throw GtpFailure("malformed_response", "", "GTP response identifier is malformed");
    if (id != expected_id) throw GtpFailure("wrong_response_id", "", "Expected GTP response " + std::to_string(expected_id) +
        ", received " + std::to_string(id));
    std::string body = trim(std::string_view(parsed.ptr, static_cast<std::size_t>(header.data() + header.size() - parsed.ptr)));
    for (std::size_t index = 1; index < lines.size(); ++index) {
        if (!body.empty()) body += '\n';
        body += lines[index];
    }
    try { (void)Json(body).dump(); }
    catch (const Json::exception&) {
        throw GtpFailure("malformed_response", "", "GTP response body is not valid UTF-8");
    }
    const bool success = header.front() == '=';
    buffer_.erase(0, position);
    return GtpResponse{success, id, std::move(body)};
}

void ExternalGtpConfiguration::validate() const {
    if (adapter != "generic" && adapter != "katago") throw std::invalid_argument("External adapter must be generic or katago");
    valid_text(display_name, "External engine display_name");
    valid_text(path_text(executable), "External executable");
    std::error_code executable_error;
    if (!executable.is_absolute() || std::filesystem::is_directory(executable, executable_error))
        throw std::invalid_argument("External executable must be an absolute file path, not a directory");
    if (!working_directory.is_absolute() || !std::filesystem::is_directory(working_directory))
        throw std::invalid_argument("External working_directory must be an existing absolute directory");
    for (int value : {startup_ms, command_ms, shutdown_ms})
        if (value < 1 || value > 3600000) throw std::invalid_argument("External timeouts must be 1..3600000 milliseconds");
    std::size_t argument_bytes = 0;
    for (const auto& argument : arguments) {
        valid_text(argument, "External argument", true);
        argument_bytes += argument.size();
        if (argument_bytes > MAX_RESPONSE_BYTES) throw std::invalid_argument("External arguments exceed 1 MiB");
    }
    std::set<std::string> roles;
    for (const auto& file : files) {
        valid_text(file.role, "Declared file role"); valid_text(path_text(file.path), "Declared file path");
        if (!roles.insert(file.role).second || file.role == "executable" || file.role == "profile")
            throw std::invalid_argument("Declared file roles must be unique and cannot be executable or profile");
        if (!file.path.is_absolute() || !std::filesystem::is_regular_file(file.path))
            throw std::invalid_argument("Declared files must be existing absolute paths");
    }
    valid_text(reference_label, "External reference_label", true);
    if (notes.find('\0') != std::string::npos || seed_documentation.find('\0') != std::string::npos)
        throw std::invalid_argument("External notes and seed_documentation must not contain NUL");
    if (adapter == "katago") {
        bool has_model = false, has_config = false, has_human_model = false;
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            const auto& argument = arguments[i];
            if (argument == "-override-config" || argument == "--override-config") {
                if (++i == arguments.size()) throw std::invalid_argument("KataGo override-config requires a value");
                continue;
            }
            if (!katago_file_flag(argument)) continue;
            const auto flag = argument;
            if (++i == arguments.size()) throw std::invalid_argument("KataGo file argument requires a path: " + flag);
            const auto path = resolved(arguments[i], working_directory);
            if (std::none_of(files.begin(), files.end(), [&](const auto& file) {
                    return std::filesystem::weakly_canonical(file.path) == path;
                }))
                throw std::invalid_argument("KataGo argv file must be declared in files: " + path_text(path));
            if (flag == "-model" || flag == "--model") {
                if (has_model) throw std::invalid_argument("KataGo -model must appear only once");
                has_model = true;
            } else if (flag == "-human-model" || flag == "--human-model") {
                if (has_human_model) throw std::invalid_argument("KataGo -human-model must appear only once");
                has_human_model = true;
            } else has_config = true;
        }
        if (!has_model || !has_config) throw std::invalid_argument("KataGo profiles require explicit declared -model and -config files");
    }
}
bool ExternalGtpConfiguration::seeded_stochastic() const {
    if (!declares_seeded_stochastic || trim(seed_documentation).empty()) return false;
    if (adapter == "generic") return std::any_of(arguments.begin(), arguments.end(), [](const auto& argument) {
        return argument.find("{seed}") != std::string::npos;
    });
    if (adapter != "katago") return false;
    const auto values = override_values(arguments);
    const auto search = values.find("searchRandSeed"), network = values.find("nnRandSeed");
    return search != values.end() && network != values.end() &&
        search->second.find("{seed}") != std::string::npos && network->second.find("{seed}") != std::string::npos;
}
Json ExternalGtpConfiguration::to_json() const {
    Json declared = Json::array();
    for (const auto& file : files) declared.push_back({{"role", file.role}, {"path", path_text(file.path)}});
    return {{"schema_version", 1}, {"kind", "external_gtp_profile"}, {"executable", path_text(executable)},
        {"arguments", arguments}, {"working_directory", path_text(working_directory)}, {"adapter", adapter},
        {"display_name", display_name}, {"notes", notes}, {"reference_label", reference_label},
        {"seed_documentation", seed_documentation}, {"profile_path", path_text(profile_path)},
        {"timeouts", {{"startup_ms", startup_ms}, {"command_ms", command_ms}, {"shutdown_ms", shutdown_ms}}},
        {"files", std::move(declared)}, {"seeded_stochastic", seeded_stochastic()},
        {"declares_seeded_stochastic", declares_seeded_stochastic}, {"declared_file_fingerprints", declared_file_fingerprints},
        {"fingerprint_note", "FNV-1a 64-bit identifies inputs; it is not a cryptographic signature."}};
}
Json external_gtp_identity(const ExternalGtpConfiguration& configuration) {
    configuration.validate();
    Json records = Json::array();
    try { records.push_back(fingerprint_file(configuration.executable, "executable")); }
    catch (const std::exception& error) { records.push_back({{"role", "executable"},
        {"path", path_text(configuration.executable)}, {"bytes", nullptr}, {"fingerprint_fnv1a64", nullptr},
        {"unavailable", error.what()}}); }
    if (!configuration.profile_path.empty()) records.push_back(fingerprint_file(configuration.profile_path, "profile"));
    for (const auto& file : configuration.files) records.push_back(fingerprint_file(file.path, file.role));
    return records;
}
ExternalGtpConfiguration load_external_gtp_profile(const std::filesystem::path& path) {
    try {
        const auto absolute = std::filesystem::weakly_canonical(std::filesystem::absolute(path));
        if (std::filesystem::file_size(absolute) > MAX_RESPONSE_BYTES) throw std::invalid_argument("External profile exceeds 1 MiB");
        std::ifstream stream(absolute, std::ios::binary);
        if (!stream) throw std::invalid_argument("Cannot open external GTP profile");
        const Json document = Json::parse(stream);
        allowed_keys(document, {"schema_version", "kind", "executable", "arguments", "working_directory",
            "display_name", "adapter", "reference_label", "notes", "files", "timeouts",
            "seeded_stochastic", "seed_documentation"}, "external profile");
        if (!document.is_object() || !document.at("schema_version").is_number_integer() || document.at("schema_version") != 1 ||
            document.at("kind") != "external_gtp_profile") throw std::invalid_argument("Unsupported external GTP profile schema");
        ExternalGtpConfiguration result;
        result.profile_path = absolute;
        result.executable = resolved(string_field(document, "executable"), absolute.parent_path());
        result.working_directory = document.contains("working_directory") ?
            resolved(string_field(document, "working_directory"), absolute.parent_path()) : absolute.parent_path();
        result.adapter = string_field(document, "adapter"); result.display_name = string_field(document, "display_name");
        if (!document.at("arguments").is_array()) throw std::invalid_argument("External arguments must be an array");
        for (const auto& argument : document.at("arguments")) {
            if (!argument.is_string()) throw std::invalid_argument("External arguments must contain strings");
            result.arguments.push_back(argument.get<std::string>());
        }
        if (document.contains("notes")) result.notes = string_field(document, "notes");
        if (document.contains("reference_label")) result.reference_label = string_field(document, "reference_label");
        if (document.contains("seed_documentation")) result.seed_documentation = string_field(document, "seed_documentation");
        if (document.contains("seeded_stochastic")) {
            if (!document.at("seeded_stochastic").is_boolean()) throw std::invalid_argument("seeded_stochastic must be boolean");
            result.declares_seeded_stochastic = document.at("seeded_stochastic").get<bool>();
        }
        if (document.contains("timeouts")) {
            const auto& times = document.at("timeouts");
            allowed_keys(times, {"startup_ms", "command_ms", "shutdown_ms"}, "timeouts");
            if (times.contains("startup_ms")) result.startup_ms = timeout(times.at("startup_ms"), "startup_ms");
            if (times.contains("command_ms")) result.command_ms = timeout(times.at("command_ms"), "command_ms");
            if (times.contains("shutdown_ms")) result.shutdown_ms = timeout(times.at("shutdown_ms"), "shutdown_ms");
        }
        if (document.contains("files")) {
            if (!document.at("files").is_array()) throw std::invalid_argument("Declared files must be an array");
            for (const auto& file : document.at("files")) {
                allowed_keys(file, {"role", "path"}, "declared file");
                result.files.push_back({string_field(file, "role"), resolved(string_field(file, "path"), absolute.parent_path())});
            }
            // Declared file argv entries retain profile-relative meaning even
            // when a separate engine working directory is configured.
            for (auto& argument : result.arguments)
                for (std::size_t index = 0; index < result.files.size(); ++index)
                    if (argument == string_field(document.at("files")[index], "path")) {
                        argument = path_text(result.files[index].path);
                        break;
                    }
        }
        if (result.adapter == "katago") {
            for (std::size_t i = 0; i < result.arguments.size(); ++i) {
                if (!katago_file_flag(result.arguments[i])) continue;
                if (++i == result.arguments.size()) throw std::invalid_argument("KataGo file argument requires a path");
                result.arguments[i] = path_text(resolved(result.arguments[i], absolute.parent_path()));
            }
        }
        result.validate(); result.declared_file_fingerprints = external_gtp_identity(result);
        return result;
    } catch (const Json::exception& error) { throw std::invalid_argument(std::string("Malformed external GTP profile: ") + error.what()); }
}

ExternalGtpSession::ExternalGtpSession(ExternalGtpConfiguration configuration, std::int64_t seed, int size, double komi)
    : configuration_(std::move(configuration)), seed_(seed), size_(size), komi_(komi) {
    metadata_ = {{"configuration", configuration_.to_json()}, {"seed", seed_},
        {"actual_arguments", substituted_arguments(configuration_, seed_)}, {"board_size", size_}, {"komi", komi_},
        {"seeded_stochastic", configuration_.seeded_stochastic()}, {"adapter", configuration_.adapter},
        {"started", false}, {"rules_verification", "not_completed"}, {"warnings", Json::array()}};
    Json seed_indices = Json::array();
    for (std::size_t i = 0; i < configuration_.arguments.size(); ++i)
        if (configuration_.arguments[i].find("{seed}") != std::string::npos) seed_indices.push_back(i);
    metadata_["engine_seed"] = seed_indices.empty() ? Json(nullptr) : Json(seed_);
    metadata_["seed_status"] = seed_indices.empty() ? "unconfigured" : "argument_substitution";
    metadata_["seed_integration"] = {{"documentation", configuration_.seed_documentation},
        {"template_argument_indices", std::move(seed_indices)}, {"eligible", configuration_.seeded_stochastic()}};
}
ExternalGtpSession::~ExternalGtpSession() { shutdown(); }
std::string ExternalGtpSession::request(const std::string& command, Clock::time_point deadline) {
    if (!process_ || closed_) throw GtpFailure("unexpected_exit", command, "External engine is not running");
    if (next_id_ > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
        throw GtpFailure("malformed_response", command, "GTP command ID limit exhausted");
    const auto id = next_id_++;
    try {
        process_->write(std::to_string(id) + " " + command + "\n", deadline);
        while (true) {
            if (auto response = parser_.take(id)) {
                if (!response->success) throw GtpFailure("gtp_rejection", command, response->body);
                return response->body;
            }
            const auto chunk = process_->read(deadline);
            constexpr std::size_t DIAGNOSTIC_BYTES = 65536;
            if (chunk.size() >= DIAGNOSTIC_BYTES) {
                stdout_truncated_ = stdout_truncated_ || !stdout_tail_.empty() || chunk.size() > DIAGNOSTIC_BYTES;
                stdout_tail_.assign(chunk.end() - DIAGNOSTIC_BYTES, chunk.end());
            } else {
                if (stdout_tail_.size() + chunk.size() > DIAGNOSTIC_BYTES) {
                    stdout_tail_.erase(0, stdout_tail_.size() + chunk.size() - DIAGNOSTIC_BYTES);
                    stdout_truncated_ = true;
                }
                stdout_tail_ += chunk;
            }
            parser_.feed(chunk);
        }
    } catch (const GtpFailure& error) {
        throw GtpFailure(error.code, command, error.diagnostics + "\nstdout tail" +
            (stdout_truncated_ ? " (truncated)" : "") + ":\n" + stdout_tail_ +
            "\nstderr tail:\n" + process_->stderr_text());
    }
}
std::string ExternalGtpSession::request(const std::string& command) {
    return request(command, Clock::now() + std::chrono::milliseconds(configuration_.command_ms));
}
void ExternalGtpSession::start() {
    if (started_ || closed_ || process_) throw std::logic_error("External GTP session can be started only once");
    const auto began = Clock::now();
    const auto deadline = began + std::chrono::milliseconds(configuration_.startup_ms);
    auto record_timing = [&] { metadata_["startup_seconds"] = std::chrono::duration<double>(Clock::now() - began).count(); };
    try {
        configuration_.validate(); require_size(size_);
        if (!std::isfinite(komi_)) throw std::invalid_argument("GTP komi must be finite");
        if (!configuration_.declared_file_fingerprints.is_null() &&
            external_gtp_identity(configuration_) != configuration_.declared_file_fingerprints)
            throw GtpFailure("launch_failure", "start", "Declared external-engine files changed after profile preparation");
        const auto arguments = metadata_.at("actual_arguments").get<std::vector<std::string>>();
        process_ = make_gtp_process(configuration_.executable, arguments, configuration_.working_directory);
        if (trim(request("protocol_version", deadline)) != "2")
            throw GtpFailure("unsupported_capability", "protocol_version", "External engine must support GTP version 2");
        metadata_["engine_name"] = trim(request("name", deadline));
        metadata_["engine_version"] = trim(request("version", deadline));
        if (metadata_.at("engine_name") == "" || metadata_.at("engine_version") == "")
            throw GtpFailure("malformed_response", "name/version", "Engine name and version must be nonempty");
        const auto available = request("list_commands", deadline);
        std::istringstream commands(available);
        std::set<std::string> supported;
        std::string command;
        while (commands >> command) supported.insert(command);
        for (const auto* required : {"boardsize", "clear_board", "komi", "play", "genmove", "quit"})
            if (!supported.contains(required)) throw GtpFailure("unsupported_capability", "list_commands", std::string("Missing command: ") + required);
        metadata_["supported_commands"] = supported;
        auto empty_ack = [&](const std::string& command) {
            const auto body = request(command, deadline);
            if (!trim(body).empty()) throw GtpFailure("malformed_response", command,
                "Setup acknowledgement must have an empty body; received: " + body);
        };
        empty_ack("boardsize " + std::to_string(size_)); empty_ack("clear_board");
        std::ostringstream komi; komi << std::setprecision(17) << komi_;
        empty_ack("komi " + komi.str());
        if (configuration_.adapter == "katago") {
            for (const auto* required : {"kata-set-rules", "kata-get-rules", "get_komi", "kata-get-models", "kata-get-param"})
                if (!supported.contains(required)) throw GtpFailure("unsupported_capability", "list_commands", std::string("KataGo adapter requires: ") + required);
            const Json rules = {{"ko", "SIMPLE"}, {"scoring", "AREA"}, {"tax", "NONE"}, {"suicide", false},
                {"hasButton", false}, {"whiteHandicapBonus", "0"}, {"friendlyPassOk", false}};
            empty_ack("kata-set-rules " + rules.dump());
            const auto actual = parse_engine_json(request("kata-get-rules", deadline), "kata-get-rules");
            for (const auto& field : rules.items())
                if (!actual.is_object() || !actual.contains(field.key()) || actual.at(field.key()) != field.value())
                    throw GtpFailure("unsupported_capability", "kata-get-rules", "Effective KataGo rules disagree with BetaGo: " + field.key());
            metadata_["effective_rules"] = actual;
            const auto effective_komi = parse_engine_json(request("get_komi", deadline), "get_komi");
            if (!effective_komi.is_number() || effective_komi.get<double>() != komi_)
                throw GtpFailure("unsupported_capability", "get_komi", "Effective KataGo komi disagrees with requested komi");
            metadata_["effective_komi"] = effective_komi;
            const auto models = parse_engine_json(request("kata-get-models", deadline), "kata-get-models");
            if (!models.is_array() || models.empty()) throw GtpFailure("malformed_response", "kata-get-models", "Expected nonempty model metadata array");
            metadata_["models"] = models;
            Json effective = Json::object();
            for (const auto* setting : {"allowResignation", "ponderingEnabled", "delayMoveScale", "delayMoveMax"}) {
                const auto text = trim(request(std::string("kata-get-param ") + setting, deadline));
                const auto value = parse_engine_json(text, std::string("kata-get-param ") + setting);
                const bool disabled = std::string_view(setting) == "allowResignation" || std::string_view(setting) == "ponderingEnabled";
                if (disabled ? value != false : (!value.is_number() || value.get<double>() != 0))
                    throw GtpFailure("unsupported_capability", std::string("kata-get-param ") + setting, "KataGo startup override was not effective");
                effective[setting] = value;
            }
            for (const auto* setting : {"maxVisits", "maxPlayouts", "maxTime", "numSearchThreads", "humanSLProfile"})
                effective[setting] = trim(request(std::string("kata-get-param ") + setting, deadline));
            metadata_["effective_parameters"] = std::move(effective);
            metadata_["rules_note"] = "KataGo rule settings verified; repetition/no-result handling and internal termination/adjudication may still differ. BetaGo raw final-board scoring is authoritative.";
        } else {
            metadata_["effective_rules"] = nullptr;
            metadata_["rules_note"] = "Generic GTP cannot verify scoring/ko configuration; local legality and scoring remain authoritative.";
        }
        const auto loaded_identity = external_gtp_identity(configuration_);
        if (!configuration_.declared_file_fingerprints.is_null() && loaded_identity != configuration_.declared_file_fingerprints)
            throw GtpFailure("launch_failure", "start", "Declared external-engine files changed while the engine was starting");
        if (Clock::now() >= deadline) throw GtpFailure("timeout", "start", "Startup deadline expired during input verification");
        metadata_["verified_input_identity"] = loaded_identity;
        metadata_["rules_verification"] = configuration_.adapter == "katago" ? "verified_configuration" : "unverified_generic";
        started_ = true; metadata_["started"] = true; metadata_["process"] = process_->metadata(); record_timing();
    } catch (const GtpFailure&) { record_timing(); throw; }
    catch (const std::exception& error) { record_timing(); throw GtpFailure("launch_failure", "start", error.what()); }
}
Move ExternalGtpSession::genmove(const GameState& state) {
    if (!started_ || closed_) throw GtpFailure("unexpected_exit", "genmove", "External session has not started or was closed");
    if (state.size() != size_ || state.komi() != komi_ || state.is_terminal())
        throw GtpFailure("illegal_move", "genmove", "Local state disagrees with external session settings or is terminal");
    if (pending_own_move_) throw GtpFailure("illegal_move", "genmove", "Previous generated move was not acknowledged");
    const auto command = "genmove " + color_name(state.to_play());
    const auto token = trim(request(command));
    if (lower(token) == "resign") throw GtpResignation(command);
    Move move;
    try { move = move_from_gtp(token, size_); }
    catch (const std::invalid_argument& error) { throw GtpFailure("malformed_response", command,
        std::string(error.what()) + "; received move: " + token); }
    try { state.play(move); }
    catch (const IllegalMove& error) { throw GtpFailure("illegal_move", command,
        std::string(error.what()) + "; received move: " + token); }
    pending_own_move_ = move; pending_own_color_ = state.to_play();
    return move;
}
void ExternalGtpSession::accepted_move(int color, const Move& move, bool own_move) {
    if (!started_ || closed_) throw GtpFailure("unexpected_exit", "play", "External session has not started or was closed");
    const auto command = "play " + color_name(color) + " " + move_to_gtp(move, size_);
    if (own_move) {
        if (!pending_own_move_ || pending_own_color_ != color || *pending_own_move_ != move)
            throw GtpFailure("illegal_move", command, "Acknowledgement does not match the external generated move");
        pending_own_move_.reset(); pending_own_color_ = 0;
    } else {
        if (pending_own_move_) throw GtpFailure("illegal_move", command, "Generated move must be acknowledged before its opponent reply");
        if (!trim(request(command)).empty()) throw GtpFailure("malformed_response", command, "GTP play acknowledgement must have an empty body");
    }
}
Json ExternalGtpSession::metadata() const {
    auto result = metadata_;
    if (process_) { result["process"] = process_->metadata(); result["stderr"] = process_->stderr_text(); }
    return result;
}
Json ExternalGtpSession::shutdown() noexcept {
    Json warnings = Json::array();
    if (closed_) return warnings;
    const auto began = Clock::now();
    try {
        if (process_) {
            const auto deadline = Clock::now() + std::chrono::milliseconds(configuration_.shutdown_ms);
            if (started_) {
                try { request("quit", deadline); }
                catch (const std::exception& error) { warnings.push_back(std::string("GTP quit: ") + error.what()); }
            }
            if (auto warning = process_->close(deadline)) warnings.push_back(*warning);
            metadata_["process"] = process_->metadata(); metadata_["stderr"] = process_->stderr_text();
        }
    } catch (const std::exception& error) {
        try { warnings.push_back(std::string("GTP shutdown: ") + error.what()); } catch (...) {}
    } catch (...) {}
    closed_ = true;
    try { metadata_["closed"] = true; metadata_["warnings"] = warnings;
          metadata_["shutdown_seconds"] = std::chrono::duration<double>(Clock::now() - began).count(); } catch (...) {}
    return warnings;
}
} // namespace betago
