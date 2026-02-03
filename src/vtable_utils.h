#pragma once

#include <windows.h>
#include <cstdint>
#include <cstddef>

inline bool IsReadablePtrRange(const void* ptr, size_t size = sizeof(void*)) {
    if (!ptr) return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if ((mbi.Protect & PAGE_NOACCESS) || (mbi.Protect & PAGE_GUARD)) return false;

    auto base = static_cast<const uint8_t*>(mbi.BaseAddress);
    auto end = base + mbi.RegionSize;
    auto p = static_cast<const uint8_t*>(ptr);
    return p >= base && (p + size) <= end;
}

inline bool ResolveVTableEntry(void* object, int index, void*** outVtable, void*** outEntry) {
    if (!object || !outVtable || !outEntry || index < 0) return false;
    void** vtable = *reinterpret_cast<void***>(object);
    if (!IsReadablePtrRange(vtable, sizeof(void*) * (index + 1))) return false;
    void** entry = &vtable[index];
    if (!IsReadablePtrRange(entry)) return false;
    *outVtable = vtable;
    *outEntry = entry;
    return true;
}
