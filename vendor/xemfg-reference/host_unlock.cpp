#include "host_unlock.h"
#include "runtime_layout.h"
#include <cstring>
#include <stdexcept>

namespace xess_host_unlock
{
namespace
{
void Write(uint8_t* address, const std::vector<uint8_t>& from, const std::vector<uint8_t>& to, DWORD protection,
    const char* name)
{
    if (memcmp(address, from.data(), from.size()) != 0) throw std::runtime_error(std::string(name)+"_unexpected_bytes");
    DWORD prior{};
    if (!VirtualProtect(address, to.size(), PAGE_EXECUTE_READWRITE, &prior))
        throw std::runtime_error(std::string(name)+"_make_writable");
    memcpy(address, to.data(), to.size());
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), address, to.size()) != FALSE;
    DWORD temporary{};
    const bool restored = VirtualProtect(address, to.size(), protection, &temporary) != FALSE;
    MEMORY_BASIC_INFORMATION now{};
    const bool queried = VirtualQuery(address, &now, sizeof(now)) == sizeof(now);
    if (!flushed || !restored || !queried || now.Protect != protection || memcmp(address, to.data(), to.size()) != 0)
        throw std::runtime_error(std::string(name)+"_write_verification");
}
}

std::vector<Site> Sites(uint8_t n, bool keep2xModel, bool llGate)
{
    std::vector<Site> all{
        {"fg_override_clamp", true, 0x1A45C2, {0xC7,0x87,0x6C,0x01,0,0,0x01,0,0,0}, {0xC7,0x87,0x6C,0x01,0,0,n,0,0,0}},
        {"fg_default_ceiling", true, 0x1A517D, {0xBB,0x03,0,0,0}, {0xBB,n,0,0,0}},
        {"fg_reported_maximum", true, 0x20973B, {0xB8,0x01,0,0,0}, {0xB8,n,0,0,0}},
        {"fg_keep_2x_model", true, 0x1A5DE4, {0x74,0x09}, {0xEB,0x06}},
        {"ll_generated_frames_gate", false, kLlGate, {0x83,0xFB,0x03,0x76,0x07}, {0x83,0xFB,n,0x76,0x07}},
    };
    if (!keep2xModel)
        all.erase(std::remove_if(all.begin(),all.end(),[](const Site& site) { return std::string(site.name) == "fg_keep_2x_model"; }),all.end());
    if (!llGate) all.erase(std::remove_if(all.begin(),all.end(),[](const Site& site) { return !site.fg; }),all.end());
    return all;
}

Unlock::Unlock(HMODULE fg, HMODULE ll, uint8_t generated, bool keep2xModel) : keeps2xModel(keep2xModel)
{
    if (generated < 2 || generated > 5) throw std::runtime_error("generated_count_range");
    xess_layout::Get(fg);
    const auto& llLayout = xess_layout::Get(ll, false);
    llGated = (llLayout.addresses.find(kLlGate)!=llLayout.addresses.end());
    if (!llGated)
    {
        if (!(llLayout.addresses.find(kLlUngatedSetter)!=llLayout.addresses.end())) throw std::runtime_error("ll_count_gate_unknown");
        // The witnessed ungated setter is still the one in the image (no gate added since discovery).
        llLayout.VerifyFunction(ll, kLlUngatedSetter);
    }
    sites = Sites(generated, keep2xModel, llGated);
    for (auto& site : sites) site.rva = xess_layout::Rva(site.fg ? fg : ll, site.rva, site.fg);
    for (const auto& site : sites)
    {
        auto* address = reinterpret_cast<uint8_t*>(site.fg ? fg : ll)+site.rva;
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info) || info.AllocationBase != (site.fg ? fg : ll)
            || info.State != MEM_COMMIT || info.Type != MEM_IMAGE || info.Protect != PAGE_EXECUTE_READ
            || reinterpret_cast<uintptr_t>(address)+site.before.size()
                > reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize)
            throw std::runtime_error(std::string(site.name)+"_ownership");
        if (memcmp(address, site.before.data(), site.before.size()) != 0)
            throw std::runtime_error(std::string(site.name)+"_unexpected_bytes");
        targets.push_back({&site, address, info.Protect});
    }
    applied.reserve(targets.size());
    size_t next = 0;
    try
    {
        for (; next < targets.size(); ++next)
        {
            const auto& target = targets[next];
            Write(target.address, target.site->before, target.site->after, target.protection, target.site->name);
            applied.push_back(*target.site);
        }
    }
    catch (...)
    {
        // A site whose write failed verification after the bytes changed is put back too.
        if (next < targets.size())
        {
            const auto& failed = targets[next];
            if (memcmp(failed.address, failed.site->after.data(), failed.site->after.size()) == 0)
                try { Write(failed.address, failed.site->after, failed.site->before, failed.protection, failed.site->name); }
                catch (...) {}
        }
        try { Restore(); } catch (...) {}
        // A known unsupported layout can retain 2x. A failed write transaction
        // must never be treated as an untouched native runtime.
        for(const auto& target:targets){
            MEMORY_BASIC_INFORMATION info{};
            if(VirtualQuery(target.address,&info,sizeof(info))!=sizeof(info) ||
               info.Protect!=target.protection ||
               memcmp(target.address,target.site->before.data(),target.site->before.size())!=0)
                throw RollbackFailure("XeSS host unlock rollback could not be verified");
        }
        throw;
    }
}

void Unlock::Restore()
{
    for (size_t i = applied.size(); i-- > 0;)
    {
        const auto& target = targets[i];
        Write(target.address, target.site->after, target.site->before, target.protection, target.site->name);
    }
    applied.clear();
}
}
