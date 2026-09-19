#include "vtable_slot.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Object { void** table; unsigned updates = 0; float x = 100; };
using Update = void(__thiscall*)(Object*);
void __fastcall Original(Object* object, void*) { ++object->updates; object->x += 1; }
void __fastcall Correct(Object* object, void*) { Original(object, nullptr); object->x += 8; }
__declspec(noinline) void Call(Object* object) {
    reinterpret_cast<Update>(object->table[3])(object);
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        Check(argc == 2, "DLL path required");
        auto table = static_cast<void**>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        Check(table != nullptr, "allocate fixture table");
        table[3] = reinterpret_cast<void*>(&Original);
        DWORD old = 0;
        Check(VirtualProtect(table, 4096, PAGE_READONLY, &old) != FALSE, "protect fixture table");
        Object object{table}; Call(&object);
        Check(object.updates == 1 && object.x == 101, "original call");
        Check(authority::ExchangeSlot(table + 3, reinterpret_cast<void*>(&Original), reinterpret_cast<void*>(&Correct)) == 0,
            "install slot hook");
        Call(&object);
        Check(object.updates == 2 && object.x == 110, "original once then correction, x86 calling convention");
        Check(authority::ExchangeSlot(table + 3, reinterpret_cast<void*>(&Original), nullptr) == ERROR_REVISION_MISMATCH,
            "do not overwrite another slot owner");
        MEMORY_BASIC_INFORMATION page{}; VirtualQuery(table, &page, sizeof(page));
        Check(page.Protect == PAGE_READONLY, "page protection restored");
        Check(authority::ExchangeSlot(table + 3, reinterpret_cast<void*>(&Correct), reinterpret_cast<void*>(&Original)) == 0,
            "restore original slot");
        Call(&object); Check(object.updates == 3 && object.x == 111, "original behavior restored");
        VirtualFree(table, 0, MEM_RELEASE);
        HMODULE dll = LoadLibraryW(argv[1]); Check(dll != nullptr, "load production adapter");
        using Export = DWORD(WINAPI*)(void*);
        auto start = reinterpret_cast<Export>(GetProcAddress(dll, "IsaacAuthorityStart"));
        auto correct = reinterpret_cast<Export>(GetProcAddress(dll, "IsaacAuthorityCorrectOnce"));
        auto stop = reinterpret_cast<Export>(GetProcAddress(dll, "IsaacAuthorityStop"));
        Check(start && correct && stop, "production exports exist");
        Check(start(nullptr) == ERROR_BAD_EXE_FORMAT, "reject unsupported executable");
        auto network = reinterpret_cast<Export>(GetProcAddress(dll, "IsaacAuthorityNetworkStart"));
        if (network) Check(network(nullptr) == ERROR_BAD_EXE_FORMAT, "network start rejects unsupported executable");
        auto replica = reinterpret_cast<Export>(GetProcAddress(dll, "IsaacAuthorityReplicaStart"));
        if (replica) Check(replica(nullptr) == ERROR_BAD_EXE_FORMAT, "replica start rejects unsupported executable");
        auto source = reinterpret_cast<Export>(GetProcAddress(dll, "IsaacAuthoritySourceStart"));
        if (source) Check(source(nullptr) == ERROR_BAD_EXE_FORMAT, "source start rejects unsupported executable");
        Check(correct(nullptr) == ERROR_NOT_READY && stop(nullptr) == ERROR_NOT_READY, "inactive exports do not write");
        FreeLibrary(dll);
        std::cout << "PASS vtable calling convention, correction order, restore, unsupported-host guard\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
