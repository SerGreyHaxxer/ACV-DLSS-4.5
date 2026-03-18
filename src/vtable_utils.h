/*
 * Copyright (C) 2026 acerthyracer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once

#include <windows.h>

#include <concepts>
#include <cstddef>
#include <type_traits>
#include <utility>


// ============================================================================
// VTable Index Strong Types — eliminate magic numbers in hook setup
// ============================================================================

namespace vtable {

enum class Device : size_t {
  CreateCommandQueue = 8,
  CreateCommandAllocator = 9,
  CreateCommandList = 12,
  CreateDescriptorHeap = 14,
  CreateConstantBufferView = 17,
  CreateShaderResourceView = 18,
  CreateUnorderedAccessView = 19,
  CreateRenderTargetView = 20,
  CreateDepthStencilView = 21,
  CreateSampler = 22,
  CreateCommittedResource = 27,
  CreatePlacedResource = 29,
};

enum class CommandList : size_t {
  Close = 9,
  ResourceBarrier = 26,
  SetComputeRootConstantBufferView = 37,
  SetGraphicsRootConstantBufferView = 38,
  ClearDepthStencilView = 47,
  ClearRenderTargetView = 48,
};

enum class CommandQueue : size_t {
  ExecuteCommandLists = 10,
};

enum class Resource : size_t {
  Map = 8,
  Unmap = 9,
  GetGPUVirtualAddress = 11,
};

enum class SwapChain : size_t {
  Present = 8,
  ResizeBuffers = 13,
};

// IDXGIFactory vtable slots (for VTable hooking, Fix 1.3)
// IDXGIFactory: IUnknown(3) + IDXGIObject(4) + EnumAdapters(1) +
//   MakeWindowAssociation(1) + GetWindowAssociation(1) + CreateSwapChain(1) +
//   CreateSoftwareAdapter(1) = slots 0-11
// IDXGIFactory2 adds CreateSwapChainForHwnd at slot 15,
//   CreateSwapChainForCoreWindow at slot 16, ...,
//   CreateSwapChainForComposition at slot 24
enum class DXGIFactory : size_t {
  CreateSwapChain = 10,
  CreateSwapChainForHwnd = 15,
  CreateSwapChainForCoreWindow = 16,
  CreateSwapChainForComposition = 24,
};

} // namespace vtable

// ============================================================================
// Concept: D3D12 Hookable COM Interface
// ============================================================================

template <typename T>
concept D3D12Hookable = std::is_base_of_v<IUnknown, T> && requires(T* t) {
  { *reinterpret_cast<void***>(t) } -> std::same_as<void**&>;
};

// ============================================================================
// VTable Utilities
// ============================================================================

template <typename T> inline void** GetVTable(T* obj) {
  return *reinterpret_cast<void***>(obj);
}

// Primary template: retrieves a typed function pointer from a vtable.
// For raw size_t indices (backward compatibility).
template <typename T> [[nodiscard]] inline T GetVTableFunc(void** vtable, size_t index) noexcept {
  return reinterpret_cast<T>(vtable[index]);
}

// Overload for strongly-typed enum VTable indices.
// Uses C++23 std::to_underlying to safely extract the enum's backing
// integer type — cleaner and more type-safe than static_cast<size_t>.
// Returns the typed FuncPtr directly, eliminating reinterpret_cast at call sites.
template <typename FuncPtr, typename EnumType>
  requires std::is_enum_v<EnumType>
[[nodiscard]] inline FuncPtr GetVTableFunc(void** vtable, EnumType index) noexcept {
  return reinterpret_cast<FuncPtr>(vtable[std::to_underlying(index)]);
}

// Legacy compatibility alias — same as enum-typed GetVTableFunc but returns void*.
template <typename FuncPtr, typename EnumType>
  requires std::is_enum_v<EnumType>
[[nodiscard]] inline void* GetVTableEntry(void** vtable, EnumType index) noexcept {
  return vtable[std::to_underlying(index)];
}
