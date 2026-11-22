#include "kernel/xbox_memory_layout.h"
#include "kernel/guest_vmem.h"
#include "platform/win32_compat.h"
#include "platform/posix_mmio.h"
#include "platform/mmio_decode_a64.h"
#include "usb/ohci.h"
#include "apu/apu_state.h"
#include "apu/apu.h"
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(XBOXRECOMP_TEST_FAILURES)
void xbox_TestFailMappings(int on);
void xbox_TestFailVirtualProtectAfter(int successes);
void xbox_TestFailViewReleaseAfter(int successes);
int xbox_TestOhciStopWorker(void);
size_t xbox_TestOhciTrapBytes(size_t bytes);
#endif
static MCPXAPUState apu;
static volatile sig_atomic_t traps, chains, allow_chain, context_changed;
#if defined(XBOXRECOMP_TEST_FAILURES)
static pthread_t producer_thread_id;
static atomic_uint callbacks[4], callbacks_on_producer;
static atomic_uint stop_ohci_in_probe, stopped_during_probe;
void xbox_TestObservePosixDevice(int device)
{
    if (device >= 1 && device <= 3)
        atomic_fetch_add_explicit(&callbacks[device], 1, memory_order_relaxed);
    if (pthread_equal(pthread_self(), producer_thread_id))
        atomic_fetch_add_explicit(&callbacks_on_producer, 1, memory_order_relaxed);
    if (device == 3 && atomic_exchange_explicit(&stop_ohci_in_probe, 0, memory_order_acq_rel))
        atomic_store_explicit(&stopped_during_probe, xbox_TestOhciStopWorker(),
                              memory_order_release);
}
#endif
static void fault_handler(int signal_number, siginfo_t *info, void *context)
{
    ++traps;
#if defined(__aarch64__)
    ucontext_t *uc = context;
    uintptr_t pc = (uintptr_t)A64_CTX_PC(uc);
    uint64_t reg = A64_CTX_X(uc, 0), sp = A64_CTX_SP(uc);
    if (!xbox_PosixMmioFault(context, info->si_addr)) {
        if (pc != (uintptr_t)A64_CTX_PC(uc) || reg != A64_CTX_X(uc, 0) ||
            sp != A64_CTX_SP(uc))
            ++context_changed;
        if (!allow_chain) _exit(80 + signal_number);
        ++chains;
        A64_CTX_PC(uc) += 4;
    }
#else
    (void)context; (void)info;
    _exit(80 + signal_number);
#endif
}
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); return 1; } } while (0)
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, sizeof(v)); }

int main(void)
{
#if !defined(__aarch64__)
    return 77;
#else
    CHECK(setenv("RECOMP_HEAP_RECLAIM", "1", 1) == 0);
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    CHECK(page >= 4096);
    uint8_t *v = VirtualAlloc(NULL, 4 * page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(v);
    v[100] = 77;
    CHECK(!VirtualAlloc(v, page, MEM_RESERVE, PAGE_NOACCESS) && v[100] == 77);
    DWORD old;
    CHECK(VirtualProtect(v + page, page, PAGE_READONLY, &old) && old == PAGE_READWRITE);
    MEMORY_BASIC_INFORMATION split;
    CHECK(VirtualQuery(v, &split, sizeof(split)) && split.Protect == PAGE_READWRITE &&
          split.RegionSize == page);
    CHECK(VirtualQuery(v + page, &split, sizeof(split)) &&
          split.Protect == PAGE_READONLY && split.RegionSize == page);
    CHECK(!VirtualProtect(v + 1, 3 * page - 2, PAGE_READWRITE, &old));
    if (page > 4096) CHECK(!VirtualProtect(v + 4096, 4096, PAGE_READONLY, &old));
    CHECK(VirtualProtect(v + page, page, PAGE_READWRITE, &old) && old == PAGE_READONLY);
    CHECK(VirtualProtect(v + 1, 3 * page - 2, PAGE_READWRITE, &old));
    CHECK(VirtualFree(v, 0, MEM_RELEASE));
    v = VirtualAlloc(NULL, 2 * page, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(v);
    if (page > 4096) {
        CHECK(!VirtualAlloc(v, 4096, MEM_COMMIT, PAGE_READWRITE));
        CHECK(!VirtualAlloc(v + 4096, 4096, MEM_COMMIT, PAGE_READWRITE));
        MEMORY_BASIC_INFORMATION info;
        CHECK(VirtualQuery(v, &info, sizeof(info)) && info.Protect == PAGE_NOACCESS);
    }
    CHECK(VirtualAlloc(v, page, MEM_COMMIT, PAGE_READWRITE) == v);
    if (page > 4096)
        CHECK(VirtualAlloc(v + 4096, 4096, MEM_COMMIT, PAGE_READWRITE) == v + 4096);
    CHECK(VirtualFree(v, 0, MEM_RELEASE));
    v = VirtualAlloc(NULL, 4 * page, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(v && VirtualFree(v + page, page, MEM_RELEASE));
    MEMORY_BASIC_INFORMATION hole;
    CHECK(VirtualQuery(v + page, &hole, sizeof(hole)) &&
          hole.State == MEM_FREE && hole.RegionSize == page);
    CHECK(VirtualFree(v, 0, MEM_RELEASE) && VirtualFree(v + 2 * page, 0, MEM_RELEASE));

    uint8_t xbe[0x1000] = {0};
    put32(xbe + 0x104, XBOX_BASE_ADDRESS);
    put32(xbe + 0x108, sizeof(xbe));
    put32(xbe + 0x11c, 1);
    put32(xbe + 0x120, XBOX_BASE_ADDRESS + 0x180);
    put32(xbe + 0x180, 4); /* synthetic executable section */
    put32(xbe + 0x184, 0x200000);
    put32(xbe + 0x188, 0x1000);
    put32(xbe + 0x18c, 0x300);
    put32(xbe + 0x190, 4);
    put32(xbe + 0x300, 0x12345678);
    CHECK(!xbox_MemoryLayoutInit(xbe, 20));
    xbox_SetMapSize(SIZE_MAX);
    CHECK(!xbox_MemoryLayoutInit(xbe, sizeof(xbe)) && !xbox_GetMemoryBase());
    xbox_SetMapSize(0);
    setenv("RECOMP_AC97_READY", "1", 1);
    setenv("RECOMP_USB", "1", 1);
    setenv("RECOMP_USB_TRACE", "1", 1);
    setenv("RECOMP_EXT_VMA", "1", 1);
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = fault_handler; action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGSEGV);
    sigaddset(&action.sa_mask, SIGBUS);
    CHECK(sigaction(SIGSEGV, &action, NULL) == 0 && sigaction(SIGBUS, &action, NULL) == 0);
    CHECK(!xbox_MemoryLayoutInit(xbe, sizeof(xbe)));
    CHECK(xbox_PosixMmioStart());
#if defined(XBOXRECOMP_TEST_FAILURES)
    producer_thread_id = pthread_self();
    xbox_TestFailMappings(1);
    CHECK(!xbox_MemoryLayoutInit(xbe, sizeof(xbe)) && !xbox_GetMemoryBase());
    xbox_TestFailMappings(0);
    xbox_TestFailVirtualProtectAfter(1);
    CHECK(!xbox_MemoryLayoutInit(xbe, sizeof(xbe)) && !xbox_GetMemoryBase());
#endif

    CHECK(xbox_MemoryLayoutInit(xbe, sizeof(xbe)));
    CHECK(xbox_PosixMmioAttach(NULL) && xbox_PosixMmioRegisterThread());
    allow_chain = 1;
    uint32_t absent_value = 0x13572468u;
    uintptr_t absent_apu = (uintptr_t)xbox_GetMemoryOffset() + 0xfe800100u;
    __asm__ volatile("ldr %w0, [%1]" : "+r"(absent_value) : "r"(absent_apu) : "memory");
    allow_chain = 0;
    CHECK(absent_value == 0x13572468u && chains == 1 && !context_changed);
    CHECK(xbox_PosixMmioUnregisterThread() && xbox_PosixMmioSealProducers());
    CHECK(xbox_PosixMmioBeginShutdown() && xbox_PosixMmioStop());
    CHECK(xbox_MemoryLayoutShutdownChecked());

    uint32_t first_heap = 0, first_contiguous = 0;
    for (int run = 0; run < 2; ++run) {
        CHECK(xbox_PosixMmioStart());
        CHECK(xbox_MemoryLayoutInit(xbe, sizeof(xbe)));
        CHECK(!xbox_MemoryLayoutInit(xbe, sizeof(xbe)));
        CHECK(guest_vmem_active());
        uint32_t vma = 0x74000000u, vma_size = 0x10000u, status;
        CHECK(guest_vmem_allocate(&vma, &vma_size, MEM_RESERVE, PAGE_READWRITE, &status) &&
              status == 0);
        uint32_t part = vma, part_size = 4096;
        CHECK(guest_vmem_allocate(&part, &part_size, MEM_COMMIT, PAGE_READWRITE, &status));
        if (page > 4096) CHECK(status != 0);
        else CHECK(status == 0);
        part = vma; part_size = (uint32_t)page;
        CHECK(guest_vmem_allocate(&part, &part_size, MEM_COMMIT, PAGE_READWRITE, &status) &&
              status == 0);
        part = vma; part_size = 0;
        CHECK(guest_vmem_free(&part, &part_size, MEM_RELEASE, &status) && status == 0);
        if (!run) {
            MCPXAPUState *live = mcpx_apu_init_standalone(xbox_GetMemoryBase());
            CHECK(live);
            CHECK(mcpx_apu_quiesce(live));
            CHECK(mcpx_apu_shutdown_checked(live));
        }
        CHECK(g_xbox_image_lo == 0x200000 && g_xbox_image_hi == 0x201000);
        CHECK(*(uint32_t *)xbox_TranslateMappedRam(0x200000, 4) == 0x12345678);
        uint32_t *ram = xbox_TranslateMappedRam(0x100000, 4);
        uint32_t *mirror = xbox_TranslateMappedRam(0x4100000, 4);
        uint32_t *contig = xbox_TranslateMappedRam(0x80001000, 4);
        uint32_t *tiled = xbox_TranslateMappedRam(0xf0001000, 4);
        CHECK(ram && mirror && contig && tiled);
        *ram = 0xa5a55a5a; CHECK(*mirror == *ram);
        *contig = 0x2468ace0; CHECK(*tiled == *contig);
        CHECK(!xbox_TranslateMappedRam(0xfd000000, 4));
        CHECK(!xbox_TranslateMappedRam(0xfffffff0, 32));
        CHECK(!xbox_TranslateMappedRam(0x10000, SIZE_MAX));
        uint32_t heap = xbox_HeapAlloc(64, 16);
        uint32_t physical = xbox_ContiguousAlloc(4096, 4096);
        CHECK(heap && physical);
        if (page > 4096) {
            CHECK((physical & (page - 1)) == 0);
            CHECK(xbox_ContiguousBlockSize(physical) == page);
            uint8_t *memory = xbox_TranslateMappedRam(physical, page);
            CHECK(memory);
            CHECK(VirtualProtect(memory, page, PAGE_READONLY, &old));
            CHECK(xbox_ContiguousFree(physical));
            physical = xbox_ContiguousAlloc(4096, 4096);
            CHECK(physical && (physical & (page - 1)) == 0);
            memory = xbox_TranslateMappedRam(physical, page);
            CHECK(memory);
            memory[0] = 0x5a;
            CHECK(memory[0] == 0x5a);
        }
        if (!run) { first_heap = heap; first_contiguous = physical; }
        else CHECK(heap == first_heap && physical == first_contiguous);

        apu.ram_ptr = (uint8_t *)xbox_GetMemoryOffset();
        CHECK(xbox_PosixMmioAttach(&apu));
        CHECK(__atomic_load_n(&g_apu_state, __ATOMIC_ACQUIRE) == &apu);
        CHECK(!mcpx_apu_shutdown_checked(&apu));
        CHECK(xbox_PosixMmioRegisterThread());
        uintptr_t off = (uintptr_t)xbox_GetMemoryOffset();
        volatile uint32_t *apu_register = (volatile uint32_t *)(off + 0xfe800100u);
        *apu_register = 0x1234abcd;
        CHECK(*apu_register == 0x1234abcd);
        volatile uint32_t *dsp_neighbour = (volatile uint32_t *)(off + 0xfe83c100u);
        volatile uint32_t *dsp_register = (volatile uint32_t *)(off + 0xfe83f100u);
        *dsp_neighbour = 0x78563412; *dsp_register = 0xfeedc0de;
        CHECK(*dsp_neighbour == 0x78563412 && *dsp_register == 0xfeedc0de);
        CHECK(*(uint32_t *)xbox_McpxMemoryAlias(0x3c100, 4) == 0x78563412);
        uint32_t *before_trap = xbox_McpxMemoryAlias(0x3bff8, 16);
        before_trap[0] = 0x01020304; before_trap[1] = 0x11223344;
        before_trap[2] = 0x55667788; before_trap[3] = 0x99aabbcc;
        uint64_t cross, pair_a, pair_b;
        void *cross_ptr = (void *)(off + 0xfe83bffcu);
        __asm__ volatile("ldr %0, [%1]" : "=r"(cross) : "r"(cross_ptr) : "memory");
        CHECK(cross == UINT64_C(0x5566778811223344));
        cross = UINT64_C(0xdeadbeef76543210);
        __asm__ volatile("str %0, [%1]" : : "r"(cross), "r"(cross_ptr) : "memory");
        CHECK(before_trap[1] == 0x76543210 && before_trap[2] == 0xdeadbeef);
        cross_ptr = (void *)(off + 0xfe83bff8u);
        __asm__ volatile("ldp %0, %1, [%2]" : "=r"(pair_a), "=r"(pair_b)
                         : "r"(cross_ptr) : "memory");
        CHECK(pair_a == UINT64_C(0x7654321001020304) &&
              pair_b == UINT64_C(0x99aabbccdeadbeef));
        volatile uint8_t *ac97 = (volatile uint8_t *)(off + 0xfec0010bu);
        volatile uint32_t *ac97_neighbour = (volatile uint32_t *)(off + 0xfec02000u);
        *ac97 = 0xff; CHECK(*ac97 == 0xfd);
        *ac97_neighbour = 0x44332211; CHECK(*ac97_neighbour == 0x44332211);
        cross = UINT64_C(0x1234567887654321);
        cross_ptr = (void *)(off + 0xfebffffcu);
        __asm__ volatile("str %0, [%1]" : : "r"(cross), "r"(cross_ptr) : "memory");
        CHECK(*(uint32_t *)xbox_McpxMemoryAlias(0x3ffffc, 4) == 0x87654321 &&
              *(uint32_t *)xbox_McpxMemoryAlias(0x400000, 4) == 0x12345678);

#if defined(XBOXRECOMP_TEST_FAILURES)
        if (!run) {
            xbox_TestFailVirtualProtectAfter(1);
            xbox_OhciInit();
            CHECK(!xbox_OhciIsEnabled() &&
                  !xbox_OhciOwnsAddress(XBOX_OHCI0_BASE) &&
                  !xbox_OhciOwnsAddress(XBOX_OHCI1_BASE));
            volatile uint32_t *restored = (volatile uint32_t *)(off + XBOX_OHCI0_BASE);
            *restored = 0xabcdef01; CHECK(*restored == 0xabcdef01);
            CHECK(xbox_OhciShutdown());
        }
#endif
        xbox_OhciInit();
        CHECK(xbox_OhciIsEnabled() && xbox_OhciOwnsAddress(XBOX_OHCI0_BASE));
        volatile uint32_t *ohci = (volatile uint32_t *)(off + XBOX_OHCI0_BASE);
        CHECK(ohci[0] == 0x10);
        *(uint32_t *)xbox_McpxMemoryAlias(0x4ffffc, 4) = 0x89abcdef;
        cross_ptr = (void *)(off + XBOX_OHCI0_BASE - 4);
        __asm__ volatile("ldr %0, [%1]" : "=r"(cross) : "r"(cross_ptr) : "memory");
        CHECK(cross == UINT64_C(0x0000001089abcdef));
        cross = UINT64_C(0x5566778812345678);
        __asm__ volatile("str %0, [%1]" : : "r"(cross), "r"(cross_ptr) : "memory");
        CHECK(*(uint32_t *)xbox_McpxMemoryAlias(0x4ffffc, 4) == 0x12345678 && ohci[0] == 0x10);
        ohci[0x18 / 4] = 0x11223344;
        *(volatile uint8_t *)((uintptr_t)ohci + 0x19) = 0xaa;
        CHECK(ohci[0x18 / 4] == 0x1122aa44);
        volatile uint32_t *usb_neighbour = (volatile uint32_t *)((uintptr_t)ohci + 0x2000);
        *usb_neighbour = 0xaabbccdd; CHECK(*usb_neighbour == 0xaabbccdd);
#if defined(XBOXRECOMP_TEST_FAILURES)
        if (!run) {
            size_t previous_trap_bytes = xbox_TestOhciTrapBytes(0x8000);
            uint64_t *alias = xbox_McpxMemoryAlias(XBOX_OHCI1_BASE - UINT32_C(0xfe800000) - 8, 8);
            CHECK(alias);
            *alias = UINT64_C(0xaabbccddeeff0011);
            ucontext_t synthetic = {0};
#if defined(__APPLE__)
            struct __darwin_mcontext64 synthetic_machine = {0};
            synthetic.uc_mcontext = &synthetic_machine;
#endif
            uintptr_t native_sp;
            __asm__ volatile("mov %0, sp" : "=r"(native_sp));
            uint32_t store_pair = 0xa9000440u; /* stp x0, x1, [x2] */
            A64_CTX_SP(&synthetic) = native_sp;
            A64_CTX_PC(&synthetic) = (uintptr_t)&store_pair;
            A64_CTX_X(&synthetic, 0) = UINT64_C(0x1111222233334444);
            A64_CTX_X(&synthetic, 1) = UINT64_C(0x5555666677778888);
            A64_CTX_X(&synthetic, 2) = off + XBOX_OHCI1_BASE - 8;
#if defined(__APPLE__)
            struct __darwin_mcontext64 saved_machine = synthetic_machine;
#else
            mcontext_t saved_machine = synthetic.uc_mcontext;
#endif
            CHECK(!xbox_PosixMmioFault(&synthetic, (void *)(off + XBOX_OHCI1_BASE)));
#if defined(__APPLE__)
            CHECK(!memcmp(&saved_machine, &synthetic_machine, sizeof(saved_machine)));
#else
            CHECK(!memcmp(&saved_machine, &synthetic.uc_mcontext, sizeof(saved_machine)));
#endif
            CHECK(*alias == UINT64_C(0xaabbccddeeff0011));
            CHECK(xbox_TestOhciTrapBytes(previous_trap_bytes) == 0x8000);

            atomic_store(&stop_ohci_in_probe, 1);
            allow_chain = 1;
            uint32_t unavailable = 0x24681357u;
            __asm__ volatile("ldr %w0, [%1]" : "+r"(unavailable) : "r"(ohci) : "memory");
            CHECK(atomic_load(&stopped_during_probe) && !xbox_OhciIsEnabled());
            CHECK(unavailable == 0x24681357u && chains == 2 && !context_changed);
            unavailable = 0x11223344u;
            __asm__ volatile("ldr %w0, [%1]" : "+r"(unavailable) : "r"(ohci) : "memory");
            allow_chain = 0;
            CHECK(unavailable == 0x11223344u && chains == 3 && !context_changed);
        }
#endif
        CHECK(!xbox_MemoryLayoutShutdownChecked());
        CHECK(!xbox_OhciShutdown());
        CHECK(xbox_PosixMmioSealProducers());
        CHECK(xbox_PosixMmioUnregisterThread());
#if defined(XBOXRECOMP_TEST_FAILURES)
        if (!run) {
            xbox_TestFailVirtualProtectAfter(0);
            CHECK(!xbox_OhciShutdown() && !xbox_OhciIsEnabled());
            CHECK(xbox_OhciOwnsAddress(XBOX_OHCI0_BASE));
        }
#endif
        CHECK(xbox_OhciShutdown());
        CHECK(!xbox_OhciOwnsAddress(XBOX_OHCI0_BASE));
        CHECK(xbox_PosixMmioBeginShutdown());
        CHECK(xbox_PosixMmioStop());
        CHECK(__atomic_load_n(&g_apu_state, __ATOMIC_ACQUIRE) == NULL);
#if defined(XBOXRECOMP_TEST_FAILURES)
        if (!run) {
            xbox_TestFailViewReleaseAfter(0);
            CHECK(!xbox_MemoryLayoutShutdownChecked());
            CHECK(!xbox_MemoryLayoutInit(xbe, sizeof(xbe)));
        }
#endif
        CHECK(xbox_MemoryLayoutShutdownChecked());
        CHECK(!xbox_GetMemoryBase() && !xbox_McpxMemoryAlias(0, 1));
        CHECK(!g_xbox_image_lo && !g_xbox_image_hi && !g_xbox_code_lo && !g_xbox_code_hi);
    }
    CHECK(traps >= 20);
#if defined(XBOXRECOMP_TEST_FAILURES)
    CHECK(atomic_load(&callbacks[1]) && atomic_load(&callbacks[2]) &&
          atomic_load(&callbacks[3]) && !atomic_load(&callbacks_on_producer));
#endif
    printf("Darwin memory/MMIO: %zu-byte host pages, %d device faults, restart passed\n", page, traps);
    return 0;
#endif
}
