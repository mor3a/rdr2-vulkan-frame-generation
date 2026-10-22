#include "dlssg_provider_policy.h"

#include <winver.h>
#include <cstring>

namespace dlssg_provider_policy
{
bool DecodeVersionResource(const void* bytes, size_t size, VersionTriplet& version) noexcept
{
    version = {};
    constexpr wchar_t key[] = L"VS_VERSION_INFO";
    constexpr size_t valueOffset = (6 + sizeof(key) + 3) & ~size_t(3);
    constexpr size_t required = valueOffset + sizeof(VS_FIXEDFILEINFO);
    if (!bytes || size < required) return false;
    const auto* data = static_cast<const unsigned char*>(bytes);
    uint16_t header[3]{};
    std::memcpy(header, data, sizeof(header));
    if (header[0] < required || header[0] > size ||
        header[1] != sizeof(VS_FIXEDFILEINFO) || header[2] != 0 ||
        std::memcmp(data + 6, key, sizeof(key))) return false;
    VS_FIXEDFILEINFO info{};
    std::memcpy(&info, data + valueOffset, sizeof(info));
    if (info.dwSignature != VS_FFI_SIGNATURE) return false;
    version = {HIWORD(info.dwFileVersionMS), LOWORD(info.dwFileVersionMS),
        HIWORD(info.dwFileVersionLS)};
    return true;
}

bool ReadLoadedProviderVersion(HMODULE module, VersionTriplet& version) noexcept
{
    version = {};
    if (!module) return false;
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(1), RT_VERSION);
    if (!resource) {
        // Resource IDs are not an eligibility rule. Support another ID without
        // trusting a filename or falling back to a file read on the hot path.
        EnumResourceNamesW(module, RT_VERSION,
            [](HMODULE owner, LPCWSTR type, LPWSTR name, LONG_PTR context)->BOOL {
                auto& found = *reinterpret_cast<HRSRC*>(context);
                found = FindResourceW(owner, name, type);
                return found ? FALSE : TRUE;
            }, reinterpret_cast<LONG_PTR>(&resource));
    }
    if (!resource) return false;
    const DWORD size = SizeofResource(module, resource);
    const auto loaded = LoadResource(module, resource);
    return loaded && DecodeVersionResource(LockResource(loaded), size, version);
}

bool ReadProviderVersion(
    const wchar_t* path, VersionTriplet& version) noexcept
{
    version = {};
    if (!path || !*path)
        return false;

    DWORD ignored = 0;
    const DWORD versionBytes = GetFileVersionInfoSizeW(path, &ignored);
    if (!versionBytes)
        return false;

    void* const versionData = VirtualAlloc(nullptr, versionBytes,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!versionData)
        return false;

    VS_FIXEDFILEINFO* fixedInfo = nullptr;
    UINT fixedInfoBytes = 0;
    const bool versionRead = GetFileVersionInfoW(
            path, 0, versionBytes, versionData)
        && VerQueryValueW(versionData, L"\\",
            reinterpret_cast<void**>(&fixedInfo), &fixedInfoBytes)
        && fixedInfo && fixedInfoBytes >= sizeof(VS_FIXEDFILEINFO)
        && fixedInfo->dwSignature == VS_FFI_SIGNATURE;
    if (versionRead)
    {
        version.major = HIWORD(fixedInfo->dwFileVersionMS);
        version.minor = LOWORD(fixedInfo->dwFileVersionMS);
        version.build = HIWORD(fixedInfo->dwFileVersionLS);
    }
    VirtualFree(versionData, 0, MEM_RELEASE);
    return versionRead;
}

bool SupportedProviderVersionMatches(const wchar_t* path) noexcept
{
    VersionTriplet version{};
    return ReadProviderVersion(path, version)
        && IsSupportedVersion(version);
}

bool IsDlssgImplementationModule(HMODULE module) noexcept
{
    return module && HasDlssgExportIdentity(
        GetProcAddress(module, kD3d12ImplementationExport) != nullptr,
        GetProcAddress(module, kDirectSrImplementationExport) != nullptr);
}

bool IsSupportedProvider(HMODULE module, const wchar_t* path) noexcept
{
    // Feature identity is structural. Once that is established, the embedded
    // provider version is the only eligibility input; delivery path, filename,
    // hash, and the versions of unrelated DLSS siblings are irrelevant.
    (void)path;
    VersionTriplet version{};
    return IsDlssgImplementationModule(module)
        && ReadLoadedProviderVersion(module, version)
        && IsSupportedVersion(version);
}
}
