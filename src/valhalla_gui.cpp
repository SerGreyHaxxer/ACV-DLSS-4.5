#include "valhalla_gui.h"
#include "logger.h"
#include <cstring>
#include <locale>
#include <codecvt>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

using Microsoft::WRL::ComPtr;

// ============================================================================
// ValhallaRenderer — D3D11On12 + Direct2D backend
// ============================================================================

bool ValhallaRenderer::Initialize(ID3D12Device* d3d12Device, ID3D12CommandQueue* cmdQueue,
                                  IDXGISwapChain3* swapChain, UINT bufferCount) {
  if (!d3d12Device || !cmdQueue || !swapChain || bufferCount == 0) return false;

  // --- Step 1: Create D3D11On12 device ---
  UINT d3d11Flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  IUnknown* queues[] = { cmdQueue };
  HRESULT hr = D3D11On12CreateDevice(
    d3d12Device, d3d11Flags, nullptr, 0,
    queues, 1, 0,
    &m_d3d11Device, &m_d3d11Context, nullptr);
  if (FAILED(hr)) {
    LOG_ERROR("[ValhallaGUI] D3D11On12CreateDevice failed: 0x{:08X}", (unsigned)hr);
    return false;
  }

  hr = m_d3d11Device.As(&m_d3d11On12Device);
  if (FAILED(hr)) {
    LOG_ERROR("[ValhallaGUI] QueryInterface ID3D11On12Device failed");
    return false;
  }

  // --- Step 2: Create D2D1 Factory + Device ---
  hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.GetAddressOf());
  if (FAILED(hr)) {
    LOG_ERROR("[ValhallaGUI] D2D1CreateFactory failed");
    return false;
  }

  ComPtr<IDXGIDevice> dxgiDevice;
  hr = m_d3d11Device.As(&dxgiDevice);
  if (FAILED(hr)) return false;

  hr = m_d2dFactory->CreateDevice(dxgiDevice.Get(), &m_d2dDevice);
  if (FAILED(hr)) {
    LOG_ERROR("[ValhallaGUI] D2D1 CreateDevice failed");
    return false;
  }

  hr = m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_d2dContext);
  if (FAILED(hr)) {
    LOG_ERROR("[ValhallaGUI] D2D1 CreateDeviceContext failed");
    return false;
  }

  m_d2dContext->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  m_d2dContext->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

  // --- Step 3: Create DirectWrite factory ---
  hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                           __uuidof(IDWriteFactory),
                           reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf()));
  if (FAILED(hr)) {
    LOG_ERROR("[ValhallaGUI] DWriteCreateFactory failed");
    return false;
  }

  // --- Step 4: Create brush ---
  hr = m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &m_brush);
  if (FAILED(hr)) return false;

  // --- Step 5: Create per-buffer render targets ---
  CreateRenderTargets(swapChain, bufferCount);

  LOG_INFO("[ValhallaGUI] D2D renderer initialized ({} buffers)", bufferCount);
  return true;
}

void ValhallaRenderer::Shutdown() {
  m_textFormats.clear();
  m_brush.Reset();
  ReleaseRenderTargets();
  m_d2dContext.Reset();
  m_d2dDevice.Reset();
  m_d2dFactory.Reset();
  m_dwriteFactory.Reset();
  m_d3d11On12Device.Reset();
  m_d3d11Context.Reset();
  m_d3d11Device.Reset();
  LOG_INFO("[ValhallaGUI] Renderer shutdown");
}

void ValhallaRenderer::CreateRenderTargets(IDXGISwapChain3* swapChain, UINT count) {
  ReleaseRenderTargets();
  m_buffers.resize(count);

  for (UINT i = 0; i < count; ++i) {
    ComPtr<ID3D12Resource> d3d12Buffer;
    HRESULT hr = swapChain->GetBuffer(i, IID_PPV_ARGS(&d3d12Buffer));
    if (FAILED(hr)) continue;

    D3D11_RESOURCE_FLAGS flags = {};
    flags.BindFlags = D3D11_BIND_RENDER_TARGET;

    hr = m_d3d11On12Device->CreateWrappedResource(
      d3d12Buffer.Get(), &flags,
      D3D12_RESOURCE_STATE_PRESENT,
      D3D12_RESOURCE_STATE_PRESENT,
      IID_PPV_ARGS(&m_buffers[i].wrappedResource));
    if (FAILED(hr)) {
      LOG_WARN("[ValhallaGUI] CreateWrappedResource failed for buffer {}", i);
      continue;
    }

    ComPtr<IDXGISurface> surface;
    hr = m_buffers[i].wrappedResource.As(&surface);
    if (FAILED(hr)) continue;

    // Query surface format
    DXGI_SURFACE_DESC surfDesc{};
    surface->GetDesc(&surfDesc);
    D2D1_PIXEL_FORMAT pixelFormat = D2D1::PixelFormat(surfDesc.Format, D2D1_ALPHA_MODE_PREMULTIPLIED);

    D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
      pixelFormat);

    hr = m_d2dContext->CreateBitmapFromDxgiSurface(surface.Get(), &bmpProps, &m_buffers[i].d2dTarget);
    if (FAILED(hr)) {
      LOG_WARN("[ValhallaGUI] CreateBitmapFromDxgiSurface failed for buffer {}: 0x{:08X}", i, (unsigned)hr);
    }
  }
}

void ValhallaRenderer::ReleaseRenderTargets() {
  for (auto& buf : m_buffers) {
    buf.d2dTarget.Reset();
    buf.wrappedResource.Reset();
  }
  m_buffers.clear();
  m_currentBuffer = -1;
}

void ValhallaRenderer::OnResize() {
  // Caller must recreate render targets after swap chain resize
  ReleaseRenderTargets();
}

bool ValhallaRenderer::BeginFrame(UINT backBufferIndex) {
  if (backBufferIndex >= m_buffers.size()) return false;
  auto& buf = m_buffers[backBufferIndex];
  if (!buf.wrappedResource || !buf.d2dTarget) return false;

  m_currentBuffer = static_cast<int>(backBufferIndex);

  // Acquire the wrapped D3D12 resource for D3D11 use
  ID3D11Resource* resources[] = { buf.wrappedResource.Get() };
  m_d3d11On12Device->AcquireWrappedResources(resources, 1);

  // Set the D2D render target
  m_d2dContext->SetTarget(buf.d2dTarget.Get());
  m_d2dContext->BeginDraw();
  return true;
}

void ValhallaRenderer::EndFrame() {
  if (m_currentBuffer < 0) return;

  m_d2dContext->EndDraw();
  m_d2dContext->SetTarget(nullptr);

  // Release the wrapped resource back to D3D12
  auto& buf = m_buffers[static_cast<size_t>(m_currentBuffer)];
  ID3D11Resource* resources[] = { buf.wrappedResource.Get() };
  m_d3d11On12Device->ReleaseWrappedResources(resources, 1);

  // Flush D3D11 context to submit all D2D commands
  m_d3d11Context->Flush();
  m_currentBuffer = -1;
}

// ============================================================================
// Drawing primitives
// ============================================================================

ID2D1SolidColorBrush* ValhallaRenderer::GetBrush(const D2D1_COLOR_F& color) {
  m_brush->SetColor(color);
  return m_brush.Get();
}

void ValhallaRenderer::FillRect(float x, float y, float w, float h, const D2D1_COLOR_F& color) {
  m_d2dContext->FillRectangle(D2D1::RectF(x, y, x + w, y + h), GetBrush(color));
}

void ValhallaRenderer::FillRoundedRect(float x, float y, float w, float h, float r, const D2D1_COLOR_F& color) {
  D2D1_ROUNDED_RECT rr = { D2D1::RectF(x, y, x + w, y + h), r, r };
  m_d2dContext->FillRoundedRectangle(rr, GetBrush(color));
}

void ValhallaRenderer::OutlineRoundedRect(float x, float y, float w, float h, float r, const D2D1_COLOR_F& color, float thick) {
  D2D1_ROUNDED_RECT rr = { D2D1::RectF(x, y, x + w, y + h), r, r };
  m_d2dContext->DrawRoundedRectangle(rr, GetBrush(color), thick);
}

void ValhallaRenderer::FillGradientV(float x, float y, float w, float h,
                                     const D2D1_COLOR_F& top, const D2D1_COLOR_F& bottom) {
  // Create a linear gradient brush for vertical gradient
  D2D1_GRADIENT_STOP stops[2] = {};
  stops[0] = { 0.0f, top };
  stops[1] = { 1.0f, bottom };

  ComPtr<ID2D1GradientStopCollection> stopCollection;
  m_d2dContext->CreateGradientStopCollection(stops, 2, &stopCollection);
  if (!stopCollection) { FillRect(x, y, w, h, top); return; }

  ComPtr<ID2D1LinearGradientBrush> gradBrush;
  D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES props = {};
  props.startPoint = D2D1::Point2F(x, y);
  props.endPoint = D2D1::Point2F(x, y + h);
  m_d2dContext->CreateLinearGradientBrush(props, stopCollection.Get(), &gradBrush);
  if (!gradBrush) { FillRect(x, y, w, h, top); return; }

  m_d2dContext->FillRectangle(D2D1::RectF(x, y, x + w, y + h), gradBrush.Get());
}

void ValhallaRenderer::DrawLine(float x1, float y1, float x2, float y2, const D2D1_COLOR_F& color, float thick) {
  m_d2dContext->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), GetBrush(color), thick);
}

void ValhallaRenderer::DrawDiamond(float cx, float cy, float size, const D2D1_COLOR_F& color) {
  // Rotate a square 45 degrees to make a diamond
  auto oldTransform = D2D1::Matrix3x2F::Identity();
  m_d2dContext->GetTransform(&oldTransform);
  m_d2dContext->SetTransform(
    D2D1::Matrix3x2F::Rotation(45.0f, D2D1::Point2F(cx, cy)) * oldTransform);
  float hs = size * 0.707f;
  m_d2dContext->FillRectangle(D2D1::RectF(cx - hs, cy - hs, cx + hs, cy + hs), GetBrush(color));
  m_d2dContext->SetTransform(oldTransform);
}

void ValhallaRenderer::DrawCircle(float cx, float cy, float radius, const D2D1_COLOR_F& color) {
  m_d2dContext->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), GetBrush(color));
}

// ============================================================================
// Text rendering
// ============================================================================

IDWriteTextFormat* ValhallaRenderer::GetTextFormat(float fontSize, bool bold) {
  int key = static_cast<int>(fontSize * 100.0f) + (bold ? 1 : 0);
  auto it = m_textFormats.find(key);
  if (it != m_textFormats.end()) return it->second.Get();

  ComPtr<IDWriteTextFormat> fmt;
  HRESULT hr = m_dwriteFactory->CreateTextFormat(
    L"Segoe UI",
    nullptr,
    bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_REGULAR,
    DWRITE_FONT_STYLE_NORMAL,
    DWRITE_FONT_STRETCH_NORMAL,
    fontSize,
    L"en-us",
    &fmt);
  if (FAILED(hr)) return nullptr;

  fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  m_textFormats[key] = fmt;
  return fmt.Get();
}

void ValhallaRenderer::DrawText(const std::wstring& text, float x, float y, float w, float h,
                                const D2D1_COLOR_F& color, float fontSize, TextAlign align, bool bold) {
  auto* fmt = GetTextFormat(fontSize, bold);
  if (!fmt) return;

  DWRITE_TEXT_ALIGNMENT dtAlign = DWRITE_TEXT_ALIGNMENT_LEADING;
  if (align == TextAlign::Center) dtAlign = DWRITE_TEXT_ALIGNMENT_CENTER;
  else if (align == TextAlign::Right) dtAlign = DWRITE_TEXT_ALIGNMENT_TRAILING;
  fmt->SetTextAlignment(dtAlign);
  fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

  m_d2dContext->DrawText(text.c_str(), static_cast<UINT32>(text.length()),
                         fmt, D2D1::RectF(x, y, x + w, y + h), GetBrush(color));
}

void ValhallaRenderer::DrawTextA(const std::string& text, float x, float y, float w, float h,
                                 const D2D1_COLOR_F& color, float fontSize, TextAlign align, bool bold) {
  // Convert to wide string
  int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  if (len <= 0) return;
  std::wstring wide(static_cast<size_t>(len - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), len);
  DrawText(wide, x, y, w, h, color, fontSize, align, bold);
}

ValhallaRenderer::TextSize ValhallaRenderer::MeasureTextA(const std::string& text, float fontSize, bool bold, float maxWidth) {
  auto* fmt = GetTextFormat(fontSize, bold);
  if (!fmt) return {0, 0};

  int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  if (len <= 0) return {0, 0};
  std::wstring wide(static_cast<size_t>(len - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), len);

  ComPtr<IDWriteTextLayout> layout;
  HRESULT hr = m_dwriteFactory->CreateTextLayout(wide.c_str(), static_cast<UINT32>(wide.length()),
                                                  fmt, maxWidth, 1000.0f, &layout);
  if (FAILED(hr)) return {0, 0};

  DWRITE_TEXT_METRICS metrics{};
  layout->GetMetrics(&metrics);
  return { metrics.width, metrics.height };
}

// ============================================================================
// Clipping
// ============================================================================

void ValhallaRenderer::PushClip(float x, float y, float w, float h) {
  m_d2dContext->PushAxisAlignedClip(D2D1::RectF(x, y, x + w, y + h), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
}

void ValhallaRenderer::PopClip() {
  m_d2dContext->PopAxisAlignedClip();
}

// ============================================================================
// Vignette — radial gradient drawn with D2D
// ============================================================================

void ValhallaRenderer::DrawVignette(float screenW, float screenH,
                                    float r, float g, float b,
                                    float intensity, float radius, float softness) {
  float cx = screenW * 0.5f;
  float cy = screenH * 0.5f;
  float maxR = std::sqrt(cx * cx + cy * cy);
  float innerR = maxR * radius;
  float outerR = maxR * std::clamp(radius + (1.0f - radius) * softness, radius + 0.001f, 1.0f);

  D2D1_GRADIENT_STOP stops[2] = {};
  stops[0] = { innerR / maxR, D2D1::ColorF(r, g, b, 0.0f) };
  stops[1] = { 1.0f, D2D1::ColorF(r, g, b, intensity) };

  ComPtr<ID2D1GradientStopCollection> stopCollection;
  m_d2dContext->CreateGradientStopCollection(stops, 2, &stopCollection);
  if (!stopCollection) return;

  ComPtr<ID2D1RadialGradientBrush> vignetteBrush;
  D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES radialProps = {};
  radialProps.center = D2D1::Point2F(cx, cy);
  radialProps.radiusX = maxR;
  radialProps.radiusY = maxR;
  m_d2dContext->CreateRadialGradientBrush(radialProps, stopCollection.Get(), &vignetteBrush);
  if (!vignetteBrush) return;

  m_d2dContext->FillRectangle(D2D1::RectF(0, 0, screenW, screenH), vignetteBrush.Get());
}
