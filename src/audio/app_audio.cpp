#include "audio/app_audio.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <unordered_map>

namespace island {

using Microsoft::WRL::ComPtr;

std::wstring LowerW(std::wstring s) {
  std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  return s;
}

std::wstring ExeStem(unsigned long pid) {
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (!h) return {};
  wchar_t buf[MAX_PATH] = {};
  DWORD n = MAX_PATH;
  std::wstring out;
  if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
    std::wstring p(buf, n);
    const size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p.erase(0, slash + 1);
    const size_t dot = p.find_last_of(L'.');
    if (dot != std::wstring::npos) p.erase(dot);
    out = LowerW(p);
  }
  CloseHandle(h);
  return out;
}

bool AppMatchesExe(const std::wstring& app_lower, const std::wstring& stem) {
  if (stem.size() < 3 || app_lower.empty()) return false;
  if (app_lower.find(stem) != std::wstring::npos) return true;
  std::wstring bare = app_lower;
  if (bare.size() > 4 && bare.compare(bare.size() - 4, 4, L".exe") == 0) bare.erase(bare.size() - 4);
  return bare.size() >= 3 && stem.find(bare) != std::wstring::npos;
}

namespace {

struct Proc {
  DWORD ppid = 0;
  std::wstring stem;   // lower-case exe name without ".exe"
};

// Process ids that currently own an audio session on the default playback device. `active` is true for the ones
// that are playing right now.
std::map<DWORD, bool> SessionPids() {
  std::map<DWORD, bool> out;
  ComPtr<IMMDeviceEnumerator> en;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(en.GetAddressOf()))))
    return out;
  ComPtr<IMMDevice> dev;
  if (FAILED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, dev.GetAddressOf()))) return out;
  ComPtr<IAudioSessionManager2> mgr;
  if (FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(mgr.GetAddressOf()))))
    return out;
  ComPtr<IAudioSessionEnumerator> sessions;
  int count = 0;
  if (FAILED(mgr->GetSessionEnumerator(sessions.GetAddressOf())) || FAILED(sessions->GetCount(&count))) return out;
  for (int i = 0; i < count; ++i) {
    ComPtr<IAudioSessionControl> ctl;
    if (FAILED(sessions->GetSession(i, ctl.GetAddressOf())) || !ctl) continue;
    ComPtr<IAudioSessionControl2> ctl2;
    if (FAILED(ctl.As(&ctl2)) || ctl2->IsSystemSoundsSession() == S_OK) continue;
    AudioSessionState state = AudioSessionStateInactive;
    ctl->GetState(&state);
    if (state == AudioSessionStateExpired) continue;
    DWORD pid = 0;
    if (FAILED(ctl2->GetProcessId(&pid)) || pid == 0) continue;
    out[pid] = out[pid] || state == AudioSessionStateActive;
  }
  return out;
}

} // namespace

unsigned long FindAppRootPid(const std::wstring& source_app) {
  const std::wstring app = LowerW(source_app);
  if (app.empty()) return 0;

  std::unordered_map<DWORD, Proc> procs;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W e{};
  e.dwSize = sizeof(e);
  for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
    std::wstring stem = LowerW(e.szExeFile);   // "Spotify.exe"
    const size_t dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos) stem.erase(dot);
    procs[e.th32ProcessID] = Proc{ e.th32ParentProcessID, std::move(stem) };
  }
  CloseHandle(snap);

  // Climb to the top of the chain of parents that run the same executable (a browser's helper -> its main process).
  auto root_of = [&](DWORD pid) {
    for (int guard = 0; guard < 32; ++guard) {
      const auto it = procs.find(pid);
      if (it == procs.end() || it->second.ppid == pid) break;
      const auto parent = procs.find(it->second.ppid);
      if (parent == procs.end() || parent->second.stem != it->second.stem) break;
      pid = it->second.ppid;
    }
    return pid;
  };

  // 1) A matching process that really owns an audio session (the one that is playing wins).
  DWORD idle_hit = 0;
  for (const auto& [pid, active] : SessionPids()) {
    const auto it = procs.find(pid);
    if (it == procs.end() || !AppMatchesExe(app, it->second.stem)) continue;
    if (active) return root_of(pid);
    if (!idle_hit) idle_hit = pid;
  }
  if (idle_hit) return root_of(idle_hit);

  // 2) No session yet: the matching process tree with the most processes.
  std::map<DWORD, int> trees;
  for (const auto& [pid, p] : procs)
    if (AppMatchesExe(app, p.stem)) ++trees[root_of(pid)];
  DWORD best = 0;
  int best_n = 0;
  for (const auto& [root, n] : trees)
    if (n > best_n) { best = root; best_n = n; }
  return best;
}

} // namespace island
