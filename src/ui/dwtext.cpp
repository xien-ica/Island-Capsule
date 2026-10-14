#include "ui/dwtext.hpp"

#include <d2d1.h>
#include <dwrite.h>
#include <dxgiformat.h>
#include <wrl/client.h>

#include <cmath>
#include <cstring>
#include <unordered_map>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace island {

using Microsoft::WRL::ComPtr;

namespace {
// Text rendering tuning (grayscale AA on a transparent surface).
constexpr float kGamma = 1.8f;
constexpr float kEnhancedContrast = 0.8f;   // 0..1+: higher = crisper / slightly heavier light-on-dark text
constexpr size_t kMaxRuns = 256;
constexpr size_t kMaxWidths = 512;

std::wstring RunKey(const std::wstring& s, bool artist, int r, int g, int b) {
  std::wstring k;
  k.reserve(s.size() + 8);
  k.push_back(artist ? L'A' : L'T');
  k.push_back(static_cast<wchar_t>(0x100 + (r & 0xFF)));
  k.push_back(static_cast<wchar_t>(0x100 + (g & 0xFF)));
  k.push_back(static_cast<wchar_t>(0x100 + (b & 0xFF)));
  k += s;
  return k;
}
} // namespace

struct DwText::Impl {
  ComPtr<ID2D1Factory> d2d;
  ComPtr<IDWriteFactory> dw;
  ComPtr<ID2D1DCRenderTarget> rt;
  ComPtr<IDWriteRenderingParams> params;
  ComPtr<IDWriteTextFormat> fmt[2];   // [0] title, [1] artist
  float line_h[2] = { -1.0f, -1.0f };
  HDC mem_dc = nullptr;

  struct Entry {
    HBITMAP dib = nullptr;
    std::unique_ptr<Gdiplus::Bitmap> bmp;   // wraps the DIB's memory: must die before the DIB
    DwText::Run run;
  };
  std::unordered_map<std::wstring, Entry> runs;
  std::unordered_map<std::wstring, float> widths[2];

  ~Impl() {
    ClearRuns();
    if (mem_dc) DeleteDC(mem_dc);
  }

  void ClearRuns() {
    for (auto& kv : runs) {
      kv.second.bmp.reset();
      if (kv.second.dib) DeleteObject(kv.second.dib);
    }
    runs.clear();
  }

  bool CreateRenderTarget() {
    rt.Reset();
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f,
        D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
    if (FAILED(d2d->CreateDCRenderTarget(&props, &rt))) return false;
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);   // ClearType needs an opaque target
    if (params) rt->SetTextRenderingParams(params.Get());
    return true;
  }

  bool FamilyExists(const wchar_t* name) {
    ComPtr<IDWriteFontCollection> coll;
    if (FAILED(dw->GetSystemFontCollection(&coll, FALSE))) return false;
    UINT32 idx = 0;
    BOOL found = FALSE;
    return SUCCEEDED(coll->FindFamilyName(name, &idx, &found)) && found;
  }

  ComPtr<IDWriteTextLayout> Layout(const std::wstring& s, bool artist) {
    ComPtr<IDWriteTextLayout> l;
    if (!fmt[artist ? 1 : 0]) return l;
    if (FAILED(dw->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), fmt[artist ? 1 : 0].Get(), 16384.0f,
                                    1024.0f, &l)))
      l.Reset();
    return l;
  }
};

DwText::DwText() : impl_(std::make_unique<Impl>()) {}
DwText::~DwText() = default;

bool DwText::Init(float title_px, float artist_px, const std::wstring& req_family) {
  Impl& d = *impl_;
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d.d2d.GetAddressOf()))) return false;
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(d.dw.GetAddressOf()))))
    return false;

  std::wstring family = req_family;
  for (const wchar_t* fb : { L"Segoe UI Variable Text", L"Segoe UI", L"Arial" }) {
    if (!family.empty() && d.FamilyExists(family.c_str())) break;
    family = fb;
  }
  auto make = [&](DWRITE_FONT_WEIGHT w, float px, ComPtr<IDWriteTextFormat>& out) {
    if (FAILED(d.dw->CreateTextFormat(family.c_str(), nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px,
                                      L"en-us", &out)))
      return false;
    out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    out->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    return true;
  };
  if (!make(DWRITE_FONT_WEIGHT_BOLD, title_px, d.fmt[0])) return false;
  if (!make(DWRITE_FONT_WEIGHT_NORMAL, artist_px, d.fmt[1])) return false;

  d.dw->CreateCustomRenderingParams(kGamma, kEnhancedContrast, 0.0f, DWRITE_PIXEL_GEOMETRY_FLAT,
                                    DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, &d.params);   // optional: null is fine

  HDC screen = GetDC(nullptr);
  d.mem_dc = CreateCompatibleDC(screen);
  ReleaseDC(nullptr, screen);
  if (!d.mem_dc) return false;
  return d.CreateRenderTarget();
}

float DwText::Width(const std::wstring& s, bool artist) {
  if (s.empty()) return 0.0f;
  Impl& d = *impl_;
  auto& cache = d.widths[artist ? 1 : 0];
  if (const auto it = cache.find(s); it != cache.end()) return it->second;
  float w = 0.0f;
  if (auto l = d.Layout(s, artist)) {
    DWRITE_TEXT_METRICS m{};
    if (SUCCEEDED(l->GetMetrics(&m))) w = m.widthIncludingTrailingWhitespace;
  }
  if (cache.size() > kMaxWidths) cache.clear();
  cache.emplace(s, w);
  return w;
}

float DwText::LineHeight(bool artist) {
  Impl& d = *impl_;
  float& h = d.line_h[artist ? 1 : 0];
  if (h < 0.0f) {
    h = 0.0f;
    if (auto l = d.Layout(L"Ag", artist)) {
      DWRITE_TEXT_METRICS m{};
      if (SUCCEEDED(l->GetMetrics(&m))) h = std::ceil(m.height);
    }
  }
  return h;
}

const DwText::Run* DwText::Get(const std::wstring& s, bool artist, int r, int g, int b) {
  if (s.empty()) return nullptr;
  Impl& d = *impl_;
  if (!d.rt || !d.mem_dc) return nullptr;

  const std::wstring key = RunKey(s, artist, r, g, b);
  if (const auto it = d.runs.find(key); it != d.runs.end()) return &it->second.run;

  auto layout = d.Layout(s, artist);
  if (!layout) return nullptr;
  DWRITE_TEXT_METRICS m{};
  if (FAILED(layout->GetMetrics(&m))) return nullptr;
  const int w = static_cast<int>(std::ceil(m.widthIncludingTrailingWhitespace)) + 4;   // a little room for AA edges
  const int h = static_cast<int>(LineHeight(artist));
  if (w <= 4 || h <= 0 || w > 16000) return nullptr;

  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;   // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP dib = CreateDIBSection(d.mem_dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!dib || !bits) {
    if (dib) DeleteObject(dib);
    return nullptr;
  }
  std::memset(bits, 0, static_cast<size_t>(w) * static_cast<size_t>(h) * 4);

  HGDIOBJ old = SelectObject(d.mem_dc, dib);
  bool ok = false;
  const RECT rc{ 0, 0, w, h };
  if (SUCCEEDED(d.rt->BindDC(d.mem_dc, &rc))) {
    ComPtr<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(d.rt->CreateSolidColorBrush(D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f), &brush))) {
      d.rt->BeginDraw();
      d.rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
      d.rt->DrawTextLayout(D2D1::Point2F(0.0f, 0.0f), layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
      const HRESULT hr = d.rt->EndDraw();
      ok = SUCCEEDED(hr);
      if (hr == D2DERR_RECREATE_TARGET) d.CreateRenderTarget();
    }
  }
  SelectObject(d.mem_dc, old);
  if (!ok) {
    DeleteObject(dib);
    return nullptr;
  }

  if (d.runs.size() >= kMaxRuns) d.ClearRuns();   // bounded; callers use the returned run immediately

  Impl::Entry e;
  e.dib = dib;
  e.bmp = std::make_unique<Gdiplus::Bitmap>(w, h, w * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
  e.run.bmp = e.bmp.get();
  e.run.w = w;
  e.run.h = h;
  auto ins = d.runs.emplace(key, std::move(e));
  return &ins.first->second.run;
}

} // namespace island
