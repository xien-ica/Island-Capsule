#include "audio/volume.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <wrl/client.h>

#include <algorithm>

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

  bool Acquire() {
    ep.Reset();
    if (!com_inited) {
      const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
      if (SUCCEEDED(hr)) com_inited = true;            // S_OK / S_FALSE: balance with CoUninitialize
      else if (hr != RPC_E_CHANGED_MODE) return false; // already initialised differently: just use it
    }
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(en.GetAddressOf()))))
      return false;
    ComPtr<IMMDevice> dev;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, dev.GetAddressOf()))) return false;
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

} // namespace island
