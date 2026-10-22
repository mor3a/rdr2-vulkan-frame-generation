#pragma once
#include <array>
#include <cstdint>

namespace sdk_pacing::cadence
{
// The DLSS-G CPU pacer's interval, from sl.dlss_g 2.14.1:
// - interval = mean of the last six batch-start deltas / outputs per batch (0x3C9F0);
// - output i targets batchStart + i*interval;
// - no output waits more than one interval from "now" (0x49A15-0x49C56).
// The batch start is the SDK's own base for that frame, stamped once output 0's
// present fence completes, so output 0 is never delayed by this estimate. The
// interval therefore follows produced cadence, not this pacer's own output.
// A discontinuity is treated like the pacer's state-change reset (0x4A920):
// a frame gap, a count change, or a delta outside 1-250 ms.
class BatchIntervalEstimator
{
public:
    static constexpr uint32_t kSamples = 6;
    static constexpr int64_t kMinimumDelta = 1000000;
    static constexpr int64_t kMaximumDelta = 250000000;
    struct Decision { int64_t deadline{}, interval{}; uint32_t samples{}; bool capped{}; };

    void Reset() noexcept { *this = {}; }
    // Called once per batch with that frame's base. Repeats of the same batch are ignored.
    void Observe(uint32_t frame, uint32_t outputs, int64_t batchStart) noexcept
    {
        if (outputs < 2 || batchStart <= 0) { Reset(); return; }
        if (started_ && frame == frame_ && outputs == outputs_ && batchStart == start_) return;
        const int64_t delta = batchStart - start_;
        const bool continuous = started_ && frame - frame_ == 1 && outputs == outputs_
            && delta >= kMinimumDelta && delta <= kMaximumDelta;
        if (!continuous) { deltas_ = {}; sum_ = 0; count_ = 0; cursor_ = 0; }
        else
        {
            if (count_ == kSamples) sum_ -= deltas_[cursor_];
            else ++count_;
            deltas_[cursor_] = delta;
            sum_ += delta;
            cursor_ = (cursor_ + 1) % kSamples;
        }
        started_ = true; frame_ = frame; outputs_ = outputs; start_ = batchStart;
    }
    // Index 1..outputs-1: generated outputs, then the real frame last.
    bool Plan(uint32_t frame, uint32_t outputs, uint32_t index, int64_t now, Decision& decision) const noexcept
    {
        if (!started_ || !count_ || frame != frame_ || outputs != outputs_ || index < 1 || index >= outputs || now <= 0)
            return false;
        const int64_t interval = sum_ / int64_t(count_) / int64_t(outputs_);
        if (interval <= 0) return false;
        int64_t deadline = start_ + interval * int64_t(index);
        const bool capped = deadline > now + interval;
        if (capped) deadline = now + interval;
        decision = {deadline, interval, count_, capped};
        return true;
    }
    uint32_t Samples() const noexcept { return count_; }

private:
    std::array<int64_t, kSamples> deltas_{};
    int64_t sum_{}, start_{};
    uint32_t count_{}, cursor_{}, frame_{}, outputs_{};
    bool started_{};
};
}
