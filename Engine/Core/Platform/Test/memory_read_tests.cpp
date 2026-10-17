// memory::copy_if_readable, the read memory::read_bytes makes under Wine (see memory.h): it must
// copy what ReadProcessMemory copies and fail where ReadProcessMemory fails, without touching a
// guard page. Run on Windows, where the same calls answer as they do under Wine.
#include "Engine/Core/Platform/memory.h"
#include <cstdio>
#include <cstring>

namespace {
int failures = 0;
void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAILED: %s\n", what);
        ++failures;
    }
}
std::uintptr_t at(void* pointer, std::size_t offset = 0) { return reinterpret_cast<std::uintptr_t>(pointer) + offset; }
}

int main() {
    using namespace dingosdk;
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const std::size_t page = info.dwPageSize;

    // Two committed pages: readable within and across them.
    auto* pages = static_cast<unsigned char*>(VirtualAlloc(nullptr, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(pages != nullptr, "allocate two pages");
    if (!pages) return 1;
    for (std::size_t i = 0; i < page * 2; ++i) pages[i] = static_cast<unsigned char>(i * 7);
    unsigned char copy[64]{};
    check(memory::copy_if_readable(at(pages, 16), copy, sizeof(copy)), "copy within a page");
    check(std::memcmp(copy, pages + 16, sizeof(copy)) == 0, "copy within a page matches");
    check(memory::copy_if_readable(at(pages, page - 8), copy, 16), "copy across two readable pages");
    check(std::memcmp(copy, pages + page - 8, 16) == 0, "copy across two readable pages matches");
    std::uint64_t value{};
    check(memory::read(at(pages, 32), value) && std::memcmp(&value, pages + 32, sizeof(value)) == 0, "read<T> on this process");

    // The second page made inaccessible: a read reaching into it fails, the first page still reads.
    DWORD old{};
    check(VirtualProtect(pages + page, page, PAGE_NOACCESS, &old) != 0, "protect the second page");
    check(!memory::copy_if_readable(at(pages, page - 8), copy, 16), "no copy into a PAGE_NOACCESS page");
    check(!memory::read_bytes(at(pages, page - 8), copy, 16), "no read_bytes into a PAGE_NOACCESS page");
    check(memory::copy_if_readable(at(pages, 0), copy, 16), "the readable page still copies");

    // A guard page: refused, and left a guard page (copying would have cleared PAGE_GUARD).
    check(VirtualProtect(pages + page, page, PAGE_READWRITE | PAGE_GUARD, &old) != 0, "make the second page a guard page");
    check(!memory::copy_if_readable(at(pages, page), copy, 8), "no copy from a guard page");
    MEMORY_BASIC_INFORMATION guard{};
    check(VirtualQuery(pages + page, &guard, sizeof(guard)) == sizeof(guard) && (guard.Protect & PAGE_GUARD),
          "the guard page is still a guard page");
    VirtualFree(pages, 0, MEM_RELEASE);

    // Reserved but not committed: refused.
    auto* reserved = VirtualAlloc(nullptr, page, MEM_RESERVE, PAGE_READWRITE);
    check(reserved != nullptr, "reserve a page");
    if (reserved) {
        check(!memory::copy_if_readable(at(reserved), copy, 8), "no copy from reserved memory");
        VirtualFree(reserved, 0, MEM_RELEASE);
    }

    std::printf("memory read tests: %s (under Wine: %s)\n", failures ? "FAILED" : "passed",
                memory::under_wine() ? "yes" : "no");
    return failures ? 1 : 0;
}
