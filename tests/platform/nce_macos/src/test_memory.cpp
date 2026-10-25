// Step 2: memory. Direct (identity) mappings, aliases and R/W/X permissions with 4 KiB
// guest pages on 16 KiB Darwin pages, with no silent approximation of protections.
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <oaknut/code_block.hpp>
#include <oaknut/oaknut.hpp>
#include <pthread.h>
#include <random>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "nce_page_plan.h"
#include "nce_runtime.h"
#include "probes.h"
#include "report.h"
#include "suite.h"

#include "nce/patcher.h"

using namespace oaknut::util;
using namespace nce_darwin;

namespace ncev {

namespace {

constexpr std::uint64_t KiB = 1024;
constexpr std::uint64_t MiB = 1024 * KiB;
constexpr std::uint64_t GiB = 1024 * MiB;

enum class BackingKind { Shm, MachEntry };

struct Backing {
    BackingKind kind = BackingKind::Shm;
    int fd = -1;
    mach_port_t entry = MACH_PORT_NULL;
    std::uint8_t * base = nullptr; // host view of the whole backing, read+write
    std::uint64_t size = 0;
    bool ok() const { return base != nullptr && base != MAP_FAILED; }
};

const char * BackingName(BackingKind k) {
    return k == BackingKind::Shm ? "shm_open (support actuel de HostMemory)" : "entrée mémoire Mach nommée";
}

Backing CreateBacking(std::uint64_t size, BackingKind kind = BackingKind::MachEntry) {
    Backing b;
    b.kind = kind;
    b.size = size;
    if (kind == BackingKind::Shm) {
        // Same backing object as HostMemory on Apple: an unlinked POSIX shared memory object.
        char name[64];
        std::snprintf(name, sizeof(name), "/ncev-%08x%08x", arc4random(), arc4random());
        b.fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (b.fd < 0) {
            return b;
        }
        shm_unlink(name);
        if (ftruncate(b.fd, static_cast<off_t>(size)) != 0) {
            return b;
        }
        b.base = static_cast<std::uint8_t *>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, b.fd, 0));
        return b;
    }
    memory_object_size_t entry_size = size;
    if (mach_make_memory_entry_64(mach_task_self(), &entry_size, 0,
                                  MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE, &b.entry,
                                  MACH_PORT_NULL) != KERN_SUCCESS) {
        return b;
    }
    mach_vm_address_t host = 0;
    if (mach_vm_map(mach_task_self(), &host, size, 0, VM_FLAGS_ANYWHERE, b.entry, 0, FALSE, VM_PROT_READ | VM_PROT_WRITE,
                    VM_PROT_ALL, VM_INHERIT_NONE) == KERN_SUCCESS) {
        b.base = reinterpret_cast<std::uint8_t *>(host);
    }
    return b;
}

void DestroyBacking(Backing & b) {
    if (b.ok()) {
        munmap(b.base, b.size);
    }
    if (b.fd >= 0) {
        close(b.fd);
    }
    if (b.entry != MACH_PORT_NULL) {
        mach_port_deallocate(mach_task_self(), b.entry);
    }
    b = Backing{};
}

// Maps [offset, offset + size) of the backing at guest address va (replacing the
// window reservation). err receives errno or the kern_return_t.
bool MapBacking(const Backing & b, std::uint64_t va, std::uint64_t offset, std::uint64_t size, int prot, int * err = nullptr) {
    if (b.kind == BackingKind::Shm) {
        void * p = mmap(reinterpret_cast<void *>(va), size, prot, MAP_SHARED | MAP_FIXED, b.fd, static_cast<off_t>(offset));
        if (err != nullptr) {
            *err = p == MAP_FAILED ? errno : 0;
        }
        return p != MAP_FAILED && reinterpret_cast<std::uint64_t>(p) == va;
    }
    mach_vm_address_t address = va;
    vm_prot_t vm_prot = (prot & PROT_READ ? VM_PROT_READ : 0) | (prot & PROT_WRITE ? VM_PROT_WRITE : 0) |
                        (prot & PROT_EXEC ? VM_PROT_EXECUTE : 0);
    const kern_return_t kr = mach_vm_map(mach_task_self(), &address, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, b.entry,
                                         offset, FALSE, vm_prot, VM_PROT_ALL, VM_INHERIT_NONE);
    if (err != nullptr) {
        *err = kr;
    }
    return kr == KERN_SUCCESS && address == va;
}

std::string MapErrorText(const Backing & b, int err) {
    return b.kind == BackingKind::Shm ? std::string(strerror(err)) : Fmt("kern_return %d", err);
}

int RegionProt(std::uint64_t addr, std::uint64_t * region_start = nullptr, std::uint64_t * region_size = nullptr) {
    mach_vm_address_t a = addr;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &a, &size, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info),
                       &count, &object) != KERN_SUCCESS) {
        return -1;
    }
    if (region_start != nullptr) {
        *region_start = a;
    }
    if (region_size != nullptr) {
        *region_size = size;
    }
    return a <= addr ? info.protection : -1;
}

std::string ProtText(int prot) {
    if (prot < 0) {
        return "non mappé";
    }
    std::string s;
    s += (prot & VM_PROT_READ) ? 'r' : '-';
    s += (prot & VM_PROT_WRITE) ? 'w' : '-';
    s += (prot & VM_PROT_EXECUTE) ? 'x' : '-';
    return s;
}

std::string FaultText(const ProbeFault & f) {
    const Esr esr{f.esr};
    return Fmt("%s si_code=%d si_addr=%#llx far=%#llx esr=%#x (EC=%#x FSC=%#x WnR=%d)", SignalName(f.sig), f.si_code,
               (unsigned long long)f.si_addr, (unsigned long long)f.far, f.esr, esr.Ec(), esr.Fsc(), esr.WnR() ? 1 : 0);
}

const char * TagName(std::uint32_t tag) {
    switch (tag) {
    case 0: return "anonyme/fichier";
    case VM_MEMORY_MALLOC: return "malloc";
    case VM_MEMORY_MALLOC_SMALL: return "malloc_small";
    case VM_MEMORY_MALLOC_LARGE: return "malloc_large";
    case VM_MEMORY_MALLOC_TINY: return "malloc_tiny";
    case VM_MEMORY_MALLOC_NANO: return "malloc_nano";
    case VM_MEMORY_STACK: return "pile";
    case VM_MEMORY_DYLD: return "dyld";
    case VM_MEMORY_SHARED_PMAP: return "shared_pmap";
    case VM_MEMORY_UNSHARED_PMAP: return "unshared_pmap";
    case VM_MEMORY_IOKIT: return "iokit";
    case VM_MEMORY_GUARD: return "garde";
    default: return "autre";
    }
}

void TestPageSizes() {
    Note("M2.1", "Tailles de page",
         Fmt("hôte : sysconf=%ld, vm_page_size=%lu, vm_kernel_page_size=%lu ; invitée Horizon : 4 Kio", sysconf(_SC_PAGESIZE),
             (unsigned long)vm_page_size, (unsigned long)vm_kernel_page_size));

    int status = 0;
    int spawn_error = 0;
    const std::string plain = RunChild(g_startup.self_path, {"--child", "page_size"}, status);
    // 0x1000 is the private posix_spawn flag that XNU uses for 4 KiB user maps
    // (Rosetta). A native arm64 executable is refused with EBADMACHO.
    const std::string forced = RunChild(g_startup.self_path, {"--child", "page_size"}, status, 0x1000, &spawn_error);
    const bool four_k = plain.find("page=4096") != std::string::npos || forced.find("page=4096") != std::string::npos;
    Expect("M2.2", four_k, Class::Change, "Pages hôte de 4 Kio disponibles pour un processus arm64 natif",
           Fmt("processus fils : %s ; avec l'attribut privé 4K (0x1000) : %s",
               plain.empty() ? "?" : plain.substr(0, plain.find('\n')).c_str(),
               spawn_error ? Fmt("refusé (%s)", strerror(spawn_error)).c_str() : forced.substr(0, forced.find('\n')).c_str()),
           "Mapping invité 4 Kio impossible tel quel : appliquer le plan exact 16 Kio (pages directes + pages scindées "
           "dont les accès refusés par l'hôte sont émulés, M2.21-M2.25)");
}

void TestPageZero() {
    int status = 0;
    int spawn_error = 0;
    const std::string out = RunChild(g_startup.probe_dir + "/pagezero_probe", {}, status, 0, &spawn_error);
    const bool ran = spawn_error == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 && out.find("running") != std::string::npos;
    void * low = mmap(reinterpret_cast<void *>(0x8000000), 0x4000, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
    const int low_err = low == MAP_FAILED ? errno : 0;
    if (low != MAP_FAILED) {
        munmap(low, 0x4000);
    }
    std::string why = spawn_error != 0 ? Fmt("exec refusé (%s)", strerror(spawn_error))
                      : WIFSIGNALED(status) ? Fmt("tué par le signal %d", WTERMSIG(status))
                                            : Fmt("statut %d", status);
    Expect("M2.3", ran || low_err == 0, Class::Change, "Adresses invitées sous 4 Gio mappables en identité",
           Fmt("binaire avec __PAGEZERO de 16 Kio : %s ; mmap fixe à 128 Mio dans ce processus : %s",
               ran ? out.c_str() : why.c_str(), low_err ? strerror(low_err) : "ok"),
           "Processus 32 et 36 bits -> Dynarmic, décidé avant le lancement");
}

std::string GiBText(std::uint64_t v) {
    return Fmt("%.1f Gio", double(v) / double(GiB));
}

void TestHostMap() {
    std::string text;
    std::uint64_t run_start = 0;
    std::uint64_t run_end = 0;
    std::uint32_t run_tag = 0;
    int run_prot = 0;
    auto flush = [&] {
        if (run_end > run_start) {
            text += Fmt("[%#llx-%#llx %s%s] ", (unsigned long long)run_start, (unsigned long long)run_end, TagName(run_tag),
                        run_prot == 0 ? " réservé" : "");
        }
    };
    for (const HostRegion & r : g_startup.regions_before) {
        // Coalesce neighbours closer than 256 MiB to keep the map readable.
        if (run_end != 0 && r.start <= run_end + 256 * MiB && (r.prot == 0) == (run_prot == 0)) {
            run_end = r.end;
            continue;
        }
        flush();
        run_start = r.start;
        run_end = r.end;
        run_tag = r.tag;
        run_prot = r.prot;
    }
    flush();
    Note("M2.4", "Occupation hôte sous 512 Gio avant main() (réservé = protection max nulle)", text);

    void * carve = mmap(reinterpret_cast<void *>(0x20'0000'0000ULL), 0x4000, PROT_READ | PROT_WRITE,
                        MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
    const int carve_err = carve == MAP_FAILED ? errno : 0;
    mach_vm_address_t a = 0x20'0000'0000ULL;
    const kern_return_t overwrite = mach_vm_map(mach_task_self(), &a, 0x4000, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                                                MACH_PORT_NULL, 0, FALSE, VM_PROT_DEFAULT, VM_PROT_ALL, VM_INHERIT_NONE);
    Expect("M2.5", carve != MAP_FAILED, Class::Change, "Plage [63 Gio, 448 Gio) utilisable pour les adresses invitées",
           Fmt("réservation permanente du noyau : mmap MAP_FIXED -> %s, mach_vm_map OVERWRITE -> %d",
               carve_err ? strerror(carve_err) : "ok", overwrite),
           "Disposition 39 bits adaptée : région ASLR invitée = [bas, 512 Gio) avec [63 Gio, 448 Gio) marqué "
           "inaccessible dans KPageTable ; régions code/heap/alias/pile/kernel-map placées dans les fenêtres réservées");

    std::uint64_t low_total = 0, low_largest = 0;
    std::string pieces;
    for (const auto & w : g_startup.low_windows) {
        low_total += w.second - w.first;
        low_largest = std::max(low_largest, w.second - w.first);
        pieces += Fmt("[%#llx-%#llx] ", (unsigned long long)w.first, (unsigned long long)w.second);
    }
    Expect("M2.6", g_startup.reserved, Class::Blocker, "Réservation identité des fenêtres invitées au démarrage",
           Fmt("haute [448 Gio, 512 Gio) : %s (kr=%d) ; trous bas réservés : %s= %s (plus grand %s)",
               g_startup.reserved ? "ok" : "ÉCHEC", g_startup.reserve_kr, pieces.c_str(), GiBText(low_total).c_str(),
               GiBText(low_largest).c_str()));

    // Horizon 39-bit region sizes (k_address_space_info.cpp) against the windows.
    struct Need {
        const char * name;
        std::uint64_t size;
    };
    const Need needs[] = {{"alias", 64 * GiB}, {"kernel-map", 64 * GiB}, {"heap", 8 * GiB}, {"pile", 2 * GiB}, {"code", 4 * GiB}};
    std::vector<std::uint64_t> free_space;
    for (const auto & w : g_startup.low_windows) {
        free_space.push_back(w.second - w.first);
    }
    free_space.push_back(HighWindowHi - HighWindowLo);
    std::string placed, missing;
    for (const Need & n : needs) {
        auto best = free_space.end();
        for (auto it = free_space.begin(); it != free_space.end(); ++it) {
            if (*it >= n.size && (best == free_space.end() || *it < *best)) {
                best = it;
            }
        }
        if (best == free_space.end()) {
            missing += Fmt("%s (%s) ", n.name, GiBText(n.size).c_str());
        } else {
            *best -= n.size;
            placed += Fmt("%s ", n.name);
        }
    }
    Expect("M2.7", missing.empty(), Class::Change, "Régions Horizon 39 bits de taille d'origine dans les fenêtres",
           Fmt("placées : %s; sans place : %s", placed.c_str(), missing.empty() ? "aucune" : missing.c_str()),
           "Réduire la région kernel-map (et l'annoncer via svcGetInfo) pour le NCE macOS ; le disposer avant le lancement "
           "ou basculer sur Dynarmic si les fenêtres ne suffisent pas");
}

void * StackThread(void * arg) {
    *static_cast<std::uint64_t *>(arg) = reinterpret_cast<std::uint64_t>(&arg);
    return nullptr;
}

void TestHostAllocationsAfterReservation() {
    auto outside = [](std::uint64_t a, std::uint64_t size) { return !InGuestWindow(a) && !InGuestWindow(a + size - 1); };
    bool ok = true;
    std::string detail;

    void * big = mmap(nullptr, 4 * GiB, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    ok = ok && big != MAP_FAILED && outside(reinterpret_cast<std::uint64_t>(big), 4 * GiB);
    detail += Fmt("mmap 4 Gio à %p ; ", big);
    void * heap = std::malloc(512 * MiB);
    ok = ok && heap != nullptr && outside(reinterpret_cast<std::uint64_t>(heap), 512 * MiB);
    detail += Fmt("malloc 512 Mio à %p ; ", heap);
    std::free(heap);
    if (big != MAP_FAILED) {
        munmap(big, 4 * GiB);
    }

    int stacks_outside = 0;
    constexpr int Threads = 64;
    for (int i = 0; i < Threads; i++) {
        pthread_t t;
        std::uint64_t sp = 0;
        pthread_create(&t, nullptr, StackThread, &sp);
        pthread_join(t, nullptr);
        stacks_outside += outside(sp, 1) ? 1 : 0;
    }
    ok = ok && stacks_outside == Threads;
    detail += Fmt("%d/%d piles de threads hors fenêtre ; ", stacks_outside, Threads);

    std::string metal;
    const bool metal_ok = MetalSmoke(metal);
    ok = ok && metal_ok;
    detail += "Metal : " + metal;
    Expect("M2.8", ok, Class::Blocker, "Allocations hôte (mmap, malloc, piles, Metal) intactes avec la fenêtre réservée",
           detail);
}

void TestGranularity() {
    const std::uint64_t base = WindowAlloc(64 * KiB, PROT_READ | PROT_WRITE);

    void * p = mmap(reinterpret_cast<void *>(base + 0x1000), 0x1000, PROT_READ, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
    const int map_err = p == MAP_FAILED ? errno : 0;
    Expect("M2.9", p == MAP_FAILED, Class::Change, "mmap MAP_FIXED sur une adresse alignée 4 Kio (non 16 Kio) refusé",
           p == MAP_FAILED ? Fmt("refusé : %s", strerror(map_err)) : Fmt("ACCEPTÉ à %p", p),
           "Ne jamais déléguer l'alignement au noyau");

    int r = mprotect(reinterpret_cast<void *>(base + 0x1000), 0x1000, PROT_READ);
    const int prot_err = r != 0 ? errno : 0;
    Expect("M2.10", r != 0, Class::Change, "mprotect sur une adresse alignée 4 Kio refusé",
           r != 0 ? Fmt("refusé : %s", strerror(prot_err)) : std::string("ACCEPTÉ"),
           "Ne jamais déléguer l'alignement au noyau");

    r = mprotect(reinterpret_cast<void *>(base + 0x4000), 0x1000, PROT_READ);
    std::uint64_t start = 0, size = 0;
    const int prot_after = RegionProt(base + 0x4000 + 0x3000, &start, &size);
    const bool widened = r == 0 && (prot_after & VM_PROT_WRITE) == 0;
    Expect("M2.11", !widened, Class::Change, "mprotect d'une longueur de 4 Kio n'agit que sur 4 Kio",
           Fmt("mprotect(base+16K, 4K, R) = %d ; protection effective de base+16K+12K : %s (région %#llx+%#llx)", r,
               ProtText(prot_after).c_str(), (unsigned long long)start, (unsigned long long)size),
           "Le noyau arrondit silencieusement la longueur à 16 Kio : la couche mémoire NCE ne doit émettre que des "
           "plages alignées et multiples de 16 Kio (assertion), issues du plan 4K->16K");

    const kern_return_t kr = mach_vm_protect(mach_task_self(), base + 0x9000, 0x1000, FALSE, VM_PROT_READ);
    const int prot_mach = RegionProt(base + 0x8000);
    const bool mach_widened = kr == KERN_SUCCESS && (prot_mach & VM_PROT_WRITE) == 0;
    Expect("M2.12", !mach_widened, Class::Change, "mach_vm_protect sur 4 Kio non aligné n'agit que sur 4 Kio",
           Fmt("mach_vm_protect(base+36K, 4K, R) = %d ; protection de base+32K : %s", kr, ProtText(prot_mach).c_str()),
           "mach_vm_protect tronque/arrondit silencieusement aux pages de 16 Kio : même règle que M2.11");
}

struct AliasResult {
    bool misaligned_refused = false;
    std::string misaligned_text;
    bool coherent = false;
    bool perms = false;
    std::string perm_text;
    bool exec = false;
    std::string exec_text;
};

AliasResult CheckAliases(BackingKind kind) {
    AliasResult r;
    Backing b = CreateBacking(4 * MiB, kind);
    if (!b.ok()) {
        r.misaligned_text = "support non créé";
        return r;
    }
    const std::uint64_t va_rw = WindowAlloc(64 * KiB, PROT_NONE);
    const std::uint64_t va_r = WindowAlloc(64 * KiB, PROT_NONE);
    const std::uint64_t va_x = WindowAlloc(64 * KiB, PROT_NONE);

    // A host page can only show 16 KiB of backing that start on a 16 KiB boundary.
    std::memset(b.base, 0xAB, 0x4000);
    std::memset(b.base + 0x4000, 0xCD, 0x4000);
    int err = 0;
    const bool misaligned = MapBacking(b, va_rw, 0x1000, 0x4000, PROT_READ, &err);
    std::uint64_t first_word = 0;
    if (misaligned) {
        first_word = *reinterpret_cast<volatile std::uint64_t *>(va_rw);
    }
    r.misaligned_refused = !misaligned;
    r.misaligned_text = misaligned ? Fmt("ACCEPTÉ, 1er mot lu %#llx (attendu le support à +4 Kio, soit 0xabab…)",
                                         (unsigned long long)first_word)
                                   : "refusé : " + MapErrorText(b, err);

    const bool m1 = MapBacking(b, va_rw, 0x4000, 0x8000, PROT_READ | PROT_WRITE);
    const bool m2 = MapBacking(b, va_r, 0x4000, 0x8000, PROT_READ);
    r.coherent = m1 && m2;
    if (r.coherent) {
        std::memcpy(b.base + 0x4000 + 0x123, "\x11\x22\x33\x44", 4);
        r.coherent = std::memcmp(reinterpret_cast<void *>(va_rw + 0x123), "\x11\x22\x33\x44", 4) == 0 &&
                     std::memcmp(reinterpret_cast<void *>(va_r + 0x123), "\x11\x22\x33\x44", 4) == 0;
        *reinterpret_cast<volatile std::uint64_t *>(va_rw + 0x5008) = 0xA11A5;
        r.coherent = r.coherent && *reinterpret_cast<volatile std::uint64_t *>(va_r + 0x5008) == 0xA11A5 &&
                     *reinterpret_cast<std::uint64_t *>(b.base + 0x4000 + 0x5008) == 0xA11A5;
        ClearProbeFault();
        const int fault_r = ncev_probe_store(reinterpret_cast<void *>(va_r + 0x10), 1);
        const ProbeFault fr = LastProbeFault();
        const int fault_rw = ncev_probe_store(reinterpret_cast<void *>(va_rw + 0x10), 2);
        r.perms = fault_r == 1 && fault_rw == 0 && *reinterpret_cast<volatile std::uint64_t *>(va_r + 0x10) == 2;
        r.perm_text = "écriture sur l'alias R : " + FaultText(fr);
    }

    // Guest code in the backing, executed through an RX alias, then rewritten through
    // the backing (guest JIT, svcMapProcessCodeMemory, code memory aliases).
    auto * code = reinterpret_cast<std::uint32_t *>(b.base + 0x10000);
    code[0] = 0xD2800540; // mov x0, #42
    code[1] = 0xD65F03C0; // ret
    const bool mx = MapBacking(b, va_x, 0x10000, 0x4000, PROT_READ | PROT_EXEC, &err);
    std::uint64_t first = 0, second = 0;
    ProbeFault ff{};
    if (mx) {
        sys_icache_invalidate(reinterpret_cast<void *>(va_x), 0x4000);
        SetProbeExecTarget(va_x);
        ClearProbeFault();
        first = ncev_probe_call(reinterpret_cast<void *>(va_x));
        ff = LastProbeFault();
        code[0] = 0xD2800560; // mov x0, #43
        sys_icache_invalidate(reinterpret_cast<void *>(va_x), 0x4000);
        second = ncev_probe_call(reinterpret_cast<void *>(va_x));
        SetProbeExecTarget(0);
    }
    r.exec = mx && first == 42 && second == 43;
    if (!mx) {
        r.exec_text = "mapping RX refusé : " + MapErrorText(b, err);
    } else if (first == 0xFA17) {
        r.exec_text = "faute d'instruction à l'exécution : " + FaultText(ff);
    } else {
        r.exec_text = Fmt("1re exécution = %llu, après réécriture + invalidation = %llu", (unsigned long long)first,
                          (unsigned long long)second);
    }
    DestroyBacking(b);
    return r;
}

void TestAliases() {
    const AliasResult shm = CheckAliases(BackingKind::Shm);
    const AliasResult entry = CheckAliases(BackingKind::MachEntry);
    Expect("M2.13", shm.misaligned_refused && entry.misaligned_refused, Class::Change,
           "Mapping d'un décalage de support aligné 4 Kio (non 16 Kio) refusé",
           "shm : " + shm.misaligned_text + " | entrée Mach : " + entry.misaligned_text,
           "La couche NCE doit vérifier elle-même l'alignement 16 Kio du décalage de support (plan 4K->16K)");
    Expect("M2.14", entry.coherent && entry.perms, Class::Blocker,
           "Mapping direct du support et alias multiples cohérents, permissions indépendantes par alias",
           Fmt("entrée Mach : cohérence %s, %s | shm : cohérence %s", entry.coherent ? "ok" : "FAUX", entry.perm_text.c_str(),
               shm.coherent && shm.perms ? "ok" : "FAUX"));
    Expect("M2.15", shm.exec, Class::Change, "Code invité exécutable depuis le support shm actuel de HostMemory",
           shm.exec_text,
           "Support du NCE macOS = entrée mémoire Mach nommée (mach_make_memory_entry_64 + mach_vm_map FIXED|OVERWRITE) "
           "au lieu de shm_open ; HostMemory de Dynarmic inchangé");
    Expect("M2.16", entry.exec, Class::Blocker,
           "Code exécutable depuis une entrée mémoire Mach (alias RX) et réécrit via l'alias RW", entry.exec_text);

    Backing b = CreateBacking(1 * MiB);
    const std::uint64_t va_rw = WindowAlloc(64 * KiB, PROT_NONE);
    MapBacking(b, va_rw, 0, 0x10000, PROT_READ | PROT_WRITE);
    const int rwx = mprotect(reinterpret_cast<void *>(va_rw), 0x4000, PROT_READ | PROT_WRITE | PROT_EXEC);
    Note("M2.17", "Page RWX unique",
         rwx == 0 ? std::string("acceptée") : Fmt("refusée (%s) : W^X, Horizon n'en a pas besoin (code = alias RX + RW)", strerror(errno)));

    // Fault classification.
    const std::uint64_t va_r = WindowAlloc(64 * KiB, PROT_NONE);
    MapBacking(b, va_r, 0, 0x10000, PROT_READ);
    ClearProbeFault();
    ncev_probe_store(reinterpret_cast<void *>(va_r + 0x2468), 3);
    const ProbeFault write_ro = LastProbeFault();
    const std::uint64_t none = WindowAlloc(16 * KiB, PROT_NONE);
    ClearProbeFault();
    std::uint64_t dummy = 0;
    ncev_probe_load(reinterpret_cast<void *>(none + 0x1238), &dummy);
    const ProbeFault read_none = LastProbeFault();
    ClearProbeFault();
    ncev_probe_ldxr(reinterpret_cast<void *>(va_rw + 0x10D)); // crosses a 16-byte boundary
    const ProbeFault align = LastProbeFault();
    ClearProbeFault();
    const int inside = ncev_probe_ldxr(reinterpret_cast<void *>(va_rw + 0x101)); // inside 16 bytes
    ClearProbeFault();
    ncev_probe_load(reinterpret_cast<void *>(0x7F00'0000'0000ULL), &dummy);
    const ProbeFault unmapped = LastProbeFault();

    const Esr e_ro{write_ro.esr}, e_none{read_none.esr}, e_align{align.esr};
    // FSC is not usable to tell "no access" from "not present": the kernel maps pages
    // lazily, so a write to a never-touched read-only page reports a translation fault.
    const bool esr_exact = e_ro.IsDataAbort() && e_ro.WnR() && write_ro.far == va_r + 0x2468 &&
                           e_none.IsDataAbort() && !e_none.WnR() && read_none.far == none + 0x1238 &&
                           e_align.IsAlignment() && align.far == va_rw + 0x10D;
    Expect("M2.18", esr_exact, Class::Blocker,
           "Faute précise à l'octet avec syndrome ESR fiable (donnée/instruction, lecture/écriture, alignement)",
           "FSC traduction/permission non significatif (pages paresseuses) : décider par la table des pages invitée | "
           "écriture sur R : " + FaultText(write_ro) + " | lecture PROT_NONE : " + FaultText(read_none) +
               " | LDXR à cheval sur 16 octets : " + FaultText(align) + " | adresse non mappée : " + FaultText(unmapped));
    const bool linux_split = write_ro.sig == SIGSEGV && read_none.sig == SIGSEGV && align.sig == SIGBUS;
    Expect("M2.19", linux_split, Class::Change,
           "Répartition Linux des signaux (SIGSEGV = accès, SIGBUS = alignement) valable",
           Fmt("écriture sur R -> %s, PROT_NONE -> %s, alignement -> %s, non mappé -> %s", SignalName(write_ro.sig),
               SignalName(read_none.sig), SignalName(align.sig), SignalName(unmapped.sig)),
           "Même gestionnaire sur SIGBUS et SIGSEGV, classification par ESR (__es.__esr : EC/FSC/WnR) et __es.__far");
    Note("M2.20", "Écart matériel M4 / Cortex-A57 (non corrigeable en natif)",
         inside == 0 ? "LDXR non aligné à l'intérieur de 16 octets : aucune faute sur l'hôte (FEAT_LSE2), faute sur "
                       "Cortex-A57 ; seul un programme qui planterait sur Switch voit la différence"
                     : "LDXR non aligné à l'intérieur de 16 octets : faute, comme sur Cortex-A57");
    DestroyBacking(b);
}

// ---- Split host pages: exact emulation of the accesses the host page refuses.

struct SplitPage {
    std::uint64_t va = 0;
    std::array<GuestPage, GuestPagesPerHostPage> guest{};
    std::uint8_t * backing = nullptr;
    std::uint64_t emulated = 0;
    std::uint64_t refused = 0;
};
SplitPage * g_split = nullptr;

// Minimal decoder for the test (LDR/STR immediate, unsigned offset). The real
// handler would use nce/interpreter_visitor (all load/store forms).
bool SplitFaultHook(Vcpu &, int, siginfo_t *, Mcontext * mc) {
    SplitPage & sp = *g_split;
    const Esr esr{mc->__es.__esr};
    const std::uint64_t far = mc->__es.__far;
    if (far < sp.va || far >= sp.va + HostPageSize) {
        return false;
    }
    const std::uint32_t index = static_cast<std::uint32_t>((far - sp.va) / GuestPageSize);
    const Access access = esr.IsInstructionAbort() ? Access::Execute : (esr.WnR() ? Access::Write : Access::Read);
    if (!GuestAllows(sp.guest[index], access) || access == Access::Execute) {
        sp.refused++;
        return false;
    }
    const std::uint32_t insn = *reinterpret_cast<const std::uint32_t *>(mc->__ss.__pc);
    if ((insn & 0x3B000000) != 0x39000000) {
        return false;
    }
    const std::uint32_t size = insn >> 30;
    const std::uint32_t opc = (insn >> 22) & 3;
    const std::uint32_t rn = (insn >> 5) & 31;
    const std::uint32_t rt = insn & 31;
    const std::uint64_t imm = static_cast<std::uint64_t>((insn >> 10) & 0xFFF) << size;
    const std::uint64_t address = (rn == 31 ? mc->__ss.__sp : Gpr(mc, static_cast<int>(rn))) + imm;
    if (address != far) {
        return false;
    }
    std::uint8_t * host = sp.backing + sp.guest[index].backing_offset + (address & (GuestPageSize - 1));
    const std::size_t bytes = std::size_t(1) << size;
    if (opc == 0) {
        const std::uint64_t value = rt == 31 ? 0 : Gpr(mc, static_cast<int>(rt));
        std::memcpy(host, &value, bytes);
    } else if (opc == 1) {
        std::uint64_t value = 0;
        std::memcpy(&value, host, bytes);
        if (rt != 31) {
            Gpr(mc, static_cast<int>(rt)) = value;
        }
    } else {
        return false;
    }
    mc->__ss.__pc += 4;
    sp.emulated++;
    return true;
}

bool BuildCode(const std::vector<std::uint32_t> & words, GuestImage & image) {
    std::string error;
    return BuildPatchedImage(words, image, error, nullptr, false);
}

void TestSplitPages() {
    Backing b = CreateBacking(1 * MiB);
    // Guest view of one host page: [RW, RW, R, R], one aligned 16 KiB backing block.
    SplitPage sp;
    sp.backing = b.base;
    sp.va = WindowAlloc(16 * KiB, PROT_NONE);
    for (std::uint32_t i = 0; i < 4; i++) {
        sp.guest[i] = {true, 0x8000 + i * GuestPageSize, static_cast<std::uint8_t>(i < 2 ? (PermR | PermW) : PermR)};
    }
    const HostPagePlan plan = PlanHostPage(sp.guest);
    const int host_prot = (plan.host_perm & PermR ? PROT_READ : 0) | (plan.host_perm & PermW ? PROT_WRITE : 0) |
                          (plan.host_perm & PermX ? PROT_EXEC : 0);
    MapBacking(b, sp.va, plan.backing_offset, HostPageSize, host_prot);
    g_split = &sp;

    std::vector<std::uint32_t> words;
    oaknut::VectorCodeGenerator c{words};
    c.STR(X1, X0);
    c.LDR(X2, X0);
    c.STR(X3, X4);
    c.BRK(0);
    GuestImage image;
    BuildCode(words, image);
    const std::uint64_t code = image.base + image.code_offset;
    const std::uint64_t stack = WindowAlloc(64 * KiB, PROT_READ | PROT_WRITE);

    Vcpu vcpu;
    vcpu.BindThread();
    vcpu.fault_hook = SplitFaultHook;
    vcpu.ctx.sp = stack + 32 * KiB;
    vcpu.ctx.pc = code;
    vcpu.ctx.cpu_registers[0] = sp.va + 0x0010;
    vcpu.ctx.cpu_registers[1] = 0x1111222233334444ULL;
    vcpu.ctx.cpu_registers[3] = 0x5555;
    vcpu.ctx.cpu_registers[4] = sp.va + 0x2010;
    const std::uint64_t hr = vcpu.Run();

    const std::uint64_t stored = *reinterpret_cast<std::uint64_t *>(b.base + 0x8000 + 0x10);
    const std::uint64_t ro_after = *reinterpret_cast<std::uint64_t *>(b.base + 0xA000 + 0x10);
    const bool exact = plan.kind == HostPageKind::Split && plan.host_perm == PermR && hr == HrDataAbort() &&
                       vcpu.ctx.pc == code + 8 && vcpu.exit_far == sp.va + 0x2010 && stored == 0x1111222233334444ULL &&
                       vcpu.ctx.cpu_registers[2] == 0x1111222233334444ULL && ro_after == 0 && sp.emulated == 1;
    Expect("M2.21", exact, Class::Blocker,
           "Page hôte scindée [RW,RW,R,R] : écriture permise émulée, écriture interdite = faute invitée exacte",
           Fmt("hôte %s ; écriture page 0 émulée (%llu), lecture native = %#llx, écriture page 2 -> sortie %#llx à "
               "pc=+%llu far=%#llx ; page 2 inchangée = %s",
               plan.host_perm == PermR ? "R" : "?", (unsigned long long)sp.emulated,
               (unsigned long long)vcpu.ctx.cpu_registers[2], (unsigned long long)hr,
               (unsigned long long)(vcpu.ctx.pc - code), (unsigned long long)vcpu.exit_far, ro_after == 0 ? "oui" : "non"));

    // Cost of one emulated access.
    words.clear();
    oaknut::VectorCodeGenerator loop_gen{words};
    oaknut::Label loop;
    loop_gen.l(loop);
    loop_gen.STR(X1, X0);
    loop_gen.SUBS(X5, X5, 1);
    loop_gen.B(oaknut::Cond::NE, loop);
    loop_gen.BRK(0);
    GuestImage loop_image;
    BuildCode(words, loop_image);
    constexpr std::uint64_t N = 20000;
    sp.emulated = 0;
    vcpu.ctx.pc = loop_image.base + loop_image.code_offset;
    vcpu.ctx.cpu_registers[5] = N;
    const std::uint64_t t0 = NowNs();
    const std::uint64_t hr2 = vcpu.Run();
    const double per = double(NowNs() - t0) / double(N);
    Note("M2.22", "Coût d'un accès émulé sur page scindée (signal + décodage + support)",
         Fmt("%.2f µs par accès (%llu émulés, sortie %#llx) ; un accès natif coûte ~0,3 ns", per / 1000.0,
             (unsigned long long)sp.emulated, (unsigned long long)hr2));

    // Execution in a split page: [RX, R, R, R] gives a host R page.
    SplitPage ex;
    ex.backing = b.base;
    ex.va = WindowAlloc(16 * KiB, PROT_NONE);
    for (std::uint32_t i = 0; i < 4; i++) {
        ex.guest[i] = {true, 0x10000 + i * GuestPageSize, static_cast<std::uint8_t>(i == 0 ? (PermR | PermX) : PermR)};
    }
    const HostPagePlan exec_plan = PlanHostPage(ex.guest);
    *reinterpret_cast<std::uint32_t *>(b.base + 0x10000) = 0xD4200000; // brk #0
    MapBacking(b, ex.va, exec_plan.backing_offset, HostPageSize, PROT_READ);
    g_split = &ex;
    words.clear();
    oaknut::VectorCodeGenerator br_gen{words};
    br_gen.BR(X0);
    GuestImage br_image;
    BuildCode(words, br_image);
    vcpu.ctx.pc = br_image.base + br_image.code_offset;
    vcpu.ctx.cpu_registers[0] = ex.va;
    const std::uint64_t hr3 = vcpu.Run();
    const Esr esr{vcpu.exit_esr};
    const bool detected = hr3 == HrPrefetchAbort() && vcpu.ctx.pc == ex.va && esr.IsInstructionAbort();
    Expect("M2.23", detected, Class::Blocker, "Exécution dans une page scindée détectée exactement (faute d'instruction)",
           Fmt("hôte %s, emulated_exec=%#x ; sortie %#llx, pc=%#llx, %s", exec_plan.host_perm == PermR ? "R" : "?",
               exec_plan.emulated_exec, (unsigned long long)hr3, (unsigned long long)vcpu.ctx.pc,
               FaultText({vcpu.exit_signal, 0, 0, vcpu.exit_far, vcpu.exit_esr, 0}).c_str()));
    Expect("M2.24", false, Class::Change, "Exécution native du code invité situé dans une page scindée",
           "la page hôte ne peut pas être X sans rendre X les pages voisines non exécutables",
           "Décision : soit interpréteur A64 complet pour les pages exécutables scindées (InterpreterVisitor ne couvre "
           "que les accès mémoire), soit politique RX explicite et journalisée par page ; jamais implicite");

    g_split = nullptr;
    DestroyBacking(b);
}

void TestPlanner() {
    using Pages = std::array<GuestPage, GuestPagesPerHostPage>;
    auto page = [](std::uint64_t off, std::uint8_t perm) { return GuestPage{true, off, perm}; };
    const std::uint8_t RW = PermR | PermW, RX = PermR | PermX;
    struct Case {
        const char * name;
        Pages pages;
        HostPageKind kind;
        std::uint8_t host_perm;
        std::uint8_t emu_w;
        std::uint8_t emu_x;
    };
    const Case cases[] = {
        {"4×RW contigu aligné", {page(0x4000, RW), page(0x5000, RW), page(0x6000, RW), page(0x7000, RW)}, HostPageKind::Direct, RW, 0, 0},
        {"4×RX contigu aligné", {page(0, RX), page(0x1000, RX), page(0x2000, RX), page(0x3000, RX)}, HostPageKind::Direct, RX, 0, 0},
        {"support non aligné", {page(0x1000, RW), page(0x2000, RW), page(0x3000, RW), page(0x4000, RW)}, HostPageKind::Split, PermNone, 0xF, 0},
        {"support non contigu", {page(0x4000, RW), page(0x9000, RW), page(0x6000, RW), page(0x7000, RW)}, HostPageKind::Split, PermNone, 0xF, 0},
        {"fin de .text | .rodata", {page(0, RX), page(0x1000, RX), page(0x2000, PermR), page(0x3000, PermR)}, HostPageKind::Split, PermR, 0, 0x3},
        {"fin de .rodata | .data", {page(0, PermR), page(0x1000, RW), page(0x2000, RW), page(0x3000, RW)}, HostPageKind::Split, PermR, 0xE, 0},
        {"page partiellement présente", {page(0, RW), page(0x1000, RW), GuestPage{}, GuestPage{}}, HostPageKind::Split, PermNone, 0x3, 0},
        {"aucune page", {GuestPage{}, GuestPage{}, GuestPage{}, GuestPage{}}, HostPageKind::Unmapped, PermNone, 0, 0},
    };
    int bad = 0;
    std::string fails;
    for (const Case & k : cases) {
        const HostPagePlan p = PlanHostPage(k.pages);
        // Exactness: the host never grants what a guest page forbids.
        bool never_wider = true;
        for (const GuestPage & g : k.pages) {
            const std::uint8_t gp = g.mapped ? g.perm : PermNone;
            never_wider = never_wider && (p.host_perm & ~gp) == 0;
        }
        if (p.kind != k.kind || p.host_perm != k.host_perm || p.emulated_write != k.emu_w || p.emulated_exec != k.emu_x ||
            !never_wider) {
            bad++;
            fails += std::string(k.name) + " ; ";
        }
    }
    // Exhaustive: every combination of present/absent and R/W/X on contiguous backing.
    std::uint64_t combos = 0, violations = 0;
    for (std::uint32_t code = 0; code < (1u << 16); code++) {
        Pages pages{};
        for (std::uint32_t i = 0; i < 4; i++) {
            const std::uint32_t nib = (code >> (4 * i)) & 0xF;
            pages[i] = GuestPage{(nib & 8) != 0, 0x40000 + i * GuestPageSize, static_cast<std::uint8_t>(nib & 7)};
        }
        const HostPagePlan p = PlanHostPage(pages);
        combos++;
        for (std::uint32_t i = 0; i < 4; i++) {
            const std::uint8_t gp = pages[i].mapped ? pages[i].perm : PermNone;
            const std::uint8_t emulated = static_cast<std::uint8_t>(((p.emulated_read >> i) & 1 ? PermR : 0) |
                                                                    ((p.emulated_write >> i) & 1 ? PermW : 0) |
                                                                    ((p.emulated_exec >> i) & 1 ? PermX : 0));
            // Host grants nothing extra, and host + emulation covers exactly the guest permission.
            if ((p.host_perm & ~gp) != 0 || static_cast<std::uint8_t>(p.host_perm | emulated) != gp) {
                violations++;
            }
        }
    }
    Expect("M2.25", bad == 0 && violations == 0, Class::Blocker, "Plan 4K->16K exact (cas types + 65 536 combinaisons)",
           Fmt("%d cas faux %s; %llu combinaisons, %llu violation(s) : l'hôte n'accorde jamais plus que l'invité, "
               "hôte + émulation = permission invitée",
               bad, fails.c_str(), (unsigned long long)combos, (unsigned long long)violations));
}

void TestModuleLayouts() {
    // Synthetic NSO-like layouts: segments start on 4 KiB boundaries, module base on a
    // 2 MiB boundary, backing allocated contiguously. Counts host pages per kind.
    std::mt19937_64 rng(1234);
    std::uint64_t total = 0, direct = 0, split = 0, split_exec = 0, split_write = 0;
    constexpr int Modules = 200;
    for (int m = 0; m < Modules; m++) {
        const std::uint64_t text = (std::uint64_t(rng() % 8000) + 16) * GuestPageSize;
        const std::uint64_t ro = (std::uint64_t(rng() % 2000) + 4) * GuestPageSize;
        const std::uint64_t data = (std::uint64_t(rng() % 1000) + 2) * GuestPageSize;
        const std::uint64_t size = text + ro + data;
        auto perm_at = [&](std::uint64_t off) -> std::uint8_t {
            if (off < text) {
                return PermR | PermX;
            }
            if (off < text + ro) {
                return PermR;
            }
            return PermR | PermW;
        };
        for (std::uint64_t host = 0; host < size; host += HostPageSize) {
            std::array<GuestPage, GuestPagesPerHostPage> pages{};
            for (std::uint32_t i = 0; i < 4; i++) {
                const std::uint64_t off = host + i * GuestPageSize;
                if (off < size) {
                    pages[i] = GuestPage{true, off, perm_at(off)};
                }
            }
            const HostPagePlan p = PlanHostPage(pages);
            total++;
            if (p.kind == HostPageKind::Direct) {
                direct++;
            } else if (p.kind == HostPageKind::Split) {
                split++;
                split_exec += p.emulated_exec != 0;
                split_write += p.emulated_write != 0;
            }
        }
    }
    Note("M2.26", "Plan sur 200 modules synthétiques (.text/.rodata/.data à 4 Kio)",
         Fmt("%llu pages hôte : %llu directes, %llu scindées (%.3f %%), dont %llu avec exécution émulée et %llu avec "
             "écriture émulée ; au plus 3 pages scindées par module, toujours aux frontières de segments",
             (unsigned long long)total, (unsigned long long)direct, (unsigned long long)split,
             100.0 * double(split) / double(total), (unsigned long long)split_exec, (unsigned long long)split_write));

    Core::NCE::Patcher patcher;
    std::vector<std::uint8_t> image(64 * KiB);
    patcher.PatchText(image.data(), image.size(), 0, image.size());
    const std::uint64_t section = patcher.GetSectionSize();
    Expect("M2.27", section % HostPageSize == 0, Class::Change, "Section de patch du NCE alignée sur 16 Kio",
           Fmt("Patcher::GetSectionSize() = %#llx (alignement NcePageSize = 4 Kio) ; en mode PostData elle suit .bss",
               (unsigned long long)section),
           "patcher : aligner la taille et le début de la section de patch sur 16 Kio, sinon sa première page partage "
           "une page hôte avec la fin de .data/.bss (RW) et devient une page scindée exécutable");
}

} // namespace

void RunMemoryTests() {
    BeginStep("2", "Mémoire (bloquant)");
    TestPageSizes();
    TestPageZero();
    TestHostMap();
    if (!g_startup.reserved) {
        EndStep();
        return;
    }
    TestHostAllocationsAfterReservation();
    TestGranularity();
    TestAliases();
    TestSplitPages();
    TestPlanner();
    TestModuleLayouts();
    EndStep();
}

} // namespace ncev
