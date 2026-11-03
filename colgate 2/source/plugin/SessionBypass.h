// Colgate by Hiccup Audio
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Shift-clicking Bypass on one instance bypasses all of them. Every instance
// loaded from the same binary shares this flag, which also means an AU instance
// won't bypass a VST3 instance, since those are separate binaries.
#pragma once

#include <atomic>

namespace colgate
{

class SessionBypass
{
public:
    static bool isActive() noexcept { return flag().load (std::memory_order_relaxed); }
    static void set (bool active) noexcept { flag().store (active, std::memory_order_relaxed); }

private:
    static std::atomic<bool>& flag() noexcept
    {
        static std::atomic<bool> value { false };
        return value;
    }
};

} // namespace colgate
