#pragma once

#include "gtp.hpp"
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace betago {
// Transport only: protocol parsing and engine adapters live in gtp.cpp.
class GtpProcess {
public:
    using Deadline = std::chrono::steady_clock::time_point;
    virtual ~GtpProcess() = default;
    virtual void write(const std::string& bytes, Deadline deadline) = 0;
    virtual std::string read(Deadline deadline) = 0;
    virtual std::string stderr_text() const = 0;
    virtual Json metadata() const = 0;
    // Wait for normal exit until the deadline, then kill the owned process tree.
    // A warning describes forced cleanup; resource cleanup always occurs.
    virtual std::optional<std::string> close(Deadline deadline) noexcept = 0;
};

// Arguments contain UTF-8 text. The application is launched directly, without
// cmd.exe or PowerShell; standard Windows argv quoting preserves each element.
std::wstring quote_windows_argument(const std::wstring& argument);
std::unique_ptr<GtpProcess> make_gtp_process(const std::filesystem::path& executable,
    const std::vector<std::string>& arguments, const std::filesystem::path& working_directory);
} // namespace betago
