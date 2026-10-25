// Synthetic tests of Common::NceDirectMap (src/yuzu_common/nce_direct_map.cpp): exactness of
// the host mapping chosen for each 16 KiB host page, and cost of the mapping operations.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <random>
#include <setjmp.h>
#include <signal.h>
#include <string>
#include <vector>

#include "yuzu_common/nce_direct_map.h"

using Common::NceDirectMap;
constexpr u64 KiB = 1024, MiB = 1024 * KiB, GiB = 1024 * MiB;
constexpr u32 R = NceDirectMap::PermRead, W = NceDirectMap::PermWrite, X = NceDirectMap::PermExecute;

static int g_failures = 0;
static void Check(bool ok, const std::string & what) {
    std::printf("%s %s\n", ok ? "OK  " : "FAIL", what.c_str());
    g_failures += ok ? 0 : 1;
}

static int HostProt(u64 address) {
    mach_vm_address_t a = address;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &a, &size, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info),
                       &count, &object) != KERN_SUCCESS || a > address) {
        return -1;
    }
    return info.protection;
}

static sigjmp_buf g_jump;
static void OnFault(int, siginfo_t *, void *) { siglongjmp(g_jump, 1); }

// 0 = ok, 1 = faulted.
static int TryRead(u64 address, u32 * out) {
    if (sigsetjmp(g_jump, 1) != 0) {
        return 1;
    }
    *out = *reinterpret_cast<volatile u32 *>(address);
    return 0;
}
static int TryWrite(u64 address, u32 value) {
    if (sigsetjmp(g_jump, 1) != 0) {
        return 1;
    }
    *reinterpret_cast<volatile u32 *>(address) = value;
    return 0;
}

int main() {
    struct sigaction sa{};
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = OnFault;
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGSEGV, &sa, nullptr);

    // Backing: Mach named memory entry, as HostMemory creates it.
    const u64 backing_size = 256 * MiB;
    memory_object_size_t entry_size = backing_size;
    mach_port_t entry = MACH_PORT_NULL;
    if (mach_make_memory_entry_64(mach_task_self(), &entry_size, 0, MAP_MEM_NAMED_CREATE | VM_PROT_ALL, &entry,
                                  MACH_PORT_NULL) != KERN_SUCCESS) {
        std::printf("FAIL entry\n");
        return 1;
    }
    mach_vm_address_t backing_addr = 0;
    mach_vm_map(mach_task_self(), &backing_addr, backing_size, 0, VM_FLAGS_ANYWHERE, entry, 0, FALSE,
                VM_PROT_READ | VM_PROT_WRITE, VM_PROT_ALL, VM_INHERIT_NONE);
    auto * backing = reinterpret_cast<u32 *>(backing_addr);
    for (u64 i = 0; i < backing_size / 4; i += 1024) {
        backing[i] = static_cast<u32>(i * 4); // each 4 KiB page starts with its offset
    }

    NceDirectMap map(entry);
    if (!map.Reserve(8 * GiB)) {
        Check(false, "Réservation des fenêtres");
        return 1;
    }
    Check(true, "Réservation des fenêtres");
    const auto & w = map.Windows();
    Check(w.size() == 2 && w[1].start == 448 * GiB && w[1].end == 512 * GiB && w[0].end <= 63 * GiB,
          "Fenêtres : basse [" + std::to_string(w[0].start / GiB) + ", " + std::to_string(w[0].end / GiB) +
              ") Gio, haute [448, 512) Gio");
    Check(map.Contains(w[0].start, w[0].end - w[0].start) &&
          !map.Contains(w[0].end - 0x1000, 0x2000) &&
          !map.Contains(w[0].start, ~u64{0}) && !map.Contains(0x8000000, 0x1000),
          "D0 bornes exactes, dépassement entier rejeté, adresses 32 bits exclues");
    const u64 base = w[0].start + 64 * MiB;

    // D1: aligned contiguous backing -> direct, same contents, guest perm.
    map.Map(base, 0x100000, 64 * KiB, R | W);
    u32 v = 0;
    bool same = true;
    for (u64 o = 0; o < 64 * KiB; o += 4 * KiB) {
        same &= TryRead(base + o, &v) == 0 && v == 0x100000 + o;
    }
    Check(same && HostProt(base) == (VM_PROT_READ | VM_PROT_WRITE), "D1 support aligné : direct RW, contenu exact");
    Check(TryWrite(base + 0x2000, 0xABCD) == 0 && backing[(0x100000 + 0x2000) / 4] == 0xABCD,
          "D1 écriture visible dans le support");
    backing[(0x100000 + 0x2000) / 4] = 0x100000 + 0x2000;

    // D2: misaligned backing (offset 4 KiB into a host page) -> inaccessible.
    map.Map(base + 1 * MiB, 0x201000, 32 * KiB, R | W);
    Check(HostProt(base + 1 * MiB) == VM_PROT_NONE && TryRead(base + 1 * MiB, &v) == 1,
          "D2 support décalé de 4 Kio : page hôte inaccessible");
    Check(map.AccessAllowed(base + 1 * MiB + 0x1000, true) && map.AccessAllowed(base + 1 * MiB + 0x1000, false),
          "D2 permission invitée connue pour l'émulation");

    // D3: non-contiguous backing inside a host page -> inaccessible.
    map.Map(base + 2 * MiB, 0x300000, 8 * KiB, R | W);
    map.Map(base + 2 * MiB + 8 * KiB, 0x400000 + 8 * KiB, 8 * KiB, R | W);
    Check(HostProt(base + 2 * MiB) == VM_PROT_NONE, "D3 support non contigu : inaccessible");

    // D4: union of permissions, R|RW -> RW; RX|R -> RX; RW|RX -> RX (never W and X).
    map.Map(base + 3 * MiB, 0x500000, 16 * KiB, R);
    map.Protect(base + 3 * MiB + 8 * KiB, 8 * KiB, R | W);
    Check(HostProt(base + 3 * MiB) == (VM_PROT_READ | VM_PROT_WRITE), "D4 R|RW -> RW");
    map.Map(base + 3 * MiB + 16 * KiB, 0x504000, 16 * KiB, R | X);
    map.Protect(base + 3 * MiB + 24 * KiB, 8 * KiB, R);
    Check(HostProt(base + 3 * MiB + 16 * KiB) == (VM_PROT_READ | VM_PROT_EXECUTE), "D4 RX|R -> RX");
    map.Map(base + 3 * MiB + 32 * KiB, 0x508000, 16 * KiB, R | W);
    map.Protect(base + 3 * MiB + 40 * KiB, 8 * KiB, R | X);
    Check(HostProt(base + 3 * MiB + 32 * KiB) == (VM_PROT_READ | VM_PROT_EXECUTE) &&
              map.AccessAllowed(base + 3 * MiB + 32 * KiB, true),
          "D4 RW|RX -> RX, écriture invitée permise donc émulée");

    // D5: partially mapped host page keeps the direct mapping of the mapped pages.
    map.Map(base + 4 * MiB, 0x600000, 8 * KiB, R | W);
    Check(HostProt(base + 4 * MiB) == (VM_PROT_READ | VM_PROT_WRITE) && TryRead(base + 4 * MiB + 4 * KiB, &v) == 0 &&
              v == 0x601000,
          "D5 page hôte à moitié mappée : directe");
    map.Unmap(base + 4 * MiB, 8 * KiB);
    Check(HostProt(base + 4 * MiB) == VM_PROT_NONE && !map.AccessAllowed(base + 4 * MiB, false),
          "D5 démappage : inaccessible");

    // D6: access mask (GPU cache): one guest page denied write -> whole host page read only,
    // guest permission unchanged; mask removed -> RW again. With reads denied too -> none.
    map.SetAccessMask(base + 0x1000, 0x1000, R);
    Check(HostProt(base) == VM_PROT_READ && TryWrite(base + 0x3000, 1) == 1 && map.AccessAllowed(base + 0x3000, true),
          "D6 masque écriture sur 1 page : page hôte R, voisine émulée");
    map.SetAccessMask(base + 0x1000, 0x1000, R | W);
    Check(HostProt(base) == (VM_PROT_READ | VM_PROT_WRITE), "D6 masque levé : RW");
    map.SetAccessMask(base, 0x4000, 0);
    Check(HostProt(base) == VM_PROT_NONE && map.AccessAllowed(base, false), "D6 masque lecture : aucune");
    map.SetAccessMask(base, 0x4000, R | W);
    map.Map(base, 0x100000, 16 * KiB, R | W);

    // D6b: distinguish a real GPU-denied read from a 16 KiB neighbor fault.
    int tracked_mismatches = 0;
    for (unsigned masks = 0; masks < 256; ++masks) {
        for (unsigned page = 0; page < 4; ++page)
            map.SetAccessMask(base + page * 4096, 4096, (masks >> (2 * page)) & 3);
        for (unsigned page = 0; page < 4; ++page) {
            const unsigned allowed = (masks >> (2 * page)) & 3;
            tracked_mismatches += map.TrackedAccessAllowed(base + page * 4096, false) != bool(allowed & R);
            tracked_mismatches += map.TrackedAccessAllowed(base + page * 4096, true) != bool(allowed & W);
        }
    }
    Check(tracked_mismatches == 0, "D6b 256 mixed GPU masks: individual 4 KiB permissions exact");
    map.SetAccessMask(base, 0x4000, R | W);
    map.SetAccessMask(base + 0x1000, 4096, 0);
    Check(map.TrackedAccessAllowed(base, false) && !map.HostAccessAllowed(base, false) &&
              TryRead(base, &v) == 1 && !map.TrackedAccessAllowed(base + 4096, false),
          "D6c actual host read faults on current data solely because its neighbor is GPU-dirty");
    map.SetAccessMask(base, 0x4000, R | W);
    map.Protect(base, 4096, W);
    Check(!map.TrackedAccessAllowed(base, false), "D6d GPU readiness never bypasses guest read denial");
    map.Protect(base, 4096, R | W);

    // D7: remapping a page to another backing updates the contents.
    map.Map(base + 0x4000, 0x700000, 16 * KiB, R | W);
    Check(TryRead(base + 0x4000, &v) == 0 && v == 0x700000, "D7 remappage : nouveau contenu");

    // D8: exhaustive check of the rule on random host pages against a reference model.
    std::mt19937_64 rng(42);
    int mismatches = 0;
    const u64 area = base + 16 * MiB;
    for (int iteration = 0; iteration < 4000; iteration++) {
        const u64 hp = area + (rng() % 256) * 16 * KiB;
        u64 offs[4];
        u32 perms[4];
        bool mapped[4];
        const bool aligned = (rng() & 1) != 0;
        const u64 block = (rng() % 1000) * 16 * KiB;
        for (int i = 0; i < 4; i++) {
            mapped[i] = (rng() % 4) != 0;
            perms[i] = static_cast<u32>(rng() % 8);
            offs[i] = aligned ? block + i * 4 * KiB : (rng() % 4000) * 4 * KiB;
            if (mapped[i]) {
                map.Map(hp + i * 4 * KiB, offs[i], 4 * KiB, perms[i]);
            } else {
                map.Unmap(hp + i * 4 * KiB, 4 * KiB);
            }
        }
        // Reference.
        bool any = false, direct = true;
        u64 b = 0;
        u32 u = 0;
        for (int i = 0; i < 4; i++) {
            if (!mapped[i]) continue;
            const u64 pb = offs[i] - i * 4 * KiB;
            if (!any) b = pb;
            any = true;
            direct &= pb == b && pb % (16 * KiB) == 0;
            u |= perms[i];
        }
        int expect = VM_PROT_NONE;
        if (any && direct) {
            expect = static_cast<int>(u);
            if ((expect & W) && (expect & X)) expect &= ~W;
            if (expect & X) expect |= R; // macOS has no execute-only pages
        }
        const int got = HostProt(hp);
        if (got != expect) {
            if (mismatches++ < 5) std::printf("  page %llx: %d attendu %d\n", (unsigned long long)hp, got, expect);
        }
        // Contents of the direct mapped pages are those of the backing.
        if (any && direct && (expect & R)) {
            for (int i = 0; i < 4; i++) {
                if (mapped[i] && (TryRead(hp + i * 4 * KiB, &v) != 0 || v != offs[i])) mismatches++;
            }
        }
    }
    Check(mismatches == 0, "D8 4000 pages hôtes aléatoires conformes au modèle (" + std::to_string(mismatches) + " écarts)");

    // P1: cost of mapping 1 GiB of heap in 64 KiB calls and of 4 KiB calls (MapPhysicalMemory).
    using clk = std::chrono::steady_clock;
    const u64 big = w[0].start + 2 * GiB;
    auto t0 = clk::now();
    for (u64 o = 0; o < 128 * MiB; o += 64 * KiB) map.Map(big + o, o, 64 * KiB, R | W);
    auto t1 = clk::now();
    for (u64 o = 0; o < 128 * MiB; o += 4 * KiB) map.Map(big + 1 * GiB + o, o, 4 * KiB, R | W);
    auto t2 = clk::now();
    const double us64 = std::chrono::duration<double, std::micro>(t1 - t0).count() / (128 * MiB / (64 * KiB));
    const double us4 = std::chrono::duration<double, std::micro>(t2 - t1).count() / (128 * MiB / (4 * KiB));
    std::printf("INFO P1 Map 64 Kio : %.2f µs/appel ; Map 4 Kio : %.2f µs/appel\n", us64, us4);

    // P2: GPU cache toggles on one 4 KiB page (protect only path) and on 1 MiB.
    t0 = clk::now();
    for (int i = 0; i < 20000; i++) map.SetAccessMask(big + 0x1000, 0x1000, (i & 1) ? (R | W) : R);
    t1 = clk::now();
    for (int i = 0; i < 2000; i++) map.SetAccessMask(big, 1 * MiB, (i & 1) ? (R | W) : R);
    t2 = clk::now();
    std::printf("INFO P2 masque 4 Kio : %.2f µs ; masque 1 Mio : %.2f µs\n",
                std::chrono::duration<double, std::micro>(t1 - t0).count() / 20000,
                std::chrono::duration<double, std::micro>(t2 - t1).count() / 2000);

    // P3: native access speed on a direct mapped page vs the backing view.
    volatile u64 * p = reinterpret_cast<volatile u64 *>(big);
    volatile u64 * q = reinterpret_cast<volatile u64 *>(backing_addr);
    t0 = clk::now();
    for (int r = 0; r < 20; r++) for (u64 i = 0; i < 16 * MiB / 8; i++) p[i] = p[i] + 1;
    t1 = clk::now();
    for (int r = 0; r < 20; r++) for (u64 i = 0; i < 16 * MiB / 8; i++) q[i] = q[i] + 1;
    t2 = clk::now();
    std::printf("INFO P3 balayage 16 Mio x20 : fenêtre %.1f ms, vue support %.1f ms\n",
                std::chrono::duration<double, std::milli>(t1 - t0).count(),
                std::chrono::duration<double, std::milli>(t2 - t1).count());

    std::printf("%s : %d échec(s)\n", g_failures == 0 ? "RÉUSSI" : "ÉCHEC", g_failures);
    return g_failures == 0 ? 0 : 1;
}
