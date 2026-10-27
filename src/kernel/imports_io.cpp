// File system: guest paths map onto the extracted game folder (read-only)
// and the user folder (cache, content packages).
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <unordered_map>

#include <windows.h>

#include "kernel/imports.h"
#include "kernel/runtime.h"
#include "live_switch.h"
#include "loader.h"
#include "kernel/vfs.h"
#include "user_dir.h"

namespace fs = std::filesystem;

void AudioNoteMusicRead(const std::filesystem::path& file, uint64_t offset, const uint8_t* data, uint32_t bytes); // audio/xma.cpp

namespace
{
struct FileObject : KernelObject
{
    FileObject() : KernelObject(Kind::File) {}
    ~FileObject() override
    {
        if (host != INVALID_HANDLE_VALUE)
            ::CloseHandle(host);
    }

    HANDLE host = INVALID_HANDLE_VALUE;
    fs::path path;
    std::string guestPath;
    bool directory = false;
    bool synchronous = true; // FILE_SYNCHRONOUS_IO_*: otherwise overlapped
    uint64_t position = 0;

    // NtQueryDirectoryFile state.
    std::vector<fs::directory_entry> entries;
    size_t nextEntry = 0;
    bool listed = false;
};

std::mutex g_linkMutex;
std::unordered_map<std::string, std::string> g_links; // lower-case "x:" -> target

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(tolower(c)); });
    return s;
}

std::string StripPrefix(std::string path)
{
    for (const char* prefix : { "\\??\\", "\\\\?\\" })
    {
        if (path.rfind(prefix, 0) == 0)
            path = path.substr(strlen(prefix));
    }
    return path;
}

uint64_t ToFileTime(const FILETIME& ft)
{
    return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

struct HostInfo
{
    uint64_t creation = 0, access = 0, write = 0, size = 0;
    uint32_t attributes = 0;
};

bool QueryHostInfo(const fs::path& path, HostInfo& info)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        return false;
    info.creation = ToFileTime(data.ftCreationTime);
    info.access = ToFileTime(data.ftLastAccessTime);
    info.write = ToFileTime(data.ftLastWriteTime);
    info.size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    info.attributes = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0x10 : 0x80; // DIRECTORY : NORMAL
    if (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
        info.attributes |= 0x01;
    return true;
}

void WriteNetworkOpenInfo(uint32_t out, const HostInfo& h)
{
    StoreBE64(out + 0x00, h.creation);
    StoreBE64(out + 0x08, h.access);
    StoreBE64(out + 0x10, h.write);
    StoreBE64(out + 0x18, h.write);
    StoreBE64(out + 0x20, (h.size + 0xFFF) & ~0xFFFull);
    StoreBE64(out + 0x28, h.size);
    StoreBE32(out + 0x30, h.attributes);
    StoreBE32(out + 0x34, 0);
}

void SetIoStatus(uint32_t iosb, uint32_t status, uint32_t information)
{
    if (!iosb)
        return;
    StoreBE32(iosb, status);
    StoreBE32(iosb + 4, information);
}

std::shared_ptr<FileObject> GetFile(uint32_t handle)
{
    auto object = GetHandle(handle);
    if (!object || object->kind != KernelObject::Kind::File)
        return nullptr;
    return std::static_pointer_cast<FileObject>(object);
}

void SignalEventHandle(uint32_t handle)
{
    if (!handle)
        return;
    if (auto e = GetHandle(handle); e && e->kind == KernelObject::Kind::Event)
    {
        {
            std::lock_guard lock(g_objectMutex);
            static_cast<EventObject&>(*e).signaled = true;
            SyncGuestSignalState(*e);
        }
        SignalObjectsChanged();
    }
}

bool WildcardMatch(const char* pattern, const char* name)
{
    if (!*pattern)
        return !*name;
    if (*pattern == '*')
        return WildcardMatch(pattern + 1, name) || (*name && WildcardMatch(pattern, name + 1));
    if (*pattern == '?' || tolower(*pattern) == tolower(*name))
        return *name && WildcardMatch(pattern + 1, name + 1);
    return false;
}
}

// Path mapping ------------------------------------------------------------------

namespace
{
// The enabled mod folders, in apply order (read once).
const std::vector<fs::path>& ModDirs()
{
    static const std::vector<fs::path> dirs = [] {
        std::vector<fs::path> out;
        std::error_code ec;
        if (getenv("FM2_NO_MODS"))
            return out;
        // DocumentsUserDir, not UserDir: the loader asks before the runtime has set that.
        for (const auto& e : fs::directory_iterator(DocumentsUserDir() / "mods", ec))
        {
            const std::string name = e.path().filename().string();
            if (e.is_directory(ec) && !name.empty() && name[0] != '_' && name[0] != '.')
                out.push_back(e.path());
        }
        std::sort(out.begin(), out.end());
        for (const fs::path& d : out)
            Log("mods: %s", d.filename().string().c_str());
        return out;
    }();
    return dirs;
}
}

bool VfsModFile(const std::string& root, const std::string& rest, fs::path& host)
{
    const auto& dirs = ModDirs();
    std::error_code ec;
    for (auto it = dirs.rbegin(); it != dirs.rend(); ++it)
    {
        const fs::path p = *it / root / fs::path(rest);
        if (fs::exists(p, ec))
        {
            host = p;
            return true;
        }
    }
    return false;
}

bool VfsModsCarryDlc()
{
    static const bool carry = [] {
        std::error_code ec;
        for (const fs::path& d : ModDirs())
            for (const auto& car : fs::directory_iterator(d / "game" / "Media" / "cars", ec))
                if (car.path().filename().string().find("_DLC") != std::string::npos)
                {
                    Log("mods: %s has the DLC cars: downloadable content is hidden", d.filename().string().c_str());
                    return true;
                }
        return false;
    }();
    return carry;
}

void VfsAddLink(const std::string& name, const std::string& target)
{
    std::lock_guard lock(g_linkMutex);
    g_links[Lower(StripPrefix(name))] = StripPrefix(target);
}

void VfsRemoveLink(const std::string& name)
{
    std::lock_guard lock(g_linkMutex);
    g_links.erase(Lower(StripPrefix(name)));
}

bool VfsResolve(std::string guest, fs::path& host, bool* readOnly)
{
    guest = StripPrefix(guest);
    std::replace(guest.begin(), guest.end(), '/', '\\');

    // Symbolic links ("game:" -> "\Device\Cdrom0"), possibly chained.
    for (int depth = 0; depth < 4; depth++)
    {
        const size_t colon = guest.find(':');
        if (colon == std::string::npos)
            break;
        std::lock_guard lock(g_linkMutex);
        auto it = g_links.find(Lower(guest.substr(0, colon + 1)));
        if (it == g_links.end())
            break;
        guest = it->second + "\\" + guest.substr(colon + 1);
    }

    // A bare relative path ("UIFrontEnd.fsb") is relative to the title's
    // working directory, the root of the disc.
    if (guest.find(':') == std::string::npos && (guest.empty() || guest.front() != '\\'))
        guest = "game:\\" + guest;

    std::string rest;
    fs::path root;
    bool ro = false;
    const std::string lower = Lower(guest);
    auto match = [&](const char* prefix, const fs::path& dir, bool isReadOnly) {
        if (lower.rfind(prefix, 0) != 0)
            return false;
        root = dir;
        rest = guest.substr(strlen(prefix));
        ro = isReadOnly;
        return true;
    };
    if (!(match("\\device\\cdrom0", GameDir(), true) || match("game:", GameDir(), true) || match("d:", GameDir(), true) ||
            match("\\device\\harddisk0\\partition1", UserDir() / "hdd", false) ||
            match("\\device\\harddisk0\\partition0", UserDir() / "hdd", false) ||
            match("\\device\\harddisk0\\cache0", UserDir() / "cache", false) ||
            match("\\device\\harddisk0\\cache1", UserDir() / "cache", false) || match("cache:", UserDir() / "cache", false) ||
            match("\\device\\content", ContentDir(), false) || match("\\device\\dlc", DlcDir(), true)
#ifdef FM2_TITLE_UPDATE
            || match("update:", TitleUpdateDir(), true)
#endif
            ))
        return false;

    while (!rest.empty() && rest.front() == '\\')
        rest.erase(rest.begin());
    host = rest.empty() ? root : root / fs::path(rest);
    // A mod's loose file in place of the game's or the update's.
    if (ro && !rest.empty() && (root == GameDir()
#ifdef FM2_TITLE_UPDATE
            || root == TitleUpdateDir()
#endif
            ))
    {
        fs::path mod;
        // Folders the game has stay its own (a mod's folder holds only the mod's files).
        std::error_code ec;
        if (VfsModFile(root == GameDir() ? "game" : "update", rest, mod) && !(fs::is_directory(mod, ec) && fs::exists(host, ec)))
            host = mod;
    }
    // The game's and the update's zips, with built-in changes and mods (zip_overlay.cpp).
    if (ro && rest.size() > 4 && Lower(rest.substr(rest.size() - 4)) == ".zip" && root != DlcDir())
    {
        fs::path ZipOverlay(const std::string& zip, const fs::path& host);
        std::string zip = Lower(rest);
        std::replace(zip.begin(), zip.end(), '\\', '/');
        host = ZipOverlay(zip, host);
    }
    if (readOnly)
        *readOnly = ro;
    return true;
}

// Imports ---------------------------------------------------------------------

GUEST_EXPORT(ObCreateSymbolicLink)
{
    // (name ANSI_STRING*, target ANSI_STRING*)
    const std::string name = GuestAnsiString(ctx.r3.u32), target = GuestAnsiString(ctx.r4.u32);
    Log("vfs: link %s -> %s", name.c_str(), target.c_str());
    VfsAddLink(name, target);
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(ObDeleteSymbolicLink)
{
    VfsRemoveLink(GuestAnsiString(ctx.r3.u32));
    Return(ctx, X_STATUS_SUCCESS);
}

namespace
{
uint32_t OpenFile(uint32_t handleOut, uint32_t access, uint32_t attributes, uint32_t iosb, uint32_t disposition, uint32_t options)
{
    uint32_t root = 0;
    std::string guest = ObjectAttributesName(attributes, &root);
    if (root)
    {
        if (auto parent = GetFile(root))
            guest = parent->guestPath + "\\" + guest;
    }

    fs::path host;
    bool readOnly = false;
    if (!VfsResolve(guest, host, &readOnly))
    {
        Log("vfs: no mapping for \"%s\"", guest.c_str());
        SetIoStatus(iosb, X_STATUS_OBJECT_PATH_NOT_FOUND, 5);
        return X_STATUS_OBJECT_PATH_NOT_FOUND;
    }

    const bool wantDirectory = options & 0x1;   // FILE_DIRECTORY_FILE
    const bool wantFile = options & 0x40;       // FILE_NON_DIRECTORY_FILE
    std::error_code ec;
    const bool exists = fs::exists(host, ec);
    const bool isDirectory = exists && fs::is_directory(host, ec);

    if (exists && ((wantDirectory && !isDirectory) || (wantFile && isDirectory)))
    {
        SetIoStatus(iosb, X_STATUS_ACCESS_DENIED, 0);
        return X_STATUS_ACCESS_DENIED;
    }

    // Dispositions: 0 supersede, 1 open, 2 create, 3 open-if, 4 overwrite, 5 overwrite-if.
    uint32_t information = 1; // FILE_OPENED
    if (!exists)
    {
        if (disposition == 1 || disposition == 4)
        {
            if (getenv("FM2_TRACE_FILES"))
                Log("vfs: not found %s (%s)", guest.c_str(), host.string().c_str());
            SetIoStatus(iosb, X_STATUS_OBJECT_NAME_NOT_FOUND, 5);
            return X_STATUS_OBJECT_NAME_NOT_FOUND;
        }
        if (readOnly)
        {
            SetIoStatus(iosb, X_STATUS_ACCESS_DENIED, 0);
            return X_STATUS_ACCESS_DENIED;
        }
        if (wantDirectory)
            fs::create_directories(host, ec);
        else
            fs::create_directories(host.parent_path(), ec);
        information = 2; // FILE_CREATED
    }
    else if (disposition == 2)
    {
        SetIoStatus(iosb, X_STATUS_OBJECT_NAME_COLLISION, 4);
        return X_STATUS_OBJECT_NAME_COLLISION;
    }

    auto file = std::make_shared<FileObject>();
    file->path = host;
    file->guestPath = guest;
    file->directory = wantDirectory || isDirectory;
    file->synchronous = (options & 0x30) != 0; // FILE_SYNCHRONOUS_IO_ALERT | _NONALERT

    if (!file->directory)
    {
        const bool write = !readOnly && (access & (0x40000000 | 0x2 | 0x4 | 0x10000000)); // GENERIC_WRITE, WRITE_DATA, APPEND_DATA, GENERIC_ALL
        DWORD create = OPEN_EXISTING;
        if (!exists)
            create = CREATE_NEW;
        else if (!readOnly && (disposition == 0 || disposition == 4 || disposition == 5))
        {
            create = TRUNCATE_EXISTING;
            information = disposition == 0 ? 0 : 3;
        }
        file->host = CreateFileW(host.c_str(), GENERIC_READ | (write || create != OPEN_EXISTING ? GENERIC_WRITE : 0),
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, create, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file->host == INVALID_HANDLE_VALUE)
        {
            Log("vfs: CreateFile %s failed (%lu)", host.string().c_str(), GetLastError());
            SetIoStatus(iosb, X_STATUS_ACCESS_DENIED, 0);
            return X_STATUS_ACCESS_DENIED;
        }
    }

    StoreBE32(handleOut, CreateHandle(file));
    static LiveSwitch trace("FM2_TRACE_FILES");
    if (trace)
        Log("vfs: open %s -> %08X", guest.c_str(), file->handle);
    SetIoStatus(iosb, X_STATUS_SUCCESS, information);
    return X_STATUS_SUCCESS;
}
}

GUEST_EXPORT(NtCreateFile)
{
    // (handle*, access, attributes*, iosb*, allocationSize*, fileAttributes, share, disposition, options)
    Return(ctx, OpenFile(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r10.u32, StackArg(ctx, 8)));
}

GUEST_EXPORT(NtOpenFile)
{
    // (handle*, access, attributes*, iosb*, share, options)
    Return(ctx, OpenFile(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 1, ctx.r8.u32));
}

std::atomic<uint64_t> g_readCalls{ 0 }, g_readBytes{ 0 };

void LogFileStats()
{
    Log("  io: %llu reads, %.1f MB", (unsigned long long)g_readCalls.load(), g_readBytes.load() / 1048576.0);
}

GUEST_EXPORT(NtReadFile)
{
    g_readCalls++;
    // (handle, event, apcRoutine, apcContext, iosb*, buffer, length, byteOffset*)
    auto file = GetFile(ctx.r3.u32);
    const uint32_t iosb = ctx.r7.u32, buffer = ctx.r8.u32, length = ctx.r9.u32, offsetPtr = ctx.r10.u32;
    if (!file || file->directory)
    {
        Log("io: NtReadFile on %s handle %08X (length %u -> %08X, thread %u, lr %08llX)", file ? "a directory" : "an invalid", ctx.r3.u32, length, buffer,
            CurrentGuestThreadId(), ctx.lr);
        Return(ctx, X_STATUS_INVALID_HANDLE);
        return;
    }
    if (ctx.r5.u32)
        LOG_ONCE("NtReadFile with an APC routine is not supported");

    uint64_t offset = file->position;
    if (offsetPtr)
    {
        const int64_t requested = int64_t(LoadBE64(offsetPtr));
        if (requested >= 0)
            offset = uint64_t(requested);
    }
    OVERLAPPED ov{};
    ov.Offset = DWORD(offset);
    ov.OffsetHigh = DWORD(offset >> 32);
    DWORD read = 0;
    if (length > (4u << 20))
    {
        std::string CurrentHostStack(int maxFrames);
        Log("io: large read %s offset %llu length %u -> %08X: %s", file->path.filename().string().c_str(), (unsigned long long)offset, length, buffer,
            CurrentHostStack(10).c_str());
    }
    static const char* traceReads = getenv("FM2_TRACE_READS");
    if (traceReads && strstr(file->path.string().c_str(), traceReads))
        Log("io: read %s offset %llu length %u -> %08X (handle %08X, offset ptr %08X, sync %d, thread %u)", file->path.filename().string().c_str(),
            (unsigned long long)offset, length, buffer, ctx.r3.u32, offsetPtr, int(file->synchronous), CurrentGuestThreadId());
    static const uint32_t watchIo = getenv("FM2_WATCH_IO") ? uint32_t(strtoul(getenv("FM2_WATCH_IO"), nullptr, 16)) : 0;
    if (watchIo && (watchIo & 0x1FFFFFFF) - (buffer & 0x1FFFFFFF) < length)
        Log("io: read into watched %08X: %s offset %llu length %u buffer %08X (lr %08llX)", watchIo, file->path.string().c_str(),
            (unsigned long long)offset, length, buffer, ctx.lr);
    if (length == 0x20000 && strstr(file->path.string().c_str(), ".bik"))
    {
        extern uint32_t g_binkRingBase;
        g_binkRingBase = buffer - uint32_t(offset % 0x200000);
    }
    const BOOL ok = ReadFile(file->host, GuestPtr(buffer), length, &read, &ov);
    file->position = offset + read;
    g_readBytes += read;
    // Music files: their packets are noted, so the audio can tell music from effects (xma.cpp).
    if (read && file->guestPath.size() > 4)
    {
        std::string guest = Lower(file->guestPath);
        std::replace(guest.begin(), guest.end(), '\\', '/');
        if (guest.find("/audio/music/") != std::string::npos && guest.compare(guest.size() - 4, 4, ".xma") == 0)
            AudioNoteMusicRead(file->path, offset, GuestPtr<uint8_t>(buffer), read);
    }

    uint32_t status = X_STATUS_SUCCESS;
    if (!ok && GetLastError() != ERROR_HANDLE_EOF)
    {
        Log("io: read failed (error %lu): %s offset %llu length %u buffer %08X", GetLastError(), file->path.string().c_str(),
            (unsigned long long)offset, length, buffer);
        status = X_STATUS_UNSUCCESSFUL;
    }
    else if (read == 0 && length != 0)
        status = X_STATUS_END_OF_FILE;
    SetIoStatus(iosb, status, read);
    SignalEventHandle(ctx.r4.u32);
    // Overlapped handles report the (already finished) read as pending, as
    // on the console: XAPI then returns ERROR_IO_PENDING and the title's
    // streaming code relies on that to advance.
    Return(ctx, !file->synchronous && status == X_STATUS_SUCCESS ? X_STATUS_PENDING : status);
}

GUEST_EXPORT(NtWriteFile)
{
    // (handle, event, apcRoutine, apcContext, iosb*, buffer, length, byteOffset*)
    auto file = GetFile(ctx.r3.u32);
    const uint32_t iosb = ctx.r7.u32, buffer = ctx.r8.u32, length = ctx.r9.u32, offsetPtr = ctx.r10.u32;
    if (!file || file->directory)
    {
        Return(ctx, X_STATUS_INVALID_HANDLE);
        return;
    }
    uint64_t offset = file->position;
    if (offsetPtr)
    {
        const int64_t requested = int64_t(LoadBE64(offsetPtr));
        if (requested >= 0)
            offset = uint64_t(requested);
    }
    OVERLAPPED ov{};
    ov.Offset = DWORD(offset);
    ov.OffsetHigh = DWORD(offset >> 32);
    DWORD written = 0;
    // FM2_TRACE_WRITES=<text>: writes to files whose path contains it, with the writer's stack.
    static const char* traceWrites = getenv("FM2_TRACE_WRITES");
    if (traceWrites && strstr(file->path.string().c_str(), traceWrites))
    {
        std::string CurrentHostStack(int maxFrames);
        Log("io: write %s offset %llu length %u from %08X (thread %u): %s", file->path.filename().string().c_str(), (unsigned long long)offset, length, buffer,
            CurrentGuestThreadId(), CurrentHostStack(8).c_str());
    }
    const BOOL ok = WriteFile(file->host, GuestPtr(buffer), length, &written, &ov);
    file->position = offset + written;
    const uint32_t status = ok ? X_STATUS_SUCCESS : X_STATUS_ACCESS_DENIED;
    SetIoStatus(iosb, status, written);
    SignalEventHandle(ctx.r4.u32);
    Return(ctx, !file->synchronous && status == X_STATUS_SUCCESS ? X_STATUS_PENDING : status);
}

GUEST_EXPORT(NtQueryInformationFile)
{
    // (handle, iosb*, info*, length, class)
    auto file = GetFile(ctx.r3.u32);
    const uint32_t iosb = ctx.r4.u32, info = ctx.r5.u32, cls = ctx.r7.u32;
    if (!file)
    {
        Return(ctx, X_STATUS_INVALID_HANDLE);
        return;
    }
    HostInfo h;
    QueryHostInfo(file->path, h);
    uint32_t written = 0;
    switch (cls)
    {
    case 4: // FileBasicInformation
        StoreBE64(info + 0x00, h.creation);
        StoreBE64(info + 0x08, h.access);
        StoreBE64(info + 0x10, h.write);
        StoreBE64(info + 0x18, h.write);
        StoreBE32(info + 0x20, h.attributes);
        StoreBE32(info + 0x24, 0);
        written = 0x28;
        break;
    case 5: // FileStandardInformation
        StoreBE64(info + 0x00, (h.size + 0xFFF) & ~0xFFFull);
        StoreBE64(info + 0x08, h.size);
        StoreBE32(info + 0x10, 1);
        *GuestPtr<uint8_t>(info + 0x14) = 0;
        *GuestPtr<uint8_t>(info + 0x15) = file->directory;
        written = 0x18;
        break;
    case 14: // FilePositionInformation
        StoreBE64(info, file->position);
        written = 8;
        break;
    case 20: // FileEndOfFileInformation
        StoreBE64(info, h.size);
        written = 8;
        break;
    case 34: // FileNetworkOpenInformation
        WriteNetworkOpenInfo(info, h);
        written = 0x38;
        break;
    case 26: // FileSectorInformation: the disc sector a file starts at. Files here live on the host,
             // not on a disc, so there is no sector to report: the call fails as on Xenia
             // (X_STATUS_INVALID_PARAMETER), and titles fall back to reading the file normally.
        LOG_ONCE("NtQueryInformationFile: no disc sector for %s (FileSectorInformation)", file->path.filename().string().c_str());
        SetIoStatus(iosb, X_STATUS_INVALID_PARAMETER, 0);
        Return(ctx, X_STATUS_INVALID_PARAMETER);
        return;
    default:
        Log("NtQueryInformationFile: class %u not supported (%s, lr %08llX)", cls, file->path.filename().string().c_str(), ctx.lr);
        SetIoStatus(iosb, X_STATUS_NOT_IMPLEMENTED, 0);
        Return(ctx, X_STATUS_NOT_IMPLEMENTED);
        return;
    }
    SetIoStatus(iosb, X_STATUS_SUCCESS, written);
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(NtSetInformationFile)
{
    // (handle, iosb*, info*, length, class)
    auto file = GetFile(ctx.r3.u32);
    const uint32_t iosb = ctx.r4.u32, info = ctx.r5.u32, cls = ctx.r7.u32;
    if (!file)
    {
        Return(ctx, X_STATUS_INVALID_HANDLE);
        return;
    }
    uint32_t status = X_STATUS_SUCCESS;
    switch (cls)
    {
    case 14: // FilePositionInformation
        file->position = LoadBE64(info);
        break;
    case 19: // FileAllocationInformation
    case 20: // FileEndOfFileInformation
    {
        LARGE_INTEGER size;
        size.QuadPart = int64_t(LoadBE64(info));
        if (!SetFileInformationByHandle(file->host, FileEndOfFileInfo, &size, sizeof(size)))
            status = X_STATUS_ACCESS_DENIED;
        break;
    }
    case 4:  // FileBasicInformation: times/attributes, ignored
        break;
    case 10: // FileRenameInformation: { u32 ReplaceIfExists, u32 RootDirectory, ANSI_STRING FileName }
    {
        // Saved photos are written to SavedPhotos:\temp\photo and then renamed into place.
        const bool replace = LoadBE32(info) != 0;
        std::string guest = GuestAnsiString(info + 8);
        if (const uint32_t root = LoadBE32(info + 4))
        {
            if (auto parent = GetFile(root))
                guest = parent->guestPath + "\\" + guest;
        }
        else if (guest.find(':') == std::string::npos && (guest.empty() || guest[0] != '\\'))
        {
            const size_t slash = file->guestPath.find_last_of('\\');
            guest = (slash == std::string::npos ? std::string() : file->guestPath.substr(0, slash + 1)) + guest;
        }
        fs::path target;
        bool readOnly = false;
        static LiveSwitch trace("FM2_TRACE_FILES");
        if (!VfsResolve(guest, target, &readOnly) || readOnly)
        {
            status = X_STATUS_ACCESS_DENIED;
            break;
        }
        std::error_code ec;
        if (fs::exists(target, ec) && !replace)
        {
            status = X_STATUS_OBJECT_NAME_COLLISION;
            break;
        }
        // The open handle would block the move: close it, move, reopen at the new name.
        const bool wasOpen = file->host != INVALID_HANDLE_VALUE;
        if (wasOpen)
            ::CloseHandle(file->host);
        file->host = INVALID_HANDLE_VALUE;
        fs::create_directories(target.parent_path(), ec);
        fs::rename(file->path, target, ec);
        if (ec)
            status = X_STATUS_ACCESS_DENIED;
        else
        {
            file->path = target;
            file->guestPath = guest;
        }
        if (wasOpen && !file->directory)
            file->host = CreateFileW(file->path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (trace)
            Log("vfs: rename -> %s (%s)", guest.c_str(), ec ? ec.message().c_str() : "ok");
        break;
    }
    case 13: // FileDispositionInformation
        if (*GuestPtr<uint8_t>(info))
        {
            ::CloseHandle(file->host);
            file->host = INVALID_HANDLE_VALUE;
            std::error_code ec;
            fs::remove(file->path, ec);
        }
        break;
    default:
        Log("NtSetInformationFile: class %u not supported", cls);
        status = X_STATUS_NOT_IMPLEMENTED;
        break;
    }
    SetIoStatus(iosb, status, 0);
    Return(ctx, status);
}

GUEST_EXPORT(NtQueryFullAttributesFile)
{
    // (attributes*, FILE_NETWORK_OPEN_INFORMATION*)
    uint32_t root = 0;
    std::string guest = ObjectAttributesName(ctx.r3.u32, &root);
    if (root)
    {
        if (auto parent = GetFile(root))
            guest = parent->guestPath + "\\" + guest;
    }
    fs::path host;
    HostInfo h;
    if (!VfsResolve(guest, host, nullptr) || !QueryHostInfo(host, h))
    {
        static LiveSwitch trace("FM2_TRACE_FILES");
        if (trace)
            Log("vfs: attributes not found %s", guest.c_str());
        Return(ctx, X_STATUS_OBJECT_NAME_NOT_FOUND);
        return;
    }
    WriteNetworkOpenInfo(ctx.r4.u32, h);
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(NtQueryDirectoryFile)
{
    // The 360's version has nine arguments: (handle, event, apcRoutine, apcContext, iosb*, info*,
    // length, mask ANSI_STRING*, restartScan). The mask is in r10, the restart flag the first
    // stack argument. (The desktop NT call has two more, which the layout used to assume: it
    // read the mask and the restart flag from stack garbage.)
    auto file = GetFile(ctx.r3.u32);
    const uint32_t iosb = ctx.r7.u32, info = ctx.r8.u32, length = ctx.r9.u32;
    const uint32_t mask = ctx.r10.u32, restart = StackArg(ctx, 8);
    if (!file || !file->directory)
    {
        Return(ctx, X_STATUS_INVALID_HANDLE);
        return;
    }
    if (!file->listed || restart)
    {
        // No mask, or an empty one: every entry (NT's meaning; the title's DLC scan passes "").
        std::string pattern = mask ? GuestAnsiString(mask) : std::string();
        pattern = pattern.c_str(); // (the title's scan passes a string holding only a NUL)
        if (pattern.empty())
            pattern = "*";
        file->entries.clear();
        std::error_code ec;
        for (auto& e : fs::directory_iterator(file->path, ec))
        {
            if (WildcardMatch(pattern.c_str(), e.path().filename().string().c_str()))
                file->entries.push_back(e);
        }
        file->nextEntry = 0;
        file->listed = true;
        static LiveSwitch traceFiles("FM2_TRACE_FILES");
        if (traceFiles)
            Log("vfs: list %s mask '%s' -> %zu entries", file->guestPath.c_str(), pattern.c_str(), file->entries.size());
    }
    if (file->nextEntry >= file->entries.size())
    {
        SetIoStatus(iosb, X_STATUS_NO_MORE_FILES, 0);
        Return(ctx, X_STATUS_NO_MORE_FILES);
        return;
    }

    // FILE_DIRECTORY_INFORMATION with an ANSI name; one entry per call.
    const auto& e = file->entries[file->nextEntry];
    const std::string name = e.path().filename().string();
    const uint32_t size = uint32_t(0x40 + name.size());
    if (size > length)
    {
        SetIoStatus(iosb, X_STATUS_BUFFER_OVERFLOW, 0);
        Return(ctx, X_STATUS_BUFFER_OVERFLOW);
        return;
    }
    file->nextEntry++;
    HostInfo h;
    QueryHostInfo(e.path(), h);
    {
        static LiveSwitch traceFiles("FM2_TRACE_FILES");
        if (traceFiles && file->guestPath.find("dlczips") != std::string::npos)
            Log("vfs:   entry %zu '%s' attributes %08X size %llu", file->nextEntry, name.c_str(), h.attributes, (unsigned long long)h.size);
    }
    StoreBE32(info + 0x00, 0);
    StoreBE32(info + 0x04, uint32_t(file->nextEntry));
    StoreBE64(info + 0x08, h.creation);
    StoreBE64(info + 0x10, h.access);
    StoreBE64(info + 0x18, h.write);
    StoreBE64(info + 0x20, h.write);
    StoreBE64(info + 0x28, h.size);
    StoreBE64(info + 0x30, (h.size + 0xFFF) & ~0xFFFull);
    StoreBE32(info + 0x38, h.attributes);
    StoreBE32(info + 0x3C, uint32_t(name.size()));
    memcpy(GuestPtr(info + 0x40), name.data(), name.size());
    SetIoStatus(iosb, X_STATUS_SUCCESS, size);
    SignalEventHandle(ctx.r4.u32);
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(NtQueryVolumeInformationFile)
{
    // (handle, iosb*, info*, length, class)
    const uint32_t iosb = ctx.r4.u32, info = ctx.r5.u32, cls = ctx.r7.u32;
    uint32_t written = 0;
    switch (cls)
    {
    case 1: // FileFsVolumeInformation
        StoreBE64(info + 0x00, 0);
        StoreBE32(info + 0x08, 0x12345678);
        StoreBE32(info + 0x0C, 0);
        *GuestPtr<uint8_t>(info + 0x10) = 0;
        written = 0x14;
        break;
    case 3: // FileFsSizeInformation: plenty of space
        StoreBE64(info + 0x00, 0x100000);
        StoreBE64(info + 0x08, 0x80000);
        StoreBE32(info + 0x10, 32);
        StoreBE32(info + 0x14, 512);
        written = 0x18;
        break;
    case 5: // FileFsAttributeInformation
        StoreBE32(info + 0x00, 0);
        StoreBE32(info + 0x04, 255);
        StoreBE32(info + 0x08, 4);
        memcpy(GuestPtr(info + 0x0C), "FATX", 4);
        written = 0x10;
        break;
    default:
        Log("NtQueryVolumeInformationFile: class %u not supported", cls);
        SetIoStatus(iosb, X_STATUS_NOT_IMPLEMENTED, 0);
        Return(ctx, X_STATUS_NOT_IMPLEMENTED);
        return;
    }
    SetIoStatus(iosb, X_STATUS_SUCCESS, written);
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(NtFlushBuffersFile)
{
    if (auto file = GetFile(ctx.r3.u32); file && file->host != INVALID_HANDLE_VALUE)
        FlushFileBuffers(file->host);
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(NtDeviceIoControlFile)
{
    LOG_ONCE("NtDeviceIoControlFile(code %08X) not supported", ctx.r6.u32);
    Return(ctx, X_STATUS_NOT_SUPPORTED);
}

GUEST_EXPORT(IoDismountVolumeByFileHandle)
{
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(FscSetCacheElementCount)
{
    Return(ctx, X_STATUS_SUCCESS);
}
