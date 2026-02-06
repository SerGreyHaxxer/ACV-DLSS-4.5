#pragma once
#include <d3d12.h>
#include <dxgi.h>

void TrackDescriptorHeap(ID3D12DescriptorHeap* heap, UINT descriptorSize);
void TrackDescriptorResource(D3D12_CPU_DESCRIPTOR_HANDLE handle, ID3D12Resource* resource, DXGI_FORMAT format);
bool TryResolveDescriptorResource(D3D12_CPU_DESCRIPTOR_HANDLE handle, ID3D12Resource** outResource, DXGI_FORMAT* outFormat);
