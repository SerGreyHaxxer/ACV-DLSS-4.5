#include "descriptor_tracker.h"
#include "resource_detector.h"
#include "logger.h"
#include <mutex>
#include <vector>
#include <unordered_map>
#include <wrl/client.h>

namespace {
    struct DescriptorRecord {
        D3D12_DESCRIPTOR_HEAP_DESC desc;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
        UINT descriptorSize = 0;
    };

    std::mutex g_descriptorMutex;
    std::vector<DescriptorRecord> g_descriptorRecords;
    std::unordered_map<uintptr_t, Microsoft::WRL::ComPtr<ID3D12Resource>> g_descriptorResources;
    std::unordered_map<uintptr_t, DXGI_FORMAT> g_descriptorFormats;

    bool IsLikelyMotionVectorFormat(DXGI_FORMAT format) {
        switch (format) {
            case DXGI_FORMAT_R16G16_FLOAT:
            case DXGI_FORMAT_R16G16_UNORM:
            case DXGI_FORMAT_R16G16_SNORM:
            case DXGI_FORMAT_R16G16_SINT:
            case DXGI_FORMAT_R16G16_UINT:
            case DXGI_FORMAT_R32G32_FLOAT:
            case DXGI_FORMAT_R32G32_SINT:
            case DXGI_FORMAT_R32G32_UINT:
            case DXGI_FORMAT_R16G16_TYPELESS:
            case DXGI_FORMAT_R32G32_TYPELESS:
            case DXGI_FORMAT_R16G16B16A16_SNORM:
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
            case DXGI_FORMAT_R8G8_SNORM:
                return true;
            default:
                return false;
        }
    }

    bool IsLikelyDepthFormat(DXGI_FORMAT format) {
        switch (format) {
            case DXGI_FORMAT_D32_FLOAT:
            case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            case DXGI_FORMAT_D24_UNORM_S8_UINT:
            case DXGI_FORMAT_D16_UNORM:
            case DXGI_FORMAT_R32_TYPELESS:
            case DXGI_FORMAT_R32G8X24_TYPELESS:
            case DXGI_FORMAT_R24G8_TYPELESS:
            case DXGI_FORMAT_R16_TYPELESS:
            case DXGI_FORMAT_R32_FLOAT:
            case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
                return true;
            default:
                return false;
        }
    }
}

void TrackDescriptorHeap(ID3D12DescriptorHeap* heap, UINT descriptorSize) {
    if (!heap) return;
    D3D12_DESCRIPTOR_HEAP_DESC desc = heap->GetDesc();
    std::lock_guard<std::mutex> lock(g_descriptorMutex);
    for (const auto& record : g_descriptorRecords) {
        if (record.heap.Get() == heap) return;
    }
    g_descriptorRecords.push_back({ desc, heap, descriptorSize });
}

void TrackDescriptorResource(D3D12_CPU_DESCRIPTOR_HANDLE handle, ID3D12Resource* resource, DXGI_FORMAT format) {
    if (!handle.ptr || !resource) return;
    {
        std::lock_guard<std::mutex> lock(g_descriptorMutex);
        // Evict old entries to prevent unbounded growth and VRAM leaks
        constexpr size_t kMaxDescriptorEntries = 16384;
        if (g_descriptorResources.size() > kMaxDescriptorEntries) {
            g_descriptorResources.clear();
            g_descriptorFormats.clear();
        }
        g_descriptorResources[handle.ptr] = resource;
        g_descriptorFormats[handle.ptr] = format;
    }
    DXGI_FORMAT effectiveFmt = format;
    if (effectiveFmt == DXGI_FORMAT_UNKNOWN) {
        effectiveFmt = resource->GetDesc().Format;
    }
    if (IsLikelyMotionVectorFormat(effectiveFmt)) {
        ResourceDetector::Get().RegisterMotionVectorFromView(resource, effectiveFmt);
    }
    if (IsLikelyDepthFormat(effectiveFmt)) {
        ResourceDetector::Get().RegisterDepthFromView(resource, effectiveFmt);
    }
}

bool TryResolveDescriptorResource(D3D12_CPU_DESCRIPTOR_HANDLE handle, ID3D12Resource** outResource, DXGI_FORMAT* outFormat) {
    if (!handle.ptr) return false;
    std::lock_guard<std::mutex> lock(g_descriptorMutex);
    auto it = g_descriptorResources.find(handle.ptr);
    if (it == g_descriptorResources.end()) return false;
    if (outResource) *outResource = it->second.Get();
    if (outFormat) {
        auto fmtIt = g_descriptorFormats.find(handle.ptr);
        *outFormat = (fmtIt != g_descriptorFormats.end()) ? fmtIt->second : DXGI_FORMAT_UNKNOWN;
    }
    return true;
}
