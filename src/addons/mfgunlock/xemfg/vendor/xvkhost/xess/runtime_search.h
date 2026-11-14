#pragma once
// The folder search for XeMFG standalone's libxess_fg.dll / libxell.dll pair, shared by the D3D12 host
// (xess/d3d12_host.cpp) and the Vulkan host (xess/vulkan/vk_host.cpp), each in its own folder order. File attributes
// and directory listings only: a folder qualifies with both exact names as regular files. Near-miss names
// (libxess_fg.dll.dll, the SDK's .lib files, "(1)" copies, a folder named like the DLL) are listed for the log and the
// menu and never loaded. A failed search says why (runtime_misnamed, runtime_split, runtime_missing_libxell,
// runtime_missing_libxess_fg, runtime_path_too_long, runtime_not_found) and where the pair belongs.
// oldruntime.1: a pair that loads without every export the host calls (a game's own older copy, such as Cyberpunk's
// libxess_fg 1.1.0.19 beside its exe) names the short file, its version and the folder a newer pair takes over from.
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace xess_runtime_search
{
inline constexpr wchar_t kFg[] = L"libxess_fg.dll";
inline constexpr wchar_t kLl[] = L"libxell.dll";
inline constexpr size_t kNearNames = 4; // per folder
// oldruntime.1: the SDK the menu points to for a newer pair (xess_runtime.h kSdk302, README step 1).
inline constexpr char kTestedSdk[] = "3.0.2";

enum class Order { eD3D12, eVulkan };

// The folders of MFG Unlocker's module and of the process EXE. A path of MAX_PATH characters or more is skipped, as
// the hosts' MAX_PATH reads did.
struct Bases
{
    std::filesystem::path module, game;
    bool moduleTooLong{}, gameTooLong{};
};

struct Folder
{
    std::filesystem::path path;
    const wchar_t* base{};   // L"module" or L"game"
    const wchar_t* sub{};    // L"", L"RTXMFG-XeSS" or L"OptiScaler"
    bool fg{}, ll{};
    std::vector<std::wstring> misses;
};

struct Result
{
    std::vector<Folder> folders;     // in search order
    size_t found = SIZE_MAX;         // the first folder holding both
    std::string reason;              // empty when found
    std::wstring misnamed;           // the first near-miss name
    // Where the pair belongs: the RTXMFG-XeSS folder beside MFG Unlocker, and the game's EXE folder.
    std::filesystem::path recommended, gameFolder;
};

// The module's full path (nullptr: the process EXE), empty on failure.
inline std::wstring ModulePath(HMODULE module)
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length < path.size() ? length : 0);
    return path;
}

inline Bases ReadBases(const std::wstring& modulePath, const std::wstring& gamePath)
{
    Bases bases;
    const auto parent = [](const std::wstring& path, bool& tooLong) {
        if (path.empty()) return std::filesystem::path{};
        if (path.size() >= MAX_PATH) { tooLong = true; return std::filesystem::path{}; }
        return std::filesystem::path(path).parent_path();
    };
    bases.module = parent(modulePath, bases.moduleTooLong);
    bases.game = parent(gamePath, bases.gameTooLong);
    return bases;
}

// D3D12: every base's RTXMFG-XeSS folder first (a game's own older copy beside it stays the game's), then the bases,
// then their OptiScaler folders. Vulkan: the module's folder, its RTXMFG-XeSS and OptiScaler folders, then the game's.
// Duplicates (case-insensitive) are searched once.
inline std::vector<Folder> Folders(const Bases& bases, Order order)
{
    std::vector<Folder> folders;
    const auto add = [&](const std::filesystem::path& base, const wchar_t* name, const wchar_t* sub) {
        if (base.empty()) return;
        const std::filesystem::path path = *sub ? base / sub : base;
        for (const auto& known : folders) if (_wcsicmp(known.path.c_str(), path.c_str()) == 0) return;
        Folder folder;
        folder.path = path;
        folder.base = name;
        folder.sub = sub;
        folders.push_back(std::move(folder));
    };
    const wchar_t* d3d12[] = {L"RTXMFG-XeSS", L"", L"OptiScaler"};
    const wchar_t* vulkan[] = {L"", L"RTXMFG-XeSS", L"OptiScaler"};
    if (order == Order::eD3D12)
        for (const wchar_t* sub : d3d12) { add(bases.module, L"module", sub); add(bases.game, L"game", sub); }
    else
    {
        for (const wchar_t* sub : vulkan) add(bases.module, L"module", sub);
        for (const wchar_t* sub : vulkan) add(bases.game, L"game", sub);
    }
    return folders;
}

inline bool RegularFile(const std::filesystem::path& path)
{
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

// Names in `folder` that start like the pair's but are not its exact regular files (listing only).
inline std::vector<std::wstring> NearMisses(const std::filesystem::path& folder, bool fg, bool ll)
{
    std::vector<std::wstring> names;
    for (const wchar_t* prefix : {L"libxess_fg", L"libxell"})
    {
        WIN32_FIND_DATAW data{};
        const std::wstring pattern = (folder / (std::wstring(prefix) + L"*")).wstring();
        const HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
        if (find == INVALID_HANDLE_VALUE) continue;
        do
        {
            if ((fg && !_wcsicmp(data.cFileName, kFg)) || (ll && !_wcsicmp(data.cFileName, kLl))) continue;
            if (names.size() < kNearNames) names.push_back(data.cFileName);
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    return names;
}

inline Result Find(const Bases& bases, Order order)
{
    Result result;
    result.folders = Folders(bases, order);
    const std::filesystem::path& home = !bases.module.empty() ? bases.module : bases.game;
    if (!home.empty()) result.recommended = home / L"RTXMFG-XeSS";
    result.gameFolder = bases.game;
    bool anyFg = false, anyLl = false;
    for (size_t index = 0; index < result.folders.size(); ++index)
    {
        Folder& folder = result.folders[index];
        folder.fg = RegularFile(folder.path / kFg);
        folder.ll = RegularFile(folder.path / kLl);
        folder.misses = NearMisses(folder.path, folder.fg, folder.ll);
        anyFg = anyFg || folder.fg;
        anyLl = anyLl || folder.ll;
        if (result.misnamed.empty() && !folder.misses.empty()) result.misnamed = folder.misses.front();
        if (folder.fg && folder.ll && result.found == SIZE_MAX) result.found = index;
    }
    if (result.found != SIZE_MAX) return result;
    if (!result.misnamed.empty()) result.reason = "runtime_misnamed";
    else if (anyFg && anyLl) result.reason = "runtime_split";
    else if (anyFg) result.reason = "runtime_missing_libxell";
    else if (anyLl) result.reason = "runtime_missing_libxess_fg";
    else if (bases.module.empty() && bases.game.empty() && (bases.moduleTooLong || bases.gameTooLong))
        result.reason = "runtime_path_too_long";
    else result.reason = "runtime_not_found";
    return result;
}

// A reason of a failed search (the menu's note with the paths).
inline bool SearchReason(const std::string& reason)
{
    return reason == "runtime_not_found" || reason == "runtime_misnamed" || reason == "runtime_split"
        || reason == "runtime_missing_libxell" || reason == "runtime_missing_libxess_fg" || reason == "runtime_path_too_long";
}

// The name a near miss stands for.
inline const wchar_t* Wanted(const std::wstring& name)
{
    return _wcsnicmp(name.c_str(), L"libxess_fg", 10) == 0 ? kFg : kLl;
}

// oldruntime.1: a loaded pair short of exports the host calls: the first file short of one (libxess_fg.dll resolves
// first), how many are missing and the first of them, and that file's version.
struct Outdated
{
    const wchar_t* file{};   // kFg or kLl; nullptr while every export resolved
    const char* first{};
    size_t missing = 0;
    std::string version;     // "1.1.0.19"; empty without a version resource
    void Miss(const wchar_t* name, const char* exportName)
    {
        if (!file) { file = name; first = exportName; }
        ++missing;
    }
};

// A loaded module's FileVersion from its own version resource (no version.dll), empty without one. nullptr is never
// read as the process EXE.
inline std::string ModuleVersion(HMODULE module)
{
    if (!module) return {};
    const HRSRC info = FindResourceW(module, MAKEINTRESOURCEW(VS_VERSION_INFO), RT_VERSION);
    const HGLOBAL resource = info ? LoadResource(module, info) : nullptr;
    const auto* data = resource ? static_cast<const BYTE*>(LockResource(resource)) : nullptr;
    const DWORD size = data ? SizeofResource(module, info) : 0;
    for (DWORD at = 0; at + sizeof(VS_FIXEDFILEINFO) <= size; at += 4)
    {
        VS_FIXEDFILEINFO fixed{};
        memcpy(&fixed, data + at, sizeof(fixed));
        if (fixed.dwSignature != VS_FFI_SIGNATURE) continue;
        return std::to_string(HIWORD(fixed.dwFileVersionMS)) + "." + std::to_string(LOWORD(fixed.dwFileVersionMS)) + "." +
            std::to_string(HIWORD(fixed.dwFileVersionLS)) + "." + std::to_string(LOWORD(fixed.dwFileVersionLS));
    }
    return {};
}

// Where a newer pair takes over from the one found: the RTXMFG-XeSS folder beside MFG Unlocker when the search reaches
// it first (always on D3D12), else the found folder itself (its files replaced; Vulkan searches the module's folder
// first). Empty when nothing was found.
inline std::filesystem::path NewerPairFolder(const Result& result)
{
    if (result.found >= result.folders.size()) return {};
    for (size_t index = 0; index < result.found; ++index)
        if (_wcsicmp(result.folders[index].path.c_str(), result.recommended.c_str()) == 0) return result.recommended;
    return result.folders[result.found].path;
}

inline std::string Utf8(const std::wstring& text)
{
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string out(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count, nullptr, nullptr);
    return out;
}

inline std::string JsonText(const std::wstring& text)
{
    std::string out;
    for (const char c : Utf8(text))
    {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
    }
    return out;
}

// The d3d12_runtime / vk_runtime log fields: both bases once ("same" when the game's folder is the module's), then
// each folder by base and subfolder (a queued log line holds 752 characters), with its files and near misses.
inline std::string Json(const Bases& bases, const Result& result)
{
    const bool same = !bases.game.empty() && _wcsicmp(bases.module.c_str(), bases.game.c_str()) == 0;
    std::string json = "\"module\":\"" + JsonText(bases.module.wstring()) + "\",\"game\":\"" +
        (same ? std::string("same") : JsonText(bases.game.wstring())) + "\"";
    if (bases.moduleTooLong || bases.gameTooLong)
        json += std::string(",\"too_long\":\"") + (bases.moduleTooLong ? "module" : "") +
            (bases.moduleTooLong && bases.gameTooLong ? "+" : "") + (bases.gameTooLong ? "game" : "") + "\"";
    json += ",\"searched\":[";
    for (size_t index = 0; index < result.folders.size(); ++index)
    {
        const Folder& folder = result.folders[index];
        std::wstring label = folder.base;
        if (*folder.sub) label += std::wstring(L"/") + folder.sub;
        json += std::string(index ? "," : "") + "{\"folder\":\"" + JsonText(label) + "\",\"fg\":" + (folder.fg ? "true" : "false") +
            ",\"ll\":" + (folder.ll ? "true" : "false");
        if (!folder.misses.empty())
        {
            json += ",\"near\":[";
            for (size_t name = 0; name < folder.misses.size(); ++name)
                json += std::string(name ? "," : "") + "\"" + JsonText(folder.misses[name]) + "\"";
            json += "]";
        }
        json += "}";
    }
    return json + "]";
}

// oldruntime.1: the d3d12_runtime / vk_runtime fields of a pair short of exports (before "searched", so a long line
// keeps them), empty for a complete pair.
inline std::string OutdatedJson(const Outdated& outdated)
{
    if (!outdated.file) return {};
    return ",\"outdated\":\"" + JsonText(outdated.file) + "\",\"outdated_version\":\"" + outdated.version +
        "\",\"missing\":" + std::to_string(outdated.missing) + ",\"first_missing\":\"" + (outdated.first ? outdated.first : "") + "\"";
}
}
