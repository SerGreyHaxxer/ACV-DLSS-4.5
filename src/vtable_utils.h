#pragma once

#include <concepts>
#include <cstddef>
#include <type_traits>
#include <windows.h>

// ============================================================================
// VTable Index Strong Types — eliminate magic numbers in hook setup
// ============================================================================

namespace vtable {

enum class Device : size_t {
  CreateCommandQueue       = 8,
  CreateCommandAllocator   = 9,
  CreateCommandList        = 12,
  CreateShaderResourceView   = 18,
  CreateUnorderedAccessView  = 19,
  CreateRenderTargetView     = 20,
  CreateDepthStencilView     = 21,
  CreateSampler              = 22,
  CreateCommittedResource    = 27,
  CreatePlacedResource       = 29,
};

enum class CommandList : size_t {
  Close                              = 9,
  ResourceBarrier                    = 26,
  SetComputeRootConstantBufferView   = 37,
  SetGraphicsRootConstantBufferView  = 38,
  ClearDepthStencilView              = 47,
  ClearRenderTargetView              = 48,
};

enum class CommandQueue : size_t {
  ExecuteCommandLists = 10,
};

} // namespace vtable

// ============================================================================
// Concept: D3D12 Hookable COM Interface
// ============================================================================

template<typename T>
concept D3D12Hookable = std::is_base_of_v<IUnknown, T> && requires(T* t) {
  { *reinterpret_cast<void***>(t) } -> std::same_as<void**&>;
};

// ============================================================================
// VTable Utilities
// ============================================================================

template <typename T> inline void **GetVTable(T *obj) {
  return *reinterpret_cast<void ***>(obj);
}

template <typename T> inline T GetVTableFunc(void **vtable, size_t index) {
  return reinterpret_cast<T>(vtable[index]);
}

// Overload for strongly-typed VTable indices
template <typename Func, typename Index>
  requires std::is_enum_v<Index>
inline void* GetVTableEntry(void **vtable, Index index) {
  return vtable[static_cast<size_t>(index)];
}
