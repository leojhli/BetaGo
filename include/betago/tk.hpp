#pragma once
#ifndef _WIN32
#error The visual board currently targets Windows; the rules and runner are portable.
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tcl.h>
#include <filesystem>
#include <string>

namespace betago {
std::string tcl_quote(const std::string& text);
std::filesystem::path executable_directory();

// Load the Tk 8.6 runtime directly through its C API.
class TkRuntime {
public:
    TkRuntime();
    ~TkRuntime();
    TkRuntime(const TkRuntime&) = delete;
    TkRuntime& operator=(const TkRuntime&) = delete;
    std::string eval(const std::string& script);
    void command(const char* name, Tcl_ObjCmdProc* callback, void* data);
    std::string text(Tcl_Obj* object) const;
    void set_result(const std::string& text);
    void loop();
private:
    HMODULE tcl_ = nullptr, tk_ = nullptr;
    Tcl_Interp* interp_ = nullptr;
    decltype(&Tcl_CreateInterp) create_interp_;
    decltype(&Tcl_DeleteInterp) delete_interp_;
    decltype(&Tcl_FindExecutable) find_executable_;
    decltype(&Tcl_Init) init_;
    decltype(&Tcl_EvalEx) eval_;
    decltype(&Tcl_GetStringResult) result_;
    decltype(&Tcl_SetVar) set_var_;
    decltype(&Tcl_CreateObjCommand) create_command_;
    decltype(&Tcl_GetString) get_string_;
    decltype(&Tcl_NewStringObj) new_string_;
    decltype(&Tcl_SetObjResult) set_result_;
    decltype(&Tcl_DoOneEvent) do_event_;
    int (*tk_init_)(Tcl_Interp*);
    int (*num_windows_)();
};
} // namespace betago
