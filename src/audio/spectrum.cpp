#include "audio/spectrum.hpp"

#include "audio/app_audio.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#if __has_include(<audioclientactivationparams.h>)
#include <audioclientactivationparams.h>
#else
// Older Windows SDKs: the process-loopback activation parameters (documented layout, Windows 10 build 20348+).
typedef enum {
  AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT = 0,
  AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1,
} AUDIOCLIENT_ACTIVATION_TYPE;
typedef enum {
  PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0,
  PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE = 1,
} PROCESS_LOOPBACK_MODE;
typedef struct {
  DWORD TargetProcessId;
  PROCESS_LOOPBACK_MODE ProcessLoopbackMode;
} AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS;
typedef struct {
  AUDIOCLIENT_ACTIVATION_TYPE ActivationType;
  union {
    AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams;
  };
} AUDIOCLIENT_ACTIVATION_PARAMS;
#endif
#ifndef VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
#define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mmdevapi.lib")

namespace island {

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kN = 2048;                 // FFT size (~43 ms at 48 kHz, ~23 Hz per bin: enough for the bass band)
constexpr double kHopSec = 0.016;        // a new spectrum roughly every 16 ms
constexpr DWORD kPollMs = 15;            // capture poll interval
constexpr DWORD kRetryMs = 2000;         // retry delay after the device could not be opened
constexpr ULONGLONG kDeviceCheckMs = 3000;   // follow a changed default playback device

// Band edges in Hz (bass .. treble).
constexpr float kEdges[AudioSpectrum::kBands + 1] = { 40.0f, 120.0f, 300.0f, 800.0f, 2000.0f, 5000.0f, 14000.0f };

// Level mapping. The loudest band is always pulled to ~1 (auto gain), the rest sit up to kRangeDb below it, so the
// look does not depend on the song's loudness or the system volume. kRefFloorDb stops silence / dither noise
// from being amplified into a full-scale display.
constexpr float kRangeDb = 36.0f;
constexpr float kRefFloorDb = -50.0f;
constexpr float kRefDecayDbPerSec = 5.0f;
constexpr float kCurve = 1.35f;          // > 1: more contrast between loud and quiet bands

constexpr float kPi = 3.14159265358979f;

// Radix-2 complex FFT, in place.
struct Fft {
  int n = 0;
  std::vector<int> rev;
  std::vector<float> cs, sn;

  void Init(int size) {
    n = size;
    int bits = 0;
    while ((1 << bits) < n) ++bits;
    rev.assign(n, 0);
    for (int i = 1; i < n; ++i) rev[i] = (rev[i >> 1] >> 1) | ((i & 1) << (bits - 1));
    cs.resize(n / 2);
    sn.resize(n / 2);
    for (int k = 0; k < n / 2; ++k) {
      cs[k] = std::cos(2.0f * kPi * static_cast<float>(k) / static_cast<float>(n));
      sn[k] = -std::sin(2.0f * kPi * static_cast<float>(k) / static_cast<float>(n));
    }
  }

  void Run(float* re, float* im) const {
    for (int i = 0; i < n; ++i) {
      const int j = rev[i];
      if (j > i) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= n; len <<= 1) {
      const int half = len >> 1, step = n / len;
      for (int i = 0; i < n; i += len) {
        for (int k = 0; k < half; ++k) {
          const float wr = cs[k * step], wi = sn[k * step];
          const int a = i + k, b = a + half;
          const float vr = re[b] * wr - im[b] * wi;
          const float vi = re[b] * wi + im[b] * wr;
          re[b] = re[a] - vr; im[b] = im[a] - vi;
          re[a] += vr;        im[a] += vi;
        }
      }
    }
  }
};

// Collects mono samples and turns the latest kN of them into kBands levels.
struct Analyzer {
  Fft fft;
  std::vector<float> window, ring, re, im;
  float window_sum = 1.0f;
  size_t wpos = 0;
  size_t pending = 0;        // samples pushed since the last analysis
  float rate = 48000.0f;
  float ref_db = kRefFloorDb;

  Analyzer() {
    fft.Init(kN);
    window.resize(kN);
    double sum = 0.0;
    for (int i = 0; i < kN; ++i) {
      window[i] = 0.5f - 0.5f * std::cos(2.0f * kPi * static_cast<float>(i) / static_cast<float>(kN - 1));   // Hann
      sum += window[i];
    }
    window_sum = static_cast<float>(sum);
    ring.assign(kN, 0.0f);
    re.assign(kN, 0.0f);
    im.assign(kN, 0.0f);
  }

  void Reset(float sample_rate) {
    rate = sample_rate > 8000.0f ? sample_rate : 48000.0f;
    std::fill(ring.begin(), ring.end(), 0.0f);
    wpos = 0;
    pending = 0;
    ref_db = kRefFloorDb;
  }

  void Push(const float* mono, size_t count) {
    for (size_t i = 0; i < count; ++i) {
      ring[wpos] = mono[i];
      wpos = (wpos + 1) % kN;
    }
    pending += count;
  }

  void PushSilence(size_t count) {
    count = (std::min)(count, static_cast<size_t>(kN));
    for (size_t i = 0; i < count; ++i) {
      ring[wpos] = 0.0f;
      wpos = (wpos + 1) % kN;
    }
    pending += count;
  }

  bool Ready() const { return static_cast<double>(pending) >= kHopSec * rate; }

  void Analyze(float (&out)[AudioSpectrum::kBands]) {
    const float dt = static_cast<float>(static_cast<double>(pending) / rate);
    pending = 0;

    // oldest sample first, windowed
    for (int i = 0; i < kN; ++i) {
      re[i] = ring[(wpos + static_cast<size_t>(i)) % kN] * window[i];
      im[i] = 0.0f;
    }
    fft.Run(re.data(), im.data());

    const float bin_hz = rate / static_cast<float>(kN);
    const int max_bin = kN / 2 - 1;
    const float amp_norm = 2.0f / window_sum;   // a full-scale sine -> amplitude 1.0 (0 dB)

    float db[AudioSpectrum::kBands];
    float cur_max = -200.0f;
    for (int b = 0; b < AudioSpectrum::kBands; ++b) {
      int lo = (std::max)(1, static_cast<int>(std::ceil(kEdges[b] / bin_hz)));
      int hi = (std::min)(max_bin, static_cast<int>(std::floor(kEdges[b + 1] / bin_hz)));
      if (hi < lo) hi = lo;
      if (lo > max_bin) lo = hi = max_bin;
      double p = 0.0;
      for (int k = lo; k <= hi; ++k) {
        const float a = std::sqrt(re[k] * re[k] + im[k] * im[k]) * amp_norm;
        p += static_cast<double>(a) * a;
      }
      const float rms = static_cast<float>(std::sqrt(p / static_cast<double>(hi - lo + 1)));
      // Music falls off ~3 dB per octave: tilt up so the treble bars are not always the shortest.
      const float fc = std::sqrt(kEdges[b] * kEdges[b + 1]);
      const float tilt = std::sqrt(fc / 1000.0f);
      db[b] = 20.0f * std::log10(rms * tilt + 1e-9f);
      cur_max = (std::max)(cur_max, db[b]);
    }

    if (cur_max > ref_db) ref_db += (cur_max - ref_db) * 0.4f;     // fast attack
    else ref_db -= kRefDecayDbPerSec * dt;                          // slow release
    ref_db = (std::max)(ref_db, kRefFloorDb);

    for (int b = 0; b < AudioSpectrum::kBands; ++b) {
      const float v = (std::clamp)((db[b] - (ref_db - kRangeDb)) / kRangeDb, 0.0f, 1.0f);
      out[b] = std::pow(v, kCurve);
    }
  }
};

// Per-process loopback exists from Windows 10 build 20348 (Windows 11 / Server 2022) on.
bool ProcessLoopbackSupported() {
  static const bool ok = [] {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    auto fn = nt ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(nt, "RtlGetVersion")) : nullptr;
    OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (!fn || fn(&vi) != 0) return true;   // cannot tell: try it
    return vi.dwBuildNumber >= 20348;
  }();
  return ok;
}

// Completion handler for ActivateAudioInterfaceAsync. It must be agile (FtmBase), the callback arrives on a
// system thread; the capture thread just waits on `done`.
class ActivationHandler
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          Microsoft::WRL::FtmBase, IActivateAudioInterfaceCompletionHandler> {
public:
  HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ~ActivationHandler() { if (done) CloseHandle(done); }
  STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation*) override {
    SetEvent(done);
    return S_OK;
  }
};

// One open loopback stream: either the whole default playback device (system mix) or, on Windows 10 build 20348+ /
// Windows 11, only one process tree (the app that owns the current media).
struct Capture {
  ComPtr<IAudioClient> client;
  ComPtr<IAudioCaptureClient> cap;
  std::wstring dev_id;
  bool is_float = true;
  int channels = 2;
  int bits = 32;
  ULONGLONG checked_at = 0;
  std::vector<float> mono;
  HANDLE evt = nullptr;          // event-callback handle of a process-loopback stream (polled, never waited on)
  DWORD target_pid = 0;          // process tree being captured (process mode)
  bool process_mode = false;
  std::wstring note;             // what the last Open() did / why it failed (shown in the app's log)

  bool IsOpen() const { return cap != nullptr; }

  void Close() {
    if (client) client->Stop();
    cap.Reset();
    client.Reset();
    dev_id.clear();
    if (evt) { CloseHandle(evt); evt = nullptr; }
    target_pid = 0;
    process_mode = false;
  }

  static bool DefaultDeviceId(std::wstring* id, ComPtr<IMMDevice>* dev_out) {
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(en.GetAddressOf()))))
      return false;
    ComPtr<IMMDevice> dev;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, dev.GetAddressOf()))) return false;
    LPWSTR raw = nullptr;
    if (FAILED(dev->GetId(&raw)) || !raw) return false;
    *id = raw;
    CoTaskMemFree(raw);
    if (dev_out) *dev_out = dev;
    return true;
  }

  bool OpenSystem(Analyzer& an) {
    Close();
    ComPtr<IMMDevice> dev;
    std::wstring id;
    if (!DefaultDeviceId(&id, &dev)) return false;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()))))
      return false;

    WAVEFORMATEX* fmt = nullptr;
    if (FAILED(client->GetMixFormat(&fmt)) || !fmt) { Close(); return false; }
    // The shared-mode mix format is 32-bit float on practically every system; 16-bit PCM is handled as well.
    bool ok = false;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22) {
      const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
      if (ext->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT && fmt->wBitsPerSample == 32) { is_float = true; ok = true; }
      else if (ext->SubFormat.Data1 == WAVE_FORMAT_PCM && fmt->wBitsPerSample == 16) { is_float = false; ok = true; }
    } else if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && fmt->wBitsPerSample == 32) {
      is_float = true; ok = true;
    } else if (fmt->wFormatTag == WAVE_FORMAT_PCM && fmt->wBitsPerSample == 16) {
      is_float = false; ok = true;
    }
    channels = (std::max)(1, static_cast<int>(fmt->nChannels));
    bits = fmt->wBitsPerSample;
    const float rate = static_cast<float>(fmt->nSamplesPerSec);
    if (!ok) { CoTaskMemFree(fmt); Close(); return false; }

    const HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                          1000000 /* 100 ms */, 0, fmt, nullptr);
    CoTaskMemFree(fmt);
    if (FAILED(hr)) { Close(); return false; }
    if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(cap.GetAddressOf())))) {
      Close();
      return false;
    }
    if (FAILED(client->Start())) { Close(); return false; }

    dev_id = id;
    checked_at = GetTickCount64();
    an.Reset(rate);
    return true;
  }

  // Activates the virtual process-loopback device for `pid`; on success `client` is an UNINITIALISED IAudioClient.
  HRESULT ActivateProcessClient(DWORD pid) {
    client.Reset();
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT pv{};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(params);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    auto handler = Microsoft::WRL::Make<ActivationHandler>();
    if (!handler || !handler->done) return E_OUTOFMEMORY;
    ComPtr<IActivateAudioInterfaceAsyncOperation> op;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv,
                                             handler.Get(), op.GetAddressOf());
    if (FAILED(hr)) return hr;
    if (WaitForSingleObject(handler->done, 3000) != WAIT_OBJECT_0) return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    HRESULT activated = E_FAIL;
    ComPtr<IUnknown> unk;
    hr = op->GetActivateResult(&activated, unk.GetAddressOf());
    if (FAILED(hr)) return hr;
    if (FAILED(activated)) return activated;
    if (!unk) return E_POINTER;
    return unk.As(&client);
  }

  // Captures only the process tree rooted at `pid`. The virtual device has no mix format, so one is chosen here.
  // The first variant is what Windows' own ApplicationLoopback sample uses; the others are tried when the stream
  // refuses it (each attempt needs a freshly activated client).
  bool OpenProcessLoopback(Analyzer& an, DWORD pid) {
    Close();
    struct Variant {
      bool use_float;
      DWORD rate;
      DWORD flags;
      REFERENCE_TIME duration;
    };
    constexpr DWORD kBase = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST;
    constexpr DWORD kConv = kBase | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    static constexpr Variant kVariants[] = {
      { false, 44100, kBase, 200000 },
      { false, 44100, kBase, 0 },
      { false, 44100, kConv, 0 },
      { true, 48000, kConv, 0 },
    };

    HRESULT hr = E_FAIL;
    const wchar_t* stage = L"activate";
    for (size_t i = 0; i < sizeof(kVariants) / sizeof(kVariants[0]); ++i) {
      const Variant& v = kVariants[i];
      stage = L"activate";
      hr = ActivateProcessClient(pid);
      if (FAILED(hr)) break;                 // bad process / not supported: another format will not help

      WAVEFORMATEX wf{};
      wf.wFormatTag = v.use_float ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
      wf.nChannels = 2;
      wf.nSamplesPerSec = v.rate;
      wf.wBitsPerSample = v.use_float ? 32 : 16;
      wf.nBlockAlign = static_cast<WORD>(wf.nChannels * wf.wBitsPerSample / 8);
      wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;

      stage = L"initialize";
      hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, v.flags, v.duration, 0, &wf, nullptr);
      if (FAILED(hr)) { Close(); continue; }
      stage = L"event handle";
      evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      hr = evt ? client->SetEventHandle(evt) : HRESULT_FROM_WIN32(GetLastError());
      if (FAILED(hr)) { Close(); continue; }
      stage = L"capture service";
      hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(cap.GetAddressOf()));
      if (FAILED(hr)) { Close(); continue; }
      stage = L"start";
      hr = client->Start();
      if (FAILED(hr)) { Close(); continue; }

      is_float = v.use_float;
      channels = 2;
      bits = wf.wBitsPerSample;
      process_mode = true;
      target_pid = pid;
      checked_at = GetTickCount64();
      an.Reset(static_cast<float>(wf.nSamplesPerSec));
      note = L"capturing only process " + std::to_wstring(pid) + L" and its children (format " + std::to_wstring(i + 1) + L")";
      return true;
    }
    Close();
    wchar_t code[16] = {};
    swprintf_s(code, L"0x%08X", static_cast<unsigned>(hr));
    note = std::wstring(L"per-app capture of process ") + std::to_wstring(pid) + L" failed at \"" + stage +
           L"\" (HRESULT " + code + L")";
    return false;
  }

  // `app` = the media session's app id; empty = the whole system mix. With an app: capture just that app.
  // The system mix stands in only when per-app capture cannot work at all (Windows before build 20348) or the
  // app id matches no running process (some players report an opaque id). When per-app capture is supported but
  // failed, nothing is captured (the caller shows synthetic motion): falling back to the system mix would let
  // other apps move the bars, which is exactly what this feature prevents.
  bool Open(Analyzer& an, const std::wstring& app) {
    Close();
    note.clear();
    std::wstring why;
    if (!app.empty()) {
      if (ProcessLoopbackSupported()) {
        const DWORD pid = static_cast<DWORD>(FindAppRootPid(app));
        if (pid != 0) return OpenProcessLoopback(an, pid);
        why = L"no running process matches \"" + app + L"\"";
      } else {
        why = L"this Windows version has no per-app capture (needs build 20348+)";
      }
    }
    if (OpenSystem(an)) {
      note = why.empty() ? std::wstring(L"capturing all system audio") : L"capturing all system audio: " + why;
      return true;
    }
    note = L"no playback device to capture";
    return false;
  }

  // Time to re-open? (checked every few seconds only) Process mode: the app's process tree changed or went away.
  // System mode: the default device changed, or an app is selected and now can be captured on its own.
  bool NeedsReopen(const std::wstring& app) {
    const ULONGLONG now = GetTickCount64();
    if (now - checked_at < kDeviceCheckMs) return false;
    if (process_mode) {
      checked_at = now;
      return app.empty() || static_cast<DWORD>(FindAppRootPid(app)) != target_pid;
    }
    if (!app.empty() && ProcessLoopbackSupported()) {
      checked_at = now;
      if (FindAppRootPid(app) != 0) return true;
    }
    return DeviceChanged();
  }

  // The default playback device changed (headphones plugged in, ...): the loopback stream stays on the old one.
  bool DeviceChanged() {
    const ULONGLONG now = GetTickCount64();
    if (now - checked_at < kDeviceCheckMs) return false;
    checked_at = now;
    std::wstring id;
    return DefaultDeviceId(&id, nullptr) && id != dev_id;
  }

  // Moves everything captured so far into the analyzer. False => the stream is gone (re-open).
  bool Drain(Analyzer& an, bool* got) {
    UINT32 packet = 0;
    HRESULT hr = S_OK;
    while (SUCCEEDED(hr = cap->GetNextPacketSize(&packet)) && packet > 0) {
      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      hr = cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
      if (FAILED(hr)) return false;
      mono.assign(frames, 0.0f);
      if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data) {
        const float inv = 1.0f / static_cast<float>(channels);
        if (is_float) {
          const float* s = reinterpret_cast<const float*>(data);
          for (UINT32 i = 0; i < frames; ++i) {
            float acc = 0.0f;
            for (int c = 0; c < channels; ++c) acc += s[static_cast<size_t>(i) * channels + c];
            mono[i] = acc * inv;
          }
        } else {
          const int16_t* s = reinterpret_cast<const int16_t*>(data);
          for (UINT32 i = 0; i < frames; ++i) {
            float acc = 0.0f;
            for (int c = 0; c < channels; ++c) acc += static_cast<float>(s[static_cast<size_t>(i) * channels + c]);
            mono[i] = acc * inv / 32768.0f;
          }
        }
      }
      an.Push(mono.data(), frames);
      cap->ReleaseBuffer(frames);
      *got = true;
    }
    return SUCCEEDED(hr);
  }
};

} // namespace

struct AudioSpectrum::Impl {
  std::atomic<bool> active{ false };
  std::atomic<bool> quit{ false };
  std::atomic<int> state{ static_cast<int>(State::Idle) };
  std::atomic<float> bands[kBands];
  HANDLE wake = nullptr;      // auto-reset: SetActive / shutdown / retry sleeps
  std::thread worker;
  std::mutex target_mtx;
  std::wstring target;        // app id to follow; empty = the whole system mix
  std::mutex diag_mtx;
  std::wstring diag, diag_last;
  std::atomic<bool> diag_ready{ false };

  // One short line per change of what the capture is doing (or why it cannot), picked up by the UI for its log.
  void PostNote(const std::wstring& n) {
    if (n.empty()) return;
    std::lock_guard<std::mutex> lk(diag_mtx);
    if (n == diag_last) return;
    diag_last = n;
    diag = n;
    diag_ready.store(true);
  }

  std::wstring Target() {
    std::lock_guard<std::mutex> lk(target_mtx);
    return target;
  }

  Impl() {
    for (auto& b : bands) b.store(0.0f, std::memory_order_relaxed);
    wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  }

  ~Impl() {
    quit.store(true);
    if (wake) SetEvent(wake);
    if (worker.joinable()) worker.join();
    if (wake) CloseHandle(wake);
  }

  void SetState(State s) { state.store(static_cast<int>(s), std::memory_order_release); }
  void ZeroBands() { for (auto& b : bands) b.store(0.0f, std::memory_order_relaxed); }

  void Run() {
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    {
      Analyzer an;
      Capture cap;
      std::wstring cap_app;       // the target `cap` was opened for
      LARGE_INTEGER freq{}, last{};
      QueryPerformanceFrequency(&freq);

      while (!quit.load()) {
        if (!active.load()) {
          cap.Close();
          ZeroBands();
          SetState(State::Idle);
          WaitForSingleObject(wake, INFINITE);
          continue;
        }
        const std::wstring app = Target();
        if (app != cap_app) {       // another app is selected (or the setting was toggled): start over
          cap.Close();
          cap_app = app;
        }
        if (!cap.IsOpen()) {
          const bool opened = cap.Open(an, app);
          PostNote(cap.note);
          if (!opened) {
            ZeroBands();
            SetState(State::Unavailable);
            WaitForSingleObject(wake, kRetryMs);
            continue;
          }
          QueryPerformanceCounter(&last);
        }

        WaitForSingleObject(wake, kPollMs);
        if (quit.load() || !active.load()) continue;

        bool got = false;
        if (!cap.Drain(an, &got)) {   // device invalidated / removed
          cap.Close();
          continue;
        }
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double elapsed = static_cast<double>(now.QuadPart - last.QuadPart) / static_cast<double>(freq.QuadPart);
        last = now;
        // Loopback delivers no packets while nothing is playing: feed silence for the elapsed time so the bars decay.
        if (!got) an.PushSilence(static_cast<size_t>((std::min)(elapsed, 0.1) * an.rate));

        if (an.Ready()) {
          float lv[kBands];
          an.Analyze(lv);
          for (int i = 0; i < kBands; ++i) bands[i].store(lv[i], std::memory_order_relaxed);
          SetState(State::Live);
        }
        if (cap.NeedsReopen(app)) cap.Close();
      }
      cap.Close();
    }
    if (com) CoUninitialize();
  }
};

AudioSpectrum::AudioSpectrum() : impl_(std::make_unique<Impl>()) {}
AudioSpectrum::~AudioSpectrum() = default;

void AudioSpectrum::SetTargetApp(const std::wstring& app) {
  Impl& d = *impl_;
  {
    std::lock_guard<std::mutex> lk(d.target_mtx);
    if (d.target == app) return;
    d.target = app;
  }
  if (d.wake) SetEvent(d.wake);
}

bool AudioSpectrum::TakeDiagnostic(std::wstring* line) {
  Impl& d = *impl_;
  if (!d.diag_ready.load()) return false;
  std::lock_guard<std::mutex> lk(d.diag_mtx);
  if (line) *line = d.diag;
  d.diag_ready.store(false);
  return true;
}

void AudioSpectrum::SetActive(bool on) {
  Impl& d = *impl_;
  if (d.active.load(std::memory_order_relaxed) == on) return;
  d.active.store(on);
  if (on && !d.worker.joinable() && !d.quit.load()) d.worker = std::thread([&d] { d.Run(); });
  if (d.wake) SetEvent(d.wake);
}

AudioSpectrum::State AudioSpectrum::GetState() const {
  return static_cast<State>(impl_->state.load(std::memory_order_acquire));
}

void AudioSpectrum::GetBands(float (&out)[kBands]) const {
  for (int i = 0; i < kBands; ++i) out[i] = impl_->bands[i].load(std::memory_order_relaxed);
}

} // namespace island
