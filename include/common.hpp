#pragma once

#include "pch.h"

namespace island {

// Version
constexpr const char* kVersion = "0.1.0";

// Capsule layout constants (Phase 0 defaults)
constexpr double kPillMinWidth   = 120.0;
constexpr double kPillMaxWidth   = 320.0;
constexpr double kPillHeight     = 36.0;   // will adapt to taskbar height
constexpr double kArtSize        = 28.0;
constexpr double kVizSlotWidth   = 32.0;  // right-side visualizer slot
constexpr double kCornerRadius   = 18.0;  // ~50% of height

// Colors (neutral glass base – Phase 3 will override with album tint)
namespace colors {
  constexpr uint32_t kDarkBase   = 0x202024;
  constexpr uint32_t kLightBase  = 0xF4F4F6;
  constexpr float    kDarkAlpha  = 0.60f;
  constexpr float    kLightAlpha = 0.40f;
}

// Forward declarations
class TaskbarAttach;
class MediaSession;
class CapsuleUI;
class Visualizer;
class AccentColor;
class Settings;

} // namespace island
