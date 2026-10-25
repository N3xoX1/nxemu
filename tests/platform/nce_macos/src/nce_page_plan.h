// Exact placement of 4 KiB guest pages on 16 KiB Darwin host pages.
//
// The guest address space is identity mapped: guest VA == host VA. One host page
// covers four guest pages. The host page is mapped straight from the backing memory
// only when the four guest pages are present, have the same permission, and are
// backed by one 16 KiB aligned block of backing memory. Any other host page is
// "split": its host permission is the intersection of the guest permissions of its
// four pages (an absent guest page counts as no access), so every access that the
// host refuses but the guest allows traps, and the fault handler performs it from the
// backing memory after checking the guest permission of that 4 KiB page. No access
// the guest forbids is ever allowed by the host.
#pragma once

#include <array>
#include <cstdint>

namespace nce_darwin {

constexpr std::uint64_t GuestPageSize = 0x1000;
constexpr std::uint64_t HostPageSize = 0x4000;
constexpr std::uint32_t GuestPagesPerHostPage = HostPageSize / GuestPageSize;

enum Perm : std::uint8_t {
    PermNone = 0,
    PermR = 1 << 0,
    PermW = 1 << 1,
    PermX = 1 << 2,
};

struct GuestPage {
    bool mapped = false;
    std::uint64_t backing_offset = 0; // offset in the backing memory, guest page aligned
    std::uint8_t perm = PermNone;
};

enum class HostPageKind : std::uint8_t {
    Unmapped, // no guest page present: host page reserved with no access
    Direct,   // exact 1:1 host mapping of the backing memory
    Split,    // host permission is the intersection; the rest is emulated on fault
};

struct HostPagePlan {
    HostPageKind kind = HostPageKind::Unmapped;
    // Split pages whose backing is one aligned 16 KiB block keep it mapped with the
    // intersection permission, so the accesses all four guest pages allow run natively.
    // Other split pages get no host access at all.
    bool backing_mapped = false;
    std::uint64_t backing_offset = 0;
    std::uint8_t host_perm = PermNone;
    // Bit i set: guest page i allows an access the host page refuses (it traps and is
    // emulated). Separate masks per access kind.
    std::uint8_t emulated_read = 0;
    std::uint8_t emulated_write = 0;
    std::uint8_t emulated_exec = 0;
    // Union of the four guest permissions: what a permissive mapping would grant.
    // Only reported, never used to map.
    std::uint8_t union_perm = PermNone;
};

constexpr bool IsAlignedBlock(const std::array<GuestPage, GuestPagesPerHostPage> & g) {
    if (g[0].backing_offset % HostPageSize != 0) {
        return false;
    }
    for (std::uint32_t i = 0; i < GuestPagesPerHostPage; i++) {
        if (!g[i].mapped || g[i].backing_offset != g[0].backing_offset + i * GuestPageSize) {
            return false;
        }
    }
    return true;
}

constexpr HostPagePlan PlanHostPage(const std::array<GuestPage, GuestPagesPerHostPage> & g) {
    HostPagePlan plan{};
    bool any = false;
    bool all = true;
    bool same_perm = true;
    std::uint8_t inter = PermR | PermW | PermX;
    for (std::uint32_t i = 0; i < GuestPagesPerHostPage; i++) {
        const std::uint8_t p = g[i].mapped ? g[i].perm : PermNone;
        any = any || g[i].mapped;
        all = all && g[i].mapped;
        same_perm = same_perm && p == (g[0].mapped ? g[0].perm : PermNone);
        inter &= p;
        plan.union_perm |= p;
    }
    if (!any) {
        return plan;
    }
    const bool aligned = IsAlignedBlock(g);
    if (all && same_perm && aligned) {
        plan.kind = HostPageKind::Direct;
        plan.backing_mapped = true;
        plan.backing_offset = g[0].backing_offset;
        plan.host_perm = g[0].perm;
        return plan;
    }
    plan.kind = HostPageKind::Split;
    // Darwin cannot map a host page from four unrelated backing pieces, nor a
    // partially present one: only a contiguous, aligned block keeps native access.
    plan.backing_mapped = aligned;
    plan.backing_offset = aligned ? g[0].backing_offset : 0;
    plan.host_perm = aligned ? inter : PermNone;
    for (std::uint32_t i = 0; i < GuestPagesPerHostPage; i++) {
        const std::uint8_t p = g[i].mapped ? g[i].perm : PermNone;
        const std::uint8_t extra = p & static_cast<std::uint8_t>(~plan.host_perm);
        if (extra & PermR) {
            plan.emulated_read |= 1u << i;
        }
        if (extra & PermW) {
            plan.emulated_write |= 1u << i;
        }
        if (extra & PermX) {
            plan.emulated_exec |= 1u << i;
        }
    }
    return plan;
}

// Fault on a split page: the access is performed by emulation only if the guest
// permission of the 4 KiB page allows it; otherwise it is a real guest fault.
enum class Access : std::uint8_t { Read, Write, Execute };

constexpr bool GuestAllows(const GuestPage & page, Access access) {
    if (!page.mapped) {
        return false;
    }
    switch (access) {
    case Access::Read: return (page.perm & PermR) != 0;
    case Access::Write: return (page.perm & PermW) != 0;
    case Access::Execute: return (page.perm & PermX) != 0;
    }
    return false;
}

} // namespace nce_darwin
