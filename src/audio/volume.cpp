#include "audio/volume.hpp"

#include "audio/app_audio.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audiopolicy.h>
#include <wrl/client.h>

#include <algorithm>
#include <vector>

#pragma comment(lib, "ole32.lib")

namespace island {

using Microsoft::WRL::ComPtr;

struct SystemVolume::Impl {
  bool com_inited = false;
  ComPtr<IAudioEndpointVolume> ep;
  ULONGLONG acquired_at = 0;

  ~Impl() {
    ep.Reset();                       // release before CoUninitialize
    if (com_inited) CoUninitialize();
  }

  bool EnsureCom() {
    if (com_inited) return true;
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) com_inited = true;            // S_OK / S_FALSE: balance with CoUninitialize
    else if (hr != RPC_E_CHANGED_MODE) return false; // already initialised differently: just use it
    return true;
  }

  bool DefaultDevice(ComPtr<IMMDevice>& dev) {
    if (!EnsureCom()) return false;
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(en.GetAddressOf()))))
      return false;
    return SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, dev.GetAddressOf()));
  }

  bool Acquire() {
    ep.Reset();
    ComPtr<IMMDevice> dev;
    if (!DefaultDevice(dev)) return false;
    if (FAILED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                             reinterpret_cast<void**>(ep.GetAddressOf()))))
      return false;
    acquired_at = GetTickCount64();
    return true;
  }
};

SystemVolume::SystemVolume() : impl_(std::make_unique<Impl>()) {}
SystemVolume::~SystemVolume() = default;

bool SystemVolume::Nudge(float delta, float* level, bool* muted) {
  Impl& d = *impl_;
  // Re-acquire now and then so a changed default device (headphones plugged in) is followed.
  if (!d.ep || GetTickCount64() - d.acquired_at > 5000) {
    if (!d.Acquire()) return false;
  }

  float cur = 0.0f;
  if (FAILED(d.ep->GetMasterVolumeLevelScalar(&cur))) {
    if (!d.Acquire() || FAILED(d.ep->GetMasterVolumeLevelScalar(&cur))) return false;
  }
  BOOL mute = FALSE;
  d.ep->GetMute(&mute);

  if (delta > 0.0f && mute) {
    d.ep->SetMute(FALSE, nullptr);
    mute = FALSE;
  }
  const float next = (std::clamp)(cur + delta, 0.0f, 1.0f);
  if (next != cur) d.ep->SetMasterVolumeLevelScalar(next, nullptr);

  if (level) *level = next;
  if (muted) *muted = mute != FALSE;
  return true;
}

bool SystemVolume::NudgeApp(const std::wstring& source_app, float delta, float* level, bool* muted) {
  Impl& d = *impl_;
  const std::wstring app = LowerW(source_app);
  if (app.empty()) return false;

  // Audio sessions belong to the default playback device. They are enumerated afresh on every call (a wheel
  // notch is rare enough), which also follows apps that start or stop playing and device changes.
  ComPtr<IMMDevice> dev;
  if (!d.DefaultDevice(dev)) return false;
  ComPtr<IAudioSessionManager2> mgr;
  if (FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(mgr.GetAddressOf()))))
    return false;
  ComPtr<IAudioSessionEnumerator> sessions;
  if (FAILED(mgr->GetSessionEnumerator(sessions.GetAddressOf()))) return false;
  int count = 0;
  if (FAILED(sessions->GetCount(&count))) return false;

  struct Hit {
    ComPtr<ISimpleAudioVolume> vol;
    bool active = false;
  };
  std::vector<Hit> hits;
  for (int i = 0; i < count; ++i) {
    ComPtr<IAudioSessionControl> ctl;
    if (FAILED(sessions->GetSession(i, ctl.GetAddressOf())) || !ctl) continue;
    ComPtr<IAudioSessionControl2> ctl2;
    if (FAILED(ctl.As(&ctl2))) continue;
    if (ctl2->IsSystemSoundsSession() == S_OK) continue;

    AudioSessionState state = AudioSessionStateInactive;
    ctl->GetState(&state);
    if (state == AudioSessionStateExpired) continue;

    DWORD pid = 0;
    if (FAILED(ctl2->GetProcessId(&pid)) || pid == 0) continue;
    if (!AppMatchesExe(app, ExeStem(pid))) continue;

    Hit h;
    if (FAILED(ctl.As(&h.vol)) || !h.vol) continue;
    h.active = (state == AudioSessionStateActive);
    hits.push_back(std::move(h));
  }
  if (hits.empty()) return false;

  // A browser has one session per audio-producing process: when some are actually playing, leave the idle ones
  // (other tabs) alone so their old level cannot drag the new value around.
  const bool any_active = std::any_of(hits.begin(), hits.end(), [](const Hit& h) { return h.active; });
  if (any_active) hits.erase(std::remove_if(hits.begin(), hits.end(), [](const Hit& h) { return !h.active; }), hits.end());

  float cur = 0.0f;
  bool all_muted = true;
  for (const Hit& h : hits) {
    float v = 0.0f;
    if (SUCCEEDED(h.vol->GetMasterVolume(&v))) cur = (std::max)(cur, v);
    BOOL m = FALSE;
    h.vol->GetMute(&m);
    if (!m) all_muted = false;
  }

  const bool unmute = delta > 0.0f && all_muted;
  const float next = (std::clamp)(cur + delta, 0.0f, 1.0f);
  for (const Hit& h : hits) {
    if (unmute) h.vol->SetMute(FALSE, nullptr);
    h.vol->SetMasterVolume(next, nullptr);
  }

  if (level) *level = next;
  if (muted) *muted = all_muted && !unmute;
  return true;
}

} // namespace island
