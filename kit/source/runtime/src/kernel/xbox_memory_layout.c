/**
 * Xbox Memory Layout Implementation
 *
 * Maps the XBE data sections to their expected virtual addresses on Windows.
 * This is critical for the recompiled code which references globals by
 * absolute address (e.g., mov eax, [0x004D532C]).
 *
 * Implementation:
 * 1. VirtualAlloc a contiguous region at XBOX_BASE_ADDRESS
 * 2. Copy .rdata and initialized .data from the XBE
 * 3. Zero-fill the BSS region
 * 4. Set memory protection (read-only for .rdata)
 */

#include "xbox_memory_layout.h"
#include "kernel.h"
#include "guest_vmem.h"
#include "nv2a_gpu_memory.h"
#include "../platform/pc_cpu_topo.h"
#include "../platform/pc_switch.h"   /* T2-2: [Render] ExecWake */
#include "../platform/host_cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#if !defined(_WIN32)
#include <unistd.h>   /* _exit */
#include "../platform/posix_mmio.h"
#endif

/* XBE header field offsets (per xboxdevwiki.net/Xbe) */
#define XBE_MAGIC_OFFSET        0x0000
#define XBE_BASE_ADDR_OFFSET    0x0104
#define XBE_HEADER_SIZE_OFFSET  0x0108
#define XBE_SECTION_COUNT_OFFSET 0x011C
#define XBE_SECTION_HEADERS_OFFSET 0x0120
#define XBE_TLS_ADDR_OFFSET     0x012C

/* XBE section header layout (56 bytes each) */
#define SECTHDR_FLAGS       0x00
#define SECTHDR_VA          0x04
#define SECTHDR_VSIZE       0x08
#define SECTHDR_RAW_OFFSET  0x0C
#define SECTHDR_RAW_SIZE    0x10
#define SECTHDR_NAME_ADDR   0x14
#define SECTHDR_SIZE        56

static void *g_memory_base = NULL;
static size_t g_memory_size = 0;
static ptrdiff_t g_memory_offset = 0;  /* actual_base - XBOX_BASE_ADDRESS */
static int s_cleanup_incomplete;
static void reset_layout_allocators(void);

/* Actual mapped RAM for this run; see the header. Default retail 64 MB. */
size_t g_xbox_total_ram = XBOX_TOTAL_RAM;
size_t g_xbox_map_size = 0;   /* 0 = same as RAM */

void xbox_SetTotalRam(size_t bytes)
{
    g_xbox_total_ram = bytes;
}


void xbox_SetMapSize(size_t bytes)
{
    g_xbox_map_size = bytes;
}

/* File mapping handle for the Xbox memory region.
 * Using CreateFileMapping + MapViewOfFileEx allows mirror views to alias
 * the same physical pages as the base region, so writes to mirror addresses
 * (which wrap modulo 64 MB on real Xbox hardware) correctly modify the
 * underlying data. */
static HANDLE g_mapping_handle = NULL;

/* Mirror view pointers for cleanup */
static void *g_mirror_views[XBOX_NUM_MIRRORS] = {0};

/* The base view and its 28 mirrors occupy one contiguous span. Reserving that
 * span up front is what makes the mirrors placeable at all: each one sits at
 * base + N * 64 MB, and on a host that chose the base for us, those addresses
 * run through whatever the loader already owns. Placing them one at a time
 * means ~3 of 28 collide, and *which* three changes with ASLR. Claiming the
 * whole range first, then carving views out of ground we hold, removes the
 * question. */
static void *g_span_base = NULL;
static size_t g_span_size = 0;
#if !defined(_WIN32)
static size_t g_span_stride = 0;
static unsigned char g_span_owned[XBOX_NUM_MIRRORS + 1];

static BOOL release_span_slice(unsigned index)
{
    if (!g_span_owned[index])
        return TRUE;
    void *base = (char *)g_span_base + index * g_span_stride;
    if (!VirtualFree(base, g_span_stride, MEM_RELEASE)) {
        fprintf(stderr, "  Could not release guest reservation slice %u at %p (error %lu)\n",
                index, base, (unsigned long)GetLastError());
        return FALSE;
    }
    g_span_owned[index] = 0;
    return TRUE;
}
#endif
static void *g_tiled_view = NULL;

/* Contiguous / physical memory window (see MemoryLayoutInit).
 * XBOX_CONTIG_BASE / XBOX_CONTIG_SIZE come from kernel.h - the bridges need
 * the same numbers for MmClaimGpuInstanceMemory. */
static void *g_contig_memory = NULL;

/* NV2A GPU register aperture (see MemoryLayoutInit). Backed as plain RAM so
 * that D3D8 code linked into the title can poke it without faulting. */
#define XBOX_NV2A_BASE 0xFD000000u
#define XBOX_NV2A_SIZE (16u * 1024u * 1024u)
static void *g_nv2a_memory = NULL;

/* MCPX southbridge register span: APU 0xFE800000 through NIC 0xFEF00000. */
#define XBOX_MCPX_BASE 0xFE800000u
#define XBOX_MCPX_SIZE (8u * 1024u * 1024u)
static void *g_mcpx_memory = NULL;
#if !defined(_WIN32)
static HANDLE g_mcpx_mapping = NULL;
static void *g_mcpx_alias = NULL;
static size_t g_mcpx_host_page = 0;
#endif

void *xbox_McpxMemoryAlias(uint32_t offset, size_t bytes)
{
    if (offset >= XBOX_MCPX_SIZE || bytes > XBOX_MCPX_SIZE - offset)
        return NULL;
#if !defined(_WIN32)
    return g_mcpx_alias ? (char *)g_mcpx_alias + offset : NULL;
#else
    return g_mcpx_memory ? (char *)g_mcpx_memory + offset : NULL;
#endif
}

/* Flash ROM. The console's 256 KB flash is mirrored through the top of the
 * address space, and the MCPX span above stops one page short of it -- so a
 * title that touches it faulted on an address that is perfectly ordinary on
 * hardware.
 *
 * The Xbox Dashboard does, from two directions at once: its XIP workers hash
 * 64 KB from 0xFF000000 (it verifies archives against digests), and its render
 * path writes to 0xFF000040. Both are hard faults today, and they kill the
 * process a few dozen lines after its first frame clears.
 *
 * Plain memory, like the other two apertures, and mapped for the same stated
 * reason: a read of zero is survivable, a fault is not. Zeros are not the
 * console's BIOS, so a digest taken over this will not match one taken over
 * real flash -- that is a separate question from whether the access should
 * fault, and this is the half that has an obviously right answer. */
#define XBOX_FLASH_BASE 0xFF000000u
#define XBOX_FLASH_SIZE (1u * 1024u * 1024u)
static void *g_flash_memory = NULL;
/* The contiguous window's backing section. It is a file mapping rather than
 * plain committed memory for one reason: the tiled aperture has to be a
 * second view of the very same bytes, and only a mapping can be mapped
 * twice. See the tiled aperture below for why that matters.
 */
static HANDLE g_contig_mapping = NULL;
/* How much of the tiled aperture can exist.
 *
 * Two ceilings, both below the mapped RAM size once that is large:
 *
 *   - it starts at 0xF0000000 in a 32-bit guest address space, so it can
 *     never reach past 0x100000000; and
 *   - the NV2A register aperture sits at 0xFD000000, which is where the
 *     window really ends on hardware.
 *
 * Asking for the full RAM size overlapped both and MapViewOfFileEx failed
 * with ERROR_INVALID_ADDRESS -- a warning at startup and then a fault on the
 * title's first surface write, with nothing connecting the two. */
/* How much guest address space is mapped.
 *
 * For anything that dereferences an address it read out of guest memory --
 * a descriptor pointer a device model follows, say. Those are attacker-ish
 * input in the only sense that matters here: the title can leave one
 * uninitialised, and 0xCCCCCCCC dereferenced is a crash in the runtime
 * rather than a fault the title would have taken. */
size_t xbox_GetMappedSize(void)
{
    return g_memory_size;
}

static size_t xbox_TiledApertureSize(void)
{
    uint64_t end = XBOX_NV2A_BASE < 0x100000000ULL
                 ? XBOX_NV2A_BASE : 0x100000000ULL;
    size_t max = (size_t)(end - XBOX_TILED_BASE);
    return g_memory_size < max ? g_memory_size : max;
}

static HANDLE g_nv2a_ack_thread = NULL;
static volatile LONG g_nv2a_ack_stop = 0;

/*
 * NV2A busy-bit acknowledgement.
 *
 * D3D8 talks to the GPU through set-a-bit / wait-for-hardware-to-clear-it
 * handshakes. Against plain RAM the bit is set and nothing ever clears it, so
 * the title spins forever. Halo hangs in the push-buffer kick at 0x001EF930:
 *
 *     mov  [eax+0x100410], edx     ; set 0x10000
 *   L: test [eax+0x100410], 0x10000
 *     jne  L                       ; wait for the GPU
 *
 * Clearing those bits from a thread is not a hack around the handshake, it is
 * the handshake: on hardware the GPU clears them asynchronously, which is
 * exactly what this does. Work that would have been submitted is being done by
 * the D3D11 layer instead, so acknowledging immediately is honest.
 *
 * Only registers listed here are touched. Blanket-zeroing the aperture would
 * also wipe registers holding real state.
 *
 * ponytail: table-driven, extend as more handshakes turn up. A spin on a bit
 * that is not listed still hangs -- run the title and the watchdog sample will
 * name the register.
 */
static const struct { uint32_t offset; uint32_t busy_mask; } NV2A_ACK[] = {
    { 0x100410, 0x00010000u },  /* PFB flush kick, Halo 0x001EF930 */

    /* Interrupt status registers. These are write-1-to-clear on hardware, so
     * an ISR "clearing" one writes the pending bit back -- against plain RAM
     * that sets it instead, the interrupt stays pending forever, and the
     * service routine re-enters until the stack is gone. Halo dies exactly
     * that way: CMiniport::ServiceGrInterrupt writes 0x1000 to PGRAPH_INTR to
     * acknowledge, reads it back still pending, and recurses into a native
     * stack overflow.
     *
     * Holding them at zero is correct rather than convenient: nothing here
     * ever raises a GPU interrupt, so "none pending" is the truth. */
    { 0x000100, 0xFFFFFFFFu },  /* PMC_INTR_0    */
    { 0x001100, 0xFFFFFFFFu },  /* PBUS_INTR_0   */
    { 0x002100, 0xFFFFFFFFu },  /* PFIFO_INTR_0  */
    { 0x400100, 0xFFFFFFFFu },  /* PGRAPH_INTR   */
    { 0x600100, 0xFFFFFFFFu },  /* PCRTC_INTR_0  */
};

/*
 * Bits that must always read as SET. The mirror image of the table above:
 * where an interrupt-pending bit is false because nothing raises interrupts,
 * a queue-empty bit is true because nothing is queued.
 *
 * Halo's CMiniport::TilingUpdateIdle spins until the PFIFO caches report
 * empty (0x001F5CD1). Zeroed RAM says "not empty" forever, so tile setup
 * during CDevice::InitializeFrameBuffers never completes.
 *
 * Note 0x003220 is deliberately absent -- that one exits on the bit being
 * CLEAR, which zeroed memory already gives.
 */
static const struct { uint32_t offset; uint32_t idle_mask; } NV2A_IDLE[] = {
    { 0x002400, 0x00000010u },  /* PFIFO_RUNOUT_STATUS  LOW_MARK (empty) */
    { 0x003214, 0x00000010u },  /* PFIFO_CACHE1_STATUS  LOW_MARK (empty) */
};

/*
 * PFIFO channel DMA pointers. Software writes DMA_PUT and spins until the GPU
 * advances DMA_GET to match -- "you have consumed everything I submitted".
 * Halo's wait is at 0x001F3948:
 *
 *   L: call BusyLoop
 *      ecx = [[dev+0x2304] + 0x44]   ; DMA_GET
 *      edx = [dev]                   ; DMA_PUT
 *      test (edx ^ ecx), 0xfffffff
 *      jne L
 *
 * [dev+0x2304] is 0xFD800000, so the channel's USER area sits at aperture
 * offset 0x800000 and the two pointers are at +0x40 / +0x44. Copying PUT to
 * GET is the acknowledgement; the commands are not executed from the push
 * buffer here -- the D3D11 layer draws -- so reporting them consumed is the
 * truthful answer.
 *
 * This was written once, removed, and restored. It was removed because
 * [dev+0x2304] read as 0x0080F7FF, i.e. no register to acknowledge -- but that
 * garbage was a downstream symptom of ordinal 47 having no stdcall arg size,
 * which walked esp 8 bytes off and made D3D initialise the DMA channel with
 * `this` = 1. With that fixed the pointer is correct and so is this.
 */
#define NV2A_USER_DMA_PUT 0x800040u
#define NV2A_USER_DMA_GET 0x800044u
/* GetWake (default on, RECOMP_GPU_GET_WAKE=0 off): DMA_GET is what D3D's make-space loop waits on (nv2a_gpu_wait_word,
 * WaitOnAddress with a 2 ms timeout). Written with a plain store it never woke the waiter, so every full-pushbuffer wait
 * slept the whole timeout -- the more a busy fight submits, the more often (laptop 10-31). */
void nv2a_gpu_publish_word(volatile uint32_t *word, uint32_t value);
static void nv2a_set_get(volatile uint32_t *regs_get, uint32_t v)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_GPU_GET_WAKE"); on = !(e && *e == '0'); }
    if (on) nv2a_gpu_publish_word(regs_get, v); else *regs_get = v;
}

/*
 * The same channel's pointers on the PFIFO side of the aperture.
 *
 * The USER area above is the window software writes through; PFIFO holds the
 * engine's own copy, and D3D reads it back on the path where the USER pointer
 * is not usable. The title's channel context switch saves and restores all
 * four of these as one block (DDS9 0x002FE2xx), which is what identifies them:
 *
 *   0x3240 CACHE1_DMA_PUT          0x3248 CACHE1_REF
 *   0x3244 CACHE1_DMA_GET          0x324C CACHE1_DMA_SUBROUTINE
 *
 * DMA_SUBROUTINE matters because it is not a flag: bits 31:1 are the offset
 * the engine returns to when a pushbuffer subroutine ends, and bit 0 says
 * whether one is running. DDS9's free-space calculation (sub_002F6CC0) reads
 * the USER GET first and falls back to this register's return offset when
 * that lands outside the ring -- i.e. "the GPU is off in a subroutine, so ask
 * where it will come back to". Zeroed RAM answers 0 to both, which is below
 * the ring base, and the free-space subtraction then goes negative and is
 * clamped to zero. The reserve wants 0x2000 bytes, gets 0, and spins.
 *
 * Not acknowledged here, only reported. DDS9 reads the USER pair and never
 * reaches the fallback, so every value in this block is zero for the one
 * title that was traced -- mirroring PUT to GET would be a guess dressed as
 * a handshake. The watchdog prints them so the next title to spin here is
 * diagnosed from data instead.
 */
#define NV2A_PFIFO_DMA_PUT        0x003240u
#define NV2A_PFIFO_DMA_GET        0x003244u
#define NV2A_PFIFO_REF            0x003248u
#define NV2A_PFIFO_DMA_SUBROUTINE 0x00324Cu

/*
 * Free-running counters in the MCPX aperture.
 *
 * Some hardware registers are clocks, not flags: software reads them and waits
 * until the value passes a target. Against zeroed RAM the value never moves and
 * the wait is forever. DirectSound's CMcpxCore::SetupVoiceProcessor spins on
 * the APU sample counter at 0xFE820010 exactly this way, which is where Halo
 * stopped once input initialisation started working.
 *
 * Ticking it is the honest model: on hardware this counter advances on its own
 * whether or not anything is listening.
 *
 * ponytail: the rate is "as fast as this thread loops", not 48 kHz. Nothing
 * paces audio off it yet. Derive it from a real clock if timing starts to
 * matter.
 */
/*
 * AC'97 bus-master reset, modelled by trapping the write rather than by
 * clearing the bit afterwards.
 *
 * Each of the three DMA channels -- PCM In, PCM Out, Mic In -- has a one-byte
 * control register at NABM + 0x0B, and bit 1 is RR, "Reset Registers".
 * Software sets it and waits for the controller to clear it. DDS9 does that
 * inside DirectSoundCreate, and the wait is worth quoting because it is not a
 * poll:
 *
 *     mov  cl, [eax+0xFEC0010B]
 *     and  cl, 2
 *   L: test cl, cl
 *     jne  L
 *
 * MSVC hoisted the load out of the loop -- the pointer was not volatile -- so
 * the title reads the register exactly ONCE, a few instructions after writing
 * it, and spins forever on whatever that single read returned. On hardware
 * the reset has long completed by then.
 *
 * That rules out the NV2A_ACK approach. A thread that clears the bit
 * afterwards is racing a window a few instructions wide and gets only one
 * attempt; measured, it loses, and the title sits on a stale cl = 2 while the
 * register itself reads 0. The bit has to be clear at the moment of the read,
 * which means the write must never deposit it.
 *
 * So the page is PAGE_READONLY: reads run at full speed and see plain memory,
 * writes fault. The fault handler makes the page writable, single-steps the
 * faulting instruction, then masks RR out of the three control bytes and
 * re-protects. No instruction decoding, which matters because the write forms
 * a compiler emits here are not worth enumerating -- and being wrong about
 * one would corrupt a register rather than fail visibly.
 *
 * Only RR. Bit 0 is RPBM, run/pause bus master, which software owns.
 */
#define AC97_NABM_OFFSET  0x400000u   /* 0xFEC00000 within the MCPX aperture */
#define AC97_TRAP_BYTES   0x1000u
#define AC97_RR           0x02u

static void *g_ac97_page = NULL;      /* host address of the trapped page */
static void *g_ac97_veh  = NULL;
static RECOMP_TLS int s_ac97_stepping = 0;

#if defined(_WIN32)
static void ac97_clear_reset_bits(void)
{
    /* Every bus-master channel, not the three a PC AC'97 has.
     *
     * The generic controller has PCM In, PCM Out and Mic In at NABM +0x00,
     * +0x10 and +0x20; the MCPX has more, and DDS9 walks a table of channel
     * offsets rather than naming them. It reset the channel at +0x00 first
     * and then one at +0x60 -- which a three-entry list did not cover, so it
     * spun on the second exactly as it had on the first. Sweeping the whole
     * NABM block is both simpler and right: +0x0B is the control byte of
     * whatever channel lives there, and RR is the same bit in all of them. */
    uint32_t off;

    for (off = 0x10B; off < 0x180; off += 0x10) {
#if defined(_WIN32)
        volatile uint8_t *r = (volatile uint8_t *)((char *)g_ac97_page + off);
#else
        volatile uint8_t *r = (volatile uint8_t *)
            xbox_McpxMemoryAlias(AC97_NABM_OFFSET + off, 1);
#endif
        if (*r & AC97_RR)
            *r = (uint8_t)(*r & ~AC97_RR);
    }
}

static LONG CALLBACK ac97_write_veh(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    DWORD old;

    if (!g_ac97_page)
        return EXCEPTION_CONTINUE_SEARCH;

    /* Second half: the faulting write has now executed. Apply what the
     * controller would have done and close the page again. Thread-local,
     * because another thread must not mistake its own single-step for this
     * one -- and re-protecting from the wrong thread would strand this one
     * mid-step. */
    if (code == EXCEPTION_SINGLE_STEP && s_ac97_stepping) {
        s_ac97_stepping = 0;
        ac97_clear_reset_bits();
        VirtualProtect(g_ac97_page, AC97_TRAP_BYTES, PAGE_READONLY, &old);
        ep->ContextRecord->EFlags &= ~0x100u;   /* clear TF */
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (code == EXCEPTION_ACCESS_VIOLATION
            && ep->ExceptionRecord->ExceptionInformation[0] == 1) {
        uintptr_t fault = ep->ExceptionRecord->ExceptionInformation[1];

        if (fault >= (uintptr_t)g_ac97_page
                && fault < (uintptr_t)g_ac97_page + AC97_TRAP_BYTES) {
            if (!VirtualProtect(g_ac97_page, AC97_TRAP_BYTES,
                                PAGE_READWRITE, &old))
                return EXCEPTION_CONTINUE_SEARCH;
            s_ac97_stepping = 1;
            ep->ContextRecord->EFlags |= 0x100u;   /* TF: step the write */
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

/* Arm the trap. Called once the MCPX aperture exists, and only alongside the
 * rest of RECOMP_AC97_READY: a title that never gets as far as resetting a
 * channel has nothing to gain from it, and the page fault costs something. */
static BOOL ac97_arm_write_trap(void)
{
    DWORD old;

    if (!g_mcpx_memory || g_ac97_page)
        return FALSE;
    g_ac97_page = (char *)g_mcpx_memory + AC97_NABM_OFFSET;
#if defined(_WIN32)
    /* First, so it runs before the game target's own crash reporter, which
     * would otherwise print the write as an access violation. */
    g_ac97_veh = AddVectoredExceptionHandler(1, ac97_write_veh);
    if (!g_ac97_veh
            || !VirtualProtect(g_ac97_page, AC97_TRAP_BYTES,
                               PAGE_READONLY, &old)) {
        if (g_ac97_veh) {
            RemoveVectoredExceptionHandler(g_ac97_veh);
            g_ac97_veh = NULL;
        }
        g_ac97_page = NULL;
        fprintf(stderr, "  AC97: could not arm the bus-master write trap;"
                        " a channel reset will spin\n");
        return FALSE;
    }
#else
    if (!g_mcpx_host_page ||
        !VirtualProtect(g_ac97_page, g_mcpx_host_page, PAGE_READONLY, &old)) {
        g_ac97_page = NULL;
        fprintf(stderr, "  AC97: could not arm the bus-master write trap (error %lu)\n",
                (unsigned long)GetLastError());
        return FALSE;
    }
#endif
    fprintf(stderr, "  AC97: bus-master writes trapped at 0x%08X"
                    " (channel reset completes on write)\n",
            XBOX_MCPX_BASE + AC97_NABM_OFFSET);
    return TRUE;
}

#if defined(__aarch64__) && !defined(_WIN32)
int xbox_Ac97OwnsAddress(uint32_t va)
{
    uint32_t base = XBOX_MCPX_BASE + AC97_NABM_OFFSET;
    return g_ac97_page && va >= base && (uint64_t)va < (uint64_t)base + g_mcpx_host_page;
}

void xbox_Ac97PosixAccess(uint32_t off, uint64_t *value, unsigned size, int write)
{
    uint8_t *p = xbox_McpxMemoryAlias(off, size);
    if (!p) return;
    if (write) {
        for (unsigned i = 0; i < size; ++i) {
            uint32_t at = off + i;
            uint8_t byte = (uint8_t)(*value >> (8 * i));
            if (at >= AC97_NABM_OFFSET + 0x10bu &&
                at < AC97_NABM_OFFSET + 0x180u &&
                ((at - (AC97_NABM_OFFSET + 0x10bu)) & 0x0fu) == 0)
                byte &= (uint8_t)~AC97_RR;
            __atomic_store_n(&p[i], byte, __ATOMIC_RELAXED);
        }
    } else {
        uint64_t result = 0;
        for (unsigned i = 0; i < size; ++i)
            result |= (uint64_t)__atomic_load_n(&p[i], __ATOMIC_RELAXED) << (8 * i);
        *value = result;
    }
}

int xbox_Ac97HandleMmio(void *context, uint32_t va)
{
    if (va < XBOX_MCPX_BASE) return 0;
    return xbox_PosixMmioFaultOffset(context, va - XBOX_MCPX_BASE);
}
#endif

/*
 * Command words the DSP stub completes instantly. A bring-up probe, not a
 * model.
 *
 * The GP and EP DSPs in src/apu are stubs -- effects bypass, encode
 * passthrough -- and a stub that never completes is worse for a title than
 * one that completes at once, because the title cannot get past it at all.
 * DDS9 posts a command and waits for the DSP to clear it:
 *
 *     mov  [ebx], 3            ; ebx = scratch + 0x810
 *   L: cmp  dword [ebx], 0
 *     jne  L
 *
 * Unlike the AC'97 reset bit, this one re-reads every iteration, so clearing
 * it from here is a race this side wins rather than loses.
 *
 * Why an environment variable rather than a registration API: the word's
 * address is reached as *(*(*(this+8)+0x10)) + 0x810 from an object with no
 * global anchor, and the APU never sees that address directly -- it reaches
 * the block through the scatter-gather descriptors the title programmed. So
 * the honest fix is for the GP stub to follow those descriptors, which is DSP
 * work. This exists to answer, in one run and without that work, whether
 * completing the command is in fact all the title is waiting for.
 *
 * RECOMP_DSP_ACK=0x804A8810[,...] -- up to 8 words, zeroed whenever non-zero.
 */
#define XBOX_MAX_DSP_ACK 8
static uint32_t g_dsp_ack[XBOX_MAX_DSP_ACK];
static int g_dsp_ack_count = 0;

static void dsp_ack_init(void)
{
    const char *spec = getenv("RECOMP_DSP_ACK");
    char buf[128], *q, *end;

    if (!spec || !*spec)
        return;
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (q = buf; *q && g_dsp_ack_count < XBOX_MAX_DSP_ACK; ) {
        unsigned long va = strtoul(q, &end, 0);
        if (end == q)
            break;
        g_dsp_ack[g_dsp_ack_count++] = (uint32_t)va;
        q = (*end == ',') ? end + 1 : end;
    }
    if (g_dsp_ack_count)
        fprintf(stderr, "  DSP ack: %d command word(s) will be completed"
                        " immediately\n", g_dsp_ack_count);
}

static int fence_readable(uint32_t va, uint32_t bytes);  /* defined below */

/* Hold a guest global at a value. A bring-up probe, like the DSP ack.
 *
 * There is exactly one reason this exists: to answer "is the title waiting on
 * this?" in one run, before spending a day making the thing that would set it
 * honestly. It is not a fix and must not be mistaken for one -- whatever it
 * holds, nothing in the guest is producing, so the state it fakes is
 * inconsistent with everything downstream of it by construction.
 *
 * RECOMP_POKE=0x30F234:1,0x30F238:1
 */
#define XBOX_MAX_POKE 8
static struct { uint32_t va, value; } g_poke[XBOX_MAX_POKE];
static int g_poke_count;

static void poke_init(void)
{
    const char *spec = getenv("RECOMP_POKE");
    char buf[192], *q, *end;

    if (!spec || !*spec)
        return;
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (q = buf; *q && g_poke_count < XBOX_MAX_POKE; ) {
        unsigned long va = strtoul(q, &end, 0);
        unsigned long val = 0;
        if (end == q)
            break;
        if (*end == ':')
            val = strtoul(end + 1, &end, 0);
        g_poke[g_poke_count].va    = (uint32_t)va;
        g_poke[g_poke_count].value = (uint32_t)val;
        g_poke_count++;
        q = (*end == ',') ? end + 1 : end;
    }
    if (g_poke_count)
        fprintf(stderr, "  POKE: holding %d guest global(s) -- bring-up probe,"
                        " not a fix\n", g_poke_count);
}

static void poke_tick(void)
{
    int i;

    for (i = 0; i < g_poke_count; i++) {
        if (!fence_readable(g_poke[i].va, 4))
            continue;
        {
            volatile uint32_t *w = (volatile uint32_t *)
                ((uintptr_t)g_poke[i].va + g_memory_offset);
            if (*w != g_poke[i].value)
                *w = g_poke[i].value;
        }
    }
}

static void dsp_ack_tick(void)
{
    int i;

    for (i = 0; i < g_dsp_ack_count; i++) {
        /* fence_readable rather than a bare bounds test: these land in the
         * contiguous window, which a plain size check against the main map
         * rejects. */
        if (!fence_readable(g_dsp_ack[i], 4))
            continue;
        {
            volatile uint32_t *w = (volatile uint32_t *)
                ((uintptr_t)g_dsp_ack[i] + g_memory_offset);
            if (*w)
                *w = 0;
        }
    }
}

static const uint32_t MCPX_COUNTERS[] = {
    0x020010,   /* APU GP sample counter, DirectSound SetupVoiceProcessor */
};

static void *g_mcpx_regs = NULL;
/* Set when the APU's registers are unmapped so they can be routed to the
 * emulated APU. Once that happens they are no longer plain memory, and the
 * counter ticking below must leave them alone -- writing through the pointer
 * faults, and the emulated APU owns those registers anyway. */
static int g_apu_mmio_trapped = 0;
#if defined(__aarch64__) && !defined(_WIN32)
int xbox_ApuMmioOwnsAddress(uint32_t va)
{
    if (!g_apu_mmio_trapped || !g_mcpx_host_page || va < XBOX_MCPX_BASE)
        return 0;
    uint32_t off = va - XBOX_MCPX_BASE;
    uint32_t mask = (uint32_t)g_mcpx_host_page - 1;
    return off < 0x30000u ||
        (off >= (0x3F000u & ~mask) && off < 0x40000u) ||
        (off >= (0x5F000u & ~mask) && off < 0x60000u);
}
#endif

/*
 * GPU completion fences the title waits on in guest memory rather than in the
 * aperture. See xbox_Nv2aMirrorFence in the header for why this is the same
 * acknowledgement the NV2A_ACK table makes, and why the address has to be
 * followed through the device struct instead of being a constant.
 */
#define XBOX_MAX_FENCE_MIRRORS 4

static struct {
    uint32_t device_ptr_va;
    uint32_t put_off;
    uint32_t get_ptr_off;
} g_fence_mirrors[XBOX_MAX_FENCE_MIRRORS];
static int g_fence_mirror_count = 0;

int xbox_Nv2aMirrorFence(uint32_t device_ptr_va,
                         uint32_t put_off, uint32_t get_ptr_off)
{
    if (g_fence_mirror_count >= XBOX_MAX_FENCE_MIRRORS)
        return -1;
    g_fence_mirrors[g_fence_mirror_count].device_ptr_va = device_ptr_va;
    g_fence_mirrors[g_fence_mirror_count].put_off = put_off;
    g_fence_mirrors[g_fence_mirror_count].get_ptr_off = get_ptr_off;
    g_fence_mirror_count++;
    fprintf(stderr, "  NV2A fence mirror: device at 0x%08X,"
            " PUT +0x%X -> *(GET +0x%X)\n",
            device_ptr_va, put_off, get_ptr_off);
    return 0;
}
/* A guest address is usable only once the window is mapped and it lands
 * inside it; the chain is followed fresh every poll because the title may not
 * have built it yet. */
static int fence_readable(uint32_t va, uint32_t bytes)
{
    /* Page zero is unmapped, so the bound is the first mapped page rather than
     * just "not null": the device pointer is zero until the title creates the
     * device, and this thread polls from before that. Rejecting only 0 let
     * dev + get_ptr_off through as 0x34 and faulted on the very first tick. */
    if (g_memory_base == NULL || va < XBOX_FS_BASE)
        return 0;
    /* The contiguous window is mapped separately and sits far above the main
     * range, so a size check against g_memory_size rejects it. The fence a
     * title waits on is exactly the kind of block that lives there --
     * MmAllocateContiguousMemory is where a GPU-written semaphore comes
     * from -- so a chain ending in that window has to be followed, not
     * discarded. */
    if (va >= XBOX_CONTIG_BASE
            && (uint64_t)va + bytes <= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE)
        return g_contig_memory != NULL;
    return (size_t)va + bytes <= g_memory_size;
}

/*
 * Two counters inside the device, one of which the GPU owns.
 *
 * D3D's swap throttle is a pair: the title bumps "frames submitted" itself and
 * waits for "frames completed", which on hardware only the GPU moves. The Xbox
 * dashboard's is exactly that, at guest 0x000AF121 --
 *
 *     eax = [esi+0x2518]        ; completed
 *     ecx = [esi+0x2B60]        ; submitted
 *     ecx = ecx - eax
 *     if (ecx < 2) proceed      ; else spin on a 400-iteration delay loop
 *
 * -- and with nothing moving completed it spins there forever once two frames
 * are outstanding. That delay loop was 99.8 million of the dashboard's calls,
 * against 35 thousand for the next function down.
 *
 * This differs from xbox_Nv2aFrameCounter, which advances a counter on a 60 Hz
 * clock, in the way that matters for this pair: a free-running counter can
 * pass submitted, and then submitted - completed underflows to about four
 * billion, which is >= 2, and the spin never ends again. Mirroring cannot do
 * that, because completed is only ever whatever submitted already is.
 *
 * It is also simply true here. The pushbuffer is executed at submit, so by the
 * time the title asks whether the frame is finished, it is.
 */
#define XBOX_MAX_COUNTER_MIRRORS 4

static struct {
    uint32_t device_ptr_va;
    uint32_t src_off, dst_off;
} g_counter_mirrors[XBOX_MAX_COUNTER_MIRRORS];
static int g_counter_mirror_count = 0;

int xbox_Nv2aMirrorCounter(uint32_t device_ptr_va,
                           uint32_t src_off, uint32_t dst_off)
{
    if (g_counter_mirror_count >= XBOX_MAX_COUNTER_MIRRORS)
        return -1;
    g_counter_mirrors[g_counter_mirror_count].device_ptr_va = device_ptr_va;
    g_counter_mirrors[g_counter_mirror_count].src_off = src_off;
    g_counter_mirrors[g_counter_mirror_count].dst_off = dst_off;
    g_counter_mirror_count++;
    fprintf(stderr, "  NV2A counter mirror: device at 0x%08X,"
            " +0x%X -> +0x%X\n", device_ptr_va, src_off, dst_off);
    return 0;
}

static void counter_mirrors_tick(void)
{
    for (int i = 0; i < g_counter_mirror_count; i++) {
        uint32_t dev;

        if (!fence_readable(g_counter_mirrors[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_counter_mirrors[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_counter_mirrors[i].src_off, 4)
                || !fence_readable(dev + g_counter_mirrors[i].dst_off, 4))
            continue;
        {
            volatile uint32_t *dst =
                (volatile uint32_t *)((uintptr_t)(dev + g_counter_mirrors[i].dst_off)
                                      + g_memory_offset);
            uint32_t src =
                *(volatile uint32_t *)((uintptr_t)(dev + g_counter_mirrors[i].src_off)
                                       + g_memory_offset);
            if (*dst != src)
                *dst = src;
        }
    }
}

/*
 * Frame counters the title polls to pace itself.
 *
 * D3D keeps a swap count inside the device and bumps it once per presented
 * frame; a title that wants to wait a frame reads it and spins until it moves.
 * Wreckless does exactly that at guest 0x000DC5E0 -- "loop while the counter
 * has advanced by less than 2" -- so a counter that never moves is not a
 * dropped frame, it is a hang with a full asset load behind it.
 *
 * Nothing here presents, so nothing would ever move it. Advancing it on a
 * clock is what makes the wait terminate, and 60 Hz is the rate the title
 * expects the display to run at. Followed through the device pointer for the
 * same reason the fence is: the device is allocated at runtime.
 */
#define XBOX_MAX_FRAME_COUNTERS 4
#define XBOX_FRAME_PERIOD_MS    16      /* ~60 Hz */

static struct {
    uint32_t device_ptr_va;
    uint32_t counter_off;
} g_frame_counters[XBOX_MAX_FRAME_COUNTERS];
static int   g_frame_counter_count = 0;
static DWORD g_frame_counter_last_ms = 0;

int xbox_Nv2aFrameCounter(uint32_t device_ptr_va, uint32_t counter_off)
{
    if (g_frame_counter_count >= XBOX_MAX_FRAME_COUNTERS)
        return -1;
    g_frame_counters[g_frame_counter_count].device_ptr_va = device_ptr_va;
    g_frame_counters[g_frame_counter_count].counter_off   = counter_off;
    g_frame_counter_count++;
    fprintf(stderr, "  Frame counter: device at 0x%08X, count +0x%X @ %d Hz\n",
            device_ptr_va, counter_off, 1000 / XBOX_FRAME_PERIOD_MS);
    return 0;
}

/* A real swap happened: advance every registered counter, and remember when.
 *
 * The timer below exists for a title nothing presents for. Once the
 * pushbuffer executor is actually running flips, the timer is the wrong
 * clock and an actively harmful one: Half-Life 2's loader paces its intro on
 * this count, so a 62 Hz timer against an executor managing a fraction of a
 * frame per second ran the video forward in virtual time far faster than it
 * could be drawn. Only every few hundredth frame was ever presented, each one
 * sampled part way through its own decode -- which looks exactly like a
 * stalling, blocky video rather than a clock running away.
 */
static DWORD g_frame_counter_flip_ms;

void xbox_Nv2aFrameCounterFlip(void)
{
    int i;

    g_frame_counter_flip_ms = GetTickCount();
    if (!g_frame_counter_flip_ms)
        g_frame_counter_flip_ms = 1;          /* 0 means "never" */
    for (i = 0; i < g_frame_counter_count; i++) {
        uint32_t dev;

        if (!fence_readable(g_frame_counters[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_frame_counters[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_frame_counters[i].counter_off, 4))
            continue;
        *(volatile uint32_t *)((uintptr_t)(dev + g_frame_counters[i].counter_off)
                               + g_memory_offset) += 1;
    }
}

static void frame_counters_tick(void)
{
    DWORD now = GetTickCount();
    int i;

    if (!g_frame_counter_count)
        return;
    if (g_frame_counter_last_ms
            && (now - g_frame_counter_last_ms) < XBOX_FRAME_PERIOD_MS)
        return;
    /* Something is presenting: let it drive the count instead. Two seconds,
     * because the executor's flips are not evenly spaced and a title that
     * genuinely stops presenting still has to be got moving again. */
    if (g_frame_counter_flip_ms && (now - g_frame_counter_flip_ms) < 2000) {
        g_frame_counter_last_ms = now;
        return;
    }
    g_frame_counter_last_ms = now;

    for (i = 0; i < g_frame_counter_count; i++) {
        uint32_t dev;

        if (!fence_readable(g_frame_counters[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_frame_counters[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_frame_counters[i].counter_off, 4))
            continue;
        *(volatile uint32_t *)((uintptr_t)(dev + g_frame_counters[i].counter_off)
                               + g_memory_offset) += 1;
    }
}

static void fence_mirrors_tick(void)
{
    for (int i = 0; i < g_fence_mirror_count; i++) {
        uint32_t dev, get_ptr;

        if (!fence_readable(g_fence_mirrors[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_fence_mirrors[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_fence_mirrors[i].get_ptr_off, 4)
                || !fence_readable(dev + g_fence_mirrors[i].put_off, 4))
            continue;
        get_ptr = *(volatile uint32_t *)((uintptr_t)(dev + g_fence_mirrors[i].get_ptr_off)
                                         + g_memory_offset);
        if (!fence_readable(get_ptr, 4))
            continue;
        {
            volatile uint32_t *fence =
                (volatile uint32_t *)((uintptr_t)get_ptr + g_memory_offset);
            uint32_t put =
                *(volatile uint32_t *)((uintptr_t)(dev + g_fence_mirrors[i].put_off)
                                       + g_memory_offset);
            if (*fence != put)
                *fence = put;
        }
    }
}

static int s_nv2a_trace = 0;

/* The display framebuffer, as reported by AvSetDisplayMode. Checksummed once a
 * second so a run can answer the only question that matters before building a
 * presenter: is the guest putting pixels anywhere at all, and do they change
 * from frame to frame. */
static uint32_t s_fb_va, s_fb_pitch, s_fb_height = 480;

void xbox_SetDisplayFramebuffer(uint32_t fb_va, uint32_t pitch)
{
    s_fb_va = fb_va;
    s_fb_pitch = pitch;
}

/* Where the title last said its framebuffer is, for anything that wants to read
 * guest pixels directly. There was only a setter, so every such reader carried
 * its own hardcoded address instead -- and a title that moves its framebuffer
 * (which is most of them, once it owns one) left that constant pointing at
 * uninitialised memory. A dump taken there is not empty, it is noise, which
 * reads as "the title drew garbage" rather than "you read the wrong page".
 *
 * This is the resolved address, not the physical one AvSetDisplayMode states:
 * the caller resolves before storing, because a physical framebuffer address
 * read directly lands in the loaded image. Getting that wrong is the same bug
 * twice over -- once in the probe below, once in a caller of this. */
uint32_t xbox_GetDisplayFramebuffer(uint32_t *pitch)
{
    if (pitch)
        *pitch = s_fb_pitch;
    return s_fb_va;
}

static void framebuffer_probe_tick(void)
{
    static DWORD last_ms;
    static uint32_t last_sum;
    DWORD now = GetTickCount();
    uint32_t sum = 0, nonzero = 0, i, n;
    const uint32_t *p;

    if (!s_nv2a_trace || !s_fb_va || !s_fb_pitch)
        return;
    if (last_ms && (now - last_ms) < 1000)
        return;
    last_ms = now;
    if ((size_t)s_fb_va + s_fb_pitch * s_fb_height > g_memory_size)
        return;
    p = (const uint32_t *)((uintptr_t)s_fb_va + g_memory_offset);
    n = (s_fb_pitch * s_fb_height) / 4;
    for (i = 0; i < n; i++) {
        sum = sum * 33u + p[i];
        if (p[i]) nonzero++;
    }
    fprintf(stderr, "  [FB] 0x%08X sum=%08X nonzero=%u/%u %s\n",
            s_fb_va, sum, nonzero, n,
            sum != last_sum ? "CHANGED" : "same");
    last_sum = sum;
    fflush(stderr);
}

/* This thread is also the GPU: it executes the pushbuffer. It used to spin with Sleep(0) all
 * the time, a full core even while the title was busy on the CPU and submitting nothing. On a
 * laptop that core is power the game thread and the integrated GPU share, and with
 * Hyper-Threading it can be the game thread's own core. Now it spins only while there is GPU
 * traffic (PUT moving, busy bits being set) and for 2 ms after; then it polls every ~0.25 ms on
 * a high-resolution timer until traffic resumes. RECOMP_NV2A_SPIN=1: always spin, as before. */

/* T2-2 E2a: executor wake without the Sleep(0) spin. [Render] ExecWake / RECOMP_NV2A_WAKE:
 *   sleep0 (0): the old policy above, unchanged.
 *   poll   (1): inside the 2 ms traffic window, poll PUT, the busy/idle bits and the DSP ack words
 *               with _mm_pause for up to RECOMP_NV2A_ACTIVE_US (default 20 us), then wait on the same
 *               250 us high-resolution timer the idle path uses; Sleep(1) if arming it fails.
 *   auto   (2, default since the T2-2 flip): poll where cores are spare (12 or more logical CPUs), else exactly sleep0.
 *               Iteration 1 polled at 4 logical CPUs too and raised UC p99 +9 %: the timer delayed
 *               each kick's pickup, while Sleep(0) there already yields straight to the game thread.
 * In the traffic window the poll's timer tail is RECOMP_NV2A_WINDOW_US / [Render] ExecWindowUs
 * (100 us, 50-250; the title kicks about 5 times a frame); outside it, the idle 250 us as before.
 * Invariant: the loop still ticks at least every 250 us (busy-bit ack, DSP ack, poke, counter
 * mirrors, MCPX counters), and returns at once when any input the tick serves changes.
 * RECOMP_NV2A_SPIN=1 keeps its meaning (always Sleep(0)). */
static int s_exec_wake = -1, s_exec_active_us, s_exec_window_us = 250;
static uint32_t s_exec_seen_put;          /* PUT as the last tick saw it */
static struct { uint64_t polls, poll_hits, timers, timer_fail, sleep0; } s_wake;
static int nv2a_wake_pending(volatile uint32_t *regs)
{
    if (*(volatile uint32_t *)((char *)regs + NV2A_USER_DMA_PUT) != s_exec_seen_put) return 1;
    for (size_t i = 0; i < sizeof(NV2A_ACK) / sizeof(NV2A_ACK[0]); i++)
        if (*(volatile uint32_t *)((char *)regs + NV2A_ACK[i].offset) & NV2A_ACK[i].busy_mask) return 1;
    for (size_t i = 0; i < sizeof(NV2A_IDLE) / sizeof(NV2A_IDLE[0]); i++) {
        uint32_t m = NV2A_IDLE[i].idle_mask;
        if ((*(volatile uint32_t *)((char *)regs + NV2A_IDLE[i].offset) & m) != m) return 1;
    }
    for (int i = 0; i < g_dsp_ack_count; i++)
        if (fence_readable(g_dsp_ack[i], 4) &&
            *(volatile uint32_t *)((uintptr_t)g_dsp_ack[i] + g_memory_offset)) return 1;
    return 0;
}
static void nv2a_wake_init(void)
{
    static const char *const names[] = { "sleep0", "poll", "auto" };
    unsigned logical = pc_cpu_logical();
    s_exec_wake = pc_switch_choice("RECOMP_NV2A_WAKE", "Render", "ExecWake", names, 3, 2);
    if (s_exec_wake < 0 || s_exec_wake > 2) s_exec_wake = 0;
    if (s_exec_wake == 2 && (!logical || logical < 12)) {
        fprintf(stderr, "[EXEC-WAKE] auto -> sleep0 (logical=%u < 12)\n", logical);
        s_exec_wake = 0;   /* the default path, unchanged */
    }
    s_exec_active_us = pc_switch("RECOMP_NV2A_ACTIVE_US", "Render", "ExecActiveUs", 20);
    if (s_exec_active_us < 0) s_exec_active_us = 0;
    if (s_exec_active_us > 2000) s_exec_active_us = 2000;
    s_exec_window_us = pc_switch("RECOMP_NV2A_WINDOW_US", "Render", "ExecWindowUs", 100);
    if (s_exec_window_us < 50) s_exec_window_us = 50;
    if (s_exec_window_us > 250) s_exec_window_us = 250;
    fprintf(stderr, "[EXEC-WAKE] window_us=%d\n", s_exec_window_us);
    fprintf(stderr, "[EXEC-WAKE] mode=%s active_us=%d logical=%u\n", names[s_exec_wake], s_exec_active_us, logical);
}
static void nv2a_wake_report(double secs)
{
    static uint64_t p[5];
    uint64_t *c = (uint64_t *)&s_wake;
    fprintf(stderr, "[EXEC-WAKE] %.1f s: mode=%d polls/s=%.0f poll_wakes/s=%.0f timers/s=%.0f timer_fail=%llu sleep0/s=%.0f\n",
            secs, s_exec_wake, (c[0] - p[0]) / secs, (c[1] - p[1]) / secs, (c[2] - p[2]) / secs,
            (unsigned long long)(c[3] - p[3]), (c[4] - p[4]) / secs);
    for (int i = 0; i < 5; i++) p[i] = c[i];
}

/* T2-0 R0: executor counters (RECOMP_PERF), the pl-prof [PROF-EXEC]/[PROF-WORDS] lines without
 * pl_prof.c. Everything below runs on the executor thread only (consume, draws, present and this
 * reporter), so plain counters suffice. Off: one test of a cached int per bracket. Slots:
 * 0 consume 1 upload 2 draw 3 clear 4 present 5 spin 6 timer 7 kicks. Words: see nv2a_pb_exec.c. */
int g_nv2a_prof = -1;
uint64_t g_nv2a_prof_words[10];
static struct { int64_t ticks[8]; uint64_t n[8]; uint64_t flips; LARGE_INTEGER freq; } s_nv2a_prof;
int64_t nv2a_prof_ticks(void)
{
    LARGE_INTEGER t;
    if (g_nv2a_prof <= 0) return 0;
    QueryPerformanceCounter(&t);
    return t.QuadPart ? t.QuadPart : 1;
}
void nv2a_prof_acc(int slot, int64_t start)
{
    LARGE_INTEGER t;
    if (!start || slot < 0 || slot > 7) return;
    QueryPerformanceCounter(&t);
    s_nv2a_prof.ticks[slot] += t.QuadPart - start;
    s_nv2a_prof.n[slot]++;
}
void nv2a_prof_count(int slot) { if (g_nv2a_prof > 0 && slot >= 0 && slot <= 7) s_nv2a_prof.n[slot]++; }
void nv2a_prof_flip(void) { if (g_nv2a_prof > 0) s_nv2a_prof.flips++; }
static void nv2a_prof_report(void)
{
    static DWORD last_ms;
    static int64_t pt[8]; static uint64_t pn[8], pf, pw[10];
    DWORD now;
    if (g_nv2a_prof <= 0) return;
    now = GetTickCount();
    if (!last_ms) { last_ms = now; return; }
    if (now - last_ms < 5000) return;
    last_ms = now;
    uint64_t f = s_nv2a_prof.flips - pf;
    double fd = f ? (double)f : 1.0, tk = (double)s_nv2a_prof.freq.QuadPart / 1000.0;
    static const char *const nm[7] = {"consume", "upload", "draw", "clear", "present", "spin", "timer"};
    char line[640]; int o = snprintf(line, sizeof line, "[PROF-EXEC] per_frame_ms");
    for (int i = 0; i < 7; i++)
        o += snprintf(line + o, sizeof line - o, " %s=%.3f(n/f=%.1f)", nm[i],
                      (double)(s_nv2a_prof.ticks[i] - pt[i]) / tk / fd, (double)(s_nv2a_prof.n[i] - pn[i]) / fd);
    snprintf(line + o, sizeof line - o, " kicks/f=%.1f flips=%llu", (double)(s_nv2a_prof.n[7] - pn[7]) / fd,
             (unsigned long long)f);
    fprintf(stderr, "%s\n", line);
    static const char *const wn[10] = {"idx16", "idx32", "drawarr", "inline", "immvtx", "tex", "const", "sema", "state", "other_subch"};
    uint64_t total = 0;
    o = snprintf(line, sizeof line, "[PROF-WORDS] per_frame");
    for (int i = 0; i < 10; i++) {
        uint64_t d = g_nv2a_prof_words[i] - pw[i]; total += d; pw[i] = g_nv2a_prof_words[i];
        o += snprintf(line + o, sizeof line - o, " %s=%.0f", wn[i], (double)d / fd);
    }
    snprintf(line + o, sizeof line - o, " total=%.0f", (double)total / fd);
    fprintf(stderr, "%s\n", line);
    for (int i = 0; i < 8; i++) { pt[i] = s_nv2a_prof.ticks[i]; pn[i] = s_nv2a_prof.n[i]; }
    nv2a_wake_report(5.0);
    pf = s_nv2a_prof.flips;
}

static void nv2a_idle_wait(int active, volatile uint32_t *regs)
{
    static int mode = -1;
    static HANDLE timer;
    static LARGE_INTEGER freq, last;
    LARGE_INTEGER now;

    if (mode < 0) {
        const char *e = getenv("RECOMP_NV2A_SPIN");
        mode = e && *e == '1' ? 0 : 1;
        QueryPerformanceFrequency(&freq);
        if (mode) {
#if defined(_WIN32)
            timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
#else
            timer = CreateWaitableTimerW(NULL, FALSE, NULL);
#endif
            if (!timer) mode = 0;
        }
    }
    if (s_exec_wake < 0) nv2a_wake_init();
    QueryPerformanceCounter(&now);
    if (active || !last.QuadPart) last = now;
    int window = (now.QuadPart - last.QuadPart) * 1000 < freq.QuadPart * 2;
    if (!mode || (window && !s_exec_wake)) {
        int64_t pt = nv2a_prof_ticks();
        Sleep(0);  /* yield; the waiter is spinning on another core */
        nv2a_prof_acc(5 /* spin */, pt);
        s_wake.sleep0++;
        return;
    }
    if (window && s_exec_active_us) {
        /* T2-2 poll: _mm_pause, re-reading the tick's inputs every 64 pauses, for ACTIVE_US. */
        int64_t pt = nv2a_prof_ticks();
        LONGLONG deadline = now.QuadPart + freq.QuadPart * s_exec_active_us / 1000000;
        s_wake.polls++;
        for (;;) {
            LARGE_INTEGER t;
            for (int k = 0; k < 64; k++) xbox_host_cpu_relax();
            if (nv2a_wake_pending(regs)) { s_wake.poll_hits++; nv2a_prof_acc(5 /* spin */, pt); return; }
            QueryPerformanceCounter(&t);
            if (t.QuadPart >= deadline) break;
        }
        nv2a_prof_acc(5 /* spin */, pt);
    }
    {
        LARGE_INTEGER due;
        int64_t pt = nv2a_prof_ticks();
        /* 250 us, relative; T2-2: the shorter tail inside the traffic window, poll modes only */
        due.QuadPart = window && s_exec_wake ? -10LL * s_exec_window_us : -2500;
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(timer, 5);
            s_wake.timers++;
        } else {
            s_wake.timer_fail++;
            Sleep(s_exec_wake ? 1 : 0);   /* T2-2: never a spin when polling was asked for */
        }
        nv2a_prof_acc(6 /* timer */, pt);
    }
}

static DWORD WINAPI nv2a_ack_thread(LPVOID param)
{
    volatile uint32_t *regs = (volatile uint32_t *)param;
    pc_thread_role_enter(PC_ROLE_EXECUTOR);
    QueryPerformanceFrequency(&s_nv2a_prof.freq);
    g_nv2a_prof = getenv("RECOMP_PERF") != NULL;
    while (!InterlockedCompareExchange(&g_nv2a_ack_stop, 0, 0)) {
        int active = 0;
        for (size_t i = 0; i < sizeof(NV2A_ACK) / sizeof(NV2A_ACK[0]); i++) {
            volatile uint32_t *r =
                (volatile uint32_t *)((char *)regs + NV2A_ACK[i].offset);
            if (*r & NV2A_ACK[i].busy_mask) {
                *r &= ~NV2A_ACK[i].busy_mask;
                active = 1;
            }
        }
        for (size_t i = 0; i < sizeof(NV2A_IDLE) / sizeof(NV2A_IDLE[0]); i++) {
            volatile uint32_t *r =
                (volatile uint32_t *)((char *)regs + NV2A_IDLE[i].offset);
            if ((*r & NV2A_IDLE[i].idle_mask) != NV2A_IDLE[i].idle_mask) {
                *r |= NV2A_IDLE[i].idle_mask;
                active = 1;
            }
        }
        /* DMA_GET used to be set to DMA_PUT here, at the top of the tick,
         * before the scan below had executed anything.
         *
         * GET is what tells the title how far the GPU has consumed, and D3D
         * waits on it before reusing the ring. Reporting "all consumed"
         * while the commands were still unread gave the title permission to
         * overwrite them, and it took it: the executor then read whatever
         * part of the segment had survived, so each pass drew a different
         * subset of the frame. It looks like unstable geometry and is a
         * lost-command race.
         *
         * The advance now happens after the scan, further down, which is
         * also the only ordering that gives the title real back-pressure. */
        /* With an executor, only an ordered semaphore command completes a
         * fence. Mirroring the producer here permits unread buffer reuse. */
        static int execute = -1;
        if (execute < 0) execute = getenv("RECOMP_PB_EXEC") != NULL;
        if (!execute) fence_mirrors_tick();
        dsp_ack_tick();
        poke_tick();
        counter_mirrors_tick();
        frame_counters_tick();
        framebuffer_probe_tick();

        /* Which framebuffer the display would be scanning out.
         *
         * PCRTC_START holds the address the CRTC reads pixels from, so
         * whatever the title last set there is the frame it believes is on
         * screen. Nothing here scans out, so this is the one place that says
         * whether the guest is producing an image at all -- and where it is.
         * Gated, because it is a bring-up question, not a runtime one. */
        /* Not gated on the trace flag: nv2a_pb_scan is what drives the
         * executor, and it already returns unless RECOMP_PB_SCAN or
         * RECOMP_PB_EXEC asked for it. Gating the call as well meant
         * RECOMP_PB_EXEC on its own did nothing at all, and the executor
         * only ran when someone happened to also be tracing. */
        {
            /* Is the title submitting GPU work at all? PUT is where the
             * title's pushbuffer writer has got to; if it never moves, nothing
             * is being drawn and the missing piece is upstream of the GPU. */
            static DWORD  last_put_ms;
            static uint32_t last_put;
            DWORD now_ms = GetTickCount();
            uint32_t put = *(volatile uint32_t *)((char *)regs + NV2A_USER_DMA_PUT);
            s_exec_seen_put = put;
            if (put != last_put)
                active = 1;
            if (put != last_put || (now_ms - last_put_ms) > 2000) {
                /* Survey the segment the title just submitted, once. */
                {
                    extern uint32_t nv2a_pb_consume(uint32_t, uint32_t);
                    extern void nv2a_pb_scan_report(void);
                    static DWORD last_report;

                    /* DMA_PUT holds a PHYSICAL address -- Xbox D3D writes
                     * `VA & 0x0FFFFFFF` and reads the GPU's position back as
                     * `GET | 0x80000000`. nv2a_pb_scan reads guest VAs, so
                     * handing it the raw register value pointed it at low
                     * memory: for the Xbox Dashboard, whose pushbuffer is at
                     * 0x80001000, PUT reads 0x1000 and the survey walked the
                     * fake TIB. It reported a plausible-looking inventory of
                     * nothing, which is worse than reporting none -- the
                     * conclusion drawn was "the title submits no methods"
                     * while it was submitting them the whole time.
                     *
                     * The contiguous window IS the physical-address view, so
                     * OR-ing its base is the documented round trip, not a
                     * guess. */
                    /* The pushbuffer is a ring, so PUT coming back below
                     * where it was is a wrap, not a rewind. Scanning only
                     * forward segments dropped everything written across
                     * the seam -- one whole submission each time round.
                     *
                     * The ring's bounds are not published anywhere this
                     * code can read, so they are learned: the lowest and
                     * highest PUT seen bracket it. That is approximate on
                     * the first lap and exact afterwards, and scanning a
                     * little short of the true end costs the same commands
                     * that were being lost anyway. */
                    uint32_t get = *(volatile uint32_t *)((char *)regs + NV2A_USER_DMA_GET);
                    if (!get) get = last_put ? last_put : put;
                    if (get != put) {
                        extern void nv2a_pb_set_get_register(volatile uint32_t *);
                        nv2a_pb_set_get_register((volatile uint32_t *)((char *)regs + NV2A_USER_DMA_GET));
                        int64_t pt = nv2a_prof_ticks();
                        nv2a_prof_count(7 /* kicks */);
                        uint32_t consumed = nv2a_pb_consume(
                            XBOX_CONTIG_BASE | (get & 0x0FFFFFFFu),
                            XBOX_CONTIG_BASE | (put & 0x0FFFFFFFu));
                        nv2a_set_get((volatile uint32_t *)((char *)regs + NV2A_USER_DMA_GET), consumed & 0x0FFFFFFFu);
                        nv2a_prof_acc(0 /* consume */, pt);
                    } else {
                        nv2a_set_get((volatile uint32_t *)((char *)regs + NV2A_USER_DMA_GET), get);
                    }
                    /* Periodic, because what the title submits at init is not
                     * what it submits once it is drawing a menu, and the
                     * question the survey answers is about the latter. */
                    /* RECOMP_PB_REPORT_MS shortens it: the report is also
                     * when RECOMP_FB_DUMP writes a frame, and stepping a
                     * scripted pad through a menu needs a picture per
                     * press rather than one every ten seconds. */
                    static long report_ms = -1;
                    if (report_ms < 0) {
                        const char *e = getenv("RECOMP_PB_REPORT_MS");
                        report_ms = e ? atol(e) : 10000;
                        if (report_ms < 100) report_ms = 100;
                    }
                    if (s_nv2a_trace && now_ms - last_report > (uint64_t)report_ms) {
                        last_report = now_ms;
                        nv2a_pb_scan_report();
                    }
                }
                /* Consumed, now that it has actually been executed. */
                {
                    volatile uint32_t *get =
                        (volatile uint32_t *)((char *)regs
                                              + NV2A_USER_DMA_GET);
                    /* nv2a_pb_consume already published its actual position. */
                    (void)get;
                }
                last_put = put; last_put_ms = now_ms;
                /* GET as well as PUT. A title that stops submitting has either
                 * finished or is spinning on the GPU catching up, and only GET
                 * tells those apart -- D3D waits for GET to reach PUT before it
                 * reuses the buffer, so GET stuck behind PUT is the shape of a
                 * pushbuffer-full hang. Also show the same pair as the Xbox
                 * Dashboard reads them: its D3D holds a register-block pointer
                 * in its device struct rather than assuming 0xFD800000, and
                 * mirroring the wrong block leaves it spinning on a GET that
                 * never moves. */
                if (s_nv2a_trace) {
                    uint32_t g = *(volatile uint32_t *)
                                 ((char *)regs + NV2A_USER_DMA_GET);
                    fprintf(stderr, "  [NV2A] DMA_PUT = 0x%08X  DMA_GET = "
                            "0x%08X%s\n", put, g,
                            g == put ? "" : "  (GPU behind)");
                }
                if (s_nv2a_trace || s_exec_wake <= 0) fflush(stderr);   /* T2-2: no per-kick flush when polling */
            }
        }
        if (s_nv2a_trace) {
            static uint32_t last_start = 0xFFFFFFFFu;
            uint32_t start = *(volatile uint32_t *)((char *)regs + 0x600800);
            if (start != last_start) {
                last_start = start;
                fprintf(stderr, "  [NV2A] PCRTC_START = 0x%08X\n", start);
                fflush(stderr);
            }
        }

        if (g_mcpx_regs && !g_apu_mmio_trapped) {
            for (size_t i = 0; i < sizeof(MCPX_COUNTERS) / sizeof(MCPX_COUNTERS[0]); i++) {
                volatile uint32_t *c =
                    (volatile uint32_t *)((char *)g_mcpx_regs + MCPX_COUNTERS[i]);
                *c += 1;
            }
        }

        nv2a_prof_report();
        nv2a_idle_wait(active, regs);
    }
    pc_thread_role_leave();
    return 0;
}

static int xbox_Nv2aAckStart(void)
{
    dsp_ack_init();
    poke_init();
    g_nv2a_ack_stop = 0;
    g_nv2a_ack_thread = CreateThread(NULL, 0, nv2a_ack_thread,
                                     g_nv2a_memory, 0, NULL);
    if (g_nv2a_ack_thread) {
        fprintf(stderr, "  NV2A busy-bit ack: %zu register(s) acknowledged\n",
                sizeof(NV2A_ACK) / sizeof(NV2A_ACK[0]));
        return 1;
    }
    fprintf(stderr, "  NV2A busy-bit ack: could not start worker (error %lu)\n",
            (unsigned long)GetLastError());
    return 0;
}

/* Separate allocation for Xbox kernel address space (0x80010000+).
 * Some RenderWare code reads the kernel PE header to detect features. */
static void *g_kernel_memory = NULL;

/* Global offset accessible by recompiled code (via recomp_types.h) */
ptrdiff_t g_xbox_mem_offset = 0;

/* Bounds of the title's executable sections, from its own XBE section table.
 *
 * RECOMP_ICALL uses these to decide whether an indirect-call target is code
 * before dispatching it. This used to be a hardcoded "0x00400000..0xFE000000 is
 * not code" test, which is true for Burnout 3 -- its .text ends at 0x002CC200,
 * so everything above 0x400000 really is data -- and false for any title with
 * more code than that. Half-Life 2's .text runs to 0x005F4A6C, so the constant
 * silently discarded every indirect call into the top two thirds of the game,
 * including the one that enters its main. No log, no crash: eax = 0 and carry
 * on, which looks exactly like a function that returned early.
 *
 * Zero until the layout is initialised, which the macro treats as "allow" so
 * nothing breaks before the title is loaded. */
uint32_t g_xbox_image_lo = 0;
uint32_t g_xbox_image_hi = 0;
uint32_t g_xbox_code_lo = 0;
uint32_t g_xbox_code_hi = 0;

/* Global registers for recompiled code (via recomp_types.h) */
/* Each guest thread's TIB. The first thread uses the one the loader built;
 * a spawned thread gets its own from xbox_AllocThreadTib(). */
RECOMP_TLS uint32_t g_fs_base = XBOX_TIB_MAIN;

/* Keep an immutable initial image: the main thread may change its own TLS
 * before another guest thread starts. */
static uint8_t *g_tls_template;
static uint32_t g_tls_total, g_tls_thread_size = 64;
static uint8_t g_tib_template[0x40];

RECOMP_TLS uint32_t g_eax = 0, g_ecx = 0, g_edx = 0, g_esp = 0;
RECOMP_TLS uint32_t g_ebx = 0, g_esi = 0, g_edi = 0;

#ifdef RECOMP_ABI_CHECK
/* Report a lifted function that returned without restoring ebx/esi/edi.
 *
 * Those are callee-saved on x86, and the recompiler keeps them in globals, so
 * a function whose epilogue was never lifted corrupts its caller rather than
 * itself -- an error with no crash and no message, just less work silently
 * done. Ranked by hit count so the routine breaking a hot loop stands out from
 * the one-offs; -DRECOMP_ABI_CHECK only, since it costs three compares on
 * every indirect call.
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;

void recomp_abi_violation_log(uint32_t va, uint32_t ebx0, uint32_t esi0,
                              uint32_t edi0, uint32_t esp0)
{
    enum { SLOTS = 32 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
        fprintf(stderr, "[ABI] sub_%08X:%s%s%s%s\n"
                        "      ebx %08X->%08X esi %08X->%08X"
                        " edi %08X->%08X esp %08X->%08X\n",
                va,
                g_ebx != ebx0 ? " ebx" : "",
                g_esi != esi0 ? " esi" : "",
                g_edi != edi0 ? " edi" : "",
                g_esp < esp0 + 4 ? " esp(epilogue never ran)" : "",
                ebx0, g_ebx, esi0, g_esi, edi0, g_edi, esp0, g_esp);
        /* esp coming back too HIGH means some callee popped arguments that
         * were never pushed -- a convention mismatch the one-sided invariant
         * above cannot see. The most recent indirect targets are the usual
         * suspects, so name them. */
        {
            int t;
            fprintf(stderr, "      esp delta %+d, recent icall targets:",
                    (int)(g_esp - esp0));
            for (t = 4; t >= 1; t--)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx - t) & 15]);
            fputc('\n', stderr);
        }
        fflush(stderr);
    }
    hits[i]++;
}
#endif

/* SEH frame pointer bridge (see recomp_types.h for explanation) */
RECOMP_TLS uint32_t g_seh_ebp = 0;
RECOMP_TLS double g_fp_stack[8];
RECOMP_TLS int g_fp_top = 0;

/* Set once at startup. The generated code reads it at the ret of every
 * --force-return function, so it has to be cheap and it has to default to
 * off: a build carrying forced functions behaves normally until the
 * variable is set. */
int g_force_return = 0;
/* x87 control and status. The reset default masks every exception and
 * rounds to nearest, which is what the CRT expects before _control87. */
RECOMP_TLS uint16_t g_fp_control_word = 0x037Fu;
RECOMP_TLS int g_fp_cmp = 0;
RECOMP_TLS uint16_t g_fp_cc = 0x4000;

/* Defined below, with the other guest registers. */
extern RECOMP_TLS uint32_t g_ebp;
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi;

/* ---- non-local jumps ---------------------------------------------------
 *
 * The native half of the guest's setjmp/longjmp. See recomp_types.h for why a
 * guest-only longjmp is not enough; in short, the recompiled frames are C
 * frames and something has to unwind them.
 *
 * Keyed by guest buffer address, per thread. Buffers nest, so jumping to an
 * outer one discards every inner entry -- those frames are gone.
 */
#define RECOMP_JMPBUF_SLOTS 32

typedef struct {
    uint32_t buf_va;
    jmp_buf  native;
} recomp_jmp_slot;

static RECOMP_TLS recomp_jmp_slot s_jmp[RECOMP_JMPBUF_SLOTS];
static RECOMP_TLS int             s_jmp_used;

jmp_buf *recomp_setjmp_slot(uint32_t buf_va)
{
    int i;

    for (i = 0; i < s_jmp_used; i++)
        if (s_jmp[i].buf_va == buf_va)
            return &s_jmp[i].native;      /* the same buffer, re-armed */
    if (s_jmp_used >= RECOMP_JMPBUF_SLOTS)
        s_jmp_used = RECOMP_JMPBUF_SLOTS - 1;   /* keep the deepest */
    s_jmp[s_jmp_used].buf_va = buf_va;
    return &s_jmp[s_jmp_used++].native;
}

int recomp_guest_longjmp(uint32_t buf_va, uint32_t value)
{
    const uint8_t *mem = (const uint8_t *)g_memory_offset;
    int i;

    for (i = s_jmp_used - 1; i >= 0; i--) {
        if (s_jmp[i].buf_va != buf_va)
            continue;

        /* The callee-saved registers and the stack, exactly as the CRT's
         * longjmp restores them: esp is the setjmp-time esp plus the return
         * address that setjmp's own ret would have popped. */
        g_ebx = *(const uint32_t *)(mem + buf_va + 0x04);
        g_edi = *(const uint32_t *)(mem + buf_va + 0x08);
        g_esi = *(const uint32_t *)(mem + buf_va + 0x0C);
        g_esp = *(const uint32_t *)(mem + buf_va + 0x10) + 4;

        /* ebp is a C local in every translated function, and a local modified
         * after setjmp is indeterminate once longjmp lands. Hand the resumed
         * frame its saved value back through the globals it already reads. */
        g_seh_ebp = *(const uint32_t *)(mem + buf_va + 0x00);
        g_ebp     = g_seh_ebp;

        s_jmp_used = i + 1;   /* the inner buffers died with their frames */
        longjmp(s_jmp[i].native, value ? (int)value : 1);
    }
    return 0;
}

/* Watchdog: dump the guest call stack if the title stops making progress.
 *
 * A hang gives nothing to work from -- no crash, no last log line, no native
 * stack that means anything, because the guest frames live in guest memory and
 * the native one only shows whichever translated function is spinning. Sampling
 * the guest stack from a second thread is the one view that says where the
 * title actually is. Same GS format the crash handler uses, so tools/
 * stackwalk.py reads either.
 *
 * Off unless RECOMP_WATCHDOG_SECS is set, so it costs a getenv in normal runs.
 */
/* Defined below, after the watchdog. */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

static uint32_t *s_watchdog_esp;
/* The other guest registers are thread-local too, so the watchdog has to be
 * handed the guest thread's copies rather than reading its own -- which are
 * always zero, and read as "every register is null" at exactly the moment the
 * registers are the thing being asked about. */
static uint32_t *s_watchdog_regs[6];
static unsigned  s_watchdog_secs;

/* Can RECOMP_PEEK dereference this guest address?
 *
 * It used to accept only the first 64 MB, which reads as "RAM" but is not the
 * question -- every window this file maps is mapped at va + g_memory_offset,
 * so the register apertures are just as dereferenceable as RAM is. Rejecting
 * them silently printed nothing for an address that was perfectly readable,
 * and a hang spinning on a GPU register is exactly the case where the value
 * that matters lives at 0xFD......  Peeking one is how the busy-wait in
 * DDS9's pushbuffer reserve was pinned to a DMA pointer rather than a flag.
 *
 * Every window is checked against its own pointer, because they are mapped
 * independently and any of them can be absent for this run. The 4 is the
 * width of the read below: an address one or two bytes short of the end is
 * inside the window and still faults. */
static int peek_readable(uint32_t va)
{
    struct { const void *mapped; uint32_t base; uint64_t size; } win[] = {
        { g_memory_base,   XBOX_BASE_ADDRESS, (uint64_t)g_memory_size },
        { g_contig_memory, XBOX_CONTIG_BASE,  XBOX_CONTIG_SIZE },
        { g_nv2a_memory,   XBOX_NV2A_BASE,    XBOX_NV2A_SIZE },
        { g_mcpx_memory,   XBOX_MCPX_BASE,    XBOX_MCPX_SIZE },
        { g_flash_memory,  XBOX_FLASH_BASE,   XBOX_FLASH_SIZE },
    };
    size_t i;

    for (i = 0; i < sizeof(win) / sizeof(win[0]); i++) {
        if (!win[i].mapped || !win[i].size)
            continue;
        if (va >= win[i].base
                && (uint64_t)va + 4 <= (uint64_t)win[i].base + win[i].size)
            return 1;
    }
    return 0;
}

/* ---- RECOMP_WATCH: name the guest code that changes a guest dword -------
 *
 * A peek says a value changed between two samples. It does not say who
 * changed it, and for a value produced deep inside a middleware layer that
 * is the only question that matters -- reading the lifted C outwards from
 * the write is guesswork, and reading it inwards from the caller is worse.
 *
 * Same mechanism as the AC'97 trap above: make the page read-only, catch the
 * write, single-step it, then report. What it adds is the guest call chain,
 * scanned off the guest stack the way the watchdog does, which turns "the
 * mask became 4" into a list of addresses to go and read.
 *
 * Off unless RECOMP_WATCH is set. Costs a page fault per write to that page,
 * so it is a bring-up tool and says so.
 */
#if defined(_WIN32)
static uint32_t g_watch_va;
static void    *g_watch_page;
static uint32_t g_watch_last;
static void    *g_watch_veh;
static RECOMP_TLS int s_watch_stepping;
static RECOMP_TLS uintptr_t s_watch_rip;   /* host instruction that wrote (runtime-armed watches print it) */
static int s_watch_left = -1;               /* reports left before a runtime-armed watch disarms itself (-1 = no limit) */
static RECOMP_TLS uintptr_t s_watch_fault;  /* host address the trapped write went to */

/* A plausible guest code address: inside the image's executable sections,
 * as recorded from the section headers at load. */
static int watch_is_code(uint32_t va)
{
    return va >= g_xbox_code_lo && va < g_xbox_code_hi;
}

static void watch_report(void)
{
    const uint8_t *mem = (const uint8_t *)g_memory_offset;
    uint32_t now = *(const uint32_t *)(mem + g_watch_va);
    uint32_t esp = g_esp, i, shown = 0;

    if (s_watch_left >= 0) {   /* runtime-armed: every write that covers the dword, same value or not */
        uintptr_t h = (uintptr_t)(mem + g_watch_va);
        if (!(s_watch_fault <= h && h < s_watch_fault + 16u))
            return;
    } else if (now == g_watch_last)
        return;
    fprintf(stderr, "[WATCH] [%08X] %08X -> %08X  (esp=%08X host+%llX)\n",
            g_watch_va, g_watch_last, now, esp,
            (unsigned long long)(s_watch_rip - (uintptr_t)GetModuleHandleA(NULL)));
    g_watch_last = now;
    if (s_watch_left > 0)
        s_watch_left--;

    /* Return addresses the recompiled code pushed, innermost first. Values
     * that merely look like code get printed too -- the chain is a lead, not
     * a proof, and saying so is cheaper than a stack walk that cannot be
     * done without frame information the lift does not keep. */
    for (i = 0; esp && i < 256u && shown < 12u; i++) {
        uint32_t slot = esp + i * 4u;
        uint32_t v;
        if (!peek_readable(slot))
            break;
        v = *(const uint32_t *)(mem + slot);
        if (watch_is_code(v)) {
            fprintf(stderr, "         [esp+%-4u] %08X\n", i * 4u, v);
            shown++;
        }
    }

    /* RECOMP_WATCH_RAW also prints the frame unfiltered. The filtered chain
     * answers "who wrote this"; the raw frame answers "to what object", which
     * is the next question every time -- saved registers and pointer
     * arguments live there and look nothing like code. */
    if (getenv("RECOMP_WATCH_RAW")) {
        for (i = 0; esp && i < 24u; i++) {
            uint32_t slot = esp + i * 4u;
            if (!peek_readable(slot))
                break;
            fprintf(stderr, "         raw[esp+%-4u] %08X\n", i * 4u,
                    *(const uint32_t *)(mem + slot));
        }
    }
    fflush(stderr);
}

static LONG CALLBACK watch_veh(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    DWORD old;

    if (!g_watch_page)
        return EXCEPTION_CONTINUE_SEARCH;

    if (code == EXCEPTION_SINGLE_STEP && s_watch_stepping) {
        s_watch_stepping = 0;
        watch_report();
        if (s_watch_left == 0) {   /* runtime-armed watch used up: leave the page writable */
            g_watch_page = NULL;
        } else
            VirtualProtect(g_watch_page, 4096, PAGE_READONLY, &old);
        ep->ContextRecord->EFlags &= ~0x100u;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (code == EXCEPTION_ACCESS_VIOLATION
            && ep->ExceptionRecord->ExceptionInformation[0] == 1) {
        uintptr_t fault = ep->ExceptionRecord->ExceptionInformation[1];

        if (fault >= (uintptr_t)g_watch_page
                && fault < (uintptr_t)g_watch_page + 4096) {
            if (!VirtualProtect(g_watch_page, 4096, PAGE_READWRITE, &old))
                return EXCEPTION_CONTINUE_SEARCH;
            s_watch_stepping = 1;
            s_watch_rip = (uintptr_t)ep->ContextRecord->Rip;
            s_watch_fault = fault;
            ep->ContextRecord->EFlags |= 0x100u;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* The target, which may be reached through pointers that do not exist yet.
 *
 * "[[0x006DF414]]+0x14" is two dereferences and an offset: the interesting
 * field of a heap object whose address changes run to run, but which is
 * always reachable from a static one. Without this the only way to watch such
 * a field is to learn its address from one run and hope the allocator repeats
 * it, which it does not. */
static unsigned g_watch_derefs;
static uint32_t g_watch_root;
static uint32_t g_watch_off;

static int watch_resolve(uint32_t *out)
{
    const uint8_t *mem = (const uint8_t *)g_memory_offset;
    uint32_t a = g_watch_root;
    unsigned k;

    for (k = 0; k < g_watch_derefs; k++) {
        if (!peek_readable(a))
            return 0;
        a = *(const uint32_t *)(mem + a);
        if (!a)
            return 0;
    }
    a += g_watch_off;
    if (!peek_readable(a))
        return 0;
    *out = a;
    return 1;
}

static int watch_arm(uint32_t va);

/* Poll until the chain resolves, then arm. Twenty milliseconds, because the
 * object appears once during bring-up and never again -- this thread exists
 * for a few seconds and then does nothing for the rest of the run. */
static DWORD WINAPI watch_resolver(LPVOID unused)
{
    unsigned tries;

    (void)unused;
    for (tries = 0; tries < 15000u && !g_watch_page; tries++) {
        uint32_t va;
        if (watch_resolve(&va) && watch_arm(va))
            return 0;
        Sleep(20);
    }
    if (!g_watch_page)
        fprintf(stderr, "  WATCH: %u-deep chain from 0x%08X never resolved\n",
                g_watch_derefs, g_watch_root);
    return 0;
}

void xbox_WatchInit(void)
{
    const char *spec = getenv("RECOMP_WATCH");
    const char *q;
    char *endp;

    if (!spec || !*spec || g_memory_base == NULL || g_watch_page)
        return;

    for (q = spec; *q == '['; q++)
        g_watch_derefs++;
    g_watch_root = (uint32_t)strtoul(q, &endp, 0);
    while (*endp == ']')
        endp++;
    if (*endp == '+')
        g_watch_off = (uint32_t)strtoul(endp + 1, NULL, 0);

    if (g_watch_derefs) {
        fprintf(stderr, "  WATCH: resolving %u-deep chain from 0x%08X "
                        "+0x%X\n", g_watch_derefs, g_watch_root, g_watch_off);
        CloseHandle(CreateThread(NULL, 0, watch_resolver, NULL, 0, NULL));
        return;
    }
    watch_arm(g_watch_root + g_watch_off);
}

static int watch_arm(uint32_t va)
{
    DWORD old;

    g_watch_va = va;
    if (!peek_readable(g_watch_va)) {
        fprintf(stderr, "  WATCH: 0x%08X is not in a mapped window; "
                        "not armed\n", g_watch_va);
        return 0;
    }
    g_watch_last = *(const uint32_t *)((const uint8_t *)g_memory_offset
                                       + g_watch_va);
    /* The page holding the guest dword, in host terms. */
    g_watch_page = (void *)(((uintptr_t)((const uint8_t *)g_memory_offset
                                         + g_watch_va)) & ~(uintptr_t)4095);
    if (!g_watch_veh)
        g_watch_veh = AddVectoredExceptionHandler(1, watch_veh);
    if (!g_watch_veh
            || !VirtualProtect(g_watch_page, 4096, PAGE_READONLY, &old)) {
        if (g_watch_veh) {
            RemoveVectoredExceptionHandler(g_watch_veh);
            g_watch_veh = NULL;
        }
        g_watch_page = NULL;
        fprintf(stderr, "  WATCH: cannot trap 0x%08X; not armed\n",
                g_watch_va);
        return 0;
    }
    fprintf(stderr, "  WATCH: writes to the page of 0x%08X are trapped "
                    "(current %08X)\n", g_watch_va, g_watch_last);
    fflush(stderr);
    return 1;
}

/* Arm (or move) the write watch at run time, e.g. from a bot diagnostic once
 * the object it cares about exists. Prints at most max_reports changes of the
 * dword, then disarms. Test-hook use only. */
void xbox_WatchArmVa(uint32_t va, int max_reports)
{
    DWORD old;

    if (g_watch_page) {
        VirtualProtect(g_watch_page, 4096, PAGE_READWRITE, &old);
        g_watch_page = NULL;
    }
    s_watch_left = max_reports > 0 ? max_reports : -1;
    watch_arm(va);
}
#else
void xbox_WatchInit(void)
{
    const char *spec = getenv("RECOMP_WATCH");
    if (spec && *spec)
        fprintf(stderr, "  WATCH: Windows single-step watchpoints are unavailable on this host\n");
}

void xbox_WatchArmVa(uint32_t va, int max_reports)
{
    (void)va;
    (void)max_reports;
    fprintf(stderr, "  WATCH: Windows single-step watchpoints are unavailable on this host\n");
}
#endif

/* Print the RECOMP_PEEK globals. Shared, because the two moments worth
 * sampling are a hang and an early exit, and only the first had it: a title
 * whose main() returns during init never reaches the watchdog, so the one
 * question that mattered -- which of its init calls failed -- was the one the
 * tooling could not answer. Silent unless RECOMP_PEEK is set. */
void xbox_PeekSample(const char *label)
{
    const uint8_t *mem = (const uint8_t *)g_memory_offset;
    const char *spec = getenv("RECOMP_PEEK");
    char buf[256], *q, *end;

    if (!spec || !*spec || g_memory_base == NULL)
        return;
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    fprintf(stderr, "  %s:", label ? label : "peek");
    for (q = buf; *q; ) {
        /* "[[0x006DF414]]+0x14" follows two pointers and adds an offset.
         * The fields worth watching during bring-up are usually inside heap
         * objects whose addresses change run to run but which are always
         * reachable from a static one, and a peek that cannot follow a
         * pointer cannot see them at all. */
        unsigned derefs = 0, k;
        unsigned long va;
        uint32_t a;
        int ok = 1;

        while (*q == '[') { derefs++; q++; }
        va = strtoul(q, &end, 0);
        if (end == q)
            break;
        while (*end == ']')
            end++;
        a = (uint32_t)va;
        for (k = 0; k < derefs && ok; k++) {
            if (!peek_readable(a) || !(a = *(const uint32_t *)(mem + a)))
                ok = 0;
        }
        if (*end == '+')
            a += (uint32_t)strtoul(end + 1, &end, 0);
        if (ok && peek_readable(a))
            fprintf(stderr, " [%08X]=%08X", a, *(const uint32_t *)(mem + a));
        else
            fprintf(stderr, " [%08X]=??", a);
        q = (*end == ',') ? end + 1 : end;
    }
    fprintf(stderr, "\n");
    fflush(stderr);
}

static DWORD WINAPI xbox_watchdog_thread(LPVOID unused)
{
    const uint8_t *mem;
    uint32_t esp, i;

    (void)unused;
    Sleep(s_watchdog_secs * 1000u);

    mem = (const uint8_t *)g_memory_offset;
    esp = s_watchdog_esp ? *s_watchdog_esp : 0;
    fprintf(stderr, "[WATCHDOG] no exit after %us; guest esp=0x%08X\n"
            "  regs: eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X\n",
            s_watchdog_secs, esp,
            s_watchdog_regs[0] ? *s_watchdog_regs[0] : 0,
            s_watchdog_regs[1] ? *s_watchdog_regs[1] : 0,
            s_watchdog_regs[2] ? *s_watchdog_regs[2] : 0,
            s_watchdog_regs[3] ? *s_watchdog_regs[3] : 0,
            s_watchdog_regs[4] ? *s_watchdog_regs[4] : 0,
            s_watchdog_regs[5] ? *s_watchdog_regs[5] : 0);
    /* The recent indirect-call targets name whatever is spinning: a stuck loop
     * inside a function reached through a pointer leaves no clue on the stack
     * beyond the return address of the call that entered it. */
    {
        uint32_t k;
        /* The running indirect-call total separates a hang from mere
         * slowness. Kernel calls cannot: a pure CPU loop makes none, so
         * "same count at 20s and 60s" proves nothing about it. */
        fprintf(stderr, "  icalls so far: %llu\n",
                (unsigned long long)g_icall_count);
        fprintf(stderr, "  recent ICALL targets:");
        for (k = 0; k < 16; k++)
            fprintf(stderr, " %08X",
                    g_icall_trace[(g_icall_trace_idx + k) & 15]);
        fprintf(stderr, "\n");
    }
    /* Guest globals worth seeing at the moment of the hang.
     *
     * RECOMP_PEEK is otherwise only sampled by the pushbuffer reporter, which
     * a title that hangs before rendering never reaches -- and a spin that
     * makes no kernel calls is invisible to RECOMP_KERNEL_WATCH too. A pure
     * CPU loop polling a global is exactly the case neither of those covers.
     */
    xbox_PeekSample("peek");
    /* The pushbuffer pointers, unconditionally.
     *
     * "Extend the table as more handshakes turn up -- run the title and the
     * watchdog sample will name the register" is only true if the sample
     * actually shows them. It did not: a title spinning on a DMA pointer made
     * no kernel calls and no indirect calls, so every other line the watchdog
     * prints was identical between two samples taken 40 seconds apart, and the
     * register that was stuck did not appear at all.
     *
     * Both sides of the channel, because which one the title consults is a
     * property of its D3D and not of the hardware: Halo waits on the USER
     * pair, DDS9 reads USER first and falls back to PFIFO's DMA_SUBROUTINE.
     * Printing only the pair that some other title used is how this stayed
     * invisible. */
    if (g_nv2a_memory) {
        const char *r = (const char *)g_nv2a_memory;
#define WD_NV2A(off) (*(const volatile uint32_t *)(r + (off)))
        fprintf(stderr, "  NV2A USER  PUT=%08X GET=%08X\n"
                        "  NV2A PFIFO PUT=%08X GET=%08X REF=%08X SUBR=%08X\n",
                WD_NV2A(NV2A_USER_DMA_PUT), WD_NV2A(NV2A_USER_DMA_GET),
                WD_NV2A(NV2A_PFIFO_DMA_PUT), WD_NV2A(NV2A_PFIFO_DMA_GET),
                WD_NV2A(NV2A_PFIFO_REF), WD_NV2A(NV2A_PFIFO_DMA_SUBROUTINE));
#undef WD_NV2A
    }

    for (i = 0; i < 400 && esp; i++) {
        uint32_t a = esp + i * 4;
        if (a < XBOX_STACK_BASE || a >= XBOX_STACK_TOP) break;
        fprintf(stderr, "    GS %08X %08X\n", a,
                *(const uint32_t *)(mem + a));
    }
    fflush(stderr);
    _exit(3);
    return 0;
}

void xbox_WatchdogStart(void)
{
    const char *secs = getenv("RECOMP_WATCHDOG_SECS");
    HANDLE h;

    if (!secs || !*secs)
        return;
    s_watchdog_secs = (unsigned)atoi(secs);
    if (!s_watchdog_secs)
        return;

    /* Taken on the guest thread: g_esp is thread-local, so the watchdog has to
     * be handed the address of the one that matters rather than reading its
     * own, which is always zero. */
    s_watchdog_esp = &g_esp;
    s_watchdog_regs[0] = &g_eax; s_watchdog_regs[1] = &g_ecx;
    s_watchdog_regs[2] = &g_edx; s_watchdog_regs[3] = &g_ebx;
    s_watchdog_regs[4] = &g_esi; s_watchdog_regs[5] = &g_edi;
    h = CreateThread(NULL, 0, xbox_watchdog_thread, NULL, 0, NULL);
    if (h)
        CloseHandle(h);
}

/* SSE. 128 bits of architectural state, per-thread like the rest. */
RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3;
RECOMP_TLS RecompMmx g_mm4, g_mm5, g_mm6, g_mm7;
RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
/* Last frame established by `mov ebp, esp`. Read by frameless functions
 * that address their caller's frame through ebp. */
RECOMP_TLS uint32_t g_ebp = 0;

/* EFLAGS.DF. Zero means the string instructions walk forwards, which is the
 * ABI's resting state and what almost every one of them does -- so this is
 * almost always 0 and costs a predictable branch. The exceptions are the ones
 * that matter: MSVC's strrchr/wcsrchr scan backwards from the terminator with
 * `std; repne scasb`, and memmove goes backwards when its regions overlap the
 * wrong way. Thread-local, because `std` and the `cld` that undoes it can land
 * in different lifted bodies of the same guest routine. */
RECOMP_TLS int g_df = 0;

/* ICALL trace ring buffer */
volatile uint32_t g_icall_trace[16] = {0};
volatile uint32_t g_icall_trace_idx = 0;
volatile uint64_t g_icall_count = 0;

/* W-11: each window the game needs; a failed one used to warn and go on to a generic crash later. */
static unsigned s_layout_failed;

#if !defined(_WIN32)
static int mirror_conflicts_with_aperture(unsigned mirror)
{
    uint64_t lo = (uint64_t)mirror * g_memory_size;
    uint64_t hi = lo + g_memory_size;
    const uint64_t ranges[][2] = {
        { XBOX_CONTIG_BASE, (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE },
        { XBOX_TILED_BASE, (uint64_t)XBOX_TILED_BASE + xbox_TiledApertureSize() },
        { XBOX_NV2A_BASE, (uint64_t)XBOX_NV2A_BASE + XBOX_NV2A_SIZE },
        { XBOX_MCPX_BASE, (uint64_t)XBOX_MCPX_BASE + XBOX_MCPX_SIZE },
        { XBOX_FLASH_BASE, (uint64_t)XBOX_FLASH_BASE + XBOX_FLASH_SIZE },
    };
    if (hi > UINT64_C(0x100000000))
        return 1;
    for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); ++i)
        if (lo < ranges[i][1] && ranges[i][0] < hi)
            return 1;
    return 0;
}
#endif

BOOL xbox_MemoryLayoutInit(const void *xbe_data, size_t xbe_size)
{
    if (!xbe_data || xbe_size < XBE_TLS_ADDR_OFFSET + sizeof(uint32_t)) {
        fprintf(stderr, "xbox_MemoryLayoutInit: XBE header is missing or truncated\n");
        return FALSE;
    }
#if defined(__aarch64__) && !defined(_WIN32)
    if (getenv("RECOMP_AC97_READY") && !xbox_PosixMmioIsStarted()) {
        fprintf(stderr, "xbox_MemoryLayoutInit: start the POSIX MMIO broker before trapping APU/AC97\n");
        return FALSE;
    }
#endif
    g_force_return = getenv("RECOMP_FORCE_RETURN") != NULL;
    DWORD old_protect;
    const uint8_t *xbe = (const uint8_t *)xbe_data;

    if (g_memory_base || g_span_base || s_cleanup_incomplete) {
        fprintf(stderr, "xbox_MemoryLayoutInit: already initialized or cleanup incomplete\n");
        return FALSE;
    }
    s_layout_failed = 0;
    g_apu_mmio_trapped = 0;

    /*
     * Calculate the full range we need to map.
     * From XBOX_MAP_START (0x0) to the end of the furthest section.
     * This includes low memory (KPCR at 0x0-0xFF) which game code reads
     * from, the XBE sections, and the simulated stack.
     */
    /* Map the full Xbox address space (covers all sections + stack + heap).
     * Size is runtime-configurable: retail 64 MB, devkit debug builds 128 MB. */
    /* The mapped range, which is not necessarily RAM. Mirrors are placed
     * at multiples of this, so growing it is what stops a title's
     * above-RAM allocations from aliasing low memory. */
    g_memory_size = g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram;
    if (!g_xbox_total_ram || g_xbox_total_ram > g_memory_size ||
        g_memory_size > UINT32_MAX || (g_memory_size & 0xFFFu)
#if !defined(_WIN32)
        || g_memory_size > SIZE_MAX / (1u + XBOX_NUM_MIRRORS)
#endif
        ) {
        fprintf(stderr, "xbox_MemoryLayoutInit: invalid RAM or mapped size\n");
        g_memory_size = 0;
        return FALSE;
    }
#if !defined(_WIN32)
    {
        long page = sysconf(_SC_PAGESIZE);
        if (page <= 0 || g_memory_size % (size_t)page) {
            fprintf(stderr, "xbox_MemoryLayoutInit: map size is not host-page aligned\n");
            g_memory_size = 0;
            return FALSE;
        }
    }
#endif

    /*
     * Create a file mapping backed by the page file.
     *
     * Using file mapping instead of VirtualAlloc allows us to map the same
     * physical pages at multiple virtual addresses via MapViewOfFileEx.
     * This is critical for the Xbox RAM mirror: the Xbox memory controller
     * uses a 26-bit address bus, so ALL addresses wrap modulo 64 MB.
     * Code that writes to address 0x20000448 is really writing to 0x00000448.
     * With file mapping views, we create aliased mappings at 64 MB intervals
     * that all point to the same physical memory.
     */
    g_mapping_handle = CreateFileMappingA(
        INVALID_HANDLE_VALUE,   /* page file backed */
        NULL,                   /* default security */
        PAGE_READWRITE,         /* read-write access */
        0,                      /* high DWORD of size */
        (DWORD)g_memory_size,   /* low DWORD of size (64 MB) */
        NULL                    /* unnamed mapping */
    );
    if (!g_mapping_handle) {
        fprintf(stderr, "xbox_MemoryLayoutInit: CreateFileMapping failed (error %lu)\n",
                (unsigned long)GetLastError());
        return FALSE;
    }

    /*
     * Map the base view at the desired virtual address.
     * Try the original Xbox base address first. If that fails (common on
     * Windows 11 where low addresses are often reserved), try page-aligned
     * addresses upward until we find a free region.
     */
    {
        static const uintptr_t try_bases[] = {
            XBOX_BASE_ADDRESS,      /* 0x00010000 - original Xbox address */
            0x00800000,             /* 8 MB - above typical PEB/TEB region */
            0x01000000,             /* 16 MB */
            0x02000000,             /* 32 MB */
            0x10000000,             /* 256 MB */
            0,                      /* sentinel - let OS choose */
        };

        /* Iterate the whole array, sentinel included. The old condition
         * (try_bases[i] != 0 || i == 0) stopped *at* the zero rather than
         * using it, so the "let the OS choose" fallback never ran: the loop
         * tried the fixed addresses and gave up. Invisible on Windows, where
         * one of the low bases succeeds -- fatal on arm64 macOS, where all of
         * them sit inside the 4 GB __PAGEZERO segment and none can. */
        /* Reserve base + mirrors as one range, and map the base at its head.
         * VirtualFree releases just the slice about to be used, so each view
         * replaces our own reservation rather than racing for free space.
         *
         * POSIX only. Win32 VirtualFree cannot release part of a reservation:
         * MEM_RELEASE with a nonzero size is ERROR_INVALID_PARAMETER, so both
         * frees below fail, the base view never maps, and the whole span stays
         * reserved. At a 64 MB map that is 1.8 GB the OS tends to place at
         * 0x80000000 -- exactly the host range the contiguous window (guest
         * 0x80000000) needs, which then fails with error 487 and the first
         * touch of the kernel page faults. Larger map sizes push the span
         * above 4 GB, which is why Half-Life 2 (768 MB) never saw it. Doing
         * this on Windows needs placeholder reservations (VirtualAlloc2). */
#ifndef _WIN32
        g_span_size = g_memory_size * (size_t)(1 + XBOX_NUM_MIRRORS);
        g_span_base = VirtualAlloc(NULL, g_span_size, MEM_RESERVE, PAGE_NOACCESS);
        if (g_span_base) {
            g_span_stride = g_memory_size;
            memset(g_span_owned, 1, sizeof(g_span_owned));
            if (release_span_slice(0))
                g_memory_base = MapViewOfFileEx(g_mapping_handle,
                                            FILE_MAP_ALL_ACCESS, 0, 0,
                                            g_memory_size, g_span_base);
        }
        if (!g_memory_base) {
            fprintf(stderr, "xbox_MemoryLayoutInit: could not reserve and map the guest mirror span\n");
            xbox_MemoryLayoutShutdown();
            return FALSE;
        }
#endif

        const size_t n_bases = sizeof(try_bases) / sizeof(try_bases[0]);
        for (size_t i = 0; !g_memory_base && i < n_bases; i++) {
            LPVOID hint = try_bases[i] ? (LPVOID)try_bases[i] : NULL;
            g_memory_base = MapViewOfFileEx(
                g_mapping_handle,
                FILE_MAP_ALL_ACCESS,
                0, 0,           /* offset into mapping */
                g_memory_size,  /* size */
                hint            /* desired base address */
            );
            if (g_memory_base) {
                if (try_bases[i] != 0 && (uintptr_t)g_memory_base != try_bases[i]) {
                    /* OS gave us a different address, retry */
                    UnmapViewOfFile(g_memory_base);
                    g_memory_base = NULL;
                    continue;
                }
                break;
            }
        }
    }

    if (!g_memory_base) {
        fprintf(stderr, "xbox_MemoryLayoutInit: failed to map base view (%zu KB)\n",
                g_memory_size / 1024);
        CloseHandle(g_mapping_handle);
        g_mapping_handle = NULL;
        return FALSE;
    }

#if !defined(_WIN32)
    /* Free conflicting slices before placing the device windows, not after. */
    for (unsigned m = 1; m <= XBOX_NUM_MIRRORS; ++m) {
        if (mirror_conflicts_with_aperture(m) && !release_span_slice(m)) {
            xbox_MemoryLayoutShutdown();
            return FALSE;
        }
    }
#endif

    g_memory_offset = (uintptr_t)g_memory_base - XBOX_MAP_START;

    /* Guest page zero: no access.
     *
     * Nothing legitimate lives there -- every XBE's image base is 0x00010000
     * and the TIB now sits at XBOX_FS_BASE -- so any access is a null pointer
     * the title dereferenced. Left readable it did quiet damage: a null check
     * of the form `cmp byte [ecx], 0` read whatever happened to be at 0 and
     * decided the pointer was fine, and a store through a null pointer landed
     * on real memory and surfaced as corruption somewhere unrelated. Faulting
     * here turns both into one access violation at the instruction that made
     * the mistake, which the crash handler can name.
     *
     * Opt-in through RECOMP_TRAP_NULL, because it converts a class of bug the
     * title currently survives into a hard stop: a guest that dereferences null
     * and ignores the result keeps running while page zero reads as zero, and
     * stops dead once it faults. That is the right default for hunting one of
     * these and the wrong one for making progress past the rest, so it is a
     * switch rather than a policy.
     *
     * Note this is separate from moving the TIB off page zero, which is not
     * optional: with the TIB gone, address 0 reads as plain zero, so a null
     * check written as a load through the pointer now gets the answer it
     * expects whether or not the page is trapped.
     *
     * Best-effort: failing to protect it costs only the diagnostic. */
    if (XBOX_MAP_START == 0 && getenv("RECOMP_TRAP_NULL")) {
        DWORD old_protect;
        /* Protection is applied at host page granularity, and the host page
         * is not always the guest's 4 KB -- Apple Silicon uses 16 KB, so this
         * 0x1000 request actually covers guest 0..0x3FFF. That is why
         * XBOX_TIB_MAIN sits at 0x4000: the widest page any supported host
         * uses fits below the TIB, so the rounding costs nothing and the
         * guard installs everywhere.
         *
         * The check below is what remains of an earlier bug rather than dead
         * code. With the TIB at 0x1000 the rounding reached it, init wrote the
         * TIB moments later, and every run that asked for the guard died at
         * startup -- so the guard disabled itself on all of Apple Silicon and
         * the diagnostic silently did nothing. It stays as a floor for a host
         * with pages wider than the TIB offset, where skipping really is
         * better than breaking the run. */
#if defined(_WIN32)
        SYSTEM_INFO si;
        long host_page;
        GetSystemInfo(&si);
        host_page = (long)si.dwPageSize;
#else
        long host_page = sysconf(_SC_PAGESIZE);
#endif
        if (host_page > 0 && (uint32_t)host_page > XBOX_TIB_MAIN) {
            fprintf(stderr, "  RECOMP_TRAP_NULL: not available -- the host page "
                    "is %ld bytes, so trapping guest page zero would also trap "
                    "the TIB at 0x%08X\n", host_page, XBOX_TIB_MAIN);
            s_layout_failed |= 0x80;
        } else {
            if (host_page > 0 &&
                VirtualProtect(g_memory_base, (SIZE_T)host_page, PAGE_NOACCESS, &old_protect)) {
                fprintf(stderr, "  guest page 0 is PAGE_NOACCESS"
                                " (null dereferences fault)\n");
            } else {
                fprintf(stderr, "  RECOMP_TRAP_NULL: could not protect the host page (error %lu)\n",
                        (unsigned long)GetLastError());
                s_layout_failed |= 0x80;
            }
        }
    }

    if (g_memory_offset == 0) {
        fprintf(stderr, "xbox_MemoryLayoutInit: mapped %zu KB at 0x%08X (original Xbox address)\n",
                g_memory_size / 1024, XBOX_MAP_START);
    } else {
        fprintf(stderr, "xbox_MemoryLayoutInit: mapped %zu KB at 0x%p (offset %+td from Xbox base)\n",
                g_memory_size / 1024, g_memory_base, g_memory_offset);
    }

    /*
     * Helper macro: convert Xbox VA to actual mapped address.
     * When g_memory_offset == 0 (ideal case), this is identity.
     */
    #define XBOX_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))

    /*
     * Copy XBE header to base address.
     * The Xbox kernel maps the XBE image header at 0x00010000.
     * Game code reads kernel thunk table, certificate data, and
     * section info from this region.
     */
    {
        /* XBE header size is at file offset 0x0108 (SizeOfImageHeader) */
        DWORD header_size = 0;
        if (xbe_size >= 0x10C) {
            header_size = *(const DWORD *)(xbe + 0x0108);
        }
        if (header_size == 0 || header_size > 0x10000)
            header_size = 0x1000;  /* fallback: 4KB */
        if (header_size > xbe_size)
            header_size = (DWORD)xbe_size;
        memcpy(XBOX_VA(XBOX_BASE_ADDRESS), xbe, header_size);
        fprintf(stderr, "  XBE header: %u bytes at %p (Xbox VA 0x%08X)\n",
                header_size, XBOX_VA(XBOX_BASE_ADDRESS), XBOX_BASE_ADDRESS);
    }

    /*
     * Dynamically load ALL XBE sections by parsing the section headers.
     *
     * This replaces the old approach of hardcoding section addresses for
     * a specific game (Burnout 3). By reading the section table from the
     * XBE header, any game's sections are loaded automatically.
     *
     * Every section is copied to its original Xbox VA:
     * - .text: needed because memory walkers may scan code pages
     * - .rdata: constants, vtables, kernel thunk table
     * - .data: global variables (initialized portion from XBE, BSS zeroed)
     * - XDK library sections (D3D, DSOUND, WMADEC, XPP, etc.)
     * - DOLBY, BINK, XTIMAGE, etc.
     */
    {
        DWORD base_addr = *(const DWORD *)(xbe + XBE_BASE_ADDR_OFFSET);
        DWORD num_sections = *(const DWORD *)(xbe + XBE_SECTION_COUNT_OFFSET);
        DWORD sect_headers_va = *(const DWORD *)(xbe + XBE_SECTION_HEADERS_OFFSET);
        DWORD sect_headers_off = sect_headers_va - base_addr;
        int sections_loaded = 0;
        /* The certificate's title name (UTF-16, 40 chars at +0x0C), for
         * the framebuffer window's title bar. */
        DWORD cert_off = *(const DWORD *)(xbe + 0x118) - base_addr;
        if (cert_off + 0x0C + 80 <= xbe_size) {
            extern void xbox_FramebufferWindowSetTitle(const uint16_t *, int);
            xbox_FramebufferWindowSetTitle(
                (const uint16_t *)(xbe + cert_off + 0x0C), 40);
        }
        int sections_short = 0;
        size_t total_bytes = 0;

        if (num_sections > 64) num_sections = 64;  /* sanity cap */

        fprintf(stderr, "  XBE sections: %u (headers at file offset 0x%08X)\n",
                num_sections, sect_headers_off);

        for (DWORD si = 0; si < num_sections; si++) {
            if (sect_headers_off + (si + 1) * SECTHDR_SIZE > xbe_size) break;

            const uint8_t *sh = xbe + sect_headers_off + si * SECTHDR_SIZE;
            DWORD sec_va       = *(const DWORD *)(sh + SECTHDR_VA);
            DWORD sec_vsize    = *(const DWORD *)(sh + SECTHDR_VSIZE);
            DWORD sec_raw_off  = *(const DWORD *)(sh + SECTHDR_RAW_OFFSET);
            DWORD sec_raw_size = *(const DWORD *)(sh + SECTHDR_RAW_SIZE);
            DWORD sec_name_va  = *(const DWORD *)(sh + SECTHDR_NAME_ADDR);

            /* Read section name from XBE header */
            const char *sec_name = "?";
            DWORD name_off = sec_name_va - base_addr;
            if (name_off < xbe_size && name_off + 8 <= xbe_size)
                sec_name = (const char *)(xbe + name_off);

            /* Validate: section must fit within our 64MB mapped region */
            if (sec_va < XBOX_BASE_ADDRESS || sec_va + sec_vsize > XBOX_TOTAL_RAM)
                continue;

            /* Determine copy size (raw_size may exceed vsize due to alignment) */
            DWORD copy_size = (sec_raw_size < sec_vsize) ? sec_raw_size : sec_vsize;

            /* Zero the full virtual size first (handles BSS) */
            memset(XBOX_VA(sec_va), 0, sec_vsize);

            /*
             * Copy initialized data from XBE.
             *
             * A section whose raw data runs past the end of the buffer is a
             * truncated or corrupt image, not a BSS section, and it must not
             * be counted among the sections loaded. Reporting it as loaded is
             * how a 4MB title read through a 1MB buffer produced "Loaded
             * 17/17 sections" with every byte of every section still zero --
             * including the kernel thunk table, which then resolved 0 imports
             * and looked like a title that calls no kernel functions.
             */
            int have_data = (copy_size == 0) ||
                            (sec_raw_off + copy_size <= xbe_size);
            if (copy_size > 0 && have_data) {
                memcpy(XBOX_VA(sec_va), xbe + sec_raw_off, copy_size);
            } else if (!have_data) {
                fprintf(stderr,
                        "  WARNING: section %u (%s) raw data 0x%08X+%u runs past "
                        "the %zu-byte image -- left zeroed\n",
                        si, sec_name, sec_raw_off, copy_size, xbe_size);
                sections_short++;
            }

            /* Every loaded section, executable or not. Anything that writes
             * guest memory from outside the title -- the pushbuffer executor
             * clearing a surface, say -- needs to know where the title itself
             * lives, because scribbling on it is not a rendering artefact, it
             * is the title's code and globals gone. */
            if (!g_xbox_image_lo || sec_va < g_xbox_image_lo)
                g_xbox_image_lo = sec_va;
            if (sec_va + sec_vsize > g_xbox_image_hi)
                g_xbox_image_hi = sec_va + sec_vsize;

            /* Executable sections define the range indirect calls may target.
             * XBE section flag 0x04 is EXECUTABLE. */
            if (*(const DWORD *)(sh + SECTHDR_FLAGS) & 0x00000004u) {
                if (!g_xbox_code_lo || sec_va < g_xbox_code_lo)
                    g_xbox_code_lo = sec_va;
                if (sec_va + sec_vsize > g_xbox_code_hi)
                    g_xbox_code_hi = sec_va + sec_vsize;
            }

            if (have_data) {
                sections_loaded++;
                total_bytes += copy_size;
            }

            fprintf(stderr, "  [%2u] %-12s VA=0x%08X vsize=%-8u raw=0x%08X rsize=%-8u%s\n",
                    si, sec_name, sec_va, sec_vsize, sec_raw_off, sec_raw_size,
                    (sec_raw_size < sec_vsize) ? " (BSS)" : "");
        }

        fprintf(stderr, "  Loaded %d/%u sections (%zu bytes total)\n",
                sections_loaded, num_sections, total_bytes);
        if (sections_short) {
            fprintf(stderr,
                    "  ERROR: %d section(s) had no data in the image -- the XBE "
                    "is truncated or was read short; the title will not run\n",
                    sections_short);
        }
    }

    /*
     * Parse the kernel thunk table address from the XBE header.
     * The XBE stores KernelImageThunkAddress at offset 0x0158, XOR-encrypted.
     * The key differs between retail and debug XBEs, and there is no flag
     * saying which was used -- decode with both and keep whichever lands in
     * the mapped address range (this is what tools/xbe_parser does).
     *
     * Debug XBEs are not an edge case here: they are the builds most worth
     * recompiling, since they still carry assert strings and symbols. Halo's
     * cachebeta.xbe is one, and assuming the retail key decoded its thunk
     * table to 0xB4F98174 instead of 0x00253090, which silently fell back to
     * the compile-time default and resolved 0 of 378 kernel imports.
     */
    if (xbe_size >= 0x015C) {
        uint32_t thunk_raw = *(const uint32_t *)(xbe + 0x0158);
        uint32_t thunk_retail = thunk_raw ^ 0x5B6D40B6;  /* retail XOR key */
        uint32_t thunk_debug  = thunk_raw ^ 0xEFB1F152;  /* debug XOR key  */
        uint32_t thunk_va;

        if (thunk_retail >= XBOX_BASE_ADDRESS && thunk_retail < XBOX_TOTAL_RAM) {
            thunk_va = thunk_retail;
        } else {
            thunk_va = thunk_debug;
        }

        /* Validate: thunk VA should be within our mapped region */
        if (thunk_va >= XBOX_BASE_ADDRESS && thunk_va < XBOX_TOTAL_RAM) {
            /* Count thunk entries by scanning until we hit 0 */
            uint32_t thunk_count = 0;
            /* XBOX_KERNEL_THUNK_TABLE_SIZE, not 366: the kernel exports 378
             * slots, and kernel.h notes 366 is short by 12. A title importing
             * a high ordinal would have had its table truncated here. */
            for (uint32_t t = 0; t < XBOX_KERNEL_THUNK_TABLE_SIZE; t++) {
                uint32_t entry = *(volatile uint32_t *)((uintptr_t)(thunk_va + t * 4) + g_memory_offset);
                if (entry == 0) break;
                thunk_count++;
            }
            xbox_kernel_set_thunk_address(thunk_va, thunk_count);
            fprintf(stderr, "  Kernel thunks: %u entries at Xbox VA 0x%08X\n",
                    thunk_count, thunk_va);
        } else {
            fprintf(stderr, "  WARNING: kernel thunk VA 0x%08X out of range (raw=0x%08X)\n",
                    thunk_va, thunk_raw);
        }
    }

    /*
     * NOTE: .rdata is NOT set read-only.
     * VirtualProtect rounds to page boundaries, and the .rdata end (0x003B2454)
     * and .data start (0x003B2360) share the same 4KB page (0x003B2000-0x003B2FFF).
     * Making .rdata read-only also makes the first ~0xCA0 bytes of .data read-only,
     * which causes game initialization code to fault when writing to .data globals
     * in that overlap range.
     */
    (void)old_protect;

    #undef XBOX_VA

    /* Set the global offset for recompiled code MEM macros */
    g_xbox_mem_offset = g_memory_offset;

    /*
     * Initialize the Xbox stack for recompiled code.
     * The stack area lives at XBOX_STACK_BASE in Xbox address space.
     * g_esp is the global stack pointer shared by all translated functions.
     */
    g_esp = XBOX_STACK_TOP;
    fprintf(stderr, "  Stack: %u KB at Xbox VA 0x%08X (ESP = 0x%08X)\n",
            XBOX_STACK_SIZE / 1024, XBOX_STACK_BASE, g_esp);

    /*
     * Populate the fake Thread Information Block (TIB) at Xbox VA 0x0.
     *
     * The original Xbox code uses fs:[offset] to read per-thread data,
     * but the recompiler drops the fs: segment prefix and generates
     * MEM32(offset) instead. Since we mapped low memory (0x0-0xFFFF),
     * we populate the TIB fields that game code accesses:
     *
     *   fs:[0x00] = SEH exception list (-1 = end of chain)
     *   fs:[0x04] = stack base (top of stack)
     *   fs:[0x08] = stack limit (bottom of stack)
     *   fs:[0x18] = self pointer (TIB address)
     *   fs:[0x20] = KPCR Prcb pointer (→ fake structure)
     *   fs:[0x28] = TLS / RW engine context pointer
     *
     * We use free space in the BSS area for the fake structures.
     */
    {
        #define XBOX_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))
        #define MEM32_INIT(va, val) (*(uint32_t *)XBOX_VA(va) = (uint32_t)(val))

        /* Fake TIB at address 0x0 */
        MEM32_INIT(XBOX_FS_BASE + 0x00, 0xFFFFFFFF);       /* SEH: end of chain */
        MEM32_INIT(XBOX_FS_BASE + 0x04, XBOX_STACK_TOP);   /* Stack base (high address) */
        MEM32_INIT(XBOX_FS_BASE + 0x08, XBOX_STACK_BASE);  /* Stack limit (low address) */
        MEM32_INIT(XBOX_FS_BASE + 0x18, XBOX_FS_BASE);     /* Self pointer */

        /*
         * fs:[0x20] - On Xbox KPCR, this is the Prcb pointer.
         * Game code reads [fs:[0x20] + 0x250] which on the real Xbox
         * accesses a D3D cache structure. We set it to 0 so the read
         * at offset 0x250 returns 0, causing the cache init to be skipped.
         */
        /* A zeroed block rather than a null pointer. The read is
         * [fs:[0x20] + 0x250], and this used to be left at 0 so that read
         * landed on guest address 0x250 and returned zero by accident -- which
         * only worked while page zero was mapped. Pointing at real zeroed
         * memory says the same thing to the title and survives that page being
         * unmapped, which is what makes a genuine null dereference visible. */
        #define FAKE_PRCB_VA 0x00761000  /* zeroed KPCR Prcb stand-in */
        memset(XBOX_VA(FAKE_PRCB_VA), 0, 0x400);
        MEM32_INIT(XBOX_FS_BASE + 0x20, FAKE_PRCB_VA);
        #undef FAKE_PRCB_VA

        /*
         * fs:[0x28] - Thread local storage / RW engine context.
         * The RW engine reads [fs:[0x28] + 0x28] to get a pointer
         * to its data area. We allocate a fake structure at 0x00760000
         * (in the BSS area) and a data buffer at 0x00700000.
         */
        #define FAKE_TLS_VA     0x00760000  /* Fake TLS structure (in BSS) */
        #define FAKE_RWDATA_VA  0x00700000  /* RW engine data area (in BSS) */

        MEM32_INIT(XBOX_FS_BASE + 0x28, FAKE_TLS_VA);
        /* TLS[0x28] = pointer to RW data area */
        MEM32_INIT(FAKE_TLS_VA + 0x28, FAKE_RWDATA_VA);

        /*
         * XBE TLS directory.
         *
         * An image with __declspec(thread) data carries one, and on hardware
         * the loader acts on it. Nothing here did, so thread-local access read
         * whatever memory happened to be under fs:[4].
         *
         * Xbox reaches thread-local data through NtTib.StackBase -- fs:[4] --
         * not Win32's fs:[0x2C], and the block sits BELOW that pointer: the
         * image's entry point computes its own index, negative, as
         * -(blocksize/4). Wreckless does this at guest 0x000EB57E and arrives
         * at -5 for its 20-byte block, so [fs:[4] + index*4] is the block's
         * first dword. The rounding below mirrors that arithmetic exactly,
         * because fs:[4] has to land where the title's own index says it is.
         *
         * The index itself is deliberately NOT written here: the title
         * computes and stores it. What the loader owes it is a block in the
         * right place.
         *
         * Slot 0 holds a pointer to per-thread data -- XAPI's SetLastError is
         * [[fs:[4] + index*4] + 4] = err -- so it gets a zeroed block rather
         * than being left NULL, which had SetLastError writing the error code
         * over fs:[4] itself and the next call faulting at guest 0xFFFFFFEF.
         *
         * This block belongs to the initial guest thread. New threads copy an
         * immutable snapshot and receive separate per-thread data.
         */
        #define FAKE_TLS_BLOCK_VA  0x00770000  /* image TLS data          */
        #define FAKE_TLS_THREAD_VA 0x00770200  /* what slot 0 points at   */
        {
            DWORD tls_dir_va = *(const DWORD *)(xbe + XBE_TLS_ADDR_OFFSET);

            if (tls_dir_va) {
                uint32_t tls[5];
                if (tls_dir_va < XBOX_BASE_ADDRESS ||
                    (uint64_t)tls_dir_va + sizeof(tls) > g_memory_size) {
                    s_layout_failed |= 0x200;
                } else {
                    memcpy(tls, XBOX_VA(tls_dir_va), sizeof(tls));
                    uint32_t data_start = tls[0], data_end = tls[1];
                    uint32_t init_size = data_end >= data_start
                                       ? data_end - data_start : 0;
                    uint64_t total64 = (((uint64_t)init_size + tls[4] + 15u)
                                        & ~(uint64_t)15u) + 4u;
                    uint64_t thread64 = ((uint64_t)FAKE_TLS_BLOCK_VA + total64 + 15u)
                                        & ~(uint64_t)15u;
                    if (thread64 < FAKE_TLS_THREAD_VA)
                        thread64 = FAKE_TLS_THREAD_VA;
                    if (data_end < data_start ||
                        (init_size && (data_start < XBOX_BASE_ADDRESS ||
                                       data_end > g_memory_size)) ||
                        total64 > UINT32_MAX || thread64 + 64u > XBOX_STACK_BASE ||
                        thread64 + 64u > g_memory_size) {
                        s_layout_failed |= 0x200;
                    } else {
                        uint32_t total = (uint32_t)total64;
                        uint32_t thread_data = (uint32_t)thread64;
                        g_tls_template = (uint8_t *)calloc(1, total);
                        if (!g_tls_template) {
                            s_layout_failed |= 0x200;
                        } else {
                            if (init_size)
                                memcpy(g_tls_template, XBOX_VA(data_start), init_size);
                            memcpy(g_tls_template, &thread_data, sizeof(thread_data));
                            memcpy(XBOX_VA(FAKE_TLS_BLOCK_VA), g_tls_template, total);
                            memset(XBOX_VA(thread_data), 0, 64);
                            MEM32_INIT(XBOX_FS_BASE + 0x04, FAKE_TLS_BLOCK_VA + total);
                            g_tls_total = total;
                            fprintf(stderr, "  TLS: %u-byte block at 0x%08X,"
                                    " fs:[4] = 0x%08X (index will be %d)\n",
                                    total, FAKE_TLS_BLOCK_VA, FAKE_TLS_BLOCK_VA + total,
                                    -(int)(total / 4));
                        }
                    }
                }
            }
        }
        #undef FAKE_TLS_BLOCK_VA
        #undef FAKE_TLS_THREAD_VA

        memcpy(g_tib_template, XBOX_VA(XBOX_TIB_MAIN), sizeof(g_tib_template));
        fprintf(stderr, "  TIB: fake TIB at VA 0x%X, TLS at 0x%08X, RW data at 0x%08X\n",
                XBOX_FS_BASE, FAKE_TLS_VA, FAKE_RWDATA_VA);

        #undef FAKE_TLS_VA
        #undef FAKE_RWDATA_VA
        #undef MEM32_INIT
        #undef XBOX_VA
    }

    /*
     * Contiguous / physical memory window at 0x80000000.
     *
     * MmAllocateContiguousMemory hands back addresses in this window: physical
     * page P is visible at 0x80000000 + P. Titles that pin buffers at fixed
     * physical addresses then use the whole range, so it has to be backed for
     * its full length - Halo pins 3.4 MB at 0x61000 and 22 MB at 0x3A6000, and
     * with only the fake kernel page mapped here a write walked off the end of
     * it a few pages in.
     *
     * Deliberately NOT a view of the 64 MB RAM mapping. On hardware this window
     * aliases physical RAM, but we load the XBE image into the low addresses of
     * that same region, so aliasing would put a title's pinned pools on top of
     * its own code. Separate storage costs an extra mapping and behaves
     * correctly; nothing here depends on the aliasing.
     *
     * Reserved before the kernel page below, which lives inside it.
     */
    {
        uintptr_t contig_native = XBOX_CONTIG_BASE + g_memory_offset;
        g_contig_mapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
            0, (DWORD)XBOX_CONTIG_SIZE, NULL);
        g_contig_memory = g_contig_mapping
            ? MapViewOfFileEx(g_contig_mapping, FILE_MAP_ALL_ACCESS,
                              0, 0, XBOX_CONTIG_SIZE, (LPVOID)contig_native)
            : NULL;
        if (!g_contig_memory)
            g_contig_memory = VirtualAlloc(
                (LPVOID)contig_native,
                XBOX_CONTIG_SIZE,
                MEM_RESERVE | MEM_COMMIT,
                PAGE_READWRITE
            );
        if (g_contig_memory) {
            fprintf(stderr, "  Contiguous window: %u MB at Xbox VA 0x%08X\n",
                    XBOX_CONTIG_SIZE / (1024 * 1024), XBOX_CONTIG_BASE);
        } else {
            s_layout_failed |= 0x01; fprintf(stderr, "  WARNING: contiguous window at 0x%08X failed "
                    "(error %lu); pinned physical allocations will fault\n",
                    XBOX_CONTIG_BASE, (unsigned long)GetLastError());
        }
    }

    /*
     * NV2A hardware register aperture at 0xFD000000 (16 MB).
     *
     * The GPU's registers are memory-mapped here on real hardware. A title
     * that only calls D3D never notices, but the D3D8 library is linked into
     * the XBE rather than provided by the kernel, so once execution is inside
     * it the register pokes are just loads and stores in recompiled code.
     * Halo faults reading 0xFD001804 during rasterizer_preinitialize, a few
     * instructions after Direct3DCreate8 returns.
     *
     * Backed as ordinary zeroed RAM. That is enough to get through
     * initialisation, and reads returning zero are the benign answer for the
     * status and capability registers touched here.
     *
     * ponytail: plain memory, no register semantics. A spin loop waiting for
     * a bit to *set* would hang rather than fault -- if that shows up, the fix
     * is to bridge the D3D8 entry point that owns the loop, not to start
     * emulating NV2A. Nothing has needed that yet.
     */
    {
        uintptr_t nv2a_native = XBOX_NV2A_BASE + g_memory_offset;
        g_nv2a_memory = VirtualAlloc(
            (LPVOID)nv2a_native,
            XBOX_NV2A_SIZE,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
        /* Execution uses the same poll, but its normal operation does not
         * require a diagnostic print for each submitted pushbuffer. */
        s_nv2a_trace = getenv("RECOMP_NV2A_TRACE") != NULL
                    || getenv("RECOMP_PB_SCAN") != NULL;
        if (g_nv2a_memory) {
            fprintf(stderr, "  NV2A register aperture: %u MB at Xbox VA "
                    "0x%08X (zeroed, no register semantics)\n",
                    XBOX_NV2A_SIZE / (1024 * 1024), XBOX_NV2A_BASE);
        } else {
            s_layout_failed |= 0x02; fprintf(stderr, "  WARNING: NV2A aperture at 0x%08X failed "
                    "(error %lu); D3D register access will fault\n",
                    XBOX_NV2A_BASE, (unsigned long)GetLastError());
        }
    }

    /*
     * MCPX device apertures.
     *
     * The NV2A block above is not the only hardware the title touches
     * directly. The southbridge devices live higher up:
     *
     *   0xFE800000  APU (audio processing unit)
     *   0xFEC00000  AC97
     *   0xFED00000  USB0 / USB1
     *   0xFEF00000  NIC
     *
     * Halo faults reading 0xFED00000 during input initialisation -- the XDK's
     * USB code talks to the host controller's registers rather than going
     * through a driver. Back the whole span as plain RAM for the same reason
     * the NV2A aperture is backed: a read of zero is survivable, a fault is
     * not.
     *
     * ponytail: no register semantics anywhere in here. If something spins
     * waiting for a bit to set, extend the NV2A ack thread's table rather than
     * emulating the device.
     */
    {
        uintptr_t mcpx_native = XBOX_MCPX_BASE + g_memory_offset;
#if defined(_WIN32)
        g_mcpx_memory = VirtualAlloc(
            (LPVOID)mcpx_native,
            XBOX_MCPX_SIZE,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
#else
        /* The guest view may need a 16 KB protection for a 4 KB device page.
         * A second, unprotected view keeps adjacent RAM and the DSP's direct
         * program-memory pointers backed by the same bytes. */
        g_mcpx_mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL,
                                             PAGE_READWRITE, 0,
                                             XBOX_MCPX_SIZE, NULL);
        if (g_mcpx_mapping) {
            g_mcpx_memory = MapViewOfFileEx(g_mcpx_mapping, FILE_MAP_ALL_ACCESS,
                                           0, 0, XBOX_MCPX_SIZE,
                                           (void *)mcpx_native);
            if (g_mcpx_memory)
                g_mcpx_alias = MapViewOfFile(g_mcpx_mapping, FILE_MAP_ALL_ACCESS,
                                            0, 0, XBOX_MCPX_SIZE);
            if (!g_mcpx_alias && g_mcpx_memory) {
                UnmapViewOfFile(g_mcpx_memory);
                g_mcpx_memory = NULL;
            }
        }
        {
            long page = sysconf(_SC_PAGESIZE);
            if (page < 4096 || page > 65536 || (page & (page - 1)) != 0 ||
                (0x30000u % (unsigned long)page) != 0) {
                fprintf(stderr, "  MCPX: unsupported host page size %ld\n", page);
                s_layout_failed |= 0x04;
            } else {
                g_mcpx_host_page = (size_t)page;
            }
        }
#endif
        g_mcpx_regs = g_mcpx_memory;
        if (g_mcpx_memory) {
            /* AC'97 codec ready.
             *
             * DirectSound resets the codec by setting a bit in 0xFEC0012C and
             * then polls 0xFEC00130 for bit 8 a thousand times waiting for the
             * codec to come up. On zeroed registers that bit never appears, so
             * the wait times out and DirectSoundCreate returns DSERR_NODRIVER
             * (0x88780078).
             *
             * That failure is not confined to audio. Wreckless initialises its
             * whole engine object behind `if (DirectSoundCreate() >= 0)`, so a
             * failed create skips the initialisation, leaves the object's table
             * pointer null, and the null propagates: a null-derived divisor
             * produces a NaN transform matrix, which produces a garbage index,
             * which crashes. Reporting the codec as present is what lets the
             * engine initialise at all.
             *
             * The aperture is plain memory, so setting the bit once is enough:
             * nothing clears it, and the poll reads it on the first pass. */
            #define MCPX_AC97_CODEC_STATUS 0x00400130u   /* 0xFEC00130 */
            #define MCPX_AC97_CODEC_READY  0x00000100u
            /* Opt-in, and not because it is wrong.
             *
             * Reporting the codec is the correct answer -- DSERR_NODRIVER is
             * not what hardware returns -- but it is only correct as far as it
             * goes. DirectSound then hands the audio DSP a command block in
             * RAM and spins until the DSP clears it, and there is no DSP here,
             * so the title trades a late crash for an early hang: 44 assets
             * loaded and then a fault, versus one asset and a stall in audio
             * init. Until the DSP handshake is answered, the honest default is
             * the failure that gets further, with the correct behaviour one
             * variable away. */
            if (getenv("RECOMP_AC97_READY")) {
                /* The APU's registers have to fault so they can be routed to
                 * the emulated APU, which is the half that answers the DSP
                 * handshake. Backed as plain memory the guest's writes go
                 * nowhere the APU can see, so it initialises and then waits
                 * forever. Only the APU's own 512K is unmapped: AC'97 above it
                 * stays plain memory, which is what the codec-ready bit needs.
                 *
                 * Enabled by the same variable, because neither half is any
                 * use without the other. */
                /* Registers and the VP only (0x00000-0x2FFFF). The GP and EP
                 * windows above them (0x30000-0x7FFFF) are the DSPs' own
                 * X/Y/P memories, which behave as RAM on hardware and which
                 * the APU model does not implement -- it drops writes there
                 * and reads back 0. Trapping them bought nothing, and cost a
                 * fault on any access the MMIO decoder cannot emulate:
                 * Burnout 3's DirectSound bulk-copies DSP memory with
                 * rep movsd, which the lifter lowers to a host memcpy, and
                 * that faulted at 0xFE830B78 with no way to resume. Plain
                 * memory keeps what the title writes. */
                enum { APU_TRAP_BYTES = 0x00030000 };
                DWORD old_protect;
#if defined(_WIN32)
                if (VirtualProtect((char *)g_mcpx_memory, APU_TRAP_BYTES,
                                   PAGE_NOACCESS, &old_protect))
                    g_apu_mmio_trapped = 1;
                if (g_apu_mmio_trapped)
                {
                    /* DSP memories remain ordinary RAM. Reset release at
                     * the end of GP/EP starts boot DMA into program memory. */
                    VirtualProtect((char *)g_mcpx_memory+0x3F000,0x1000,
                                   PAGE_NOACCESS,&old_protect);
                    VirtualProtect((char *)g_mcpx_memory+0x5F000,0x1000,
                                   PAGE_NOACCESS,&old_protect);
                    fprintf(stderr, "  APU: 0x%08X..0x%08X trapped for MMIO"
                                    " (DSP memory as RAM, reset pages trapped)\n",
                            XBOX_MCPX_BASE, XBOX_MCPX_BASE + APU_TRAP_BYTES);
                }
#elif defined(__aarch64__)
                if (g_mcpx_host_page) {
                    size_t mask = g_mcpx_host_page - 1;
                    size_t gp = 0x3F000u & ~mask, ep = 0x5F000u & ~mask;
                    if (VirtualProtect(g_mcpx_memory, APU_TRAP_BYTES,
                                       PAGE_NOACCESS, &old_protect) &&
                        VirtualProtect((char *)g_mcpx_memory + gp, g_mcpx_host_page,
                                       PAGE_NOACCESS, &old_protect) &&
                        VirtualProtect((char *)g_mcpx_memory + ep, g_mcpx_host_page,
                                       PAGE_NOACCESS, &old_protect)) {
                        g_apu_mmio_trapped = 1;
                        fprintf(stderr, "  APU: registers and DSP reset pages trapped; adjacent RAM uses a shared alias\n");
                    } else {
                        fprintf(stderr, "  APU: could not protect device pages (error %lu)\n",
                                (unsigned long)GetLastError());
                        s_layout_failed |= 0x04;
                    }
                }
#else
                fprintf(stderr, "  APU: trapped POSIX MMIO requires an AArch64 decoder\n");
                s_layout_failed |= 0x04;
#endif
                *(volatile uint32_t *)((char *)g_mcpx_memory
                                       + MCPX_AC97_CODEC_STATUS)
                    |= MCPX_AC97_CODEC_READY;
                /* Before the trap is armed: this write would otherwise be
                 * the first thing to fault. */
                if (!ac97_arm_write_trap()) {
#if !defined(_WIN32)
                    s_layout_failed |= 0x04;
#endif
                }
                fprintf(stderr, "  AC97: codec reported ready at 0x%08X"
                                " (DirectSound will initialise)\n",
                        XBOX_MCPX_BASE + MCPX_AC97_CODEC_STATUS);
            }
            fprintf(stderr, "  MCPX device aperture: %u MB at Xbox VA "
                    "0x%08X (APU/AC97/USB/NIC, zeroed)\n",
                    XBOX_MCPX_SIZE / (1024 * 1024), XBOX_MCPX_BASE);
        } else {
            s_layout_failed |= 0x04; fprintf(stderr, "  WARNING: MCPX aperture at 0x%08X failed "
                    "(error %lu); USB/audio register access will fault\n",
                    XBOX_MCPX_BASE, (unsigned long)GetLastError());
        }
    }

    /* Flash ROM aperture -- see XBOX_FLASH_BASE for why. */
    {
        uintptr_t flash_native = XBOX_FLASH_BASE + g_memory_offset;

        g_flash_memory = VirtualAlloc(
            (LPVOID)flash_native,
            XBOX_FLASH_SIZE,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
        if (g_flash_memory) {
            fprintf(stderr, "  Flash ROM aperture: %u MB at Xbox VA "
                    "0x%08X (zeroed, not a real BIOS image)\n",
                    XBOX_FLASH_SIZE / (1024 * 1024), XBOX_FLASH_BASE);
        } else {
            s_layout_failed |= 0x08; fprintf(stderr, "  WARNING: flash aperture at 0x%08X failed "
                    "(error %lu); a title reading flash will fault\n",
                    XBOX_FLASH_BASE, (unsigned long)GetLastError());
        }
    }

    if (g_nv2a_memory && !s_layout_failed && !xbox_Nv2aAckStart())
        s_layout_failed |= 0x100;

    /*
     * Allocate a page at Xbox kernel address space (0x80010000).
     *
     * RenderWare's Xbox driver code (xbcache.c) reads MEM32(0x8001003C)
     * to parse the Xbox kernel's PE header and find the INIT section for
     * CPU cache line sizing. On PC, we provide a minimal fake PE header
     * with 0 sections so the function gracefully skips the cache init.
     *
     * The actual native address is 0x80010000 + g_memory_offset.
     */
    {
        #define XBOX_KERNEL_BASE 0x80010000u
        #define KERNEL_PAGE_SIZE 4096
        uintptr_t kernel_native = XBOX_KERNEL_BASE + g_memory_offset;
        /* Already committed if the contiguous window above succeeded -
         * 0x80010000 sits inside it - so just use that storage. */
        g_kernel_memory = g_contig_memory
            ? (void *)kernel_native
            : VirtualAlloc((LPVOID)kernel_native, KERNEL_PAGE_SIZE,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (g_kernel_memory) {
            /* Zero-fill then set e_lfanew = 0x80 (offset to PE header).
             * With the rest zeroed, NumberOfSections = 0 and the INIT
             * section search finds nothing, which is the safe path. */
            memset(g_kernel_memory, 0, KERNEL_PAGE_SIZE);
            *(uint32_t *)((uint8_t *)g_kernel_memory + 0x3C) = 0x80;  /* e_lfanew */
            fprintf(stderr, "  Kernel: fake PE header at Xbox VA 0x%08X (native %p)\n",
                    XBOX_KERNEL_BASE, g_kernel_memory);
        } else {
            s_layout_failed |= 0x10; fprintf(stderr, "  WARNING: could not map Xbox kernel VA 0x%08X\n",
                    XBOX_KERNEL_BASE);
        }
        #undef XBOX_KERNEL_BASE
        #undef KERNEL_PAGE_SIZE
    }

    /* Initialize the dynamic heap. */
    fprintf(stderr, "  Heap: %u MB at Xbox VA 0x%08X-0x%08X\n",
            (unsigned)((XBOX_HEAP_TOP - XBOX_HEAP_BASE) / (1024 * 1024)),
            XBOX_HEAP_BASE, XBOX_HEAP_TOP);

    /*
     * Map mirror views of the 64 MB region.
     *
     * On retail Xbox, physical RAM wraps at 64 MB due to the 26-bit
     * address bus. Address 0x04070000 reads the same data as 0x00070000.
     * The RenderWare engine's memory walker crosses 64 MB and accesses
     * mirrored data for an extended walk covering 256+ MB of virtual
     * addresses. Game init code also writes large data structures past
     * 64 MB that on real hardware wrap into physical RAM.
     *
     * We map additional views of the SAME file mapping section at 64 MB
     * intervals. All views alias the same physical pages, so reads and
     * writes at any mirror address correctly access the base data.
     */
    {
        int mirrors_ok = 0;
        /* The tiled aperture is a specific architectural alias -- physical RAM
         * a second time at 0xF0000000, which is where titles render -- while
         * these mirrors are a generic emulation of the address wrap. When the
         * mapped size is large enough that a mirror would cover 0xF0000000,
         * the mirror wins the address and the tiled mapping fails with
         * ERROR_INVALID_ADDRESS; Half-Life 2 then faults on its first surface
         * write. The specific alias is worth more than one wrap mirror, so
         * skip any that would overlap it.
         *
         * Guest addresses, not host: mirror m covers guest
         * (m + 1) * g_memory_size. */
        uint64_t tiled_lo = XBOX_TILED_BASE;
        uint64_t tiled_hi = tiled_lo + xbox_TiledApertureSize();

        for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
            uintptr_t mirror_base = (uintptr_t)g_memory_base +
                                    (uintptr_t)(m + 1) * g_memory_size;
            uint64_t guest_lo = (uint64_t)(m + 1) * g_memory_size;
            uint64_t guest_hi = guest_lo + g_memory_size;

            if ((guest_lo < tiled_hi && tiled_lo < guest_hi)
#if !defined(_WIN32)
                || mirror_conflicts_with_aperture((unsigned)m + 1)
#endif
                ) {
                fprintf(stderr, "  Mirror %d: skipped, overlaps the tiled"
                                " aperture or another device window\n", m + 1);
#if !defined(_WIN32)
                if (g_span_base && !release_span_slice((unsigned)m + 1))
                    s_layout_failed |= 0x20;
#endif
                continue;
            }
            /* Inside the reservation this hands back the slice we are about
             * to use; outside it (no reservation) this is a no-op on an
             * address we never held. */
#if !defined(_WIN32)
            if (g_span_base && !release_span_slice((unsigned)m + 1)) {
                s_layout_failed |= 0x20;
                continue;
            }
#endif
            g_mirror_views[m] = MapViewOfFileEx(
                g_mapping_handle,
                FILE_MAP_ALL_ACCESS,
                0, 0,
                g_memory_size,
                (LPVOID)mirror_base
            );
            if (g_mirror_views[m]) {
                mirrors_ok++;
            } else {
                s_layout_failed |= 0x20; fprintf(stderr, "  Mirror %d: FAILED at %p (error %lu)\n",
                        m + 1, (void *)mirror_base, (unsigned long)GetLastError());
            }
        }
        fprintf(stderr, "  RAM mirror: %d/%d views mapped (covers %d MB)\n",
                mirrors_ok, XBOX_NUM_MIRRORS,
                (int)((mirrors_ok + 1) * g_memory_size / (1024 * 1024)));
    }

    /* Guest address space above the mirrors, for titles that ask for a specific
     * high address. RECOMP_EXT_VMA; a no-op without it. Straight after the
     * mirrors, while a fixed host address is still likely to be free. */
    guest_vmem_init(g_memory_offset, g_memory_size,
                    (uint64_t)g_memory_size * (1u + XBOX_NUM_MIRRORS));

    /*
     * Tiled / write-combined aperture at 0xF0000000.
     *
     * The NV2A exposes physical RAM a second time here and titles render
     * through it. Wreckless's first surface write goes to guest 0xF1954000 --
     * the tiled alias of physical 0x01954000, already inside our RAM -- and
     * faulted because nothing was mapped there.
     *
     * A view of the same section rather than fresh storage: the title writes a
     * surface through the tiled address and reads it back through the normal
     * one, so the two have to be the same bytes. That is the whole reason the
     * RAM lives in a file mapping.
     */
    {
        uintptr_t tiled_native = XBOX_TILED_BASE + g_memory_offset;
        size_t tiled_size = xbox_TiledApertureSize();
        /* A view of the CONTIGUOUS window, not of RAM.
         *
         * On hardware all three -- physical P, 0x80000000+P and 0xF0000000+P
         * -- are one and the same memory. Here they cannot be: the XBE image
         * is loaded at its own VA in the RAM mapping, so aliasing the
         * contiguous window onto RAM would drop a title's pinned physical
         * pools on top of its own code (Halo pins 3.4 MB at 0x61000, which is
         * inside its image). The contiguous window therefore has separate
         * storage, and the question becomes which of the two the tiled
         * aperture should be a view of.
         *
         * It is the contiguous one. A tiled address is a GPU surface address
         * by construction, and GPU surfaces come from
         * MmAllocateContiguousMemory -- so the pairing that has to hold is
         * tiled to contiguous. Against RAM instead, Half-Life 2's loader wrote
         * every decoded video frame through 0xF1C63000 while D3D sampled the
         * texture at 0x81C63000, and the sampler read zeros: 1.8 billion black
         * pixels rasterised, perfectly, from an empty texture.
         */
        if (tiled_size > XBOX_CONTIG_SIZE)
            tiled_size = XBOX_CONTIG_SIZE;
        g_tiled_view = g_contig_mapping
            ? MapViewOfFileEx(
                g_contig_mapping,
                FILE_MAP_ALL_ACCESS,
                0, 0,
                tiled_size,
                (LPVOID)tiled_native)
            : NULL;
        if (g_tiled_view) {
            /* Prove the alias rather than assert it. Everything the title
             * renders goes through this window and is read back through the
             * physical address, so if the two are not the same bytes the GPU
             * sees empty buffers and the screen stays black -- with nothing
             * anywhere to say why. One write and one read turns that into a
             * startup line. */
            {
                volatile uint32_t *via_tiled =
                    (volatile uint32_t *)((uintptr_t)(XBOX_TILED_BASE + 0x1000)
                                          + g_memory_offset);
                volatile uint32_t *via_contig =
                    (volatile uint32_t *)((uintptr_t)(XBOX_CONTIG_BASE + 0x1000)
                                          + g_memory_offset);
                uint32_t saved = *via_contig;

                *via_tiled = 0xA5C30F17u;
                if (*via_contig != 0xA5C30F17u)
                    fprintf(stderr, "  WARNING: tiled aperture does NOT alias"
                            " the contiguous window (wrote A5C30F17, read"
                            " %08X) -- the GPU will sample empty textures\n",
                            *via_contig);
                else
                    fprintf(stderr, "  Tiled aperture alias verified"
                            " (tiled 0x%08X == contiguous 0x%08X)\n",
                            XBOX_TILED_BASE, XBOX_CONTIG_BASE);
                *via_contig = saved;
            }
            fprintf(stderr, "  Tiled aperture: %u MB at Xbox VA 0x%08X"
                    " (aliases the contiguous window)\n",
                    (unsigned)(g_memory_size / (1024 * 1024)),
                    XBOX_TILED_BASE);
        } else {
            s_layout_failed |= 0x40; fprintf(stderr, "  WARNING: tiled aperture at 0x%08X failed"
                    " (error %lu); rendering writes will fault\n",
                    XBOX_TILED_BASE, (unsigned long)GetLastError());
        }
    }

    xbox_WatchInit();
    {   /* RECOMP_TEST_LAYOUT_FAIL=<mask>: pretend those windows failed (W-11 test hook) */
        const char *t = getenv("RECOMP_TEST_LAYOUT_FAIL");
        if (t && *t) s_layout_failed |= (unsigned)strtoul(t, NULL, 0);
    }
    if (s_layout_failed) {
        fprintf(stderr, "xbox_MemoryLayoutInit: required windows failed mask=%02X\n", s_layout_failed);
        xbox_MemoryLayoutShutdown();
        return FALSE;
    }
    fprintf(stderr, "xbox_MemoryLayoutInit: complete\n");
    return TRUE;
}

/*
 * Make every RAM mirror read-only, for finding writes that reach low memory
 * through an alias.
 *
 * Xbox RAM is visible at 28 virtual addresses that alias the same pages, so a
 * store to 0x04000004 changes Xbox VA 4 without ever touching VA 4. Both a
 * page-protection watchpoint and a DR0 hardware watchpoint on VA 4 therefore
 * report nothing while the memory demonstrably changes -- which is exactly
 * what happened chasing Halo's fs:[4] corruption.
 *
 * Debug aid, not part of normal startup: a title that legitimately writes
 * through a mirror will fault here too, and the fault address names the alias
 * and the code.
 */
void xbox_ProtectMirrorsForDebug(void)
{
    int n = 0;
    for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
        DWORD old;
        if (g_mirror_views[m] &&
            VirtualProtect(g_mirror_views[m], g_memory_size,
                           PAGE_READONLY, &old)) {
            n++;
        }
    }
    fprintf(stderr, "  Mirrors: %d/%d made read-only (debug)\n",
            n, XBOX_NUM_MIRRORS);
}

static int release_layout_view(void **address, const char *name)
{
    if (!*address) return 1;
    if (!UnmapViewOfFile(*address)) {
        fprintf(stderr, "  Could not unmap %s at %p (error %lu)\n",
                name, *address, (unsigned long)GetLastError());
        return 0;
    }
    *address = NULL;
    return 1;
}

static int release_layout_memory(void **address, const char *name)
{
    if (!*address) return 1;
    if (!VirtualFree(*address, 0, MEM_RELEASE)) {
        fprintf(stderr, "  Could not release %s at %p (error %lu)\n",
                name, *address, (unsigned long)GetLastError());
        return 0;
    }
    *address = NULL;
    return 1;
}

static int close_layout_mapping(HANDLE *handle, const char *name)
{
    if (!*handle) return 1;
    if (!CloseHandle(*handle)) {
        fprintf(stderr, "  Could not close %s (error %lu)\n",
                name, (unsigned long)GetLastError());
        return 0;
    }
    *handle = NULL;
    return 1;
}

BOOL xbox_MemoryLayoutShutdownChecked(void)
{
#if defined(__aarch64__) && !defined(_WIN32)
    if (xbox_PosixMmioIsBound()) {
        fprintf(stderr, "  POSIX MMIO remains attached; stop the broker before releasing memory\n");
        return FALSE;
    }
#endif
    int released = 1;
    if (g_nv2a_ack_thread) {
        InterlockedExchange(&g_nv2a_ack_stop, 1);
        if (WaitForSingleObject(g_nv2a_ack_thread, INFINITE) != WAIT_OBJECT_0) {
            fprintf(stderr, "  Could not join NV2A ack worker (error %lu); memory retained\n",
                    (unsigned long)GetLastError());
            return FALSE;
        }
        if (!close_layout_mapping(&g_nv2a_ack_thread, "NV2A worker handle"))
            released = 0;
    }
    xbox_ClockStop(); /* No writes into the data export after it is unmapped. */
    nv2a_gpu_memory_shutdown();
    if (g_kernel_memory) {
        if (g_contig_memory) g_kernel_memory = NULL;
        else if (!release_layout_memory(&g_kernel_memory, "kernel page")) released = 0;
    }
    if (!release_layout_memory(&g_nv2a_memory, "NV2A aperture")) released = 0;
    /* Unmap mirror views first */
    for (int m = 0; m < XBOX_NUM_MIRRORS; m++)
        if (!release_layout_view(&g_mirror_views[m], "RAM mirror")) released = 0;
    /* Unmap base view */
    if (!release_layout_view(&g_memory_base, "base RAM view")) released = 0;
    /* The apertures. Left mapped, a second init cannot place them: the first
     * run still owns 0x80000000, 0xFD000000, 0xFE800000, 0xFF000000 and the
     * tiled alias, and every one of those comes back as "failed" while init
     * still returns TRUE because they are best-effort. The result is a layout
     * that looks initialised and has no device apertures at all. */
    if (!release_layout_view(&g_tiled_view, "tiled aperture")) released = 0;
    if (g_contig_memory) {
        if (g_contig_mapping) {
            if (!release_layout_view(&g_contig_memory, "contiguous aperture")) released = 0;
        } else if (!release_layout_memory(&g_contig_memory, "contiguous aperture")) released = 0;
    }
    if (!g_contig_memory && !close_layout_mapping(&g_contig_mapping, "contiguous mapping"))
        released = 0;
    if (g_ac97_veh) {
        if (RemoveVectoredExceptionHandler(g_ac97_veh)) g_ac97_veh = NULL;
        else {
            fprintf(stderr, "  Could not remove AC97 fault handler\n");
            released = 0;
        }
    }
    g_ac97_page = NULL;
#if defined(_WIN32)
    if (!release_layout_memory(&g_mcpx_memory, "MCPX aperture")) released = 0;
#else
    if (!release_layout_view(&g_mcpx_memory, "MCPX aperture")) released = 0;
#endif
    if (!g_mcpx_memory) g_mcpx_regs = NULL;
#if !defined(_WIN32)
    if (!release_layout_view(&g_mcpx_alias, "MCPX host alias")) released = 0;
    if (!g_mcpx_memory && !g_mcpx_alias &&
        !close_layout_mapping(&g_mcpx_mapping, "MCPX mapping")) released = 0;
#endif
    if (!release_layout_memory(&g_flash_memory, "flash aperture")) released = 0;

    int ram_views_gone = !g_memory_base;
    for (int m = 0; m < XBOX_NUM_MIRRORS; m++)
        if (g_mirror_views[m]) ram_views_gone = 0;
    if (ram_views_gone && !close_layout_mapping(&g_mapping_handle, "RAM mapping"))
        released = 0;

    if (!guest_vmem_shutdown()) released = 0;

    /* Whatever is left of the base+mirrors reservation. The views carved out
     * of it are already unmapped above; this releases the range itself. */
    if (g_span_base) {
#if !defined(_WIN32)
        int span_released = 1;
        for (unsigned i = 0; i <= XBOX_NUM_MIRRORS; ++i)
            if (!release_span_slice(i)) span_released = 0;
        if (span_released) {
            g_span_base = NULL;
            g_span_size = g_span_stride = 0;
        } else released = 0;
#else
        if (release_layout_memory(&g_span_base, "guest reservation"))
            g_span_size = 0;
        else released = 0;
#endif
    }
    if (!released) {
        s_cleanup_incomplete = 1;
        fprintf(stderr, "xbox_MemoryLayoutShutdown: incomplete; retain state and retry\n");
        return FALSE;
    }
    g_memory_size = 0;
    g_span_size = 0;
    g_memory_offset = 0;
    g_xbox_mem_offset = 0;
    g_apu_mmio_trapped = 0;
    g_xbox_image_lo = g_xbox_image_hi = 0;
    g_xbox_code_lo = g_xbox_code_hi = 0;
    free(g_tls_template);
    g_tls_template = NULL;
    memset(g_tib_template, 0, sizeof(g_tib_template));
    g_tls_total = 0;
    g_tls_thread_size = 64;
    g_dsp_ack_count = g_fence_mirror_count = g_counter_mirror_count = 0;
    g_frame_counter_count = 0;
    s_fb_va = s_fb_pitch = 0;
    s_fb_height = 480;
    reset_layout_allocators();
    s_layout_failed = 0;
#if !defined(_WIN32)
    g_mcpx_host_page = 0;
#endif
    s_cleanup_incomplete = 0;
    fprintf(stderr, "xbox_MemoryLayoutShutdown: released\n");
    return TRUE;
}

void xbox_MemoryLayoutShutdown(void)
{
    (void)xbox_MemoryLayoutShutdownChecked();
}

/* Bump allocator for pure address-space reservations, above RAM.
 *
 * A MEM_RESERVE costs no memory on real hardware -- it takes address space out
 * of a 4 GB range, not pages out of the 64 MB the console has -- so titles
 * reserve far more than exists and commit a fraction. Satisfying that out of
 * the RAM heap does not work: Half-Life 2 asks for 128 MB and then 200 MB, and
 * clamping those to what the heap can back left it sub-allocating across a
 * range it believed it owned, walking past the top of RAM and aliasing low
 * memory through the mirrors.
 *
 * So reservations come from the mapped space *above* RAM instead. Those pages
 * are already backed and distinct, nothing else hands them out, and a commit
 * inside one is a no-op because it is real memory already.
 *
 * Returns 0 when the mapping is no larger than RAM -- the default for titles
 * that never call xbox_SetMapSize -- which leaves the old behaviour untouched.
 *
 * ponytail: a bump allocator with no free. A reservation is address space, the
 * range is large, and a title that reserves and releases repeatedly would need
 * a real allocator; none has yet.
 */
static uint32_t g_reserve_next;

uint32_t xbox_ReserveAlloc(uint32_t size, uint32_t align)
{
    uint32_t base;

    if (g_memory_size <= g_xbox_total_ram || size == 0)
        return 0;
    if (!align)
        align = 4096;
    if (!g_reserve_next)
        g_reserve_next = (uint32_t)g_xbox_total_ram;

    base = (g_reserve_next + align - 1) & ~(align - 1);
    if ((size_t)base + size > g_memory_size)
        return 0;
    g_reserve_next = base + size;
    return base;
}

BOOL xbox_IsXboxAddress(uintptr_t address)
{
    return (address >= XBOX_BASE_ADDRESS &&
            address < XBOX_BASE_ADDRESS + g_memory_size);
}

void *xbox_TranslateMappedRam(uint32_t address, size_t bytes)
{
    if (!bytes || bytes > UINT64_C(0x100000000) - address) return NULL;
    uint64_t end = (uint64_t)address + bytes;
    if (g_memory_base && end <= g_memory_size)
        return (char *)g_memory_base + address;
    if (g_contig_memory && address >= XBOX_CONTIG_BASE &&
        end <= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE)
        return (char *)g_contig_memory + (address - XBOX_CONTIG_BASE);
    size_t tiled_size = xbox_TiledApertureSize();
    if (tiled_size > XBOX_CONTIG_SIZE) tiled_size = XBOX_CONTIG_SIZE;
    if (g_tiled_view && address >= XBOX_TILED_BASE &&
        end <= (uint64_t)XBOX_TILED_BASE + tiled_size)
        return (char *)g_tiled_view + (address - XBOX_TILED_BASE);
    for (unsigned i = 0; i < XBOX_NUM_MIRRORS; ++i) {
        uint64_t start = (uint64_t)(i + 1) * g_memory_size;
        if (g_mirror_views[i] && address >= start && end <= start + g_memory_size)
            return (char *)g_mirror_views[i] + (size_t)(address - start);
    }
    return NULL;
}

void *xbox_GetMemoryBase(void)
{
    return g_memory_base;
}

ptrdiff_t xbox_GetMemoryOffset(void)
{
    return g_memory_offset;
}

/* ── Dynamic heap allocator ────────────────────────────────
 *
 * Simple bump allocator for MmAllocateContiguousMemory and similar.
 * Returns Xbox VAs within the mapped region so MEM32() works correctly.
 * No free support (bump-only for now).
 */
static uint32_t g_heap_next = XBOX_HEAP_BASE;

static int g_heap_alloc_count = 0;

/* Block table backing xbox_HeapFree. A bump pointer alone never reclaims,
 * which is fine for a title that allocates once and fatal for a debug build
 * that churns. Flat array rather than an intrusive list: allocations come back
 * in bump order, so index order is address order and coalescing is a
 * neighbour check. */
#define XBOX_HEAP_MAX_BLOCKS 65536
static struct { uint32_t addr; uint32_t size; uint8_t free; }
    g_heap_blocks[XBOX_HEAP_MAX_BLOCKS];
static int g_heap_block_count = 0;

/* Guest threads allocate at once (audio, movie and streaming threads next to
 * the main one), and the table above is shared state. */
static SRWLOCK g_heap_lock = SRWLOCK_INIT;

/* RECOMP_HEAP_RECLAIM: give memory back properly.
 *
 * Reuse below hands a whole freed block to whatever asks next, however small,
 * and freeing skips over the empty slots that merging leaves behind, so a
 * title that allocates and frees a lot drains the heap far faster than it
 * uses it. Fixing that changes which address every later allocation gets, in
 * every title that ever frees, so it is off unless asked for. Read once: the
 * heap is hit constantly and the answer does not change. */
int xbox_HeapReclaimEnabled(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("RECOMP_HEAP_RECLAIM") != NULL;
    return on;
}

/* Insert a free block at index `at`, keeping address order. Reuses an empty
 * slot (size 0, left behind by coalescing) when one is already there. */
static int heap_insert_free(int at, uint32_t addr, uint32_t size)
{
    if (!(at < g_heap_block_count && g_heap_blocks[at].size == 0)) {
        if (g_heap_block_count >= XBOX_HEAP_MAX_BLOCKS)
            return 0;
        memmove(&g_heap_blocks[at + 1], &g_heap_blocks[at],
                (size_t)(g_heap_block_count - at) * sizeof g_heap_blocks[0]);
        g_heap_block_count++;
    }
    g_heap_blocks[at].addr = addr;
    g_heap_blocks[at].size = size;
    g_heap_blocks[at].free = 1;
    return 1;
}

/* Fast address lookup in the block table (RECOMP_HEAP_RECLAIM mode).
 *
 * The table is kept in address order (see above) and live blocks never
 * overlap, but coalescing leaves size-0 "holes" with addr 0 in between, so the
 * array as a whole is not sorted. Frees used to scan from index 0 for the
 * address while the table grew 1.5k -> 9.7k entries in a 10 minute match.
 * Binary search that steps over holes finds the same block; compaction below
 * keeps the holes (and the table) from piling up. Caller holds g_heap_lock.
 * RECOMP_HEAP_CHECK=1 cross-checks every lookup against the old linear scan. */
static int g_heap_hole_hint = 0;   /* upper bound on size-0 slots since the last compaction */
static int g_heap_stack_seeded;

/* Index of the non-hole block with the greatest addr <= va, or -1. */
static int heap_floor_index(uint32_t va)
{
    int lo = 0, hi = g_heap_block_count - 1, res = -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2, m = mid;
        while (m <= hi && g_heap_blocks[m].size == 0) m++;
        if (m > hi) { hi = mid - 1; continue; }
        if (g_heap_blocks[m].addr <= va) { res = m; lo = m + 1; }
        else hi = mid - 1;
    }
    return res;
}

static int heap_check_enabled(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("RECOMP_HEAP_CHECK") != NULL;
    return on;
}

/* Index of the first non-free block whose addr == va, or -1. */
static int heap_find_live(uint32_t va)
{
    int r = -1;
    if (xbox_HeapReclaimEnabled()) {
        int i = heap_floor_index(va);
        if (i >= 0 && g_heap_blocks[i].addr == va && !g_heap_blocks[i].free)
            r = i;
        if (heap_check_enabled()) {
            int lin = -1;
            for (int k = 0; k < g_heap_block_count; k++)
                if (g_heap_blocks[k].addr == va && !g_heap_blocks[k].free) { lin = k; break; }
            if (lin != r) {
                static int shown;
                if (shown++ < 16)
                    fprintf(stderr, "  [HEAP-CHECK] free lookup mismatch va=0x%08X fast=%d linear=%d\n",
                            va, r, lin);
            }
            return lin;
        }
        return r;
    }
    for (int k = 0; k < g_heap_block_count; k++)
        if (g_heap_blocks[k].addr == va && !g_heap_blocks[k].free)
            return k;
    return -1;
}

/* Squeeze the holes out, keeping order. Same live/free sequence, fewer slots. */
static void heap_maybe_compact(void)
{
    int w = 0;
    if (!xbox_HeapReclaimEnabled() ||
        g_heap_hole_hint < 64 || g_heap_hole_hint < g_heap_block_count / 8)
        return;
    for (int r = 0; r < g_heap_block_count; r++)
        if (g_heap_blocks[r].size)
            g_heap_blocks[w++] = g_heap_blocks[r];
    g_heap_block_count = w;
    g_heap_hole_hint = 0;
}

/* RECOMP_STACK_RECLAIM (on with RECOMP_HEAP_RECLAIM; =0 disables): give the
 * unused middle of the 8 MB stack region to the heap.
 *
 * Spawned guest threads now take their stacks from the heap, so the region
 * holds only the main thread's stack, growing down from XBOX_STACK_TOP, and
 * the host worker slices carved upward from XBOX_WORKER_STACK_BASE (the timer
 * thread keeps one; APU and OHCI interrupts borrow one while they run). The
 * title's XBE asks for a 128 KB main stack; 1 MB stays reserved for it, and
 * worker slices are capped at 8 (xbox_worker_stack_limit), twice the most
 * seen at once. That returns 5 MB to a heap that is ~6 MB smaller than the
 * console's free memory: MechAssault 2's front end needs ~50 MB after a
 * multiplayer session and the 48.5 MB heap ran out. */
#define XBOX_WORKER_STACK_LIMIT 8
#define XBOX_MAIN_STACK_RESERVE (1024u * 1024u)
static int xbox_StackReclaimEnabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_STACK_RECLAIM");
        on = xbox_HeapReclaimEnabled() && !(v && *v && atoi(v) == 0);
    }
    return on;
}
int xbox_worker_stack_limit(void)
{
    return xbox_StackReclaimEnabled() ? XBOX_WORKER_STACK_LIMIT : XBOX_WORKER_STACK_COUNT;
}
/* Caller holds g_heap_lock. Inserted below the heap base, so index 0 keeps
 * the table in address order; it is not adjacent to the heap or the main
 * stack, so no coalescing reaches across either boundary. */
static void heap_seed_stack_gap(void)
{
    uint32_t lo = XBOX_WORKER_STACK_BASE + XBOX_WORKER_STACK_LIMIT * XBOX_WORKER_STACK_SIZE;
    uint32_t hi = XBOX_STACK_BASE + XBOX_STACK_SIZE - XBOX_MAIN_STACK_RESERVE;
    if (g_heap_stack_seeded)
        return;
    g_heap_stack_seeded = 1;
    if (!xbox_StackReclaimEnabled() || hi <= lo)
        return;
    if (heap_insert_free(0, lo, hi - lo))
        fprintf(stderr, "  [HEAP] reclaimed unused stack range 0x%08X..0x%08X (%u KB)\n",
                lo, hi, (hi - lo) / 1024);
}

/* Take exactly `size` bytes, aligned, out of the smallest free block that has
 * room for them; what is left in front of and behind the piece stays free.
 * Returns 0 when no free block fits. Caller holds g_heap_lock.
 *
 * Best fit, not first fit. First fit split the lowest large hole for every
 * small request, so after a long session MechAssault 2 had 7 MB free in 39
 * runs, none over 3.9 MB, and the front end's 4.98 MB allocation after
 * multiplayer failed -- the title then stopped at its HeapAlloc Verify. The
 * console never sees this: its virtual allocations do not need physically
 * contiguous memory. Keeping large holes whole is the closest equivalent. */
static uint32_t heap_carve_free(uint32_t size, uint32_t alignment)
{
    int best = -1;
    uint32_t best_slack = UINT32_MAX;
    for (int i = 0; i < g_heap_block_count; i++) {
        uint32_t a, start, end;
        if (!g_heap_blocks[i].free || g_heap_blocks[i].size < size)
            continue;
        a = g_heap_blocks[i].addr;
        start = (a + alignment - 1) & ~(alignment - 1);
        end = a + g_heap_blocks[i].size;
        if (start + size > end || start + size < start)
            continue;   /* not enough room once aligned */
        if (g_heap_blocks[i].size - size < best_slack) {
            best_slack = g_heap_blocks[i].size - size;
            best = i;
            if (!best_slack) break;
        }
    }
    for (int i = best; i >= 0; i = -1) {
        uint32_t a, start, end, front, back;
        a = g_heap_blocks[i].addr;
        start = (a + alignment - 1) & ~(alignment - 1);
        end = a + g_heap_blocks[i].size;
        front = start - a;
        back = end - (start + size);
        if (front && !heap_insert_free(i, a, front))
            continue;   /* table full: leave this block alone */
        if (front)
            i++;        /* the taken piece moved up one slot */
        g_heap_blocks[i].addr = start;
        g_heap_blocks[i].size = size;
        g_heap_blocks[i].free = 0;
        if (back && !heap_insert_free(i + 1, start + size, back))
            g_heap_blocks[i].size += back;   /* table full: keep it attached */
        memset((void *)((uintptr_t)start + g_memory_offset), 0, size);
        return start;
    }
    return 0;
}

/*
 * Simulated stacks for spawned threads.
 *
 * The main thread owns the top of the XBOX_STACK region and grows down; worker
 * stacks are carved from the bottom upward so the two cannot meet until the
 * whole 8 MB is gone. Xbox VAs, not host memory: recompiled code addresses its
 * stack through MEM32() like any other Xbox pointer.
 */
uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);

#define XBOX_THREAD_STACK_SIZE  (512 * 1024)
#define XBOX_MAX_THREAD_STACKS  8

static int g_thread_stacks_used = 0;
static SRWLOCK g_thread_stacks_lock = SRWLOCK_INIT;

/* A TIB and TLS block for a newly spawned guest thread.
 *
 * A TIB is per-thread on the console and was per-process here: one address,
 * 0x1000, for everyone. Two things live in it that must not be shared. fs:[0]
 * is the SEH chain head, so two threads unwinding at once walk each other's
 * frames. fs:[4] points at the image's TLS block, whose slot 0 is the CRT's
 * per-thread data -- errno, the locale, and the bookkeeping _lock() uses to
 * decide who owns which lock.
 *
 * Half-Life 2 deadlocked on the last of those: two threads inside _lock(),
 * each holding the CRT lock the other was waiting for, because "which thread
 * am I" was a single shared answer.
 *
 * The new block is a copy of the template the loader built, so a thread starts
 * with the image's initialised thread-local data rather than zeros, and its
 * own per-thread structure behind slot 0.
 */
uint32_t xbox_AllocThreadTib(void)
{
    /* XBOX_VA is scoped to the loader; the same arithmetic, spelled here. */
    #define TIB_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))
    const uint32_t tib_size = 0x40;
    uint32_t tib, block, thread_data, total;

    if (!g_memory_base)
        return 0;
    total = g_tls_total;
    tib = xbox_HeapAlloc(tib_size + (total ? total + g_tls_thread_size : 0), 16);
    if (!tib)
        return 0;

    memcpy(TIB_VA(tib), g_tib_template, tib_size);
    *(uint32_t *)TIB_VA(tib + 0x00) = 0xFFFFFFFFu;   /* own SEH chain    */
    *(uint32_t *)TIB_VA(tib + 0x18) = tib;            /* own TIB pointer  */
    if (total) {
        block       = tib + tib_size;
        thread_data = block + total;
        memcpy(TIB_VA(block), g_tls_template, total);
        memset(TIB_VA(thread_data), 0, g_tls_thread_size);
        *(uint32_t *)TIB_VA(block)      = thread_data;
        *(uint32_t *)TIB_VA(tib + 0x04) = block + total;
    }

    return tib;
    #undef TIB_VA
}

uint32_t xbox_AllocThreadStack(void)
{
    uint32_t base;

    AcquireSRWLockExclusive(&g_thread_stacks_lock);
    if (g_thread_stacks_used >= XBOX_MAX_THREAD_STACKS) {
        ReleaseSRWLockExclusive(&g_thread_stacks_lock);
        return 0;
    }

    /* From the heap, not from XBOX_STACK_BASE.
     *
     * The stack region begins at 0x00780000, which is fine only while the
     * title's image ends below that. Half-Life 2's image runs to 0x009B68C0,
     * so the first thread stack (0x00780000..0x00800000) landed inside its
     * .rdata and .data: the worker spawned during engine init wrote its
     * frames over the game's own static data. Nothing faults -- the pages are
     * mapped and writable -- so it shows up later as globals that were
     * correct when written and wrong when read.
     *
     * The heap already starts above the image and knows how big it is, so
     * taking slices from it is correct for any image size instead of only
     * for small ones.
     */
    base = xbox_HeapAlloc(XBOX_THREAD_STACK_SIZE, 4096);
    if (!base) {
        ReleaseSRWLockExclusive(&g_thread_stacks_lock);
        return 0;
    }
    g_thread_stacks_used++;
    ReleaseSRWLockExclusive(&g_thread_stacks_lock);

    /* Top of the slice, 16-byte aligned, growing down. */
    return base + XBOX_THREAD_STACK_SIZE - 16;
}

/* Give a worker's stack back when the worker ends.
 *
 * The counter used to only ever go up, so a title that creates and destroys
 * threads ran the pool dry no matter how few were alive at once. The Xbox
 * Dashboard spawns one worker per ambient WAV and terminates it before loading
 * the next; after XBOX_MAX_THREAD_STACKS files the pool was empty and
 * PsCreateSystemThreadEx fell back to running the worker inline. That fallback
 * is a deadlock here rather than a slowdown: the worker ran to completion
 * before the caller reached its wait, so the main thread then waited forever on
 * events whose only signaller had already finished. It looked like an audio
 * hang, three layers away from the cause.
 *
 * Takes the value AllocThreadStack returned, so callers never do the arithmetic.
 */
void xbox_FreeThreadStack(uint32_t stack_top)
{
    if (!stack_top)
        return;
    AcquireSRWLockExclusive(&g_thread_stacks_lock);
    xbox_HeapFree(stack_top + 16 - XBOX_THREAD_STACK_SIZE);
    if (g_thread_stacks_used > 0)
        g_thread_stacks_used--;
    ReleaseSRWLockExclusive(&g_thread_stacks_lock);
}

/* Bump allocator over the contiguous window mapped at XBOX_CONTIG_BASE.
 *
 * MmAllocateContiguousMemory hands back physical memory, and on Xbox physical
 * page P is visible at 0x80000000 + P. Drivers rely on that being an exact
 * round trip: Xbox D3D writes its pushbuffer position to the NV2A as
 * `VA & 0x0FFFFFFF` and reads the GPU's position back as `GET | 0x80000000`,
 * then compares the two. That holds for any address in this window and for
 * nothing in the general heap, whose position depends on what the title
 * reserved first -- Half-Life 2 reserves 128 MB and then 200 MB before D3D
 * allocates its pushbuffer, which put the buffer at 0x15782000 and left the
 * engine comparing 0x857844C0 against it forever.
 *
 * Grows up from the base; XBOX_GPU_INSTANCE_DEFAULT is carved off the top by
 * the GPU-instance bridge, so the two do not meet until the window is full.
 * Without RECOMP_HEAP_RECLAIM never freed: contiguous blocks are framebuffers
 * and pushbuffers, which a title allocates once. */
/* Starts one page in. Physical page 0 is never handed out by the real
 * kernel, and the XDK's USB stack relies on that: XPP carves its host
 * controller structures from a private 0xFE0-byte arena ending at
 * 0x80001000 (sub_00365454 in Burnout 3), with no allocation call at all.
 * Starting at the base gave that same page to the title's first
 * MmAllocateContiguousMemory -- XAPI's launch data page -- and whichever
 * wrote last won. The symptom was timing-dependent: enumeration worked
 * when logging slowed the title down and otherwise stopped in the root
 * port reset, walking a device whose parent pointer had been overwritten. */
static uint32_t g_contig_next = XBOX_CONTIG_BASE + 0x1000u;

/* Under RECOMP_HEAP_RECLAIM, blocks handed out in address order, so
 * MmFreeContiguousMemory can give them back. "Allocated once" holds for
 * framebuffers and pushbuffers but not for titles that keep textures and
 * vertex data here: one title (T()NY) frees its title-screen scene and
 * allocates the first mission's, and with a bump allocator the 64 MB window
 * ran out on the first character model.
 * Freed blocks are reused first-fit and merge with free neighbours; the bump
 * pointer only grows, so xbox_ContiguousAllocatedBytes keeps its meaning.
 * Without the switch nothing is tracked and the arena is the bump allocator.
 * ponytail: linear scan over at most CONTIG_MAX_BLOCKS; a size-bucketed free
 * list if a title ever allocates here thousands of times a frame. */
#define CONTIG_MAX_BLOCKS 16384
static struct { uint32_t addr, size; int free; } g_contig_blocks[CONTIG_MAX_BLOCKS];
static int g_contig_block_count;
static SRWLOCK g_contig_lock = SRWLOCK_INIT;

static uint32_t contig_allocation_granularity(void)
{
#if !defined(_WIN32)
    long page = sysconf(_SC_PAGESIZE);
    if (page > 4096 && page <= 65536 && (page & (page - 1)) == 0)
        return (uint32_t)page;
#endif
    return 4096;
}

/* Shutdown is called only after every guest/device worker has stopped. */
static void reset_layout_allocators(void)
{
    g_reserve_next = 0;
    g_heap_next = XBOX_HEAP_BASE;
    g_heap_alloc_count = g_heap_block_count = g_heap_hole_hint = 0;
    g_heap_stack_seeded = g_thread_stacks_used = 0;
    g_contig_next = XBOX_CONTIG_BASE + 0x1000u;
    g_contig_block_count = 0;
}

static void contig_insert(int at, uint32_t addr, uint32_t size, int is_free)
{
    if (g_contig_block_count >= CONTIG_MAX_BLOCKS || size == 0)
        return;                        /* table full: the piece is simply not tracked */
    memmove(&g_contig_blocks[at + 1], &g_contig_blocks[at],
            (g_contig_block_count - at) * sizeof g_contig_blocks[0]);
    g_contig_blocks[at].addr = addr;
    g_contig_blocks[at].size = size;
    g_contig_blocks[at].free = is_free;
    g_contig_block_count++;
}

uint32_t xbox_ContiguousAlloc(uint32_t size, uint32_t alignment)
{
    uint32_t requested=size;
    uint32_t result = 0;
    uint32_t granularity = contig_allocation_granularity();
    int reclaim = xbox_HeapReclaimEnabled();
    int i;

    if(!size || size>XBOX_CONTIG_SIZE || (alignment && (alignment&(alignment-1))))return 0;
    if (alignment < granularity) alignment = granularity;
    if (granularity > 4096) {
        if (size > UINT32_MAX - (granularity - 1))
            return 0;
        size = (size + granularity - 1) & ~(granularity - 1);
        if (size > XBOX_CONTIG_SIZE)
            return 0;
    }
    if (reclaim) {
        AcquireSRWLockExclusive(&g_contig_lock);
        size = (size + 4095u) & ~4095u;
        if (size == 0) size = 4096;

        /* A freed block first: carve the aligned piece, keep what is left free. */
        for (i = 0; i < g_contig_block_count; i++) {
            uint32_t a, end, front, back;
            if (!g_contig_blocks[i].free || g_contig_blocks[i].size < size)
                continue;
            a = (g_contig_blocks[i].addr + alignment - 1) & ~(alignment - 1);
            end = g_contig_blocks[i].addr + g_contig_blocks[i].size;
            if ((uint64_t)a + size > end)
                continue;
            front = a - g_contig_blocks[i].addr;
            back = end - (a + size);
            g_contig_blocks[i].addr = a;
            g_contig_blocks[i].size = size;
            g_contig_blocks[i].free = 0;
            if (back)  contig_insert(i + 1, a + size, back, 1);
            if (front) contig_insert(i, a - front, front, 1);
            result = a;
            break;
        }
    }

    if (!result) {
        result = (g_contig_next + alignment - 1) & ~(alignment - 1);

        /* Leave the top of the window for GPU instance memory. */
        if ((uint64_t)result + size >
                (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE
                    - XBOX_GPU_INSTANCE_DEFAULT) {
            fprintf(stderr, "  [CONTIG] arena exhausted (%u requested, %u of %u used)\n",
                    size, g_contig_next - XBOX_CONTIG_BASE,
                    (unsigned)XBOX_CONTIG_SIZE);
            fflush(stderr);
            if (reclaim)
                ReleaseSRWLockExclusive(&g_contig_lock);
            return 0;
        }
        if (reclaim) {
            if (result > g_contig_next)    /* the alignment gap stays usable */
                contig_insert(g_contig_block_count, g_contig_next,
                              result - g_contig_next, 1);
            contig_insert(g_contig_block_count, result, size, 0);
        }
        g_contig_next = result + size;
    }
    if (reclaim)
        ReleaseSRWLockExclusive(&g_contig_lock);

    /* MmFreeContiguousMemory releases page protection as well as ownership.
     * The physical view stays mapped in this runtime, so a former read-only
     * GPU allocation must become writable before its pages are zeroed/reused. */
    DWORD old_protect;
    if(!VirtualProtect((void *)((uintptr_t)result+g_memory_offset),size,PAGE_READWRITE,&old_protect)) {
        fprintf(stderr,"[CONTIG] cannot reset allocated pages %08X+%u: %lu\n",
                result,size,(unsigned long)GetLastError());
        xbox_ContiguousFree(result);
        return 0;
    }
    static int alloc_trace = -1;   /* getenv once: this runs for every texture / push buffer */
    if(alloc_trace < 0) alloc_trace = getenv("RECOMP_ALLOC_TRACE") != NULL;
    if(alloc_trace) {
        MEMORY_BASIC_INFORMATION region;
        VirtualQuery((void *)((uintptr_t)result+g_memory_offset),&region,sizeof(region));
        fprintf(stderr,"[CONTIG-ALLOC] request=%u rounded=%u alignment=%u -> %08X protect=%lX esp=%08X\n",
            requested,size,alignment,result,(unsigned long)region.Protect,g_esp);
        fflush(stderr);
    }
    nv2a_gpu_memory_reallocated((void *)((uintptr_t)result+g_memory_offset),size);
    memset((void *)((uintptr_t)result + g_memory_offset), 0, size);
    return result;
}

/* Give a block back (RECOMP_HEAP_RECLAIM only). Returns 0 when the address is
 * not one this arena tracks, so the caller can try the general heap. */
int xbox_ContiguousFree(uint32_t addr)
{
    int i, found = 0;

    if (!xbox_HeapReclaimEnabled())
        return 0;
    AcquireSRWLockExclusive(&g_contig_lock);
    for (i = 0; i < g_contig_block_count; i++) {
        if (g_contig_blocks[i].addr != addr || g_contig_blocks[i].free)
            continue;
        nv2a_gpu_memory_reallocated((void *)((uintptr_t)addr+g_memory_offset),g_contig_blocks[i].size);
        g_contig_blocks[i].free = 1;
        found = 1;
        if (i + 1 < g_contig_block_count && g_contig_blocks[i + 1].free
                && g_contig_blocks[i].addr + g_contig_blocks[i].size == g_contig_blocks[i + 1].addr) {
            g_contig_blocks[i].size += g_contig_blocks[i + 1].size;
            memmove(&g_contig_blocks[i + 1], &g_contig_blocks[i + 2],
                    (g_contig_block_count - i - 2) * sizeof g_contig_blocks[0]);
            g_contig_block_count--;
        }
        if (i > 0 && g_contig_blocks[i - 1].free
                && g_contig_blocks[i - 1].addr + g_contig_blocks[i - 1].size == g_contig_blocks[i].addr) {
            g_contig_blocks[i - 1].size += g_contig_blocks[i].size;
            memmove(&g_contig_blocks[i], &g_contig_blocks[i + 1],
                    (g_contig_block_count - i - 1) * sizeof g_contig_blocks[0]);
            g_contig_block_count--;
        }
        break;
    }
    ReleaseSRWLockExclusive(&g_contig_lock);
    return found;
}

/* Size of the block at addr (MmQueryAllocationSize), 0 if not tracked. */
uint32_t xbox_ContiguousBlockSize(uint32_t addr)
{
    uint32_t r = 0;
    int i;

    if (!xbox_HeapReclaimEnabled())
        return 0;
    AcquireSRWLockShared(&g_contig_lock);
    for (i = 0; i < g_contig_block_count; i++)
        if (g_contig_blocks[i].addr == addr && !g_contig_blocks[i].free) {
            r = g_contig_blocks[i].size;
            break;
        }
    ReleaseSRWLockShared(&g_contig_lock);
    return r;
}

/* How much of the window has been handed out.
 *
 * Lets a caller holding a physical address decide whether it names contiguous
 * memory this runtime allocated. The pushbuffer executor needs exactly that:
 * a surface offset is physical, and only the window makes it addressable. */
uint32_t xbox_ContiguousAllocatedBytes(void)
{
    return g_contig_next - XBOX_CONTIG_BASE;
}


static uint32_t heap_alloc_locked(uint32_t size, uint32_t alignment);

uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment)
{
    uint32_t r;

    AcquireSRWLockExclusive(&g_heap_lock);
    r = heap_alloc_locked(size, alignment);
    ReleaseSRWLockExclusive(&g_heap_lock);
    return r;
}

/* Join a freed final extent with the unused bump tail only when the normal
 * allocation path would fail. The last nonempty descriptor must end exactly
 * at the watermark: never rewind across an untracked or live allocation.
 * Caller holds g_heap_lock. No mutation unless the whole request fits. */
static int heap_join_free_tail(uint32_t size, uint32_t alignment,
                               uint32_t *result)
{
    int last = g_heap_block_count - 1;
    uint64_t start, end;
    while (last >= 0 && g_heap_blocks[last].size == 0)
        last--;
    if (last < 0 || !g_heap_blocks[last].free ||
        (uint64_t)g_heap_blocks[last].addr + g_heap_blocks[last].size != g_heap_next)
        return 0;
    start = ((uint64_t)g_heap_blocks[last].addr + alignment - 1) &
            ~((uint64_t)alignment - 1);
    end = start + size;
    if (end > XBOX_HEAP_TOP || start > UINT32_MAX)
        return 0;
    /* The new bump allocation replaces the freed tail descriptor. Preserve
     * any alignment prefix as free rather than discarding available bytes. */
    if (start != g_heap_blocks[last].addr) {
        if (last + 1 >= XBOX_HEAP_MAX_BLOCKS)
            return 0;
        g_heap_blocks[last].size = (uint32_t)start - g_heap_blocks[last].addr;
        g_heap_block_count = last + 1;
    } else {
        g_heap_block_count = last;
    }
    g_heap_next = (uint32_t)start;
    *result = (uint32_t)start;
    return 1;
}

static uint32_t heap_alloc_locked(uint32_t size, uint32_t alignment)
{
    uint32_t result;

    if (alignment < 4) alignment = 4;

    /* Enforce minimum allocation size.
     * The Xbox D3D8 code sometimes computes resource sizes from GPU
     * capabilities that return 0 (since we don't have real NV2A hardware),
     * resulting in zero-size allocations. With a bump allocator, these all
     * return the same address, causing overlapping structures. Enforce a
     * minimum of 4096 bytes so each allocation gets its own memory. */
    if (size < 16) size = 16;

    /* Reuse a freed block first. Without this the heap only ever grows: Halo's
     * debug build allocates and releases heavily through init, exhausted all
     * 48 MB in 4,726 allocations, and its second D3D CreateDevice then failed
     * with E_OUTOFMEMORY -- which the title reports by clearing
     * global_d3d_device, so the rasterizer asserts and startup stops. */
    if (xbox_HeapReclaimEnabled()) {
        heap_seed_stack_gap();
        /* Take only what the request needs; see xbox_HeapReclaimEnabled(). */
        size = (size + 15) & ~15u;
        result = heap_carve_free(size, alignment);
        if (result)
            return result;
    } else {
        for (int i = 0; i < g_heap_block_count; i++) {
            if (!g_heap_blocks[i].free || g_heap_blocks[i].size < size) {
                continue;
            }
            if (g_heap_blocks[i].addr & (alignment - 1)) {
                continue;   /* wrong alignment for this request */
            }
            g_heap_blocks[i].free = 0;
            result = g_heap_blocks[i].addr;
            memset((void *)((uintptr_t)result + g_memory_offset), 0, size);
            return result;
        }
    }

    /* Align the next pointer */
    result = (g_heap_next + alignment - 1) & ~(alignment - 1);

    if ((uint64_t)result + size > XBOX_HEAP_TOP &&
        !(xbox_HeapReclaimEnabled() && heap_join_free_tail(size, alignment, &result))) {
        fprintf(stderr, "xbox_HeapAlloc: out of memory (requested %u, used %u/%u)\n",
                size, g_heap_next - XBOX_HEAP_BASE,
                (unsigned)(XBOX_HEAP_TOP - XBOX_HEAP_BASE));
        /* Exhausted or fragmented? Live bytes, free bytes inside the bump
         * range, the largest free run and how many runs there are. */
        {
            uint64_t live = 0, free_bytes = 0; uint32_t largest = 0; int runs = 0;
            for (int i = 0; i < g_heap_block_count; i++) {
                if (!g_heap_blocks[i].size) continue;
                if (g_heap_blocks[i].free) {
                    free_bytes += g_heap_blocks[i].size; runs++;
                    if (g_heap_blocks[i].size > largest) largest = g_heap_blocks[i].size;
                } else live += g_heap_blocks[i].size;
            }
            fprintf(stderr, "  [HEAP] live=%llu KB free=%llu KB in %d runs, largest free run %u KB, "
                            "tail %u KB, descriptors %d\n",
                    (unsigned long long)(live / 1024), (unsigned long long)(free_bytes / 1024), runs,
                    largest / 1024, (unsigned)((XBOX_HEAP_TOP - g_heap_next) / 1024), g_heap_block_count);
        }
        /* Who ate the heap? Group live blocks by size -- an exhausted heap is
         * nearly always one request size repeated, and the count names it. */
        {
            static int dumped = 0;
            static struct { uint32_t size; int n; } hist[256];
            if (!dumped) {
                int used = 0;
                dumped = 1;
                for (int i = 0; i < g_heap_block_count; i++) {
                    int j = 0;
                    if (g_heap_blocks[i].free || !g_heap_blocks[i].size) continue;
                    while (j < used && hist[j].size != g_heap_blocks[i].size) j++;
                    if (j == used) {
                        if (used == 256) continue;   /* ponytail: 256 distinct sizes is plenty */
                        hist[used].size = g_heap_blocks[i].size;
                        hist[used++].n = 0;
                    }
                    hist[j].n++;
                }
                for (int j = 0; j < used; j++) {
                    if ((uint64_t)hist[j].n * hist[j].size < 1024 * 1024) continue;
                    fprintf(stderr, "  [HEAP] %d live blocks of %u bytes (%u KB)\n",
                            hist[j].n, hist[j].size,
                            (unsigned)((uint64_t)hist[j].n * hist[j].size / 1024));
                }
                fflush(stderr);
            }
        }
        return 0;
    }

    g_heap_next = result + size;

    /* Zero-fill the allocated block (Xbox memory is always zeroed) */
    memset((void *)((uintptr_t)result + g_memory_offset), 0, size);

    if (g_heap_block_count < XBOX_HEAP_MAX_BLOCKS) {
        g_heap_blocks[g_heap_block_count].addr = result;
        g_heap_blocks[g_heap_block_count].size = size;
        g_heap_blocks[g_heap_block_count].free = 0;
        g_heap_block_count++;
    }

    g_heap_alloc_count++;
    /* Rate-limited: a debug title makes thousands of these and the log is a
     * diagnostic, not a transaction record. */
    if (g_heap_alloc_count <= 32 || (g_heap_alloc_count % 512) == 0) {
        fprintf(stderr, "  [HEAP] #%d: size=%u align=%u → 0x%08X..0x%08X (used %u/%u)\n",
                g_heap_alloc_count, size, alignment, result, result + size,
                g_heap_next - XBOX_HEAP_BASE,
                (unsigned)(XBOX_HEAP_TOP - XBOX_HEAP_BASE));
        fflush(stderr);
    }

    return result;
}

/* How big is the block at this guest address?
 *
 * MmQueryAllocationSize and ExQueryPoolBlockSize both ask this, and both used
 * to answer 0 -- ExQueryPoolBlockSize by returning a literal, and
 * MmQueryAllocationSize by having no bridge at all. The host cannot answer it:
 * VirtualQuery on the translated address reports the size of the whole 64 MB
 * guest mapping, which is a worse answer than none. The block table already
 * has the real one, and it is the same table xbox_HeapFree matches against.
 *
 * Interior addresses count: a title that asks about a pointer it has walked
 * forward is asking about the block that contains it. Returns 0 for an address
 * this heap never handed out, which is what "not one of mine" has to look like.
 */
uint32_t xbox_HeapBlockSize(uint32_t xbox_va)
{
    int i;
    uint32_t r = 0;

    if (!xbox_va)
        return 0;
    AcquireSRWLockShared(&g_heap_lock);
    if (xbox_HeapReclaimEnabled() && !heap_check_enabled()) {
        /* Live blocks never overlap: only the nearest block at or below
         * xbox_va can contain it. */
        i = heap_floor_index(xbox_va);
        if (i >= 0 && !g_heap_blocks[i].free &&
            xbox_va < g_heap_blocks[i].addr + g_heap_blocks[i].size)
            r = g_heap_blocks[i].size - (xbox_va - g_heap_blocks[i].addr);
    } else
    for (i = 0; i < g_heap_block_count; i++) {
        if (g_heap_blocks[i].free)
            continue;
        if (xbox_va >= g_heap_blocks[i].addr &&
            xbox_va <  g_heap_blocks[i].addr + g_heap_blocks[i].size) {
            r = g_heap_blocks[i].size - (xbox_va - g_heap_blocks[i].addr);
            break;
        }
    }
    ReleaseSRWLockShared(&g_heap_lock);
    return r;
}

void ma2_xact_on_free(uint32_t addr, uint32_t size);   /* recomp_0097.c: RECOMP_XACT_UAF free check */
volatile DWORD g_heap_free_log_until;   /* test-only [HEAP-FREE] log deadline (GetTickCount), 0 = off */
void xbox_HeapFree(uint32_t xbox_va)
{
    static int frees = 0, matched = 0;

    if (!xbox_va) {
        return;
    }
    AcquireSRWLockExclusive(&g_heap_lock);
    frees++;
    if (frees <= 8) {
        fprintf(stderr, "  [HEAP] free #%d va=0x%08X blocks=%d\n",
                frees, xbox_va, g_heap_block_count);
        fflush(stderr);
    }
    for (int i = heap_find_live(xbox_va); i >= 0; i = -1) {
        if (g_heap_blocks[i].addr != xbox_va || g_heap_blocks[i].free) {
            continue;
        }
        ma2_xact_on_free(xbox_va, g_heap_blocks[i].size);
        if (g_heap_free_log_until && (LONG)(GetTickCount() - g_heap_free_log_until) < 0) {
            /* test-only (RECOMP_TEST_HIJACK arms it for a few seconds): who frees what */
            uint32_t sp = g_esp, k, shown = 0;
            fprintf(stderr, "[HEAP-FREE] va=%08X size=%X tid=%lu ret:", xbox_va, g_heap_blocks[i].size, (unsigned long)GetCurrentThreadId());
            for (k = 0; k < 48 && shown < 8 && sp >= 0x10000 && sp < 0x03FFFF00; k++) {
                uint32_t v = *(const uint32_t *)((uintptr_t)g_xbox_mem_offset + sp + k * 4);
                if (v >= 0x00011000 && v < 0x00460000) { fprintf(stderr, " %08X", v); shown++; }
            }
            fputc('\n', stderr);
        }
        g_heap_blocks[i].free = 1;
        if (++matched % 512 == 0) {
            fprintf(stderr, "  [HEAP] frees=%d matched=%d blocks=%d\n",
                    frees, matched, g_heap_block_count);
            fflush(stderr);
        }

        /* Coalesce with neighbours. Blocks are recorded in bump order, so
         * index order is address order and adjacency is a simple end==start
         * test. Keeps large contiguous requests satisfiable after a lot of
         * small churn. Under RECOMP_HEAP_RECLAIM the neighbour search steps
         * over the empty slots (size 0) earlier merges leave behind; without
         * that, one merge stops every later one dead. */
        {
            int n = i + 1, p = i - 1;
            if (xbox_HeapReclaimEnabled()) {
                while (n < g_heap_block_count && g_heap_blocks[n].size == 0) n++;
                while (p >= 0 && g_heap_blocks[p].size == 0) p--;
            }
            if (n < g_heap_block_count && g_heap_blocks[n].free &&
                g_heap_blocks[i].addr + g_heap_blocks[i].size == g_heap_blocks[n].addr) {
                g_heap_blocks[i].size += g_heap_blocks[n].size;
                g_heap_blocks[n].size = 0;
                g_heap_blocks[n].addr = 0;
                g_heap_hole_hint++;
            }
            if (p >= 0 && g_heap_blocks[p].free &&
                g_heap_blocks[p].addr + g_heap_blocks[p].size == g_heap_blocks[i].addr) {
                g_heap_blocks[p].size += g_heap_blocks[i].size;
                g_heap_blocks[i].size = 0;
                g_heap_blocks[i].addr = 0;
                g_heap_hole_hint++;
            }
        }
        heap_maybe_compact();
        ReleaseSRWLockExclusive(&g_heap_lock);
        return;
    }
    ReleaseSRWLockExclusive(&g_heap_lock);
}

HANDLE xbox_GetMappingHandle(void)
{
    return g_mapping_handle;
}
