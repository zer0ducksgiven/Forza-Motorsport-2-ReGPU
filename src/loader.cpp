#include "loader.h"

#include <fstream>
#include <windows.h>
#include <vector>

#include <ppc_config.h>
#include <ppc_context.h>

#include <image.h>
#ifdef FM2_TITLE_UPDATE
#include <xex_patcher.h>
#endif

#include "log.h"
#include "memory.h"
#include "user_dir.h"
#include "kernel/vfs.h"

#include <cstring>
#include <filesystem>

LoadedTitle g_title;

namespace
{
constexpr uint32_t kXexHeaderTls = 0x00020104;
constexpr uint32_t kXexHeaderStackSize = 0x00020200;
constexpr uint32_t kXexHeaderImports = 0x000103FF;
constexpr uint32_t kXexHeaderExecutionId = 0x00040006;
constexpr uint32_t kXexHeaderSystemFlags = 0x00030000;

std::vector<uint8_t> ReadFile(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        Fatal("cannot open %s", path.string().c_str());
    return { std::istreambuf_iterator<char>(f), {} };
}

uint32_t Be32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint32_t AllocZeroed(uint32_t size)
{
    return GuestAlloc(Heap::Virtual, size, 16);
}

// Guest storage for a kernel variable export, by xboxkrnl ordinal.
uint32_t KernelVariable(uint32_t ordinal)
{
    switch (ordinal)
    {
    case 0x01B: // ExThreadObjectType
    {
        const uint32_t type = AllocZeroed(0x40);
        StoreBE32(type, 0x54687264); // 'Thrd', only compared by address
        return type;
    }
    case 0x059: // KeDebugMonitorData
    case 0x266: // KeCertMonitorData
    case 0x1C1: // VdHSIOCalibrationLock
        return AllocZeroed(0x100);
    case 0x0AD: // KeTimeStampBundle: InterruptTime, SystemTime, TickCount
        g_title.keTimeStampBundle = AllocZeroed(0x18);
        return g_title.keTimeStampBundle;
    case 0x1AE: // ExLoadedCommandLine
    {
        const uint32_t s = AllocZeroed(0x100);
        strcpy(GuestPtr<char>(s), "\"default.xex\"");
        return s;
    }
    case 0x193: // XexExecutableModuleHandle: pointer to the module handle
    {
        const uint32_t p = AllocZeroed(4);
        StoreBE32(p, g_title.ldrEntry);
        return p;
    }
    case 0x1C0: // VdGpuClockInMHz
    {
        const uint32_t p = AllocZeroed(4);
        StoreBE32(p, 500);
        return p;
    }
    case 0x158: // XboxKrnlVersion: major, minor, build, qfe
    {
        const uint32_t p = AllocZeroed(8);
        auto* v = GuestPtr<uint16_t>(p);
        v[0] = __builtin_bswap16(2);
        v[1] = __builtin_bswap16(0);
        v[2] = __builtin_bswap16(17559);
        v[3] = 0;
        return p;
    }
    case 0x1BE: // VdGlobalDevice
        return AllocZeroed(4);
    default:
        Log("loader: unknown kernel variable ordinal %03X, giving it zeroed storage", ordinal);
        return AllocZeroed(0x100);
    }
}

void ResolveVariableImports(const uint8_t* xex, uint32_t textStart, uint32_t textEnd)
{
    const uint32_t importsOffset = [&] {
        const uint32_t count = Be32(xex + 0x14);
        for (uint32_t i = 0; i < count; i++)
        {
            if (Be32(xex + 0x18 + i * 8) == kXexHeaderImports)
                return Be32(xex + 0x18 + i * 8 + 4);
        }
        return 0u;
    }();
    if (!importsOffset)
        return;

    const uint8_t* header = xex + importsOffset;
    const uint32_t stringTableSize = Be32(header + 4);
    const uint32_t libraryCount = Be32(header + 8);
    std::vector<std::string> names;
    for (const char* p = reinterpret_cast<const char*>(header + 12); names.size() < libraryCount;)
    {
        names.emplace_back(p);
        p += ((names.back().size() + 1) + 3) & ~size_t(3);
    }

    const uint8_t* library = header + 12 + stringTableSize;
    for (uint32_t lib = 0; lib < libraryCount; lib++)
    {
        const uint32_t librarySize = Be32(library);
        const uint16_t nameIndex = (library[0x24] << 8) | library[0x25];
        const uint16_t recordCount = (library[0x26] << 8) | library[0x27];
        auto inText = [&](uint32_t a) { return a >= textStart && a < textEnd; };
        for (uint32_t r = 0; r < recordCount; r++)
        {
            const uint32_t record = Be32(library + 0x28 + r * 4);
            if (inText(record))
                continue; // function thunk, replaced by the recompiled import
            if (r + 1 < recordCount)
            {
                // A function import: an address-table slot followed by its
                // thunk. Point the slot at the thunk for indirect calls.
                const uint32_t next = Be32(library + 0x28 + (r + 1) * 4);
                if (inText(next))
                {
                    StoreBE32(record, next);
                    continue;
                }
            }
            // XenonUtils leaves variable slots byte-swapped: host order now.
            const uint32_t value = *GuestPtr<uint32_t>(record);
            const uint32_t ordinal = value & 0xFFFF;
            if (names[nameIndex] != "xboxkrnl.exe")
            {
                Log("loader: variable import %03X from %s not supported", ordinal, names[nameIndex].c_str());
                StoreBE32(record, AllocZeroed(0x100));
                continue;
            }
            StoreBE32(record, KernelVariable(ordinal));
        }
        library += librarySize;
    }
}

uint32_t OptHeader(const uint8_t* xex, uint32_t key, bool* found = nullptr)
{
    const uint32_t count = Be32(xex + 0x14);
    for (uint32_t i = 0; i < count; i++)
    {
        if (Be32(xex + 0x18 + i * 8) == key)
        {
            if (found)
                *found = true;
            return Be32(xex + 0x18 + i * 8 + 4);
        }
    }
    if (found)
        *found = false;
    return 0;
}
}

uint32_t XexHeaderField(uint32_t headerBase, uint32_t key)
{
    const uint8_t* xex = GuestPtr<uint8_t>(headerBase);
    const uint32_t count = Be32(xex + 0x14);
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t entry = 0x18 + i * 8;
        if (Be32(xex + entry) != key)
            continue;
        switch (key & 0xFF)
        {
        case 0x00:
            return Be32(xex + entry + 4);
        case 0x01:
            return headerBase + entry + 4;
        default:
            return headerBase + Be32(xex + entry + 4);
        }
    }
    return 0;
}

const std::filesystem::path& TitleUpdateDir()
{
    static const std::filesystem::path dir = [] {
        char buffer[MAX_PATH];
        if (GetEnvironmentVariableA("FM2_TITLE_UPDATE_DIR", buffer, MAX_PATH))
            return std::filesystem::path(buffer);
        return DocumentsUserDir() / "update";
    }();
    return dir;
}

void LoadTitle(const std::filesystem::path& xexPath)
{
    std::vector<uint8_t> file = ReadFile(xexPath);
#ifdef FM2_TITLE_UPDATE
    {
        // This build's recompiled code is the title-updated executable: the update's patch
        // turns the disc's default.xex into it (the same patch XenonRecomp applied).
        const std::filesystem::path xexp = TitleUpdateDir() / "default.xexp";
        if (!std::filesystem::exists(xexp))
            Fatal("this build needs the title update: %s is missing (tools/stfs.py update <package> <folder>)", xexp.string().c_str());
        const std::vector<uint8_t> patch = ReadFile(xexp);
        std::vector<uint8_t> patched;
        const auto result = XexPatcher::apply(file.data(), file.size(), patch.data(), patch.size(), patched, false);
        if (result != XexPatcher::Result::Success)
            Fatal("could not apply %s to %s (result %d): is it the right title update for this disc?", xexp.string().c_str(),
                xexPath.string().c_str(), int(result));
        file = std::move(patched);
        Log("loader: applied the title update %s (%zu bytes)", xexp.string().c_str(), patch.size());
    }
#endif
    Image image = Image::ParseImage(file.data(), file.size());
    if (!image.data)
        Fatal("%s: could not decrypt/decompress the XEX", xexPath.string().c_str());
    if (image.base != PPC_IMAGE_BASE)
        Fatal("%s: image base %08zX does not match the recompiled code (%08llX); wrong default.xex?", xexPath.string().c_str(), image.base, PPC_IMAGE_BASE);

    // PE headers, then every section.
    memcpy(GuestPtr(image.base), image.data.get(), 0x600);
    uint32_t textStart = 0, textEnd = 0;
    for (const Section& s : image.sections)
    {
        // .reloc runs past the end of XenonUtils' buffer and is not needed.
        if (s.name == ".reloc")
            continue;
        memcpy(GuestPtr(uint32_t(s.base)), s.data, s.size);
        if (s.name == ".text")
        {
            textStart = uint32_t(s.base);
            textEnd = uint32_t(s.base + s.size);
        }
    }

    // Indirect-call table: host function for every guest code address.
    size_t mapped = 0;
    for (const PPCFuncMapping* m = PPCFuncMappings; m->host; m++, mapped++)
        PPC_LOOKUP_FUNC(g_base, m->guest) = m->host;
    Log("loader: %zu recompiled functions mapped", mapped);
    // The game checks two of its data files against SHA-1 hashes kept in the image (a table of
    // "camera\camerasettings.ini" and "db\gamedb.slt", each with its hash) and refuses changed ones. A mod
    // that replaces one of them (Forza Plus 2's gamedb.slt) gets its entry renamed, so it is never
    // matched: what the Forza Plus 2 executable does to the disc's.
    for (const char* name : { "camera\\camerasettings.ini", "db\\gamedb.slt" })
    {
        std::filesystem::path mod;
        if (!VfsModFile("game", std::string("Media\\") + name, mod))
            continue;
        // Searched for in the image's data sections (their addresses differ between the disc and
        // the title update, so none is written here: the remapper can't place a range's end).
        const size_t len = strlen(name) + 1;
        for (const Section& s : image.sections)
        {
            if (s.name == ".text" || s.name == ".reloc" || s.size <= len)
                continue;
            for (uint32_t a = uint32_t(s.base) + 1; a + len <= uint32_t(s.base + s.size); a++)
            {
                char* p = reinterpret_cast<char*>(g_base + a);
                if (memcmp(p, name, len) == 0 && p[-1] != '\\')
                {
                    p[0] = 'x';
                    Log("loader: %s comes from a mod: its hash check is off", name);
                }
            }
        }
    }
    // Diagnostic: FM2_PROTECT_GUEST=lo-hi (hex) makes that guest range read-only, so whatever writes
    // into it (image data that should never change) faults at the write, with its stack.
    if (const char* e = getenv("FM2_PROTECT_GUEST"))
    {
        uint32_t lo = 0, hi = 0;
        DWORD old;
        if (sscanf(e, "%x-%x", &lo, &hi) == 2 && hi > lo)
            Log("loader: guest %08X-%08X read-only: %s", lo, hi,
                VirtualProtect(g_base + (lo & ~0xFFFu), ((hi - (lo & ~0xFFFu)) + 0xFFF) & ~0xFFFu, PAGE_READONLY, &old) ? "ok" : "failed");
    }

    // XEX header, kept in guest memory for RtlImageXexHeaderField and XAM.
    const uint32_t headerSize = Be32(file.data() + 8);
    g_title.xexHeader = GuestAlloc(Heap::Virtual, headerSize, 0x1000);
    memcpy(GuestPtr(g_title.xexHeader), file.data(), headerSize);

    g_title.entryPoint = uint32_t(image.entry_point);
    g_title.stackSize = OptHeader(file.data(), kXexHeaderStackSize);
    g_title.systemFlags = OptHeader(file.data(), kXexHeaderSystemFlags);
    if (const uint32_t tls = OptHeader(file.data(), kXexHeaderTls))
    {
        g_title.tlsSlotCount = Be32(file.data() + tls);
        g_title.tlsTemplate = Be32(file.data() + tls + 4);
        g_title.tlsDataSize = Be32(file.data() + tls + 8);
    }
    if (const uint32_t exec = OptHeader(file.data(), kXexHeaderExecutionId))
    {
        g_title.executionId = g_title.xexHeader + exec;
        g_title.titleId = Be32(file.data() + exec + 0xC);
    }

    // Module handle: LDR_DATA_TABLE_ENTRY with the fields titles read.
    g_title.ldrEntry = GuestAlloc(Heap::Virtual, 0x100);
    const uint32_t ldr = g_title.ldrEntry;
    for (uint32_t list = 0; list < 0x18; list += 8)
    {
        StoreBE32(ldr + list, ldr + list);
        StoreBE32(ldr + list + 4, ldr + list);
    }
    StoreBE32(ldr + 0x18, uint32_t(image.base));
    StoreBE32(ldr + 0x1C, uint32_t(image.base));
    StoreBE32(ldr + 0x20, image.size);
    StoreBE32(ldr + 0x38, image.size);
    StoreBE32(ldr + 0x3C, g_title.entryPoint);
    StoreBE32(ldr + 0x58, g_title.xexHeader);

    ResolveVariableImports(file.data(), textStart, textEnd);

    Log("loader: %s title %08X entry %08X stack %#x tls %u slots + %#x bytes @%08X", xexPath.string().c_str(),
        g_title.titleId, g_title.entryPoint, g_title.stackSize, g_title.tlsSlotCount, g_title.tlsDataSize, g_title.tlsTemplate);
}
