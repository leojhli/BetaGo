#include "betago/gtp_process.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace betago {
std::wstring quote_windows_argument(const std::wstring& argument) {
    std::wstring quoted = L"\"";
    std::size_t slashes = 0;
    for (wchar_t character : argument) {
        if (character == L'\\') { ++slashes; continue; }
        quoted.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        quoted += character;
    }
    quoted.append(slashes * 2, L'\\');
    quoted += L'"';
    return quoted;
}

#if defined(_WIN32)
namespace {
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
    void reset(HANDLE value = nullptr) {
        if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_ = nullptr;
};

std::string windows_error(const char* operation, DWORD error = GetLastError()) {
    wchar_t* message = nullptr;
    const auto length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::string text = std::string(operation) + " (Windows error " + std::to_string(error) + ")";
    if (length && message) {
        const auto bytes = WideCharToMultiByte(CP_UTF8, 0, message, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
        std::string detail(static_cast<std::size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, message, static_cast<int>(length), detail.data(), bytes, nullptr, nullptr);
        text += ": " + detail;
    }
    if (message) LocalFree(message);
    return text;
}
[[noreturn]] void launch_error(const char* operation) {
    throw GtpFailure("launch_failure", "", windows_error(operation));
}
std::wstring utf8(const std::string& text) {
    if (text.find('\0') != std::string::npos || text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw GtpFailure("launch_failure", "", "Invalid external argument (NUL or excessive length)");
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!count) launch_error("Decode UTF-8 argument");
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}
DWORD remaining(GtpProcess::Deadline deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (deadline <= now) return 0;
    const auto count = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count() + 1;
    return static_cast<DWORD>(std::min<std::int64_t>(count, INFINITE - 1));
}
void output_pipe(Handle& parent, Handle& child) {
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &security, 65536)) launch_error("Create output pipe");
    parent.reset(read); child.reset(write);
    if (!SetHandleInformation(parent.get(), HANDLE_FLAG_INHERIT, 0)) launch_error("Restrict pipe inheritance");
}
void input_pipe(Handle& parent, Handle& child) {
    static std::atomic<unsigned long long> sequence{0};
    const auto name = L"\\\\.\\pipe\\betago-gtp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(++sequence);
    parent.reset(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED |
        FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0, nullptr));
    if (parent.get() == INVALID_HANDLE_VALUE) launch_error("Create input pipe");
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    child.reset(CreateFileW(name.c_str(), GENERIC_READ, 0, &security, OPEN_EXISTING, 0, nullptr));
    if (child.get() == INVALID_HANDLE_VALUE) launch_error("Open child input pipe");
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.get()) launch_error("Create connect event");
    OVERLAPPED connect{}; connect.hEvent = event.get();
    if (!ConnectNamedPipe(parent.get(), &connect)) {
        const auto error = GetLastError();
        if (error != ERROR_PIPE_CONNECTED) {
            if (error != ERROR_IO_PENDING || WaitForSingleObject(event.get(), 1000) != WAIT_OBJECT_0)
                launch_error("Connect child input pipe");
            DWORD ignored = 0;
            if (!GetOverlappedResult(parent.get(), &connect, &ignored, FALSE)) launch_error("Connect input result");
        }
    }
}

class WindowsGtpProcess final : public GtpProcess {
public:
    WindowsGtpProcess(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
                      const std::filesystem::path& directory) {
        Handle child_input, child_output, child_error;
        input_pipe(input_, child_input);
        output_pipe(output_, child_output);
        output_pipe(error_, child_error);
        job_.reset(CreateJobObjectW(nullptr, nullptr));
        if (!job_.get()) launch_error("Create process job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job_.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            launch_error("Set process cleanup job");

        SIZE_T attribute_bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
        std::vector<unsigned char> storage(attribute_bytes);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_bytes)) launch_error("Create launch attributes");
        struct AttributeCleanup { LPPROC_THREAD_ATTRIBUTE_LIST value; ~AttributeCleanup() { DeleteProcThreadAttributeList(value); } } attribute_cleanup{attributes};
        HANDLE inherited[] = {child_input.get(), child_output.get(), child_error.get()};
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                       sizeof(inherited), nullptr, nullptr)) launch_error("Restrict child handles");
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.StartupInfo.wShowWindow = SW_HIDE;
        startup.StartupInfo.hStdInput = child_input.get();
        startup.StartupInfo.hStdOutput = child_output.get();
        startup.StartupInfo.hStdError = child_error.get();
        startup.lpAttributeList = attributes;
        std::wstring command = quote_windows_argument(executable.wstring());
        for (const auto& argument : arguments) command += L" " + quote_windows_argument(utf8(argument));
        if (command.size() >= 32767) throw GtpFailure("launch_failure", "", "Windows command line exceeds 32766 characters");
        PROCESS_INFORMATION created{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
            directory.empty() ? nullptr : directory.c_str(), &startup.StartupInfo, &created)) launch_error("CreateProcessW");
        process_.reset(created.hProcess);
        Handle primary_thread(created.hThread);
        pid_ = created.dwProcessId;
        if (!AssignProcessToJobObject(job_.get(), process_.get())) {
            const auto saved = GetLastError();
            TerminateProcess(process_.get(), 1);
            throw GtpFailure("launch_failure", "", windows_error("Assign process cleanup job", saved));
        }
        if (ResumeThread(primary_thread.get()) == static_cast<DWORD>(-1)) launch_error("Resume external engine");
    }
    void start_drains() {
        output_reader_ = std::thread([this] { drain_output(); });
        error_reader_ = std::thread([this] { drain_error(); });
    }
    ~WindowsGtpProcess() override { close(std::chrono::steady_clock::now()); }
    void write(const std::string& bytes, Deadline deadline) override {
        if (closed_ || !input_.get()) throw GtpFailure("unexpected_exit", "", "Engine input is closed");
        if (remaining(deadline) == 0) throw GtpFailure("timeout", "", "Command write deadline expired");
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event.get()) throw GtpFailure("unexpected_exit", "", windows_error("Create write event"));
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            OVERLAPPED operation{}; operation.hEvent = event.get();
            ResetEvent(event.get());
            DWORD written = 0;
            const DWORD length = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 65536));
            if (!WriteFile(input_.get(), bytes.data() + offset, length, &written, &operation)) {
                const auto error = GetLastError();
                if (error != ERROR_IO_PENDING) throw GtpFailure("unexpected_exit", "", windows_error("Write engine command", error));
                const auto wait = WaitForSingleObject(event.get(), remaining(deadline));
                if (wait != WAIT_OBJECT_0) {
                    CancelIoEx(input_.get(), &operation);
                    // Cancellation must complete before OVERLAPPED/buffer storage is released.
                    GetOverlappedResult(input_.get(), &operation, &written, TRUE);
                    throw GtpFailure("timeout", "", "Command write deadline expired");
                }
                if (!GetOverlappedResult(input_.get(), &operation, &written, FALSE))
                    throw GtpFailure("unexpected_exit", "", windows_error("Write engine result"));
            }
            if (!written) throw GtpFailure("unexpected_exit", "", "Engine command pipe closed");
            offset += written;
        }
    }
    std::string read(Deadline deadline) override {
        std::unique_lock lock(output_mutex_);
        std::optional<Deadline> exit_observed;
        for (;;) {
            if (overflow_) throw GtpFailure("malformed_response", "", "Engine stdout exceeded the bounded response buffer (1 MiB)");
            if (!pending_.empty()) { std::string result; result.swap(pending_); return result; }
            if (output_done_) throw GtpFailure("unexpected_exit", "", output_error_.empty() ? "Engine stdout closed before response" : output_error_);
            const auto now = std::chrono::steady_clock::now();
            if (WaitForSingleObject(process_.get(), 0) == WAIT_OBJECT_0) {
                // Drain bytes already written before treating parent exit as a
                // failure. Descendants may hold stdout open after the parent dies.
                if (!exit_observed) exit_observed = now;
                if (now - *exit_observed >= std::chrono::milliseconds(20))
                    throw GtpFailure("unexpected_exit", "", "Engine process exited before its response completed");
            }
            if (now >= deadline) {
                if (exit_observed) throw GtpFailure("unexpected_exit", "", "Engine exited with an incomplete response");
                throw GtpFailure("timeout", "", "Command response deadline expired");
            }
            output_ready_.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(10)),
                [this] { return !pending_.empty() || output_done_ || overflow_; });
        }
    }
    std::string stderr_text() const override { std::lock_guard lock(error_mutex_); return diagnostics_; }
    Json metadata() const override {
        DWORD exit_code = STILL_ACTIVE;
        const bool available = process_.get() && GetExitCodeProcess(process_.get(), &exit_code);
        std::lock_guard lock(error_mutex_);
        return {{"process_id", pid_}, {"exit_code", available && exit_code != STILL_ACTIVE ? Json(exit_code) : Json(nullptr)},
            {"stderr_retained_bytes", diagnostics_.size()}, {"stderr_discarded_bytes", discarded_},
            {"process_transport", "Windows CreateProcessW; hidden; owned job object"}};
    }
    std::optional<std::string> close(Deadline deadline) noexcept override {
        if (closed_) return {};
        closed_ = true;
        std::optional<std::string> warning;
        try {
            input_.reset();
            const auto status = process_.get() ? WaitForSingleObject(process_.get(), remaining(deadline)) : WAIT_OBJECT_0;
            if (status != WAIT_OBJECT_0) {
                warning = status == WAIT_TIMEOUT ? "Shutdown deadline expired; terminated engine process tree" : windows_error("Wait for engine exit");
                if (job_.get()) TerminateJobObject(job_.get(), 1);
            } else if (process_.get()) {
                DWORD exit_code = 0;
                if (GetExitCodeProcess(process_.get(), &exit_code) && exit_code != 0)
                    warning = "Engine exited with status " + std::to_string(exit_code) + " during cleanup";
            }
            // Closing the job also kills descendants left after an otherwise normal parent exit.
            job_.reset();
            stopping_ = true;
            if (output_reader_.joinable()) CancelSynchronousIo(output_reader_.native_handle());
            if (error_reader_.joinable()) CancelSynchronousIo(error_reader_.native_handle());
            if (output_reader_.joinable()) output_reader_.join();
            if (error_reader_.joinable()) error_reader_.join();
            output_.reset(); error_.reset();
        } catch (...) {
            // No caller should lose a recorded result because cleanup diagnostics failed.
            // RAII job ownership still guarantees process tree termination.
            job_.reset();
        }
        return warning;
    }
private:
    static constexpr std::size_t response_bound = 1024 * 1024;
    static constexpr std::size_t stderr_bound = 64 * 1024;
    Handle input_, output_, error_, process_, job_;
    DWORD pid_ = 0;
    std::thread output_reader_, error_reader_;
    std::atomic<bool> stopping_{false};
    bool closed_ = false;
    mutable std::mutex output_mutex_, error_mutex_;
    std::condition_variable output_ready_;
    std::string pending_, output_error_, diagnostics_;
    bool output_done_ = false, overflow_ = false;
    std::uint64_t discarded_ = 0;
    void drain_output() {
        char buffer[8192];
        while (!stopping_) {
            DWORD count = 0;
            if (!ReadFile(output_.get(), buffer, sizeof(buffer), &count, nullptr) || !count) {
                const auto error = GetLastError();
                std::lock_guard lock(output_mutex_);
                if (error != ERROR_BROKEN_PIPE && error != ERROR_OPERATION_ABORTED && error != ERROR_SUCCESS)
                    output_error_ = windows_error("Read engine stdout", error);
                break;
            }
            {
                std::lock_guard lock(output_mutex_);
                if (pending_.size() + count > response_bound) overflow_ = true;
                if (!overflow_) pending_.append(buffer, count);
            }
            output_ready_.notify_all();
        }
        { std::lock_guard lock(output_mutex_); output_done_ = true; }
        output_ready_.notify_all();
    }
    void drain_error() {
        char buffer[8192];
        while (!stopping_) {
            DWORD count = 0;
            if (!ReadFile(error_.get(), buffer, sizeof(buffer), &count, nullptr) || !count) break;
            std::lock_guard lock(error_mutex_);
            const auto overflow = diagnostics_.size() + count > stderr_bound ? diagnostics_.size() + count - stderr_bound : 0;
            if (overflow) { diagnostics_.erase(0, overflow); discarded_ += overflow; }
            diagnostics_.append(buffer, count);
        }
    }
};
} // namespace

std::unique_ptr<GtpProcess> make_gtp_process(const std::filesystem::path& executable,
    const std::vector<std::string>& arguments, const std::filesystem::path& working_directory) {
    auto process = std::make_unique<WindowsGtpProcess>(executable, arguments, working_directory);
    process->start_drains();
    return process;
}
#else
std::unique_ptr<GtpProcess> make_gtp_process(const std::filesystem::path&,
    const std::vector<std::string>&, const std::filesystem::path&) {
    throw GtpFailure("launch_failure", "", "External GTP process launching is supported on Windows only");
}
#endif
} // namespace betago
