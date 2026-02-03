#include "d3d12_wrappers.h"
#include "streamline_integration.h"
#include "hooks.h"
#include "dlss4_config.h"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cfloat>
#include <cmath>
#include <atomic>
#include <wrl/client.h>

namespace {
    struct CameraCandidate {
        float view[16];
        float proj[16];
        float jitterX = 0.0f;
        float jitterY = 0.0f;
        float score = 0.0f;
        uint64_t frame = 0;
        bool valid = false;
    };

    std::mutex g_cameraMutex;
    CameraCandidate g_bestCamera;
    std::atomic<bool> g_loggedCamera(false);
    struct UploadCbvInfo {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        D3D12_GPU_VIRTUAL_ADDRESS gpuBase = 0;
        uint64_t size = 0;
        uint8_t* cpuPtr = nullptr;
    };

    std::mutex g_cbvMutex;
    std::vector<UploadCbvInfo> g_cbvInfos;
    std::atomic<uint64_t> g_cameraFrame(0);
    std::atomic<uint64_t> g_lastFullScanFrame(0);
    std::atomic<uint64_t> g_lastCameraFoundFrame(0);
    struct DescriptorRecord {
        D3D12_DESCRIPTOR_HEAP_DESC desc;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
        UINT descriptorSize = 0;
    };
    std::mutex g_descriptorMutex;
    std::vector<DescriptorRecord> g_descriptorRecords;
    std::unordered_map<uintptr_t, Microsoft::WRL::ComPtr<ID3D12Resource>> g_descriptorResources;
    std::unordered_map<uintptr_t, DXGI_FORMAT> g_descriptorFormats;
    struct SamplerRecord {
        D3D12_SAMPLER_DESC desc = {};
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = {};
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        bool valid = false;
    };
    std::mutex g_samplerMutex;
    std::vector<SamplerRecord> g_samplerRecords;
    struct CbvGpuAddrEntry {
        D3D12_GPU_VIRTUAL_ADDRESS addr = 0;
        uint64_t lastFrame = 0;
    };
    std::unordered_map<uintptr_t, CbvGpuAddrEntry> g_cbvGpuAddrs;
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> g_rootCbvAddrs;
    std::atomic<uint64_t> g_cbvDescriptorCount(0);
    std::atomic<uint64_t> g_cbvGpuAddrCount(0);

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
                return true;
            default:
                return false;
        }
    }

    void TrackDescriptorHeapInternal(ID3D12DescriptorHeap* heap, UINT descriptorSize) {
        if (!heap) return;
        D3D12_DESCRIPTOR_HEAP_DESC desc = heap->GetDesc();
        std::lock_guard<std::mutex> lock(g_descriptorMutex);
        for (const auto& record : g_descriptorRecords) {
            if (record.heap.Get() == heap) return;
        }
        g_descriptorRecords.push_back({ desc, heap, descriptorSize });
    }

    void TrackDescriptorResourceInternal(D3D12_CPU_DESCRIPTOR_HANDLE handle, ID3D12Resource* resource, DXGI_FORMAT format) {
        if (!handle.ptr || !resource) return;
        {
            std::lock_guard<std::mutex> lock(g_descriptorMutex);
            g_descriptorResources[handle.ptr] = resource;
            g_descriptorFormats[handle.ptr] = format;
        }
        DXGI_FORMAT mvFormat = format;
        if (mvFormat == DXGI_FORMAT_UNKNOWN) {
            mvFormat = resource->GetDesc().Format;
        }
        if (IsLikelyMotionVectorFormat(mvFormat)) {
            ResourceDetector::Get().RegisterMotionVectorFromView(resource, mvFormat);
        }
    }

    bool TryResolveDescriptorResourceInternal(D3D12_CPU_DESCRIPTOR_HANDLE handle, ID3D12Resource** outResource, DXGI_FORMAT* outFormat) {
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

    void TrackCbvDescriptorInternal(D3D12_CPU_DESCRIPTOR_HANDLE handle, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc) {
        if (!handle.ptr || !desc || desc->BufferLocation == 0) return;
        std::lock_guard<std::mutex> lock(g_descriptorMutex);
        g_cbvGpuAddrs[handle.ptr] = { desc->BufferLocation, StreamlineIntegration::Get().GetFrameCount() };
        g_cbvDescriptorCount++;
    }

    void TrackRootCbvAddressInternal(D3D12_GPU_VIRTUAL_ADDRESS address) {
        if (!address) return;
        std::lock_guard<std::mutex> lock(g_descriptorMutex);
        auto it = std::find(g_rootCbvAddrs.begin(), g_rootCbvAddrs.end(), address);
        if (it != g_rootCbvAddrs.end()) g_rootCbvAddrs.erase(it);
        g_rootCbvAddrs.push_back(address);
        const size_t maxKeep = CAMERA_DESCRIPTOR_SCAN_MAX * CAMERA_SCAN_EXTENDED_MULTIPLIER;
        if (g_rootCbvAddrs.size() > maxKeep) {
            g_rootCbvAddrs.erase(g_rootCbvAddrs.begin(), g_rootCbvAddrs.begin() + (g_rootCbvAddrs.size() - maxKeep));
        }
        g_cbvGpuAddrCount++;
    }

    bool IsFinite(float v) { return v == v && v > -FLT_MAX && v < FLT_MAX; }
    bool LooksLikeMatrix(const float* m) {
        for (int i = 0; i < 16; ++i) {
            if (!IsFinite(m[i])) return false;
        }
        return true;
    }

    static void TransposeMatrix(const float* in, float* out) {
        out[0] = in[0];  out[1] = in[4];  out[2] = in[8];  out[3] = in[12];
        out[4] = in[1];  out[5] = in[5];  out[6] = in[9];  out[7] = in[13];
        out[8] = in[2];  out[9] = in[6];  out[10] = in[10]; out[11] = in[14];
        out[12] = in[3]; out[13] = in[7]; out[14] = in[11]; out[15] = in[15];
    }

    static float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    static void GetRow3(const float* m, int row, float* out) { out[0] = m[row * 4 + 0]; out[1] = m[row * 4 + 1]; out[2] = m[row * 4 + 2]; }
    static float Length3(const float* v) { return sqrtf(Dot3(v, v)); }

    float ScoreMatrixPair(const float* view, const float* proj) {
        float score = 0.0f;
        if (!LooksLikeMatrix(view) || !LooksLikeMatrix(proj)) return 0.0f;
        
        // View Matrix [15] is always 1.0
        if (fabsf(view[15] - 1.0f) < 0.01f) score += 0.2f;
        
        // Projection Matrix Checks
        // Strong perspective: [15]=0, [11]=+/-1.
        bool isStrongPerspective = fabsf(proj[15]) < 0.01f && fabsf(fabsf(proj[11]) - 1.0f) < 0.1f;
        // Weak perspective: tolerate titles that pack projection differently.
        bool isWeakPerspective = fabsf(proj[15]) < 0.8f && fabsf(proj[11]) > 0.2f;
        
        // Orthographic: [15]=1, [11]=0. This is usually UI.
        bool isOrtho = fabsf(proj[15] - 1.0f) < 0.01f && fabsf(proj[11]) < 0.1f;

        if (isStrongPerspective) score += 0.6f; // High score for 3D perspective
        else if (isWeakPerspective) score += 0.3f;
        else if (isOrtho) score += 0.0f;  // Ignore UI matrices
        
        // Sanity check translation elements
        if (fabsf(view[3]) < 1.0f && fabsf(view[7]) < 1.0f && fabsf(view[11]) < 1.0f) score += 0.1f;
        if (fabsf(view[12]) < CAMERA_POS_TOLERANCE && fabsf(view[13]) < CAMERA_POS_TOLERANCE && fabsf(view[14]) < CAMERA_POS_TOLERANCE) score += 0.1f;
        
        float r0[3], r1[3], r2[3];
        GetRow3(view, 0, r0);
        GetRow3(view, 1, r1);
        GetRow3(view, 2, r2);
        float len0 = Length3(r0);
        float len1 = Length3(r1);
        float len2 = Length3(r2);
        if (len0 > 0.1f && len1 > 0.1f && len2 > 0.1f) {
            float orthoScore = 0.0f;
            float d01 = fabsf(Dot3(r0, r1) / (len0 * len1));
            float d02 = fabsf(Dot3(r0, r2) / (len0 * len2));
            float d12 = fabsf(Dot3(r1, r2) / (len1 * len2));
            if (d01 < 0.2f) orthoScore += 0.1f;
            if (d02 < 0.2f) orthoScore += 0.1f;
            if (d12 < 0.2f) orthoScore += 0.1f;
            score += orthoScore;
        }

        if (fabsf(view[15] - 1.0f) > 0.1f) return 0.0f;
        return score;
    }

    bool TryExtractCameraFromBuffer(const uint8_t* data, size_t size, float* outView, float* outProj, float* outScore, size_t* outOffset) {
        if (!data || size < CAMERA_CBV_MIN_SIZE) return false;
        float bestScore = 0.0f;
        size_t bestOffset = 0;
        auto scanWithStride = [&](size_t stride, float& bestScoreOut, size_t& bestOffsetOut) {
            size_t scanLimit = size;
            for (size_t offset = 0; offset + sizeof(float) * 32 <= scanLimit; offset += stride) {
                const float* view = reinterpret_cast<const float*>(data + offset);
                const float* proj = view + 16;
                float score = ScoreMatrixPair(view, proj);
                if (score > bestScoreOut) { bestScoreOut = score; bestOffsetOut = offset; }
                
                float tView[16], tProj[16];
                TransposeMatrix(view, tView);
                TransposeMatrix(proj, tProj);
                float tScore = ScoreMatrixPair(tView, tProj);
                if (tScore > bestScoreOut) { bestScoreOut = tScore; bestOffsetOut = offset; }
            }
        };
        
        // Fast path: 256-byte alignment (D3D12 requirement). Fallback: 64-byte stride if nothing scores.
        scanWithStride(256, bestScore, bestOffset);
        if (bestScore < 0.6f) {
            bestScore = 0.0f;
            bestOffset = 0;
            scanWithStride(CAMERA_SCAN_MED_STRIDE, bestScore, bestOffset);
        }
        if (bestScore < 0.6f) {
            bestScore = 0.0f;
            bestOffset = 0;
            scanWithStride(64, bestScore, bestOffset);
        }
        if (bestScore < 0.6f) {
            bestScore = 0.0f;
            bestOffset = 0;
            scanWithStride(CAMERA_SCAN_FINE_STRIDE, bestScore, bestOffset);
        }
        if (bestScore < 0.6f) return false;
        const float* view = reinterpret_cast<const float*>(data + bestOffset);
        const float* proj = view + 16;
        float score = ScoreMatrixPair(view, proj);
        float tView[16], tProj[16];
        TransposeMatrix(view, tView);
        TransposeMatrix(proj, tProj);
        float tScore = ScoreMatrixPair(tView, tProj);
        if (tScore > score) {
            memcpy(outView, tView, sizeof(float) * 16);
            memcpy(outProj, tProj, sizeof(float) * 16);
            if (outScore) *outScore = tScore;
        } else {
            memcpy(outView, view, sizeof(float) * 16);
            memcpy(outProj, proj, sizeof(float) * 16);
            if (outScore) *outScore = score;
        }
        if (outOffset) *outOffset = bestOffset;
        return true;
    }

    bool TryGetCbvData(D3D12_GPU_VIRTUAL_ADDRESS gpuAddress, const uint8_t** outData, size_t* outSize) {
        std::lock_guard<std::mutex> lock(g_cbvMutex);
        for (const auto& info : g_cbvInfos) {
            if (!info.cpuPtr || info.gpuBase == 0 || info.size == 0) continue;
            if (gpuAddress >= info.gpuBase && gpuAddress < info.gpuBase + info.size) {
                size_t offset = static_cast<size_t>(gpuAddress - info.gpuBase);
                if (offset >= info.size) return false;
                *outData = info.cpuPtr + offset;
                *outSize = static_cast<size_t>(info.size - offset);
                return true;
            }
        }
        return false;
    }

    bool TryGetCbvDataFromDescriptor(ID3D12DescriptorHeap* heap, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, const uint8_t** outData, size_t* outSize) {
        if (!heap || cpuHandle.ptr == 0) return false;
        D3D12_DESCRIPTOR_HEAP_DESC desc = heap->GetDesc();
        if (desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) return false;
        D3D12_CPU_DESCRIPTOR_HANDLE start = heap->GetCPUDescriptorHandleForHeapStart();
        if (cpuHandle.ptr < start.ptr) return false;
        UINT increment = StreamlineIntegration::Get().GetDescriptorSize();
        if (increment == 0) return false;
        uint64_t index = (cpuHandle.ptr - start.ptr) / increment;
        if (index >= desc.NumDescriptors) return false;
        D3D12_GPU_DESCRIPTOR_HANDLE gpuStart = heap->GetGPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
        gpuHandle.ptr = gpuStart.ptr + index * increment;
        return TryGetCbvData(gpuHandle.ptr, outData, outSize);
    }

    void UpdateBestCamera(const float* view, const float* proj, float jitterX, float jitterY) {
        float score = ScoreMatrixPair(view, proj);
        if (score < 0.6f) return;
        std::lock_guard<std::mutex> lock(g_cameraMutex);
        float stabilityBonus = 0.0f;
        if (g_bestCamera.valid) {
            float deltaSum = 0.0f;
            for (int i = 0; i < 16; ++i) {
                deltaSum += fabsf(g_bestCamera.view[i] - view[i]);
                deltaSum += fabsf(g_bestCamera.proj[i] - proj[i]);
            }
            if (deltaSum < 0.2f) stabilityBonus = 0.2f;
            else if (deltaSum < 1.0f) stabilityBonus = 0.1f;
        }
        score += stabilityBonus;
        g_bestCamera.score = score;
        memcpy(g_bestCamera.view, view, sizeof(float) * 16);
        memcpy(g_bestCamera.proj, proj, sizeof(float) * 16);
        g_bestCamera.jitterX = jitterX;
        g_bestCamera.jitterY = jitterY;
        g_bestCamera.frame = ++g_cameraFrame;
        g_bestCamera.valid = true;
        if (!g_loggedCamera.exchange(true)) {
            LOG_INFO("Camera matrices detected (score %.2f)", score);
        }
    }

    bool FetchCamera(CameraCandidate& out) {
        std::lock_guard<std::mutex> lock(g_cameraMutex);
        if (!g_bestCamera.valid) return false;
        out = g_bestCamera;
        return true;
    }
}

void UpdateCameraCache(const float* view, const float* proj, float jitterX, float jitterY) {
    if (!view || !proj) return;
    UpdateBestCamera(view, proj, jitterX, jitterY);
}

void TrackDescriptorHeap(ID3D12DescriptorHeap* heap, UINT descriptorSize) { TrackDescriptorHeapInternal(heap, descriptorSize); }
void TrackDescriptorResource(D3D12_CPU_DESCRIPTOR_HANDLE handle, ID3D12Resource* resource, DXGI_FORMAT format) { TrackDescriptorResourceInternal(handle, resource, format); }
bool TryResolveDescriptorResource(D3D12_CPU_DESCRIPTOR_HANDLE handle, ID3D12Resource** outResource, DXGI_FORMAT* outFormat) { return TryResolveDescriptorResourceInternal(handle, outResource, outFormat); }

void ApplySamplerLodBias(float bias) {
    std::lock_guard<std::mutex> lock(g_samplerMutex);
    if (g_samplerRecords.empty()) return;
    for (const auto& record : g_samplerRecords) {
        if (!record.valid || !record.device || record.cpuHandle.ptr == 0) continue;
        D3D12_SAMPLER_DESC nD = record.desc;
        nD.MipLODBias += bias;
        nD.MipLODBias = std::clamp(nD.MipLODBias, -3.0f, 3.0f);
        record.device->CreateSampler(&nD, record.cpuHandle);
    }
}

bool GetLastCameraStats(float& outScore, uint64_t& outFrame) {
    std::lock_guard<std::mutex> lock(g_cameraMutex);
    if (!g_bestCamera.valid) return false;
    outScore = g_bestCamera.score;
    outFrame = g_bestCamera.frame;
    return true;
}

void TrackCbvDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc) {
    TrackCbvDescriptorInternal(handle, desc);
}

void TrackRootCbvAddress(D3D12_GPU_VIRTUAL_ADDRESS address) {
    TrackRootCbvAddressInternal(address);
}

void GetCameraScanCounts(uint64_t& cbvCount, uint64_t& descCount, uint64_t& rootCount) {
    cbvCount = g_cbvInfos.size();
    {
        std::lock_guard<std::mutex> lock(g_descriptorMutex);
        descCount = g_cbvGpuAddrs.size();
        rootCount = g_rootCbvAddrs.size();
    }
}

void RegisterCbv(ID3D12Resource* pResource, UINT64 size, uint8_t* cpuPtr) {
    std::lock_guard<std::mutex> lock(g_cbvMutex);
    UploadCbvInfo info{};
    info.resource = pResource; // Smart pointer assignment
    info.gpuBase = pResource->GetGPUVirtualAddress();
    info.size = size;
    info.cpuPtr = cpuPtr;
    g_cbvInfos.push_back(info);
}

static D3D12_GPU_VIRTUAL_ADDRESS s_lastCameraCbv = 0;

static size_t s_lastCameraOffset = 0;

void ResetCameraScanCache() {
    std::lock_guard<std::mutex> lock(g_cbvMutex);
    g_cbvInfos.clear();
    s_lastCameraCbv = 0;
    s_lastCameraOffset = 0;
    g_lastFullScanFrame.store(0);
    g_lastCameraFoundFrame.store(0);
    g_loggedCamera.store(false);
    {
        std::lock_guard<std::mutex> dlock(g_descriptorMutex);
        g_cbvGpuAddrs.clear();
        g_rootCbvAddrs.clear();
    }
    g_cbvDescriptorCount.store(0);
    g_cbvGpuAddrCount.store(0);
}

uint64_t GetLastCameraFoundFrame() {
    return g_lastCameraFoundFrame.load();
}

bool TryScanAllCbvsForCamera(float* outView, float* outProj, float* outScore, bool logCandidates, bool allowFullScan) {
    std::lock_guard<std::mutex> lock(g_cbvMutex);

    auto it = std::remove_if(g_cbvInfos.begin(), g_cbvInfos.end(), [](const UploadCbvInfo& info) {
        if (!info.cpuPtr) return true;
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(info.cpuPtr, &mbi, sizeof(mbi)) == 0) return true;
        if (mbi.State != MEM_COMMIT) return true;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return true;
        return false;
    });
    if (it != g_cbvInfos.end()) g_cbvInfos.erase(it, g_cbvInfos.end());

    // OPTIMIZATION: Fast path - check known location first
    if (s_lastCameraCbv != 0) {
        for (const auto& info : g_cbvInfos) {
            if (info.gpuBase == s_lastCameraCbv) {
                if (s_lastCameraOffset + sizeof(float) * 32 <= info.size) {
                    const float* view = reinterpret_cast<const float*>(info.cpuPtr + s_lastCameraOffset);
                    const float* proj = view + 16;
                    float score = ScoreMatrixPair(view, proj);
                    float bestScore = score;
                    bool useTranspose = false;
                    float tView[16], tProj[16];
                    TransposeMatrix(view, tView);
                    TransposeMatrix(proj, tProj);
                    float tScore = ScoreMatrixPair(tView, tProj);
                    if (tScore > bestScore) { bestScore = tScore; useTranspose = true; }
                    if (bestScore > 0.6f) {
                        if (useTranspose) {
                            memcpy(outView, tView, sizeof(float) * 16);
                            memcpy(outProj, tProj, sizeof(float) * 16);
                        } else {
                            memcpy(outView, view, sizeof(float) * 16);
                            memcpy(outProj, proj, sizeof(float) * 16);
                        }
                        if (outScore) *outScore = bestScore;
                        g_lastCameraFoundFrame.store(StreamlineIntegration::Get().GetFrameCount());
                        return true;
                    }
                }

                float tempView[16], tempProj[16], score = 0.0f;
                size_t newOffset = 0;
                if (TryExtractCameraFromBuffer(info.cpuPtr, static_cast<size_t>(info.size), tempView, tempProj, &score, &newOffset)) {
                    s_lastCameraOffset = newOffset;
                    memcpy(outView, tempView, sizeof(float) * 16);
                    memcpy(outProj, tempProj, sizeof(float) * 16);
                    if (outScore) *outScore = score;
                    g_lastCameraFoundFrame.store(StreamlineIntegration::Get().GetFrameCount());
                    return true;
                }
            }
        }
    }

    float bestScore = 0.0f;
    bool found = false;
    D3D12_GPU_VIRTUAL_ADDRESS foundGpuBase = 0;

    if (g_cbvInfos.empty() && logCandidates) {
        LOG_INFO("[CAM] No CBVs registered! Check RegisterCbv hooks.");
        return false;
    }

    if (!allowFullScan) {
        return false;
    }
    g_lastFullScanFrame.store(StreamlineIntegration::Get().GetFrameCount());

    uint32_t scanned = 0;
    const uint32_t maxScan = CAMERA_SCAN_MAX_CBVS_PER_FRAME * CAMERA_SCAN_EXTENDED_MULTIPLIER;
    for (const auto& info : g_cbvInfos) {
        if (!info.cpuPtr || info.size < CAMERA_CBV_MIN_SIZE) continue;
        if (scanned++ >= maxScan) break;

        float tempView[16], tempProj[16], score = 0.0f;
        size_t foundOffset = 0;
        if (TryExtractCameraFromBuffer(info.cpuPtr, static_cast<size_t>(info.size), tempView, tempProj, &score, &foundOffset)) {
            if (logCandidates && score > 0.0f) {
                LOG_INFO("[CAM] Candidate GPU:0x%llx Size:%llu Score:%.2f View[15]:%.2f Proj[15]:%.2f Proj[11]:%.2f",
                    info.gpuBase, info.size, score, tempView[15], tempProj[15], tempProj[11]);
            }
            if (score > bestScore) {
                bestScore = score;
                memcpy(outView, tempView, sizeof(float) * 16);
                memcpy(outProj, tempProj, sizeof(float) * 16);
                foundGpuBase = info.gpuBase;
                s_lastCameraOffset = foundOffset; // Cache it!
                found = true;
            }
        }
    }

    if (found) {
        s_lastCameraCbv = foundGpuBase;
        if (outScore) *outScore = bestScore;
        g_lastCameraFoundFrame.store(StreamlineIntegration::Get().GetFrameCount());
        LOG_INFO("Camera matrices detected (Score: %.2f) at GPU: 0x%llx Offset: +0x%zX", bestScore, foundGpuBase, s_lastCameraOffset);
    } else if (logCandidates) {
        LOG_INFO("[CAM] Scan failed. Checked %llu CBVs. Best Score: %.2f", (uint64_t)g_cbvInfos.size(), bestScore);
    }

    return found;
}
bool TryScanDescriptorCbvsForCamera(float* outView, float* outProj, float* outScore, bool logCandidates) {
    std::vector<std::pair<D3D12_GPU_VIRTUAL_ADDRESS, uint64_t>> addrs;
    {
        std::lock_guard<std::mutex> lock(g_descriptorMutex);
        addrs.reserve(g_cbvGpuAddrs.size());
        for (const auto& entry : g_cbvGpuAddrs) {
            addrs.push_back({ entry.second.addr, entry.second.lastFrame });
        }
    }
    if (addrs.empty()) {
        if (logCandidates) {
            LOG_INFO("[CAM] No CBV descriptors captured (CBV descriptors: %llu, GPU addr hits: %llu).",
                (unsigned long long)g_cbvDescriptorCount.load(), (unsigned long long)g_cbvGpuAddrCount.load());
        }
        return false;
    }
    float bestScore = 0.0f;
    bool found = false;
    uint64_t currentFrame = StreamlineIntegration::Get().GetFrameCount();
    uint64_t lastFound = g_lastCameraFoundFrame.load();
    bool stale = lastFound == 0 || (currentFrame > lastFound + CAMERA_SCAN_STALE_FRAMES);
    uint32_t maxScan = stale ? (CAMERA_DESCRIPTOR_SCAN_MAX * CAMERA_SCAN_EXTENDED_MULTIPLIER) : CAMERA_DESCRIPTOR_SCAN_MAX;
    std::sort(addrs.begin(), addrs.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::unordered_set<D3D12_GPU_VIRTUAL_ADDRESS> seen;
    uint32_t scanned = 0;
    for (const auto& entry : addrs) {
        if (scanned >= maxScan) break;
        if (!seen.insert(entry.first).second) continue;
        scanned++;
        const uint8_t* data = nullptr;
        size_t size = 0;
        if (!TryGetCbvData(entry.first, &data, &size)) continue;
        float tempView[16], tempProj[16], score = 0.0f;
        size_t offset = 0;
        if (!TryExtractCameraFromBuffer(data, size, tempView, tempProj, &score, &offset)) continue;
        if (score > bestScore) {
            bestScore = score;
            memcpy(outView, tempView, sizeof(float) * 16);
            memcpy(outProj, tempProj, sizeof(float) * 16);
            found = true;
        }
    }
    if (logCandidates) {
        LOG_INFO("[CAM] Descriptor scan: candidates=%llu scanned=%u bestScore=%.2f",
            (unsigned long long)addrs.size(), scanned, bestScore);
    }
    if (found && outScore) *outScore = bestScore;
    return found;
}
bool TryScanRootCbvsForCamera(float* outView, float* outProj, float* outScore, bool logCandidates) {
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> addrs;
    {
        std::lock_guard<std::mutex> lock(g_descriptorMutex);
        addrs = g_rootCbvAddrs;
    }
    if (addrs.empty()) {
        if (logCandidates) {
            LOG_INFO("[CAM] No root CBV addresses captured yet.");
        }
        return false;
    }
    float bestScore = 0.0f;
    bool found = false;
    uint64_t currentFrame = StreamlineIntegration::Get().GetFrameCount();
    uint64_t lastFound = g_lastCameraFoundFrame.load();
    bool stale = lastFound == 0 || (currentFrame > lastFound + CAMERA_SCAN_STALE_FRAMES);
    uint32_t maxScan = stale ? (CAMERA_DESCRIPTOR_SCAN_MAX * CAMERA_SCAN_EXTENDED_MULTIPLIER) : CAMERA_DESCRIPTOR_SCAN_MAX;
    uint32_t scanned = 0;
    for (size_t i = addrs.size(); i-- > 0;) {
        if (scanned++ >= maxScan) break;
        const uint8_t* data = nullptr;
        size_t size = 0;
        if (!TryGetCbvData(addrs[i], &data, &size)) continue;
        float tempView[16], tempProj[16], score = 0.0f;
        size_t offset = 0;
        if (!TryExtractCameraFromBuffer(data, size, tempView, tempProj, &score, &offset)) continue;
        if (score > bestScore) {
            bestScore = score;
            memcpy(outView, tempView, sizeof(float) * 16);
            memcpy(outProj, tempProj, sizeof(float) * 16);
            found = true;
        }
    }
    if (logCandidates) {
        LOG_INFO("[CAM] Root CBV scan: candidates=%llu scanned=%u bestScore=%.2f",
            (unsigned long long)addrs.size(), scanned, bestScore);
    }
    if (found && outScore) *outScore = bestScore;
    return found;
}
WrappedID3D12GraphicsCommandList::WrappedID3D12GraphicsCommandList(ID3D12GraphicsCommandList* pReal, WrappedID3D12Device* pDeviceWrapper) 
    : m_pReal(pReal), m_pDeviceWrapper(pDeviceWrapper), m_refCount(1) {
    if(m_pReal) m_pReal->AddRef();
    if(m_pDeviceWrapper) m_pDeviceWrapper->AddRef();
}

WrappedID3D12GraphicsCommandList::~WrappedID3D12GraphicsCommandList() {
    if(m_pReal) m_pReal->Release();
    if(m_pDeviceWrapper) m_pDeviceWrapper->Release();
}

HRESULT STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::QueryInterface(REFIID riid, void** ppvObject) {
    if (ppvObject == nullptr) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12CommandList) || riid == __uuidof(ID3D12GraphicsCommandList)) {
        *ppvObject = this; AddRef(); return S_OK;
    }
    return m_pReal->QueryInterface(riid, ppvObject);
}

ULONG STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::AddRef() { return InterlockedIncrement(&m_refCount); }
ULONG STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::Release() { ULONG ref = InterlockedDecrement(&m_refCount); if (ref == 0) delete this; return ref; }
HRESULT STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) { return m_pReal->GetPrivateData(guid, pDataSize, pData); }
HRESULT STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) { return m_pReal->SetPrivateData(guid, DataSize, pData); }
HRESULT STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) { return m_pReal->SetPrivateDataInterface(guid, pData); }
HRESULT STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetName(LPCWSTR Name) { return m_pReal->SetName(Name); }
HRESULT STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::GetDevice(REFIID riid, void** ppvDevice) { return m_pDeviceWrapper->QueryInterface(riid, ppvDevice); }
D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::GetType() { return m_pReal->GetType(); }

HRESULT STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::Close() {
    NotifyWrappedCommandListUsed();
    static uint64_t s_hbLast = 0;
    static uint64_t s_hbCloseCount = 0;
    float jitterX = 0.0f, jitterY = 0.0f;
    if (!TryGetPatternJitter(jitterX, jitterY)) {
        jitterX = 0.0f;
        jitterY = 0.0f;
    }

    s_hbCloseCount++;
    uint64_t hbNow = GetTickCount64();
    if (hbNow - s_hbLast >= 2000) {
        LOG_DEBUG("[HB] Wrapped_Close tick (calls=%llu)", (unsigned long long)s_hbCloseCount);
        s_hbLast = hbNow;
    }
    
    // Throttle scanning to once per frame
    static uint64_t s_lastScanFrame = 0;
    uint64_t currentFrame = ResourceDetector::Get().GetFrameCount();
    
    if (currentFrame > s_lastScanFrame) {
        float view[16], proj[16], score = 0.0f;
        static int s_camLog = 0;
        bool doLog = (++s_camLog % CAMERA_SCAN_LOG_INTERVAL == 0);
        uint64_t lastFound = g_lastCameraFoundFrame.load();
        uint64_t lastFull = g_lastFullScanFrame.load();
        bool stale = lastFound == 0 || (currentFrame > lastFound + CAMERA_SCAN_STALE_FRAMES);
        bool forceFull = stale || (currentFrame % CAMERA_SCAN_FORCE_FULL_FRAMES == 0);
        bool allowFull = forceFull || (currentFrame > lastFull + CAMERA_SCAN_MIN_INTERVAL_FRAMES);

        if (doLog) {
            uint64_t cbvCount = g_cbvInfos.size();
            uint64_t descCount = 0;
            uint64_t rootCount = 0;
            {
                std::lock_guard<std::mutex> lock(g_descriptorMutex);
                descCount = g_cbvGpuAddrs.size();
                rootCount = g_rootCbvAddrs.size();
            }
            LOG_INFO("[CAM] Scan start (frame %llu): CBVs=%llu DescCBVs=%llu RootCBVs=%llu",
                currentFrame, (unsigned long long)cbvCount, (unsigned long long)descCount, (unsigned long long)rootCount);
        }
        bool found = TryScanAllCbvsForCamera(view, proj, &score, doLog, allowFull);
        if (!found) {
            found = TryScanDescriptorCbvsForCamera(view, proj, &score, doLog);
        }
        if (!found) {
            found = TryScanRootCbvsForCamera(view, proj, &score, doLog);
        }
        if (found) {
            UpdateBestCamera(view, proj, jitterX, jitterY);
            StreamlineIntegration::Get().SetCameraData(view, proj, jitterX, jitterY);
        } else {
            StreamlineIntegration::Get().SetCameraData(nullptr, nullptr, jitterX, jitterY);
            if (doLog) {
                LOG_WARN("[CAM] Camera scan failed (frame %llu)", currentFrame);
            }
        }
        s_lastScanFrame = currentFrame;
    } else {
        // Just update jitter using cached matrices
        StreamlineIntegration::Get().SetCameraData(nullptr, nullptr, jitterX, jitterY);
    }

    StreamlineIntegration::Get().EvaluateDLSS(this);
    return m_pReal->Close();
}

HRESULT STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::Reset(ID3D12CommandAllocator* pAllocator, ID3D12PipelineState* pInitialState) { return m_pReal->Reset(pAllocator, pInitialState); }

void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::ResourceBarrier(UINT NumBarriers, const D3D12_RESOURCE_BARRIER* pBarriers) {
    if (pBarriers) {
        static uint64_t s_lastScanFrame = 0;
        uint64_t currentFrame = StreamlineIntegration::Get().GetFrameCount();
        if (currentFrame != s_lastScanFrame) {
            s_lastScanFrame = currentFrame;
            UINT scanned = 0;
            for (UINT i = 0; i < NumBarriers && scanned < RESOURCE_BARRIER_SCAN_MAX; i++) {
                if (pBarriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                    ResourceDetector::Get().RegisterResource(pBarriers[i].Transition.pResource, true);
                    scanned++;
                }
            }
        }
    }
    m_pReal->ResourceBarrier(NumBarriers, pBarriers);
}

    void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::OMSetRenderTargets(UINT N, const D3D12_CPU_DESCRIPTOR_HANDLE* R, BOOL S, const D3D12_CPU_DESCRIPTOR_HANDLE* D) {
        if (D && D->ptr) {
            ID3D12Resource* res = nullptr;
            DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
            if (TryResolveDescriptorResource(*D, &res, &fmt) && res) {
                ResourceDetector::Get().RegisterDepthFromView(res, fmt);
            }
        }
        m_pReal->OMSetRenderTargets(N, R, S, D);
    }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::ExecuteBundle(ID3D12GraphicsCommandList* pCommandList) { m_pReal->ExecuteBundle(pCommandList); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView, D3D12_CLEAR_FLAGS ClearFlags, FLOAT Depth, UINT8 Stencil, UINT NumRects, const D3D12_RECT* pRects) { m_pReal->ClearDepthStencilView(DepthStencilView, ClearFlags, Depth, Stencil, NumRects, pRects); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView, const FLOAT ColorRGBA[4], UINT NumRects, const D3D12_RECT* pRects) { m_pReal->ClearRenderTargetView(RenderTargetView, ColorRGBA, NumRects, pRects); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource* pResource, const UINT Values[4], UINT NumRects, const D3D12_RECT* pRects) { m_pReal->ClearUnorderedAccessViewUint(ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values, NumRects, pRects); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource* pResource, const FLOAT Values[4], UINT NumRects, const D3D12_RECT* pRects) { m_pReal->ClearUnorderedAccessViewFloat(ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values, NumRects, pRects); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::DiscardResource(ID3D12Resource* pResource, const D3D12_DISCARD_REGION* pRegion) { m_pReal->DiscardResource(pResource, pRegion); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::ResolveQueryData(ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT StartElement, UINT ElementCount, ID3D12Resource* pDestinationBuffer, UINT64 AlignedDestinationBufferOffset) { m_pReal->ResolveQueryData(pQueryHeap, Type, StartElement, ElementCount, pDestinationBuffer, AlignedDestinationBufferOffset); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::Dispatch(UINT X, UINT Y, UINT Z) { m_pReal->Dispatch(X, Y, Z); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::CopyBufferRegion(ID3D12Resource* D, UINT64 DO, ID3D12Resource* S, UINT64 SO, UINT64 B) { m_pReal->CopyBufferRegion(D, DO, S, SO, B); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* D, UINT DX, UINT DY, UINT DZ, const D3D12_TEXTURE_COPY_LOCATION* S, const D3D12_BOX* SB) { m_pReal->CopyTextureRegion(D, DX, DY, DZ, S, SB); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::CopyResource(ID3D12Resource* D, ID3D12Resource* S) { m_pReal->CopyResource(D, S); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::CopyTiles(ID3D12Resource* R, const D3D12_TILED_RESOURCE_COORDINATE* C, const D3D12_TILE_REGION_SIZE* S, ID3D12Resource* B, UINT64 O, D3D12_TILE_COPY_FLAGS F) { m_pReal->CopyTiles(R, C, S, B, O, F); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::ResolveSubresource(ID3D12Resource* D, UINT DI, ID3D12Resource* S, UINT SI, DXGI_FORMAT F) { m_pReal->ResolveSubresource(D, DI, S, SI, F); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY T) { m_pReal->IASetPrimitiveTopology(T); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::RSSetViewports(UINT N, const D3D12_VIEWPORT* V) { m_pReal->RSSetViewports(N, V); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::RSSetScissorRects(UINT N, const D3D12_RECT* R) { m_pReal->RSSetScissorRects(N, R); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::OMSetBlendFactor(const FLOAT F[4]) { m_pReal->OMSetBlendFactor(F); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::OMSetStencilRef(UINT R) { m_pReal->OMSetStencilRef(R); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetPipelineState(ID3D12PipelineState* P) { m_pReal->SetPipelineState(P); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetDescriptorHeaps(UINT N, ID3D12DescriptorHeap* const* H) { 
    if (H) {
        for (UINT i = 0; i < N; ++i) {
            UINT descriptorSize = m_pDeviceWrapper ? m_pDeviceWrapper->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) : 0;
            TrackDescriptorHeap(H[i], descriptorSize);
        }
    }
    m_pReal->SetDescriptorHeaps(N, H);
}
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetComputeRootSignature(ID3D12RootSignature* S) { m_pReal->SetComputeRootSignature(S); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetGraphicsRootSignature(ID3D12RootSignature* S) { m_pReal->SetGraphicsRootSignature(S); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetComputeRootDescriptorTable(UINT I, D3D12_GPU_DESCRIPTOR_HANDLE H) { m_pReal->SetComputeRootDescriptorTable(I, H); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable(UINT I, D3D12_GPU_DESCRIPTOR_HANDLE H) { m_pReal->SetGraphicsRootDescriptorTable(I, H); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetComputeRoot32BitConstant(UINT I, UINT D, UINT O) { m_pReal->SetComputeRoot32BitConstant(I, D, O); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetGraphicsRoot32BitConstant(UINT I, UINT D, UINT O) { m_pReal->SetGraphicsRoot32BitConstant(I, D, O); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetComputeRoot32BitConstants(UINT I, UINT N, const void* D, UINT O) { m_pReal->SetComputeRoot32BitConstants(I, N, D, O); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetGraphicsRoot32BitConstants(UINT I, UINT N, const void* D, UINT O) { m_pReal->SetGraphicsRoot32BitConstants(I, N, D, O); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetComputeRootConstantBufferView(UINT I, D3D12_GPU_VIRTUAL_ADDRESS A) { m_pReal->SetComputeRootConstantBufferView(I, A); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetGraphicsRootConstantBufferView(UINT I, D3D12_GPU_VIRTUAL_ADDRESS A) { m_pReal->SetGraphicsRootConstantBufferView(I, A); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetComputeRootShaderResourceView(UINT I, D3D12_GPU_VIRTUAL_ADDRESS A) { m_pReal->SetComputeRootShaderResourceView(I, A); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetGraphicsRootShaderResourceView(UINT I, D3D12_GPU_VIRTUAL_ADDRESS A) { m_pReal->SetGraphicsRootShaderResourceView(I, A); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetComputeRootUnorderedAccessView(UINT I, D3D12_GPU_VIRTUAL_ADDRESS A) { m_pReal->SetComputeRootUnorderedAccessView(I, A); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetGraphicsRootUnorderedAccessView(UINT I, D3D12_GPU_VIRTUAL_ADDRESS A) { m_pReal->SetGraphicsRootUnorderedAccessView(I, A); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* V) { m_pReal->IASetIndexBuffer(V); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::IASetVertexBuffers(UINT S, UINT N, const D3D12_VERTEX_BUFFER_VIEW* V) { m_pReal->IASetVertexBuffers(S, N, V); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SOSetTargets(UINT S, UINT N, const D3D12_STREAM_OUTPUT_BUFFER_VIEW* V) { m_pReal->SOSetTargets(S, N, V); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::BeginQuery(ID3D12QueryHeap* H, D3D12_QUERY_TYPE T, UINT I) { m_pReal->BeginQuery(H, T, I); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::EndQuery(ID3D12QueryHeap* H, D3D12_QUERY_TYPE T, UINT I) { m_pReal->EndQuery(H, T, I); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetPredication(ID3D12Resource* B, UINT64 O, D3D12_PREDICATION_OP Op) { m_pReal->SetPredication(B, O, Op); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetMarker(UINT M, const void* D, UINT S) { m_pReal->SetMarker(M, D, S); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::BeginEvent(UINT M, const void* D, UINT S) { m_pReal->BeginEvent(M, D, S); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::EndEvent() { m_pReal->EndEvent(); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::ExecuteIndirect(ID3D12CommandSignature* S, UINT M, ID3D12Resource* A, UINT64 AO, ID3D12Resource* C, UINT64 CO) { m_pReal->ExecuteIndirect(S, M, A, AO, C, CO); }

// ============================================================================
// WRAPPED COMMAND QUEUE IMPLEMENTATION
// ============================================================================

WrappedID3D12CommandQueue::WrappedID3D12CommandQueue(ID3D12CommandQueue* pReal, WrappedID3D12Device* pDeviceWrapper)
    : m_pReal(pReal), m_pDeviceWrapper(pDeviceWrapper), m_refCount(1) {
    if (m_pReal) m_pReal->AddRef();
    if (m_pDeviceWrapper) m_pDeviceWrapper->AddRef();
}

WrappedID3D12CommandQueue::~WrappedID3D12CommandQueue() {
    if (m_pReal) m_pReal->Release();
    if (m_pDeviceWrapper) m_pDeviceWrapper->Release();
}

HRESULT STDMETHODCALLTYPE WrappedID3D12CommandQueue::QueryInterface(REFIID riid, void** ppvObject) {
    if (ppvObject == nullptr) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12CommandQueue)) {
        *ppvObject = this; AddRef(); return S_OK;
    }
    return m_pReal->QueryInterface(riid, ppvObject);
}

ULONG STDMETHODCALLTYPE WrappedID3D12CommandQueue::AddRef() { return InterlockedIncrement(&m_refCount); }
ULONG STDMETHODCALLTYPE WrappedID3D12CommandQueue::Release() { ULONG ref = InterlockedDecrement(&m_refCount); if (ref == 0) delete this; return ref; }
HRESULT STDMETHODCALLTYPE WrappedID3D12CommandQueue::GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) { return m_pReal->GetPrivateData(guid, pDataSize, pData); }
HRESULT STDMETHODCALLTYPE WrappedID3D12CommandQueue::SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) { return m_pReal->SetPrivateData(guid, DataSize, pData); }
HRESULT STDMETHODCALLTYPE WrappedID3D12CommandQueue::SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) { return m_pReal->SetPrivateDataInterface(guid, pData); }
HRESULT STDMETHODCALLTYPE WrappedID3D12CommandQueue::SetName(LPCWSTR Name) { return m_pReal->SetName(Name); }
HRESULT STDMETHODCALLTYPE WrappedID3D12CommandQueue::GetDevice(REFIID riid, void** ppvDevice) { return m_pDeviceWrapper->QueryInterface(riid, ppvDevice); }
void STDMETHODCALLTYPE WrappedID3D12CommandQueue::UpdateTileMappings(ID3D12Resource* pResource, UINT NumResourceRegions, const D3D12_TILED_RESOURCE_COORDINATE* pResourceRegionStartCoordinates, const D3D12_TILE_REGION_SIZE* pResourceRegionSizes, ID3D12Heap* pHeap, UINT NumRanges, const D3D12_TILE_RANGE_FLAGS* pRangeFlags, const UINT* pHeapRangeStartOffsets, const UINT* pRangeTileCounts, D3D12_TILE_MAPPING_FLAGS Flags) { m_pReal->UpdateTileMappings(pResource, NumResourceRegions, pResourceRegionStartCoordinates, pResourceRegionSizes, pHeap, NumRanges, pRangeFlags, pHeapRangeStartOffsets, pRangeTileCounts, Flags); }
void STDMETHODCALLTYPE WrappedID3D12CommandQueue::CopyTileMappings(ID3D12Resource* pDstResource, const D3D12_TILED_RESOURCE_COORDINATE* pDstRegionStartCoordinate, ID3D12Resource* pSrcResource, const D3D12_TILED_RESOURCE_COORDINATE* pSrcRegionStartCoordinate, const D3D12_TILE_REGION_SIZE* pRegionSize, D3D12_TILE_MAPPING_FLAGS Flags) { m_pReal->CopyTileMappings(pDstResource, pDstRegionStartCoordinate, pSrcResource, pSrcRegionStartCoordinate, pRegionSize, Flags); }
void STDMETHODCALLTYPE WrappedID3D12CommandQueue::ExecuteCommandLists(UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists) {
    if (!StreamlineIntegration::Get().IsInitialized()) {
        ID3D12Device* pDevice = nullptr;
        if (SUCCEEDED(m_pReal->GetDevice(__uuidof(ID3D12Device), (void**)&pDevice))) {
            LOG_INFO("Lazy initializing Streamline via ExecuteCommandLists...");
            StreamlineIntegration::Get().Initialize(pDevice);
            pDevice->Release();
        }
    }
    ResourceDetector::Get().NewFrame();
    StreamlineIntegration::Get().SetCommandQueue(m_pReal);
    static bool s_camHookBannerLogged = false;
    if (!s_camHookBannerLogged) {
        LOG_INFO("[CAM] Camera scan active (Close hook)");
        s_camHookBannerLogged = true;
    }
    ID3D12Resource* pColor = ResourceDetector::Get().GetBestColorCandidate();
    ID3D12Resource* pDepth = ResourceDetector::Get().GetBestDepthCandidate();
    ID3D12Resource* pMVs = ResourceDetector::Get().GetBestMotionVectorCandidate();
    if (pColor) StreamlineIntegration::Get().TagColorBuffer(pColor);
    if (pDepth) StreamlineIntegration::Get().TagDepthBuffer(pDepth);
    if (pMVs) StreamlineIntegration::Get().TagMotionVectors(pMVs);
    m_pReal->ExecuteCommandLists(NumCommandLists, ppCommandLists);
}
void STDMETHODCALLTYPE WrappedID3D12CommandQueue::SetMarker(UINT Metadata, const void* pData, UINT Size) { m_pReal->SetMarker(Metadata, pData, Size); }
void STDMETHODCALLTYPE WrappedID3D12CommandQueue::BeginEvent(UINT Metadata, const void* pData, UINT Size) { m_pReal->BeginEvent(Metadata, pData, Size); }
void STDMETHODCALLTYPE WrappedID3D12CommandQueue::EndEvent() { m_pReal->EndEvent(); }
HRESULT STDMETHODCALLTYPE WrappedID3D12CommandQueue::Signal(ID3D12Fence* pFence, UINT64 Value) { return m_pReal->Signal(pFence, Value); }
HRESULT STDMETHODCALLTYPE WrappedID3D12CommandQueue::Wait(ID3D12Fence* pFence, UINT64 Value) { return m_pReal->Wait(pFence, Value); }
HRESULT STDMETHODCALLTYPE WrappedID3D12CommandQueue::GetTimestampFrequency(UINT64* pFrequency) { return m_pReal->GetTimestampFrequency(pFrequency); }
HRESULT STDMETHODCALLTYPE WrappedID3D12CommandQueue::GetClockCalibration(UINT64* pGpuTimestamp, UINT64* pCpuTimestamp) { return m_pReal->GetClockCalibration(pGpuTimestamp, pCpuTimestamp); }
D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE WrappedID3D12CommandQueue::GetDesc() { return m_pReal->GetDesc(); }

// ============================================================================
// WRAPPED DEVICE IMPLEMENTATION
// ============================================================================

WrappedID3D12Device::WrappedID3D12Device(ID3D12Device* pReal) : m_pReal(pReal), m_refCount(1) { if(m_pReal) m_pReal->AddRef(); }
WrappedID3D12Device::~WrappedID3D12Device() {
    {
        std::lock_guard<std::mutex> lock(g_samplerMutex);
        g_samplerRecords.clear();
    }
    if (m_pReal) m_pReal->Release();
}
HRESULT STDMETHODCALLTYPE WrappedID3D12Device::QueryInterface(REFIID riid, void** ppvObject) {
    if (ppvObject == nullptr) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12Device)) {
        *ppvObject = this; AddRef(); return S_OK;
    }
    return m_pReal->QueryInterface(riid, ppvObject);
}
ULONG STDMETHODCALLTYPE WrappedID3D12Device::AddRef() { return InterlockedIncrement(&m_refCount); }
ULONG STDMETHODCALLTYPE WrappedID3D12Device::Release() { ULONG ref = InterlockedDecrement(&m_refCount); if (ref == 0) delete this; return ref; }
HRESULT STDMETHODCALLTYPE WrappedID3D12Device::GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) { return m_pReal->GetPrivateData(guid, pDataSize, pData); }
HRESULT STDMETHODCALLTYPE WrappedID3D12Device::SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) { return m_pReal->SetPrivateData(guid, DataSize, pData); }
HRESULT STDMETHODCALLTYPE WrappedID3D12Device::SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) { return m_pReal->SetPrivateDataInterface(guid, pData); }
HRESULT STDMETHODCALLTYPE WrappedID3D12Device::SetName(LPCWSTR Name) { return m_pReal->SetName(Name); }

HRESULT STDMETHODCALLTYPE WrappedID3D12Device::CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC* pDesc, REFIID riid, void** ppCommandQueue) {
    ID3D12CommandQueue* pRealQueue = nullptr;
    HRESULT hr = m_pReal->CreateCommandQueue(pDesc, __uuidof(ID3D12CommandQueue), (void**)&pRealQueue);
    if (FAILED(hr)) return hr;
    WrappedID3D12CommandQueue* pWrapper = new WrappedID3D12CommandQueue(pRealQueue, this);
    hr = pWrapper->QueryInterface(riid, ppCommandQueue);
    pWrapper->Release();
    pRealQueue->Release();
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedID3D12Device::CreateCommandList(UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* pCommandAllocator, ID3D12PipelineState* pInitialState, REFIID riid, void** ppCommandList) {
    ID3D12GraphicsCommandList* pRealList = nullptr;
    HRESULT hr = m_pReal->CreateCommandList(nodeMask, type, pCommandAllocator, pInitialState, __uuidof(ID3D12GraphicsCommandList), (void**)&pRealList);
    if (FAILED(hr)) return hr;
    if (type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COMPUTE) {
        WrappedID3D12GraphicsCommandList* pWrapper = new WrappedID3D12GraphicsCommandList(pRealList, this);
        hr = pWrapper->QueryInterface(riid, ppCommandList);
        pWrapper->Release(); pRealList->Release();
    } else {
        hr = pRealList->QueryInterface(riid, ppCommandList);
        pRealList->Release();
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedID3D12Device::CreateCommittedResource(const D3D12_HEAP_PROPERTIES* pH, D3D12_HEAP_FLAGS HF, const D3D12_RESOURCE_DESC* pD, D3D12_RESOURCE_STATES IS, const D3D12_CLEAR_VALUE* pO, REFIID riid, void** ppv) {
    HRESULT hr = m_pReal->CreateCommittedResource(pH, HF, pD, IS, pO, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv) {
        ID3D12Resource* pRes = (ID3D12Resource*)*ppv;
        ResourceDetector::Get().RegisterResource(pRes);
        if (pD && pH && pH->Type == D3D12_HEAP_TYPE_UPLOAD && pD->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            uint8_t* mapped = nullptr; D3D12_RANGE range = { 0, 0 };
            if (SUCCEEDED(pRes->Map(0, &range, reinterpret_cast<void**>(&mapped))) && mapped) {
                RegisterCbv(pRes, pD->Width, mapped);
            }
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedID3D12Device::CreatePlacedResource(ID3D12Heap* pH, UINT64 HO, const D3D12_RESOURCE_DESC* pD, D3D12_RESOURCE_STATES IS, const D3D12_CLEAR_VALUE* pO, REFIID riid, void** ppv) {
    HRESULT hr = m_pReal->CreatePlacedResource(pH, HO, pD, IS, pO, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv && pD) {
        ID3D12Resource* pRes = (ID3D12Resource*)*ppv;
        
        if (pD->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
            DXGI_FORMAT f = pD->Format;
            if (f == DXGI_FORMAT_R16G16_FLOAT || f == DXGI_FORMAT_R16G16_UNORM || f == DXGI_FORMAT_R16G16_TYPELESS || f == DXGI_FORMAT_D32_FLOAT || f == DXGI_FORMAT_R32_FLOAT || f == DXGI_FORMAT_R32_TYPELESS || f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R10G10B10A2_UNORM) {
                ResourceDetector::Get().RegisterResource(pRes);
            }
        }
        else if (pD->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && pH) {
            D3D12_HEAP_DESC hDesc = pH->GetDesc();
            if (hDesc.Properties.Type == D3D12_HEAP_TYPE_UPLOAD) {
                uint8_t* mapped = nullptr; D3D12_RANGE range = { 0, 0 };
                if (SUCCEEDED(pRes->Map(0, &range, reinterpret_cast<void**>(&mapped))) && mapped) {
                    RegisterCbv(pRes, pD->Width, mapped);
                }
            }
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedID3D12Device::CreateReservedResource(const D3D12_RESOURCE_DESC* pD, D3D12_RESOURCE_STATES IS, const D3D12_CLEAR_VALUE* pO, REFIID riid, void** ppv) {
    HRESULT hr = m_pReal->CreateReservedResource(pD, IS, pO, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv && pD && pD->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        DXGI_FORMAT f = pD->Format;
        if (f == DXGI_FORMAT_R16G16_FLOAT || f == DXGI_FORMAT_R16G16_UNORM || f == DXGI_FORMAT_R16G16_TYPELESS || f == DXGI_FORMAT_D32_FLOAT || f == DXGI_FORMAT_R32_FLOAT || f == DXGI_FORMAT_R32_TYPELESS) {
            ResourceDetector::Get().RegisterResource((ID3D12Resource*)*ppv);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedID3D12Device::CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC* pDesc, REFIID riid, void** ppvHeap) {
    HRESULT hr = m_pReal->CreateDescriptorHeap(pDesc, riid, ppvHeap);
    if (SUCCEEDED(hr) && ppvHeap && *ppvHeap && pDesc && pDesc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
        UINT descriptorSize = m_pReal->GetDescriptorHandleIncrementSize(pDesc->Type);
        TrackDescriptorHeap((ID3D12DescriptorHeap*)*ppvHeap, descriptorSize);
    }
    return hr;
}

void STDMETHODCALLTYPE WrappedID3D12Device::CreateShaderResourceView(ID3D12Resource* pResource, const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    m_pReal->CreateShaderResourceView(pResource, pDesc, DestDescriptor);
    if (!pResource) return;
    if (pDesc && pDesc->ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2D) return;
    DXGI_FORMAT format = pDesc ? pDesc->Format : pResource->GetDesc().Format;
    TrackDescriptorResource(DestDescriptor, pResource, format);
}

void STDMETHODCALLTYPE WrappedID3D12Device::CreateUnorderedAccessView(ID3D12Resource* pResource, ID3D12Resource* pCounterResource, const D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    m_pReal->CreateUnorderedAccessView(pResource, pCounterResource, pDesc, DestDescriptor);
    if (!pResource) return;
    if (pDesc && pDesc->ViewDimension != D3D12_UAV_DIMENSION_TEXTURE2D) return;
    DXGI_FORMAT format = pDesc ? pDesc->Format : pResource->GetDesc().Format;
    TrackDescriptorResource(DestDescriptor, pResource, format);
}

void STDMETHODCALLTYPE WrappedID3D12Device::CreateRenderTargetView(ID3D12Resource* pResource, const D3D12_RENDER_TARGET_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    m_pReal->CreateRenderTargetView(pResource, pDesc, DestDescriptor);
    if (!pResource) return;
    DXGI_FORMAT format = pDesc ? pDesc->Format : pResource->GetDesc().Format;
    TrackDescriptorResource(DestDescriptor, pResource, format);
}

void STDMETHODCALLTYPE WrappedID3D12Device::CreateDepthStencilView(ID3D12Resource* pResource, const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    m_pReal->CreateDepthStencilView(pResource, pDesc, DestDescriptor);
    if (!pResource) return;
    DXGI_FORMAT format = pDesc ? pDesc->Format : pResource->GetDesc().Format;
    TrackDescriptorResource(DestDescriptor, pResource, format);
}

void STDMETHODCALLTYPE WrappedID3D12Device::CreateSampler(const D3D12_SAMPLER_DESC* pD, D3D12_CPU_DESCRIPTOR_HANDLE Dest) {
    if (pD) {
        D3D12_SAMPLER_DESC nD = *pD;
        float bias = StreamlineIntegration::Get().GetLODBias();
        if (bias != 0.0f) { nD.MipLODBias += bias; nD.MipLODBias = std::clamp(nD.MipLODBias, -3.0f, 3.0f); }
        m_pReal->CreateSampler(&nD, Dest);
        SamplerRecord record{};
        record.desc = *pD;
        record.cpuHandle = Dest;
        record.device = m_pReal;
        record.valid = true;
        {
            std::lock_guard<std::mutex> lock(g_samplerMutex);
            for (auto& existing : g_samplerRecords) {
                if (existing.cpuHandle.ptr == Dest.ptr) {
                    existing = record;
                    return;
                }
            }
            g_samplerRecords.push_back(record);
        }
    } else m_pReal->CreateSampler(pD, Dest);
}

void STDMETHODCALLTYPE WrappedID3D12Device::CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    m_pReal->CreateConstantBufferView(pDesc, DestDescriptor);
    if (pDesc && pDesc->BufferLocation) {
        TrackCbvDescriptor(DestDescriptor, pDesc);
    }
}
