#include "betago/state.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <optional>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#endif

namespace {
using namespace betago;
using Json = nlohmann::json;
std::string lower(std::string text) {
    for (char& value : text) if (value >= 'A' && value <= 'Z') value = static_cast<char>(value - 'A' + 'a');
    return text;
}
std::uint64_t pid() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}
[[noreturn]] void hang() { for (;;) std::this_thread::sleep_for(std::chrono::seconds(1)); }
void write_pid(const std::string& path, std::uint64_t process) {
    if (path.empty()) return;
    const std::filesystem::path output(path);
    if (output.has_parent_path()) std::filesystem::create_directories(output.parent_path());
    std::ofstream stream(output); stream << process << '\n'; stream.flush();
}
bool spawn_child(const std::string& child_pid, bool inherit_pipes = false) {
#ifdef _WIN32
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length == executable.size()) return false;
    executable.resize(length);
    auto quote = [](const std::wstring& argument) {
        std::wstring output = L"\"";
        std::size_t slashes = 0;
        for (wchar_t value : argument) {
            if (value == L'\\') { ++slashes; continue; }
            output.append(value == L'\"' ? 2 * slashes + 1 : slashes, L'\\');
            output += value; slashes = 0;
        }
        output.append(2 * slashes, L'\\'); output += L'\"'; return output;
    };
    std::wstring command = quote(executable) + L" --mode child-hang --child-pid " +
                           quote(std::filesystem::path(child_pid).wstring());
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    if (inherit_pipes) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    }
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, inherit_pipes ? TRUE : FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return false;
    write_pid(child_pid, process.dwProcessId);
    CloseHandle(process.hThread); CloseHandle(process.hProcess); return true;
#else
    (void)inherit_pipes;
    const auto child = fork();
    if (child < 0) return false;
    if (child == 0) { write_pid(child_pid, pid()); hang(); }
    write_pid(child_pid, static_cast<std::uint64_t>(child)); return true;
#endif
}
Move vertex(std::string text, int size) {
    text = lower(std::move(text));
    if (text == "pass") return PASS;
    if (text.size() < 2 || text[0] < 'a' || text[0] > 'z' || text[0] == 'i')
        throw std::invalid_argument("bad vertex");
    const int column = text[0] - 'a' - (text[0] > 'i' ? 1 : 0);
    std::size_t used = 0; const int row = std::stoi(text.substr(1), &used);
    if (used != text.size() - 1 || row < 1 || row > size || column < 0 || column >= size)
        throw std::invalid_argument("vertex outside board");
    return Point{size - row, column};
}
int color(std::string text) {
    text = lower(std::move(text));
    if (text == "b" || text == "black") return BLACK;
    if (text == "w" || text == "white") return WHITE;
    throw std::invalid_argument("bad color");
}
void stderr_flood() {
    const std::string chunk(4096, 'x');
    for (int index = 0; index < 512; ++index) std::cerr << chunk;
    std::cerr << "\nfake stderr sentinel\n" << std::flush;
}
void response(std::uint64_t id, bool success, const std::string& body,
              bool fragment, bool crlf, bool missing_id = false) {
    const std::string newline = crlf ? "\r\n" : "\n";
    std::string normalized;
    for (char value : body) normalized += value == '\n' ? newline : std::string(1, value);
    const std::string bytes = std::string(success ? "=" : "?") +
        (missing_id ? std::string() : std::to_string(id)) +
        (body.empty() ? std::string() : " " + normalized) + newline + newline;
    if (!fragment) { std::cout << bytes << std::flush; return; }
    for (std::size_t offset = 0; offset < bytes.size();) {
        const auto count = std::min<std::size_t>(1 + offset % 4, bytes.size() - offset);
        std::cout.write(bytes.data() + offset, static_cast<std::streamsize>(count)); std::cout.flush();
        offset += count; std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // Test framing byte-for-byte: Windows text mode would expand an explicitly
    // emitted CRLF into CRCRLF and invalidate the intended protocol fixture.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    std::string mode = "normal", log_path, moves_text = "pass", tag, child_pid;
    std::optional<std::int64_t> seed;
    std::map<std::string, std::string> overrides;
    std::vector<std::string> arguments;
    for (int index = 1; index < argc; ++index) {
        arguments.push_back(argv[index]); const std::string option = argv[index];
        auto value = [&]() -> std::string {
            if (++index == argc) throw std::invalid_argument("missing fake argument");
            arguments.push_back(argv[index]); return argv[index];
        };
        if (option == "--mode") mode = value();
        else if (option == "--log") log_path = value();
        else if (option == "--moves") moves_text = value();
        else if (option == "--tag") tag = value();
        else if (option == "--child-pid") child_pid = value();
        else if (option == "--seed") seed = std::stoll(value());
        else if (option == "-model" || option == "-config") value();
        else if (option == "-override-config") {
            std::istringstream override_list(value()); std::string entry;
            while (std::getline(override_list, entry, ',')) {
                const auto equals = entry.find('=');
                if (equals != std::string::npos) overrides[entry.substr(0, equals)] = entry.substr(equals + 1);
            }
        }
        else if (option != "gtp") return 42;
    }
    if (mode == "child-hang") { write_pid(child_pid, pid()); hang(); }
    std::ofstream log;
    if (!log_path.empty()) {
        const auto path = std::filesystem::path(log_path);
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
        log.open(path, std::ios::app);
    }
    auto event = [&](Json entry) {
        entry["pid"] = pid(); if (log) { log << entry.dump() << '\n'; log.flush(); }
    };
    event({{"event", "startup"}, {"mode", mode}, {"arguments", arguments}, {"tag", tag},
           {"seed", seed ? Json(*seed) : Json(nullptr)}});
    if ((mode == "spawn-child" || mode == "exit-child") && !spawn_child(child_pid, mode == "exit-child")) return 43;
    if (mode == "startup-exit") return 23;
    if (mode == "startup-hang") hang();
    if (mode == "startup-stderr") stderr_flood();
    std::vector<std::string> script;
    std::istringstream scripted(moves_text); std::string action;
    while (std::getline(scripted, action, ',')) script.push_back(action);
    int size = 9; double komi = 7.5; std::size_t selected = 0;
    GameState state = GameState::new_game(size, komi);
    Json effective_rules = Json::object();
    std::string line;
    int replies = 0;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream input(line); std::uint64_t id; std::string command;
        if (!(input >> id >> command)) return 44;
        std::vector<std::string> values; std::string part;
        while (input >> part) values.push_back(part);
        event({{"event", "command"}, {"id", id}, {"command", command}, {"values", values},
               {"to_play", state.to_play()}, {"passes", state.consecutive_passes()}});
        const bool fragment = mode == "fragment" || command == "test_fragment";
        const bool crlf = mode == "crlf" || command == "test_crlf" || (fragment && replies % 2 == 0);
        ++replies;
        if (command == "test_exit" || (command == "genmove" && mode == "exit-child")) return 17;
        if (command == "test_hang" || (command == "genmove" && mode == "hang-genmove") ||
            (command == "quit" && mode == "no-quit")) hang();
        if (command == "test_stderr" || (command == "genmove" && mode == "stderr-flood")) stderr_flood();
        if (command == "test_malformed" || (command == "genmove" && mode == "malformed")) {
            std::cout << "not a GTP response\n\n" << std::flush; continue;
        }
        if (command == "genmove" && mode == "binary-diagnostics") {
            std::cerr << "invalid stderr byte: " << static_cast<char>(0xfe) << '\n' << std::flush;
            response(id, true, std::string(1, static_cast<char>(0xff)), false, false);
            continue;
        }
        if (command == "test_oversize" || (command == "genmove" && mode == "oversize")) {
            response(id, true, std::string(1024 * 1024 + 128, 'z'), false, false); continue;
        }
        if (command == "test_wrong_id" || (command == "genmove" && mode == "wrong-id")) {
            response(id + 1, true, "pass", false, false); continue;
        }
        if (command == "test_missing_id" || (command == "genmove" && mode == "missing-id")) {
            response(id, true, "pass", false, false, true); continue;
        }
        if (command == "test_error" || (command == "genmove" && (mode == "error" ||
            (mode == "error-seed-17" && seed && *seed == 17 && selected >= 1)))) {
            response(id, false, "fake engine rejected command", false, false); continue;
        }
        try {
            auto argument = [&](std::size_t index) -> std::string {
                if (index >= values.size()) throw std::invalid_argument("missing command argument");
                return values[index];
            };
            std::string body;
            if (command == "name") body = mode == "empty-name" ? "" : "Fake Go engine";
            else if (command == "version") body = "test-1.0";
            else if (command == "protocol_version") body = mode == "old-protocol" ? "1" : "2";
            else if (command == "list_commands") body = "protocol_version\nname\nversion\nknown_command\nlist_commands\nboardsize\nkomi\nclear_board\nplay\ngenmove\nquit\nkata-set-rules\nkata-get-rules\nget_komi\nkata-get-models\nkata-get-param";
            else if (command == "known_command") body = "true";
            else if (command == "boardsize") { size = std::stoi(argument(0)); state = GameState::new_game(size, komi);
                                                   if (mode == "bad-setup-ack") body = "unexpected setup acknowledgement"; }
            else if (command == "komi") { komi = std::stod(argument(0)); state = GameState::new_game(size, komi); }
            else if (command == "clear_board") { state = GameState::new_game(size, komi); selected = 0; }
            else if (command == "kata-set-rules") { effective_rules = Json::parse(argument(0));
                                                       if (mode == "bad-rules-ack") body = "unexpected rules acknowledgement"; }
            else if (command == "kata-get-rules") {
                auto rules = effective_rules;
                if (mode == "kata-bad-rules") rules["ko"] = "POSITIONAL";
                body = rules.dump();
            } else if (command == "get_komi") {
                std::ostringstream value; value << std::setprecision(17) << (mode == "kata-bad-komi" ? komi + 1 : komi);
                body = value.str();
            } else if (command == "kata-get-models") body = mode == "kata-bad-models" ? "{}" : "[{\"name\":\"fake-model\",\"sha256\":\"test-placeholder\"}]";
            else if (command == "kata-get-param") {
                const auto setting = argument(0);
                if (setting == "allowResignation" || setting == "ponderingEnabled")
                    body = mode == "kata-bad-params" ? "true" : overrides.contains(setting) ? overrides[setting] : "true";
                else if (setting == "delayMoveScale" || setting == "delayMoveMax") body = overrides.contains(setting) ? overrides[setting] : "1";
                else if (setting == "maxTime" || setting == "maxPlayouts") body = "0";
                else if (setting == "maxVisits") body = "8";
                else if (setting == "numSearchThreads") body = "1";
                else if (setting == "humanSLProfile") body = "none";
                else throw std::invalid_argument("unknown KataGo parameter");
            }
            else if (command == "set_seed" || command == "seed") seed = std::stoll(argument(0));
            else if (command == "play") {
                if (color(argument(0)) != state.to_play()) throw std::invalid_argument("out of turn or duplicate own move");
                const auto next = state.play(vertex(argument(1), size));
                if (mode == "reject-terminal-play" && next.is_terminal())
                    throw std::invalid_argument("rejected final opponent pass");
                state = next;
            } else if (command == "genmove") {
                if (color(argument(0)) != state.to_play()) throw std::invalid_argument("genmove color out of turn");
                if (mode == "resign") body = "resign";
                else if (mode == "illegal") body = "Z99";
                else if (mode == "occupied") body = "A1";
                else if (mode == "suicide") body = "B2";
                else if (mode == "ko") body = "B4";
                else {
                    body = selected < script.size() ? script[selected++] : "pass";
                    if (lower(body) != "resign") state = state.play(vertex(body, size));
                }
            } else if (command == "test_fragment" || command == "test_crlf") body = "first line\nsecond line";
            else if (command == "test_stderr") body = "stderr drained";
            else if (command == "quit") {
                response(id, true, "", fragment, crlf);
                event({{"event", "quit"}});
                if (mode == "hang-quit") hang();
                return 0;
            } else throw std::invalid_argument("unknown command");
            response(id, true, body, fragment, crlf);
        } catch (const std::exception& error) {
            response(id, false, error.what(), fragment, crlf);
        }
    }
    return 0;
}
