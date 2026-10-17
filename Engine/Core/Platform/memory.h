#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace dingosdk::memory {
inline constexpr std::uintptr_t highest_user_address = 0x00007fffffffffffULL;

// Under Wine, ReadProcessMemory on this process is a request to the wineserver, which reads the
// memory back through the host's process-memory calls: far more than Windows' system call per
// read. On a Mac (CrossOver 24's Wine) the twice-a-second world and customization reads held the
// game's update thread for 14-21 ms each (perf status: "world model" 20.6 ms, "local profile"
// 13.7 ms; client updates 23.6/s of 30). There, a read is a plain copy instead, made only when
// VirtualQuery, which Wine answers in-process, shows every page committed, readable and not a
// guard page: it fails where ReadProcessMemory would, without touching a guard page.
inline bool under_wine() noexcept {
    static const bool wine = [] {
        const auto ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
    }();
    return wine;
}
inline bool readable(std::uintptr_t address, std::size_t size) noexcept {
    constexpr DWORD readable_protection = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const auto end = address + size;
    for (auto at = address; at < end;) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(info.Protect & readable_protection))
            return false;
        const auto region_end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (region_end <= at) return false;
        at = region_end;
    }
    return true;
}
// The copy itself, also for tests: false when a page is not readable (or stops being so).
inline bool copy_if_readable(std::uintptr_t address, void* destination, std::size_t size) noexcept {
    if (!readable(address, size)) return false;
    __try {
        std::memcpy(destination, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool read_bytes(std::uintptr_t address, void* destination, std::size_t size) noexcept {
    if (!destination || !size || size > highest_user_address || address < 0x10000 ||
        address > highest_user_address - size) return false;
    if (under_wine()) return copy_if_readable(address, destination, size);
    SIZE_T count{};
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), destination, size, &count) && count == size;
}
template<class T> requires std::is_trivially_copyable_v<T>
bool read(std::uintptr_t address, T& value) noexcept { return read_bytes(address, &value, sizeof(value)); }

// The same read without a kernel transition: a plain copy that stops at an access violation.
// ReadProcessMemory above costs a system call (about 1 us) per read, which adds up on paths
// that run every game frame. Use peek there, for game objects expected to be readable; a
// failed peek costs an exception, so keep read for probing addresses that are often invalid.
inline bool peek_bytes(std::uintptr_t address, void* destination, std::size_t size) noexcept {
    if (!destination || !size || size > highest_user_address || address < 0x10000 ||
        address > highest_user_address - size) return false;
    __try {
        std::memcpy(destination, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template<class T> requires std::is_trivially_copyable_v<T>
bool peek(std::uintptr_t address, T& value) noexcept { return peek_bytes(address, &value, sizeof(value)); }

// A NUL-terminated string copied the same way: the length (terminator written at destination[length]),
// or -1 if it is unreadable or has no terminator within capacity bytes. Reads byte by byte, so it never
// touches memory past the terminator.
inline std::ptrdiff_t peek_cstring(std::uintptr_t address, char* destination, std::size_t capacity) noexcept {
    if (!destination || !capacity || address < 0x10000 || address > highest_user_address - capacity) return -1;
    __try {
        const auto* source = reinterpret_cast<const char*>(address);
        for (std::size_t i = 0; i < capacity; ++i) {
            destination[i] = source[i];
            if (destination[i] == '\0') return static_cast<std::ptrdiff_t>(i);
        }
        return -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
}
