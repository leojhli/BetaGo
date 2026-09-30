#include "betago/tk.hpp"
#include <bit>
#include <stdexcept>
#include <vector>

namespace betago {
std::filesystem::path executable_directory() {
    std::vector<wchar_t> buffer(32768);
    auto count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!count || count == buffer.size()) throw std::runtime_error("Cannot locate executable");
    return std::filesystem::path(std::wstring(buffer.data(), count)).parent_path();
}

std::string tcl_quote(const std::string& text) {
    std::string result = "\"";
    for (char ch : text) {
        if (ch == '\\' || ch == '"' || ch == '$' || ch == '[' || ch == ']') result += '\\';
        if (ch == '\n') result += "\\n";
        else if (ch == '\r') result += "\\r";
        else result += ch;
    }
    return result + '"';
}

template<class Function> Function symbol(HMODULE module, const char* name) {
    auto address = GetProcAddress(module, name);
    if (!address) throw std::runtime_error(std::string("Missing Tk runtime function: ") + name);
    // Windows exports use the declared C calling convention; both pointer
    // representations have the same size on this platform.
    return std::bit_cast<Function>(address);
}

TkRuntime::TkRuntime() {
    auto runtime = executable_directory() / "runtime";
    tcl_ = LoadLibraryExW((runtime / "tcl86t.dll").c_str(), nullptr,
                         LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!tcl_) throw std::runtime_error("Cannot load runtime/tcl86t.dll (Windows error " + std::to_string(GetLastError()) + "). Run build.ps1 to bundle Tk and its dependencies.");
    try {
        tk_ = LoadLibraryExW((runtime / "tk86t.dll").c_str(), nullptr,
                            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!tk_) throw std::runtime_error("Cannot load runtime/tk86t.dll (Windows error " + std::to_string(GetLastError()) + ")");
#define LOAD(field, name) field = symbol<decltype(field)>(tcl_, name)
        LOAD(create_interp_, "Tcl_CreateInterp");
        LOAD(delete_interp_, "Tcl_DeleteInterp");
        LOAD(find_executable_, "Tcl_FindExecutable");
        LOAD(init_, "Tcl_Init");
        LOAD(eval_, "Tcl_EvalEx");
        LOAD(result_, "Tcl_GetStringResult");
        LOAD(set_var_, "Tcl_SetVar");
        LOAD(create_command_, "Tcl_CreateObjCommand");
        LOAD(get_string_, "Tcl_GetString");
        LOAD(new_string_, "Tcl_NewStringObj");
        LOAD(set_result_, "Tcl_SetObjResult");
        LOAD(do_event_, "Tcl_DoOneEvent");
#undef LOAD
        tk_init_ = symbol<decltype(tk_init_)>(tk_, "Tk_Init");
        num_windows_ = symbol<decltype(num_windows_)>(tk_, "Tk_GetNumMainWindows");
        auto executable = (executable_directory() / "play.exe").string();
        find_executable_(executable.c_str());
        interp_ = create_interp_();
        if (!interp_) throw std::runtime_error("Cannot create Tcl interpreter");
        auto tcl_library = (runtime / "tcl8.6").generic_string();
        auto tk_library = (runtime / "tk8.6").generic_string();
        set_var_(interp_, "env(TCL_LIBRARY)", tcl_library.c_str(), TCL_GLOBAL_ONLY);
        set_var_(interp_, "env(TK_LIBRARY)", tk_library.c_str(), TCL_GLOBAL_ONLY);
        if (init_(interp_) != TCL_OK || tk_init_(interp_) != TCL_OK)
            throw std::runtime_error(result_(interp_));
    } catch (...) {
        if (interp_) delete_interp_(interp_);
        if (tk_) FreeLibrary(tk_);
        FreeLibrary(tcl_);
        throw;
    }
}

TkRuntime::~TkRuntime() {
    if (interp_) delete_interp_(interp_);
    // Tcl/Tk keep process-wide state. Leave their modules loaded until exit.
}

std::string TkRuntime::eval(const std::string& script) {
    if (eval_(interp_, script.c_str(), static_cast<int>(script.size()), TCL_EVAL_GLOBAL) != TCL_OK)
        throw std::runtime_error(result_(interp_));
    return result_(interp_);
}
void TkRuntime::command(const char* name, Tcl_ObjCmdProc* callback, void* data) {
    create_command_(interp_, name, callback, data, nullptr);
}
std::string TkRuntime::text(Tcl_Obj* object) const { return get_string_(object); }
void TkRuntime::set_result(const std::string& text) {
    set_result_(interp_, new_string_(text.data(), static_cast<int>(text.size())));
}
void TkRuntime::loop() { while (num_windows_() > 0) do_event_(0); }
} // namespace betago
