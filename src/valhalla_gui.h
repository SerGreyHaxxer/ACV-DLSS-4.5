#pragma once
// ============================================================================
// Valhalla GUI — Custom D2D rendering backend + immediate-mode widget system
// Zero third-party GUI dependencies. Uses Windows D3D11On12 + Direct2D + DirectWrite.
// ============================================================================
#include <d3d12.h>
#include <d3d11on12.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// THEME — Assassin's Creed Valhalla inspired color palette
// ============================================================================
namespace vtheme {

inline D2D1_COLOR_F rgba(float r, float g, float b, float a = 1.0f) {
  return {r, g, b, a};
}
inline D2D1_COLOR_F hex(uint32_t c, float a = 1.0f) {
  return {((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f,
          (c & 0xFF) / 255.0f, a};
}

// Primary palette — Nordic night
inline const auto kBgDeep      = hex(0x0A0B14, 0.96f);   // Deepest background
inline const auto kBgPanel     = hex(0x12131E, 0.94f);   // Panel background
inline const auto kBgSection   = hex(0x1A1B2A, 1.0f);    // Section header bg
inline const auto kBgWidget    = hex(0x1E1F30, 1.0f);    // Widget background
inline const auto kBgHover     = hex(0x2A2C40, 1.0f);    // Hover state
inline const auto kBgActive    = hex(0x353850, 1.0f);    // Active/pressed

// Gold accents — Norse gold
inline const auto kGold        = hex(0xD4AF37, 1.0f);    // Primary accent
inline const auto kGoldBright  = hex(0xF0D060, 1.0f);    // Hover/active accent
inline const auto kGoldDim     = hex(0x8B7530, 0.6f);    // Borders, inactive

// Text
inline const auto kTextPrimary = hex(0xE2D8C4, 1.0f);    // Main text (parchment)
inline const auto kTextSecondary = hex(0x9B9484, 1.0f);  // Muted text
inline const auto kTextGold    = hex(0xD4AF37, 1.0f);    // Highlighted text

// Status
inline const auto kStatusOk    = hex(0x4CAF50, 1.0f);    // Green
inline const auto kStatusWarn  = hex(0xFFB300, 1.0f);    // Amber
inline const auto kStatusBad   = hex(0xEF5350, 1.0f);    // Red

// Slider
inline const auto kSliderTrack = hex(0x2A2B3D, 1.0f);
inline const auto kSliderFill  = hex(0xD4AF37, 1.0f);
inline const auto kSliderGrab  = hex(0xF0D060, 1.0f);

// Scrollbar
inline const auto kScrollBg    = hex(0x0A0B14, 0.5f);
inline const auto kScrollThumb = hex(0x3A3B4D, 1.0f);

// Sizes
inline constexpr float kPanelWidth     = 520.0f;
inline constexpr float kTitleBarHeight = 44.0f;
inline constexpr float kStatusBarHeight= 32.0f;
inline constexpr float kWidgetHeight   = 28.0f;
inline constexpr float kSpacing        = 6.0f;
inline constexpr float kPadding        = 16.0f;
inline constexpr float kCornerRadius   = 6.0f;
inline constexpr float kSectionHeight  = 32.0f;
inline constexpr float kSliderGrabW    = 14.0f;
inline constexpr float kScrollbarW     = 8.0f;
inline constexpr float kCheckboxSize   = 20.0f;
inline constexpr float kComboHeight    = 28.0f;

// Font sizes
inline constexpr float kFontTitle      = 20.0f;
inline constexpr float kFontSection    = 15.0f;
inline constexpr float kFontBody       = 13.0f;
inline constexpr float kFontSmall      = 11.0f;
inline constexpr float kFontFPS        = 36.0f;
inline constexpr float kFontFPSLabel   = 13.0f;

// Animation timing
inline constexpr float kAnimOpenDuration  = 0.35f;  // seconds
inline constexpr float kAnimCloseDuration = 0.25f;
inline constexpr float kAnimHoverDuration = 0.12f;

} // namespace vtheme

// ============================================================================
// ANIMATION TYPES
// ============================================================================
enum class AnimType : int {
  SlideLeft = 0, SlideRight, SlideTop, SlideBottom,
  Fade, Scale, Bounce, Elastic, Count
};

inline const char* AnimTypeNames[] = {
  "Slide Left", "Slide Right", "Slide Top", "Slide Bottom",
  "Fade", "Scale", "Bounce", "Elastic"
};

// ============================================================================
// FPS Position / Style enums
// ============================================================================
enum class FPSPosition : int { TopRight = 0, TopLeft, BottomRight, BottomLeft, Count };
inline const char* FPSPositionNames[] = { "Top Right", "Top Left", "Bottom Right", "Bottom Left" };

enum class FPSStyle : int { Standard = 0, Minimal, Detailed, Count };
inline const char* FPSStyleNames[] = { "Standard", "Minimal", "Detailed" };

enum class LayoutMode : int { Compact = 0, Normal, Expanded, Count };
inline const char* LayoutModeNames[] = { "Compact", "Normal", "Expanded" };

// ============================================================================
// ANIMATION UTILITIES
// ============================================================================
namespace vanim {

constexpr float PI = 3.14159265358979323846f;

inline float EaseOutCubic(float t) {
  t = 1.0f - t;
  return 1.0f - t * t * t;
}
inline float EaseInCubic(float t) { return t * t * t; }
inline float EaseInOutCubic(float t) {
  return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) / 2.0f;
}
inline float EaseOutQuint(float t) { float u = 1.0f - t; return 1.0f - u * u * u * u * u; }
inline float EaseOutBack(float t) {
  constexpr float c1 = 1.70158f, c3 = c1 + 1.0f;
  return 1.0f + c3 * std::pow(t - 1.0f, 3.0f) + c1 * std::pow(t - 1.0f, 2.0f);
}
inline float EaseBounce(float t) {
  if (t < 1.0f / 2.75f) return 7.5625f * t * t;
  if (t < 2.0f / 2.75f) { t -= 1.5f / 2.75f; return 7.5625f * t * t + 0.75f; }
  if (t < 2.5f / 2.75f) { t -= 2.25f / 2.75f; return 7.5625f * t * t + 0.9375f; }
  t -= 2.625f / 2.75f; return 7.5625f * t * t + 0.984375f;
}
inline float EaseElastic(float t) {
  if (t <= 0.0f || t >= 1.0f) return t;
  return std::pow(2.0f, -10.0f * t) * std::sin((t - 0.075f) * (2.0f * PI) / 0.3f) + 1.0f;
}
inline float EaseOutExpo(float t) {
  return t >= 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t);
}
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float SmoothDamp(float current, float target, float speed, float dt) {
  return Lerp(current, target, 1.0f - std::exp(-speed * dt));
}

struct AnimatedFloat {
  float current = 0.0f;
  float target = 0.0f;
  float start = 0.0f;
  float startTime = -1.0f;
  float duration = 0.3f;
  bool opening = true;

  void SetTarget(float t, float dur, bool isOpening = true) {
    if (std::abs(target - t) < 0.001f) return;
    start = current;
    target = t;
    duration = dur;
    opening = isOpening;
    startTime = -1.0f; // signal to capture time on next update
  }

  void Update(float globalTime) {
    if (startTime < 0.0f) startTime = globalTime;
    float elapsed = globalTime - startTime;
    float progress = (duration > 0.0f) ? std::clamp(elapsed / duration, 0.0f, 1.0f) : 1.0f;
    float eased = opening ? EaseOutCubic(progress) : EaseInCubic(progress);
    current = Lerp(start, target, eased);
  }

  [[nodiscard]] bool IsAnimating() const {
    return std::abs(current - target) > 0.001f;
  }
};

} // namespace vanim

// ============================================================================
// ValhallaRenderer — D3D11On12 + Direct2D rendering backend
// ============================================================================
class ValhallaRenderer {
public:
  bool Initialize(ID3D12Device* d3d12Device, ID3D12CommandQueue* cmdQueue,
                  IDXGISwapChain3* swapChain, UINT bufferCount);
  void Shutdown();
  bool BeginFrame(UINT backBufferIndex);
  void EndFrame();
  void OnResize();

  // Drawing primitives
  void FillRect(float x, float y, float w, float h, const D2D1_COLOR_F& color);
  void FillRoundedRect(float x, float y, float w, float h, float r, const D2D1_COLOR_F& color);
  void OutlineRoundedRect(float x, float y, float w, float h, float r, const D2D1_COLOR_F& color, float thick = 1.0f);
  void FillGradientV(float x, float y, float w, float h, const D2D1_COLOR_F& top, const D2D1_COLOR_F& bottom);
  void DrawLine(float x1, float y1, float x2, float y2, const D2D1_COLOR_F& color, float thick = 1.0f);
  void DrawDiamond(float cx, float cy, float size, const D2D1_COLOR_F& color);
  void DrawCircle(float cx, float cy, float radius, const D2D1_COLOR_F& color);

  // Text
  enum class TextAlign { Left, Center, Right };
  void DrawText(const std::wstring& text, float x, float y, float w, float h,
                const D2D1_COLOR_F& color, float fontSize, TextAlign align = TextAlign::Left, bool bold = false);
  void DrawTextA(const std::string& text, float x, float y, float w, float h,
                 const D2D1_COLOR_F& color, float fontSize, TextAlign align = TextAlign::Left, bool bold = false);
  struct TextSize { float width; float height; };
  TextSize MeasureTextA(const std::string& text, float fontSize, bool bold = false, float maxWidth = 10000.0f);

  // Clipping
  void PushClip(float x, float y, float w, float h);
  void PopClip();

  // Vignette (radial gradient)
  void DrawVignette(float screenW, float screenH, float r, float g, float b, float intensity, float radius, float softness);

  // State
  [[nodiscard]] bool IsValid() const { return m_d2dContext != nullptr; }

private:
  void CreateRenderTargets(IDXGISwapChain3* swapChain, UINT count);
  void ReleaseRenderTargets();
  ID2D1SolidColorBrush* GetBrush(const D2D1_COLOR_F& color);
  IDWriteTextFormat* GetTextFormat(float fontSize, bool bold);

  Microsoft::WRL::ComPtr<ID3D11Device>          m_d3d11Device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext>    m_d3d11Context;
  Microsoft::WRL::ComPtr<ID3D11On12Device>       m_d3d11On12Device;
  Microsoft::WRL::ComPtr<ID2D1Factory1>          m_d2dFactory;
  Microsoft::WRL::ComPtr<ID2D1Device>            m_d2dDevice;
  Microsoft::WRL::ComPtr<ID2D1DeviceContext>     m_d2dContext;
  Microsoft::WRL::ComPtr<IDWriteFactory>         m_dwriteFactory;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>   m_brush;

  struct PerBuffer {
    Microsoft::WRL::ComPtr<ID3D11Resource>  wrappedResource;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1>    d2dTarget;
  };
  std::vector<PerBuffer> m_buffers;
  int m_currentBuffer = -1;

  // Text format cache (keyed by fontSize*100 + bold)
  std::unordered_map<int, Microsoft::WRL::ComPtr<IDWriteTextFormat>> m_textFormats;
};

// ============================================================================
// Widget ID system
// ============================================================================
inline uint32_t VGuiHash(const char* str) {
  uint32_t hash = 2166136261u;
  while (*str) { hash ^= static_cast<uint32_t>(*str++); hash *= 16777619u; }
  return hash;
}

// ============================================================================
// Input state for the GUI
// ============================================================================
struct VGuiInput {
  float mouseX = 0, mouseY = 0;
  bool mouseDown = false;
  bool mouseClicked = false;   // just pressed this frame
  bool mouseReleased = false;  // just released this frame
  float scrollDelta = 0.0f;
};
