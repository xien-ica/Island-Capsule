// Pill window: drawing. Text, capsule surface, cover, volume HUD, visualizer and the layered-window
// render target (premultiplied DIB shown with UpdateLayeredWindow).

#include "ui/pill_impl.hpp"

namespace island {

namespace {

float MeasureWidth(gp::Graphics& g, const std::wstring& s, const gp::Font& f,
                   const gp::StringFormat& fmt) {
  if (s.empty()) return 0.0f;
  gp::RectF box;
  g.MeasureString(s.c_str(), static_cast<INT>(s.size()), &f, gp::PointF(0, 0), &fmt, &box);
  return box.Width;
}

inline uint32_t ScalePremul(uint32_t p, float f) {
  const uint32_t a = static_cast<uint32_t>(((p >> 24) & 0xFF) * f);
  const uint32_t r = static_cast<uint32_t>(((p >> 16) & 0xFF) * f);
  const uint32_t g = static_cast<uint32_t>(((p >> 8) & 0xFF) * f);
  const uint32_t b = static_cast<uint32_t>((p & 0xFF) * f);
  return (a << 24) | (r << 16) | (g << 8) | b;
}

// Fade the left/right edges of a premultiplied bitmap (marquee edge fade).
void ApplyEdgeFade(gp::Bitmap& bmp, int w, int h, int left_w, int right_w) {
  if (left_w <= 0 && right_w <= 0) return;
  gp::BitmapData bd;
  gp::Rect r(0, 0, w, h);
  if (bmp.LockBits(&r, gp::ImageLockModeRead | gp::ImageLockModeWrite,
                   PixelFormat32bppPARGB, &bd) != gp::Ok)
    return;
  for (int y = 0; y < h; ++y) {
    auto* row = reinterpret_cast<uint32_t*>(static_cast<BYTE*>(bd.Scan0) + static_cast<ptrdiff_t>(y) * bd.Stride);
    for (int x = 0; x < left_w && x < w; ++x)
      row[x] = ScalePremul(row[x], (static_cast<float>(x) + 0.5f) / static_cast<float>(left_w));
    for (int x = 0; x < right_w && x < w; ++x)
      row[w - 1 - x] = ScalePremul(row[w - 1 - x], (static_cast<float>(x) + 0.5f) / static_cast<float>(right_w));
  }
  bmp.UnlockBits(&bd);
}

// Fills `clip` with the cover (anti-aliased rounded edge, unlike a GDI+ clip region), placed at
// whole-pixel (x, y). `alpha` is the crossfade opacity.
void DrawArtFill(gp::Graphics& g, ScaledArt& art, const gp::GraphicsPath& clip, float x, float y, float alpha) {
  if (!art.bmp || alpha <= 0.002f) return;

  // Steady state (no crossfade running): one TextureBrush is built per cover and reused every frame.
  if (alpha >= 0.998f) {
    if (!art.brush || art.brush_x != x || art.brush_y != y) {
      art.brush = std::make_unique<gp::TextureBrush>(art.bmp.get(), gp::Rect(0, 0, art.px, art.px));
      art.brush->SetWrapMode(gp::WrapModeTileFlipXY);
      art.brush->TranslateTransform(x, y);
      art.brush_x = x;
      art.brush_y = y;
    }
    g.FillPath(art.brush.get(), &clip);
    return;
  }

  // Crossfade: the opacity changes every frame, so the brush is rebuilt (short, ~0.35 s).
  gp::ImageAttributes ia;
  if (alpha < 0.998f) {
    gp::ColorMatrix cm{};
    for (int i = 0; i < 5; ++i) cm.m[i][i] = 1.0f;
    cm.m[3][3] = alpha;
    ia.SetColorMatrix(&cm, gp::ColorMatrixFlagsDefault, gp::ColorAdjustTypeBitmap);
  }
  gp::TextureBrush tb(art.bmp.get(), gp::Rect(0, 0, art.px, art.px), &ia);
  tb.SetWrapMode(gp::WrapModeTileFlipXY);
  tb.TranslateTransform(x, y);
  g.FillPath(&tb, &clip);
}

} // namespace

float Fonts::Width(gp::Graphics& g, const std::wstring& s, bool is_artist) {
  if (dw) return dw->Width(s, is_artist);
  return MeasureWidth(g, s, is_artist ? *artist : *title, *fmt);
}

// Draws `str` with DirectWrite (cached bitmap + DrawImage) at a whole-pixel origin. False => not available,
// the caller draws with GDI+ instead.
bool PillWindow::Impl::DrawDwRun(gp::Graphics& g, const std::wstring& str, bool is_artist, const Rgb& ink, float alpha255,
               float x, float y) {
  if (!fonts || !fonts->dw || alpha255 <= 0.5f) return false;
  const DwText::Run* run = fonts->dw->Get(str, is_artist, Ch(ink.r), Ch(ink.g), Ch(ink.b));
  if (!run || !run->bmp) return false;
  gp::ImageAttributes ia;
  AlphaAttrs(ia, alpha255);
  const gp::GraphicsState st = g.Save();
  if (RootScale() == 1.0f) g.SetInterpolationMode(gp::InterpolationModeNearestNeighbor);   // exact 1:1 copy
  g.DrawImage(run->bmp, gp::RectF(std::round(x), std::round(y), static_cast<float>(run->w), static_cast<float>(run->h)),
              0.0f, 0.0f, static_cast<float>(run->w), static_cast<float>(run->h), gp::UnitPixel, &ia);
  g.Restore(st);
  return true;
}

// One line of text. `x` is where the text starts (may be slid), `clip_x` the fixed
// left edge of the text area. `mq` null => static (outgoing text during a slide).
void PillWindow::Impl::DrawLine(gp::Graphics& g, const std::wstring& str, const gp::Font& font, const Rgb& ink,
              float alpha, float x, float clip_x, float y, float avail, float line_h,
              float text_w, bool overflow, const Marquee* mq) {
  if (str.empty() || avail <= 1.0f || alpha <= 0.5f) return;
  // Whole-pixel origin: text drawn at a fractional position is resampled and looks blurry.
  x = std::round(x);
  y = std::round(y);

  const bool is_artist = fonts && &font == fonts->artist.get();
  const DwText::Run* run = (fonts && fonts->dw) ? fonts->dw->Get(str, is_artist, Ch(ink.r), Ch(ink.g), Ch(ink.b)) : nullptr;
  gp::ImageAttributes ia;
  if (run) AlphaAttrs(ia, alpha);
  const bool exact = RootScale() == 1.0f;

  const gp::GraphicsState st = g.Save();
  g.SetClip(gp::RectF(clip_x, y, avail, line_h), gp::CombineModeIntersect);

  if (!overflow || !mq) {
    if (run) {
      if (exact) g.SetInterpolationMode(gp::InterpolationModeNearestNeighbor);
      g.DrawImage(run->bmp, gp::RectF(x, y, static_cast<float>(run->w), static_cast<float>(run->h)), 0.0f, 0.0f,
                  static_cast<float>(run->w), static_cast<float>(run->h), gp::UnitPixel, &ia);
    } else {
      gp::SolidBrush brush(Col(ink, alpha));
      g.DrawString(str.c_str(), -1, &font, gp::RectF(x, y, text_w + 4.0f, line_h), fonts->fmt.get(), &brush);
    }
  } else {
    // Marquee: render into a persistent off-screen strip (re-created only when its size changes), fade the
    // edges, composite.
    const int tw = static_cast<int>(std::ceil(avail));
    const int th = static_cast<int>(std::ceil(line_h));
    MarqueeScratch& sc = mq_scratch[mq == &artist_mq ? 1 : 0];
    if (!sc.bmp || sc.w != tw || sc.h != th) {
      sc.Release();
      sc.bmp = std::make_unique<gp::Bitmap>(tw, th, PixelFormat32bppPARGB);
      sc.w = tw;
      sc.h = th;
    }
    {
      // Graphics goes out of scope before ApplyEdgeFade locks the bits (same order as before).
      gp::Graphics tg(sc.bmp.get());
      tg.SetTextRenderingHint(kCapsuleTextHint);
      tg.SetTextContrast(kCapsuleTextContrast);
      tg.Clear(gp::Color(0, 0, 0, 0));
      const float off = std::round(mq->Offset());
      const float loop = std::round(mq->Loop());
      if (run) {
        tg.SetInterpolationMode(gp::InterpolationModeNearestNeighbor);
        const float rw = static_cast<float>(run->w), rh = static_cast<float>(run->h);
        tg.DrawImage(run->bmp, gp::RectF(-off, 0.0f, rw, rh), 0.0f, 0.0f, rw, rh, gp::UnitPixel, &ia);
        tg.DrawImage(run->bmp, gp::RectF(-off + loop, 0.0f, rw, rh), 0.0f, 0.0f, rw, rh, gp::UnitPixel, &ia);
      } else {
        gp::SolidBrush brush(Col(ink, alpha));
        tg.DrawString(str.c_str(), -1, &font, gp::RectF(-off, 0.0f, text_w + 4.0f, static_cast<float>(th)),
                      fonts->fmt.get(), &brush);
        tg.DrawString(str.c_str(), -1, &font, gp::RectF(-off + loop, 0.0f, text_w + 4.0f, static_cast<float>(th)),
                      fonts->fmt.get(), &brush);
      }
    }
    const int fade = static_cast<int>(std::lround(kFadeW * Scale()));
    const int left_fade = (std::min)(fade, static_cast<int>(std::round(mq->Offset())));
    ApplyEdgeFade(*sc.bmp, tw, th, left_fade, fade);
    g.DrawImage(sc.bmp.get(), gp::RectF(x, y, static_cast<float>(tw), static_cast<float>(th)),
                0.0f, 0.0f, static_cast<gp::REAL>(tw), static_cast<gp::REAL>(th), gp::UnitPixel);
  }
  g.Restore(st);
}

void PillWindow::Impl::DrawTextBlock(gp::Graphics& g, float tx, float avail, float H, float xoff, float alpha,
                   const std::wstring& title, const std::wstring& artist, float tw, float aw,
                   bool tover, bool aover, bool is_dim, const Marquee* tmq, const Marquee* amq) {
  const float th = fonts->TitleH(g);
  const float ah = fonts->ArtistH(g);
  const float hud = 1.0f - static_cast<float>(vol_t);   // text gives way to the volume bar
  const float ta = (is_dim ? 150.0f : style.title_a) * alpha * content_a * hud * def_mul;
  const float aa = style.sub_a * alpha * content_a * hud * def_mul;

  if (artist.empty()) {
    const float ty = std::round((H - th) / 2.0f);
    DrawLine(g, title, *fonts->title, style.title, ta, tx + xoff, tx, ty, avail, th, tw, tover, tmq);
  } else {
    const float ty = std::round((H - (th + ah)) / 2.0f);
    DrawLine(g, title, *fonts->title, style.title, ta, tx + xoff, tx, ty, avail, th, tw, tover, tmq);
    DrawLine(g, artist, *fonts->artist, style.sub, aa, tx + xoff, tx, ty + th, avail, ah, aw, aover, amq);
  }
}

// Capsule + control tab as a single closed outline (pill-local coords, capsule at 0,0; the tab rises to y = -U).
// Concave fillets where tab meets capsule make it read as one shape, with no seam line.
void PillWindow::Impl::BuildUnionPath(gp::GraphicsPath& path, float W, float H, float U, float tw) const {
  const float R = H * 0.5f;
  const float xl = (W - tw) * 0.5f, xr = xl + tw;
  const float f = (std::min)({ static_cast<float>(kUpFillet * Scale()), U, xl - R });
  const float rp = (std::max)(1.0f, (std::min)({ static_cast<float>(strip.UpHeight()) * 0.5f, U * 0.5f, tw * 0.5f }));
  if (f > 0.5f) path.AddArc(xl - 2 * f, -2 * f, 2 * f, 2 * f, 90.0f, -90.0f);   // fillet (left)
  path.AddArc(xl, -U, 2 * rp, 2 * rp, 180.0f, 90.0f);                             // tab top-left
  path.AddArc(xr - 2 * rp, -U, 2 * rp, 2 * rp, 270.0f, 90.0f);                    // tab top-right
  if (f > 0.5f) path.AddArc(xr, -2 * f, 2 * f, 2 * f, 180.0f, -90.0f);          // fillet (right)
  path.AddArc(W - H, 0.0f, H, H, 270.0f, 180.0f);                             // right end
  path.AddArc(0.0f, 0.0f, H, H, 90.0f, 180.0f);                               // left end
  path.CloseFigure();
}

void PillWindow::Impl::DrawSurface(gp::Graphics& g, float W, float H) {
  if (W <= 1.0f) return;
  const float U = static_cast<float>(up_extra_px);
  const bool merged = U >= 2.0f && W - H > 8.0f;   // control tab open: one merged shape
  const float tw = merged ? UpTabWidth() : 0.0f;
  const float uk = merged ? U : 0.0f;

  // Geometry is rebuilt only when the capsule size / tab changed (not for viz-only or colour-only frames).
  SurfaceCache& c = surf;
  if (!c.body || c.W != W || c.H != H || c.U != uk || c.tw != tw || c.merged != merged) {
    c.body = std::make_unique<gp::GraphicsPath>();
    c.rim_path.reset();
    if (merged) {
      BuildUnionPath(*c.body, W, H, U, tw);
    } else {
      AddPill(*c.body, 0.0f, 0.0f, W, H);
      c.rim_path = std::make_unique<gp::GraphicsPath>();
      AddPill(*c.rim_path, 0.5f, 0.5f, W - 1.0f, H - 1.0f);
    }
    c.fill = std::make_unique<gp::LinearGradientBrush>(gp::PointF(0.0f, 0.0f), gp::PointF(W, 0.0f),
                                                       Col(style.fill, style.fill_a),
                                                       Col(style.fill2, style.fill2_a));
    c.rim = std::make_unique<gp::Pen>(Col(style.rim, style.rim_a), 1.0f);
    if (merged) c.rim->SetAlignment(gp::PenAlignmentInset);
    c.W = W; c.H = H; c.U = uk; c.tw = tw; c.merged = merged;
  }
  // The album tint animates: colours are updated in place.
  c.fill->SetLinearColors(Col(style.fill, style.fill_a), Col(style.fill2, style.fill2_a));
  c.rim->SetColor(Col(style.rim, style.rim_a));

  g.FillPath(c.fill.get(), c.body.get());
  g.DrawPath(c.rim.get(), merged ? c.body.get() : c.rim_path.get());
}

// Accent glow crawling out from the join, clipped to the strip capsule (strip coordinates).
void PillWindow::Impl::DrawSeam(gp::Graphics& g, float alpha) {
  const float W = static_cast<float>(strip.Width());
  const float ov = static_cast<float>(strip.Overlap());
  const float sh = strip.SurfaceHeight();
  const float y0 = strip.SurfaceInset();
  if (W <= 0.0f) return;

  const float t = static_cast<float>(EaseOutCubic(seam));
  const float env = std::sin(3.14159265f * static_cast<float>(seam));
  // join = where the strip meets the pill edge (inner end), glow travels to the far end
  const float cx = Dir() > 0 ? ov + (W - ov) * t : (W - ov) * (1.0f - t);
  const float r = W * 0.75f;

  gp::GraphicsPath body;
  AddPill(body, 0.0f, y0, W, sh);
  const gp::GraphicsState st = g.Save();
  g.SetClip(&body, gp::CombineModeIntersect);

  gp::GraphicsPath blob;
  blob.AddEllipse(cx - r, y0 - sh * 0.5f, r * 2.0f, sh * 2.0f);
  gp::PathGradientBrush pb(&blob);
  pb.SetCenterColor(Col(style.viz, 115.0f * env, alpha));
  gp::Color edge = Col(style.viz, 0.0f, 0.0f);
  INT count = 1;
  pb.SetSurroundColors(&edge, &count);
  g.FillPath(&pb, &blob);
  g.Restore(st);
}

// The current lyric line (single line, title font), crossfaded in over the title / artist block.
void PillWindow::Impl::DrawLyricLine(gp::Graphics& g, float tx, float avail, float H) {
  if (lyr_mix <= 0.003 || lyr_text.empty() || !fonts) return;
  const float s = static_cast<float>(Scale());
  const float th = fonts->TitleH(g);
  const float hud = (1.0f - static_cast<float>(vol_t)) * (1.0f - static_cast<float>(nudge_t));
  const float base = style.title_a * content_a * hud * static_cast<float>(lyr_mix);
  const float ty = std::round((H - th) / 2.0f);
  const float te = lyr_t < 1.0 ? static_cast<float>(EaseOutCubic(lyr_t)) : 1.0f;
  const float off = (cfg.reduced_motion || !cfg.text_slide) ? 0.0f : static_cast<float>(kLyricSlideOff) * s;
  // Lines are centered in the text area (between the cover and the visualizer) and never scroll.
  auto cx = [&](float w) { return tx + (std::max)(0.0f, (avail - w) * 0.5f); };
  if (lyr_t < 1.0 && !lyr_old.empty())   // outgoing line drifts up and fades
    DrawLine(g, lyr_old, *fonts->title, style.title, base * (1.0f - te), cx(lyr_old_w), tx, ty - off * te, avail,
             th, lyr_old_w, false, nullptr);
  DrawLine(g, lyr_text, *fonts->title, style.title, base * te, cx(lyr_text_w), tx, ty + off * (1.0f - te), avail,
           th, lyr_text_w, false, nullptr);
}

// "Lyrics +300 ms": replaces the lyric line for a moment after a timing nudge.
void PillWindow::Impl::DrawNudge(gp::Graphics& g, float tx, float avail, float H) {
  const float a = static_cast<float>(nudge_t) * static_cast<float>(lyr_mix);
  if (a <= 0.01f || nudge_str.empty() || !fonts) return;
  if (nudge_w < 0.0f) nudge_w = fonts->Width(g, nudge_str, false);
  const float th = fonts->TitleH(g);
  const float hud = 1.0f - static_cast<float>(vol_t);
  const float ty = std::round((H - th) / 2.0f);
  const float x = tx + (std::max)(0.0f, (avail - nudge_w) * 0.5f);
  DrawLine(g, nudge_str, *fonts->title, style.title, style.title_a * content_a * hud * a, x, tx, ty, avail, th,
           nudge_w, false, nullptr);
}

// Volume HUD that replaces the text area while scrolling: [speaker] [====----] [45]
void PillWindow::Impl::DrawVolume(gp::Graphics& g, float tx, float avail, float H) {
  const float a = static_cast<float>(vol_t);
  const float s = static_cast<float>(Scale());
  if (a <= 0.01f || avail < 60.0f * s) return;

  const float cy = H * 0.5f;
  const float right = tx + avail;

  // percent, right aligned
  const int pct = vol_muted ? 0 : static_cast<int>(std::lround((std::clamp)(vol_disp, 0.0f, 1.0f) * 100.0f));
  const std::wstring num = std::to_wstring(pct);
  Fonts& fo = *fonts;
  if (fo.vol_reserve < 0.0f) fo.vol_reserve = fo.Width(g, L"100", false);
  if (fo.vol_num_pct != pct) {   // the number only changes when the level does, not every frame
    fo.vol_num_w = fo.Width(g, num, false);
    fo.vol_num_pct = pct;
  }
  const float reserve = fo.vol_reserve;
  const float nw = fo.vol_num_w;
  const float nh = fo.TitleH(g);
  if (!DrawDwRun(g, num, false, style.title, style.title_a * a, std::round(right - nw), std::round(cy - nh * 0.5f))) {
    gp::SolidBrush nb(Col(style.title, style.title_a, a));
    g.DrawString(num.c_str(), -1, fonts->title.get(),
                 gp::PointF(std::round(right - nw), std::round(cy - nh * 0.5f)), fonts->fmt.get(), &nb);
  }

  // speaker glyph
  const float ix = std::round(tx);
  {
    gp::SolidBrush ink(Col(style.ink, 235.0f, a));
    const gp::PointF cone[6] = { { ix, cy - 2.4f * s }, { ix + 3.0f * s, cy - 2.4f * s },
                                 { ix + 6.6f * s, cy - 5.4f * s }, { ix + 6.6f * s, cy + 5.4f * s },
                                 { ix + 3.0f * s, cy + 2.4f * s }, { ix, cy + 2.4f * s } };
    g.FillPolygon(&ink, cone, 6);

    gp::Pen pen(Col(style.ink, 235.0f, a), (std::max)(1.2f, 1.4f * s));
    pen.SetStartCap(gp::LineCapRound);
    pen.SetEndCap(gp::LineCapRound);
    if (vol_muted || pct == 0) {
      g.DrawLine(&pen, ix + 8.4f * s, cy - 2.6f * s, ix + 12.6f * s, cy + 2.6f * s);
      g.DrawLine(&pen, ix + 8.4f * s, cy + 2.6f * s, ix + 12.6f * s, cy - 2.6f * s);
    } else {
      const float r1 = 2.6f * s, r2 = 5.2f * s, ax = ix + 7.4f * s;
      g.DrawArc(&pen, ax - r1, cy - r1, r1 * 2.0f, r1 * 2.0f, -45.0f, 90.0f);
      if (pct > 33) g.DrawArc(&pen, ax - r2, cy - r2, r2 * 2.0f, r2 * 2.0f, -45.0f, 90.0f);
    }
  }

  // bar
  const float x0 = ix + 14.5f * s + 6.0f * s;
  const float x1 = right - reserve - 8.0f * s;
  if (x1 - x0 < 16.0f * s) return;
  const float th = (std::max)(3.0f, std::round(4.0f * s));
  const float by = std::round(cy - th * 0.5f);

  gp::GraphicsPath track;
  AddRoundRect(track, x0, by, x1 - x0, th, th * 0.5f);
  gp::SolidBrush tb(Col(style.ink, 46.0f, a));
  g.FillPath(&tb, &track);

  const float lvl = vol_muted ? 0.0f : (std::clamp)(vol_disp, 0.0f, 1.0f);
  if (lvl > 0.004f) {
    const float fw = (std::max)(th, (x1 - x0) * lvl);
    gp::GraphicsPath done;
    AddRoundRect(done, x0, by, fw, th, th * 0.5f);
    gp::SolidBrush fb(Col(style.viz, style.viz_a, a));
    g.FillPath(&fb, &done);
  }
}

// Pill content only (cover, text, visualizer); the surface is drawn by DrawSurface.
void PillWindow::Impl::DrawPill(gp::Graphics& g) {
  const double s = Scale();
  const float W = static_cast<float>(pill_w);
  const float H = static_cast<float>(pill_h);

  // Album art, crossfaded on track change. Whole-pixel size + position, pre-downsampled copy.
  const int art_px = (std::max)(1, static_cast<int>(std::lround(kArtSize * s)));
  const float art = static_cast<float>(art_px);
  const float art_x = static_cast<float>(std::lround(kPadLeft * s));
  const float art_y = std::floor((H - art) / 2.0f);
  {
    const float art_r = static_cast<float>(kArtRadius * s);
    ArtGfxCache& ac = art_gfx;
    if (!ac.clip || ac.px != art_px || ac.x != art_x || ac.y != art_y || ac.r != art_r) {
      ac.clip = std::make_unique<gp::GraphicsPath>();
      AddRoundRect(*ac.clip, art_x, art_y, art, art, art_r);
      ac.px = art_px; ac.x = art_x; ac.y = art_y; ac.r = art_r;
    }
    const gp::GraphicsPath& clip = *ac.clip;

    const gp::Color ph_col = Col(style.ink, 34.0f, content_a);
    if (!ac.placeholder) ac.placeholder = std::make_unique<gp::SolidBrush>(ph_col);
    else ac.placeholder->SetColor(ph_col);
    g.FillPath(ac.placeholder.get(), &clip);

    if (art_cur && !scaled_cur.Matches(art_cur, art_px)) scaled_cur.Build(art_cur, art_px);
    if (art_prev && !scaled_prev.Matches(art_prev, art_px)) scaled_prev.Build(art_prev, art_px);
    if (!art_cur) scaled_cur = ScaledArt{};
    if (!art_prev) scaled_prev = ScaledArt{};

    const float te = art_t < 1.0 ? static_cast<float>(EaseOutCubic(art_t)) : 1.0f;
    if (art_prev) DrawArtFill(g, scaled_prev, clip, art_x, art_y, (art_cur ? 1.0f : 1.0f - te) * content_a);
    if (art_cur)  DrawArtFill(g, scaled_cur, clip, art_x, art_y, te * content_a);

    const gp::Color st_col = Col(style.ink, 40.0f, content_a);
    if (!ac.stroke) ac.stroke = std::make_unique<gp::Pen>(st_col, 1.0f);
    else ac.stroke->SetColor(st_col);
    g.DrawPath(ac.stroke.get(), &clip);
  }

  // Text (with skip-direction slide)
  const float tx = static_cast<float>(std::lround((kPadLeft + kArtSize + kGap) * s));
  const float avail = W - tx - static_cast<float>(RightReservePx());

  if (slide_active) {
    const float te = static_cast<float>(EaseOutCubic(slide_t));
    const float off = (cfg.reduced_motion || !cfg.text_slide) ? 0.0f : static_cast<float>(kSlideOff * s * slide_dir);
    DrawTextBlock(g, tx, avail, H, -off * te, 1.0f - te, old_title, old_artist, old_tw, old_aw,
                  false, false, old_dim, nullptr, nullptr);
    DrawTextBlock(g, tx, avail, H, off * (1.0f - te), te, title_text, artist_text, title_w, artist_w,
                  title_over, artist_over, dim, &title_mq, &artist_mq);
  } else {
    DrawTextBlock(g, tx, avail, H, 0.0f, 1.0f, title_text, artist_text, title_w, artist_w,
                  title_over, artist_over, dim, &title_mq, &artist_mq);
  }

  DrawLyricLine(g, tx, avail, H);
  DrawNudge(g, tx, avail, H);
  DrawVolume(g, tx, avail, H);
  // The visualizer is NOT drawn here: it is the top layer, painted by DrawViz() after the static layer
  // has been snapshotted (see Render()).
}

// ---- layers ---------------------------------------------------------------
// Everything except the visualizer is the "static layer". Render() snapshots it after each full draw, so a
// visualizer-only frame (the common playing-with-no-other-animation case, ~30 fps) restores a small rect
// from the snapshot, redraws just the visualizer into it and tells DWM only that rect changed.

void PillWindow::Impl::SetupGraphics(gp::Graphics& g) const {
  g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
  g.SetInterpolationMode(gp::InterpolationModeHighQualityBicubic);
  g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
  g.SetTextRenderingHint(kCapsuleTextHint);   // no ClearType on alpha
  g.SetTextContrast(kCapsuleTextContrast);
}

void PillWindow::Impl::ApplyRootTransform(gp::Graphics& g) const {
  const float sc = RootScale();
  if (sc == 1.0f) return;
  const float cx = static_cast<float>(win_w) * 0.5f;
  const float cy = static_cast<float>(pill_off_y) + static_cast<float>(pill_h) * 0.5f;   // scale about the capsule
  g.TranslateTransform(cx, cy);
  g.ScaleTransform(sc, sc);
  g.TranslateTransform(-cx, -cy);
}

// Visualizer slot: fixed on the right, inside the pill (pill-local coordinates).
void PillWindow::Impl::VizOrigin(float& vx, float& vy) const {
  const double s = Scale();
  vx = static_cast<float>(pill_w) - static_cast<float>(std::lround(kPadRightViz * s)) -
       static_cast<float>(viz.SlotWidth());
  vy = (static_cast<float>(pill_h) - static_cast<float>(viz.SlotHeight())) * 0.5f;
}

// Window-pixel rect the visualizer is allowed to touch: its slot + a margin for the glow halo, carried
// through the hover scale, clamped to the window. The visualizer is clipped to it in EVERY frame, so the
// snapshot restore + dirty rect are exactly the pixels it can ever change.
RECT PillWindow::Impl::VizRect() const {
  float vx, vy;
  VizOrigin(vx, vy);
  const float sw = static_cast<float>(viz.SlotWidth()), sh = static_cast<float>(viz.SlotHeight());
  const float m = std::ceil(static_cast<float>(kVizMargin) * (std::max)(sw, sh)) + 2.0f;
  float x0 = static_cast<float>(pill_off_x) + vx - m, y0 = static_cast<float>(pill_off_y) + vy - m;
  float x1 = x0 + sw + 2.0f * m, y1 = y0 + sh + 2.0f * m;
  const float sc = RootScale();
  if (sc != 1.0f) {
    const float cx = static_cast<float>(win_w) * 0.5f;
    const float cy = static_cast<float>(pill_off_y) + static_cast<float>(pill_h) * 0.5f;
    x0 = cx + (x0 - cx) * sc; x1 = cx + (x1 - cx) * sc;
    y0 = cy + (y0 - cy) * sc; y1 = cy + (y1 - cy) * sc;
  }
  RECT r{ static_cast<LONG>(std::floor(x0)), static_cast<LONG>(std::floor(y0)),
          static_cast<LONG>(std::ceil(x1)),  static_cast<LONG>(std::ceil(y1)) };
  r.left = (std::clamp)(r.left, 0L, static_cast<LONG>(win_w));
  r.right = (std::clamp)(r.right, 0L, static_cast<LONG>(win_w));
  r.top = (std::clamp)(r.top, 0L, static_cast<LONG>(win_h));
  r.bottom = (std::clamp)(r.bottom, 0L, static_cast<LONG>(win_h));
  return r;
}

void PillWindow::Impl::DrawViz(gp::Graphics& g, const RECT& clip) {
  if (!viz.Enabled() || clip.right <= clip.left || clip.bottom <= clip.top) return;
  float vx, vy;
  VizOrigin(vx, vy);
  SetupGraphics(g);
  g.ResetTransform();
  g.SetClip(gp::Rect(clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top), gp::CombineModeReplace);
  ApplyRootTransform(g);
  g.TranslateTransform(static_cast<float>(pill_off_x), static_cast<float>(pill_off_y));
  viz.Draw(g, vx, vy, style.viz, style.viz_a);
  g.ResetTransform();
  g.ResetClip();
}

// Static layer: everything except the visualizer.
void PillWindow::Impl::Draw(gp::Graphics& g) {
  SetupGraphics(g);
  g.Clear(gp::Color(0, 0, 0, 0));

  // Hover scale: one transform for the whole capsule group, so text/art/strip scale together
  // (text is re-rendered at the scaled size, not stretched as a bitmap).
  const gp::GraphicsState root = g.Save();
  ApplyRootTransform(g);

  // 1) transport strip, BEHIND the main pill: its inner end is hidden under the pill
  if (Expand() > 0.01 && extra_px > 0) {
    const float a = static_cast<float>((std::clamp)(Expand(), 0.0, 1.0));
    gp::GraphicsPath pill_shape;
    AddPill(pill_shape, static_cast<float>(pill_off_x), static_cast<float>(pill_off_y), static_cast<float>(pill_w),
            static_cast<float>(pill_h));

    const gp::GraphicsState st = g.Save();
    g.SetClip(&pill_shape, gp::CombineModeExclude);   // never blend glass over glass
    g.TranslateTransform(0.0f, static_cast<float>(pill_off_y));
    strip.Draw(g, static_cast<float>(strip_x), a, style, CurrentTransport());
    if (seam_active) {
      g.TranslateTransform(static_cast<float>(strip_x), 0.0f);
      DrawSeam(g, a);
    }
    g.Restore(st);

    // soft contact shadow where the main pill sits on the strip
    const gp::GraphicsState st2 = g.Save();
    g.SetClip(&pill_shape, gp::CombineModeExclude);
    for (int i = 3; i >= 1; --i) {
      gp::Pen sh(gp::Color(static_cast<BYTE>(10 + (4 - i) * 8 * a), 0, 0, 0), static_cast<gp::REAL>(i * 2));
      g.DrawPath(&sh, &pill_shape);
    }
    g.Restore(st2);
  }

  // 2) main capsule on top (no scaling: everything stays on whole pixels)
  {
    const gp::GraphicsState st = g.Save();
    g.TranslateTransform(static_cast<float>(pill_off_x), static_cast<float>(pill_off_y));
    DrawSurface(g, static_cast<float>(pill_w), static_cast<float>(pill_h));
    DrawPill(g);
    g.Restore(st);
  }

  // 3) lyrics mode: the buttons ride on the tab, clipped to it so they slide up out of the capsule
  if (ExpandUp() > 0.01 && up_extra_px > 0 && pill_w - pill_h > 8) {
    const float a = static_cast<float>((std::clamp)(ExpandUp(), 0.0, 1.0));
    const float tw = UpTabWidth();
    const float tx0 = static_cast<float>(pill_off_x) + (static_cast<float>(pill_w) - tw) * 0.5f;
    const gp::GraphicsState st = g.Save();
    g.SetClip(gp::RectF(tx0, 0.0f, tw, static_cast<float>(pill_off_y)), gp::CombineModeIntersect);
    strip.DrawUpButtons(g, tx0, static_cast<float>(pill_off_y - up_extra_px), tw, a, style, CurrentTransport());
    g.Restore(st);
  }

  g.Restore(root);
}

auto PillWindow::Impl::MakeKey() const -> SnapKey {
  SnapKey k;
  k.win_w = win_w; k.win_h = win_h; k.off_x = pill_off_x; k.off_y = pill_off_y;
  k.pill_w = pill_w; k.pill_h = pill_h; k.dpi = dpi;
  k.hov = static_cast<float>(hov_sp.x);
  k.acc_r = acc_cur.r; k.acc_g = acc_cur.g; k.acc_b = acc_cur.b; k.str = str_cur;
  return k;
}

void PillWindow::Impl::ReleaseTarget() {   // GDI+ objects must go before GdiplusShutdown
  snap_valid = false;
  ul_valid = false;
  std::vector<uint32_t>().swap(snap);
  rt_g.reset();
  rt_bmp.reset();
  if (rt_dc && rt_old) SelectObject(rt_dc, rt_old);
  if (rt_dib) DeleteObject(rt_dib);
  if (rt_dc) DeleteDC(rt_dc);
  rt_dc = nullptr; rt_dib = nullptr; rt_old = nullptr; rt_bits = nullptr;
  rt_cap_w = rt_cap_h = rt_w = rt_h = 0;
}

bool PillWindow::Impl::EnsureTarget() {
  if (rt_g && rt_w == win_w && rt_h == win_h) return true;

  // (Re)allocate the DIB only when the window outgrew it, or it is wastefully large (> 2x in an axis).
  const bool too_small = win_w > rt_cap_w || win_h > rt_cap_h;
  const bool too_big = rt_cap_w > win_w * 2 + 128 || rt_cap_h > win_h * 2 + 64;
  if (!rt_dib || too_small || too_big) {
    ReleaseTarget();
    const int cw = ((win_w + 63) / 64) * 64 + 64;   // headroom for the capsule growing while it animates
    const int ch = ((win_h + 15) / 16) * 16 + 16;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = cw;
    bmi.bmiHeader.biHeight = -ch;   // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    rt_dc = CreateCompatibleDC(nullptr);   // memory DC compatible with the screen
    if (rt_dc) rt_dib = CreateDIBSection(rt_dc, &bmi, DIB_RGB_COLORS, &rt_bits, nullptr, 0);
    if (!rt_dc || !rt_dib || !rt_bits) {
      ReleaseTarget();
      return false;
    }
    rt_old = SelectObject(rt_dc, rt_dib);
    rt_cap_w = cw;
    rt_cap_h = ch;
  }

  // Wrapper over the top-left win_w x win_h part of the DIB (stride = the full DIB row).
  rt_g.reset();
  rt_bmp.reset();
  rt_bmp = std::make_unique<gp::Bitmap>(win_w, win_h, rt_cap_w * 4, PixelFormat32bppPARGB,
                                        static_cast<BYTE*>(rt_bits));
  rt_g = std::make_unique<gp::Graphics>(rt_bmp.get());
  rt_w = win_w;
  rt_h = win_h;
  return true;
}

// viz_only: the caller knows nothing but the visualizer changed since the last frame. Render() still
// verifies that against the snapshot key and falls back to a full draw if anything differs.
void PillWindow::Impl::Render(bool viz_only) {
  if (!hwnd || win_w <= 0 || win_h <= 0 || !fonts) return;
  if (!EnsureTarget()) return;

  const SnapKey key = MakeKey();
  const RECT vr = VizRect();
  const bool partial = viz_only && viz.Enabled() && snap_valid && !static_dirty && key == snap_key &&
                       vr.right > vr.left && vr.bottom > vr.top;

  BYTE* const base = static_cast<BYTE*>(rt_bits);
  const size_t stride = static_cast<size_t>(rt_cap_w) * 4;

  if (partial) {
    // Put the static pixels back under the visualizer, then redraw only the visualizer, clipped to that rect.
    const size_t bytes = static_cast<size_t>(vr.right - vr.left) * 4;
    for (int y = vr.top; y < vr.bottom; ++y) {
      std::memcpy(base + static_cast<size_t>(y) * stride + static_cast<size_t>(vr.left) * 4,
                  snap.data() + static_cast<size_t>(y) * win_w + vr.left, bytes);
    }
    DrawViz(*rt_g, vr);
  } else {
    rt_g->ResetTransform();
    rt_g->ResetClip();
    Draw(*rt_g);   // starts with g.Clear(transparent) over the whole window: no separate memset needed
    GdiFlush();

    snap.resize(static_cast<size_t>(win_w) * win_h);
    for (int y = 0; y < win_h; ++y)
      std::memcpy(snap.data() + static_cast<size_t>(y) * win_w, base + static_cast<size_t>(y) * stride,
                  static_cast<size_t>(win_w) * 4);
    snap_key = key;
    snap_valid = true;
    static_dirty = false;

    DrawViz(*rt_g, vr);
  }
  GdiFlush();

  POINT dst{ win_x, win_y };
  POINT src{ 0, 0 };
  SIZE size{ win_w, win_h };
  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = static_cast<BYTE>((std::clamp)(vis, 0.0, 1.0) * 255.0 + 0.5);
  blend.AlphaFormat = AC_SRC_ALPHA;

  // Same position, size and alpha as the previous update => DWM only needs to take the visualizer rect.
  const bool use_dirty = partial && ul_valid && ul_x == win_x && ul_y == win_y && ul_w == win_w &&
                         ul_h == win_h && ul_alpha == blend.SourceConstantAlpha;
  RECT dirty = vr;
  UPDATELAYEREDWINDOWINFO info{};
  info.cbSize = sizeof(info);
  info.pptDst = &dst;
  info.psize = &size;
  info.hdcSrc = rt_dc;
  info.pptSrc = &src;
  info.pblend = &blend;
  info.dwFlags = ULW_ALPHA;
  info.prcDirty = use_dirty ? &dirty : nullptr;
  ul_valid = UpdateLayeredWindowIndirect(hwnd, &info) != FALSE;
  ul_x = win_x; ul_y = win_y; ul_w = win_w; ul_h = win_h; ul_alpha = blend.SourceConstantAlpha;
}

} // namespace island
