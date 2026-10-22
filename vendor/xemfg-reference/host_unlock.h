#pragma once
#include <Windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>

namespace xess_host_unlock
{
// Local integration: distinguish an unsupported untouched runtime from a
// failed transaction whose original bytes/protection could not be verified.
struct RollbackFailure : std::runtime_error {using std::runtime_error::runtime_error;};
// dev.40: the frame-count unlock for OptiScaler's own XeFG output (FGOutput xefg), whose
// contexts RTXMFG leaves untracked. runtime_layout.h discovers the verified SDK 3.x
// instruction families and maps these reference RVAs into the actual loaded pair.
//  - fg 0x1A45C2: the override clamp, mov [rdi+16Ch],1 -> generated;
//  - fg 0x1A517D: the default ceiling, mov ebx,3 -> generated;
//  - fg 0x20973B: the reported maximum, mov eax,1 -> generated;
//  - fg 0x1A5DE4: for more than one generated frame XeFG swaps its 2x model for an MFG model
//    (18 -> 16, absent from this build); the predicate is forced true, so the 2x model stays;
//  - ll 0xD1C9:  xellSetGeneratedFramesCount rejects counts above 3, cmp ebx,3 -> generated.
// Every site is checked (decoded layout, committed execute-read image page owned by the module,
// exact bytes) before any is written; writes restore the protection and are read back.
// beta.7-xell.1: XeLL 1.2.x (SDK 2.1.0/2.1.1, what OptiScaler master pairs its XeFG with when the game
// ships one) has no count gate. Its layout family witnesses the whole setter and binds only its entry
// (0xD1B0); the ll site is left out, that module is not written, and the setter must still match.
inline constexpr uint32_t kLlGate = 0xD1C9;
inline constexpr uint32_t kLlUngatedSetter = 0xD1B0;
struct Site
{
    const char* name;
    bool fg;
    uint32_t rva;
    std::vector<uint8_t> before, after;
};
// dev.46: keep2xModel=false leaves fg_keep_2x_model out, so XeFG remaps its 2x model to the MFG model
// for more than one generated frame (the game route publishes the ported model 16 for it).
std::vector<Site> Sites(uint8_t generated, bool keep2xModel = true, bool llGate = true);

class Unlock
{
public:
    // All sites or none; throws std::runtime_error with the failing site and reason.
    Unlock(HMODULE fg, HMODULE ll, uint8_t generated, bool keep2xModel = true);
    bool Keeps2xModel() const noexcept { return keeps2xModel; }
    bool GatesXeLL() const noexcept { return llGated; }
    ~Unlock() = default; // production keeps it for the process lifetime (contexts may use it)
    Unlock(const Unlock&) = delete;
    Unlock& operator=(const Unlock&) = delete;
    // Writes the original bytes back (tests; never while a context exists).
    void Restore();
    const std::vector<Site>& Applied() const noexcept { return applied; }
private:
    struct Target { const Site* site; uint8_t* address; DWORD protection; };
    std::vector<Site> sites;
    std::vector<Site> applied;
    bool keeps2xModel{true};
    bool llGated{true};
    std::vector<Target> targets;
};
}
