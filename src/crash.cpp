#include "crash.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <shlobj.h>

#include <cstdio>
#include <cstring>
#include <cwchar>

#include "island_version.hpp"

#pragma comment(lib, "dbghelp.lib")

namespace island {
namespace {

wchar_t g_dir[MAX_PATH] = {};   // %APPDATA%\IslandCapsule, filled once at install time (no allocation in the handler)
volatile LONG g_in_handler = 0;

void AppendLine(HANDLE f, const char* text) {
  DWORD n = 0;
  WriteFile(f, text, static_cast<DWORD>(strlen(text)), &n, nullptr);
}

// "island_loader.exe+0x2ab46" for an address inside a loaded module.
void DescribeAddress(void* addr, char* out, size_t cap) {
  HMODULE mod = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCWSTR>(addr), &mod) && mod) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(mod, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    char narrow[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
    snprintf(out, cap, "%s+0x%llx", narrow,
             static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(addr) - reinterpret_cast<ULONG_PTR>(mod)));
  } else {
    snprintf(out, cap, "0x%llx", static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(addr)));
  }
}

LONG WINAPI OnCrash(EXCEPTION_POINTERS* ep) {
  if (InterlockedExchange(&g_in_handler, 1) != 0) return EXCEPTION_CONTINUE_SEARCH;   // crash inside the handler

  wchar_t log_path[MAX_PATH], dmp_path[MAX_PATH];
  swprintf(log_path, MAX_PATH, L"%ls\\crash.log", g_dir);
  swprintf(dmp_path, MAX_PATH, L"%ls\\crash.dmp", g_dir);

  HANDLE f = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    char line[512], where[256];
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(line, sizeof(line), "=== crash %04d-%02d-%02d %02d:%02d:%02d  island_loader %ls  thread %lu ===\r\n", st.wYear,
             st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, ISLAND_VERSION_W, GetCurrentThreadId());
    AppendLine(f, line);

    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    DescribeAddress(er->ExceptionAddress, where, sizeof(where));
    snprintf(line, sizeof(line), "exception 0x%08lx at %s\r\n", er->ExceptionCode, where);
    AppendLine(f, line);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
      snprintf(line, sizeof(line), "access violation: %s address 0x%llx\r\n",
               er->ExceptionInformation[0] == 0 ? "reading" : (er->ExceptionInformation[0] == 1 ? "writing" : "executing"),
               static_cast<unsigned long long>(er->ExceptionInformation[1]));
      AppendLine(f, line);
    }

    void* frames[48] = {};
    const USHORT n = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
    AppendLine(f, "stack (RVAs, innermost first):\r\n");
    for (USHORT i = 0; i < n; ++i) {
      DescribeAddress(frames[i], where, sizeof(where));
      snprintf(line, sizeof(line), "  #%02u %s\r\n", i, where);
      AppendLine(f, line);
    }
    AppendLine(f, "\r\n");
    CloseHandle(f);
  }

  HANDLE d = CreateFileW(dmp_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (d != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), d, MiniDumpNormal, &mei, nullptr, nullptr);
    CloseHandle(d);
  }
  return EXCEPTION_CONTINUE_SEARCH;   // then the normal Windows crash handling (Event Viewer entry) still happens
}

}  // namespace

void InstallCrashHandler() {
  PWSTR appdata = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata)) && appdata) {
    swprintf(g_dir, MAX_PATH, L"%ls\\IslandCapsule", appdata);
    CoTaskMemFree(appdata);
    CreateDirectoryW(g_dir, nullptr);
  }
  SetUnhandledExceptionFilter(OnCrash);
}

}  // namespace island
