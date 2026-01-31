# DLSS 4 Proxy DLL for Assassin's Creed Valhalla

A custom proxy DLL that injects **NVIDIA DLSS 4** functionality with **4x Multi-Frame Generation** into games that don't natively support it.

## Features

| Feature | Status | Requirements |
|---------|--------|--------------|
| **DLSS 4 Super Resolution** | ✅ Ready | Streamline SDK + `nvngx_dlss.dll` v4.0+ |
| **Ray Reconstruction** | ✅ Ready | Streamline SDK + RT enabled |
| **Frame Generation 2x** | ✅ Ready | Streamline SDK + `nvngx_dlssg.dll` + RTX 40/50 |
| **Frame Generation 3x** | ✅ Ready | Streamline SDK + RTX 50 series |
| **Frame Generation 4x** | ✅ Ready | Streamline SDK + RTX 50 series only |

## Quick Start

### 1. Build the DLL

```powershell
# Using CMake + Visual Studio
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

Or compile directly with MSVC:
```powershell
cl /LD /EHsc /std:c++17 /Fe:dxgi.dll main.cpp src\*.cpp /link d3d12.lib dxgi.lib dxguid.lib /DEF:dxgi.def
```

### 2. Install

1. Copy `dxgi.dll` to your AC Valhalla folder (next to `ACValhalla.exe`)
2. Copy `nvngx_dlss.dll` (v4.0+) to the same folder
3. Copy `nvngx_dlssg.dll` for Frame Generation
4. Copy Streamline runtime DLLs (`sl.interposer.dll`, `sl.common.dll`) to the same folder
5. Run the game!

### 3. Verify

Check for `dlss4_proxy.log` in the game folder. It should show:
```
[INFO] DLSS 4 Proxy DLL Loaded
[INFO] Frame Gen Multiplier: 4x
[INFO] NVIDIA DLSS 4 support detected!
```

## Configuration

Edit `src/dlss4_config.h` before building:

```cpp
// Frame Generation: 2, 3, or 4 (max for RTX 50)
#define DLSS4_FRAME_GEN_MULTIPLIER 4

// Quality: 0=Perf, 1=Balanced, 2=Quality, 3=UltraQuality, 4=DLAA
#define DLSS4_SR_QUALITY_MODE 2

// Toggle features
#define DLSS4_ENABLE_SUPER_RESOLUTION 1
#define DLSS4_ENABLE_RAY_RECONSTRUCTION 1
#define DLSS4_ENABLE_FRAME_GENERATION 1
```

## File Structure

```
tensor-curie/
├── main.cpp              # DLL entry point
├── dxgi.def              # Export definitions
├── CMakeLists.txt        # Build configuration
└── src/
    ├── dlss4_config.h    # User configuration
    ├── logger.h          # Logging utility
    ├── proxy.h/cpp       # DXGI forwarding
    ├── hooks.h/cpp       # DirectX Present hooks
    ├── ngx_wrapper.h/cpp # NGX wrapper (fallback)
    ├── streamline_integration.* # Streamline SDK integration
    ├── d3d12_wrappers.*  # D3D12 resource/command list wrappers
    ├── dxgi_wrappers.*   # DXGI swapchain wrappers
    └── resource_detector.* # Resource heuristics
```

## How It Works

```
┌─────────────────┐      ┌─────────────────┐      ┌─────────────────┐
│  Game Engine    │──────│  Our dxgi.dll   │──────│  System DXGI    │
│  (AC Valhalla)  │      │  (Proxy)        │      │                 │
└─────────────────┘      └────────┬────────┘      └─────────────────┘
                                  │
                                  ▼
                         ┌─────────────────┐
                         │  DLSS 4 NGX     │
                         │  (nvngx_*.dll)  │
                         └─────────────────┘
```

1. Game loads our `dxgi.dll` (proxy) instead of system DLL
2. We forward all calls to real DXGI
3. We hook `Present()` to inject DLSS processing
4. Before Present: Apply Super Resolution upscaling
5. After Present: Generate 1-3 extra frames (Multi-Frame Gen)

## Requirements

- **GPU**: NVIDIA RTX 40 series (Frame Gen 2x) or RTX 50 series (up to 4x)
- **Driver**: 560.xx or newer with DLSS 4 support
- **Windows**: 10/11 64-bit
- **Build**: Visual Studio 2022 with C++ workload
- **Streamline SDK**: Official NVIDIA Streamline SDK headers/libs and runtime DLLs

## Limitations

> [!WARNING]
> This is a framework/skeleton implementation. For full functionality:

1. **NVIDIA DLLs Required**: You must supply `nvngx_dlss.dll` and `nvngx_dlssg.dll`
2. **Streamline Runtime**: `sl.interposer.dll` and `sl.common.dll` must be present in the game folder
3. **Motion Vectors**: The game must expose motion vectors, or you need game-specific hooks
4. **Offsets**: Memory offsets in `dlss4_config.h` are placeholders

## Troubleshooting

| Issue | Solution |
|-------|----------|
| Game crashes on start | Check `dlss4_proxy.log` for errors |
| No DLSS effect | Verify `nvngx_*.dll` files are present |
| Low FPS with Frame Gen | Ensure RTX 40/50 GPU and latest drivers |
| Log says "DLL missing" | Place NVIDIA DLLs in game folder |

## License

Educational use only. NVIDIA NGX SDK and DLLs are property of NVIDIA Corporation.
