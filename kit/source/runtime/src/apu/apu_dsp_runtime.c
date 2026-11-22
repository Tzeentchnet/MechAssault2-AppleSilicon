/* GP/EP integration for xemu's DSP56300 interpreter and DMA engine.
 * Interpreter: GPL-2.0-or-later; DMA and integration: LGPL-2.0-or-later.
 * Upstream provenance and licenses are in dsp/UPSTREAM.json and dsp/COPYING.
 */
#include "apu_state.h"
#include "fpconv.h"
#include "../kernel/nv2a_gpu_memory.h"
#include "../kernel/xbox_memory_layout.h"
#include "../platform/pc_cpu_topo.h"   /* pc_thread_role_enter */
#include "../platform/pc_switch.h"     /* T2-8: [Audio] Threads */
#if defined(_WIN32)
#include <avrt.h>
#endif

/* A0 counters (apu_core.c): g_apu_ws is on under RECOMP_AUDIO_WAIT_STATS / RECOMP_PERF. */
extern int g_apu_ws;
extern double apu_ws_now_ms(void);
extern void apu_wt_dspjoin(double ms);

bool mcpx_apu_dsp_enabled(void)
{
    static int enabled=-1;
    if(enabled<0) {
        const char *setting=getenv("RECOMP_APU_DSP");
        enabled=!setting || atoi(setting)!=0;
    }
    return enabled!=0;
}

static void transfer_error(DSPState *dsp)
{
    dsp->dma.error=true;
    static unsigned shown;
    if(shown++<8)fprintf(stderr,"[APU-DSP] %s rejected invalid DMA descriptors\n",dsp->is_gp?"GP":"EP");
}

/* DMA accesses physical RAM independently of the guest CPU's page permissions.
 * Notify GPU caches before writes, and retain deliberately read-only CPU pages
 * after the transfer. GPU write-watch protections are released by changed(). */
static bool physical_write(uint8_t *memory,const uint8_t *source,size_t bytes)
{
    return nv2a_gpu_memory_dma_write(memory,source,bytes)!=0;
}

static bool scatter_rw(DSPState *dsp,uint32_t table,uint32_t limit,
                       uint8_t *ptr,uint32_t address,size_t length,bool write)
{
    while(length) {
        uint32_t page=address>>12,offset=address&4095;
        size_t bytes=MIN(length,4096-offset);
        uint64_t entry=(uint64_t)table+(uint64_t)page*8;
        uint64_t physical_entry=xbox_DmaIsMain(table)?entry-XBOX_DMA_MAIN_BASE:entry;
        if(page>limit || physical_entry>0x3FFFFF8) {
            static unsigned shown;if(shown++<4)fprintf(stderr,"[APU-DSP] SG table=%08X limit=%X page=%X address=%X bytes=%zu\n",table,limit,page,address,length);
            transfer_error(dsp);if(!write)memset(ptr,0,length);return false;
        }
        uint32_t physical=ldl_le_phys(NULL,(uint32_t)entry);
        uint32_t normalized=xbox_DmaIsMain(physical)?physical-XBOX_DMA_MAIN_BASE:physical;
        if((uint64_t)normalized+offset+bytes>0x4000000) {
            static unsigned shown;if(shown++<4)fprintf(stderr,"[APU-DSP] SG physical=%08X table=%08X page=%X\n",physical,table,page);
            transfer_error(dsp);if(!write)memset(ptr,0,length);return false;
        }
        uint8_t *memory=mcpx_apu_phys(physical+offset);
        if(write) {
            if(!physical_write(memory,ptr,bytes)){transfer_error(dsp);return false;}
        } else memcpy(ptr,memory,bytes);
        ptr+=bytes;length-=bytes;address+=(uint32_t)bytes;
    }
    return true;
}

static void scratch_rw(void *opaque,uint8_t *ptr,uint32_t address,size_t length,bool write)
{
    DSPState *dsp=opaque;MCPXAPUState *d=dsp->owner;
    uint32_t table=qatomic_read(&d->regs[dsp->is_gp?NV_PAPU_GPSADDR:NV_PAPU_EPSADDR]);
    uint32_t limit=qatomic_read(&d->regs[dsp->is_gp?NV_PAPU_GPSMAXSGE:NV_PAPU_EPSMAXSGE]);
    scatter_rw(dsp,table,limit,ptr,address,length,write);
}

/* DSP headroom. The title's GP/EP programs sum many mixbins into the final
 * stereo pair in 24-bit fixed point. In dense combat that sum saturates inside
 * the EP: at the end of mission 2, 14% of output samples sat flat at full
 * scale in runs of up to 7 ms (recorded output, 2026-10-11), independent of
 * the PC volume, which is applied afterwards. The GP itself measured linear
 * (front bins pass at 1.0, XTLK bins 6/7 at 0.29). So: feed the GP 12 dB lower
 * (which also removes the float_to_24b clamp on hot mixbins), take the EP's
 * stereo output at its full 24 bits instead of the DMA's 16, restore the 12 dB
 * and soft-limit only what would still exceed full scale. Below full scale the
 * output level is unchanged. RECOMP_APU_DSP_HEADROOM=0 restores the original
 * path exactly. */
#define DSP_HEADROOM_GAIN 0.25f
#define DSP_HEADROOM_CEILING 0.99f
#define DSP_HEADROOM_RELEASE 2.083e-4f /* ~100 ms */
static int headroom_state=-1;
static const void *headroom_ep_core;
static uint32_t headroom_reads[512];
static unsigned headroom_read_count;
static bool dsp_headroom(void)
{
    if(headroom_state<0) {
        const char *value=getenv("RECOMP_APU_DSP_HEADROOM");
        headroom_state=!(value && *value && atoi(value)==0);
        fprintf(stderr,"[APU-HEADROOM] %s\n",headroom_state?
            "on (GP input -12 dB, EP output +12 dB from 24-bit, soft limit above full scale)":"off");
    }
    return headroom_state==1;
}

/* EP stereo output in, int16 out: restore the headroom gain, then a 32-sample
 * lookahead limiter (linear attack to exactly the needed gain, ~100 ms release)
 * so no sample is flattened at full scale. */
static void headroom_output(float in[256][2],int16_t out[256][2])
{
    static float delay[32][2],window[64],gain=1.0f;
    static bool primed;
    static int stats=-1;
    static unsigned frames,limited,mismatched_frames;static float min_gain=1.0f,peak;
    if(!primed){for(int i=0;i<64;i++)window[i]=1.0f;primed=true;}
    for(int c=0;c<8;c++) {
        float (*block)[2]=in+c*32;
        for(int i=0;i<32;i++) {
            float p=fmaxf(fabsf(block[i][0]),fabsf(block[i][1]));
            if(!isfinite(p)){block[i][0]=block[i][1]=0.0f;p=0.0f;}
            window[32+i]=p>DSP_HEADROOM_CEILING?DSP_HEADROOM_CEILING/p:1.0f;
            peak=fmaxf(peak,p);
        }
        for(int i=0;i<32;i++) {
            float target=1.0f;
            for(int j=0;j<=32;j++){float need=window[i+j];target=fminf(target,need+(1.0f-need)*(j/32.0f));}
            gain=fminf(target,gain+(1.0f-gain)*DSP_HEADROOM_RELEASE);
            for(int ch=0;ch<2;ch++) {
                float y=delay[i][ch]*gain*32767.0f;
                out[c*32+i][ch]=(int16_t)lrintf(fmaxf(-32768.0f,fminf(32767.0f,y)));
            }
            if(gain<0.999f)limited++;
            min_gain=fminf(min_gain,gain);
        }
        memcpy(delay,block,sizeof(delay));
        memmove(window,window+32,32*sizeof(window[0]));
    }
    if(stats<0)stats=getenv("RECOMP_AUDIO_STATS")!=NULL;
    if(stats && ++frames==938) {
        fprintf(stderr,"[APU-HEADROOM] frames=%u limited=%.2f%% min_gain=%.3f (%.1f dB) peak=%.3f (1.0 = old clip point)\n",
                frames,100.0*limited/(frames*256.0),min_gain,20.0*log10(fmax(min_gain,1e-6f)),peak);
        frames=limited=0;min_gain=1.0f;peak=0.0f;
    }
    (void)mismatched_frames;
}

/* Rebuild the EP's stereo block from the 512 24-bit words the DMA just read
 * (interleaved, sample-major), checking each against the 16-bit word the DMA
 * produced (v >> 8). On any mismatch fall back to the 16-bit data, at the
 * same restored level, for that block. */
static void headroom_ep_block(const uint8_t *ptr,int16_t out[256][2])
{
    static unsigned mismatches;
    float in[256][2];
    const uint16_t *words=(const uint16_t *)ptr;
    bool exact=headroom_read_count>=512;
    if(exact) {
        unsigned first=headroom_read_count-512;
        for(unsigned k=0;k<512 && exact;k++)
            exact=(uint16_t)(headroom_reads[(first+k)%512]>>8)==words[k];
    }
    if(exact) {
        unsigned first=headroom_read_count-512;
        for(unsigned k=0;k<512;k++) {
            int32_t v=(int32_t)(headroom_reads[(first+k)%512]<<8)>>8;
            in[k/2][k%2]=v/8388608.0f/DSP_HEADROOM_GAIN;
        }
    } else {
        if(mismatches++<4)fprintf(stderr,"[APU-HEADROOM] EP output block used 16-bit data (24-bit words unavailable or mismatched)\n");
        for(unsigned k=0;k<512;k++)in[k/2][k%2]=(int16_t)words[k]/32768.0f/DSP_HEADROOM_GAIN;
    }
    headroom_read_count=0;
    headroom_output(in,out);
}

static void fifo_rw(void *opaque,uint8_t *ptr,unsigned index,size_t length,bool write)
{
    DSPState *dsp=opaque;MCPXAPUState *d=dsp->owner;
    if(index>=(write?4u:2u)){transfer_error(dsp);return;}
    uint32_t first=dsp->is_gp?(write?NV_PAPU_GPOFBASE0:NV_PAPU_GPIFBASE0):
                                (write?NV_PAPU_EPOFBASE0:NV_PAPU_EPIFBASE0);
    uint32_t base=qatomic_read(&d->regs[first+index*16])&0xFFFFFF;
    uint32_t end=qatomic_read(&d->regs[first+index*16+4])&0xFFFFFF;
    uint32_t cur_reg=first+index*16+8;
    uint32_t current=qatomic_read(&d->regs[cur_reg])&0xFFFFFF;
    /* EP output zero is the interleaved stereo PCM stream. Keep writing the
     * guest FIFO as well, so CPU/DMA readers see the actual encoded data. */
    if(!dsp->is_gp && write && index==0 && length==sizeof(d->monitor.frame_buf) &&
       (d->monitor.point==MCPX_APU_DEBUG_MON_EP || d->monitor.point==MCPX_APU_DEBUG_MON_GP_OR_EP)) {
        if(dsp_headroom())headroom_ep_block(ptr,d->monitor.frame_buf);
        else memcpy(d->monitor.frame_buf,ptr,length);
    }
    if(end<=base){static unsigned shown;if(shown++<4)fprintf(stderr,"[APU-DSP] %s FIFO %u base=%X end=%X current=%X bytes=%zu\n",dsp->is_gp?"GP":"EP",index,base,end,current,length);transfer_error(dsp);if(!write)memset(ptr,0,length);return;}
    if(current<base)current=base;
    if(current>=end)current=base+(current-base)%(end-base);
    uint32_t table=qatomic_read(&d->regs[dsp->is_gp?NV_PAPU_GPFADDR:NV_PAPU_EPFADDR]);
    uint32_t limit=qatomic_read(&d->regs[dsp->is_gp?NV_PAPU_GPFMAXSGE:NV_PAPU_EPFMAXSGE]);
    while(length) {
        size_t bytes=MIN(length,end-current);
        if(!scatter_rw(dsp,table,limit,ptr,current,bytes,write))return;
        current+=(uint32_t)bytes;ptr+=bytes;length-=bytes;
        if(current==end)current=base;
    }
    qatomic_set(&d->regs[cur_reg],(qatomic_read(&d->regs[cur_reg])&0xFF000000)|current);
    /* Audio the CPU streams straight into the GP would not have been scaled
     * down, so the restored gain would make it 12 dB too loud. */
    if(dsp->is_gp && !write && headroom_state==1) {
        static unsigned shown;
        if(shown++<4)fprintf(stderr,"[APU-HEADROOM] GP input FIFO %u read (%zu bytes)\n",index,length);
    }
}

static uint32_t mem_read(void *opaque,int space,uint32_t address)
{
    uint32_t value=dsp56k_read_memory(opaque,space,address);
    if(opaque==headroom_ep_core)headroom_reads[headroom_read_count++%512]=value;
    return value;
}
static void mem_write(void *opaque,int space,uint32_t address,uint32_t value)
{ dsp56k_write_memory(opaque,space,address,value); }

static uint32_t peripheral_read(dsp_core_t *core,uint32_t address)
{
    DSPState *dsp=core->opaque;
    switch(address) {
    case 0xFFFFB3:return 0;
    case 0xFFFFC5:return dsp->interrupts|(dsp->dma.eol?1u<<7:0);
    case 0xFFFFD4:return dsp_dma_read(&dsp->dma,DMA_NEXT_BLOCK);
    case 0xFFFFD5:return dsp_dma_read(&dsp->dma,DMA_START_BLOCK);
    case 0xFFFFD6:return dsp_dma_read(&dsp->dma,DMA_CONTROL);
    case 0xFFFFD7:return dsp_dma_read(&dsp->dma,DMA_CONFIGURATION);
    default:return 0xABABA;
    }
}
static void peripheral_write(dsp_core_t *core,uint32_t address,uint32_t value)
{
    DSPState *dsp=core->opaque;
    switch(address) {
    case 0xFFFFC4:if(value&1)core->is_idle=true;break;
    case 0xFFFFC5:dsp->interrupts&=~value;if(value&(1u<<7))dsp->dma.eol=false;break;
    case 0xFFFFD4:dsp_dma_write(&dsp->dma,DMA_NEXT_BLOCK,value);break;
    case 0xFFFFD5:dsp_dma_write(&dsp->dma,DMA_START_BLOCK,value);break;
    case 0xFFFFD6:dsp_dma_write(&dsp->dma,DMA_CONTROL,value);break;
    case 0xFFFFD7:dsp_dma_write(&dsp->dma,DMA_CONFIGURATION,value);break;
    }
}

/* Held by the DSP thread while it runs a GP/EP frame; a reset waits for that frame. */
static SRWLOCK g_dsp_exec=SRWLOCK_INIT;

void mcpx_apu_dsp_reset_program(DSPState *dsp)
{
    if(!dsp)return;
    AcquireSRWLockExclusive(&g_dsp_exec);
    dsp56k_reset_cpu(&dsp->core);
    dsp->save_cycles=0;dsp->interrupts=0;
    dsp->core.is_idle=true;dsp->core.cycle_count=0;
    memset((void *)dsp->core.pram_opcache,0,sizeof(dsp->core.pram_opcache));
    dsp->dma.configuration=dsp->dma.control=dsp->dma.start_block=dsp->dma.next_block=0;
    dsp->dma.error=dsp->dma.eol=false;dsp->dma.dma_read_count=0;
    ReleaseSRWLockExclusive(&g_dsp_exec);
}

/* RECOMP_STALL_PROFILE=1: find where long frames go. A watcher thread counts
 * advances of the title's frame counter (guest 0x4E2420). A save's progress
 * callback still advances it a few times per second, so "stalled" means fewer
 * than 10 advances in the last 500 ms. While stalled, each thread is suspended
 * every ~2 ms, its call stack is unwound (up to 32 frames) and resumed. For a
 * stall of at least 1 s, each thread with code in this image prints its top
 * "self" addresses (innermost image frame) and "incl" addresses (any image
 * frame), as image RVAs for lookup in the linker map. Diagnostic only: idle
 * unless stalled; no guest state is touched. */
#if defined(_WIN32)
#include <tlhelp32.h>
#define STALL_THREADS 64
#define STALL_SLOTS 4096
typedef struct { uint32_t rva; unsigned self, incl, seen; } StallSlot;
static struct { DWORD tid; HANDLE h; unsigned samples, image_samples, used; StallSlot s[STALL_SLOTS]; } stall_t[STALL_THREADS];
static unsigned stall_threads, stall_pass;
/* RECOMP_STALL_PROFILE=1 samples below 10 frame advances per 500 ms; =N (N > 1) below N, so a
 * large N samples all the time and reports every 10 s (profiling ordinary frames). */
static unsigned stall_min = 10;
static StallSlot *stall_slot(int t, uint32_t rva)
{
    for (unsigned j = 0; j < stall_t[t].used; j++) if (stall_t[t].s[j].rva == rva) return &stall_t[t].s[j];
    if (stall_t[t].used >= STALL_SLOTS) return NULL;
    StallSlot *x = &stall_t[t].s[stall_t[t].used++]; memset(x, 0, sizeof(*x)); x->rva = rva; return x;
}
static void stall_sample(int t, uint64_t base, uint64_t size)
{
    CONTEXT c; memset(&c, 0, sizeof(c)); c.ContextFlags = CONTEXT_FULL;
    uint32_t frames[32]; unsigned n = 0;
    if (SuspendThread(stall_t[t].h) == (DWORD)-1) return;
    if (GetThreadContext(stall_t[t].h, &c)) {
        for (unsigned depth = 0; depth < 32 && c.Rip; depth++) {
            if (c.Rip >= base && c.Rip < base + size) frames[n++] = (uint32_t)(c.Rip - base);
            DWORD64 image = 0; PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &image, NULL);
            if (!fn) { if (!c.Rsp) break; c.Rip = *(DWORD64 *)(uintptr_t)c.Rsp; c.Rsp += 8; continue; }
            PVOID handler_data; DWORD64 establisher;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, fn, &c, &handler_data, &establisher, NULL);
        }
    }
    ResumeThread(stall_t[t].h);
    stall_t[t].samples++;
    if (!n) return;
    stall_t[t].image_samples++; stall_pass++;
    for (unsigned i = 0; i < n; i++) {
        StallSlot *x = stall_slot(t, frames[i]);
        if (!x) continue;
        if (i == 0) x->self++;
        if (x->seen != stall_pass) { x->seen = stall_pass; x->incl++; }
    }
}
static void stall_print_top(int t, int self)
{
    for (int r = 0; r < 16; r++) {
        unsigned best = 0; StallSlot *b = NULL;
        for (unsigned j = 0; j < stall_t[t].used; j++) {
            StallSlot *x = &stall_t[t].s[j]; unsigned v = self ? x->self : x->incl;
            if (v > best && !(x->seen & 0x80000000u)) { best = v; b = x; }
        }
        if (!b) break;
        fprintf(stderr, " %08X:%u", b->rva, best); b->seen |= 0x80000000u;
    }
    for (unsigned j = 0; j < stall_t[t].used; j++) stall_t[t].s[j].seen &= 0x7FFFFFFFu;
}
static void stall_report(const char *state, uint32_t first, uint32_t last, ULONGLONG ms)
{
    fprintf(stderr, "[STALL] %s: frames %u -> %u during %llu ms\n", state, first, last, (unsigned long long)ms);
    for (unsigned t = 0; t < stall_threads; t++) {
        if (stall_t[t].image_samples < 10) continue;
        fprintf(stderr, "[STALL]  tid=%lu samples=%u in_image=%u self:", stall_t[t].tid, stall_t[t].samples, stall_t[t].image_samples);
        stall_print_top((int)t, 1);
        fprintf(stderr, "\n[STALL]  tid=%lu incl:", stall_t[t].tid);
        stall_print_top((int)t, 0);
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

static DWORD WINAPI stall_profiler(void *opaque)
{
    const volatile uint32_t *frame = (const volatile uint32_t *)opaque;
    pc_thread_role_enter(PC_ROLE_DIAG);
    uint64_t base = (uint64_t)(uintptr_t)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(uintptr_t)(base + ((IMAGE_DOS_HEADER *)(uintptr_t)base)->e_lfanew);
    uint64_t size = nt->OptionalHeader.SizeOfImage;
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    unsigned reports = 0; uint32_t ring[10] = {0}; unsigned at = 0;
    fprintf(stderr, "[STALL] profiler v3 armed: <%u advances of guest 0x4E2420 per 500 ms, report >= 1000 ms, hangs every 10 s\n", stall_min);
    for (unsigned i = 0; i < 10; i++) ring[i] = *frame;
    for (;;) {
        Sleep(50);
        uint32_t now = *frame; uint32_t old = ring[at]; ring[at] = now; at = (at + 1) % 10;
        if (now - old >= stall_min) continue;
        /* Stalled: open every thread once, then sample until the rate recovers. */
        ULONGLONG since = GetTickCount64(); uint32_t first = now;
        memset(stall_t, 0, sizeof(stall_t)); stall_threads = 0; stall_pass = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te; te.dwSize = sizeof(te);
            for (BOOL ok = Thread32First(snap, &te); ok && stall_threads < STALL_THREADS; ok = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
                if (!h) continue;
                stall_t[stall_threads].tid = te.th32ThreadID; stall_t[stall_threads++].h = h;
            }
            CloseHandle(snap);
        }
        uint32_t window[25] = {0}; unsigned w = 0, filled = 0; ULONGLONG tick = GetTickCount64(), interim = tick;
        for (;;) {
            for (unsigned t = 0; t < stall_threads; t++) stall_sample((int)t, base, size);
            Sleep(2);
            if (GetTickCount64() - tick >= 20) {
                tick = GetTickCount64(); window[w] = *frame; w = (w + 1) % 25; if (filled < 25) filled++;
                if (filled == 25 && window[(w + 24) % 25] - window[w] >= stall_min) break;
            }
            /* A hang never ends, so report it while it lasts: every 10 s,
             * print what was sampled since the last report and start over. */
            if (GetTickCount64() - interim >= 10000 && reports < 40) {
                reports++; interim = GetTickCount64();
                stall_report("ongoing", first, *frame, interim - since);
                for (unsigned t = 0; t < stall_threads; t++) {
                    stall_t[t].samples = stall_t[t].image_samples = stall_t[t].used = 0;
                }
            }
        }
        ULONGLONG ms = GetTickCount64() - since;
        if (ms >= 1000 && reports < 40) {
            reports++;
            stall_report("ended", first, *frame, ms);
        }
        for (unsigned t = 0; t < stall_threads; t++) CloseHandle(stall_t[t].h);
        for (unsigned i = 0; i < 10; i++) ring[i] = *frame;
    }
    return 0;
}
#endif

void mcpx_apu_dsp_native_init(MCPXAPUState *d)
{
    for(unsigned p=0;p<2;p++) {
        DSPState *dsp=p?d->ep.dsp:d->gp.dsp;
        if(!dsp)continue;
        dsp->is_gp=p==0;dsp->owner=d;dsp->core.is_gp=dsp->is_gp;
        dsp->core.opaque=dsp;
        if(!dsp->is_gp)headroom_ep_core=&dsp->core;
        dsp->core.read_peripheral=peripheral_read;dsp->core.write_peripheral=peripheral_write;
        dsp->dma.mem_opaque=&dsp->core;dsp->dma.mem_read=mem_read;dsp->dma.mem_write=mem_write;
        dsp->dma.rw_opaque=dsp;dsp->dma.scratch_rw=scratch_rw;dsp->dma.fifo_rw=fifo_rw;
        if(d->ram_ptr) {
#if defined(_WIN32)
            uintptr_t base=(uintptr_t)d->ram_ptr+(p?0xFE850000u:0xFE830000u);
#else
            void *alias = xbox_McpxMemoryAlias(p ? 0x50000u : 0x30000u, 0x10000u);
            if (!alias) {
                fprintf(stderr, "[APU] %s DSP host memory alias unavailable\n", p ? "EP" : "GP");
                continue;
            }
            uintptr_t base = (uintptr_t)alias;
#endif
            dsp->core.mapped_xram=(uint32_t *)base;
            dsp->core.mapped_yram=(uint32_t *)(base+0x6000);
            dsp->core.mapped_pram=(uint32_t *)(base+0xA000);
            dsp->core.mapped_mixbuffer=(uint32_t *)(base+0x5000);
        }
        mcpx_apu_dsp_reset_program(dsp);
    }
}

void mcpx_apu_dsp_finalize(MCPXAPUState *d)
{
    if(d->gp.dsp){free(d->gp.dsp->dma.scratch_buf);free(d->gp.dsp);d->gp.dsp=NULL;}
    if(d->ep.dsp){free(d->ep.dsp->dma.scratch_buf);free(d->ep.dsp);d->ep.dsp=NULL;}
}

/* T2-7 step 1 (A3b dump): RECOMP_APU_DSP_DUMP=<dir> writes each distinct GP/EP program image as
 * <dir>\gp_<hash>.pram / ep_<hash>.pram (the PRAM words, little-endian u32, DSP_PRAM_SIZE of them)
 * and logs [APU-DSP-DUMP] once per new image. Checked on the 1st idle frame after a reset and then
 * every 1500th frame (1 s), so a program swapped in later is caught too. Read-only on the DSP. */
static void dsp_program_dump(DSPState *dsp)
{
    static int on=-1;static const char *dir;
    static uint64_t seen[2][16];static unsigned nseen[2];
    if(on<0){dir=getenv("RECOMP_APU_DSP_DUMP");on=dir&&*dir;}
    if(!on || !dsp->core.is_idle || (dsp->executed_frames!=1 && dsp->executed_frames%1500))return;
    const uint32_t *p=dsp->core.mapped_pram?dsp->core.mapped_pram:dsp->core.pram;
    uint64_t h=1469598103934665603ull;unsigned used=0;
    for(unsigned i=0;i<DSP_PRAM_SIZE;i++){uint32_t w=p[i]&0xFFFFFF;h=(h^w)*1099511628211ull;if(w)used=i+1;}
    int k=dsp->is_gp?0:1;
    for(unsigned i=0;i<nseen[k];i++)if(seen[k][i]==h)return;
    if(nseen[k]<16)seen[k][nseen[k]++]=h;
    char path[1024];snprintf(path,sizeof(path),"%s\\%s_%016llx.pram",dir,k?"ep":"gp",(unsigned long long)h);
    FILE *f=fopen(path,"wb");
    if(f){for(unsigned i=0;i<DSP_PRAM_SIZE;i++){uint32_t w=p[i]&0xFFFFFF;fwrite(&w,4,1,f);}fclose(f);}
    fprintf(stderr,"[APU-DSP-DUMP] %s program %016llx: %u words used (top), frame %llu, %s\n",k?"EP":"GP",
            (unsigned long long)h,used,(unsigned long long)dsp->executed_frames,f?path:"write failed");
}

static void run_frame(DSPState *dsp)
{
    dsp->interrupts|=1u<<1;dsp->core.is_idle=false;dsp->core.cycle_count=0;
    unsigned limit=dsp->is_gp?250000:2000000;
    while(!dsp->core.is_idle && dsp->core.cycle_count<limit) {
        dsp56k_execute_instruction(&dsp->core);
        dsp->core.cycle_count+=dsp->core.instr_cycle;
    }
    dsp->executed_frames++;
    dsp_program_dump(dsp);
    if(!dsp->core.is_idle && dsp->budget_exhaustions++<8)
        fprintf(stderr,"[APU-DSP] %s frame budget exceeded: PC=%04X instruction=%06X cycles=%u\n",
                dsp->is_gp?"GP":"EP",dsp->core.pc,dsp->core.cur_inst,dsp->core.cycle_count);
}

/* Optional, bounded recordings on either side of the DSP distinguish source
 * discontinuities from processing errors without altering the audio path.
 * Each record starts with its 64-bit VP frame, then 32x32 float mix samples
 * or 2x32 raw 24-bit GP output words. */
static void capture_pipeline(MCPXAPUState *d,float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    static int enabled=-1, finished;
    static uint64_t begin, end;
    static FILE *mix_file, *gp_file, *state_file;
    if(enabled<0) {
        const char *prefix=getenv("RECOMP_APU_PIPELINE_CAPTURE");
        enabled=prefix && *prefix;
        const char *start=getenv("RECOMP_APU_PIPELINE_START");
        const char *duration=getenv("RECOMP_APU_PIPELINE_SECONDS");
        unsigned seconds=duration?(unsigned)atoi(duration):30;
        if(seconds<1 || seconds>300)seconds=30;
        begin=(uint64_t)(start?strtoul(start,NULL,10):45)*1500;
        end=begin+(uint64_t)seconds*1500;
    }
    if(!enabled || finished || (uint64_t)d->ep_frame_div<begin)return;
    if((uint64_t)d->ep_frame_div>=end) {
        if(mix_file)fclose(mix_file);
        if(gp_file)fclose(gp_file);
        if(state_file)fclose(state_file);
        mix_file=gp_file=NULL;finished=1;return;
    }
    if(!mix_file) {
        char path[4096];const char *prefix=getenv("RECOMP_APU_PIPELINE_CAPTURE");
        snprintf(path,sizeof(path),"%s-mix.f32",prefix);mix_file=fopen(path,"wb");
        snprintf(path,sizeof(path),"%s-gp.u32",prefix);gp_file=fopen(path,"wb");
        snprintf(path,sizeof(path),"%s-state.u32",prefix);state_file=fopen(path,"wb");
        if(!mix_file || !gp_file) {
            if(mix_file)fclose(mix_file);
            if(gp_file)fclose(gp_file);
            if(state_file)fclose(state_file);
            mix_file=gp_file=NULL;finished=1;return;
        }
        fprintf(stderr,"[APU-CAPTURE] pipeline frames=%llu..%llu prefix=%s\n",
            (unsigned long long)begin,(unsigned long long)end,prefix);
    }
    uint64_t frame=(uint64_t)d->ep_frame_div;
    uint32_t gp[2][NUM_SAMPLES_PER_FRAME];
    for(unsigned ch=0;ch<2;ch++)for(unsigned i=0;i<NUM_SAMPLES_PER_FRAME;i++)
        gp[ch][i]=dsp56k_read_memory(&d->gp.dsp->core,DSP_SPACE_X,
            DSP_MIXBUFFER_BASE+ch*NUM_SAMPLES_PER_FRAME+i);
    fwrite(&frame,1,sizeof(frame),mix_file);
    fwrite(mixbins,1,sizeof(float)*NUM_MIXBINS*NUM_SAMPLES_PER_FRAME,mix_file);
    fwrite(&frame,1,sizeof(frame),gp_file);fwrite(gp,1,sizeof(gp),gp_file);
    if(state_file) {
        uint32_t state[2]={d->regs[NV_PAPU_SECTL],d->regs[NV_PAPU_FECTL]};
        fwrite(&frame,1,sizeof(frame),state_file);fwrite(state,1,sizeof(state),state_file);
    }
}

/* Pre-GP bus limiter. float_to_24b hard-clamps each mixbin at full scale on
 * its way into the GP. In dense combat the title's own mix exceeds that (bins
 * 6/7 reach 3.5x: 30-50 3D voices per frame at unity gain, measured with
 * RECOMP_APU_MIX_TRACE), and the clamp is the audible crackle. One gain shared
 * by all 32 bins keeps their balance, so the GP/EP programs see the same mix,
 * only quieter for the moment. One block (32 samples, 0.67 ms) of lookahead
 * ramps the gain linearly down to exactly the level each sample needs, so no
 * sample exceeds the ceiling and the gain never steps; release is ~100 ms.
 * Below the ceiling the audio passes unchanged (apart from the delay).
 * Opt-in with RECOMP_APU_LIMITER=1: the human found it no better than the
 * hard clamp (2026-10-11), so the default stays the original behaviour. */
#define BUS_LIMIT_CEILING 0.98f
#define BUS_LIMIT_RELEASE 2.083e-4f /* 1 - exp(-1 / (0.1 s * 48000)) */
static bool bus_limiter_enabled(void)
{
    static int enabled=-1;
    if(enabled<0) {
        const char *value=getenv("RECOMP_APU_LIMITER");
        enabled=value && *value && atoi(value)!=0;
        fprintf(stderr,"[APU-LIMIT] pre-GP bus limiter %s\n",enabled?"on (ceiling 0.98)":"off (hard clamp)");
    }
    return enabled!=0;
}

static void bus_limit(float in[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME],
                      float out[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    enum { N=NUM_SAMPLES_PER_FRAME };
    static float delay[NUM_MIXBINS][N], window[2*N], gain=1.0f;
    static bool primed;
    static int stats=-1;
    static unsigned frames,limited;static float min_gain=1.0f,input_peak;
    if(!primed){for(int i=0;i<2*N;i++)window[i]=1.0f;primed=true;}
    for(int i=0;i<N;i++) {
        float peak=0.0f;
        for(int b=0;b<NUM_MIXBINS;b++) {
            if(!isfinite(in[b][i]))in[b][i]=0.0f;
            peak=fmaxf(peak,fabsf(in[b][i]));
        }
        window[N+i]=peak>BUS_LIMIT_CEILING?BUS_LIMIT_CEILING/peak:1.0f;
        input_peak=fmaxf(input_peak,peak);
    }
    for(int i=0;i<N;i++) {
        /* window[i] belongs to the delayed sample being output now. */
        float target=1.0f;
        for(int j=0;j<=N;j++) {
            float need=window[i+j];
            target=fminf(target,need+(1.0f-need)*((float)j/N));
        }
        gain=fminf(target,gain+(1.0f-gain)*BUS_LIMIT_RELEASE);
        for(int b=0;b<NUM_MIXBINS;b++)out[b][i]=delay[b][i]*gain;
        if(gain<0.999f)limited++;
        min_gain=fminf(min_gain,gain);
    }
    memcpy(delay,in,sizeof(delay));
    memcpy(window,window+N,N*sizeof(window[0]));
    if(stats<0)stats=getenv("RECOMP_AUDIO_STATS")!=NULL;
    if(stats && ++frames==7500) {
        fprintf(stderr,"[APU-LIMIT] frames=%u limited=%.2f%% min_gain=%.3f (%.1f dB) input_peak=%.3f\n",
                frames,100.0*limited/(frames*(double)N),min_gain,20.0*log10(fmax(min_gain,1e-6f)),input_peak);
        frames=limited=0;min_gain=1.0f;input_peak=0.0f;
    }
}

/* [APU-TIME] every 5 s of wall time (RECOMP_PERF or RECOMP_AUDIO_STATS): APU frames made (7500 is
 * real time; fewer means the audio fell behind and XAudio2 starves -- crackle), and where the time
 * went: voice processing on the APU thread (apu_vp.c, with active voices per frame), and the GP
 * program (with the 32 mixbins written into it) and EP program on the DSP thread below. */
extern long long g_apu_vp_ticks;
extern unsigned long long g_apu_vp_voices;
extern unsigned g_apu_vp_voices_max;
static volatile long long g_dsp_gp_ticks,g_dsp_ep_ticks;
static void dsp_threads_report(double wall_ms);   /* T2-8, below */
static void apu_time_frame(int threaded)
{
    static int on=-1;static LARGE_INTEGER f,last;static long long vp0,gp0,ep0;static unsigned long long frames,voices0;
    if(on<0){on=getenv("RECOMP_PERF")||getenv("RECOMP_AUDIO_STATS");QueryPerformanceFrequency(&f);}
    if(!on)return;
    LARGE_INTEGER now;QueryPerformanceCounter(&now);
    if(!last.QuadPart){last=now;vp0=g_apu_vp_ticks;voices0=g_apu_vp_voices;gp0=g_dsp_gp_ticks;ep0=g_dsp_ep_ticks;}
    frames++;
    if((now.QuadPart-last.QuadPart)>=5*f.QuadPart) {
        double ms=1000.0/(double)f.QuadPart,wall=(now.QuadPart-last.QuadPart)*ms;
        long long vp=g_apu_vp_ticks-vp0,gp=g_dsp_gp_ticks-gp0,ep=g_dsp_ep_ticks-ep0;
        unsigned long long voices=g_apu_vp_voices-voices0;
        fprintf(stderr,"[APU-TIME] %.1f s: %llu frames (%.0f%% of real time); VP %.0f ms (%.1f voices/frame, max %u), GP %.0f ms, EP %.0f ms; %s\n",
                wall/1000.0,frames,frames*0.6666667/wall*100.0,vp*ms,frames?(double)voices/frames:0.0,g_apu_vp_voices_max,
                gp*ms,ep*ms,threaded?"DSP on its own thread":"DSP inline");
        dsp_threads_report(wall);
        last=now;vp0=g_apu_vp_ticks;voices0=g_apu_vp_voices;gp0=g_dsp_gp_ticks;ep0=g_dsp_ep_ticks;g_apu_vp_voices_max=0;frames=0;
    }
}

/* GP and EP run here: the mixbins go into the GP, it runs one frame, and every eighth frame the
 * EP runs and DMAs its 256-sample block into d->monitor.frame_buf. */
static uint64_t g_ep_runs,g_ep_cycles;
static void dsp_run_frame(MCPXAPUState *d,float feed[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME],
                          bool gp,bool ep_on,bool ep_run,unsigned div)
{
    LARGE_INTEGER t0,t1,t2;QueryPerformanceCounter(&t0);t1=t2=t0;
    if(gp) {
        float scale=dsp_headroom()?DSP_HEADROOM_GAIN:1.0f;
        for(unsigned b=0;b<NUM_MIXBINS;b++)for(unsigned i=0;i<NUM_SAMPLES_PER_FRAME;i++)
            dsp56k_write_memory(&d->gp.dsp->core,DSP_SPACE_X,DSP_MIXBUFFER_BASE+b*NUM_SAMPLES_PER_FRAME+i,
                               float_to_24b(feed[b][i]*scale));
        run_frame(d->gp.dsp);g_dbg.gp.cycles=d->gp.dsp->core.cycle_count;
        if(d->monitor.point==MCPX_APU_DEBUG_MON_GP ||
           (d->monitor.point==MCPX_APU_DEBUG_MON_GP_OR_EP && !ep_on)) {
            unsigned off=(div%8)*NUM_SAMPLES_PER_FRAME;
            for(unsigned i=0;i<NUM_SAMPLES_PER_FRAME;i++)for(unsigned ch=0;ch<2;ch++)
                d->monitor.frame_buf[off+i][ch]=(int16_t)(dsp56k_read_memory(&d->gp.dsp->core,DSP_SPACE_X,
                    DSP_MIXBUFFER_BASE+ch*NUM_SAMPLES_PER_FRAME+i)>>8);
        }
        QueryPerformanceCounter(&t1);t2=t1;
    }
    if(ep_run) {
        run_frame(d->ep.dsp);g_dbg.ep.cycles=d->ep.dsp->core.cycle_count;
        g_ep_runs++;g_ep_cycles+=g_dbg.ep.cycles;
        QueryPerformanceCounter(&t2);
    }
    g_dsp_gp_ticks+=t1.QuadPart-t0.QuadPart;g_dsp_ep_ticks+=t2.QuadPart-t1.QuadPart;
}

/* The DSPs on their own thread (2026-10-15). The APU thread did everything in turn each 0.67 ms
 * frame: voices, then the GP program (about half a core by itself, constant), then the EP. In a
 * big fight on a laptop the sum went past a core, the audio fell to 53-70% of real time and
 * XAudio2 starved into constant popping. Now the APU thread mixes frame n+1's voices while this
 * thread runs frame n's GP and EP, which splits the load over two cores.
 *
 * Ordering is unchanged: the GP sees each frame's mixbins in order and the EP runs after the GP
 * of its frame. The output is safe one frame late: the EP writes its block into frame_buf on
 * frame 8k, and mcpx_apu_monitor_frame sends frame_buf on frame 8k+7; the APU thread waits for
 * this thread before submitting the next frame, so a block is complete before it is read and
 * the next EP (frame 8k+8) cannot start before that read. A GP/EP reset from the title waits for
 * the frame in progress (mcpx_apu_dsp_reset_program takes g_dsp_exec).
 * RECOMP_APU_DSP_THREAD=0 runs them inline as before; so does a pipeline capture. */
static struct {
    int mode;                       /* -1 not decided, 0 inline, 1 threaded */
    HANDLE go,done;
    int pending;
    MCPXAPUState *d;
    bool gp,ep_on,ep_run;
    unsigned div;
    float in[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
} dsp_thread={-1,NULL,NULL,0};

/* T2-8 A8 thread plan (PIPELINE-C 4.6): [Audio] Threads / RECOMP_APU_THREADS = auto|high|low|inline,
 * default auto. Only scheduling changes; the GP/EP see the same inputs in the same order in every
 * mode, so the output is the same.
 *   high   = the DSP thread at MMCSS Pro Audio HIGH (priority ~25), the old path.
 *   low    = the DSP thread at plain NORMAL priority (8, dynamic boosts off), so it never preempts
 *            the game or executor threads (also 8); it takes a CPU when one is free or at a
 *            quantum end. At 4 logical CPUs the t2p1 sampler found the DSP running in 72 % of the
 *            moments the game thread was runnable without a CPU.
 *            Guard: when the APU falls behind real time by more than 2 EP blocks (10.7 ms; XAudio2
 *            holds about 64 ms), the DSP thread goes back to MMCSS HIGH until the lag has stayed
 *            under one block for 2 s ([APU-THREADS] promotions=).
 *   inline = GP/EP on the APU thread (as RECOMP_APU_DSP_THREAD=0; the 10-15 laptop overload case).
 *   auto   = low at <=4 logical CPUs (pc_cpu_logical, affinity mask applied), high above.
 * The APU (VP) thread stays MMCSS HIGH in every plan: it feeds XAudio2 and runs the guest ISR, and
 * the game thread waits on it for NEW_VOICE. */
enum { DSP_PLAN_HIGH=0, DSP_PLAN_LOW=1, DSP_PLAN_INLINE=2 };
static int g_dsp_plan=-1;
static volatile LONG g_dsp_prio_high=1;   /* low plan: 1 while the lag guard has promoted the DSP thread */
static struct { unsigned promotions; long long high_us; int64_t ok_since_us,promoted_at_us; } g_dsp_guard;
static int dsp_plan_choose(void)
{
    static const char *const names[]={"high","low","inline","auto"};
    int v=pc_switch_choice("RECOMP_APU_THREADS","Audio","Threads",names,4,3);
    unsigned cpus=pc_cpu_logical();
    if(v==3)v=cpus<=4?DSP_PLAN_LOW:DSP_PLAN_HIGH;
#if defined(_WIN32)
    fprintf(stderr,"[APU] threads: cpus=%u plan=%s (APU thread MMCSS HIGH%s)\n",cpus,
            v==DSP_PLAN_LOW?"low":v==DSP_PLAN_INLINE?"inline":"high",
            v==DSP_PLAN_LOW?"; DSP thread NORMAL, no boost, lag guard":v==DSP_PLAN_INLINE?"; GP/EP inline":"; DSP thread MMCSS HIGH");
#else
    fprintf(stderr,"[APU] threads: cpus=%u plan=%s (host scheduling; MMCSS unavailable)\n",cpus,
            v==DSP_PLAN_LOW?"low":v==DSP_PLAN_INLINE?"inline":"high");
#endif
    if(v==DSP_PLAN_LOW)g_dsp_prio_high=0;
    return v;
}
static void dsp_thread_set_prio(HANDLE *mmcss,int high)
{
#if defined(_WIN32)
    HANDLE self=GetCurrentThread();
    if(high) {
        if(!*mmcss) {
            DWORD task_index=0;
            *mmcss=AvSetMmThreadCharacteristicsW(L"Pro Audio",&task_index);
            if(*mmcss)AvSetMmThreadPriority(*mmcss,AVRT_PRIORITY_HIGH);
        }
        SetThreadPriorityBoost(self,FALSE);
    } else {
        if(*mmcss){AvRevertMmThreadCharacteristics(*mmcss);*mmcss=NULL;}
        SetThreadPriority(self,THREAD_PRIORITY_NORMAL);
        SetThreadPriorityBoost(self,TRUE);   /* TRUE = boosts off: stays at 8 when the go event wakes it */
    }
#else
    (void)mmcss;
    (void)high;
#endif
}
/* APU thread, every 8th frame, low plan only: promote/demote the DSP thread from the real-time lag. */
static void dsp_guard_tick(MCPXAPUState *d)
{
    int64_t now=qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    int64_t lag=now-d->next_frame_time_us+5333;   /* throttle already advanced next_frame_time_us by one block */
    if(!d->next_frame_time_us)return;
    if(!g_dsp_prio_high) {
        if(lag>2*5333){g_dsp_prio_high=1;g_dsp_guard.promotions++;g_dsp_guard.promoted_at_us=now;g_dsp_guard.ok_since_us=0;}
    } else {
        if(lag>5333){g_dsp_guard.ok_since_us=0;return;}
        if(!g_dsp_guard.ok_since_us)g_dsp_guard.ok_since_us=now;
        else if(now-g_dsp_guard.ok_since_us>=2000000) {
            g_dsp_guard.high_us+=now-g_dsp_guard.promoted_at_us;
            g_dsp_prio_high=0;g_dsp_guard.ok_since_us=0;
        }
    }
}

static void dsp_threads_report(double wall_ms)
{
    if(g_dsp_plan!=DSP_PLAN_LOW)return;
    int64_t now=qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    long long high_us=g_dsp_guard.high_us+(g_dsp_prio_high&&g_dsp_guard.promoted_at_us?now-g_dsp_guard.promoted_at_us:0);
    if(g_dsp_prio_high&&g_dsp_guard.promoted_at_us)g_dsp_guard.promoted_at_us=now;
    fprintf(stderr,"[APU-THREADS] %.1f s: plan=low promotions=%u high=%.0f ms now=%s\n",wall_ms/1000.0,
            g_dsp_guard.promotions,high_us/1000.0,g_dsp_prio_high?"high":"low");
    g_dsp_guard.promotions=0;g_dsp_guard.high_us=0;
}

static DWORD WINAPI dsp_thread_main(LPVOID unused)
{
    (void)unused;
    pc_thread_role_enter(PC_ROLE_DSP);
    HANDLE mmcss=NULL;
    int prio=-1;
    for(;;) {
        WaitForSingleObject(dsp_thread.go,INFINITE);
        int want=g_dsp_plan==DSP_PLAN_LOW?(int)g_dsp_prio_high:1;
        if(want!=prio){dsp_thread_set_prio(&mmcss,want);prio=want;}
        AcquireSRWLockExclusive(&g_dsp_exec);
        dsp_run_frame(dsp_thread.d,dsp_thread.in,dsp_thread.gp,dsp_thread.ep_on,dsp_thread.ep_run,dsp_thread.div);
        ReleaseSRWLockExclusive(&g_dsp_exec);
        SetEvent(dsp_thread.done);
    }
}
static void dsp_thread_join(void)
{
    if(dsp_thread.pending) {
        double t0=g_apu_ws?apu_ws_now_ms():0.0;     /* [APU-WAIT] dspjoin: the APU thread waiting for the GP/EP frame */
        WaitForSingleObject(dsp_thread.done,INFINITE);dsp_thread.pending=0;
        if(g_apu_ws)apu_wt_dspjoin(apu_ws_now_ms()-t0);
    }
}
static int dsp_thread_mode(MCPXAPUState *d)
{
    if(dsp_thread.mode>=0)return dsp_thread.mode;
    const char *setting=getenv("RECOMP_APU_DSP_THREAD"),*capture=getenv("RECOMP_APU_PIPELINE_CAPTURE");
    dsp_thread.mode=0;
    if(g_dsp_plan<0)g_dsp_plan=dsp_plan_choose();
    if((setting && atoi(setting)==0) || (capture && *capture) || g_dsp_plan==DSP_PLAN_INLINE)return 0;
    dsp_thread.d=d;
    dsp_thread.go=CreateEventW(NULL,FALSE,FALSE,NULL);
    dsp_thread.done=CreateEventW(NULL,FALSE,FALSE,NULL);
    HANDLE thread=dsp_thread.go && dsp_thread.done?CreateThread(NULL,0,dsp_thread_main,NULL,0,NULL):NULL;
    if(thread){CloseHandle(thread);dsp_thread.mode=1;}
    fprintf(stderr,"[APU-DSP] GP/EP %s\n",dsp_thread.mode?"run on their own thread, one frame behind the voices":"run inline");
    return dsp_thread.mode;
}

/* [APU-DSP] GP PRAM (A0): the title loads GP programs with plain stores into the mapped PRAM
 * (0xFE83A000...), which nothing intercepts, so there is no single place to count writes. Instead,
 * every 8th APU frame (the DSP is at rest after the join) the GP PRAM is compared with the previous
 * sample: words that changed (net, so a rewrite of the same value is not seen). APU thread only. */
static struct { unsigned samples, changed_samples, words, max_words; } pram_stats;
static void pram_sample(MCPXAPUState *d)
{
    static uint32_t snap[sizeof(((dsp_core_t *)0)->pram)/sizeof(uint32_t)];
    static bool have;
    if(!d->gp.dsp || !d->gp.dsp->core.mapped_pram || d->ep_frame_div%8)return;
    const uint32_t *p=d->gp.dsp->core.mapped_pram;
    unsigned changed=0;
    for(unsigned i=0;i<sizeof(snap)/sizeof(snap[0]);i++)if(p[i]!=snap[i]){snap[i]=p[i];changed++;}
    if(!have){have=true;return;}        /* the first sample only sets the baseline */
    pram_stats.samples++;
    if(changed){pram_stats.changed_samples++;pram_stats.words+=changed;if(changed>pram_stats.max_words)pram_stats.max_words=changed;}
}
void apu_dsp_pram_report(double secs)
{
    fprintf(stderr,"[APU-DSP] GP PRAM %.1f s: %u samples (every 8th APU frame), %u with changes, %u words changed, max %u per sample\n",
            secs,pram_stats.samples,pram_stats.changed_samples,pram_stats.words,pram_stats.max_words);
    memset(&pram_stats,0,sizeof(pram_stats));
}

bool mcpx_apu_dsp_native_frame(MCPXAPUState *d,float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
#if defined(_WIN32)
    {
        static int started; /* first frame: guest RAM is mapped */
        static int v_init; static const char *v; /* getenv once: runs every DSP frame */
        if(!v_init){v=getenv("RECOMP_STALL_PROFILE");v_init=1;}
        if(!started && v && atoi(v)>=1 && d->ram_ptr) {
            if(atoi(v)>1)stall_min=(unsigned)atoi(v);
            started=1;
            HANDLE t=CreateThread(NULL,0,stall_profiler,(void *)(d->ram_ptr+0x4E2420u),0,NULL);
            if(t)CloseHandle(t);
        }
    }
#else
    {
        static int reported;
        const char *profile = getenv("RECOMP_STALL_PROFILE");
        if (!reported && profile && atoi(profile) >= 1) {
            fprintf(stderr, "[STALL] Windows thread sampling is unavailable on this host\n");
            reported = 1;
        }
    }
#endif
    /* The previous frame's GP/EP first: everything after this sees the DSPs at rest. */
    dsp_thread_join();
    if(g_apu_ws)pram_sample(d);
    if(!mcpx_apu_dsp_enabled())return false;
    bool gp=(qatomic_read(&d->gp.regs[NV_PAPU_GPRST])&3)==3;
    bool ep=(qatomic_read(&d->ep.regs[NV_PAPU_EPRST])&3)==3;
    if(!gp && !ep)return false;
    static float limited[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    float (*feed)[NUM_SAMPLES_PER_FRAME]=mixbins;
    if(gp && d->gp.dsp && bus_limiter_enabled()){bus_limit(mixbins,limited);feed=limited;}
    bool gp_run=gp && d->gp.dsp,ep_run=ep && d->ep.dsp && d->ep_frame_div%8==0;
    int threaded=dsp_thread_mode(d);
    if(threaded && g_dsp_plan==DSP_PLAN_LOW && d->ep_frame_div%8==0)dsp_guard_tick(d);
    if(threaded) {
        if(gp_run)memcpy(dsp_thread.in,feed,sizeof(dsp_thread.in));
        dsp_thread.gp=gp_run;dsp_thread.ep_on=ep;dsp_thread.ep_run=ep_run;dsp_thread.div=d->ep_frame_div;
        dsp_thread.pending=1;SetEvent(dsp_thread.go);
    } else {
        dsp_run_frame(d,feed,gp_run,ep,ep_run,d->ep_frame_div);
        if(d->gp.dsp)capture_pipeline(d,mixbins);
    }
    apu_time_frame(threaded);
    static int stats=-1;
    if(stats<0)stats=getenv("RECOMP_AUDIO_STATS")!=NULL;
    if(stats) {
        static uint64_t frames,cycles;static uint32_t peak;
        frames++;cycles+=g_dbg.gp.cycles;
        if((uint32_t)g_dbg.gp.cycles>peak)peak=(uint32_t)g_dbg.gp.cycles;
        if(frames==7500) {
            fprintf(stderr,"[APU-DSP] GP cycles avg=%llu peak=%u frames=%llu EP frames=%llu EP cycles avg=%llu budgets=%llu/%llu reset=%X/%X\n",
                (unsigned long long)(cycles/frames),peak,(unsigned long long)d->gp.dsp->executed_frames,
                (unsigned long long)d->ep.dsp->executed_frames,(unsigned long long)(g_ep_runs?g_ep_cycles/g_ep_runs:0),
                (unsigned long long)d->gp.dsp->budget_exhaustions,
                (unsigned long long)d->ep.dsp->budget_exhaustions,d->gp.regs[NV_PAPU_GPRST],d->ep.regs[NV_PAPU_EPRST]);
            frames=cycles=0;peak=0;g_ep_runs=g_ep_cycles=0;
        }
    }
    return true;
}
