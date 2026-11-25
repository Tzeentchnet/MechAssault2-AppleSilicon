/**
 * Execute the parts of the title's pushbuffer that produce visible pixels.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; without
 * something consuming them the framebuffer stays whatever it was, which is how
 * a fully booted title renders a black screen. This walks the same command
 * stream nv2a_pb_scan.c surveys and carries out the subset that decides what is
 * on screen: which surface is being drawn into, and clearing it.
 *
 * It also rasterises geometry, but only the part that can be drawn honestly:
 * batches whose attribute 0 is already in screen space, flat-shaded, straight
 * into the same guest framebuffer the clear writes. Titles draw their UI, HUD
 * and 2D overlays that way, so it is the first geometry to appear. Batches that
 * need a vertex program executed are counted and skipped rather than drawn
 * somewhere wrong -- see raster_batch(). Texturing, depth and vertex programs
 * are still a renderer, not a command decoder; the upgrade path is the D3D11
 * translator in src/nv2a/nv2a_pgraph_d3d11.c.
 *
 * Everything this does not handle is counted and ranked by
 * nv2a_pb_exec_report(), so what remains is a list rather than a guess.
 *
 * Enabled with RECOMP_PB_EXEC. RECOMP_RASTER_TEST draws one known triangle
 * after every clear, which separates "the pixel path is broken" from "the title
 * has not given us any vertices". RECOMP_FB_DUMP=<prefix> writes the surface to
 * <prefix>NNN.bmp, so the result can be looked at without a display.
 */
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if !defined(_WIN32)
#include <pthread.h>
#include <unistd.h>
#endif
#include "kernel.h"   /* XBOX_CONTIG_BASE / XBOX_CONTIG_SIZE */
#include "xbox_memory_layout.h"   /* xbox_Nv2aFrameCounterFlip */
#include "../platform/pc_cpu_topo.h"   /* T2-0: thread roles, pc_cpu_logical() */
/* The swizzle decoder the D3D8 layer already uses -- one implementation of
 * Morton order, not a second one that can disagree with it. */
#include "../d3d/d3d8_swizzle.h"
#include "nv2a_vsh_interp.h"
#include "nv2a_combiner.h"
#include "nv2a_gpu_d3d11.h"
#include "nv2a_backend_dispatch.h"
#include "nv2a_gpu_wait.h"
#include "nv2a_vertex_layout.h"
#include "nv2a_index_stream.h"
#include "nv2a_texture.h"
#include "nv2a_texture_pack.h"
#include "nv2a_pb_objects.h"
#include "../platform/pc_frontend.h"

/* Keep the existing D3D11 path as the default. A registered renderer receives
 * the same complete MA2 state and may decline a draw for the CPU fallback. */
#define nv2a_gpu_enabled nv2a_dispatch_enabled
#define nv2a_gpu_discard_frame nv2a_dispatch_discard_frame
#define nv2a_gpu_program_pending nv2a_dispatch_program_pending
#define nv2a_gpu_clear nv2a_dispatch_clear
#define nv2a_gpu_draw nv2a_dispatch_draw
#define nv2a_gpu_draw_program_indexed nv2a_dispatch_draw_program_indexed
#define nv2a_gpu_draw_buffers nv2a_dispatch_draw_buffers
#define nv2a_gpu_sync_all nv2a_dispatch_sync_all
#define nv2a_gpu_readback nv2a_dispatch_readback
#define nv2a_gpu_invalidate nv2a_dispatch_invalidate
#define nv2a_gpu_surface_info nv2a_dispatch_surface_info
/* Lightning (B2, 2026-10-26): the unit shaders light with a sun-colour vertex constant that the game uploads once, so
 * the env's lightning flash light never reached 'Mechs, vehicles and pilots. recomp_0028 (env update) sets
 * g_lt_flash_on while a flash is on and keeps g_lt_sun_col = the env's sun colour (env+0x88) between flashes; the draw
 * path adds the flash to the constant holding that colour for that draw only. Nothing changes without a flash.
 * R27 tint3: the add used to match by value in EVERY draw, so terrain (its shader passes the sun colour straight into
 * V1 for the light map), billboards, bushes and stale constant slots pulsed too and the whole screen flickered through
 * several shades per strike. Now only a constant the program reads directly AND uses in a MAD of a program with a
 * DP3 against a constant (the N.L model lighting of 'Mechs, vehicles, pilots, props) gets the flash.
 * RECOMP_LIGHTNING_UNITS=0 off, =2 the old match-by-value behaviour (A/B). */
volatile int g_lt_flash_on;
float g_lt_sun_col[3];
static const float g_lt_flash_add = 0.9f;

#if defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>
#define NV_TLS __declspec(thread)
#else
#define NV_TLS __thread
#define _InterlockedIncrement(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch);
extern void xbox_FramebufferWindowStart(void);
extern uint32_t g_xbox_image_lo, g_xbox_image_hi;

/* Optional wall-clock measurements. Time whole batches, not individual pixels,
 * so profiling does not change the renderer's inner loops. */
static struct {
    double start, draw_ms, vertex_ms, parallel_ms;
    uint32_t frames, draws, parallel_jobs;
    float frame_intervals[32768];
    unsigned interval_count,over_16_ms,over_33_ms;
    double longest_interval;
    /* Visibility-test traffic: how often the title counts, resets and reads. */
    uint32_t zpass_draws, zpass_toggles, report_clears, report_gets, report_nonzero;
} s_perf;

enum { PERF_DRAW_SAMPLES=8 };
typedef struct {
    uint32_t primitive,count,point_valid,point_raw,point_class,point_subch;
    uint32_t blend,src,dst,equation,depth,depth_func,depth_write,color_write;
    uint32_t alpha,alpha_func,alpha_ref,context,device,captured;
    Nv2aGpuTexture textures[4];
    uint64_t frame,draw_serial,program_hash;
    int program_known;
} PerfDrawSignature;
static NV_TLS struct {
    uint64_t frames,point_writes,draws[5],vertices[5],overflow;
    uint32_t point_valid,point_raw,point_class,point_subch,count;
    PerfDrawSignature samples[PERF_DRAW_SAMPLES];
} s_draw_perf;
static void perf_report_cpu(void);

static double perf_clock_ms(void)
{
    return nv2a_gpu_cpu_begin();
}

static int compare_frame_interval(const void *a,const void *b)
{
    float first=*(const float *)a,second=*(const float *)b;
    return (first>second)-(first<second);
}

/* ---- Hitch tracer (W2 push 2026-10-27; env-only, default off) ----
 * RECOMP_HITCH=<ms> (e.g. 25): per presented frame, logs frames slower than <ms> as
 *   [HITCH] t=+S.SSSs dt=MS frame=N phase=ms/calls ...   (render-thread CPU phases from the PERF counters)
 * and a sampler thread that, while the next flip is overdue, suspends every other thread and prints where
 * each is ([HITCH-S] ... rip=<module>+off stk: +RVA ...; resolve RVAs with the .map). nv2a_hitch_mark("match-start")
 * (called by the bot tick when the match clock restarts) opens a 10 s window; its
 *   [HITCH-SUM] window=10s frames=N worst=MS over33=N over50=N
 * is printed when the window closes. t is seconds since the mark (negative before it). */
/* Log privacy (kernel_thunks.c): a path for a log line, with the player's profile folders written as
 * %LOCALAPPDATA% / %APPDATA% / %USERPROFILE% (players post logs publicly). */
const char *xbox_log_path(const char *path, char *out, size_t size);
const char *xbox_log_pathw(const wchar_t *path, char *out, size_t size);
void xbox_log_redact(char *text, size_t size);
static struct {
    int on, inited; double thresh_ms; LARGE_INTEGER freq;
    volatile LONG64 last_flip_q; volatile LONG64 mark_q; LONG64 base_q;
    volatile LONG serial;
    unsigned frames, over33, over50, over25, lines; double worst, worst_at; int reported;
} s_hitch;
static double hitch_ms_since(LONG64 q0){LARGE_INTEGER n;QueryPerformanceCounter(&n);return (double)(n.QuadPart-q0)*1000.0/(double)s_hitch.freq.QuadPart;}
static LONG64 hitch_q(void){LARGE_INTEGER n;QueryPerformanceCounter(&n);return n.QuadPart;}
int nv2a_hitch_enabled(void)
{
    if(!s_hitch.inited){
        const char *e=getenv("RECOMP_HITCH");
        s_hitch.inited=1;QueryPerformanceFrequency(&s_hitch.freq);s_hitch.base_q=hitch_q();
        if(e&&*e){s_hitch.on=1;s_hitch.thresh_ms=atof(e);if(s_hitch.thresh_ms<5)s_hitch.thresh_ms=25;}
    }
    return s_hitch.on;
}
/* Seconds since the match-start mark (negative before it, 0 when never marked). */
double nv2a_hitch_rel_s(void)
{
    LONG64 m=s_hitch.mark_q;
    if(!s_hitch.on||!m)return 0;
    return hitch_ms_since(m)/1000.0;
}
void nv2a_hitch_mark(const char *what)
{
    if(!nv2a_hitch_enabled())return;
    if(what&&!strcmp(what,"match-start")){
        s_hitch.mark_q=hitch_q();s_hitch.frames=s_hitch.over33=s_hitch.over50=s_hitch.over25=0;
        s_hitch.worst=s_hitch.worst_at=0;s_hitch.reported=0;
    }
    fprintf(stderr,"[HITCH] mark %s rel=%.3fs\n",what?what:"?",nv2a_hitch_rel_s());
}
void nv2a_hitch_note(const char *what,double ms)
{
    if(!nv2a_hitch_enabled())return;
    fprintf(stderr,"[HITCH-N] abs=%.0fms t=%+.3fs %s %.2fms\n",hitch_ms_since(s_hitch.base_q),nv2a_hitch_rel_s(),what,ms);
}
#if defined(_WIN32)
static DWORD WINAPI hitch_sampler(LPVOID unused)
{
    (void)unused;
    pc_thread_role_enter(PC_ROLE_DIAG);
    uintptr_t base=(uintptr_t)GetModuleHandleA(NULL);
    DWORD self=GetCurrentThreadId(),pid=GetCurrentProcessId();
    LONG64 seen=0;int samples=0;
    static char out[65536];
    for(;;){
        Sleep(3);
        LONG64 lf=s_hitch.last_flip_q,m=s_hitch.mark_q;
        if(!lf||!m)continue;
        if(lf!=seen){seen=lf;samples=0;continue;}
        double rel=hitch_ms_since(m)/1000.0;
        if(rel<0||rel>40.0)continue;
        double stalled=hitch_ms_since(lf);
        if(samples>=14||stalled<s_hitch.thresh_ms+samples*35.0)continue;
        int o=0;
        o+=snprintf(out+o,sizeof out-(size_t)o,"[HITCH-S] t=%+.3fs stalled=%.0fms sample=%d\n",rel,stalled,samples);
        HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
        if(snap==INVALID_HANDLE_VALUE)continue;
        THREADENTRY32 te;te.dwSize=sizeof te;
        for(BOOL ok=Thread32First(snap,&te);ok&&o<(int)sizeof out-1500;ok=Thread32Next(snap,&te)){
            if(te.th32OwnerProcessID!=pid||te.th32ThreadID==self)continue;
            HANDLE th=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT,FALSE,te.th32ThreadID);
            if(!th)continue;
            if(SuspendThread(th)!=(DWORD)-1){
                CONTEXT c;memset(&c,0,sizeof c);c.ContextFlags=CONTEXT_FULL;
                if(GetThreadContext(th,&c)){
                    HMODULE mod=NULL;char name[48]="?";
                    if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)c.Rip,&mod)&&mod){
                        char full[MAX_PATH];
                        if(GetModuleFileNameA(mod,full,sizeof full)){const char *bs=strrchr(full,'\\');snprintf(name,sizeof name,"%s",bs?bs+1:full);}
                    }
                    o+=snprintf(out+o,sizeof out-(size_t)o,"[HITCH-S] tid=%lu rip=%s+%llX stk:",(unsigned long)te.th32ThreadID,name,
                                (unsigned long long)(c.Rip-(uintptr_t)(mod?mod:(HMODULE)0)));
                    const uintptr_t *sp=(const uintptr_t *)c.Rsp;
                    MEMORY_BASIC_INFORMATION mbi;
                    size_t avail=VirtualQuery(sp,&mbi,sizeof mbi)?(size_t)((uintptr_t)mbi.BaseAddress+mbi.RegionSize-(uintptr_t)sp)/sizeof(uintptr_t):0;
                    for(size_t k=0,shown=0;k<avail&&k<1024&&shown<14&&o<(int)sizeof out-64;k++){
                        uintptr_t v=sp[k];
                        if(v>base+0x1000&&v<base+0x8000000u){o+=snprintf(out+o,sizeof out-(size_t)o," +%llX",(unsigned long long)(v-base));shown++;}
                    }
                    o+=snprintf(out+o,sizeof out-(size_t)o,"\n");
                }
                ResumeThread(th);
            }
            CloseHandle(th);
        }
        CloseHandle(snap);
        DWORD wr;WriteFile(GetStdHandle(STD_ERROR_HANDLE),out,(DWORD)o,&wr,NULL);
        samples++;
    }
    return 0;
}
#endif
static void hitch_flip(void)
{
    static const char *const phase_names[NV2A_CPU_PHASE_COUNT]={
        "texture_prepare","texture_resource","texture_srv","pixel_miss","program_miss",
        "shader_resource","buffer_upload","fetch_resource","vertex_fallback",
        "pack_digest","pack_read","pack_resource","pack_srv","pack_select","present","pacer"};
    static LONG64 prev_q;static unsigned long long frame_no;static int started;
    LONG64 q=hitch_q();
    Nv2aCpuSnapshot snap;nv2a_gpu_cpu_take(&snap);
    double dt=prev_q?(double)(q-prev_q)*1000.0/(double)s_hitch.freq.QuadPart:0;
    prev_q=q;frame_no++;
    s_hitch.last_flip_q=q;
    if(!started){
        started=1;
#if defined(_WIN32)
        HANDLE t=CreateThread(NULL,0,hitch_sampler,NULL,0,NULL);if(t)CloseHandle(t);
#else
        fprintf(stderr,"[HITCH] Windows thread sampling is unavailable on this host\n");
#endif
    }
    LONG64 m=s_hitch.mark_q;
    double rel=m?(double)(q-m)*1000.0/(double)s_hitch.freq.QuadPart/1000.0:0;
    if(m&&rel>=0&&rel<=10.0&&dt>0){
        s_hitch.frames++;
        if(dt>1000.0/40)s_hitch.over25++;
        if(dt>1000.0/30)s_hitch.over33++;
        if(dt>50.0)s_hitch.over50++;
        if(dt>s_hitch.worst){s_hitch.worst=dt;s_hitch.worst_at=rel;}
    }
    if(m&&rel>10.0&&!s_hitch.reported){
        s_hitch.reported=1;
        fprintf(stderr,"[HITCH-SUM] window=10s frames=%u worst=%.1fms at=%.2fs over25=%u over33=%u over50=%u\n",
                s_hitch.frames,s_hitch.worst,s_hitch.worst_at,s_hitch.over25,s_hitch.over33,s_hitch.over50);
    }
    if(dt>=s_hitch.thresh_ms&&s_hitch.lines<400&&(!m||rel<40.0)){
        char line[1024];unsigned used=0;
        used+=snprintf(line+used,sizeof line-used,"[HITCH] abs=%.0fms t=%+.3fs dt=%.1fms frame=%llu",hitch_ms_since(s_hitch.base_q),rel,dt,frame_no);
        for(unsigned i=0;i<NV2A_CPU_PHASE_COUNT&&used<sizeof line-80;i++){
            const Nv2aCpuCounter *c=&snap.phases[i];
            if(c->total_ms>=0.4&&i!=NV2A_CPU_PACER&&i!=NV2A_CPU_PRESENT)
                used+=snprintf(line+used,sizeof line-used," %s=%.1fms/%llu(max %.1f)",phase_names[i],c->total_ms,(unsigned long long)c->calls,c->max_ms);
        }
        fprintf(stderr,"%s\n",line);s_hitch.lines++;
    }
}

static void perf_frame(void)
{
    static double previous_flip;
    if (nv2a_hitch_enabled()) hitch_flip();
    double now = perf_clock_ms();
    if (!now || !getenv("RECOMP_PERF")) return;
    s_draw_perf.frames++;
    if(previous_flip) {
        double interval=now-previous_flip;
        if(s_perf.interval_count<sizeof(s_perf.frame_intervals)/sizeof(*s_perf.frame_intervals))
            s_perf.frame_intervals[s_perf.interval_count++]=(float)interval;
        if(interval>1000.0/60)s_perf.over_16_ms++;
        if(interval>1000.0/30)s_perf.over_33_ms++;
        if(interval>s_perf.longest_interval)s_perf.longest_interval=interval;
    }
    previous_flip=now;
    if (!s_perf.start) s_perf.start = now;
    s_perf.frames++;
    if (now - s_perf.start >= 5000) {
        double elapsed = now - s_perf.start;
        fprintf(stderr, "[PERF] %.2f fps; GPU draws %.1f ms/frame (%u batches); "
                "vertex %.1f ms/frame; parallel raster %.1f ms/frame (%u jobs)\n",
                s_perf.frames * 1000.0 / elapsed,
                s_perf.draw_ms / s_perf.frames, s_perf.draws,
                s_perf.vertex_ms / s_perf.frames,
                s_perf.parallel_ms / s_perf.frames, s_perf.parallel_jobs);
        if(s_perf.interval_count) {
            unsigned n=s_perf.interval_count;
            qsort(s_perf.frame_intervals,n,sizeof(*s_perf.frame_intervals),compare_frame_interval);
            fprintf(stderr,"[PERF-PACING] %u frames; median %.3f ms; p95 %.3f ms; p99 %.3f ms; "
                "max %.3f ms; over 16.7 ms %u; over 33.3 ms %u\n",n,
                s_perf.frame_intervals[n/2],s_perf.frame_intervals[(n-1)*95/100],
                s_perf.frame_intervals[(n-1)*99/100],s_perf.longest_interval,
                s_perf.over_16_ms,s_perf.over_33_ms);
        }
        /* Keep the existing PERF/PERF-PACING pair adjacent for the capture
         * reader. Extra recognized-tag evidence follows that complete pair. */
        fprintf(stderr,"[PERF-VISIBILITY] per_frame counted_batches=%.1f toggles=%.1f "
            "clears=%.1f reports=%.1f nonzero_reports=%.1f\n",
            (double)s_perf.zpass_draws/s_perf.frames,(double)s_perf.zpass_toggles/s_perf.frames,
            (double)s_perf.report_clears/s_perf.frames,(double)s_perf.report_gets/s_perf.frames,
            (double)s_perf.report_nonzero/s_perf.frames);
        perf_report_cpu();
        memset(&s_perf, 0, sizeof(s_perf));
        s_perf.start = now;
    }
}

/* Would writing this surface land on the title's own image?
 *
 * NV097_SET_SURFACE_COLOR_OFFSET is an offset within the colour DMA object,
 * not a guest virtual address, and this executor has always used it as one.
 * That is harmless while the two happen to agree and catastrophic when they do
 * not: the Xbox Dashboard names surface 0x00088000 at 1280x960x4, so clearing
 * it wrote 4.9 MB of opaque black from 0x00088000 to 0x00538000 -- straight
 * over its own code, its D3D context at 0x000BBFC0 and the register-block
 * pointer at 0x000BE2C4. The symptom was a title that submitted one perfect
 * frame and then spun forever in a pushbuffer-full loop, three layers away,
 * with every D3D global reading 0xFF000000: the clear colour.
 *
 * So refuse, and say so. Getting the address right needs the DMA object base
 * this ignores (NV097_SET_CONTEXT_DMA_COLOR); until that exists, writing
 * nothing is strictly better than writing over the guest, and a title that
 * cannot draw is easier to debug than one that has been overwritten.
 */
static int surface_hits_image(uint32_t base, uint32_t bytes)
{
    if (!g_xbox_image_hi || !bytes)
        return 0;
    return base < g_xbox_image_hi && base + bytes > g_xbox_image_lo;
}

/* Where a DMA-object offset actually lives.
 *
 * NV097_SET_SURFACE_COLOR_OFFSET is an offset inside the colour DMA object,
 * and for a framebuffer that object covers physical memory -- so the offset
 * is a physical address, not a guest VA. Those are the same number in this
 * runtime, which is why treating it as a VA works until it does not: on
 * hardware the image is mapped at VA 0x00010000 from arbitrary physical
 * pages, so a framebuffer at physical 0x84000 does not overlap it. Here it
 * would.
 *
 * The title tells us which it is by where it allocated. Half-Life 2's
 * framebuffer comes from MmAllocateContiguousMemory, which this runtime
 * serves from the window at XBOX_CONTIG_BASE, so physical P is visible at
 * XBOX_CONTIG_BASE + P -- clear of the image, and the same bytes the title's
 * own writes and the framebuffer window reach.
 *
 * So: use the offset as a VA when that is credible, and fall back to the
 * physical mirror exactly when it is not. Titles whose surfaces already sit
 * in ordinary RAM (Wreckless renders to the tiled alias of physical
 * 0x01954000) keep the first path and are unaffected.
 */
static uint32_t dma_resolve(uint32_t offset)
{
    extern uint32_t xbox_ContiguousAllocatedBytes(void);
    if(xbox_DmaIsMain(offset))return offset-XBOX_DMA_MAIN_BASE;

    /* Did this runtime hand the offset out as contiguous memory? Then the
     * bytes live in the window, and that is not a guess: the arena is a bump
     * allocator from XBOX_CONTIG_BASE, so everything below its high-water
     * mark is memory some MmAllocateContiguousMemory call returned. The
     * title's own writes go through the window, so the executor's must too.
     *
     * Checking this BEFORE the image test is the whole point. The image test
     * only catches an offset that would land on the title's code, and whether
     * it does is an accident of where the image happens to end: Half-Life 2's
     * colour surface is physical 0x00A6C000, which clears the image by 700 KB.
     * So it looked like an ordinary VA, and the executor cleared 1.2 MB of
     * black straight through the guest heap -- which faulted the title three
     * frames later on a pointer that had been overwritten, while the real
     * framebuffer at 0x80A6C000 stayed untouched and the screen stayed black. */
    if (offset < xbox_ContiguousAllocatedBytes())
        return XBOX_CONTIG_BASE + offset;
    if (!surface_hits_image(offset, 1))
        return offset;
    if ((uint64_t)offset < XBOX_CONTIG_SIZE)
        return XBOX_CONTIG_BASE + offset;
    return offset;                         /* nothing better to offer */
}

static int surface_write_refused(uint32_t base, uint32_t bytes, const char *what)
{
    static int said;

    if (!surface_hits_image(base, bytes))
        return 0;
    if (!said) {
        said = 1;
        fprintf(stderr,
                "  [GPU] REFUSING to %s surface 0x%08X..0x%08X: that overlaps "
                "the loaded image (0x%08X..0x%08X).\n"
                "  [GPU]   SET_SURFACE_COLOR_OFFSET is a DMA-object offset, not "
                "a guest VA, and this executor treats it as one. Writing here "
                "would destroy the title's own code and globals.\n",
                what, base, base + bytes, g_xbox_image_lo, g_xbox_image_hi);
        fflush(stderr);
    }
    return 1;
}

/* NV097 methods this executor acts on. */
/* Blending. The pair this title programs, read from its own pushbuffer
 * rather than guessed: BLEND_ENABLE written 1168 times and left on,
 * SFACTOR 0x0302 (SRC_ALPHA) and DFACTOR 0x0303 (ONE_MINUS_SRC_ALPHA).
 * ALPHA_TEST_ENABLE is written 390 times and left at zero, so this is
 * blending and not an alpha test. */
#define NV097_SET_BLEND_ENABLE            0x0304
#define NV097_SET_BLEND_FUNC_SFACTOR      0x0344
#define NV097_SET_BLEND_FUNC_DFACTOR      0x0348
#define NV_BLEND_SRC_ALPHA                0x0302
#define NV_BLEND_ONE_MINUS_SRC_ALPHA      0x0303
#define NV097_SET_BLEND_COLOR             0x034C
#define NV097_SET_BLEND_EQUATION          0x0350

#define NV097_SET_SURFACE_CLIP_HORIZONTAL 0x0200
#define NV097_SET_SURFACE_CLIP_VERTICAL   0x0204
#define NV097_SET_SURFACE_FORMAT          0x0208
#define NV097_SET_SURFACE_PITCH           0x020C
#define NV097_SET_SURFACE_COLOR_OFFSET    0x0210
#define NV097_SET_COLOR_CLEAR_VALUE       0x1D90
#define NV097_CLEAR_SURFACE               0x1D94
#define NV097_SET_VERTEX_DATA_ARRAY_OFFSET 0x1720   /* +i*4, 16 attributes */
#define NV097_SET_VERTEX_DATA_ARRAY_FORMAT 0x1760   /* +i*4 */
#define NV097_SET_BEGIN_END               0x17FC
#define NV097_SET_TEXTURE_OFFSET          0x1B00   /* +i*0x40 */
#define NV097_SET_TEXTURE_FORMAT          0x1B04
#define NV097_SET_TEXTURE_PALETTE         0x1B20   /* offset | size<<2 | dma */
#define NV097_SET_TEXTURE_ADDRESS         0x1B08
#define NV097_SET_TEXTURE_CONTROL1        0x1B10
#define NV097_SET_TEXTURE_IMAGE_RECT      0x1B1C
/* The buffer flip. A title double-buffers by telling the GPU which buffer
 * the CRTC reads and which it draws into, advancing the write index and
 * then stalling until the flip has happened. Ignoring these means the
 * stall never clears: Half-Life 2's loader submits its initialisation,
 * asks for a flip, and waits for it in a loop that makes no kernel calls
 * and burns no dispatch, which reads as a hang with no cause.
 *
 * ponytail: the flip completes the moment it is asked for, because there is
 * no scanout to be in the middle of. That makes every frame land instantly
 * and a title that paces itself on the flip runs as fast as it can draw.
 * Pacing wants the vblank clock in the kernel, not a sleep in here. */
#define NV097_SET_FLIP_READ               0x0120
#define NV097_SET_FLIP_WRITE              0x0124
#define NV097_SET_FLIP_MODULO             0x0128
#define NV097_FLIP_INCREMENT_WRITE        0x012C
#define NV097_FLIP_STALL                  0x0130
#define NV097_ARRAY_ELEMENT16             0x1800
#define NV097_ARRAY_ELEMENT32             0x1808
/* Draw a run of vertices straight out of the arrays, with no index list:
 * bits 0..23 are the first vertex, bits 24..31 the count minus one. It may
 * appear several times inside one BEGIN_END to draw a longer run. */
#define NV097_DRAW_ARRAYS                 0x1810
#define NV097_INLINE_ARRAY                0x1818
/* Immediate-mode vertices. SET_VERTEX3F/4F carry the position, and writing
 * its last component completes a vertex using whatever the SET_VERTEX_DATA*
 * registers currently hold for the other attributes. This is how Half-Life
 * 2's Xbox loader and the game's own 2D drawing submit every quad -- neither
 * uses INLINE_ARRAY -- so without these the executor saw SET_BEGIN_END pairs
 * with nothing attached and reported `draws 0` while a million and a half
 * textured quads a minute went past it. */
#define NV097_SET_VERTEX3F                0x1500   /* +0..0x08, 3 floats */
#define NV097_SET_VERTEX4F                0x1518   /* +0..0x0C, 4 floats */
#define NV097_SET_VERTEX_DATA2F_M         0x1880   /* + attr*8,  2 floats */
#define NV097_SET_VERTEX_DATA4F_M         0x1A00   /* + attr*16, 4 floats */
#define NV097_SET_VERTEX_DATA4UB          0x1940   /* + attr*4,  4 x u8 */
#define NV097_SET_VERTEX_DATA2S           0x1900   /* + attr*4,  2 x s16 */
#define NV097_SET_VERTEX_DATA4S_M         0x1980   /* + attr*8,  4 x s16 */

/* One immediate vertex, as this file packs it for the shared draw path:
 * position float4, diffuse D3DCOLOR, texcoord0 float2. */
/* An immediate-mode vertex: all 16 attributes as float4. */
#define IMM_VERTEX_DWORDS (NV_VERTEX_ATTRS * 4)

#define NV097_CLEAR_COLOR_MASK            0xF0   /* R,G,B,A bits */

/* One vertex attribute stream, as the title describes it. Attribute 0 is
 * position; the rest are colours, texture coordinates and so on. */
typedef struct {
    uint32_t offset;      /* guest address of element 0 */
    uint32_t type;        /* NV097 data type nibble */
    uint32_t size;        /* components per element */
    uint32_t stride;      /* bytes between elements */
} VertexAttr;

#define NV_VERTEX_ATTRS 16
#define NV_MAX_INDICES  NV2A_INDEX_BATCH_SIZE
#define NV_MAX_INLINE   65536           /* dwords of INLINE_ARRAY per batch */

/* Texture stage 0, decoded from what the title programmed.
 *
 * Only stage 0: it is the only one the dashboard configures, and a stage
 * nothing writes to is a stage nothing can be sampled from. The rest arrive
 * as unhandled methods and are counted as such, which is how the next title
 * that needs them will say so. */
typedef Nv2aGpuTexture Texture;

static struct {
    VertexAttr attr[NV_VERTEX_ATTRS];
    uint32_t   prim;                    /* SET_BEGIN_END parameter, 0 = ended */
    Nv2aIndexStream indices;
    /* INLINE_ARRAY payload: vertices written straight into the pushbuffer
     * instead of into a buffer the title points at. Same vertex format, a
     * different place to read them from. */
    uint32_t   inline_buf[NV_MAX_INLINE];
    uint32_t   inline_count;
    /* Current values of the immediate-mode attributes, and how many complete
     * vertices they have produced in this batch. */
    /* Immediate mode: every attribute's current value, as the SET_VERTEX
     * methods leave it. Writing attribute 0 (position) emits a vertex. */
    float      imm_attr[NV_VERTEX_ATTRS][4];
    uint16_t   imm_used;                /* attributes written since BEGIN */
    uint32_t   imm_count;
    int        inline_active;
    uint32_t   draws, verts, nonzero_draws;
    float      min_x, max_x, min_y, max_y;
    uint32_t color_offset, color_base, pitch, format;
    /* The surface the last batch actually drew into. A double-buffered title
     * has already pointed color_offset at the next buffer and cleared it by
     * the time the flip arrives, so dumping the current one dumps the frame
     * that has not been drawn yet -- which is how a correctly rendered
     * sequence came out as 12 black BMPs. */
    uint32_t drawn_offset;
    /* ...and the shape of that surface. A title also draws into small
     * render targets (Burnout 3: 128x128 shadow/reflection maps, pitch 512),
     * often after the main scene, so "last surface drawn" alone picked one of
     * those and read it with the framebuffer's pitch: horizontal noise. The
     * biggest surface drawn since the last flip is the one being presented. */
    uint32_t drawn_pitch, drawn_x, drawn_y, drawn_w, drawn_h, drawn_bpp;
    int drawn_stale;      /* set at a flip: the next draw starts a new frame */
    uint64_t pixels;
    uint32_t pixel_max;   /* brightest value any pixel write carried */
    uint32_t clip_x, clip_w, clip_y, clip_h;
    uint32_t clear_color;
    uint32_t clears, unhandled_total;
    uint32_t flip_read, flip_write, flip_modulo, flips;
    uint32_t tris_drawn, tris_skipped_offscreen, batches_untransformed;
    /* Why a batch came out flat. "Untextured" has two causes that look
     * identical on screen and want opposite fixes: the batch carried no
     * texture coordinates, or it did and the stage was not usable. */
    uint32_t batches_textured, batches_no_uv, batches_no_tex;
    uint32_t blend_enable, blend_sfactor, blend_dfactor;
    uint32_t blend_equation, blend_color;
    uint32_t color_mask;        /* SET_COLOR_MASK: A<<24 R<<16 G<<8 B */
    /* Visibility tests (D3D's Begin/EndVisibilityTest): pixels that pass the
     * depth test while counting is on, reported by GET_REPORT. */
    uint32_t zpass_enable, zpass_count, reports;
    uint32_t blend_pairs[16];   /* sfactor<<16 | dfactor seen, for the report */
    int blend_npairs;
    Texture  texs[4];                   /* one per texture stage */
    /* Register combiners and the per-pixel state around them. rc_seen: the
     * title has programmed the combiners, so they decide every pixel's
     * colour; until then the old "stage 0 texel * diffuse" stands in. */
    Nv2aCombiner rc;
    int      rc_seen;
    uint32_t clip_plane_mode;           /* SET_SHADER_CLIP_PLANE_MODE */
    uint32_t z_clamp,control0;
    float clip_min,clip_max;
    uint32_t cull_enable, cull_face, front_face, polygon_offset_enable;
    float polygon_offset_scale, polygon_offset_bias;
    float bump_matrix[4][4], bump_scale[4], bump_offset[4], eye_vector[3];
    uint32_t alpha_test, alpha_func, alpha_ref;
    uint32_t fog_enable, fog_mode, fog_color;
    float    fog_param[2];
    /* Vertex programs (nv2a_vsh_interp.c) and the depth buffer, which
     * together are what a 3D scene needs and a 2D one never used. */
    uint32_t xf_mode;                   /* TRANSFORM_EXECUTION_MODE: 2 = program */
    uint32_t batches_program, verts_program, tris_behind;
    /* Where program-path triangles go, so "nothing drew" has a reason. */
    uint32_t xf_degenerate, xf_offscreen, xf_drawn;
    uint64_t xf_depth_fail, xf_pixels;
    float xf_min[3], xf_max[3];
    uint32_t xf_fmt[16];  /* attribute formats seen: type | size<<4 | slot<<8 */
    int xf_nfmt, xf_seeded;
    uint32_t depth_test, depth_func, depth_mask;
    uint32_t stencil_test, stencil_func, stencil_ref, stencil_read_mask, stencil_write_mask;
    uint32_t stencil_fail, stencil_zfail, stencil_zpass;
    uint32_t clear_x, clear_y, clear_w, clear_h, clear_rect_set;
    uint32_t zeta_offset, zstencil_clear, zeta_pitch;
} s_gpu;

#if defined(__APPLE__)
typedef struct RegisteredRamRange {
    uint8_t *base;
    uint64_t offset, bytes;
    unsigned domain;
} RegisteredRamRange;

static int registered_ram_range(uint32_t va, uint64_t bytes, RegisteredRamRange *range)
{
    if (!bytes || bytes > SIZE_MAX || bytes > UINT64_C(0x100000000) - va)
        return 0;
    uint8_t *base = xbox_TranslateMappedRam(va, (size_t)bytes);
    if (!base) return 0;
    uint64_t end = (uint64_t)va + bytes, offset;
    unsigned domain;
    if (va >= XBOX_CONTIG_BASE && end <= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE) {
        domain = 1; offset = va - XBOX_CONTIG_BASE;
    } else if (va >= XBOX_TILED_BASE && end <= (uint64_t)XBOX_TILED_BASE + XBOX_CONTIG_SIZE) {
        domain = 1; offset = va - XBOX_TILED_BASE;
    } else {
        size_t size = xbox_GetMappedSize();
        if (!size || end > (uint64_t)size * (XBOX_NUM_MIRRORS + 1u) ||
            va / size != (end - 1) / size)
            return 0;
        domain = 0; offset = va % size;
        if (g_xbox_image_hi && offset < g_xbox_image_hi &&
            offset + bytes > g_xbox_image_lo)
            return 0;
    }
    if (range) {
        range->base = base; range->offset = offset;
        range->bytes = bytes; range->domain = domain;
    }
    return 1;
}

static void registered_quantize_screen(float position[4])
{
    for (unsigned i = 0; i < 2; ++i)
        if (isfinite(position[i]))
            position[i] = (float)(trunc((double)position[i] * 16.0) / 16.0);
}

static void registered_prepare_vertex(Nv2aVshOutput *vertex)
{
    registered_quantize_screen(vertex->pos);
    for (unsigned i = 0; i < 4; ++i) {
        vertex->d0[i] = isnan(vertex->d0[i]) ? 1.0f : fminf(1.0f, fmaxf(0.0f, vertex->d0[i]));
        vertex->d1[i] = isnan(vertex->d1[i]) ? 1.0f : fminf(1.0f, fmaxf(0.0f, vertex->d1[i]));
    }
}

static int registered_culled(float area)
{
    if (!nv2a_dispatch_registered()) return 0;
    if (!isfinite(area)) return 1;
    if (!s_gpu.cull_enable) return 0;
    if ((s_gpu.front_face != 0x900 && s_gpu.front_face != 0x901) ||
        (s_gpu.cull_face != 0x404 && s_gpu.cull_face != 0x405 && s_gpu.cull_face != 0x408)) {
        nv2a_dispatch_report_failure("software cull state");
        return 1;
    }
    int front = s_gpu.front_face == 0x900 ? area > 0 : area < 0;
    return s_gpu.cull_face == 0x408 || (s_gpu.cull_face == 0x404 ? front : !front);
}
#endif

static int perf_draw_start(void)
{
    uint32_t primitive=s_gpu.prim;
    if(primitive<1 || primitive>4 || !nv2a_gpu_cpu_enabled())return 0;
    s_draw_perf.draws[primitive]++;
    s_draw_perf.vertices[primitive]+=s_gpu.indices.count;
    if(s_draw_perf.count>=PERF_DRAW_SAMPLES){s_draw_perf.overflow++;return 0;}
    return 1;
}

static void perf_draw_finish(uint64_t before)
{
    PerfDrawSignature *v=&s_draw_perf.samples[s_draw_perf.count++];
    memset(v,0,sizeof(*v));
    v->primitive=s_gpu.prim;v->count=s_gpu.indices.count;
    v->point_valid=s_draw_perf.point_valid;v->point_raw=s_draw_perf.point_raw;
    v->point_class=s_draw_perf.point_class;v->point_subch=s_draw_perf.point_subch;
    v->blend=s_gpu.blend_enable;v->src=s_gpu.blend_sfactor;v->dst=s_gpu.blend_dfactor;
    v->equation=s_gpu.blend_equation;v->depth=s_gpu.depth_test;v->depth_func=s_gpu.depth_func;
    v->depth_write=s_gpu.depth_mask;v->color_write=s_gpu.color_mask;
    v->alpha=s_gpu.alpha_test;v->alpha_func=s_gpu.alpha_func;v->alpha_ref=s_gpu.alpha_ref;
    memcpy(v->textures,s_gpu.texs,sizeof(v->textures));
    v->context=pc_input_context();v->device=pc_input_device();v->captured=pc_frontend_captured();
    v->frame=s_draw_perf.frames+1;v->draw_serial=nv2a_gpu_cpu_draw_serial();
    v->program_known=nv2a_gpu_cpu_program_since(before,&v->program_hash);
}

static void perf_append(char *line,size_t capacity,unsigned *used,const char *format,...)
{
    if(*used>=capacity-1)return;
    va_list args;va_start(args,format);
    int n=vsnprintf(line+*used,capacity-*used,format,args);
    va_end(args);
    if(n>0) {
        size_t room=capacity-1-*used;
        *used+=(unsigned)((size_t)n<room?(size_t)n:room);
    }
}


/* Arm from existing guest fire/boost input, sampled without additional polls.
 * This identifies input intervals, not a weapon/effect-object identity. */
#if defined(_WIN32)
extern unsigned long long xbox_InputTraceState(void);
#else
static unsigned long long xbox_InputTraceState(void){return 0;}
#endif
static NV_TLS struct { int enabled,initialized;unsigned last_body,epoch,count,per_topology[11][2];uint64_t until,keys[64]; } s_effect_trace;
static volatile long effect_trace_records;
static int effect_trace_start(unsigned long long *input)
{
    if(!s_effect_trace.initialized) {const char *v=getenv("RECOMP_EFFECT_TRACE");s_effect_trace.enabled=v&&!strcmp(v,"1");s_effect_trace.initialized=1;}
    if(!s_effect_trace.enabled || effect_trace_records>=64 || !nv2a_gpu_cpu_enabled())return 0;
    *input=xbox_InputTraceState();unsigned body=(unsigned)*input;
    unsigned actions=body&7u;
    if((body&0xB00u) || pc_input_context()!=PC_INPUT_GAMEPLAY || pc_frontend_captured()) {s_effect_trace.last_body=body;s_effect_trace.until=0;return 0;}
    if((actions&~s_effect_trace.last_body&7u)!=0 || (actions && !s_effect_trace.until)) {
        s_effect_trace.until=s_draw_perf.frames+32;s_effect_trace.count=0;s_effect_trace.epoch=(unsigned)(*input>>32);
        memset(s_effect_trace.per_topology,0,sizeof(s_effect_trace.per_topology));
    }
    s_effect_trace.last_body=body;
    return s_effect_trace.until && s_draw_perf.frames<s_effect_trace.until && s_gpu.prim>=1 && s_gpu.prim<=10;
}
static void effect_trace_finish(uint64_t before,unsigned long long input)
{
    unsigned group=s_gpu.blend_enable!=0;
    if(s_gpu.prim<1 || s_gpu.prim>10 || s_effect_trace.per_topology[s_gpu.prim][group]>=3)return;
    unsigned submitted=nv2a_gpu_cpu_draw_serial()>before;
    uint64_t program=0;int known=nv2a_gpu_cpu_program_since(before,&program);
    uint64_t key=program ^ ((uint64_t)s_gpu.prim<<60) ^ ((uint64_t)s_gpu.indices.count<<32) ^
        s_gpu.texs[0].offset ^ ((uint64_t)s_gpu.blend_sfactor<<16) ^ s_gpu.blend_dfactor ^ ((uint64_t)s_gpu.depth_mask<<48) ^ ((uint64_t)group<<52) ^ ((uint64_t)submitted<<56);
    for(unsigned i=0;i<s_effect_trace.count;i++)if(s_effect_trace.keys[i]==key)return;
    if(s_effect_trace.count>=64)return;
    s_effect_trace.keys[s_effect_trace.count++]=key;
    s_effect_trace.per_topology[s_gpu.prim][group]++;
#if defined(_WIN32)
    long record=InterlockedIncrement((volatile LONG *)&effect_trace_records);
#else
    long record=++effect_trace_records;
#endif
    if(record>64)return;
    char line[2048];unsigned used=0;line[0]=0;
    perf_append(line,sizeof(line),&used,"[PERF] effect-draw record=%ld interval_epoch=%u input_epoch=%u action=%X frame=%llu primitive=%u vertices=%u native_submitted=%u draw_serial=%llu program_known=%d program=%016llX point_observed=%u raw043C=%08X blend=%u/%X/%X/%X depth=%u/%X/%u alpha=%u/%X/%u color_write=%08X context=%u device=%u captured=%u",
        record,s_effect_trace.epoch,(unsigned)(input>>32),(unsigned)input&31u,(unsigned long long)s_draw_perf.frames,
        s_gpu.prim,s_gpu.indices.count,nv2a_gpu_cpu_draw_serial()>before,(unsigned long long)nv2a_gpu_cpu_draw_serial(),known,(unsigned long long)program,
        s_draw_perf.point_valid,s_draw_perf.point_raw,s_gpu.blend_enable,s_gpu.blend_sfactor,s_gpu.blend_dfactor,s_gpu.blend_equation,
        s_gpu.depth_test,s_gpu.depth_func,s_gpu.depth_mask,s_gpu.alpha_test,s_gpu.alpha_func,s_gpu.alpha_ref,s_gpu.color_mask,
        (unsigned)pc_input_context(),(unsigned)pc_input_device(),pc_frontend_captured()!=0);
    for(unsigned i=0;i<4;i++) {const Nv2aGpuTexture *t=&s_gpu.texs[i];perf_append(line,sizeof(line),&used," tex%u=%u,%08X,%u,%u,%u,%u,%u,%08X,%u/%u/%u",i,t->valid,t->offset,t->color,t->width,t->height,t->depth,t->levels,t->filter,t->addr_u,t->addr_v,t->addr_w);}
    perf_append(line,sizeof(line),&used," scope=automatic_input_interval_not_weapon_identity\n");fputs(line,stderr);
}

static void perf_report_cpu(void)
{
    static const char *const phase_names[NV2A_CPU_PHASE_COUNT]={
        "texture_prepare","texture_resource","texture_srv","pixel_miss","program_miss",
        "shader_resource","buffer_upload","fetch_resource","vertex_fallback",
        "pack_digest","pack_read","pack_resource","pack_srv","pack_select","present","pacer"};
    static const char *const event_names[NV2A_CPU_EVENT_COUNT]={
        "texture_evict","texture_update","pixel_evict","program_evict","pixel_pending",
        "program_pending","fetch_evict","fetch_update","pack_evict","upload_discard",
        "upload_wrap","present_ok"};
    Nv2aCpuSnapshot cpu;
    nv2a_gpu_cpu_take(&cpu);
    Nv2aTexturePackStats pack=nv2a_texture_pack_stats();
    /* Compose each record in bounded stack storage, then use one stdio write.
     * Shader/kernel worker stderr records cannot split a new diagnostic line
     * between its fields. No file lock or output work is added to draw calls. */
    char line[4096];unsigned used=0;line[0]=0;
    perf_append(line,sizeof(line),&used,"[PERF] CPU phases frame=%llu draw_serial=%llu present_serial=%llu tid=%u "
        "scope=reporting_thread cpu_wall_ms=1 nested=texture_pixel_program_upload_fetch_fallback_pack_load_in_draw,shader_resource_in_miss "
        "outside_draw=pack_select_present_pacer no_sum=1 draw_distance=%d texture_quality=%d "
        "pack_quality=%u pack_entries=%u pack_cache_bytes=%llu "
        "phase_fields=calls,total_ms,max_ms,bytes",
        (unsigned long long)s_draw_perf.frames,(unsigned long long)cpu.draw_serial,
        (unsigned long long)cpu.present_serial,cpu.owner_thread,
        pc_setting_get(PC_DRAW_DISTANCE,50),pc_setting_get(PC_TEXTURE_QUALITY,0),pack.quality,pack.entries,
        (unsigned long long)pack.cache_bytes);
    for(unsigned i=0;i<NV2A_CPU_PHASE_COUNT;i++) {
        const Nv2aCpuCounter *c=&cpu.phases[i];
        perf_append(line,sizeof(line),&used," %s=%llu,%.3f,%.3f,%llu",phase_names[i],
            (unsigned long long)c->calls,c->total_ms,c->max_ms,(unsigned long long)c->bytes);
    }
    for(unsigned i=0;i<NV2A_CPU_EVENT_COUNT;i++)
        perf_append(line,sizeof(line),&used," %s=%llu",event_names[i],(unsigned long long)cpu.events[i]);
    perf_append(line,sizeof(line),&used," point_writes=%llu points=%llu/%llu lines2=%llu/%llu lines3=%llu/%llu "
        "lines4=%llu/%llu sample=first8 samples=%u overflow=%llu\n",
        (unsigned long long)s_draw_perf.point_writes,
        (unsigned long long)s_draw_perf.draws[1],(unsigned long long)s_draw_perf.vertices[1],
        (unsigned long long)s_draw_perf.draws[2],(unsigned long long)s_draw_perf.vertices[2],
        (unsigned long long)s_draw_perf.draws[3],(unsigned long long)s_draw_perf.vertices[3],
        (unsigned long long)s_draw_perf.draws[4],(unsigned long long)s_draw_perf.vertices[4],
        s_draw_perf.count,(unsigned long long)s_draw_perf.overflow);
    fputs(line,stderr);
    for(unsigned i=0;i<s_draw_perf.count;i++) {
        const PerfDrawSignature *v=&s_draw_perf.samples[i];
        char program[32];
        if(v->program_known)snprintf(program,sizeof(program),"%016llX",(unsigned long long)v->program_hash);
        else strcpy(program,"unknown");
        used=0;line[0]=0;
        perf_append(line,sizeof(line),&used,"[PERF] draw-signature sample=%u frame=%llu draw_serial=%llu primitive=%u vertices=%u "
            "point_observed=%u raw043C=%08X class=%02X subch=%u program=%s "
            "blend=%u/%X/%X/%X depth=%u/%X/%u color_write=%08X alpha=%u/%X/%u context=%u device=%u captured=%u",
            i,(unsigned long long)v->frame,(unsigned long long)v->draw_serial,v->primitive,v->count,
            v->point_valid,v->point_raw,v->point_class,v->point_subch,program,
            v->blend,v->src,v->dst,v->equation,v->depth,v->depth_func,v->depth_write,v->color_write,
            v->alpha,v->alpha_func,v->alpha_ref,v->context,v->device,v->captured);
        for(unsigned t=0;t<4;t++) {
            const Nv2aGpuTexture *texture=&v->textures[t];
            perf_append(line,sizeof(line),&used," tex%u=%u/%08X/%02X/%ux%ux%u/%u",t,texture->valid,texture->offset,
                texture->color,texture->width,texture->height,texture->depth,texture->dimensionality);
        }
        perf_append(line,sizeof(line),&used,"\n");fputs(line,stderr);
    }
    memset(s_draw_perf.draws,0,sizeof(s_draw_perf.draws));
    memset(s_draw_perf.vertices,0,sizeof(s_draw_perf.vertices));
    s_draw_perf.count=0;s_draw_perf.overflow=0;s_draw_perf.point_writes=0;
}

/* Unhandled methods, ranked. The interesting output is not that something was
 * skipped but which things dominate, because that is the order to implement
 * them in. */
#define PB_EXEC_MAX_UNHANDLED 2048
typedef struct { uint32_t method, count, last_param; } PbUnhandled;
static PbUnhandled s_unhandled[PB_EXEC_MAX_UNHANDLED];
static int s_unhandled_count;

/* Every texture-stage register, as the title last set it.
 * Texturing is not implemented yet; knowing which formats and sizes a title
 * actually programs is what decides which ones are worth implementing. */
#define NV_TEX_FIRST 0x1B00
#define NV_TEX_LAST  0x1BFC
static uint32_t s_tex_reg[(NV_TEX_LAST - NV_TEX_FIRST) / 4 + 1];
static uint8_t  s_tex_set[(NV_TEX_LAST - NV_TEX_FIRST) / 4 + 1];

/* Formats whose dimensions come from the format word and whose coordinates
 * arrive normalised, rather than from a pitch and SET_TEXTURE_IMAGE_RECT with
 * coordinates in texels. Swizzled and block-compressed are both in this group,
 * and every place that used to test only for swizzled needs the pair. */
static int tex_size_from_format(uint32_t fmt)
{
    return d3d8_format_is_swizzled(fmt) || d3d8_format_dxt_block_bytes(fmt);
}

static void tex_update_valid(Texture *t)
{
    t->valid = (t->control0 & (1u<<30)) && t->offset && t->width && t->height
            && (tex_size_from_format(t->color) || t->pitch);
}

static void record_tex_reg(uint32_t method, uint32_t param)
{
    s_tex_reg[(method - NV_TEX_FIRST) / 4] = param;
    s_tex_set[(method - NV_TEX_FIRST) / 4] = 1;
    /* A pitch is a linear texture's property. A swizzled one has no rows and
     * so no pitch, and requiring one here refused every swizzled texture --
     * which is nearly all of them, since swizzled is the Xbox default. That
     * left the title's own textures unsampled and every textured quad drawn in
     * flat vertex colour. */
    tex_update_valid(&s_gpu.texs[0]);
}

/* Every distinct texture a batch was drawn with, and how many batches used it.
 *
 * The per-draw verbose print shows the first few draws of the first frame,
 * which is enough to see that texturing works at all and not enough to answer
 * "is a font page ever bound". This is the same shape as the unhandled-method
 * table below it: a small set, ranked, printed with the rest of the report. */
#define PB_EXEC_MAX_TEXTURES 64
typedef struct {
    uint32_t offset, color, width, height, batches;
} PbTexUse;
static PbTexUse s_tex_use[PB_EXEC_MAX_TEXTURES];
static int s_tex_use_count;

/* Defined below, next to the sampler it goes through. */
static void dump_texture_bmp(uint32_t seq);

/* Repeat dumps are numbered from well past the first-use sequence, so a
 * listing sorts them after the textures they came from and no first-use file
 * is ever overwritten by one. */
#define TEX_DUMP_SEQ_BASE 1000u
#define TEX_DUMP_SEQ_MAX  40u

static void note_texture_use(void)
{
    int i;

    if (!s_gpu.texs[0].valid)
        return;
    for (i = 0; i < s_tex_use_count; i++) {
        if (s_tex_use[i].offset == s_gpu.texs[0].offset
         && s_tex_use[i].color  == s_gpu.texs[0].color) {
            s_tex_use[i].batches++;
            /* Dump a surface that is redrawn, every Nth time it is bound.
             *
             * First use alone cannot tell a decode error that is wrong in
             * every frame from one that accumulates across them. A block
             * transform that is wrong is wrong on its own, in the keyframe
             * as much as anywhere; motion compensation that is wrong starts
             * from a clean keyframe and smears further with each predicted
             * frame after it. In a single frame the two look identical, and
             * in a sequence they look nothing alike -- so the sequence is
             * what has to be captured.
             *
             * It belongs on this side of the return: a video surface keeps
             * one address for the whole film, so after the first frame it is
             * only ever found here, and the first-use dump below never fires
             * for it again. RECOMP_TEX_DUMP_EVERY=<n> sets the interval, and
             * RECOMP_TEX_DUMP still names the files. */
            {
                static int every = -1;
                static unsigned binds, seq;
                if (every < 0) {
                    const char *e = getenv("RECOMP_TEX_DUMP_EVERY");
                    every = e ? atoi(e) : 0;
                }
                if (every > 0 && ++binds % (unsigned)every == 0
                    && seq < TEX_DUMP_SEQ_MAX)
                    dump_texture_bmp(TEX_DUMP_SEQ_BASE + seq++);
            }
            return;
        }
    }
    if (s_tex_use_count < PB_EXEC_MAX_TEXTURES) {
        s_tex_use[s_tex_use_count].offset  = s_gpu.texs[0].offset;
        s_tex_use[s_tex_use_count].color   = s_gpu.texs[0].color;
        s_tex_use[s_tex_use_count].width   = s_gpu.texs[0].width;
        s_tex_use[s_tex_use_count].height  = s_gpu.texs[0].height;
        s_tex_use[s_tex_use_count].batches = 1;
        s_tex_use_count++;
        dump_texture_bmp((uint32_t)s_tex_use_count - 1);
    }
}

static void note_unhandled(uint32_t method, uint32_t param)
{
    int i;

    s_gpu.unhandled_total++;
    for (i = 0; i < s_unhandled_count; i++) {
        if (s_unhandled[i].method == method) {
            s_unhandled[i].count++;
            s_unhandled[i].last_param = param;
            return;
        }
    }
    if (s_unhandled_count < PB_EXEC_MAX_UNHANDLED) {
        s_unhandled[s_unhandled_count].method = method;
        s_unhandled[s_unhandled_count].count = 1;
        s_unhandled[s_unhandled_count].last_param = param;
        s_unhandled_count++;
    }
}

/* Read attribute `a` of vertex `index` as floats. Only the float and the
 * normalised-byte types appear in practice; anything else returns 0 so a
 * caller sees a degenerate vertex rather than reading past the array. */
static int fetch_attr(const VertexAttr *a, uint32_t index, float out[4])
{
    const uint8_t *mem;
    const uint8_t *p;
    uint32_t i;

    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (!a->size) {
        /* Disabled arrays retain the current immediate value. In particular,
         * skinning programs use constant blend weights set by SET_VERTEX_DATA. */
        ptrdiff_t attr = a - s_gpu.attr;
        if (attr >= 0 && attr < NV_VERTEX_ATTRS) memcpy(out,s_gpu.imm_attr[attr],16);
        return 1;
    }
    if (s_gpu.inline_active) {
        /* The batch arrived as INLINE_ARRAY, so `offset` is a byte offset into
         * the buffered payload rather than a guest address -- and 0 is a legal
         * one there, which is why the offset test is on the other side of this
         * branch. */
        size_t at = (size_t)a->offset + (size_t)index * a->stride;
        if (at + nv2a_vertex_value_bytes(a->type,a->size) > (size_t)s_gpu.inline_count * 4)
            return 0;
        p = (const uint8_t *)s_gpu.inline_buf + at;
    } else {
        if (!a->offset)
            return 0;
        mem=(const uint8_t *)xbox_GetMemoryOffset();
        p = mem + a->offset + (size_t)index * a->stride;
    }

    switch (a->type) {
    case 0:                                  /* D3DCOLOR */
        /* A DWORD 0xAARRGGBB, so little-endian bytes are B,G,R,A -- not the
         * component order of every other format here. Returned as R,G,B,A so
         * callers need not know which format the title chose. */
        out[0] = (float)p[2] / 255.0f;
        out[1] = (float)p[1] / 255.0f;
        out[2] = (float)p[0] / 255.0f;
        out[3] = (float)p[3] / 255.0f;
        return 1;
    case 2:                                  /* float */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = ((const float *)p)[i];
        return 1;
    case 4:                                  /* unsigned byte, normalised */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)p[i] / 255.0f;
        return 1;
    /* The formats 3D geometry uses and screen-space quads never did. */
    case 1:                                  /* signed short, normalised */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)((const int16_t *)p)[i] / 32767.0f;
        return 1;
    case 5:                                  /* signed short, as is */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)((const int16_t *)p)[i];
        return 1;
    case 6: {                                /* packed 11:11:10 normal */
        uint32_t v = *(const uint32_t *)p;
        out[0] = (float)((int32_t)(v << 21) >> 21) / 1023.0f;
        out[1] = (float)((int32_t)(v << 10) >> 21) / 1023.0f;
        out[2] = (float)((int32_t)v >> 22) / 511.0f;
        return 1;
    }
    default:
        return 0;
    }
}

static uint32_t surface_bpp(void);
static float zclear_value(void);

#if defined(__APPLE__)
static int registered_color_range(RegisteredRamRange *range)
{
    if (range) memset(range, 0, sizeof(*range));
    if (!s_gpu.clip_w || !s_gpu.clip_h) return 1;
    uint32_t bpp = surface_bpp();
    uint32_t type = (s_gpu.format >> 8) & 15u;
    uint64_t right = (uint64_t)s_gpu.clip_x + s_gpu.clip_w;
    uint64_t bottom = (uint64_t)s_gpu.clip_y + s_gpu.clip_h;
    if (bpp != 2 && bpp != 4)
        return 0;
    if (type == 2) {
        uint32_t wx = (s_gpu.format >> 16) & 15u;
        uint32_t hy = (s_gpu.format >> 24) & 15u;
        if (wx > 12 || hy > 12 || right > (UINT64_C(1) << wx) ||
            bottom > (UINT64_C(1) << hy))
            return 0;
        uint64_t bytes = (UINT64_C(1) << (wx + hy)) * bpp;
        return registered_ram_range(dma_resolve(s_gpu.color_offset), bytes, range);
    }
    if (type > 1 || !s_gpu.pitch || right > s_gpu.pitch / bpp ||
        bottom - 1 > UINT32_MAX / s_gpu.pitch)
        return 0;
    uint64_t bytes = (bottom - 1) * s_gpu.pitch + right * bpp;
    return registered_ram_range(dma_resolve(s_gpu.color_offset), bytes, range);
}

typedef struct RegisteredDepthUse {
    uint64_t start, end;
    unsigned domain, nonlinear;
} RegisteredDepthUse;

static int registered_depth_mode_guard(int clear_rect)
{
    static RegisteredDepthUse uses[128];
    static unsigned count;
    static uint64_t epoch;
    if (!nv2a_dispatch_registered() || !s_gpu.zeta_offset) return 1;
    uint64_t current_epoch = nv2a_dispatch_registration_epoch();
    if (epoch != current_epoch) {
        epoch = current_epoch;
        count = 0;
    }
    uint64_t left = s_gpu.clip_x, top = s_gpu.clip_y;
    uint64_t right = left + s_gpu.clip_w, bottom = top + s_gpu.clip_h;
    if (clear_rect && s_gpu.clear_rect_set == 3) {
        uint64_t end_x = (uint64_t)s_gpu.clear_x + s_gpu.clear_w;
        uint64_t end_y = (uint64_t)s_gpu.clear_y + s_gpu.clear_h;
        if (left < s_gpu.clear_x) left = s_gpu.clear_x;
        if (top < s_gpu.clear_y) top = s_gpu.clear_y;
        if (right > end_x) right = end_x;
        if (bottom > end_y) bottom = end_y;
    }
    if (right <= left || bottom <= top) return 1;
    uint32_t zformat = (s_gpu.format >> 4) & 15u;
    uint32_t type = (s_gpu.format >> 8) & 15u;
    uint32_t aa = (s_gpu.format >> 12) & 15u;
    if ((zformat != 1 && zformat != 2) || type > 2 || aa > 2) {
        nv2a_dispatch_report_failure("unknown software depth layout");
        return 0;
    }
    uint32_t bpp = zformat == 1 ? 2 : 4;
    uint64_t bytes;
    if (type == 2) {
        uint32_t wx = (s_gpu.format >> 16) & 255u, hy = s_gpu.format >> 24;
        if (wx > 12 || hy > 12 || (UINT64_C(1) << wx) < right ||
            (UINT64_C(1) << hy) < bottom) {
            nv2a_dispatch_report_failure("unknown software depth extent");
            return 0;
        }
        bytes = (UINT64_C(1) << (wx + hy)) * bpp;
    } else {
        uint32_t color_bpp = surface_bpp();
        uint64_t pitch = s_gpu.zeta_pitch;
        if (!pitch && color_bpp && s_gpu.pitch % color_bpp == 0)
            pitch = (uint64_t)(s_gpu.pitch / color_bpp) * bpp;
        if (!pitch || pitch > UINT32_MAX || pitch % bpp ||
            right * bpp > pitch || bottom - 1 > UINT32_MAX / pitch) {
            nv2a_dispatch_report_failure("software depth extent");
            return 0;
        }
        bytes = (bottom - 1) * pitch + right * bpp;
    }
    if (aa) bytes *= aa == 1 ? 2u : 4u;
    RegisteredRamRange range;
    if (!registered_ram_range(dma_resolve(s_gpu.zeta_offset), bytes, &range)) {
        nv2a_dispatch_report_failure("software depth mapping");
        return 0;
    }
    unsigned nonlinear = type == 2 || aa != 0;
    uint64_t end = range.offset + range.bytes;
    for (unsigned i = 0; i < count; ++i) {
        RegisteredDepthUse *use = &uses[i];
        if (use->domain != range.domain || range.offset >= use->end || use->start >= end)
            continue;
        if (use->nonlinear != nonlinear) {
            nv2a_dispatch_report_failure("mixed linear and non-linear depth");
            return 0;
        }
    }
    for (unsigned i = 0; i < count; ++i) {
        RegisteredDepthUse *use = &uses[i];
        if (use->domain == range.domain && use->nonlinear == nonlinear &&
            range.offset < use->end && use->start < end) {
            if (range.offset < use->start) use->start = range.offset;
            if (end > use->end) use->end = end;
            return 1;
        }
    }
    if (count == sizeof(uses) / sizeof(uses[0])) {
        nv2a_dispatch_report_failure("software depth tracking capacity");
        return 0;
    }
    uses[count++] = (RegisteredDepthUse){range.offset, end, range.domain, nonlinear};
    return 1;
}
#endif

static void hardware_state(Nv2aGpuState *g)
{
    memset(g, 0, sizeof(*g));
    g->color_va = dma_resolve(s_gpu.color_offset);
    g->pitch = s_gpu.pitch; g->bpp = surface_bpp(); g->format = s_gpu.format;
    g->depth_va = dma_resolve(s_gpu.zeta_offset);
#if defined(__APPLE__)
    if (nv2a_dispatch_registered() && !s_gpu.zeta_offset) g->depth_va = 0;
#endif
    g->depth_pitch = s_gpu.zeta_pitch;
    g->zpass_enable = s_gpu.zpass_enable != 0;
    g->fog_enable = s_gpu.fog_enable; g->fog_mode = s_gpu.fog_mode;
    g->fog_params[0] = s_gpu.fog_param[0]; g->fog_params[1] = s_gpu.fog_param[1];
    g->clip_x = s_gpu.clip_x; g->clip_y = s_gpu.clip_y;
    g->clip_w = s_gpu.clip_w; g->clip_h = s_gpu.clip_h;
    g->depth_test = s_gpu.depth_test; g->depth_func = s_gpu.depth_func; g->depth_mask = s_gpu.depth_mask;
    g->cull_enable=s_gpu.cull_enable;g->cull_face=s_gpu.cull_face;g->front_face=s_gpu.front_face;
    g->polygon_offset_enable=s_gpu.polygon_offset_enable;
    g->polygon_offset_scale=s_gpu.polygon_offset_scale;g->polygon_offset_bias=s_gpu.polygon_offset_bias;
    g->stencil_test=s_gpu.stencil_test;g->stencil_func=s_gpu.stencil_func;g->stencil_ref=s_gpu.stencil_ref;
    g->stencil_read_mask=s_gpu.stencil_read_mask;g->stencil_write_mask=s_gpu.stencil_write_mask;
    g->stencil_fail=s_gpu.stencil_fail;g->stencil_zfail=s_gpu.stencil_zfail;g->stencil_zpass=s_gpu.stencil_zpass;
    g->stencil_clear=s_gpu.zstencil_clear&255;
    g->blend_enable = s_gpu.blend_enable; g->blend_src = s_gpu.blend_sfactor;
    g->blend_dst = s_gpu.blend_dfactor; g->blend_equation = s_gpu.blend_equation;
    g->blend_color = s_gpu.blend_color; g->color_mask = s_gpu.color_mask;
    g->alpha_test = s_gpu.alpha_test; g->alpha_func = s_gpu.alpha_func; g->alpha_ref = s_gpu.alpha_ref;
    g->clip_plane_mode = s_gpu.clip_plane_mode; g->fog_color = s_gpu.fog_color;
    g->z_clamp=s_gpu.z_clamp;g->clip_min=s_gpu.clip_min;g->clip_max=s_gpu.clip_max;
    memcpy(g->bump_matrix,s_gpu.bump_matrix,sizeof(g->bump_matrix));
    memcpy(g->bump_scale,s_gpu.bump_scale,sizeof(g->bump_scale));
    memcpy(g->bump_offset,s_gpu.bump_offset,sizeof(g->bump_offset));
    memcpy(g->eye_vector,s_gpu.eye_vector,sizeof(g->eye_vector));
    g->use_rc = s_gpu.rc_seen; g->textured = s_gpu.texs[0].valid; g->rc = s_gpu.rc;
    memcpy(g->textures, s_gpu.texs, sizeof(g->textures));
}

static int hardware_surface_ok(const Nv2aGpuState *g)
{
    return g->color_va && g->pitch && g->clip_h && (g->bpp == 4 || g->bpp == 2) &&
        !surface_write_refused(g->color_va, (g->clip_y + g->clip_h) * g->pitch, "render");
}

/* Laptop diagnostics (always on, rate-limited): every way a frame leaves the GPU path. The first 8 of each
 * site, then every 512th, with the reason; a tinted, slow picture on an iGPU (2026-10-31) was never logged. */
static void gpu_fallback_note(int site, uint32_t va, int hw_ok)
{
    static const char *const names[4] = { "clear", "program draw", "draw", "present" };
    static volatile long count[4];
    extern int nv2a_gpu_present_reason(void);
    long n = _InterlockedIncrement(&count[site & 3]);
    if (n <= 8 || (n & 511) == 0)
        fprintf(stderr, "[GPU-FALLBACK] %s on the CPU (%ld so far) va=%08X surface_ok=%d present_reason=%d draws=%u\n",
                names[site & 3], n, va, hw_ok, nv2a_gpu_present_reason(), s_gpu.draws);
}

static void note_drawn(void)
{
    if (!s_gpu.drawn_stale && s_gpu.drawn_offset
        && s_gpu.clip_w * s_gpu.clip_h < s_gpu.drawn_w * s_gpu.drawn_h)
        return;
    s_gpu.drawn_stale = 0;
    s_gpu.drawn_offset = s_gpu.color_offset;
    s_gpu.drawn_pitch = s_gpu.pitch;
    s_gpu.drawn_bpp = surface_bpp();
    s_gpu.drawn_x = s_gpu.clip_x; s_gpu.drawn_y = s_gpu.clip_y;
    s_gpu.drawn_w = s_gpu.clip_w; s_gpu.drawn_h = s_gpu.clip_h;
}

static uint32_t surface_bpp(void)
{
    /* The surface format says it outright (NV097_SET_SURFACE_FORMAT_COLOR).
     * Deriving it from pitch / clip width -- the only way before -- is right
     * only while the clip spans the whole surface: Burnout 3 narrows the clip
     * to a 250-pixel window to draw its option values, 2560 / 250 came out as
     * 10 bytes a pixel, and every such text quad was refused. */
    switch (s_gpu.format & 0xF) {
    case 0x1: case 0x2: case 0x3: case 0xA:
        return 2;
    case 0x4: case 0x5: case 0x6: case 0x7: case 0x8:
        return 4;
    case 0x9:
        return 1;
    default:
        break;
    }
    /* The pitch and the clip width together give the pixel size, which is more
     * reliable than decoding the format field: the format's colour code is
     * only meaningful alongside a type the title also sets, while the pitch is
     * always exactly how many bytes a row occupies. */
    if (!s_gpu.clip_w)
        return 0;
    return s_gpu.pitch / s_gpu.clip_w;
}

/* 16-bit colour surfaces. G8B8 (format 0xA) keeps G in the high byte and B
 * in the low byte; red and alpha are not stored. Every other 16-bit format
 * is treated as R5G6B5. Matches the D3D11 and Metal backends. */
static int surface_g8b8(void)
{
    return (s_gpu.format & 0xF) == 0xA;
}

static uint16_t pack_surface16(uint32_t argb)
{
    if (surface_g8b8())
        return (uint16_t)argb;
    return (uint16_t)(((argb >> 8) & 0xF800) | ((argb >> 5) & 0x07E0)
                    | ((argb >> 3) & 0x001F));
}

static uint32_t unpack_surface16(uint32_t pixel)
{
    if (surface_g8b8())
        return 0xFF000000u | pixel;
    return 0xFF000000u | ((pixel & 0xF800u) << 8) | ((pixel & 0x07E0u) << 5)
         | ((pixel & 0x001Fu) << 3);
}


/* Write the current surface out as a 24-bit BMP.
 *
 * A framebuffer window needs someone watching it. A file does not, which makes
 * this the only way to check what a title actually rendered on a machine you
 * are not sitting at -- and the only way to put a picture in a bug report.
 *
 * ponytail: bottom-up 24bpp BMP, no palette, no compression. That is the one
 * format every viewer reads and it is 30 lines; PNG would need a dependency.
 */
/* Set while RECOMP_FB_DUMP_FLIPS is dumping every flip: the report and
 * after-draw dumps stand down then, or they would put duplicate frames into
 * the sequence. */
static int s_flip_dumping;

static void dump_surface_bmp(void)
{
    const char *prefix = getenv("RECOMP_FB_DUMP");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    int drawn = s_gpu.drawn_offset != 0;
    uint32_t offset = drawn ? s_gpu.drawn_offset : s_gpu.color_offset;
    uint32_t pitch = drawn ? s_gpu.drawn_pitch : s_gpu.pitch;
    uint32_t cx = drawn ? s_gpu.drawn_x : s_gpu.clip_x;
    uint32_t cy = drawn ? s_gpu.drawn_y : s_gpu.clip_y;
    static int seq;
    char path[512];
    uint32_t w = drawn ? s_gpu.drawn_w : s_gpu.clip_w;
    uint32_t h = drawn ? s_gpu.drawn_h : s_gpu.clip_h, y, x;
    uint32_t bpp = drawn ? s_gpu.drawn_bpp : surface_bpp();
    uint32_t row_bytes, pad, filesz;
    uint8_t hdr[54];
    FILE *f;

    if (!prefix || !w || !h || (bpp != 2 && bpp != 4) || !offset)
        return;

    nv2a_gpu_readback(dma_resolve(offset));
    if (nv2a_dispatch_failed())
        return;

    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;

    snprintf(path, sizeof path, "%s%05d.bmp", prefix, seq++);
    f = fopen(path, "wb");
    if (!f)
        return;

    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);

    /* BMP rows run bottom-up. */
    for (y = h; y-- > 0; ) {
        const uint8_t *row = mem + dma_resolve(offset)
                           + (size_t)(cy + y) * pitch;
        for (x = 0; x < w; x++) {
            uint8_t bgr[3];
            if (bpp == 4) {
                uint32_t v = ((const uint32_t *)row)[cx + x];
                bgr[0] = (uint8_t)(v);
                bgr[1] = (uint8_t)(v >> 8);
                bgr[2] = (uint8_t)(v >> 16);
            } else {
                uint16_t v = ((const uint16_t *)row)[cx + x];
                bgr[0] = (uint8_t)(( v        & 0x1F) << 3);
                bgr[1] = (uint8_t)(((v >>  5) & 0x3F) << 2);
                bgr[2] = (uint8_t)(((v >> 11) & 0x1F) << 3);
            }
            fwrite(bgr, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = {0, 0, 0};
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
    if (seq == 1)
        fprintf(stderr, "  [GPU] framebuffer dump: %s (%ux%u from 0x%08X %ubpp)\n",
                path, w, h, s_gpu.color_offset, bpp);
}

/* Defined below, next to the rest of the rasteriser; the clear path uses it
 * for RECOMP_RASTER_TEST. */
static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb,
                            const float uv[3][2]);

/* Type-2 colour surfaces are render-to-texture targets stored in Morton
 * order. Writing them row-major makes the later texture sample appear as
 * scattered blocks across the frame. */
static int s_swz_on;
static uint32_t s_swz_mx, s_swz_my;

static void surface_swizzle_update(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("RECOMP_SWIZZLED_RT");
        enabled = !(value && value[0] == '0');
    }
    s_swz_on = enabled && ((s_gpu.format >> 8) & 15u) == 2u;
    if (s_swz_on)
        xbox_swizzle_masks(1u << ((s_gpu.format >> 16) & 15u),
                           1u << ((s_gpu.format >> 24) & 15u),
                           &s_swz_mx, &s_swz_my);
}

#if defined(__APPLE__)
static int registered_color_depth_safe(int clear_rect, int shadow_bounds);
#endif

static int clear_surface(uint32_t param)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    uint32_t y, x;

    int hardware=nv2a_gpu_enabled();
    if(nv2a_gpu_discard_frame()) {
        /* The window and FIFO keep progressing while this frame is lost. */
        xbox_FramebufferWindowSet(dma_resolve(s_gpu.color_offset),s_gpu.pitch);
        xbox_FramebufferWindowStart();
        return 1;
    }
#if defined(__APPLE__)
    if (((param & 1u) || ((param & 2u) && ((s_gpu.format >> 4) & 15u) != 1u)) &&
        !registered_depth_mode_guard(1)) return 1;
#endif
    if (hardware) {
        Nv2aGpuState g;
        hardware_state(&g);
        if(s_gpu.clear_rect_set==3) {
            uint32_t left=g.clip_x>s_gpu.clear_x?g.clip_x:s_gpu.clear_x;
            uint32_t top=g.clip_y>s_gpu.clear_y?g.clip_y:s_gpu.clear_y;
            uint32_t right=g.clip_x+g.clip_w, bottom=g.clip_y+g.clip_h;
            if(right>s_gpu.clear_x+s_gpu.clear_w)right=s_gpu.clear_x+s_gpu.clear_w;
            if(bottom>s_gpu.clear_y+s_gpu.clear_h)bottom=s_gpu.clear_y+s_gpu.clear_h;
            if(right<=left || bottom<=top)return 1;
            g.clip_x=left;g.clip_y=top;g.clip_w=right-left;g.clip_h=bottom-top;
        }
        if (hardware_surface_ok(&g) && nv2a_gpu_clear(&g, param, s_gpu.clear_color, zclear_value())) {
            s_gpu.color_base = g.color_va;
            s_gpu.clears++;
            xbox_FramebufferWindowSet(g.color_va, g.pitch);
            xbox_FramebufferWindowStart();
            return 1;
        }
        if(nv2a_gpu_discard_frame())return 1;
        gpu_fallback_note(0, g.color_va, hardware_surface_ok(&g));
        nv2a_gpu_sync_all();
        nv2a_gpu_invalidate(g.color_va);
    }
    if(nv2a_gpu_discard_frame())return 1;

#if defined(__APPLE__)
    if (nv2a_dispatch_registered() && (param & NV097_CLEAR_COLOR_MASK) && (param & 3u)) {
        int safe = registered_color_depth_safe(1, 0);
        if (safe <= 0) {
            nv2a_dispatch_report_failure(safe ? "software surface mapping" : "software surface overlap");
            return 1;
        }
    }
#endif
    if (!(param & NV097_CLEAR_COLOR_MASK))
        return 0;                          /* depth/stencil only */
    if (!s_gpu.color_offset || !s_gpu.pitch || !s_gpu.clip_h || bpp == 0)
        return 0;
#if defined(__APPLE__)
    if (nv2a_dispatch_registered() && !registered_color_range(NULL)) {
        nv2a_dispatch_report_failure("software color mapping");
        return 1;
    }
#endif
    surface_swizzle_update();
    {
        uint32_t base = dma_resolve(s_gpu.color_offset);
        uint64_t bytes = s_swz_on
            ? (UINT64_C(1) << (((s_gpu.format >> 16) & 15u)
                             + ((s_gpu.format >> 24) & 15u))) * bpp
            : (uint64_t)(s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch;
        if (bytes > UINT32_MAX ||
            surface_write_refused(base, (uint32_t)bytes, "clear"))
            return 0;
        s_gpu.color_base = base;
    }

    if (s_swz_on) {
        uint8_t *base = mem + s_gpu.color_base;
        uint32_t sw = 1u << ((s_gpu.format >> 16) & 15u);
        uint32_t sh = 1u << ((s_gpu.format >> 24) & 15u);
        uint32_t x0 = s_gpu.clip_x, x1 = s_gpu.clip_x + s_gpu.clip_w;
        uint32_t y0 = s_gpu.clip_y, y1 = s_gpu.clip_y + s_gpu.clip_h;
        uint16_t v16 = pack_surface16(s_gpu.clear_color);
        if (s_gpu.clear_rect_set == 3) {
            x0 = s_gpu.clear_x;
            x1 = s_gpu.clear_x + s_gpu.clear_w;
            y0 = s_gpu.clear_y;
            y1 = s_gpu.clear_y + s_gpu.clear_h;
        }
        if (x1 > sw) x1 = sw;
        if (y1 > sh) y1 = sh;
        for (y = y0; y < y1; ++y)
            for (x = x0; x < x1; ++x) {
                size_t index = swizzle_deposit(x, s_swz_mx)
                             | swizzle_deposit(y, s_swz_my);
                if (bpp == 4)
                    ((uint32_t *)base)[index] = s_gpu.clear_color;
                else
                    ((uint16_t *)base)[index] = v16;
            }
    } else for (y = 0; y < s_gpu.clip_h; y++) {
        uint8_t *row = mem + s_gpu.color_base
                     + (size_t)(s_gpu.clip_y + y) * s_gpu.pitch;
        if (bpp == 4) {
            uint32_t *p = (uint32_t *)row + s_gpu.clip_x;
            for (x = 0; x < s_gpu.clip_w; x++)
                p[x] = s_gpu.clear_color;
        } else if (bpp == 2) {
            /* The clear value is always given as A8R8G8B8; a 16-bit surface
             * takes the same colour reduced to 5:6:5, or its G:B for G8B8. */
            uint16_t v = pack_surface16(s_gpu.clear_color);
            uint16_t *p = (uint16_t *)row + s_gpu.clip_x;
            for (x = 0; x < s_gpu.clip_w; x++)
                p[x] = v;
        }
    }
    s_gpu.clears++;
    /* Progress markers, interleaved with everything else in the log. The
     * summary says drawing stopped; only a marker next to the surrounding
     * activity says what the title was doing when it stopped. */
    if ((s_gpu.clears % 100) == 0)
        fprintf(stderr, "  [GPU] clear #%u\n", s_gpu.clears);
    /* Distinct clear colours actually used. "Cleared to black" and "the clear
     * never ran" look identical in the framebuffer, and only one of them is a
     * bug -- so record what was asked for, not just how often. */
    {
        static uint32_t seen[8];
        static int n;
        int i;
        for (i = 0; i < n; i++)
            if (seen[i] == s_gpu.clear_color) break;
        if (i == n && n < 8) {
            seen[n++] = s_gpu.clear_color;
            fprintf(stderr, "  [GPU] clear colour 0x%08X -> surface 0x%08X"
                            " (%ubpp)\n",
                    s_gpu.clear_color, s_gpu.color_offset, surface_bpp());
        }
    }

    /* Prove the pixel path end to end, independent of whether the title has
     * given us any geometry yet.
     *
     * "Nothing on screen" has three very different causes -- the surface
     * address or pitch is wrong, the rasteriser is broken, or the title's
     * vertex buffers are empty -- and they are indistinguishable from a black
     * window. RECOMP_RASTER_TEST draws one known triangle into the surface
     * just cleared, so a visible triangle rules out the first two and leaves
     * only the third. On Wreckless it is the third: attribute 0 decodes
     * correctly (float, size 2, stride 16) and the buffer it points at stays
     * zero.
     *
     * ponytail: bring-up aid, not a feature. It costs one branch per clear. */
    static int raster_test = -1;
    if (raster_test < 0)
        raster_test = getenv("RECOMP_RASTER_TEST") != NULL;
    if (raster_test) {
        static int announced;
        /* Every clear, not once: the title clears each frame and double-buffers,
         * so a triangle drawn a single time is erased before anyone sees it. */
        if (s_gpu.clip_w && s_gpu.clip_h) {
            float a[2], b[2], c[2];
            a[0] = s_gpu.clip_w * 0.5f; a[1] = s_gpu.clip_h * 0.15f;
            b[0] = s_gpu.clip_w * 0.85f; b[1] = s_gpu.clip_h * 0.85f;
            c[0] = s_gpu.clip_w * 0.15f; c[1] = s_gpu.clip_h * 0.85f;
            raster_triangle(a, b, c, 0xFFFF00FFu, NULL);  /* magenta: never a clear colour */
            if (announced++ == 0)
            fprintf(stderr, "  [GPU] raster self-test: triangle (%.0f,%.0f)"
                            " (%.0f,%.0f) (%.0f,%.0f) into 0x%08X %ubpp\n",
                    a[0], a[1], b[0], b[1], c[0], c[1],
                    s_gpu.color_offset, surface_bpp());
        }
    }

    /* Show the surface actually being drawn into. A title that double-buffers
     * renders into the back buffer, so following AvSetDisplayMode's address
     * would show the one nothing is writing. */
    /* The window has to read where the pixels actually are, which is the
     * resolved address rather than the DMA-object offset. */
    /* Only until the title flips. Following the draw surface on every scan
     * shows the buffer being written right now, half a frame at a time; past
     * the first flip the window is repointed at the finished one instead. */
    if (s_gpu.flips == 0)
        xbox_FramebufferWindowSet(dma_resolve(s_gpu.color_offset), s_gpu.pitch);

    /* And open the window, rather than waiting for AvSetDisplayMode to do it.
     *
     * That was the only caller, so a title which draws before setting a display
     * mode -- or never sets one at all -- got no window however much it
     * rendered. The Xbox Dashboard clears a 1280x960 surface at 0x00088000 on
     * its first frame and had not called AvSetDisplayMode by then, so
     * RECOMP_FB_WINDOW=1 was set, the executor knew the address and the pitch,
     * and nothing appeared.
     *
     * Here is the better trigger anyway: this runs when a surface address is
     * known to be real, because a clear just used it. Idempotent and gated on
     * RECOMP_FB_WINDOW, so the cost is one interlocked compare per clear. */
    xbox_FramebufferWindowStart();
    return 0;
}


/* ── Rasteriser ──────────────────────────────────────────────────────────
 *
 * Fills triangles straight into the guest framebuffer, the same memory
 * clear_surface() writes and the framebuffer window already shows. That is the
 * whole reason it is done on the CPU rather than through D3D: nothing new has
 * to be plumbed for the result to be visible.
 *
 * ponytail: flat-shaded, no depth buffer, no texturing, no perspective
 * correction, and only batches whose attribute 0 is already in screen space.
 * A title running a vertex program hands over object-space positions that mean
 * nothing without executing the program, so those batches are counted and
 * skipped rather than drawn somewhere wrong. Upgrade path is the D3D11
 * translator in src/nv2a/nv2a_pgraph_d3d11.c once vertex programs are
 * translated; this exists to get the first geometry on screen for every title,
 * which in practice is UI, HUD and 2D overlays -- all pre-transformed.
 */

/* One texel, in the title's own format.
 *
 * The codes are the NV097 colour field, which is the Xbox D3DFMT_ enum --
 * src/d3d/d3d8_xbox.h is the table, and it is the table to check against
 * rather than recollection: 0x1E is LIN_X8R8G8B8 and not, as this first read
 * it, a byte-reversed BGRA. Getting that one wrong turned an opaque black
 * render target into a screen of pure blue, which is the kind of wrong that
 * looks like content.
 *
 * Only the linear (LIN_) formats are read. A swizzled texture stores its
 * texels in Morton order rather than in rows, so reading one as if it had a
 * pitch does not give a slightly wrong colour, it gives a different image --
 * and inventing that image is exactly what this is not for. An unsupported
 * format samples nothing and the caller keeps the vertex colour, which is
 * visibly wrong rather than quietly wrong.
 *
 * ponytail: nearest texel, no filtering, whatever SET_TEXTURE_FILTER asked
 * for. Bilinear when a title's output actually depends on it.
 */
/* Off the edge of the texture, the way the title asked for.
 *
 * Refusing to sample instead is not neutral: it hands the caller back the
 * vertex colour, so a pass whose coordinates reach the last texel by half a
 * texel gets a bright line down the edge of the screen. The dashboard's
 * resolve does exactly that -- its last column and last row, 1119 pixels of
 * white on a black frame, from a rounding step at the boundary.
 */
static uint32_t wrap_coord(uint32_t c, uint32_t size, uint32_t mode)
{
    /* Coordinates are signed: a bilinear tap one texel left of or above
     * texel 0 is -1, which must wrap to the far edge or clamp to 0 -- as an
     * unsigned value it clamped to the far edge instead. */
    int32_t sc = (int32_t)c, n = (int32_t)size;
    if (!size)
        return 0;
    if (mode == 1)                         /* wrap */
        return (uint32_t)(((sc % n) + n) % n);
    if (mode == 2) {                       /* mirror */
        int32_t m = ((sc % (2 * n)) + 2 * n) % (2 * n);
        return (uint32_t)(m < n ? m : 2 * n - 1 - m);
    }
    return sc < 0 ? 0u : (sc >= n ? size - 1 : c);   /* clamp, and the rest */
}

/* Address a texel before the shared format decoder, including volume Z. */
static int sample_tex_3d(const Texture *t,uint32_t face_offset,uint32_t u,uint32_t v,
                         uint32_t z,uint32_t *argb)
{
    const uint8_t *mem=(const uint8_t *)xbox_GetMemoryOffset();
    uint32_t fmt=t->color,base=t->offset+face_offset;
    if(!t->valid)return 0;
    if (t->border_source_color &&
        ((t->addr_u == 4 && u >= t->width) ||
         (t->addr_v == 4 && v >= t->height) ||
         (t->addr_w == 4 && z >= (t->depth ? t->depth : 1)))) {
        *argb = t->border_color;
        return 1;
    }
    u=wrap_coord(u,t->width,t->addr_u);
    v=wrap_coord(v,t->height,t->addr_v);
    z=wrap_coord(z,t->depth?t->depth:1,t->addr_w);
    uint32_t block=d3d8_format_dxt_block_bytes(fmt);
    if (block) {
        /* Decoded blocks, direct-mapped by address. Every texel of a DXT
         * texture decodes its whole 4x4 block, and bilinear reads four
         * neighbours that nearly always share one -- this was a tenth of
         * the executor's time. Keyed on the block's bytes as well as its
         * address, so a texture rewritten in place (a render target reused,
         * a streamed mip) never returns stale texels.
         * The TLS table gives each raster worker an independent decoder cache. */
        static NV_TLS struct { uintptr_t key; uint32_t fmt; uint64_t raw[2];
                        uint32_t px[16]; } cache[4096];
        uint32_t bb = block;
        const uint8_t *blk = mem + base + nv2a_texture_dxt_block_offset(t,u,v,z);
        uintptr_t key = (uintptr_t)blk;
        size_t slot = ((key / bb) ^ (key >> 16)) & 4095;
        uint64_t raw[2] = {0, 0};
        memcpy(raw, blk, bb);
        if (cache[slot].key != key || cache[slot].fmt != fmt
            || cache[slot].raw[0] != raw[0] || cache[slot].raw[1] != raw[1]) {
            uint32_t i;
            for (i = 0; i < 16; i++)
                d3d8_dxt_decode_texel(blk, fmt, i & 3,
                                      i >> 2, 4,
                                      &cache[slot].px[i]);
            cache[slot].key = key;
            cache[slot].fmt = fmt;
            cache[slot].raw[0] = raw[0];
            cache[slot].raw[1] = raw[1];
        }
        *argb = cache[slot].px[(v & 3) * 4 + (u & 3)];
        return 1;
    }
    return nv2a_texture_decode(mem,t,face_offset,u,v,z,argb);
}
static int sample_tex(const Texture *t,uint32_t face_offset,uint32_t u,uint32_t v,uint32_t *argb)
{
    return sample_tex_3d(t,face_offset,u,v,0,argb);
}

static int sample_texture(uint32_t u, uint32_t v, uint32_t *argb)
{
    return sample_tex(&s_gpu.texs[0], 0, u, v, argb);
}

/* Write a bound texture out as a BMP, through the sampler rather than around it.
 *
 * "Which texture is this" is not answerable from an address and a format, and
 * it is the question behind most of the ones that matter -- is that a font
 * page or an icon atlas, did the swizzle decode, is the alpha inverted. Going
 * through sample_texture means the file shows exactly what the rasteriser
 * sees, so a decode bug appears here rather than only as a wrong-looking
 * triangle.
 *
 * ponytail: RGB only, alpha dropped. A glyph page is alpha and would come out
 * black, so alpha is composited onto mid-grey to stay legible; that is a
 * viewing choice, not a decode. One file per distinct texture, first use only.
 */
static void dump_texture_bmp(uint32_t seq)
{
    const char *prefix = getenv("RECOMP_TEX_DUMP");
    uint32_t w = s_gpu.texs[0].width, h = s_gpu.texs[0].height, x, y;
    uint32_t row_bytes, pad, filesz;
    uint8_t hdr[54];
    char path[512];
    FILE *f;

    if (!prefix || !w || !h || w > 4096 || h > 4096)
        return;
    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;

    snprintf(path, sizeof path, "%s%02u_%08X_fmt%02X.bmp",
             prefix, seq, s_gpu.texs[0].offset, s_gpu.texs[0].color);
    f = fopen(path, "wb");
    if (!f)
        return;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t argb = 0, a;
            uint8_t px[3];
            if (!sample_texture(x, h - 1 - y, &argb))
                argb = 0;
            a = (argb >> 24) & 0xFFu;
            /* over mid-grey, so an alpha-only page is visible either way */
            px[0] = (uint8_t)(((argb & 0xFFu) * a + 128u * (255u - a)) / 255u);
            px[1] = (uint8_t)((((argb >> 8) & 0xFFu) * a + 128u * (255u - a)) / 255u);
            px[2] = (uint8_t)((((argb >> 16) & 0xFFu) * a + 128u * (255u - a)) / 255u);
            fwrite(px, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = {0, 0, 0};
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
    fprintf(stderr, "  [TEXDUMP] %s (%ux%u fmt 0x%02X)\n",
            path, w, h, s_gpu.texs[0].color);
    fflush(stderr);
}

/* The surface, resolved once per batch.
 *
 * dma_resolve consults the contiguous arena's high-water mark and
 * surface_hits_image walks the image range; both were being done per pixel
 * -- dma_resolve twice -- which cost more than the rasterisation they
 * guarded. Neither answer can change inside a batch, because the colour
 * offset arrives as a method and a method cannot arrive mid-triangle.
 *
 * This is not a micro-optimisation for its own sake: the loader's video
 * paces on frames actually presented, so the rasteriser's throughput is the
 * playback rate. */
static uint8_t *s_surface;          /* host address of surface row 0 */

static size_t surface_index(int x, int y, uint8_t **row)
{
    if (s_swz_on) {
        *row = s_surface;
        return swizzle_deposit((uint32_t)x, s_swz_mx)
             | swizzle_deposit((uint32_t)y, s_swz_my);
    }
    *row = s_surface + (size_t)y * s_gpu.pitch;
    return (size_t)x;
}

static int surface_begin_batch(const uint8_t *mem)
{
    uint32_t base = dma_resolve(s_gpu.color_offset);
    uint64_t bytes;

    surface_swizzle_update();

#if defined(__APPLE__)
    if (nv2a_dispatch_registered() && !registered_color_range(NULL)) {
        nv2a_dispatch_report_failure("software color mapping");
        return 0;
    }
#endif
    if (s_swz_on) {
        uint32_t wx = (s_gpu.format >> 16) & 15u;
        uint32_t hy = (s_gpu.format >> 24) & 15u;
        bytes = (UINT64_C(1) << (wx + hy)) * surface_bpp();
    } else {
        bytes = (uint64_t)(s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch;
    }
    if (bytes > UINT32_MAX || surface_hits_image(base, (uint32_t)bytes))
        return 0;
    s_surface = (uint8_t *)mem + base;
    return 1;
}

static int trace_cpu_pixel(uint32_t x,uint32_t y,uint32_t *argb)
{
    uint8_t *row;
    uint32_t bpp=surface_bpp();
    if(!argb || x<s_gpu.clip_x || y<s_gpu.clip_y ||
       x-s_gpu.clip_x>=s_gpu.clip_w || y-s_gpu.clip_y>=s_gpu.clip_h ||
       !surface_begin_batch((const uint8_t *)xbox_GetMemoryOffset()))
        return 0;
    if(s_swz_on && (x>>(s_gpu.format>>16&15u) || y>>(s_gpu.format>>24&15u)))
        return 0;
    size_t index=surface_index((int)x,(int)y,&row);
    if(bpp==4)*argb=((const uint32_t *)row)[index];
    else if(bpp==2){
        *argb=unpack_surface16(((const uint16_t *)row)[index]);
    }else return 0;
    return 1;
}

static uint32_t pack_color(const float c[4]);

/* One GL blend factor, per channel (r g b a). */
static void blend_factor(uint32_t f, const float s[4], const float d[4],
                         float out[4])
{
    int k;
    for (k = 0; k < 4; k++) {
        float c = (float)((s_gpu.blend_color >> (k == 3 ? 24 : 16 - 8 * k))
                          & 0xFF) / 255.0f;
        switch (f) {
        case 0x0000: out[k] = 0.0f;            break;   /* ZERO                */
        case 0x0001: out[k] = 1.0f;            break;   /* ONE                 */
        case 0x0300: out[k] = s[k];            break;   /* SRC_COLOR           */
        case 0x0301: out[k] = 1.0f - s[k];     break;
        case 0x0302: out[k] = s[3];            break;   /* SRC_ALPHA           */
        case 0x0303: out[k] = 1.0f - s[3];     break;
        case 0x0304: out[k] = d[3];            break;   /* DST_ALPHA           */
        case 0x0305: out[k] = 1.0f - d[3];     break;
        case 0x0306: out[k] = d[k];            break;   /* DST_COLOR           */
        case 0x0307: out[k] = 1.0f - d[k];     break;
        case 0x0308: out[k] = k == 3 ? 1.0f : fminf(s[3], 1.0f - d[3]); break;
        case 0x8001: out[k] = c;               break;   /* CONSTANT_COLOR      */
        case 0x8002: out[k] = 1.0f - c;        break;
        case 0x8003: out[k] = (float)(s_gpu.blend_color >> 24) / 255.0f; break;
        case 0x8004: out[k] = 1.0f - (float)(s_gpu.blend_color >> 24) / 255.0f; break;
        default:     out[k] = f ? 1.0f : 0.0f; break;
        }
    }
}

static void put_pixel_internal(uint8_t *mem, uint32_t bpp, int x, int y,
                               uint32_t argb, int track_stats)
{
    uint8_t *row;
    size_t xi;

    (void)mem;
    if (x < (int)s_gpu.clip_x || x >= (int)(s_gpu.clip_x + s_gpu.clip_w))
        return;
    if (y < (int)s_gpu.clip_y || y >= (int)(s_gpu.clip_y + s_gpu.clip_h))
        return;
    if (s_swz_on && ((uint32_t)x >> ((s_gpu.format >> 16) & 15u) ||
                     (uint32_t)y >> ((s_gpu.format >> 24) & 15u)))
        return;
    if (track_stats) {
        s_gpu.pixels++;
        if ((argb & 0x00FFFFFFu) > (s_gpu.pixel_max & 0x00FFFFFFu))
            s_gpu.pixel_max = argb;
    }
    xi = surface_index(x, y, &row);

    /* Blending, with the GL factor set and equations the NV2A takes.
     *
     * Only SRC_ALPHA / ONE_MINUS_SRC_ALPHA used to be honoured and every other
     * pair was written opaque. Menus never noticed; a race does: Burnout 3
     * finishes its 3D frame with screen-space passes that modulate or add
     * over the scene, and written opaque they paint the whole view over. */
    if (s_gpu.blend_enable
        && !(s_gpu.blend_sfactor == 1 && s_gpu.blend_dfactor == 0)) {
        uint32_t dst = 0xFF000000u;
        float sc[4], dc[4], sf[4], df[4], out[4];
        int k;
        if (bpp == 4) {
            dst = ((const uint32_t *)row)[xi];
        } else if (bpp == 2) {
            dst = unpack_surface16(((const uint16_t *)row)[xi]);
        }
        for (k = 0; k < 4; k++) {           /* r g b a, 0..1 */
            int sh = k == 3 ? 24 : 16 - 8 * k;
            sc[k] = (float)((argb >> sh) & 0xFF) / 255.0f;
            dc[k] = (float)((dst  >> sh) & 0xFF) / 255.0f;
        }
        blend_factor(s_gpu.blend_sfactor, sc, dc, sf);
        blend_factor(s_gpu.blend_dfactor, sc, dc, df);
        for (k = 0; k < 4; k++) {
            float a = sc[k] * sf[k], b = dc[k] * df[k];
            switch (s_gpu.blend_equation) {
            case 0x800A: out[k] = a - b; break;               /* SUBTRACT     */
            case 0x800B: out[k] = b - a; break;               /* REV_SUBTRACT */
            case 0x8007: out[k] = fminf(sc[k], dc[k]); break; /* MIN          */
            case 0x8008: out[k] = fmaxf(sc[k], dc[k]); break; /* MAX          */
            default:     out[k] = a + b; break;               /* ADD          */
            }
        }
        argb = pack_color(out);
    }

    if (s_gpu.color_mask != 0x01010101u) {
        uint32_t keep = 0, old;
        if (!(s_gpu.color_mask & 0x01000000u)) keep |= 0xFF000000u;
        if (!(s_gpu.color_mask & 0x00010000u)) keep |= 0x00FF0000u;
        if (!(s_gpu.color_mask & 0x00000100u)) keep |= 0x0000FF00u;
        if (!(s_gpu.color_mask & 0x00000001u)) keep |= 0x000000FFu;
        if (keep == 0xFFFFFFFFu)
            return;
        if (bpp == 4) {
            old = ((const uint32_t *)row)[xi];
        } else {
            old = unpack_surface16(((const uint16_t *)row)[xi]);
        }
        argb = (argb & ~keep) | (old & keep);
    }
    if (bpp == 4) {
        ((uint32_t *)row)[xi] = argb;
    } else if (bpp == 2) {
        ((uint16_t *)row)[xi] = pack_surface16(argb);
    }
}

static void put_pixel(uint8_t *mem, uint32_t bpp, int x, int y, uint32_t argb)
{
    put_pixel_internal(mem,bpp,x,y,argb,1);
}

/* Half-space fill. Barycentric edge functions rather than scanline slopes:
 * the same test decides both windings, so a title that emits clockwise
 * triangles does not silently render nothing. */
static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb,
                            const float uv[3][2])
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    float area;
    int minx, maxx, miny, maxy, x, y;
    int textured = uv && s_gpu.texs[0].valid;

    if (bpp != 4 && bpp != 2)
        return;

    area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (area == 0.0f)
        return;                            /* degenerate */
#if defined(__APPLE__)
    if (registered_culled(area)) return;
#endif

    /* Where this batch writes. The same check the per-pixel path made, made
     * once: a surface address landing on the title's own image is no safer
     * one pixel at a time than 4.9 MB at once. */
    if (!surface_begin_batch(mem))
        return;

    minx = (int)floorf(fminf(a[0], fminf(b[0], c[0])));
    maxx = (int)ceilf (fmaxf(a[0], fmaxf(b[0], c[0])));
    miny = (int)floorf(fminf(a[1], fminf(b[1], c[1])));
    maxy = (int)ceilf (fmaxf(a[1], fmaxf(b[1], c[1])));

    if (minx < (int)s_gpu.clip_x) minx = (int)s_gpu.clip_x;
    if (miny < (int)s_gpu.clip_y) miny = (int)s_gpu.clip_y;
    if (maxx > (int)(s_gpu.clip_x + s_gpu.clip_w)) maxx = (int)(s_gpu.clip_x + s_gpu.clip_w);
    if (maxy > (int)(s_gpu.clip_y + s_gpu.clip_h)) maxy = (int)(s_gpu.clip_y + s_gpu.clip_h);
    if (minx >= maxx || miny >= maxy) {
        s_gpu.tris_skipped_offscreen++;
        return;
    }

    for (y = miny; y < maxy; y++) {
        for (x = minx; x < maxx; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
            float w1 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0]);
            float w2 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0]);
            if (!((w0 >= 0 && w1 >= 0 && w2 >= 0)
               || (w0 <= 0 && w1 <= 0 && w2 <= 0)))
                continue;
            if (textured) {
                /* Barycentric, straight from the edge functions already
                 * computed: w1 is the area opposite a, w2 opposite b, w0
                 * opposite c, and the three sum to the whole triangle.
                 *
                 * No perspective divide. These are screen-space vertices with
                 * no w to divide by -- which is exactly the case a full-screen
                 * pass is, and the only case that reaches here. */
                uint32_t texel;
                float su = (w1 * uv[0][0] + w2 * uv[1][0] + w0 * uv[2][0]) / area;
                float sv = (w1 * uv[0][1] + w2 * uv[1][1] + w0 * uv[2][1]) / area;
                if (su < 0.0f) su = 0.0f;
                if (sv < 0.0f) sv = 0.0f;
                if (sample_texture((uint32_t)su, (uint32_t)sv, &texel)) {
                    put_pixel(mem, bpp, x, y, texel);
                    continue;
                }
            }
            put_pixel(mem, bpp, x, y, argb);
        }
    }
    s_gpu.tris_drawn++;
    note_drawn();
}

/* Attribute 3 is diffuse colour in every NV2A layout that sets one. Absent it,
 * white -- a visible wrong colour beats an invisible correct one during
 * bring-up. */
/* Which attribute carries the colour.
 *
 * Slot 3 is diffuse by convention and titles that follow it are read straight
 * from there. Half-Life 2 does not: its vertex is position, colour, texcoord
 * at stride 24, and the colour arrives in slot 5. So fall back to the format
 * rather than the slot number -- D3DCOLOR is the one attribute type that is
 * only ever a colour, which makes it a stronger signal than the convention. */
static const VertexAttr *color_attr(void)
{
    uint32_t a;

    if (s_gpu.attr[3].offset && s_gpu.attr[3].stride)
        return &s_gpu.attr[3];
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].type == 0 && s_gpu.attr[a].size == 4
                && s_gpu.attr[a].offset && s_gpu.attr[a].stride)
            return &s_gpu.attr[a];
    return &s_gpu.attr[3];
}

/* Attribute 9 is texture coordinate 0 in the NV2A vertex layout, the same way
 * 0 is position and 3 is diffuse -- for a title that follows the convention.
 *
 * Half-Life 2 does not, in either place. Its menu and HUD vertex is position,
 * colour, texcoord at stride 24, with the colour in slot 5 and the texcoords
 * in slot 7, so reading slot 9 found nothing and every batch drew untextured.
 * That is invisible rather than wrong-looking: the menu paints a full-screen
 * quad and then draws its text over it, and with no sampling both come out
 * white, so the screen is blank white and nothing suggests the text was ever
 * drawn.
 *
 * Falling back to the format works because the three attributes of such a
 * vertex are distinguishable: position is float3, colour is D3DCOLOR, and a
 * float2 is a texture coordinate and nothing else.
 *
 * ponytail: takes the first float2 it finds, so a title with two texcoord sets
 * gets stage 0's -- which is what this single-texture rasteriser samples
 * anyway. Multi-texture wants the D3D11 translator, not another heuristic. */
static const VertexAttr *texcoord_attr(void)
{
    uint32_t a;

    if (s_gpu.attr[9].offset && s_gpu.attr[9].stride)
        return &s_gpu.attr[9];
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].type == 2 && s_gpu.attr[a].size == 2
                && s_gpu.attr[a].offset && s_gpu.attr[a].stride)
            return &s_gpu.attr[a];
    return &s_gpu.attr[9];
}

/* Texel coordinates, whichever convention the title used.
 *
 * The two are not interchangeable and the format decides which is in force: a
 * swizzled texture is addressed in [0,1], a linear one in texels. Both are
 * scaled to texels here so that everything downstream -- the barycentric
 * interpolation and the sampler -- works in one unit.
 *
 * This mattered the moment swizzled formats became samplable. Normalised
 * coordinates truncated to a texel index land on texel 0 for any coordinate
 * below 1.0, so a whole quad sampled a single texel and came out flat: the
 * background painted one near-black colour, which looks like a texture that
 * decoded wrong rather than one that was never indexed. */
static int fetch_texcoord(uint32_t index, float out[2])
{
    float t[4];

    if (!fetch_attr(texcoord_attr(), index, t))
        return 0;
    out[0] = t[0];
    out[1] = t[1];
    if (tex_size_from_format(s_gpu.texs[0].color)) {
        out[0] *= (float)s_gpu.texs[0].width;
        out[1] *= (float)s_gpu.texs[0].height;
    }
    return 1;
}

static uint32_t vertex_color(uint32_t index)
{
    float c[4];

    if (!fetch_attr(color_attr(), index, c))
        return 0xFFFFFFFFu;
    return ((uint32_t)(c[3] * 255.0f) << 24)
         | ((uint32_t)(c[0] * 255.0f) << 16)
         | ((uint32_t)(c[1] * 255.0f) <<  8)
         |  (uint32_t)(c[2] * 255.0f);
}

/* An untransformed batch drawn as if it were screen space smears a few pixels
 * into the corner, so the batch has to be classified before it is rasterised.
 *
 * This used to demand that every vertex land inside the surface, which is a
 * different question and the wrong one: geometry that extends past the
 * viewport is ordinary, and clipping it is raster_triangle's job (it clamps
 * its span to the clip rect). The dashboard is exactly the case that exposed
 * it -- a full-screen pass drawn as one oversized triangle, vertices at
 * (-0.5,-0.5), (2*w,-0.5), (-0.5,2*h), all correct and all rejected.
 *
 * What actually separates the two is scale. Object-space positions are model
 * units, a handful either side of the origin; screen-space ones are measured
 * in pixels of a surface hundreds of pixels wide. So: the batch has to be able
 * to touch the surface at all, and it has to be bigger than object space.
 *
 * ponytail: a genuinely tiny screen-space sprite reads as object space and is
 * skipped. It is counted as skipped rather than silently dropped, and the
 * unambiguous answer needs the vertex-program state, which is not tracked yet.
 */
#define OBJECT_SPACE_SPAN 8.0f

static int batch_is_screen_space(void)
{
    float p[4], lo_x, hi_x, lo_y, hi_y;
    uint32_t i;

    if (!s_gpu.clip_w || !s_gpu.clip_h || !s_gpu.indices.count)
        return 0;
    if (!fetch_attr(&s_gpu.attr[0], s_gpu.indices.values[0], p))
        return 0;
    lo_x = hi_x = p[0];
    lo_y = hi_y = p[1];
    for (i = 1; i < s_gpu.indices.count; i++) {
        if (!fetch_attr(&s_gpu.attr[0], s_gpu.indices.values[i], p))
            return 0;
        if (p[0] < lo_x) lo_x = p[0];
        if (p[0] > hi_x) hi_x = p[0];
        if (p[1] < lo_y) lo_y = p[1];
        if (p[1] > hi_y) hi_y = p[1];
    }

    /* Entirely off the surface: nothing to draw under either reading. */
    if (hi_x < (float)s_gpu.clip_x
     || lo_x > (float)(s_gpu.clip_x + s_gpu.clip_w)
     || hi_y < (float)s_gpu.clip_y
     || lo_y > (float)(s_gpu.clip_y + s_gpu.clip_h))
        return 0;

    /* Small enough to be model units rather than pixels. */
    if (hi_x - lo_x < OBJECT_SPACE_SPAN && hi_y - lo_y < OBJECT_SPACE_SPAN)
        return 0;

    return 1;
}

/* NV097 primitive types.
 *
 * These are the operand of SET_BEGIN_END, where 0 is END and the list starts
 * at 1. They were each one too low, so every title's geometry was decomposed
 * as the primitive below the one it asked for -- a strip as a fan, a fan as
 * quads, and TRIANGLES, the one case whose vertex count must be a multiple
 * of three, as a strip.
 *
 * The vertex order says which numbering is right without taking a table on
 * trust: a strip arrives in Z order and a fan in cyclic order, and they only
 * line up with the primitive under this one. */
#define NV_PRIM_POINTS         1
#define NV_PRIM_LINES          2
#define NV_PRIM_LINE_LOOP      3
#define NV_PRIM_LINE_STRIP     4
#define NV_PRIM_TRIANGLES      5
#define NV_PRIM_TRIANGLE_STRIP 6
#define NV_PRIM_TRIANGLE_FAN   7
#define NV_PRIM_QUADS          8
#define NV_PRIM_QUAD_STRIP     9
#define NV_PRIM_POLYGON        10

static int strip_alt_winding(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("RECOMP_STRIP_ALT_WINDING");
        enabled = !(value && value[0] == '0');
    }
    return enabled;
}

/* How many post-draw captures to keep: enough to see whether the geometry
 * is stable from frame to frame, few enough not to fill a directory. */
#define FB_DUMP_AFTER_DRAW 8
static int s_drawn_dumps;

static void dump_surface_bmp(void);

/* One triangle by vertex index: gather position and, if the batch has one,
 * texture coordinate 0. A vertex whose position cannot be read is not drawn;
 * a batch whose texcoords cannot be read is drawn untextured rather than not
 * at all, so a missing coordinate stream costs the colour and not the shape.
 */
static void raster_indexed(uint32_t i0, uint32_t i1, uint32_t i2, uint32_t argb)
{
    float p[3][4], uv[3][2];
    int textured;

    if (!fetch_attr(&s_gpu.attr[0], i0, p[0])
     || !fetch_attr(&s_gpu.attr[0], i1, p[1])
     || !fetch_attr(&s_gpu.attr[0], i2, p[2]))
        return;

#if defined(__APPLE__)
    if (nv2a_dispatch_registered())
        for (unsigned i = 0; i < 3; ++i) registered_quantize_screen(p[i]);
#endif
    textured = fetch_texcoord(i0, uv[0])
            && fetch_texcoord(i1, uv[1])
            && fetch_texcoord(i2, uv[2]);

    raster_triangle(p[0], p[1], p[2], argb,
                    textured ? (const float (*)[2])uv : NULL);
}

/* ---- 3D: vertex programs and depth -------------------------------------
 *
 * A batch drawn while TRANSFORM_EXECUTION_MODE says "program" carries
 * model-space positions. Each vertex is run through the title's own vertex
 * program (nv2a_vsh_interp.c), whose oPos output on Xbox D3D is already
 * screen space -- the runtime appends the viewport transform and the divide
 * by w -- with the clip-space w left in oPos.w. So the rasteriser gets pixel
 * coordinates and a depth, and w for perspective-correct texturing.
 *
 * ponytail: no clipping. A triangle with any vertex behind the eye (w <= 0)
 * is dropped rather than clipped, which loses the slivers that cross the near
 * plane; road right under the camera is where that shows. Clip in
 * homogeneous space if it matters. */

/* Depth buffers, host-side, one per zeta surface the title uses. The real one
 * lives in guest memory in a tiled format nothing here decodes, and nothing
 * the title does reads it back, so a float buffer per zeta offset is enough. */
#define NV_ZBUF_W 1024
#define NV_ZBUF_H 1024
static struct { uint32_t offset; float *z; } s_zbufs[4];
static int s_ftrace;            /* frame trace: 0 idle, 2 tracing, 3 done */
static int s_zbuf_next;

static float zclear_value(void)
{
    /* Zeta format in SURFACE_FORMAT bits 4-7: 1 is Z16, 2 is Z24S8. */
    uint32_t zf = (s_gpu.format >> 4) & 0xF;
    return zf == 1 ? (float)(s_gpu.zstencil_clear & 0xFFFF)
                   : (float)(s_gpu.zstencil_clear >> 8);
}

static float *zbuf_current(int create)
{
    int i;
    size_t k;

    for (i = 0; i < 4; i++)
        if (s_zbufs[i].z && s_zbufs[i].offset == s_gpu.zeta_offset)
            return s_zbufs[i].z;
    if (!create)
        return NULL;
    i = s_zbuf_next++ & 3;
    if (!s_zbufs[i].z)
        s_zbufs[i].z = (float *)malloc(sizeof(float) * NV_ZBUF_W * NV_ZBUF_H);
    if (!s_zbufs[i].z)
        return NULL;
    s_zbufs[i].offset = s_gpu.zeta_offset;
    for (k = 0; k < (size_t)NV_ZBUF_W * NV_ZBUF_H; k++)
        s_zbufs[i].z[k] = 3.4e38f;
    return s_zbufs[i].z;
}

static void zbuf_clear(void)
{
    float *z = zbuf_current(1), v = zclear_value();
    size_t k;
    if (z)
        for (k = 0; k < (size_t)NV_ZBUF_W * NV_ZBUF_H; k++)
            z[k] = v;
}

#if defined(__APPLE__)
/* The software vertex path keeps a private float Z buffer. A synchronous
 * registered backend instead publishes linear Z16/Z24S8 in guest RAM, so
 * import before a fallback batch and export its writes before the next
 * hardware batch. Swizzled/unknown Z formats remain software-only. */
typedef struct RegisteredDepthRegion {
    uint8_t *base;
    uint64_t offset, bytes;
    unsigned domain;
    uint32_t pitch, bpp, format, x, y, w, h;
} RegisteredDepthRegion;

static int registered_depth_region(RegisteredDepthRegion *r, int clear_rect, int shadow_bounds)
{
    uint32_t format = (s_gpu.format >> 4) & 15u;
    uint32_t type = (s_gpu.format >> 8) & 15u;
    if (!nv2a_dispatch_registered() || !s_gpu.zeta_offset ||
        (format != 1 && format != 2) || type > 1 || (s_gpu.format & 0xf000u))
        return 0;
    uint64_t left = s_gpu.clip_x, top = s_gpu.clip_y;
    uint64_t right = left + s_gpu.clip_w, bottom = top + s_gpu.clip_h;
    if (clear_rect && s_gpu.clear_rect_set == 3) {
        uint64_t clear_right = (uint64_t)s_gpu.clear_x + s_gpu.clear_w;
        uint64_t clear_bottom = (uint64_t)s_gpu.clear_y + s_gpu.clear_h;
        if (left < s_gpu.clear_x) left = s_gpu.clear_x;
        if (top < s_gpu.clear_y) top = s_gpu.clear_y;
        if (right > clear_right) right = clear_right;
        if (bottom > clear_bottom) bottom = clear_bottom;
    }
    if (shadow_bounds) {
        if (right > NV_ZBUF_W) right = NV_ZBUF_W;
        if (bottom > NV_ZBUF_H) bottom = NV_ZBUF_H;
    }
    memset(r, 0, sizeof(*r));
    if (right <= left || bottom <= top) return 1;
    uint32_t bpp = format == 1 ? 2 : 4;
    uint32_t color_bpp = surface_bpp();
    uint64_t pitch = s_gpu.zeta_pitch;
    if (!pitch && color_bpp && s_gpu.pitch % color_bpp == 0)
        pitch = (uint64_t)(s_gpu.pitch / color_bpp) * bpp;
    if (!pitch || pitch > UINT32_MAX || pitch % bpp || right * bpp > pitch ||
        bottom - 1 > UINT32_MAX / pitch)
        return -1;
    uint64_t bytes = (bottom - 1) * pitch + right * bpp;
    uint32_t va = dma_resolve(s_gpu.zeta_offset);
    RegisteredRamRange backing;
    if (!va || bytes > UINT32_MAX || !registered_ram_range(va, bytes, &backing)) return -1;
    r->base = backing.base;
    r->offset = backing.offset; r->bytes = backing.bytes; r->domain = backing.domain;
    r->pitch = (uint32_t)pitch; r->bpp = bpp; r->format = format;
    r->x = (uint32_t)left; r->y = (uint32_t)top;
    r->w = (uint32_t)(right - left); r->h = (uint32_t)(bottom - top);
    return 1;
}

static int registered_color_depth_safe(int clear_rect, int shadow_bounds)
{
    RegisteredDepthRegion depth;
    int status = registered_depth_region(&depth, clear_rect, shadow_bounds);
    if (status < 0) return -1;
    if (!status || !depth.w || !depth.h) return 1;
    RegisteredRamRange color;
    if (!registered_color_range(&color)) return -1;
    return !color.bytes || color.domain != depth.domain ||
           color.offset >= depth.offset + depth.bytes ||
           depth.offset >= color.offset + color.bytes;
}

static int registered_depth_transfer(int write_back)
{
    RegisteredDepthRegion r;
    int status = registered_depth_region(&r, 0, 1);
    if (status <= 0 || !r.w || !r.h) return status;
    float *z = zbuf_current(1);
    if (!z) return -1;
    double maximum = r.format == 1 ? 65535.0 : 16777215.0;
    if (write_back) {
        for (uint32_t y = 0; y < r.h; ++y)
            for (uint32_t x = 0; x < r.w; ++x) {
                float value = z[(size_t)(r.y + y) * NV_ZBUF_W + r.x + x];
                if (!isfinite(value) || value < 0 || value > maximum) return -1;
            }
    }
    for (uint32_t y = 0; y < r.h; ++y) {
        uint8_t *row = r.base + (size_t)(r.y + y) * r.pitch + (size_t)r.x * r.bpp;
        for (uint32_t x = 0; x < r.w; ++x) {
            uint32_t packed = 0;
            float *value = &z[(size_t)(r.y + y) * NV_ZBUF_W + r.x + x];
            memcpy(&packed, row + (size_t)x * r.bpp, r.bpp);
            if (!write_back) {
                *value = (float)(r.format == 1 ? packed : packed >> 8);
            } else {
                uint32_t depth = (uint32_t)floor((double)*value + 0.5);
                packed = r.format == 1 ? depth : (depth << 8) | (packed & 255u);
                memcpy(row + (size_t)x * r.bpp, &packed, r.bpp);
            }
        }
    }
    return 1;
}

static int registered_depth_clear(uint32_t mask)
{
    RegisteredDepthRegion r;
    int status = registered_depth_region(&r, 1, 0);
    if (status <= 0 || !r.w || !r.h) return status;
    uint32_t z = r.format == 1 ? s_gpu.zstencil_clear & 0xffffu
                               : s_gpu.zstencil_clear >> 8;
    for (uint32_t y = 0; y < r.h; ++y) {
        uint8_t *row = r.base + (size_t)(r.y + y) * r.pitch + (size_t)r.x * r.bpp;
        for (uint32_t x = 0; x < r.w; ++x) {
            uint32_t packed = 0;
            memcpy(&packed, row + (size_t)x * r.bpp, r.bpp);
            if (r.format == 1) {
                if (mask & 1u) packed = z;
            } else {
                if (mask & 1u) packed = (z << 8) | (packed & 255u);
                if (mask & 2u) packed = (packed & 0xffffff00u) | (s_gpu.zstencil_clear & 255u);
            }
            memcpy(row + (size_t)x * r.bpp, &packed, r.bpp);
        }
    }
    return 1;
}
#endif

static int depth_pass(float z, float stored)
{
    switch (s_gpu.depth_func) {                   /* GL enums, as the NV2A */
    case 0x200: return 0;
    case 0x201: return z <  stored;
    case 0x202: return z == stored;
    case 0x203: return z <= stored;
    case 0x204: return z >  stored;
    case 0x205: return z != stored;
    case 0x206: return z >= stored;
    default:    return 1;
    }
}

static uint32_t pack_color(const float c[4])
{
    int i;
    uint32_t b[4];
    for (i = 0; i < 4; i++) {
        float f = c[i] < 0.0f ? 0.0f : (c[i] > 1.0f ? 1.0f : c[i]);
        b[i] = (uint32_t)(f * 255.0f + 0.5f);
    }
    return (b[3] << 24) | (b[0] << 16) | (b[1] << 8) | b[2];
}

static uint32_t modulate(uint32_t t, const float c[4])
{
    float f[4];
    f[0] = (float)((t >> 16) & 0xFF) / 255.0f * c[0];
    f[1] = (float)((t >> 8) & 0xFF) / 255.0f * c[1];
    f[2] = (float)(t & 0xFF) / 255.0f * c[2];
    f[3] = (float)(t >> 24) / 255.0f * c[3];
    return pack_color(f);
}

/* Bytes one texel of a (non-DXT) format takes, for cube-face strides. */
/* Bytes between cube faces: one face with all its mip levels, rounded up to
 * the NV2A's 128-byte face alignment (xemu texture.c). */
static uint32_t tex_face_stride(const Texture *t)
{
    uint32_t w = t->width, h = t->height, lv, total = 0;
    uint32_t block = d3d8_format_dxt_block_bytes(t->color);
    uint32_t levels = t->levels ? t->levels : 1;

    if (!tex_size_from_format(t->color))
        return (t->pitch * t->height + 127u) & ~127u;
    for (lv = 0; lv < levels; lv++) {
        uint32_t lw = (w >> lv) ? (w >> lv) : 1, lh = (h >> lv) ? (h >> lv) : 1;
        total += block ? ((lw + 3) / 4) * ((lh + 3) / 4) * block
                       : lw * lh * nv2a_texture_texel_bytes(t->color);
    }
    return (total + 127u) & ~127u;
}

/* One texel at normalised (swizzled/DXT) or texel (linear) coordinates, as
 * floats. An unusable stage reads white, so a missing texture multiplies
 * through instead of blacking the pixel out. */
static void rc_texel_depth(const Texture *t,uint32_t face,float u,float v,float w,float out[4])
{
    uint32_t texel,mag=(t->filter>>24)&15,min=(t->filter>>16)&255;
    int volume=t->dimensionality==3;
    if(tex_size_from_format(t->color)) {u*=t->width;v*=t->height;if(volume)w*=t->depth;}
    if(mag==2 || min==2 || min==4 || min==6) {
        float fu=u-.5f,fv=v-.5f,fw=w-.5f;
        int32_t iu=(int32_t)floorf(fu),iv=(int32_t)floorf(fv),iw=volume?(int32_t)floorf(fw):0;
        float wu=fu-iu,wv=fv-iv,ww=volume?fw-iw:0,c[8][4];
        for(int k=0;k<(volume?8:4);k++) {
            if(sample_tex_3d(t,face,(uint32_t)(iu+(k&1)),(uint32_t)(iv+((k>>1)&1)),
                              (uint32_t)(iw+(k>>2)),&texel))nv2a_rc_unpack(texel,c[k]);
            else c[k][0]=c[k][1]=c[k][2]=c[k][3]=1;
        }
        for(int j=0;j<4;j++) {
            float low=(c[0][j]*(1-wu)+c[1][j]*wu)*(1-wv)+(c[2][j]*(1-wu)+c[3][j]*wu)*wv;
            float high=volume?(c[4][j]*(1-wu)+c[5][j]*wu)*(1-wv)+(c[6][j]*(1-wu)+c[7][j]*wu)*wv:low;
            out[j]=low*(1-ww)+high*ww;
        }
        return;
    }
    if(sample_tex_3d(t,face,(uint32_t)(int32_t)floorf(u),(uint32_t)(int32_t)floorf(v),
                        volume?(uint32_t)(int32_t)floorf(w):0,&texel))nv2a_rc_unpack(texel,out);
    else out[0]=out[1]=out[2]=out[3]=1;
}
static void rc_texel(const Texture *t,uint32_t face,float u,float v,float out[4])
{
    rc_texel_depth(t,face,u,v,0,out);
}

static void rc_texel_cube(const Texture *t,const float c[3],float out[4])
{
    float x=c[0],y=c[1],z=c[2];
    float ax=fabsf(x),ay=fabsf(y),az=fabsf(z),ma,sc,tc;
    uint32_t face;
    if(!t->cube){
        if(ax>=ay && ax>=az){
            ma=ax;sc=x>0?-z:z;tc=y;
        }else if(ay>=az){
            ma=ay;sc=x;tc=y>0?-z:z;
        }else{
            ma=az;sc=z>0?x:-x;tc=y;
        }
        if(ma<1.0e-20f)ma=1.0e-20f;
        rc_texel(t,0,sc/ma,tc/ma,out);
        return;
    }
    if(ax>=ay && ax>=az){
        face=x>0?0:1;ma=ax;sc=x>0?-z:z;tc=-y;
    }else if(ay>=az){
        face=y>0?2:3;ma=ay;sc=x;tc=y>0?z:-z;
    }else{
        face=z>0?4:5;ma=az;sc=z>0?x:-x;tc=-y;
    }
    if(ma<1.0e-20f)ma=1.0e-20f;
    rc_texel(t,face*tex_face_stride(t),(sc/ma+1.0f)*0.5f,
             (tc/ma+1.0f)*0.5f,out);
}

static uint32_t rc_stage_input(int st)
{
    if (st < 2)
        return 0;
    uint32_t input = (s_gpu.rc.other_stage_input >> (st == 2 ? 16 : 20)) & 15u;
    return input < (uint32_t)st ? input : (uint32_t)st - 1u;
}

static uint32_t rc_stage_dot_map(int st)
{
    return st ? (s_gpu.rc.other_stage_input >> ((st - 1) * 4)) & 15u : 0;
}

static void rc_signed_dot_map(const float in[3],uint32_t mode,float out[3])
{
    for (int i = 0; i < 3; i++) {
        float b = in[i] * 255.0f;
        if (mode == 1)
            out[i] = (b - 128.0f) / 127.0f;
        else if (mode == 2)
            out[i] = b >= 128.0f ? (b - 255.5f) / 127.5f
                                 : (b + 0.5f) / 127.5f;
        else
            out[i] = b >= 128.0f ? (b - 256.0f) / 127.0f : b / 127.0f;
    }
}

static void rc_dot_map(const float in[4],uint32_t mode,float out[3])
{
    if (mode == 0) {
        memcpy(out,in,3*sizeof(*out));
        return;
    }
    if (mode <= 3) {
        rc_signed_dot_map(in,mode,out);
        return;
    }
    if (mode == 4) {
        uint32_t r=(uint32_t)(in[0]*255.0f),g=(uint32_t)(in[1]*255.0f);
        uint32_t b=(uint32_t)(in[2]*255.0f),a=(uint32_t)(in[3]*255.0f);
        out[0]=(float)((a<<8)|r)/65535.0f;
        out[1]=(float)((g<<8)|b)/65535.0f;
        out[2]=1.0f;
        return;
    }
    memcpy(out,in,3*sizeof(*out));
}

/* What texture stage `st` contributes, by its SHADER_STAGE_PROGRAM mode
 * (xemu psh.c). Returns 0 when a clip-plane stage kills the pixel. */
static int rc_stage_fetch(int st,const float coords[4][4],const float prior[4][4],
                          float dots[4],float out[4])
{
    const Texture *t = &s_gpu.texs[st];
    uint32_t mode = (s_gpu.rc.stage_program >> (st * 5)) & 0x1F, j;
    const float *c = coords[st];
    float q = c[3] != 0.0f ? c[3] : 1.0f;

    switch (mode) {
    case 0:                                       /* NONE */
        out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f;
        return 1;
    case 1: case 2:                               /* PROJECT2D / 3D */
        rc_texel_depth(t,0,c[0]/q,c[1]/q,mode==2?c[2]/q:0,out);
        return 1;
    case 3:                                       /* CUBEMAP */
        rc_texel_cube(t,c,out);
        return 1;
    case 4:                                       /* PASSTHRU */
        for (j = 0; j < 4; j++)
            out[j] = c[j] < 0.0f ? 0.0f : (c[j] > 1.0f ? 1.0f : c[j]);
        return 1;
    case 5:                                       /* CLIPPLANE */
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        for (j = 0; j < 4; j++) {
            int ge = (s_gpu.clip_plane_mode >> (st * 4 + j)) & 1;
            if (ge ? c[j] >= 0.0f : c[j] < 0.0f)
                return 0;
        }
        return 1;
    case 6: case 7: {                             /* BUMPENVMAP / BUMPENVMAP_LUM */
        const float *previous=prior[rc_stage_input(st)];
        float encoded[3]={previous[2],previous[1],previous[1]},bump[3];
        const float *m=s_gpu.bump_matrix[st];
        rc_signed_dot_map(encoded,3,bump);
        rc_texel(t,0,c[0]+m[0]*bump[0]+m[2]*bump[1],
                 c[1]+m[1]*bump[0]+m[3]*bump[1],out);
        if(mode==7){
            float luminance=s_gpu.bump_scale[st]*previous[0]+s_gpu.bump_offset[st];
            for(j=0;j<4;j++)out[j]*=luminance;
        }
        return 1;
    }
    case 9: case 10: case 11: case 12: case 13: case 14:
    case 17: case 18: {
        float mapped[3];
        const float *previous=prior[rc_stage_input(st)];
        rc_dot_map(previous,rc_stage_dot_map(st),mapped);
        dots[st]=c[0]*mapped[0]+c[1]*mapped[1]+c[2]*mapped[2];
        out[0]=out[1]=out[2]=out[3]=0.0f;
        if(mode==10 || mode==17)return 1;
        if(mode==9){
            uint32_t previous_dot=st>1?(uint32_t)st-1u:0u;
            rc_texel(t,0,dots[previous_dot],dots[st],out);
            return 1;
        }
        if(mode==11){
            float next_mapped[3];
            const float *next=prior[rc_stage_input(3)];
            float cube[3];
            rc_dot_map(next,rc_stage_dot_map(3),next_mapped);
            cube[0]=dots[1];
            cube[1]=dots[2];
            cube[2]=coords[3][0]*next_mapped[0]+coords[3][1]*next_mapped[1]+
                    coords[3][2]*next_mapped[2];
            rc_texel_cube(t,cube,out);
            return 1;
        }
        float n[3]={dots[1],dots[2],dots[3]};
        if(mode==12 || mode==18){
            float e[3]={
                mode==18?s_gpu.eye_vector[0]:coords[1][3],
                mode==18?s_gpu.eye_vector[1]:coords[2][3],
                mode==18?s_gpu.eye_vector[2]:coords[3][3]
            };
            float nn=n[0]*n[0]+n[1]*n[1]+n[2]*n[2];
            float scale=2.0f*(n[0]*e[0]+n[1]*e[1]+n[2]*e[2])/
                        (nn>1.0e-20f?nn:1.0e-20f);
            for(j=0;j<3;j++)n[j]=n[j]*scale-e[j];
        }
        if(mode==13)rc_texel(t,0,n[0],n[1],out);
        else rc_texel_cube(t,n,out);
        return 1;
    }
    case 15: {                                    /* DEPENDENT_AR */
        const float *previous=prior[rc_stage_input(st)];
        rc_texel(t,0,previous[3],previous[0],out);
        return 1;
    }
    case 16: {                                    /* DEPENDENT_GB */
        const float *previous=prior[rc_stage_input(st)];
        rc_texel(t,0,previous[1],previous[2],out);
        return 1;
    }
    default: {
        static uint32_t warned_modes;
        uint32_t bit=1u<<mode;
        if(!(__atomic_fetch_or(&warned_modes,bit,__ATOMIC_RELAXED)&bit)){
            fprintf(stderr,"[GPU] unsupported CPU texture stage mode %u (stage %d)\n",
                    mode,st);
        }
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return 1;
    }
    }
}

/* The fixed-function fog unit, per vertex, from the program's oFog.x
 * (xemu vsh.c). Fog disabled reads as factor 1: no fog. */
static float fog_factor(float d)
{
    float f, px = s_gpu.fog_param[0], py = s_gpu.fog_param[1];
    if (!s_gpu.fog_enable)
        return 1.0f;
    switch (s_gpu.fog_mode) {
    case 0x800: case 0x802:                       /* EXP, EXP_ABS */
        f = px + exp2f(d * py * 16.0f) - 1.5f;
        break;
    case 0x801: case 0x803:                       /* EXP2, EXP2_ABS */
        f = px + exp2f(-d * d * py * py * 32.0f) - 1.5f;
        break;
    default:                                      /* LINEAR, LINEAR_ABS */
        f = px + d * py - 1.0f;
        break;
    }
    if (s_gpu.fog_mode == 0x802 || s_gpu.fog_mode == 0x803
        || s_gpu.fog_mode == 0x804)
        f = fabsf(f);
    if (isinf(d) || f != f)
        f = s_gpu.fog_mode == 0x800 || s_gpu.fog_mode == 0x2601 ? 1.0f : 0.0f;
    /* Fog saturates after interpolation, unlike vertex colors. */
    return fmaxf(-3.402823466e38f, fminf(3.402823466e38f, f));
}

static int alpha_test_pass(float a)
{
    int v = (int)(a * 255.0f + 0.5f), r = (int)(s_gpu.alpha_ref & 0xFF);
    switch (s_gpu.alpha_func) {
    case 0x200: return 0;                         /* NEVER    */
    case 0x201: return v <  r;                    /* LESS     */
    case 0x202: return v == r;                    /* EQUAL    */
    case 0x203: return v <= r;                    /* LEQUAL   */
    case 0x204: return v >  r;                    /* GREATER  */
    case 0x205: return v != r;                    /* NOTEQUAL */
    case 0x206: return v >= r;                    /* GEQUAL   */
    default:    return 1;                         /* ALWAYS   */
    }
}

/* Everything a triangle's pixels need, computed once per triangle, so the
 * pixel loop can run over any subset of rows on any thread. */
typedef struct {
    uint8_t *mem;
    uint32_t bpp;
    const Nv2aVshOutput *va, *vb, *vc;
    const float *a, *b, *c;
    float iw[3], uv[3][2], stc[3][4][4], vfog[3], fogc[4], inv_area;
    float *zb;
    int minx, maxx, miny, maxy, use_rc, textured;
#if defined(__APPLE__)
    int quantize_depth;
#endif
} XfTri;

typedef struct {
    uint64_t depth_fail,pixels;
    uint32_t zpass,pixel_max;
} XfCount;

static void xf_count_merge(XfCount *dst,const XfCount *src)
{
    dst->depth_fail+=src->depth_fail;
    dst->pixels+=src->pixels;
    dst->zpass+=src->zpass;
    if((src->pixel_max&0x00ffffffu)>(dst->pixel_max&0x00ffffffu))
        dst->pixel_max=src->pixel_max;
}

/* Rows y0, y0+step, ... < maxy of triangle T. */
static void xf_rows(const XfTri *T, int y0, int step, XfCount *cnt)
{
    const float *a = T->a, *b = T->b, *c = T->c;
    const Nv2aVshOutput *va = T->va, *vb = T->vb, *vc = T->vc;
    int x, y, k;

    for (y = y0; y < T->maxy; y += step) {
        for (x = T->minx; x < T->maxx; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
            float w1 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0]);
            float w2 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0]);
            float l0, l1, l2, z, col[4], pw;
            uint32_t argb;
            if (!((w0 >= 0 && w1 >= 0 && w2 >= 0)
               || (w0 <= 0 && w1 <= 0 && w2 <= 0)))
                continue;
            /* w1 is opposite a, w2 opposite b, w0 opposite c. */
            l0 = w1 * T->inv_area; l1 = w2 * T->inv_area; l2 = w0 * T->inv_area;
            z = l0 * a[2] + l1 * b[2] + l2 * c[2];
            if (s_gpu.z_clamp) {
                float maximum = s_gpu.clip_max > 0 ? s_gpu.clip_max :
                    ((s_gpu.format >> 4) & 15) == 1 ? 65535.0f : 16777215.0f;
                if (z < s_gpu.clip_min) z = s_gpu.clip_min;
                if (z > maximum) z = maximum;
            }
#if defined(__APPLE__)
            if (T->quantize_depth && isfinite(z) && z >= 0.0f &&
                z <= (((s_gpu.format >> 4) & 15u) == 1 ? 65535.0f : 16777215.0f))
                z = (float)floor((double)z + 0.5);
#endif
            {
                float *zp = T->zb ? &T->zb[(size_t)y * NV_ZBUF_W + x] : NULL;
                int shade = (s_gpu.color_mask & 0x01010101u) || s_gpu.alpha_test;
                /* Depth test, then shade, then alpha test, and only then the
                 * depth write: an alpha-tested texel that is cut away must
                 * not leave its depth behind (foliage, fences). */
                if (zp && s_gpu.depth_test && !depth_pass(z, *zp)) {
                    cnt->depth_fail++;
                    continue;
                }
                argb = 0;
                if (shade) {
                    pw = l0 * T->iw[0] + l1 * T->iw[1] + l2 * T->iw[2];
                    float p0 = l0 * T->iw[0] / pw;
                    float p1 = l1 * T->iw[1] / pw;
                    float p2 = l2 * T->iw[2] / pw;
                    for (k = 0; k < 4; k++)
                        col[k] = p0 * va->d0[k] + p1 * vb->d0[k] + p2 * vc->d0[k];
                    if (T->use_rc) {
                        float d1[4], fog[4], t[4][4]={{0}}, out[4], tc[4][4], dots[4]={0};
                        int st, j, keep = 1;
                        for (k = 0; k < 4; k++)
                            d1[k] = p0 * va->d1[k] + p1 * vb->d1[k] + p2 * vc->d1[k];
                        fog[0] = T->fogc[0]; fog[1] = T->fogc[1]; fog[2] = T->fogc[2];
                        fog[3] = p0 * T->vfog[0] + p1 * T->vfog[1] + p2 * T->vfog[2];
                        fog[3] = fmaxf(0.0f, fminf(1.0f, fog[3]));
                        for(st=0;st<4;st++)
                            for(j=0;j<4;j++)
                                tc[st][j]=(l0*T->stc[0][st][j]+l1*T->stc[1][st][j]+
                                           l2*T->stc[2][st][j])/pw;
                        for (st = 0; st < 4 && keep; st++) {
                            if (!((s_gpu.rc.stage_program >> (st * 5)) & 0x1F)) {
                                t[st][0] = t[st][1] = t[st][2] = 0.0f;
                                t[st][3] = 1.0f;
                                continue;
                            }
                            keep = rc_stage_fetch(st,(const float (*)[4])tc,
                                                  (const float (*)[4])t,dots,t[st]);
                        }
                        if (!keep)
                            continue;
                        nv2a_rc_eval(&s_gpu.rc, col, d1, fog,
                                     (const float (*)[4])t, out);
                        if (s_gpu.alpha_test && !alpha_test_pass(out[3]))
                            continue;
                        argb = pack_color(out);
                    } else {
                        argb = pack_color(col);
                        if (T->textured) {
                            uint32_t texel;
                            float tu, tv;
                            tu = (l0 * T->uv[0][0] + l1 * T->uv[1][0] + l2 * T->uv[2][0]) / pw;
                            tv = (l0 * T->uv[0][1] + l1 * T->uv[1][1] + l2 * T->uv[2][1]) / pw;
                            if (sample_texture((uint32_t)(int32_t)floorf(tu),
                                               (uint32_t)(int32_t)floorf(tv), &texel))
                                argb = modulate(texel, col);
                        }
                        if (s_gpu.alpha_test
                            && !alpha_test_pass((float)(argb >> 24) / 255.0f))
                            continue;
                    }
                }
                if (zp && s_gpu.depth_test && s_gpu.depth_mask)
                    *zp = z;
                if (s_gpu.zpass_enable)
                    cnt->zpass++;
                if (!(s_gpu.color_mask & 0x01010101u))
                    continue;              /* colour writes off: depth only */
                cnt->pixels++;
                if((argb&0x00ffffffu)>(cnt->pixel_max&0x00ffffffu))
                    cnt->pixel_max=argb;
            }
            put_pixel_internal(T->mem,T->bpp,x,y,argb,0);
        }
    }
}

/* Worker threads for big triangles.
 *
 * The rasteriser is a software GPU, and a Burnout 3 frame is dominated by a
 * handful of full-screen passes -- composites, blurs, the menu backdrops --
 * each two triangles of 300,000 pixels through the register combiners. Those
 * split cleanly by row: every pixel reads only its own depth and colour, so
 * interleaved rows on N threads need no locking. Small triangles stay on the
 * executor thread, where waking workers would cost more than it saves.
 * RECOMP_RASTER_THREADS=<n> sets the count (1 = off). Windows retains its
 * established topology-derived default. POSIX remains serial by default until
 * retail A/B evidence shows that waking workers helps rather than hurts.
 * Workers collect statistics locally, then the executor merges them after all
 * rows complete. */
#define NV_RASTER_MAX_THREADS 16
#define NV_RASTER_MT_MIN_PIXELS 8192

static int raster_has_color_feedback(const XfTri *tri)
{
    uint32_t color=dma_resolve(s_gpu.color_offset);
    for(unsigned st=0;st<4;st++){
        int sampled=tri->use_rc?
            ((s_gpu.rc.stage_program>>(st*5))&0x1f)!=0:
            (tri->textured && st==0);
        if(sampled && s_gpu.texs[st].valid &&
           dma_resolve(s_gpu.texs[st].offset)==color)
            return 1;
    }
    return 0;
}

#if defined(_WIN32)
static struct {
    int n;                                  /* threads incl. the caller */
    HANDLE start[NV_RASTER_MAX_THREADS], done;
    volatile LONG pending;
    const XfTri *tri;
    XfCount cnt[NV_RASTER_MAX_THREADS];
} s_pool;

static DWORD WINAPI raster_worker(LPVOID arg)
{
    int k = (int)(intptr_t)arg;
    pc_thread_role_enter(PC_ROLE_RASTER);
    for (;;) {
        WaitForSingleObject(s_pool.start[k], INFINITE);
        memset(&s_pool.cnt[k], 0, sizeof s_pool.cnt[k]);
        xf_rows(s_pool.tri, s_pool.tri->miny + k, s_pool.n, &s_pool.cnt[k]);
        if (InterlockedDecrement(&s_pool.pending) == 0)
            SetEvent(s_pool.done);
    }
    return 0;
}

static int raster_pool_size(void)
{
    static int init;
    if (!init) {
        const char *e = getenv("RECOMP_RASTER_THREADS");
        SYSTEM_INFO si;
        int n, k;
        init = 1;
        GetSystemInfo(&si);
        n = e ? atoi(e) : (int)pc_cpu_logical() - 4;   /* M0: the process's CPUs, not the machine's */
        (void)si;
        if (n < 1) n = 1;
        if (n > NV_RASTER_MAX_THREADS) n = NV_RASTER_MAX_THREADS;
        s_pool.n = n;
        if (n > 1) {
            s_pool.done = CreateEventA(NULL, FALSE, FALSE, NULL);
            for (k = 0; k < n - 1; k++) {
                s_pool.start[k] = CreateEventA(NULL, FALSE, FALSE, NULL);
                CloseHandle(CreateThread(NULL, 0, raster_worker,
                                         (LPVOID)(intptr_t)k, 0, NULL));
            }
        }
    }
    return s_pool.n;
}

static void xf_rows_parallel(const XfTri *T, XfCount *total)
{
    int n,k;
    long px = (long)(T->maxx - T->minx) * (T->maxy - T->miny);

    if(raster_has_color_feedback(T)){
        xf_rows(T,T->miny,1,total);
        return;
    }
    n=raster_pool_size();
    if (n <= 1 || px < NV_RASTER_MT_MIN_PIXELS || T->maxy - T->miny < n) {
        xf_rows(T, T->miny, 1, total);
        return;
    }
    double profile_start = perf_clock_ms();
    s_pool.tri = T;
    s_pool.pending = n - 1;
    for (k = 0; k < n - 1; k++)
        SetEvent(s_pool.start[k]);
    {
        XfCount mine = {0};
        xf_rows(T, T->miny + (n - 1), n, &mine);
        WaitForSingleObject(s_pool.done, INFINITE);
        xf_count_merge(total,&mine);
    }
    for (k = 0; k < n - 1; k++) {
        xf_count_merge(total,&s_pool.cnt[k]);
    }
    if (profile_start) {
        s_perf.parallel_ms += perf_clock_ms() - profile_start;
        s_perf.parallel_jobs++;
    }
}
static void raster_pool_shutdown(void) {}
#else
static struct {
    pthread_mutex_t mutex;
    pthread_cond_t start,done;
    pthread_t threads[NV_RASTER_MAX_THREADS-1];
    int initialized,n,created,stop;
    unsigned generation;
    int pending;
    const XfTri *tri;
    XfCount cnt[NV_RASTER_MAX_THREADS];
} s_pool = {
    .mutex=PTHREAD_MUTEX_INITIALIZER,
    .start=PTHREAD_COND_INITIALIZER,
    .done=PTHREAD_COND_INITIALIZER
};

static void *raster_worker(void *arg)
{
    int k=(int)(intptr_t)arg;
    unsigned seen=0;
    pc_thread_role_enter(PC_ROLE_RASTER);
    pthread_mutex_lock(&s_pool.mutex);
    for(;;){
        while(!s_pool.stop && seen==s_pool.generation)
            pthread_cond_wait(&s_pool.start,&s_pool.mutex);
        if(s_pool.stop)break;
        seen=s_pool.generation;
        const XfTri *tri=s_pool.tri;
        int n=s_pool.n;
        pthread_mutex_unlock(&s_pool.mutex);
        memset(&s_pool.cnt[k],0,sizeof s_pool.cnt[k]);
        xf_rows(tri,tri->miny+k,n,&s_pool.cnt[k]);
        pthread_mutex_lock(&s_pool.mutex);
        if(--s_pool.pending==0)pthread_cond_signal(&s_pool.done);
    }
    pthread_mutex_unlock(&s_pool.mutex);
    pc_thread_role_leave();
    return NULL;
}

static int raster_pool_size(void)
{
    if(!s_pool.initialized){
        const char *e=getenv("RECOMP_RASTER_THREADS");
        int requested=e?atoi(e):1;
        if(requested<1)requested=1;
        if(requested>NV_RASTER_MAX_THREADS)requested=NV_RASTER_MAX_THREADS;
        s_pool.initialized=1;
        s_pool.stop=0;
        s_pool.created=0;
        for(int k=0;k<requested-1;k++){
            if(pthread_create(&s_pool.threads[k],NULL,raster_worker,
                              (void *)(intptr_t)k)!=0)
                break;
            s_pool.created++;
        }
        s_pool.n=s_pool.created+1;
        if(s_pool.n<requested)
            fprintf(stderr,"[GPU] POSIX raster pool reduced from %d to %d threads\n",
                    requested,s_pool.n);
    }
    return s_pool.n;
}

static void xf_rows_parallel(const XfTri *T, XfCount *total)
{
    int n,k;
    long px=(long)(T->maxx-T->minx)*(T->maxy-T->miny);
    if(raster_has_color_feedback(T)){
        xf_rows(T,T->miny,1,total);
        return;
    }
    n=raster_pool_size();
    if(n<=1 || px<NV_RASTER_MT_MIN_PIXELS || T->maxy-T->miny<n){
        xf_rows(T,T->miny,1,total);
        return;
    }
    double profile_start=perf_clock_ms();
    pthread_mutex_lock(&s_pool.mutex);
    s_pool.tri=T;
    s_pool.pending=s_pool.created;
    s_pool.generation++;
    pthread_cond_broadcast(&s_pool.start);
    pthread_mutex_unlock(&s_pool.mutex);
    XfCount mine={0};
    xf_rows(T,T->miny+n-1,n,&mine);
    pthread_mutex_lock(&s_pool.mutex);
    while(s_pool.pending)pthread_cond_wait(&s_pool.done,&s_pool.mutex);
    pthread_mutex_unlock(&s_pool.mutex);
    xf_count_merge(total,&mine);
    for(k=0;k<s_pool.created;k++){
        xf_count_merge(total,&s_pool.cnt[k]);
    }
    if(profile_start){
        s_perf.parallel_ms+=perf_clock_ms()-profile_start;
        s_perf.parallel_jobs++;
    }
}

static void raster_pool_shutdown(void)
{
    if(!s_pool.initialized)return;
    pthread_mutex_lock(&s_pool.mutex);
    s_pool.stop=1;
    s_pool.generation++;
    pthread_cond_broadcast(&s_pool.start);
    pthread_mutex_unlock(&s_pool.mutex);
    for(int k=0;k<s_pool.created;k++)pthread_join(s_pool.threads[k],NULL);
    s_pool.initialized=0;
    s_pool.n=s_pool.created=0;
    s_pool.stop=0;
}
#endif

/* The fixed switch preserves its existing process-lifetime behavior. The flag
 * form is sampled at completed flips so diagnostics can restore full shading
 * without a filesystem query in every triangle or pixel. */
static struct {
    int initialized;
    int fixed;
    int enabled;
    const char *while_file;
} s_no_combiners;

static void no_combiners_refresh(void)
{
    int enabled;
    if (!s_no_combiners.initialized) {
        s_no_combiners.fixed = getenv("RECOMP_NO_COMBINERS") != NULL;
        s_no_combiners.while_file = getenv("RECOMP_NO_COMBINERS_WHILE_FILE");
        s_no_combiners.enabled = -1;
        s_no_combiners.initialized = 1;
    }
    enabled = s_no_combiners.fixed;
    if (!enabled && s_no_combiners.while_file && *s_no_combiners.while_file)
#if defined(_WIN32)
        enabled = GetFileAttributesA(s_no_combiners.while_file) != INVALID_FILE_ATTRIBUTES;
#else
        enabled = access(s_no_combiners.while_file, F_OK) == 0;
#endif
    if (enabled != s_no_combiners.enabled) {
        s_no_combiners.enabled = enabled;
        if (s_no_combiners.while_file && *s_no_combiners.while_file)
            fprintf(stderr, "[NV2A] diagnostic register combiners %s at frame boundary\n",
                    enabled ? "disabled" : "restored");
    }
}

static int no_combiners_enabled(void)
{
    if (!s_no_combiners.initialized)
        no_combiners_refresh();
    return s_no_combiners.enabled;
}

static void raster_xf_triangle(const Nv2aVshOutput *va, const Nv2aVshOutput *vb,
                               const Nv2aVshOutput *vc)
{
    XfTri T;
    XfCount cnt = {0};
    const Nv2aVshOutput *v[3];
    const float *a, *b, *c;
    float area, su = 1.0f, sv = 1.0f;
    int k;
    memset(&T, 0, sizeof T);
    T.mem = (uint8_t *)xbox_GetMemoryOffset();
    T.bpp = surface_bpp();
    T.textured = s_gpu.texs[0].valid;
    T.use_rc = s_gpu.rc_seen && !no_combiners_enabled();
#if defined(__APPLE__)
    uint32_t zformat = (s_gpu.format >> 4) & 15u;
    T.quantize_depth = nv2a_dispatch_registered() && s_gpu.depth_test && s_gpu.zeta_offset &&
        (zformat == 1 || zformat == 2) && ((s_gpu.format >> 8) & 15u) <= 1u &&
        !(s_gpu.format & 0xf000u);
#endif
    T.va = va; T.vb = vb; T.vc = vc;

    v[0] = va; v[1] = vb; v[2] = vc;
    a = va->pos; b = vb->pos; c = vc->pos;
    T.a = a; T.b = b; T.c = c;
    if (T.bpp != 4 && T.bpp != 2)
        return;
    if (a[3] <= 0.0f || b[3] <= 0.0f || c[3] <= 0.0f) {
        s_gpu.tris_behind++;
        return;
    }
    area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (area == 0.0f || area != area) {
        s_gpu.xf_degenerate++;
        return;
    }
#if defined(__APPLE__)
    if (registered_culled(area)) return;
#endif
    for (k = 0; k < 3; k++) {
        if (!s_gpu.xf_seeded)
            s_gpu.xf_min[k] = s_gpu.xf_max[k] = a[k];
        if (a[k] < s_gpu.xf_min[k]) s_gpu.xf_min[k] = a[k];
        if (a[k] > s_gpu.xf_max[k]) s_gpu.xf_max[k] = a[k];
    }
    s_gpu.xf_seeded = 1;
    if (!surface_begin_batch(T.mem))
        return;
    if (s_gpu.depth_test)
        T.zb = zbuf_current(1);
    if (tex_size_from_format(s_gpu.texs[0].color)) {
        su = (float)s_gpu.texs[0].width;
        sv = (float)s_gpu.texs[0].height;
    }
    for (k = 0; k < 3; k++) {
        T.iw[k] = 1.0f / v[k]->pos[3];
        T.uv[k][0] = v[k]->tex[0][0] * su * T.iw[k];
        T.uv[k][1] = v[k]->tex[0][1] * sv * T.iw[k];
    }
    if (T.use_rc) {
        int st, j;
        for (k = 0; k < 3; k++) {
            for (st = 0; st < 4; st++)
                for (j = 0; j < 4; j++)
                    T.stc[k][st][j] = v[k]->tex[st][j] * T.iw[k];
            T.vfog[k] = fog_factor(v[k]->fog[0]);
        }
        /* FOG_COLOR is R in bits 0-7, the reverse of a D3DCOLOR. */
        nv2a_rc_unpack(s_gpu.fog_color, T.fogc);
        { float r = T.fogc[2]; T.fogc[2] = T.fogc[0]; T.fogc[0] = r; }
    }

    T.minx = (int)floorf(fminf(a[0], fminf(b[0], c[0])));
    T.maxx = (int)ceilf (fmaxf(a[0], fmaxf(b[0], c[0])));
    T.miny = (int)floorf(fminf(a[1], fminf(b[1], c[1])));
    T.maxy = (int)ceilf (fmaxf(a[1], fmaxf(b[1], c[1])));
    if (T.minx < (int)s_gpu.clip_x) T.minx = (int)s_gpu.clip_x;
    if (T.miny < (int)s_gpu.clip_y) T.miny = (int)s_gpu.clip_y;
    if (T.maxx > (int)(s_gpu.clip_x + s_gpu.clip_w)) T.maxx = (int)(s_gpu.clip_x + s_gpu.clip_w);
    if (T.maxy > (int)(s_gpu.clip_y + s_gpu.clip_h)) T.maxy = (int)(s_gpu.clip_y + s_gpu.clip_h);
    if (T.maxx > NV_ZBUF_W) T.maxx = NV_ZBUF_W;
    if (T.maxy > NV_ZBUF_H) T.maxy = NV_ZBUF_H;
    if (T.minx >= T.maxx || T.miny >= T.maxy) {
        s_gpu.tris_skipped_offscreen++;
        s_gpu.xf_offscreen++;
        return;
    }
    s_gpu.xf_drawn++;
    T.inv_area = 1.0f / area;

    xf_rows_parallel(&T, &cnt);
    s_gpu.xf_depth_fail += cnt.depth_fail;
    s_gpu.xf_pixels += cnt.pixels;
    s_gpu.zpass_count += cnt.zpass;
    s_gpu.pixels += cnt.pixels;
    if((cnt.pixel_max&0x00ffffffu)>(s_gpu.pixel_max&0x00ffffffu))
        s_gpu.pixel_max=cnt.pixel_max;
    s_gpu.tris_drawn++;
    note_drawn();
}


/* Near-plane clipping.
 *
 * A triangle with a vertex behind the eye (w <= 0) used to be dropped whole.
 * That is invisible for distant geometry and ruinous close up: in a chase
 * view the road under the camera is exactly the set of triangles that reach
 * behind it, so the bottom of the frame showed a hole -- a quarter of every
 * race frame's triangles went this way.
 *
 * Clipping has to happen in clip space, but the D3D epilogue has already
 * divided x, y and z by w and applied the viewport, leaving w alone. Each
 * vertex is taken to (x*w, y*w, z*w, w), an affine image of clip space that
 * does not depend on where the title keeps its viewport scale.
 *
 * The polygon is clipped against w = NV_CLIP_W and then the near plane
 * z = the viewport's z offset. Clipping at w alone lets geometry between the
 * eye and near plane through at huge screen coordinates, producing torn
 * streaks. RECOMP_CLIP_NEAR_Z=0 retains the old w-only path for comparison.
 * x/y/far clipping remains the rasteriser's bounded responsibility. */
#define NV_CLIP_W 1e-3f

static float s_xf_z_offset;

static void xf_lerp(const Nv2aVshOutput *a, const Nv2aVshOutput *b, float t,
                    Nv2aVshOutput *out)
{
    const float *pa = (const float *)a, *pb = (const float *)b;
    float *po = (float *)out;
    size_t i, n = sizeof(Nv2aVshOutput) / sizeof(float);
    for (i = 0; i < n; i++)
        po[i] = pa[i] + (pb[i] - pa[i]) * t;
}

static float xf_clip_dist(const Nv2aVshOutput *v, int plane)
{
    return plane == 0 ? v->pos[3] - NV_CLIP_W
                      : v->pos[2] - s_xf_z_offset * v->pos[3];
}

static int xf_clip_poly(const Nv2aVshOutput *in, int count, int plane,
                        Nv2aVshOutput *out)
{
    int written = 0;
    for (int i = 0; i < count; ++i) {
        const Nv2aVshOutput *a = &in[i], *b = &in[(i + 1) % count];
        float da = xf_clip_dist(a, plane), db = xf_clip_dist(b, plane);
        if (da >= 0.0f)
            out[written++] = *a;
        if ((da >= 0.0f) != (db >= 0.0f)) {
            Nv2aVshOutput *vertex = &out[written++];
            xf_lerp(a, b, da / (da - db), vertex);
            if (plane == 0)
                vertex->pos[3] = NV_CLIP_W;
            else
                vertex->pos[2] = s_xf_z_offset * vertex->pos[3];
        }
    }
    return written;
}

static void raster_xf_clipped(const Nv2aVshOutput *a, const Nv2aVshOutput *b,
                              const Nv2aVshOutput *c)
{
    static int near_z = -1;
    const Nv2aVshOutput *in[3];
    Nv2aVshOutput first[8], second[8];
    int count = 3, inside = 1;
    int planes;

    if (near_z < 0) {
        const char *value = getenv("RECOMP_CLIP_NEAR_Z");
        near_z = !(value && value[0] == '0');
    }
    planes = near_z ? 2 : 1;
    in[0] = a;
    in[1] = b;
    in[2] = c;
    for (int i = 0; i < 3; ++i) {
        first[i] = *in[i];
        for (int j = 0; j < 3; ++j)
            first[i].pos[j] = in[i]->pos[j] * in[i]->pos[3];
        for (int plane = 0; plane < planes; ++plane) {
            float distance = xf_clip_dist(&first[i], plane);
            if (!isfinite(distance))
                return;
            if (distance < 0.0f)
                inside = 0;
        }
    }
    if (inside) {
        raster_xf_triangle(a, b, c);
        return;
    }
    count = xf_clip_poly(first, count, 0, second);
    if (planes > 1 && count)
        count = xf_clip_poly(second, count, 1, first);
    else
        memcpy(first, second, sizeof second);
    if (count < 3) {
        s_gpu.tris_behind++;
        return;
    }
    for (int i = 0; i < count; ++i)
        for (int j = 0; j < 3; ++j)
            first[i].pos[j] /= first[i].pos[3];
    for (int i = 1; i + 1 < count; ++i)
        raster_xf_triangle(&first[0], &first[i], &first[i + 1]);
}

static Nv2aVshOutput s_xf[NV_MAX_INDICES];

static int transform_vertex(uint32_t index, Nv2aVshOutput *out)
{
    float in[NV2A_VSH_INPUTS][4];
    uint32_t a;

    for (a = 0; a < NV2A_VSH_INPUTS; a++)
        fetch_attr(&s_gpu.attr[a], index, in[a]);   /* absent: 0,0,0,1 */
    return nv2a_vsh_run((const float (*)[4])in, out);
}

static unsigned trace_pixel_triangle(uint32_t a,uint32_t b,uint32_t c,float x,float y,unsigned count)
{
    uint32_t ids[3]={a,b,c};
    double polygons[2][16][4];unsigned n=3,source=0;
    for(unsigned i=0;i<3;i++) {
        const float *p=s_xf[ids[i]].pos;
        polygons[0][i][0]=trunc((double)p[0]*16)/16*p[3];
        polygons[0][i][1]=trunc((double)p[1]*16)/16*p[3];
        polygons[0][i][2]=(double)p[2]*p[3];polygons[0][i][3]=p[3];
    }
    double maximum=s_gpu.clip_max>0?s_gpu.clip_max:((s_gpu.format>>4)&15)==1?65535.0:16777215.0;
    double width=s_gpu.pitch/surface_bpp(),height=s_gpu.clip_y+s_gpu.clip_h;
    /* Screen-space containment is misleading for vertices behind the camera.
     * Clip the diagnostic polygon in homogeneous coordinates first. */
    for(unsigned plane=0;plane<(s_gpu.z_clamp?5u:7u) && n;plane++) {
        unsigned out=0,destination=source^1;
        for(unsigned i=0;i<n;i++) {
            const double *p=polygons[source][i],*q=polygons[source][(i+1)%n];
            double dp,dq;
            switch(plane) {
            case 0:dp=p[3]-1e-6;dq=q[3]-1e-6;break;
            case 1:dp=p[0];dq=q[0];break;
            case 2:dp=width*p[3]-p[0];dq=width*q[3]-q[0];break;
            case 3:dp=p[1];dq=q[1];break;
            case 4:dp=height*p[3]-p[1];dq=height*q[3]-q[1];break;
            case 5:dp=p[2];dq=q[2];break;
            default:dp=maximum*p[3]-p[2];dq=maximum*q[3]-q[2];break;
            }
            if(dp>=0)memcpy(polygons[destination][out++],p,4*sizeof(double));
            if((dp>=0)!=(dq>=0)) {
                double t=dp/(dp-dq);
                for(unsigned j=0;j<4;j++)polygons[destination][out][j]=p[j]+(q[j]-p[j])*t;
                out++;
            }
        }
        n=out;source=destination;
    }
    for(unsigned i=1;i+1<n;i++) {
        double projected[3][3];unsigned pi[3]={0,i,i+1};
        for(unsigned j=0;j<3;j++)for(unsigned k=0;k<3;k++)
            projected[j][k]=polygons[source][pi[j]][k]/polygons[source][pi[j]][3];
        const double *p=projected[0],*q=projected[1],*r=projected[2];
        double area=(q[0]-p[0])*(r[1]-p[1])-(q[1]-p[1])*(r[0]-p[0]);
        if(!isfinite(area) || fabs(area)<0.00001)continue;
        double l1=((x-p[0])*(r[1]-p[1])-(y-p[1])*(r[0]-p[0]))/area;
        double l2=((q[0]-p[0])*(y-p[1])-(q[1]-p[1])*(x-p[0]))/area,l0=1-l1-l2;
        if(l0<0 || l1<0 || l2<0)continue;
        int front=s_gpu.front_face==0x900?area>0:area<0;
        int culled=s_gpu.cull_enable && (s_gpu.cull_face==0x408 ||
            (s_gpu.cull_face==0x404?front:!front));
        if(count<8)fprintf(stderr,"[FTRACE] coverage-clipped tri=%u,%u,%u z=%.9g area=%.9g culled=%d\n",
            a,b,c,l0*p[2]+l1*q[2]+l2*r[2],area,culled);
        count++;
    }
    return count;
}

static void trace_pixel_coverage(unsigned x,unsigned y)
{
    Nv2aVshProgram program;float constants[NV2A_VSH_CONSTANTS][4];
    if((s_gpu.xf_mode&3)!=2 || !nv2a_vsh_snapshot(&program,constants) || program.context_write)return;
    uint32_t n=s_gpu.indices.count;
    for(uint32_t i=0;i<n;i++)if(!transform_vertex(s_gpu.indices.values[i],&s_xf[i]))return;
    const char *vertices_path=getenv("RECOMP_FRAME_TRACE_VERTICES");
    const char *texture_filter=getenv("RECOMP_FRAME_TRACE_TEXTURE");
    if(vertices_path && texture_filter && (!strcmp(texture_filter,"*") || s_gpu.texs[0].offset==(uint32_t)strtoul(texture_filter,NULL,16))) {
        char path[1024];snprintf(path,sizeof(path),"%s-%u.csv",vertices_path,s_gpu.draws);
        FILE *file=fopen(path,"w");
        if(file) {
            fprintf(file,"index,x,y,z,w,u,v,q,fog\n");
            for(uint32_t i=0;i<n;i++)fprintf(file,"%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
                s_gpu.indices.values[i],s_xf[i].pos[0],s_xf[i].pos[1],s_xf[i].pos[2],s_xf[i].pos[3],
                s_xf[i].tex[0][0],s_xf[i].tex[0][1],s_xf[i].tex[0][3],s_xf[i].fog[0]);
            fclose(file);
        }
        snprintf(path,sizeof(path),"%s-%u.json",vertices_path,s_gpu.draws);
        file=fopen(path,"w");
        if(file) {
            fprintf(file,"{\"primitive\":%u,\"texture\":%u,\"surface\":%u,\"cull\":%u,\"front\":%u,\"face\":%u,\"clamp\":%u,\"clip_min\":%.9g,\"clip_max\":%.9g}\n",
                s_gpu.prim,s_gpu.texs[0].offset,s_gpu.color_offset,s_gpu.cull_enable,s_gpu.front_face,s_gpu.cull_face,s_gpu.z_clamp,s_gpu.clip_min,s_gpu.clip_max);
            fclose(file);
        }
        /* Keep the instruction words and fetched inputs beside the coverage
         * trace so an independent interpreter can reproduce a suspect draw.
         * This diagnostic is opt-in and records one armed frame only. */
        snprintf(path,sizeof(path),"%s-%u.vsh",vertices_path,s_gpu.draws);
        file=fopen(path,"wb");
        if(file) {
            const uint32_t header[4]={0x4853564e,2,n,s_gpu.prim};
            fwrite(header,sizeof(header),1,file);
            fwrite(&program,sizeof(program),1,file);
            fwrite(constants,sizeof(constants),1,file);
            for(uint32_t i=0;i<n;i++) {
                float input[NV2A_VSH_INPUTS][4];
                for(unsigned a=0;a<NV2A_VSH_INPUTS;a++)fetch_attr(&s_gpu.attr[a],s_gpu.indices.values[i],input[a]);
                fwrite(&s_gpu.indices.values[i],sizeof(s_gpu.indices.values[i]),1,file);
                fwrite(input,sizeof(input),1,file);
                fwrite(&s_xf[i],sizeof(s_xf[i]),1,file);
            }
            fclose(file);
        }
    }
    unsigned count=0;
    #define TRACE_TRI(a,b,c) count=trace_pixel_triangle(a,b,c,x+0.25f,y+0.25f,count)
    switch(s_gpu.prim) {
    case 5:for(uint32_t i=0;i+2<n;i+=3){TRACE_TRI(i,i+1,i+2);}break;
    case 6:for(uint32_t i=0;i+2<n;i++){if(i&1){TRACE_TRI(i+1,i,i+2);}else{TRACE_TRI(i,i+1,i+2);}}break;
    case 7:case 10:for(uint32_t i=1;i+1<n;i++){TRACE_TRI(0,i,i+1);}break;
    case 8:for(uint32_t i=0;i+3<n;i+=4){TRACE_TRI(i,i+1,i+2);TRACE_TRI(i,i+2,i+3);}break;
    }
    #undef TRACE_TRI
    if(count)fprintf(stderr,"[FTRACE] coverage count=%u clip=%u/%.9g/%.9g control0=%08X stencil=%u/%X/%u\n",
        count,s_gpu.z_clamp,s_gpu.clip_min,s_gpu.clip_max,s_gpu.control0,
        s_gpu.stencil_test,s_gpu.stencil_func,s_gpu.stencil_ref);
}

/* RECOMP_OVERLAY_TRACE=1 (diagnostic, off by default; R27 tint2): shared trace file for full-screen overlay
 * diagnostics (game 'D' filter lines, 'C' covering draws below, 'P' present probes in nv2a_gpu_d3d11.c).
 * RECOMP_OVERLAY_TRACE_FILE sets the path (default overlay_trace.txt in the working directory). */
FILE *nv2a_overlay_trace_file(void)
{
    static int on=-1; static FILE *file;
    if(on<0) {
        const char *v=getenv("RECOMP_OVERLAY_TRACE");on=v && atoi(v)!=0;
        if(on) {
            const char *path=getenv("RECOMP_OVERLAY_TRACE_FILE");
            file=fopen(path && *path?path:"overlay_trace.txt","w");
            if(!file)on=0; else setvbuf(file,NULL,_IOFBF,1<<16);
        }
    }
    return on?file:NULL;
}
uint32_t nv2a_overlay_guest_frame(void){return *(volatile uint32_t *)((const uint8_t *)xbox_GetMemoryOffset()+0x4E2420);}

/* RECOMP_COVER_TRACE=1 (diagnostic, off by default): log blended programmable draws whose vertices cover most
 * of the 640x480 target (screen-washing overlays), with w range, colour and state; at most 40 lines a flip. */
static void cover_trace(void)
{
    static int on=-1; static uint32_t flip=~0u; static unsigned lines;
    if(on<0){const char *v=getenv("RECOMP_COVER_TRACE");on=v && atoi(v)!=0;}
    FILE *ovl=nv2a_overlay_trace_file();
    if((!on && !ovl) || !s_gpu.blend_enable || (s_gpu.xf_mode&3)!=2 || s_gpu.indices.count>256 || s_gpu.clip_w<600)return;
    if(flip!=s_gpu.flips){flip=s_gpu.flips;lines=0;}
    if(lines>=40)return;
    float x0=1e30f,x1=-1e30f,y0=1e30f,y1=-1e30f,w0=1e30f,w1=-1e30f; Nv2aVshOutput o,first;
    for(uint32_t i=0;i<s_gpu.indices.count;i++){
        if(!transform_vertex(s_gpu.indices.values[i],&o))return;
        if(!i)first=o;
        if(o.pos[3]<w0)w0=o.pos[3]; if(o.pos[3]>w1)w1=o.pos[3];
        if(o.pos[3]>0){ if(o.pos[0]<x0)x0=o.pos[0]; if(o.pos[0]>x1)x1=o.pos[0]; if(o.pos[1]<y0)y0=o.pos[1]; if(o.pos[1]>y1)y1=o.pos[1]; }
    }
    int covers=x0<=s_gpu.clip_x+32 && x1>=s_gpu.clip_x+s_gpu.clip_w-32 && y0<=s_gpu.clip_y+32 && y1>=s_gpu.clip_y+s_gpu.clip_h-32;
    if(!covers && !(w0<0 && w1>0))return;
    lines++;
    if(ovl && covers)
        fprintf(ovl,"C %u %u prim %u n %u tex %08X blend %X/%X eq %X bc %08X z %u/%X/%u fog %u w %.3g..%.3g d0 %.3f %.3f %.3f %.3f\n",
            nv2a_overlay_guest_frame(),s_gpu.flips,s_gpu.prim,s_gpu.indices.count,s_gpu.texs[0].valid?s_gpu.texs[0].offset:0,
            s_gpu.blend_sfactor,s_gpu.blend_dfactor,s_gpu.blend_equation,s_gpu.blend_color,s_gpu.depth_test,s_gpu.depth_func,
            s_gpu.depth_mask,s_gpu.fog_enable,w0,w1,first.d0[0],first.d0[1],first.d0[2],first.d0[3]);
    if(!on)return;
    fprintf(stderr,"[COVER] flip %u %s prim %u n %u tex %08X %ux%u fmt %02X blend %X/%X z %u/%X/%u fog %u w %.3g..%.3g box %.0f,%.0f..%.0f,%.0f v0 %.1f %.1f %.4g %.4g d0 %.3f %.3f %.3f %.3f t0 %.3f %.3f\n",
        s_gpu.flips,covers?"COVERS":"MIXEDW",s_gpu.prim,s_gpu.indices.count,s_gpu.texs[0].valid?s_gpu.texs[0].offset:0,
        s_gpu.texs[0].width,s_gpu.texs[0].height,s_gpu.texs[0].color,s_gpu.blend_sfactor,s_gpu.blend_dfactor,
        s_gpu.depth_test,s_gpu.depth_func,s_gpu.depth_mask,s_gpu.fog_enable,w0,w1,x0,y0,x1,y1,
        first.pos[0],first.pos[1],first.pos[2],first.pos[3],first.d0[0],first.d0[1],first.d0[2],first.d0[3],first.tex[0][0],first.tex[0][1]);
}

static void raster_batch_program(void)
{
    cover_trace();
    uint32_t i, n = s_gpu.indices.count;
    double profile_start = perf_clock_ms();
#if defined(__APPLE__)
    int transformed = 0, software_depth = 0;
    /* Counted draws leave the CPU only for a backend with exact results. */
    int allow_hardware = !nv2a_dispatch_registered() || !s_gpu.zpass_enable ||
                         nv2a_dispatch_visibility();
#else
    const int allow_hardware = 1;
#endif
    if(nv2a_gpu_discard_frame()){note_drawn();return;}

    if (nv2a_gpu_enabled()) {
        static Nv2aGpuRawVertex raw[NV_MAX_INDICES];
        static uint32_t remap[65536],indices[NV_MAX_INDICES];
        static uint32_t stamps[65536],keys[65536],generation;
        Nv2aVshProgram program;
        float constants[NV2A_VSH_CONSTANTS][4];
        Nv2aGpuState state;
        hardware_state(&state);
        if (hardware_surface_ok(&state) && nv2a_vsh_snapshot(&program, constants)) {
            /* RECOMP_LIGHTNING_UNITS: 0 = B2 unit lightning boost off, 2 = old match-by-value boost (A/B), else N.L-lit only. */
            static int lt_units=-1;
            if(lt_units<0){const char *v=getenv("RECOMP_LIGHTNING_UNITS");lt_units=(v && *v=='0')?0:(v && *v=='2')?2:1;}
            /* diag RECOMP_LT_PAINT_FILE (R27 tint3): the file holds program hashes (hex, one per line); draws of those
             * programs get +2 on the sun-colour registers they read, always, so a screenshot shows what they draw. */
            static const char *paint_path=(const char *)-1; static uint32_t paint[16], npaint, paint_flip=~0u;
            if(paint_path==(const char *)-1)paint_path=getenv("RECOMP_LT_PAINT_FILE");
            if(paint_path && s_gpu.flips/30!=paint_flip) {
                paint_flip=s_gpu.flips/30; npaint=0; FILE *pf=fopen(paint_path,"r");
                if(pf){unsigned h; while(npaint<16 && fscanf(pf,"%x",&h)==1)paint[npaint++]=h; fclose(pf);}
            }
            if ((g_lt_flash_on && lt_units) || npaint) {
                /* constants this program reads directly (a0-relative reads excluded) */
                uint32_t rd[(NV2A_VSH_CONSTANTS+31)/32]={0},mad[(NV2A_VSH_CONSTANTS+31)/32]={0}; uint32_t hash=2166136261u; int dp3c=0;
                for(uint32_t k=0;k<program.count;k++) {
                    const uint32_t *w=program.words[k];
                    for(int q=1;q<4;q++){hash^=w[q];hash*=16777619u;}
                    if((((w[2]>>26)&3)==3 || ((w[2]>>11)&3)==3 || ((w[3]>>28)&3)==3) && !((w[3]>>1)&1)) {
                        uint32_t ci=(w[1]>>13)&0xFF, mac=(w[1]>>21)&15;
                        if(ci<NV2A_VSH_CONSTANTS) {
                            rd[ci>>5]|=1u<<(ci&31);
                            if(mac==4)mad[ci>>5]|=1u<<(ci&31);   /* MAD: colour x (N.L) + accumulated light */
                            if(mac==5)dp3c=1;                     /* DP3 against a constant: N.L with a light direction */
                        }
                    }
                }
                int boosted=0; char regs[96]; size_t rl=0; regs[0]=0;
                for (int r = 0; r < NV2A_VSH_CONSTANTS; r++)
                    if (fabsf(constants[r][0]-g_lt_sun_col[0])<2e-3f && fabsf(constants[r][1]-g_lt_sun_col[1])<2e-3f
                        && fabsf(constants[r][2]-g_lt_sun_col[2])<2e-3f) {
                        int read=(rd[r>>5]>>(r&31))&1;
                        static const float pc[7][3]={{2,0,0},{0,2,0},{0,0,2},{2,2,0},{0,2,2},{2,0,2},{2,2,2}};
                        int painted=-1; for(unsigned j=0;j<npaint;j++)if(paint[j]==hash)painted=(int)j%7;
                        int lit=read && dp3c && ((mad[r>>5]>>(r&31))&1);   /* sun colour lights this draw through N.L */
                        float add=(g_lt_flash_on && lt_units && (lt_units==2 || lit))?g_lt_flash_add:0.0f;
                        if(painted>=0 && read) {
                            constants[r][0]+=pc[painted][0]; constants[r][1]+=pc[painted][1]; constants[r][2]+=pc[painted][2];
                            boosted=1;
                        } else if(add>0) {
                            constants[r][0]+=add; constants[r][1]+=add; constants[r][2]+=add;
                            boosted=1;
                        }
                        if(rl+8<sizeof regs)rl+=(size_t)snprintf(regs+rl,sizeof regs-rl,"%s%d%s",rl?",":"",r,lit?"":read?"r":"u");
                    }
                FILE *ovl=nv2a_overlay_trace_file();
                int dump=0; for(unsigned j=0;j<npaint;j++)if(paint[j]==1u)dump=1;
                static uint32_t dump_flip=~0u; if(dump && dump_flip==~0u)dump_flip=s_gpu.flips; if(!dump)dump_flip=~0u;
                dump = dump && dump_flip==s_gpu.flips;   /* paint file entry '1': list every draw of one flip */
                if(ovl && ((rl && g_lt_flash_on) || dump)) {   /* diag: draws holding the sun colour during a flash (per flip, first 48) */
                    static uint32_t bflip=~0u; static unsigned bn; static uint32_t seen[512]; static unsigned nseen;
                    if(bflip!=s_gpu.flips){bflip=s_gpu.flips;bn=0;}
                    if(bn++<48 || dump)fprintf(ovl,"B %u %u regs %s n %u tex %08X prog %u h %08X b %d\n",nv2a_overlay_guest_frame(),s_gpu.flips,rl?regs:"-",
                        s_gpu.indices.count,s_gpu.texs[0].valid?s_gpu.texs[0].offset:0,program.count,hash,boosted|(dump?2:0));
                    if(dump) {   /* diag: values of the low constants this program reads (before any boost) */
                        fprintf(ovl,"K %08X n %u tex %08X",hash,s_gpu.indices.count,s_gpu.texs[0].valid?s_gpu.texs[0].offset:0);
                        for(int r=0;r<32;r++)if((rd[0]>>r)&1)fprintf(ovl," c%d=%.3f,%.3f,%.3f,%.3f",r,constants[r][0],constants[r][1],constants[r][2],constants[r][3]);
                        fputc(10,ovl);
                    }
                    unsigned j=0; while(j<nseen && seen[j]!=hash)j++;
                    if(j==nseen && nseen<512) {   /* first sight of this program: dump its words */
                        seen[nseen++]=hash; fprintf(ovl,"V %08X %u",hash,program.count);
                        for(uint32_t k=0;k<program.count;k++)fprintf(ovl," %08X%08X%08X",program.words[k][1],program.words[k][2],program.words[k][3]);
                        fputc('\n',ovl);
                    }
                }
            }
            static int vertex_pull=-1;
            if(vertex_pull<0){const char *v=getenv("RECOMP_VERTEX_PULL");vertex_pull=!v || atoi(v)!=0;}
            if(allow_hardware && vertex_pull && !s_gpu.inline_active) {
                Nv2aGpuVertexAttr attrs[16];
                for(unsigned a=0;a<16;a++) {
                    attrs[a].offset=s_gpu.attr[a].offset;attrs[a].type=s_gpu.attr[a].type;
                    attrs[a].size=s_gpu.attr[a].size;attrs[a].stride=s_gpu.attr[a].stride;
                }
                if(nv2a_gpu_draw_buffers(&state,attrs,(const float (*)[4])s_gpu.imm_attr,s_gpu.indices.values,n,
                    s_gpu.prim,sample_tex_3d,&program,(const float (*)[4])constants,
                    s_gpu.fog_enable,s_gpu.fog_mode,s_gpu.fog_param)) {
                    s_gpu.batches_program++;s_gpu.verts_program+=n;note_drawn();return;
                }
                if(nv2a_gpu_discard_frame()){note_drawn();return;}
                if(nv2a_gpu_program_pending())goto program_cpu_fallback;
            }
            uint32_t inputs=0;
            for(uint32_t k=0;k<program.count;k++) {
                const uint32_t *w=program.words[k];
                if(((w[2]>>26)&3)==2 || ((w[2]>>11)&3)==2 || ((w[3]>>28)&3)==2)
                    inputs|=1u<<((w[1]>>9)&15);
            }
            uint32_t unique=0;
            if(++generation==0){memset(stamps,0,sizeof(stamps));generation=1;}
            uint32_t attributes[16],attribute_count=0;
            for(uint32_t a=0;a<NV2A_VSH_INPUTS;a++)if(inputs&(1u<<a))attributes[attribute_count++]=a;
            for (i = 0; i < n; i++) {
                uint32_t index=s_gpu.indices.values[i],slot=index&65535u;
                if(stamps[slot]!=generation || keys[slot]!=index) {
                    stamps[slot]=generation;keys[slot]=index;remap[slot]=unique;
                    for(uint32_t k=0;k<attribute_count;k++) {
                        uint32_t a=attributes[k];
                        fetch_attr(&s_gpu.attr[a],index,raw[unique].attributes[a]);
                    }
                    unique++;
                }
                indices[i]=remap[slot];
            }
            if (allow_hardware && nv2a_gpu_draw_program_indexed(&state, raw, unique, indices, n, s_gpu.prim, sample_tex_3d, &program,
                    (const float (*)[4])constants, s_gpu.fog_enable, s_gpu.fog_mode, s_gpu.fog_param)) {
                s_gpu.batches_program++; s_gpu.verts_program += n;
                note_drawn();
                return;
            }
        }
program_cpu_fallback:
        if(nv2a_gpu_discard_frame()){note_drawn();return;}
        /* Keep rasterization on the GPU while a new vertex shader compiles.
         * The reference interpreter provides the same vertex outputs without
         * a render-thread compile or a framebuffer round trip. */
        if(nv2a_gpu_program_pending() && hardware_surface_ok(&state)) {
            double fallback_start=nv2a_gpu_cpu_begin();
            for(i=0;i<n;i++)if(!transform_vertex(s_gpu.indices.values[i],&s_xf[i]))break;
            nv2a_gpu_cpu_end(NV2A_CPU_VERTEX_FALLBACK,fallback_start,(uint64_t)i*sizeof(*s_xf));
            if(i==n && nv2a_gpu_draw(&state,s_xf,n,s_gpu.prim,sample_tex_3d)) {
                s_gpu.batches_program++;s_gpu.verts_program+=n;note_drawn();return;
            }
        }
#if defined(__APPLE__)
        /* A registered backend may rasterize the interpreter's screen-space
         * output without implementing guest vertex programs. Retain the
         * transformed batch if it declines: programs can write constants. */
        if (nv2a_dispatch_registered() && allow_hardware && hardware_surface_ok(&state)) {
            for (i = 0; i < n; i++)
                if (!transform_vertex(s_gpu.indices.values[i], &s_xf[i])) return;
            transformed = 1;
            if (nv2a_gpu_draw(&state, s_xf, n, s_gpu.prim, sample_tex_3d)) {
                s_gpu.batches_program++; s_gpu.verts_program += n; note_drawn(); return;
            }
        }
#endif
        if(nv2a_gpu_discard_frame()){note_drawn();return;}
        gpu_fallback_note(2, state.color_va, hardware_surface_ok(&state));
        nv2a_gpu_sync_all();
        nv2a_gpu_invalidate(state.color_va);
    }
    if(nv2a_gpu_discard_frame()){note_drawn();return;}

#if defined(__APPLE__)
    if (!transformed)
#endif
        for (i = 0; i < n; i++)
            if (!transform_vertex(s_gpu.indices.values[i], &s_xf[i]))
                return;                                /* no program loaded */
    s_xf_z_offset = nv2a_vsh_constant(59)[2];
#if defined(__APPLE__)
    if (nv2a_dispatch_registered())
        for (i = 0; i < n; ++i) registered_prepare_vertex(&s_xf[i]);
    if (nv2a_dispatch_registered() && s_gpu.depth_test) {
        if (s_gpu.color_mask & 0x01010101u) {
            int safe = registered_color_depth_safe(0, 1);
            if (safe <= 0) {
                nv2a_dispatch_report_failure(safe ? "software surface mapping" : "software surface overlap");
                return;
            }
        }
        software_depth = registered_depth_transfer(0);
        if (software_depth < 0) {
            nv2a_dispatch_report_failure("software depth import");
            return;
        }
    }
#endif
    s_gpu.batches_program++;
    s_gpu.verts_program += n;
    if (profile_start) s_perf.vertex_ms += perf_clock_ms() - profile_start;
    if (s_gpu.blend_enable) {
        uint32_t pair = s_gpu.blend_sfactor << 16 | (s_gpu.blend_dfactor & 0xFFFF);
        int j;
        for (j = 0; j < s_gpu.blend_npairs && s_gpu.blend_pairs[j] != pair; j++)
            ;
        if (j == s_gpu.blend_npairs && j < 16)
            s_gpu.blend_pairs[s_gpu.blend_npairs++] = pair;
    }
    {
        /* RECOMP_VSH_TRACE=<n>: print n program batches -- attribute setup,
         * raw inputs, key constants, and what came out. */
        static int left = -1;
        if (left < 0) {
            const char *t = getenv("RECOMP_VSH_TRACE");
            left = t ? atoi(t) : 0;
        }
        /* Spread out: one every 20000 program batches, so a run that spends
         * its first half in menus still traces the 3D scene. */
        if (left > 0 && n >= 3 && s_gpu.batches_program % 20000 == 0) {
            float in[4];
            left--;
            fprintf(stderr, "[VTRACE] prim %u n %u idx %u %u %u\n", s_gpu.prim,
                    n, s_gpu.indices.values[0], s_gpu.indices.values[1], s_gpu.indices.values[2]);
            for (i = 0; i < NV_VERTEX_ATTRS; i++) {
                const VertexAttr *at = &s_gpu.attr[i];
                if (!at->size)
                    continue;
                fetch_attr(at, s_gpu.indices.values[0], in);
                fprintf(stderr, "[VTRACE]   v%u off %08X type %u size %u"
                        " stride %u = %g %g %g %g\n", i, at->offset, at->type,
                        at->size, at->stride, in[0], in[1], in[2], in[3]);
            }
            {
                static const uint32_t cs[] = {58, 59, 96, 97, 112, 113, 114, 115};
                for (i = 0; i < 8; i++) {
                    const float *c = nv2a_vsh_constant(cs[i]);
                    fprintf(stderr, "[VTRACE]   c[%u] %g %g %g %g%c", cs[i],
                            c[0], c[1], c[2], c[3], 10);
                }
            }
            for (i = 0; i < 3; i++)
                fprintf(stderr, "[VTRACE]   out%u pos %g %g %g %g\n", i,
                        s_xf[i].pos[0], s_xf[i].pos[1], s_xf[i].pos[2],
                        s_xf[i].pos[3]);
        }
    }
    for (i = 0; i < NV2A_VSH_INPUTS; i++) {
        const VertexAttr *at = &s_gpu.attr[i];
        uint32_t f = at->type | (at->size << 4) | (i << 8), j;
        if (!at->size || !at->stride)
            continue;
        for (j = 0; j < (uint32_t)s_gpu.xf_nfmt && s_gpu.xf_fmt[j] != f; j++)
            ;
        if (j == (uint32_t)s_gpu.xf_nfmt && s_gpu.xf_nfmt < 16)
            s_gpu.xf_fmt[s_gpu.xf_nfmt++] = f;
    }
    if (s_gpu.texs[0].valid)
        note_texture_use();

    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < n; i += 3)
            raster_xf_clipped(&s_xf[i], &s_xf[i+1], &s_xf[i+2]);
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < n; i++) {
            if ((i & 1u) && strip_alt_winding())
                raster_xf_clipped(&s_xf[i+1], &s_xf[i], &s_xf[i+2]);
            else
                raster_xf_clipped(&s_xf[i], &s_xf[i+1], &s_xf[i+2]);
        }
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < n; i++)
            raster_xf_clipped(&s_xf[0], &s_xf[i], &s_xf[i+1]);
        break;
    case NV_PRIM_QUADS:
        for (i = 0; i + 3 < n; i += 4) {
            raster_xf_clipped(&s_xf[i], &s_xf[i+1], &s_xf[i+2]);
            raster_xf_clipped(&s_xf[i], &s_xf[i+2], &s_xf[i+3]);
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        for (i = 0; i + 3 < n; i += 2) {
            raster_xf_clipped(&s_xf[i], &s_xf[i+1], &s_xf[i+3]);
            raster_xf_clipped(&s_xf[i], &s_xf[i+3], &s_xf[i+2]);
        }
        break;
    default:
        break;
    }
#if defined(__APPLE__)
    if (software_depth > 0 && s_gpu.depth_mask && !nv2a_dispatch_failed() &&
        registered_depth_transfer(1) < 0)
        nv2a_dispatch_report_failure("software depth export");
#endif
}

static void raster_batch(void)
{
    uint32_t i;
    uint32_t before = s_gpu.tris_drawn;
    if(nv2a_gpu_discard_frame()){note_drawn();return;}

    /* Streamed line loops end with a separate two-vertex closing edge. */
    uint32_t minimum = s_gpu.prim == NV_PRIM_POINTS ? 1 :
                       s_gpu.prim < NV_PRIM_TRIANGLES ? 2 : 3;
    if (s_gpu.indices.count < minimum)
        return;
    static int no_vsh = -1;
    if (no_vsh < 0)
        no_vsh = getenv("RECOMP_NO_VSH") != NULL;
    if ((s_gpu.xf_mode & 3) == 2 && !no_vsh) {
#if defined(__APPLE__)
        if (s_gpu.depth_test && !registered_depth_mode_guard(0)) return;
#endif
        raster_batch_program();
        return;
    }
    if (!batch_is_screen_space()) {
        s_gpu.batches_untransformed++;
        return;
    }
#if defined(__APPLE__)
    if (s_gpu.depth_test && !registered_depth_mode_guard(0)) return;
#endif

    if (nv2a_gpu_enabled()) {
        static Nv2aVshOutput output[NV_MAX_INDICES];
        Nv2aGpuState state;
        hardware_state(&state);
        state.use_rc = 0; state.legacy = 1;
        const VertexAttr *tc = texcoord_attr();
        state.textured = s_gpu.texs[0].valid && tc->offset && tc->stride;
        if (hardware_surface_ok(&state)
#if defined(__APPLE__)
            && (!nv2a_dispatch_registered() || !s_gpu.zpass_enable ||
                nv2a_dispatch_visibility())
#endif
            ) {
            for (i = 0; i < s_gpu.indices.count; i++) {
                memset(&output[i], 0, sizeof(output[i]));
                fetch_attr(&s_gpu.attr[0], s_gpu.indices.values[i], output[i].pos);
                output[i].pos[3] = 1.0f;
                nv2a_rc_unpack(vertex_color(s_gpu.indices.values[i]), output[i].d0);
                fetch_attr(tc, s_gpu.indices.values[i], output[i].tex[0]);
                output[i].tex[0][3] = 1.0f;
            }
            if (nv2a_gpu_draw(&state, output, s_gpu.indices.count, s_gpu.prim, sample_tex_3d)) {
                note_drawn();
                return;
            }
        }
        if(nv2a_gpu_discard_frame()){note_drawn();return;}
        gpu_fallback_note(2, state.color_va, hardware_surface_ok(&state));
        nv2a_gpu_sync_all();
        nv2a_gpu_invalidate(state.color_va);
    }
    if(nv2a_gpu_discard_frame()){note_drawn();return;}

    /* Count why, once per batch: the texture stage cannot change inside one. */
    {
        const VertexAttr *tc = texcoord_attr();

        if (!(tc->offset && tc->stride))
            s_gpu.batches_no_uv++;
        else if (!s_gpu.texs[0].valid)
            s_gpu.batches_no_tex++;
        else {
            s_gpu.batches_textured++;
            note_texture_use();
        }
    }

    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < s_gpu.indices.count; i += 3)
            raster_indexed(s_gpu.indices.values[i], s_gpu.indices.values[i+1], s_gpu.indices.values[i+2],
                           vertex_color(s_gpu.indices.values[i]));
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < s_gpu.indices.count; i++) {
            if ((i & 1u) && strip_alt_winding()) {
                raster_indexed(s_gpu.indices.values[i+1], s_gpu.indices.values[i], s_gpu.indices.values[i+2],
                               vertex_color(s_gpu.indices.values[i]));
                continue;
            }
            raster_indexed(s_gpu.indices.values[i], s_gpu.indices.values[i+1], s_gpu.indices.values[i+2],
                           vertex_color(s_gpu.indices.values[i]));
        }
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < s_gpu.indices.count; i++)
            raster_indexed(s_gpu.indices.values[0], s_gpu.indices.values[i], s_gpu.indices.values[i+1],
                           vertex_color(s_gpu.indices.values[0]));
        break;
    case NV_PRIM_QUADS:
        /* Independent quads, four vertices each. A batch of eight is two
         * quads, not one six-triangle fan around the first vertex; with
         * exactly four the two agreed, which is why sharing the fan arm
         * looked right. */
        for (i = 0; i + 3 < s_gpu.indices.count; i += 4) {
            raster_indexed(s_gpu.indices.values[i], s_gpu.indices.values[i+1], s_gpu.indices.values[i+2],
                           vertex_color(s_gpu.indices.values[i]));
            raster_indexed(s_gpu.indices.values[i], s_gpu.indices.values[i+2], s_gpu.indices.values[i+3],
                           vertex_color(s_gpu.indices.values[i]));
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        /* Each vertex pair past the first closes another quad against the
         * pair before it. */
        for (i = 0; i + 3 < s_gpu.indices.count; i += 2) {
            raster_indexed(s_gpu.indices.values[i], s_gpu.indices.values[i+1], s_gpu.indices.values[i+3],
                           vertex_color(s_gpu.indices.values[i]));
            raster_indexed(s_gpu.indices.values[i], s_gpu.indices.values[i+3], s_gpu.indices.values[i+2],
                           vertex_color(s_gpu.indices.values[i]));
        }
        break;
    default:
        break;                             /* points and lines: not yet */
    }

    if (s_gpu.tris_drawn && (s_gpu.tris_drawn % 500) == 0)
        fprintf(stderr, "  [GPU] %u triangles rasterised\n", s_gpu.tris_drawn);

    /* Capture the surface while the geometry is still on it.
     *
     * The periodic report dumps too, but a title clears every frame and draws
     * in only some of them, so a report almost always lands on a surface that
     * was wiped a moment ago -- which reads as "nothing was drawn" when the
     * triangles went down correctly just before it. A few frames that actually
     * contain geometry are worth more than any number of clears. */
    if (s_gpu.tris_drawn != before && s_drawn_dumps < FB_DUMP_AFTER_DRAW
        && !s_flip_dumping) {
        s_drawn_dumps++;
        dump_surface_bmp();
    }
}

/* What a batch actually contains. Before anything can be rasterised, the
 * question is what space attribute 0 arrives in: a title running a vertex
 * program hands over object-space positions that mean nothing without running
 * it, while pre-transformed screen-space coordinates can be drawn directly. */
/* RECOMP_FRAME_TRACE=<flag file>: once the file exists, log every batch of
 * the next whole frame (flip to flip) -- where it drew, with what texture,
 * blend and depth state, and how many pixels it actually wrote. "The frame
 * is black" has many causes and this is what tells them apart. */

static void trace_surface_snapshot(const char *phase)
{
    const char *prefix=getenv("RECOMP_FRAME_TRACE_SURFACES");
    if(!prefix || surface_bpp()!=4 || !s_gpu.clip_w || !s_gpu.clip_h)return;
    uint32_t address=dma_resolve(s_gpu.color_offset);
    nv2a_gpu_readback(address);
    if (nv2a_dispatch_failed()) return;
    char path[512];snprintf(path,sizeof(path),"%s-%u-%s.bgra",prefix,s_gpu.draws,phase);
    FILE *file=fopen(path,"wb");if(!file)return;
    uint32_t header[2]={s_gpu.clip_w,s_gpu.clip_h};fwrite(header,sizeof(header),1,file);
    const uint8_t *memory=(const uint8_t *)xbox_GetMemoryOffset()+address;
    for(uint32_t y=0;y<s_gpu.clip_h;y++)
        fwrite(memory+(size_t)(y+s_gpu.clip_y)*s_gpu.pitch+s_gpu.clip_x*4,4,s_gpu.clip_w,file);
    fclose(file);
}

static void frame_trace_flip(void)
{
    static const char *flag = (const char *)-1;
    if (flag == (const char *)-1)
        flag = getenv("RECOMP_FRAME_TRACE");
    if (!flag)
        return;
    if (s_ftrace == 3) {
        /* Deleting the flag file re-arms the trace for the next time it
         * appears, so one run can trace several moments. */
        FILE *f = fopen(flag, "rb");
        if (f)
            fclose(f);
        else
            s_ftrace = 0;
        return;
    }
    if (s_ftrace == 2) {
        s_ftrace = 3;
        fprintf(stderr, "[FTRACE] end of frame\n");
        return;
    }
    if (s_ftrace == 0) {
        FILE *f = fopen(flag, "rb");
        if (!f)
            return;
        fclose(f);
        s_ftrace = 2;
        fprintf(stderr, "[FTRACE] frame begins (flip %u)\n", s_gpu.flips);
    }
}

/* Writes seen per render-target address in the traced frame, so TEXDUMP can re-dump a texture that was drawn into since. */
static uint32_t s_tex_epoch[1024];
#define TEX_EPOCH(off) s_tex_epoch[((off)&0x0FFFFFFFu)>>10&1023]

static void draw_primitive(void)
{
    float v[4];
    uint32_t i;

    if (!s_gpu.prim || !s_gpu.indices.count)
        return;
    s_gpu.draws++;
    static int verbose=-1;
    if(verbose<0)verbose=getenv("RECOMP_PB_EXEC_VERBOSE")!=NULL;
    if (verbose && (s_gpu.draws % 200) == 0)
        fprintf(stderr, "  [GPU] draw #%u\n", s_gpu.draws);
    s_gpu.verts += s_gpu.indices.count;
    int diagnostic_sample=perf_draw_start();
    uint64_t diagnostic_before=diagnostic_sample?nv2a_gpu_cpu_draw_serial():0;
    unsigned long long effect_input=0;int effect_sample=effect_trace_start(&effect_input);
    uint64_t effect_before=effect_sample?nv2a_gpu_cpu_draw_serial():0;

    /* How many batches carry coordinates at all, and what range they span.
     * A pipeline that decodes perfectly and draws nothing is indistinguishable
     * from one that never ran, unless the vertices themselves are measured. */
    {
        float p[4];
        if (fetch_attr(&s_gpu.attr[0], s_gpu.indices.values[0], p)) {
            if (p[0] != 0.0f || p[1] != 0.0f || p[2] != 0.0f) {
                s_gpu.nonzero_draws++;
                if (p[0] < s_gpu.min_x) s_gpu.min_x = p[0];
                if (p[0] > s_gpu.max_x) s_gpu.max_x = p[0];
                if (p[1] < s_gpu.min_y) s_gpu.min_y = p[1];
                if (p[1] > s_gpu.max_y) s_gpu.max_y = p[1];
            }
        }
    }

    {
        uint32_t t0 = s_gpu.tris_drawn;
        uint64_t p0 = s_gpu.pixels;
        double profile_start = perf_clock_ms();
        int trace_shadow=s_ftrace==2 && s_gpu.stencil_test && s_gpu.blend_enable &&
            s_gpu.blend_sfactor==0x306 && s_gpu.blend_dfactor==0x304;
        if(trace_shadow)trace_surface_snapshot("before-shadow");
        raster_batch();
        if(trace_shadow)trace_surface_snapshot("after-shadow");
        if (profile_start) {
            s_perf.draw_ms += perf_clock_ms() - profile_start;
            s_perf.draws++;
            if (s_gpu.zpass_enable) s_perf.zpass_draws++;
        }
        if(diagnostic_sample)perf_draw_finish(diagnostic_before);
        if(effect_sample)effect_trace_finish(effect_before,effect_input);
        if (s_ftrace == 2) {
            TEX_EPOCH(s_gpu.color_offset)++;
            /* One pixel can identify the draw that introduced an artifact,
             * without copying the whole render target after every batch. */
            static int pixel_configured;
            static unsigned pixel_x,pixel_y;
            static int pixel_enabled;
            if(!pixel_configured) {
                const char *point=getenv("RECOMP_FRAME_TRACE_PIXEL");
                pixel_enabled=point && sscanf(point,"%ux%u",&pixel_x,&pixel_y)==2;
                pixel_configured=1;
            }
            if(pixel_enabled) {
                uint32_t argb;
                if(nv2a_gpu_debug_pixel(dma_resolve(s_gpu.color_offset),pixel_x,pixel_y,&argb))
                    fprintf(stderr,"[FTRACE] pixel %u,%u=%08X draw=%u\n",pixel_x,pixel_y,argb,s_gpu.draws);
                else if(trace_cpu_pixel(pixel_x,pixel_y,&argb))
                    fprintf(stderr,"[FTRACE] pixel %u,%u=%08X draw=%u source=cpu\n",
                            pixel_x,pixel_y,argb,s_gpu.draws);
                if(getenv("RECOMP_FRAME_TRACE_DEPTH") && nv2a_gpu_debug_depth(dma_resolve(s_gpu.zeta_offset),pixel_x,pixel_y,&argb))
                    fprintf(stderr,"[FTRACE] depth %u,%u=%08X va=%08X draw=%u\n",pixel_x,pixel_y,argb,s_gpu.zeta_offset,s_gpu.draws);
                else if(getenv("RECOMP_FRAME_TRACE_DEPTH")){
                    float *depth=zbuf_current(0);
                    if(depth && pixel_x<NV_ZBUF_W && pixel_y<NV_ZBUF_H)
                        fprintf(stderr,"[FTRACE] depth %u,%u=%.9g va=%08X draw=%u source=cpu\n",
                                pixel_x,pixel_y,depth[(size_t)pixel_y*NV_ZBUF_W+pixel_x],
                                s_gpu.zeta_offset,s_gpu.draws);
                }
                trace_pixel_coverage(pixel_x,pixel_y);
            }
            /* The hardware path does not execute the CPU vertex reference.
             * Trace the actual current inputs instead of stale s_xf output. */
            if((s_gpu.xf_mode & 3)==2)
                for(uint32_t k=0;k<3 && k<s_gpu.indices.count;k++)
                    transform_vertex(s_gpu.indices.values[k],&s_xf[k]);
            fprintf(stderr, "[FTRACE] %s prim %u n %u surf %08X %ux%u+%u+%u"
                    " pitch %u | tex %s %08X %ux%u fmt %02X | blend %u %X/%X"
                    " eq %X bc %08X | z test %u func %X mask %u | tris %u px %llu\n",
                    (s_gpu.xf_mode & 3) == 2 ? "PRG" : "FIX", s_gpu.prim,
                    s_gpu.indices.count, s_gpu.color_offset, s_gpu.clip_w,
                    s_gpu.clip_h, s_gpu.clip_x, s_gpu.clip_y, s_gpu.pitch,
                    s_gpu.texs[0].valid ? "on" : "off", s_gpu.texs[0].offset,
                    s_gpu.texs[0].width, s_gpu.texs[0].height, s_gpu.texs[0].color,
                    s_gpu.blend_enable, s_gpu.blend_sfactor,
                    s_gpu.blend_dfactor, s_gpu.blend_equation, s_gpu.blend_color,
                    s_gpu.depth_test, s_gpu.depth_func, s_gpu.depth_mask,
                    s_gpu.tris_drawn - t0,
                    (unsigned long long)(s_gpu.pixels - p0));
            if(pixel_enabled)
                fprintf(stderr,"[FTRACE]     raster cull %u/%X/%X alpha %u/%X/ref%u color-mask %08X\n",
                    s_gpu.cull_enable,s_gpu.cull_face,s_gpu.front_face,
                    s_gpu.alpha_test,s_gpu.alpha_func,s_gpu.alpha_ref,s_gpu.color_mask);
            if(pixel_enabled)
                fprintf(stderr,"[FTRACE]     fog enable %u mode %X color %08X coefficients %.9g %.9g\n",
                    s_gpu.fog_enable,s_gpu.fog_mode,s_gpu.fog_color,
                    s_gpu.fog_param[0],s_gpu.fog_param[1]);
            if ((s_gpu.xf_mode & 3) == 2)
                fprintf(stderr, "[FTRACE]     v0 %g %g %g %g  v1 %g %g  v2 %g %g%c",
                        s_xf[0].pos[0], s_xf[0].pos[1], s_xf[0].pos[2],
                        s_xf[0].pos[3], s_xf[1].pos[0], s_xf[1].pos[1],
                        s_xf[2].pos[0], s_xf[2].pos[1], 10);
            if(pixel_enabled && (s_gpu.xf_mode & 3)==2)
                fprintf(stderr,"[FTRACE]     d0 %g %g %g %g tex0 %g %g %g %g\n",
                    s_xf[0].d0[0],s_xf[0].d0[1],s_xf[0].d0[2],s_xf[0].d0[3],
                    s_xf[0].tex[0][0],s_xf[0].tex[0][1],s_xf[0].tex[0][2],s_xf[0].tex[0][3]);
            if (s_gpu.rc_seen) {
                int st;
                fprintf(stderr, "[FTRACE]     rc stages %u ctl %08X prog %05X dot %08X fin %08X %08X",
                        s_gpu.rc.control & 0xFF, s_gpu.rc.control,
                        s_gpu.rc.stage_program, s_gpu.rc.other_stage_input,
                        s_gpu.rc.final0, s_gpu.rc.final1);
                for (st = 0; st < 4; st++)
                    if ((s_gpu.rc.stage_program >> (st * 5)) & 0x1F)
                        fprintf(stderr, " | t%d %08X %ux%u fmt %02X%s", st,
                                s_gpu.texs[st].offset, s_gpu.texs[st].width,
                                s_gpu.texs[st].height, s_gpu.texs[st].color,
                                s_gpu.texs[st].valid ? "" : " (invalid)");
                fputc(10, stderr);
                /* RECOMP_FRAME_TRACE_TEXDUMP=<prefix>[,stage]: the sampler's view of
                 * that stage's texture as int32 w,h + RGBA bytes, once per address.
                 * A render-target texture is read back first. */
                {
                    static const char *dump_prefix=(const char *)-1;static int dump_stage=3;
                    static uint32_t dumped[512],dumped_ep[512];static unsigned dumped_n;
                    if(dump_prefix==(const char *)-1){
                        dump_prefix=getenv("RECOMP_FRAME_TRACE_TEXDUMP");
                        const char *comma=dump_prefix?strrchr(dump_prefix,','):NULL;
                        if(comma)dump_stage=atoi(comma+1);
                    }
                    if(dump_prefix && dump_stage>=0 && dump_stage<4 &&
                       ((s_gpu.rc.stage_program>>(dump_stage*5))&0x1F) && s_gpu.texs[dump_stage].valid &&
                       s_gpu.texs[dump_stage].width<=1024 && s_gpu.texs[dump_stage].height<=1024){
                        const Texture *dt=&s_gpu.texs[dump_stage];unsigned k;
                        for(k=0;k<dumped_n && !(dumped[k]==dt->offset && dumped_ep[k]==TEX_EPOCH(dt->offset));k++);
                        if(k==dumped_n && dumped_n<512){
                            char path[600];size_t plen=strlen(dump_prefix);const char *comma=strrchr(dump_prefix,',');
                            if(comma)plen=(size_t)(comma-dump_prefix);
                            dumped[dumped_n]=dt->offset;dumped_ep[dumped_n++]=TEX_EPOCH(dt->offset);
                            nv2a_gpu_readback(dma_resolve(dt->offset));
                            snprintf(path,sizeof(path),"%.*s-t%d-%08X-d%u.rgba",(int)plen,dump_prefix,dump_stage,dt->offset,s_gpu.draws);
                            FILE *df=nv2a_dispatch_failed()?NULL:fopen(path,"wb");
                            if(df){
                                int32_t hdr[2]={(int32_t)dt->width,(int32_t)dt->height};fwrite(hdr,sizeof(hdr),1,df);
                                for(uint32_t y=0;y<dt->height;y++)for(uint32_t x=0;x<dt->width;x++){
                                    uint32_t argb=0;uint8_t px[4];
                                    if(!sample_tex(dt,0,x,y,&argb))argb=0;
                                    px[0]=(uint8_t)(argb>>16);px[1]=(uint8_t)(argb>>8);px[2]=(uint8_t)argb;px[3]=(uint8_t)(argb>>24);
                                    fwrite(px,4,1,df);
                                }
                                fclose(df);
                                char shown[MAX_PATH*2];fprintf(stderr,"[FTRACE]     texdump %s %ux%u fmt %02X\n",xbox_log_path(path,shown,sizeof shown),dt->width,dt->height,dt->color);
                            }
                        }
                    }
                }
                for (st = 0; st < (int)(s_gpu.rc.control & 0xFF) && st < 8; st++)
                    fprintf(stderr, "[FTRACE]       s%d icw %08X %08X ocw %08X %08X c %08X %08X%c",
                            st, s_gpu.rc.color_icw[st], s_gpu.rc.alpha_icw[st],
                            s_gpu.rc.color_ocw[st], s_gpu.rc.alpha_ocw[st],
                            s_gpu.rc.factor0[st], s_gpu.rc.factor1[st], 10);
            }
            if ((s_gpu.xf_mode & 3) == 2) {
                uint32_t r;
                for (r = 112; r < 116; r++) {
                    const float *c = nv2a_vsh_constant(r);
                    fprintf(stderr, "[FTRACE]     c[%u] %g %g %g %g%c", r,
                            c[0], c[1], c[2], c[3], 10);
                }
            }
        }
    }

    if (verbose) {
        static int shown;
        if (shown++ < 6) {
            fprintf(stderr, "  [GPU] prim %u, %u indices, pos attr:"
                            " off 0x%08X type %u size %u stride %u\n",
                    s_gpu.prim, s_gpu.indices.count, s_gpu.attr[0].offset,
                    s_gpu.attr[0].type, s_gpu.attr[0].size, s_gpu.attr[0].stride);
            /* The texture stage, for either kind of batch. This used to print
             * only for inline batches, which meant a title drawing through
             * vertex arrays -- Half-Life 2's menu, for one -- showed no
             * texture state at all, and the reason a quad sampled flat was
             * invisible. */
            fprintf(stderr, "  [GPU]   tex: off 0x%08X %ux%u pitch %u"
                            " colour 0x%02X swizzled %d valid %d\n",
                    s_gpu.texs[0].offset, s_gpu.texs[0].width, s_gpu.texs[0].height,
                    s_gpu.texs[0].pitch, s_gpu.texs[0].color,
                    d3d8_format_is_swizzled(s_gpu.texs[0].color), s_gpu.texs[0].valid);
            {
                uint32_t k;
                for (k = 0; k < s_gpu.indices.count && k < 3; k++) {
                    float t[2];
                    if (fetch_texcoord(s_gpu.indices.values[k], t))
                        fprintf(stderr, "  [GPU]   uv[%u] = %.3f %.3f\n",
                                k, t[0], t[1]);
                }
            }
            /* An inline batch has no guest buffer to go and look at -- the
             * vertices are the payload -- so print the payload too. */
            if (s_gpu.inline_active) {
                uint32_t k;
                fprintf(stderr, "  [GPU]   inline %u dwords:", s_gpu.inline_count);
                for (k = 0; k < s_gpu.inline_count && k < 16; k++)
                    fprintf(stderr, " %08X", s_gpu.inline_buf[k]);
                fprintf(stderr, "\n");
                for (k = 0; k < s_gpu.indices.count && k < 4; k++) {
                    float t[2];
                    if (fetch_texcoord(s_gpu.indices.values[k], t))
                        fprintf(stderr, "  [GPU]   uv[%u] = %.3f %.3f\n",
                                k, t[0], t[1]);
                }
                fprintf(stderr, "  [GPU]   tex: off 0x%08X %ux%u pitch %u"
                                " colour 0x%02X valid %d\n",
                        s_gpu.texs[0].offset, s_gpu.texs[0].width, s_gpu.texs[0].height,
                        s_gpu.texs[0].pitch, s_gpu.texs[0].color, s_gpu.texs[0].valid);
            }
            {
                /* Every attribute the batch has, not just position. If the
                 * other streams carry data and position does not, the problem
                 * is one buffer rather than the whole vertex path. */
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t a, k;
                for (a = 0; a < NV_VERTEX_ATTRS; a++) {
                    const VertexAttr *at = &s_gpu.attr[a];
                    uint32_t nz = 0;
                    if (!at->offset || !at->size)
                        continue;
                    for (k = 0; k < 64; k++)
                        if (mem[at->offset + k]) nz++;
                    fprintf(stderr, "  [GPU]   attr%-2u off 0x%08X type %u"
                                    " size %u stride %-3u  %u/64 bytes set\n",
                            a, at->offset, at->type, at->size, at->stride, nz);
                }
            }
            {
                /* Raw bytes at the array, in case the values read as zero:
                 * that looks the same whether the offset is wrong or the
                 * buffer genuinely has not been filled yet. */
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t k;
                fprintf(stderr, "  [GPU]   bytes @0x%08X:", s_gpu.attr[0].offset);
                for (k = 0; k < 32; k++)
                    fprintf(stderr, " %02X", mem[s_gpu.attr[0].offset + k]);
                fprintf(stderr, "\n");
                fprintf(stderr, "  [GPU]   indices:");
                for (k = 0; k < s_gpu.indices.count && k < 8; k++)
                    fprintf(stderr, " %u", s_gpu.indices.values[k]);
                fprintf(stderr, "\n");
            }
            for (i = 0; i < s_gpu.indices.count && i < 3; i++) {
                if (fetch_attr(&s_gpu.attr[0], s_gpu.indices.values[i], v))
                    fprintf(stderr, "  [GPU]   v[%u] = %.3f %.3f %.3f %.3f\n",
                            s_gpu.indices.values[i], v[0], v[1], v[2], v[3]);
            }
        }
    }
}

/* Draw the vertices the title wrote straight into the pushbuffer.
 *
 * INLINE_ARRAY carries no offsets and no indices: the dwords between BEGIN and
 * END *are* the vertex buffer, packed in attribute order using the same
 * SET_VERTEX_DATA_ARRAY_FORMAT registers an ordinary array would use. So the
 * whole batch is describable as a vertex array whose base happens to be that
 * payload, which means synthesising the layout and handing it to the existing
 * path -- rather than a second copy of the topology and rasterisation code.
 *
 * The title's own attribute table is saved and put back: these offsets and
 * strides are ours, and it has not stopped using its.
 *
 * Inline attributes align to their component width. In particular, particle
 * corners use S32K2 (two 16-bit values), followed by three floats. Treating
 * those shorts as floats shifted the next attribute and lost quad vertices.
 */
/* The stream owns the same index storage used by both rendering paths.
 * Temporarily expose this chunk's topology/count (line loops flush as strips)
 * without changing the original BEGIN_END state for subsequent FIFO methods. */
static void draw_index_chunk(void *context, uint32_t primitive,
                             const uint32_t *values, uint32_t count)
{
    (void)context; (void)values;
    uint32_t saved_primitive = s_gpu.prim, saved_count = s_gpu.indices.count;
    s_gpu.prim = primitive;
    s_gpu.indices.count = count;
    draw_primitive();
    s_gpu.indices.count = saved_count;
    s_gpu.prim = saved_primitive;
}

static void draw_inline_array(void)
{
    VertexAttr saved[NV_VERTEX_ATTRS];
    uint32_t off = 0, a, i, vsize, count;

    memcpy(saved, s_gpu.attr, sizeof saved);

    for (a = 0; a < NV_VERTEX_ATTRS; a++) {
        if (!s_gpu.attr[a].size)
            continue;
        if(!nv2a_vertex_component_bytes(s_gpu.attr[a].type))goto out;
        off=nv2a_vertex_inline_attribute(off,s_gpu.attr[a].type,s_gpu.attr[a].size,
                                       &s_gpu.attr[a].offset);
    }
    vsize = off;
    if (!vsize)
        goto out;

    count = (s_gpu.inline_count * 4) / vsize;
    static int verify=-1;
    static unsigned shown;
    if(verify<0)verify=getenv("RECOMP_INLINE_VERIFY")!=NULL;
    if(verify && shown<8 && ((s_gpu.inline_count*4)%vsize || (s_gpu.prim==8 && count%4))) {
        shown++;
        fprintf(stderr,"[INLINE-VERIFY] primitive=%u dwords=%u stride=%u vertices=%u\n",s_gpu.prim,s_gpu.inline_count,vsize,count);
        for(a=0;a<NV_VERTEX_ATTRS;a++)if(s_gpu.attr[a].size)
            fprintf(stderr,"[INLINE-VERIFY] attribute=%u type=%u size=%u offset=%u\n",a,s_gpu.attr[a].type,s_gpu.attr[a].size,s_gpu.attr[a].offset);
        for(i=0;i<s_gpu.inline_count && i<64;i++)fprintf(stderr,"[INLINE-VERIFY] word=%u bits=%08X\n",i,s_gpu.inline_buf[i]);
    }
    if (count < 3 || count > NV_MAX_INDICES)
        goto out;
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].size)
            s_gpu.attr[a].stride = vsize;

    for (i = 0; i < count; i++)
        s_gpu.indices.values[i] = (uint16_t)i;
    s_gpu.indices.count = count;

    s_gpu.inline_active = 1;
    draw_primitive();
    s_gpu.inline_active = 0;

out:
    memcpy(s_gpu.attr, saved, sizeof saved);
    s_gpu.indices.count = 0;
}

/* Draw the vertices SET_VERTEX3F/4F completed.
 *
 * Same trick as draw_inline_array: rather than a second copy of the topology
 * and rasterisation code, describe what was accumulated as an ordinary vertex
 * array and hand it to the existing path. The layout is ours and fixed, so
 * the attribute table is written out here rather than derived from the
 * title's format registers.
 *
 * The title's own table is saved and put back -- it has not stopped using it.
 *
 * ponytail: position, diffuse and texcoord0 only. That is what a 2D quad
 * carries and what this rasteriser samples; a second texcoord set or a normal
 * would need the D3D11 translator, not more slots here.
 */
static void draw_immediate(void)
{
    VertexAttr saved[NV_VERTEX_ATTRS];
    uint32_t i;

    if (s_gpu.imm_count < 3)
        return;

    memcpy(saved, s_gpu.attr, sizeof saved);
    memset(s_gpu.attr, 0, sizeof s_gpu.attr);
    /* Offsets are byte offsets into inline_buf here, not guest addresses --
     * fetch_attr reads them that way while inline_active is set, which is
     * also why 0 is a legal offset for position. Every attribute is a float4;
     * the ones the batch never set carry their standing values, as on the
     * GPU. */
    for (i = 0; i < NV_VERTEX_ATTRS; i++) {
        s_gpu.attr[i].type = 2;
        s_gpu.attr[i].size = 4;
        s_gpu.attr[i].offset = i * 16;
        s_gpu.attr[i].stride = IMM_VERTEX_DWORDS * 4;
    }

    for (i = 0; i < s_gpu.imm_count && i < NV_MAX_INDICES; i++)
        s_gpu.indices.values[i] = (uint16_t)i;
    s_gpu.indices.count = i;

    /* fetch_attr bounds-checks against inline_count dwords. */
    s_gpu.inline_count = s_gpu.imm_count * IMM_VERTEX_DWORDS;
    s_gpu.inline_active = 1;
    draw_primitive();
    s_gpu.inline_active = 0;

    memcpy(s_gpu.attr, saved, sizeof saved);
    s_gpu.indices.count = 0;
    s_gpu.inline_count = 0;
}

/* A vertex is complete: append all 16 attributes in the layout
 * draw_immediate describes. */
static void imm_emit_vertex(void)
{
    uint32_t at = s_gpu.imm_count * IMM_VERTEX_DWORDS;

    if (!s_gpu.prim || at + IMM_VERTEX_DWORDS > NV_MAX_INLINE)
        return;
    memcpy(&s_gpu.inline_buf[at], s_gpu.imm_attr, sizeof s_gpu.imm_attr);
    s_gpu.imm_count++;
}

/* The immediate-mode writes. Returns 1 if `method` was one of them.
 *
 * Split out because it is a range test against five separate bases, and that
 * reads better than five more cases in an already long switch.
 */
/* The immediate-mode attribute methods (xemu pgraph.c SET_VERTEX_DATA*).
 * Each sets an attribute's current value; completing attribute 0 -- the
 * position -- emits a vertex carrying every attribute as it stands. This used
 * to keep position, diffuse and texcoord 0 only, which dropped any quad with
 * a second texture coordinate: Burnout 3 composites its whole 3D scene into
 * the frame with exactly such a quad, and the frame never got the scene. */
static int imm_vertex_method(uint32_t method, uint32_t param)
{
    union { uint32_t u; float f; } v;
    uint32_t attr, c;
    float *a;
    v.u = param;

    if (method >= NV097_SET_VERTEX4F && method < NV097_SET_VERTEX4F + 16) {
        c = (method - NV097_SET_VERTEX4F) / 4;
        s_gpu.imm_attr[0][c] = v.f;
        s_gpu.imm_used |= 1;
        if (c == 3)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX3F && method < NV097_SET_VERTEX3F + 12) {
        c = (method - NV097_SET_VERTEX3F) / 4;
        s_gpu.imm_attr[0][c] = v.f;
        s_gpu.imm_used |= 1;
        if (c == 2) {
            s_gpu.imm_attr[0][3] = 1.0f;
            imm_emit_vertex();
        }
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA2F_M
            && method < NV097_SET_VERTEX_DATA2F_M + NV_VERTEX_ATTRS * 8) {
        attr = (method - NV097_SET_VERTEX_DATA2F_M) / 8;
        c = ((method - NV097_SET_VERTEX_DATA2F_M) % 8) / 4;
        a = s_gpu.imm_attr[attr];
        a[c] = v.f;
        if (c == 1) {
            a[2] = 0.0f; a[3] = 1.0f;
        }
        s_gpu.imm_used |= (uint16_t)(1u << attr);
        if (attr == 0 && c == 1)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4F_M
            && method < NV097_SET_VERTEX_DATA4F_M + NV_VERTEX_ATTRS * 16) {
        attr = (method - NV097_SET_VERTEX_DATA4F_M) / 16;
        c = ((method - NV097_SET_VERTEX_DATA4F_M) % 16) / 4;
        s_gpu.imm_attr[attr][c] = v.f;
        s_gpu.imm_used |= (uint16_t)(1u << attr);
        if (attr == 0 && c == 3)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA2S
            && method < NV097_SET_VERTEX_DATA2S + NV_VERTEX_ATTRS * 4) {
        attr = (method - NV097_SET_VERTEX_DATA2S) / 4;
        a = s_gpu.imm_attr[attr];
        a[0] = (float)(int16_t)(param & 0xFFFF);
        a[1] = (float)(int16_t)(param >> 16);
        a[2] = 0.0f; a[3] = 1.0f;
        s_gpu.imm_used |= (uint16_t)(1u << attr);
        if (attr == 0)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4UB
            && method < NV097_SET_VERTEX_DATA4UB + NV_VERTEX_ATTRS * 4) {
        /* Bytes in register order, x from the low byte (xemu). */
        attr = (method - NV097_SET_VERTEX_DATA4UB) / 4;
        a = s_gpu.imm_attr[attr];
        a[0] = (float)( param        & 0xFF) / 255.0f;
        a[1] = (float)((param >>  8) & 0xFF) / 255.0f;
        a[2] = (float)((param >> 16) & 0xFF) / 255.0f;
        a[3] = (float)( param >> 24        ) / 255.0f;
        s_gpu.imm_used |= (uint16_t)(1u << attr);
        if (attr == 0)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4S_M
            && method < NV097_SET_VERTEX_DATA4S_M + NV_VERTEX_ATTRS * 8) {
        attr = (method - NV097_SET_VERTEX_DATA4S_M) / 8;
        c = ((method - NV097_SET_VERTEX_DATA4S_M) % 8) / 4;
        a = s_gpu.imm_attr[attr];
        a[c * 2]     = (float)(int16_t)(param & 0xFFFF);
        a[c * 2 + 1] = (float)(int16_t)(param >> 16);
        s_gpu.imm_used |= (uint16_t)(1u << attr);
        if (attr == 0 && c == 1)
            imm_emit_vertex();
        return 1;
    }
    return 0;
}
/* The texture stage registers, all four stages. Stage i's registers are
 * 0x1B00 + 0x40*i onwards; only stage 0 used to be decoded, which was
 * enough for menus drawn one texture at a time and not for a 3D frame that
 * composites its scene through a second stage. */
static void tex_stage_method(uint32_t method, uint32_t param)
{
    uint32_t stage = (method - NV_TEX_FIRST) / 0x40;
    uint32_t reg = NV_TEX_FIRST + ((method - NV_TEX_FIRST) & 0x3F);
    Texture *t = &s_gpu.texs[stage];

    switch (reg) {
    case NV097_SET_TEXTURE_OFFSET:
        /* A DMA-object offset like a surface's: physical, reached through
         * the contiguous window when it names contiguous memory. */
        t->offset = dma_resolve(param);
        break;
    case NV097_SET_TEXTURE_FORMAT:
        t->color = (param >> 8) & 0xFF;
        t->cube = (param >> 2) & 1;
        t->border_source_color = (param >> 3) & 1;
        t->dimensionality = (param >> 4) & 15;
        t->depth = t->dimensionality==3 ? 1u<<((param>>28)&15) : 1;
        t->levels = (param >> 16) & 0xF;
        /* A swizzled texture carries its own dimensions here, as log2 in
         * BASE_SIZE_U/V. It has to: IMAGE_RECT describes a linear image, and
         * a title that only uses swizzled textures never sends one. */
        if (tex_size_from_format(t->color)) {
            t->width  = 1u << ((param >> 20) & 0xF);
            t->height = 1u << ((param >> 24) & 0xF);
        }
        break;
    case NV097_SET_TEXTURE_PALETTE:
        /* The low six bits carry the DMA context and the entry count. */
        t->palette = dma_resolve(param & ~0x3Fu);
        break;
    case NV097_SET_TEXTURE_ADDRESS:
        t->addr_u =  param        & 0xF;
        t->addr_v = (param >>  8) & 0xF;
        t->addr_w = (param >> 16) & 0xF;
        break;
    case 0x1B0C:                                  /* SET_TEXTURE_CONTROL0 */
        t->control0=param;
        break;
    case 0x1B24:                                  /* SET_TEXTURE_BORDER_COLOR */
        t->border_color=param;
        break;
    case NV097_SET_TEXTURE_CONTROL1:
        t->pitch = param >> 16;          /* linear formats only */
        break;
    case NV097_SET_TEXTURE_IMAGE_RECT:
        t->width  = param >> 16;
        t->height = param & 0xFFFF;
        break;
    case 0x1B14:                                  /* SET_TEXTURE_FILTER */
        t->filter = param;
        break;
    case 0x1B28: case 0x1B2C: case 0x1B30: case 0x1B34:
        if(stage) {
            /* NV097 supplies 00,01,11,10, unlike the matrix register order. */
            static const unsigned order[4]={0,1,3,2};
            memcpy(&s_gpu.bump_matrix[stage][order[(reg-0x1B28)/4]],&param,4);
        }
        break;
    case 0x1B38: if(stage)memcpy(&s_gpu.bump_scale[stage],&param,4); break;
    case 0x1B3C: if(stage)memcpy(&s_gpu.bump_offset[stage],&param,4); break;
    default:
        break;
    }
    tex_update_valid(t);
    if (stage == 0)
        record_tex_reg(method, param);
}

static uint32_t s_object_classes[8];
static int s_exec_inited;   /* the 3D state defaults below were applied (first method) */

/* Only pure program/constant uploads use this entry point. PFIFO captures
 * the payload before advancing GET, then preserves the upload cursors here. */
/* T2-0 R0 executor counters (RECOMP_PERF), defined in xbox_memory_layout.c. Words per method class:
 * 0 idx16 1 idx32 2 draw_arrays 3 inline_array 4 imm_vertex(0x1500-0x17FF,0x1880-0x1AFF) 5 tex
 * 6 constants/program (bulk + non-bulk) 7 semaphore/sw 8 other 3D state 9 non-3D subchannel. */
extern int g_nv2a_prof;
extern uint64_t g_nv2a_prof_words[10];
extern int64_t nv2a_prof_ticks(void);
extern void nv2a_prof_acc(int slot, int64_t start);
extern void nv2a_prof_flip(void);
static __inline int nv2a_prof_word_class(uint32_t subch, uint32_t method)
{
    if (subch) return 9;
    if (method >= 0x1800 && method < 0x1808) return 0;
    if (method >= 0x1808 && method < 0x1810) return 1;
    if (method == 0x1810) return 2;
    if (method >= 0x1818 && method < 0x181C) return 3;
    if ((method >= 0x1500 && method < 0x1800) || (method >= 0x1880 && method < 0x1B00)) return 4;
    if (method >= NV_TEX_FIRST && method <= NV_TEX_LAST) return 5;
    if (method >= 0x0B00 && method < 0x0C00) return 6;
    if (method == 0x1A4 || method == 0x100 || method == 0x1D6C || method == 0x1D70) return 7;
    return 8;
}

void nv2a_pb_exec_upload(uint32_t method, uint32_t step, const uint32_t *words, uint32_t count)
{
    (void)step;
    if(s_object_classes[0]==0x44)return;
    if(g_nv2a_prof>0)g_nv2a_prof_words[6]+=count;
    int64_t pt=nv2a_prof_ticks();
    if(method<0x0B80)nv2a_vsh_program_words(words,count);
    else nv2a_vsh_constant_words(words,count);
    nv2a_prof_acc(1 /* upload */,pt);
}

/* T2-1 E1: bulk pushbuffer words. nv2a_pb_consume (nv2a_pb_scan.c) hands whole non-incrementing
 * subchannel-0 payloads of ARRAY_ELEMENT16 (0x1800), ARRAY_ELEMENT32 (0x1808) and INLINE_ARRAY
 * (0x1818) here instead of one nv2a_pb_exec_method call per word. Exact by construction: per word,
 * the same guards the per-word path applies (s_gpu.prim, then the stream's primitive), the same
 * appends in the same order through the same nv2a_index_stream_append, and so the same flush
 * points and draw_index_chunk calls. Returns 0 (the caller falls back to the per-word path) for a
 * 0x44 object on subchannel 0, a traced frame, RECOMP_PB_EXEC_VERBOSE, or before the first method
 * initialised the 3D state. RECOMP_PB_BULK_VERIFY=N: 1 in N index packets is also decoded by the
 * per-word reference into a scratch stream with a recording callback, and by the bulk loop into a
 * second scratch stream; streams and flush records must match ([PB-BULK] mismatches=). */
static void pb_bulk_index(Nv2aIndexStream *st, uint32_t method, const uint32_t *w, uint32_t n,
                          Nv2aIndexDraw draw, void *context)
{
    if (method == 0x1800) {
        for (uint32_t i = 0; i < n; i++) {
            if (!s_gpu.prim || !st->primitive) continue;
            uint32_t v = w[i];
            nv2a_index_stream_append(st, v & 0xFFFFu, draw, context);
            nv2a_index_stream_append(st, v >> 16, draw, context);
        }
    } else {
        for (uint32_t i = 0; i < n; i++) {
            if (!s_gpu.prim || !st->primitive) continue;
            nv2a_index_stream_append(st, w[i], draw, context);
        }
    }
}

typedef struct { uint32_t n, prim[64], count[64], hash[64]; } PbBulkRecord;
static void pb_bulk_record(void *context, uint32_t primitive, const uint32_t *values, uint32_t count)
{
    PbBulkRecord *r = (PbBulkRecord *)context;
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < count; i++) h = (h ^ values[i]) * 16777619u;
    if (r->n < 64) { r->prim[r->n] = primitive; r->count[r->n] = count; r->hash[r->n] = h; }
    r->n++;
}

int nv2a_pb_exec_bulk(uint32_t method, const uint32_t *words, uint32_t count)
{
    static int verbose = -1, verify = -1;
    static unsigned long long packets, total, verified, mismatches, tick;
    static DWORD report_ms;
    if (verbose < 0) {
        const char *e = getenv("RECOMP_PB_BULK_VERIFY");
        verbose = getenv("RECOMP_PB_EXEC_VERBOSE") != NULL;
        verify = e ? atoi(e) : 0;
        if (verify < 0) verify = 0;
    }
    if (verbose || s_ftrace == 2 || !s_exec_inited || s_object_classes[0] == 0x44) return 0;
    if (g_nv2a_prof > 0) g_nv2a_prof_words[method == 0x1800 ? 0 : method == 0x1808 ? 1 : 3] += count;
    packets++; total += count;
    if (method == 0x1818) {
        if (s_gpu.prim) {
            uint32_t room = NV_MAX_INLINE - s_gpu.inline_count, n = count < room ? count : room;
            memcpy(&s_gpu.inline_buf[s_gpu.inline_count], words, n * sizeof(uint32_t));
            s_gpu.inline_count += n;
        }
    } else if (verify && ++tick % (unsigned)verify == 0) {
        static Nv2aIndexStream ref, bulk;
        static PbBulkRecord ra, rb;
        ref = s_gpu.indices; bulk = s_gpu.indices;
        memset(&ra, 0, sizeof ra); memset(&rb, 0, sizeof rb);
        for (uint32_t i = 0; i < count; i++)            /* the per-word path, as nv2a_pb_exec_method runs it */
            if (s_gpu.prim) nv2a_index_stream_method(&ref, method, words[i], pb_bulk_record, &ra);
        pb_bulk_index(&bulk, method, words, count, pb_bulk_record, &rb);
        pb_bulk_index(&s_gpu.indices, method, words, count, draw_index_chunk, NULL);
        verified++;
        int bad = memcmp(&ref, &bulk, sizeof ref) != 0 || memcmp(&ra, &rb, sizeof ra) != 0 ||
                  memcmp(&ref, &s_gpu.indices, sizeof ref) != 0;
        if (bad && mismatches++ < 8)
            fprintf(stderr, "[PB-BULK] MISMATCH method=%04X words=%u count %u/%u/%u total %llu/%llu flushes %u/%u\n",
                    method, count, ref.count, bulk.count, s_gpu.indices.count, (unsigned long long)ref.total,
                    (unsigned long long)bulk.total, ra.n, rb.n);
    } else {
        pb_bulk_index(&s_gpu.indices, method, words, count, draw_index_chunk, NULL);
    }
    if (verify || g_nv2a_prof > 0) {
        DWORD now = GetTickCount();
        if (!report_ms) report_ms = now;
        else if (now - report_ms >= 30000) {
            report_ms = now;
            fprintf(stderr, "[PB-BULK] packets=%llu words=%llu verified=%llu mismatches=%llu\n",
                    packets, total, verified, mismatches);
        }
    }
    return 1;
}

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param)
{
    static uint32_t semaphore_handle, semaphore_va;
    if (subch >= 8) return;
    if (g_nv2a_prof > 0) g_nv2a_prof_words[nv2a_prof_word_class(subch, method)]++;
    if (!method) {
        s_object_classes[subch] = nv2a_pb_object_class(param);
        return;
    }
    if (s_object_classes[subch] == 0x44) {
        if (method == 0x310) nv2a_pb_pattern_color(param);
        return;
    }
    if (subch == 0 && method == 0x1A4) { semaphore_handle = param; return; }
    if (subch == 0 && method == 0x100) { nv2a_pb_software_method(param); return; }
    if (subch == 0 && method == 0x1D6C) {
        if (!nv2a_pb_dma_address(semaphore_handle, param, &semaphore_va)) {
            semaphore_va = 0;
            static unsigned warned;
            if (warned++ < 4) fprintf(stderr,"[PB] unresolved semaphore DMA %08X + %08X\n",semaphore_handle,param);
        }
        return;
    }
    if (subch == 0 && method == 0x1D70) {
        /* Vertex/index/texture inputs have been captured by the native draw
         * submissions. Only now may the guest reuse that storage. */
        if (semaphore_va) {
            volatile uint32_t *p = (volatile uint32_t *)(xbox_GetMemoryOffset() + (uintptr_t)semaphore_va);
            nv2a_gpu_publish_word(p,param);
        }
        return;
    }
    if (!s_exec_inited) {
        s_exec_inited = 1;
        s_gpu.color_mask = 0x01010101u;           /* all channels, as reset */
        {
            int i;
            for (i = 0; i < NV_VERTEX_ATTRS; i++)
                s_gpu.imm_attr[i][3] = 1.0f;
        }
        s_gpu.min_x = s_gpu.min_y = 1e30f;
        s_gpu.max_x = s_gpu.max_y = -1e30f;
    }
    /* Bring-up: the first parameters each surface method carries. A wrong
     * pitch or clip is indistinguishable from a method never arriving unless
     * the values are visible. Cached: this runs for every method, and an
     * uncached getenv here was a quarter of the executor's time. */
    static int verbose = -1;
    if (verbose < 0)
        verbose = getenv("RECOMP_PB_EXEC_VERBOSE") != NULL;
    if (verbose) {
        static int shown[8];
        int slot = -1;
        switch (method) {
        case NV097_SET_SURFACE_CLIP_HORIZONTAL: slot = 0; break;
        case NV097_SET_SURFACE_CLIP_VERTICAL:   slot = 1; break;
        case NV097_SET_SURFACE_FORMAT:          slot = 2; break;
        case NV097_SET_SURFACE_PITCH:           slot = 3; break;
        case NV097_SET_SURFACE_COLOR_OFFSET:    slot = 4; break;
        case NV097_SET_COLOR_CLEAR_VALUE:       slot = 5; break;
        case NV097_CLEAR_SURFACE:               slot = 6; break;
        default: break;
        }
        if (slot >= 0 && shown[slot]++ < 4)
            fprintf(stderr, "  [GPU] subch %u method 0x%04X param 0x%08X\n",
                    subch, method, param);
    }

    if (s_ftrace == 2) {
        /* RECOMP_FRAME_TRACE_METHODS: every method of the traced frame,
         * less the bulk (vertex data, program and constant uploads). */
        static int all = -1;
        if (all < 0)
            all = getenv("RECOMP_FRAME_TRACE_METHODS") != NULL;
        if (all && (method == NV097_ARRAY_ELEMENT32 || !(method >= 0x1800 && method < 0x1A00))
            && !(method >= 0x0B00 && method < 0x0C00))
            fprintf(stderr, "[FTRACE]   m %u:%04X = %08X%c", subch, method,
                    param, 10);
    }
    if (subch == 0 && method >= NV_TEX_FIRST && method <= NV_TEX_LAST) {
        tex_stage_method(method, param);
        return;
    }
    /* Observe the raw command only. It still reaches the original unhandled
     * route; this does not implement point sizing or mutate guest state. */
    if(method==0x043C && nv2a_gpu_cpu_enabled()) {
        s_draw_perf.point_writes++;s_draw_perf.point_valid=1;
        s_draw_perf.point_raw=param;s_draw_perf.point_subch=subch;
        s_draw_perf.point_class=s_object_classes[subch];
    }
    if (subch != 0) {                      /* 3D class lives on subchannel 0 */
        note_unhandled(method, param);
        return;
    }
    switch (method) {
    case NV097_SET_SURFACE_CLIP_HORIZONTAL:
        s_gpu.clip_x = param & 0xFFFF;
        s_gpu.clip_w = (param >> 16) & 0xFFFF;
        break;
    case NV097_SET_SURFACE_CLIP_VERTICAL:
        s_gpu.clip_y = param & 0xFFFF;
        s_gpu.clip_h = (param >> 16) & 0xFFFF;
        break;
    case NV097_SET_SURFACE_FORMAT:
        s_gpu.format = param;
        break;
    case NV097_SET_SURFACE_PITCH:
        s_gpu.pitch = param & 0xFFFF;      /* colour pitch; zeta is the top half */
        s_gpu.zeta_pitch = param >> 16;
        break;
    case NV097_SET_SURFACE_COLOR_OFFSET:
        if (s_ftrace == 2)
            fprintf(stderr, "[FTRACE] color offset -> %08X%c", param, 10);
        s_gpu.color_offset = param;
        break;
    case NV097_SET_COLOR_CLEAR_VALUE:
        s_gpu.clear_color = param;
        break;
    case NV097_SET_BLEND_ENABLE:
        s_gpu.blend_enable = param;
        break;
    case NV097_SET_BLEND_EQUATION:
        s_gpu.blend_equation = param;
        break;
    case 0x0358:                                  /* SET_COLOR_MASK */
        s_gpu.color_mask = param;
        break;

    /* Register combiners. */
    case 0x0288: s_gpu.rc.final0 = param; break;  /* SPECULAR_FOG_CW0 */
    case 0x028C: s_gpu.rc.final1 = param; break;  /* SPECULAR_FOG_CW1 */
    case 0x1E20: s_gpu.rc.final_c0 = param; break;/* SPECULAR_FOG_FACTOR */
    case 0x1E24: s_gpu.rc.final_c1 = param; break;
    case 0x1E60:                                  /* COMBINER_CONTROL */
        s_gpu.rc.control = param;
        s_gpu.rc_seen = 1;
        break;
    case 0x1E70: s_gpu.rc.stage_program = param; break;
    case 0x1E74:
        s_gpu.rc.other_stage_input=(s_gpu.rc.other_stage_input&~0xFFFu)|(param&0xFFFu);
        break;
    case 0x1E78:
        s_gpu.rc.other_stage_input=(s_gpu.rc.other_stage_input&~0xFFFF000u)|(param&0xFFFF000u);
        break;
    case 0x181C: case 0x1820: case 0x1824:
        memcpy(&s_gpu.eye_vector[(method-0x181C)/4],&param,4); break;
    case 0x17F8: s_gpu.clip_plane_mode = param; break;
    case 0x0290: s_gpu.control0=param;break; /* CONTROL0, retained for frame diagnostics */
    case 0x0394: memcpy(&s_gpu.clip_min,&param,4);break;
    case 0x0398: memcpy(&s_gpu.clip_max,&param,4);break;
    case 0x1D78: s_gpu.z_clamp=((param>>4)&15)==1;break;

    /* Alpha test and fog. */
    case 0x0300: s_gpu.alpha_test = param; break;
    case 0x0308: s_gpu.cull_enable=param; break;
    case 0x039C: s_gpu.cull_face=param; break;
    case 0x03A0: s_gpu.front_face=param; break;
    case 0x0338: s_gpu.polygon_offset_enable=param; break;
    case 0x0384: memcpy(&s_gpu.polygon_offset_scale,&param,4); break;
    case 0x0388: memcpy(&s_gpu.polygon_offset_bias,&param,4); break;
    case 0x033C: s_gpu.alpha_func = param; break;
    case 0x0340: s_gpu.alpha_ref = param; break;
    case 0x02A4: s_gpu.fog_enable = param; break;
    case 0x02A8: s_gpu.fog_color = param; break;  /* R in bits 0-7 */
    case 0x029C: s_gpu.fog_mode = param; break;
    case 0x09C0: memcpy(&s_gpu.fog_param[0], &param, 4); break;
    case 0x09C4: memcpy(&s_gpu.fog_param[1], &param, 4); break;
    case 0x17C8:                                  /* CLEAR_REPORT_VALUE */
        s_gpu.zpass_count = 0;
#if defined(__APPLE__)
        nv2a_dispatch_visibility_reset();
#endif
        s_perf.report_clears++;
        break;
    case 0x17CC:                                  /* SET_ZPASS_PIXEL_COUNT_ENABLE */
        if (s_gpu.zpass_enable != param) s_perf.zpass_toggles++;
        s_gpu.zpass_enable = param;
        break;
    case 0x17D0: {                                /* GET_REPORT */
        /* The GPU's answer to a visibility test: 16 bytes at the report
         * DMA offset -- a timestamp, the pixel count, and 0 for "done".
         * Never written, a title polling it (D3D's GetVisibilityTestResult
         * starts the slot at 0xFFFFFFFF) sees every test incomplete. Burnout
         * 3 culls its world with these, so the race's main view drew only
         * the sky while the cube-map passes, which do not test, drew it all.
         * The report DMA object starts at physical 0 here, as D3D sets it. */
        uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
        uint32_t at = dma_resolve(param & 0x00FFFFFFu);
        uint64_t stamp = (uint64_t)s_gpu.reports++ * 1000u;
        uint32_t passed = s_gpu.zpass_count;
#if defined(__APPLE__)
        /* Samples counted by a registered backend's hardware draws. */
        passed += nv2a_dispatch_visibility_count();
#endif
        s_perf.report_gets++;
        if (passed) s_perf.report_nonzero++;
        memcpy(mem + at, &stamp, 8);
        memcpy(mem + at + 8, &passed, 4);
        memset(mem + at + 12, 0, 4);
        break;
    }
    case NV097_SET_BLEND_COLOR:
        s_gpu.blend_color = param;
        break;
    case NV097_SET_BLEND_FUNC_SFACTOR:
        s_gpu.blend_sfactor = param;
        break;
    case NV097_SET_BLEND_FUNC_DFACTOR:
        s_gpu.blend_dfactor = param;
        break;
    case NV097_CLEAR_SURFACE: {
        if (s_ftrace == 2)
            fprintf(stderr, "[FTRACE] clear %X surf %08X %ux%u colour %08X depth %.9g format %X zeta %08X%c",
                    param, s_gpu.color_offset, s_gpu.clip_w, s_gpu.clip_h,
                    s_gpu.clear_color, zclear_value(),s_gpu.format,s_gpu.zeta_offset,10);
        int64_t pt = nv2a_prof_ticks();
        int handled = clear_surface(param);
        nv2a_prof_acc(3 /* clear */, pt);
#if defined(__APPLE__)
        if ((param & 3u) && nv2a_dispatch_registered() && !handled && !nv2a_gpu_discard_frame()) {
            int mirrored = registered_depth_clear(param);
            if (mirrored < 0) nv2a_dispatch_report_failure("software depth clear");
            else if (!mirrored && (param & 1u)) zbuf_clear();
        }
#else
        (void)handled;
#endif
        if ((param & 0x01) && !nv2a_gpu_enabled() && !nv2a_gpu_discard_frame())  /* Z */
            zbuf_clear();
        break;
    }

    case NV097_SET_BEGIN_END:
        if (param) {
            s_gpu.prim = param;
            nv2a_index_stream_begin(&s_gpu.indices, param);
            s_gpu.inline_count = 0;
            s_gpu.imm_count = 0;
        } else {
            /* Three ways a batch can have arrived, and only one is in use at
             * a time: vertices completed by SET_VERTEX4F, a payload written
             * with INLINE_ARRAY, or indices into the title's own arrays. */
            int64_t prof_t = nv2a_prof_ticks();
            if (s_gpu.imm_count)
                draw_immediate();
            else if (s_gpu.inline_count)
                draw_inline_array();
            else {
                nv2a_index_stream_end(&s_gpu.indices, draw_index_chunk, NULL);
                static int diagnose = -1;
                static unsigned reported;
                if (diagnose < 0) diagnose = getenv("RECOMP_PRIMITIVE_DIAG") != NULL;
                if (diagnose && s_gpu.indices.total > NV_MAX_INDICES && (s_ftrace == 2 || reported++ < 32))
                    fprintf(stderr, "[INDEX-STREAM] primitive=%u indices=%llu batches=%u texture=%08X color-mask=%08X stencil=%u\n",
                        s_gpu.prim, (unsigned long long)s_gpu.indices.total, s_gpu.indices.batches,
                        s_gpu.texs[0].offset, s_gpu.color_mask, s_gpu.stencil_test);
            }
            nv2a_prof_acc(2 /* draw */, prof_t);
            s_gpu.prim = 0;
            s_gpu.inline_count = 0;
            s_gpu.imm_count = 0;
        }
        break;

    case NV097_INLINE_ARRAY:
        /* Vertex data, not a pointer to it. Buffered rather than decoded here
         * because the format is only fully known at END. */
        if (s_gpu.prim && s_gpu.inline_count < NV_MAX_INLINE)
            s_gpu.inline_buf[s_gpu.inline_count++] = param;
        break;

    case NV097_DRAW_ARRAYS:
    case NV097_ARRAY_ELEMENT16:
    case NV097_ARRAY_ELEMENT32:
        if (s_gpu.prim)
            nv2a_index_stream_method(&s_gpu.indices, method, param, draw_index_chunk, NULL);
        break;

    case NV097_SET_FLIP_READ:
        if (s_ftrace == 2)
            fprintf(stderr, "[FTRACE] P_READ %u%c", param, 10);
        s_gpu.flip_read = param;
        return;

    case NV097_SET_FLIP_WRITE:
        if (s_ftrace == 2)
            fprintf(stderr, "[FTRACE] P_WRITE %u%c", param, 10);
        s_gpu.flip_write = param;
        return;

    case NV097_SET_FLIP_MODULO:
        s_gpu.flip_modulo = param;
        return;

    case NV097_FLIP_INCREMENT_WRITE:
        s_gpu.flip_write = s_gpu.flip_modulo
                         ? (s_gpu.flip_write + 1) % s_gpu.flip_modulo
                         : s_gpu.flip_write + 1;
        s_gpu.flips++;
        frame_trace_flip();
        return;

    case NV097_FLIP_STALL:
        no_combiners_refresh();
        /* QueueFlip is authoritative. Clip area cannot identify scanout:
         * MA2's letterboxed 640x240 view is smaller than its 512x512 bloom
         * pass, and the old heuristic showed that pass as a missing shot. */
        {
            uint32_t va, pitch, width, height, bpp;
            if (nv2a_pb_get_scanout(&va, &pitch)) {
                s_gpu.drawn_offset = va;
                s_gpu.drawn_pitch = pitch;
                if (nv2a_gpu_surface_info(va, &width, &height, &pitch, &bpp)) {
                    s_gpu.drawn_pitch = pitch; s_gpu.drawn_bpp = bpp;
                    s_gpu.drawn_x = s_gpu.drawn_y = 0;
                    s_gpu.drawn_w = width; s_gpu.drawn_h = height;
                }
                static int scanout_diag = -1;   /* getenv once: runs every flip */
                if (scanout_diag < 0) scanout_diag = getenv("RECOMP_SCANOUT_DIAG") != NULL;
                if (scanout_diag) {
                    static unsigned n;
                    if (n++ < 16 || n % 1000 == 0)
                        fprintf(stderr,"[SCANOUT] flip=%u va=%08X pitch=%u size=%ux%u current=%08X\n",
                            s_gpu.flips,va,pitch,s_gpu.drawn_w,s_gpu.drawn_h,s_gpu.color_offset);
                }
            }
        }
        perf_frame();
        nv2a_prof_flip();
        if (s_ftrace == 2)
            fprintf(stderr, "[FTRACE] FLIP_STALL presenting %08X (color now %08X)%c",
                    s_gpu.drawn_offset, s_gpu.color_offset, 10);
        /* The stall ends when the buffer being read is the one just finished.
         * There is no scanout here to wait for, so that is now. */
        s_gpu.flip_read = s_gpu.flip_write;
        /* And this is a completed swap, which is what a title's own swap
         * counter counts -- see xbox_Nv2aFrameCounterFlip. */
        xbox_Nv2aFrameCounterFlip();
        /* Hand the window a copy of the frame just finished.
         *
         * The buffer the title has finished is the one the last batch drew
         * into, which is what drawn_offset holds and why it exists: by the
         * flip, color_offset has already moved to the next buffer. Copying
         * here, rather than letting the window read guest memory on its own
         * clock, is what stops it showing a surface the rasteriser is still
         * writing. */
        {
            extern void xbox_FramebufferWindowFrameStats(uint32_t);
            static uint32_t draws_at_flip;
            xbox_FramebufferWindowFrameStats(s_gpu.draws - draws_at_flip);
            draws_at_flip = s_gpu.draws;
        }
        if (s_gpu.pitch) {
            extern void xbox_FramebufferWindowPresent(uint32_t, uint32_t);
            uint32_t done = s_gpu.drawn_offset ? s_gpu.drawn_offset
                                               : s_gpu.color_offset;
            uint32_t pitch = s_gpu.drawn_offset ? s_gpu.drawn_pitch
                                                : s_gpu.pitch;
            if (done) {
                extern void *xbox_FramebufferWindowHandle(void);
                extern void xbox_FramebufferWindowHardware(int);
                int64_t prof_t=nv2a_prof_ticks();
                int native=nv2a_dispatch_present(dma_resolve(done),pitch,xbox_FramebufferWindowHandle());
                nv2a_prof_acc(4 /* present */,prof_t);
                xbox_FramebufferWindowHardware(native);
                xbox_FramebufferWindowSet(dma_resolve(done), pitch);
                if(!native) {
                    if (!nv2a_dispatch_registered())
                        gpu_fallback_note(3, dma_resolve(done), 1);
                    nv2a_gpu_readback(dma_resolve(done));
                    if (!nv2a_dispatch_failed())
                        xbox_FramebufferWindowPresent(dma_resolve(done), pitch);
                }
            }
        }
        {
            /* RECOMP_FB_DUMP_FLIPS: dump every presented frame, not one per
             * report -- a consecutive sequence, which is what a headless
             * recording (frames -> ffmpeg) needs. "1" dumps from boot; any
             * other value names a file, and frames are dumped while it
             * exists, so a capture can start mid-game without writing
             * gigabytes of menus first. */
            static const char *spec = (const char *)-1;
            if (spec == (const char *)-1)
                spec = getenv("RECOMP_FB_DUMP_FLIPS");
            if (spec) {
                static uint32_t capture_frame;
                static uint32_t capture_interval;
                if(!capture_interval) {
                    const char *interval=getenv("RECOMP_FB_DUMP_INTERVAL");
                    capture_interval=interval?(uint32_t)strtoul(interval,NULL,10):1;
                    if(!capture_interval)capture_interval=1;
                }
                int on = strcmp(spec, "1") == 0;
                if (!on) {
                    FILE *f = fopen(spec, "rb");
                    if (f) { fclose(f); on = 1; }
                }
                s_flip_dumping = on;
                if (on && capture_frame++ % capture_interval == 0)
                    dump_surface_bmp();
            }
        }
        nv2a_dispatch_flip(dma_resolve(s_gpu.drawn_offset ? s_gpu.drawn_offset : s_gpu.color_offset),
                           s_gpu.drawn_offset ? s_gpu.drawn_pitch : s_gpu.pitch);
        s_gpu.drawn_stale = 1;

        if (verbose) {
            static unsigned n;
            if (n++ < 8) {
                fprintf(stderr, "  [GPU] flip %u: read=%u write=%u\n",
                        s_gpu.flips, s_gpu.flip_read, s_gpu.flip_write);
                fflush(stderr);
            }
        }
        return;


    /* Vertex programs and their constants: forwarded as they arrive. */
    case 0x1E94:                                  /* TRANSFORM_EXECUTION_MODE */
        s_gpu.xf_mode = param;
        break;
    case 0x1E98:                                  /* _PROGRAM_CXT_WRITE_EN */
        nv2a_vsh_set_cxt_write(param);
        break;
    case 0x1E9C:                                  /* _PROGRAM_LOAD */
        nv2a_vsh_set_load_slot(param);
        break;
    case 0x1EA0:                                  /* _PROGRAM_START */
        nv2a_vsh_set_start_slot(param);
        break;
    case 0x1EA4:                                  /* _CONSTANT_LOAD */
        nv2a_vsh_set_constant_load(param);
        break;

    /* Depth. */
    case 0x030C: s_gpu.depth_test = param; break; /* DEPTH_TEST_ENABLE */
    case 0x0354: s_gpu.depth_func = param; break; /* DEPTH_FUNC */
    case 0x035C: s_gpu.depth_mask = param; break; /* DEPTH_MASK */
    case 0x0214: s_gpu.zeta_offset = param; break;/* SURFACE_ZETA_OFFSET */
    case 0x1D8C: s_gpu.zstencil_clear = param; break; /* ZSTENCIL_CLEAR_VALUE */
    case 0x032C: s_gpu.stencil_test=param;break;
    case 0x0360: s_gpu.stencil_write_mask=param;break;
    case 0x0364: s_gpu.stencil_func=param;break;
    case 0x0368: s_gpu.stencil_ref=param;break;
    case 0x036C: s_gpu.stencil_read_mask=param;break;
    case 0x0370: s_gpu.stencil_fail=param;break;
    case 0x0374: s_gpu.stencil_zfail=param;break;
    case 0x0378: s_gpu.stencil_zpass=param;break;
    case 0x1D98: s_gpu.clear_x=param&0xFFFF;s_gpu.clear_w=(param>>16)>=s_gpu.clear_x?(param>>16)-s_gpu.clear_x+1:0;s_gpu.clear_rect_set|=1;break;
    case 0x1D9C: s_gpu.clear_y=param&0xFFFF;s_gpu.clear_h=(param>>16)>=s_gpu.clear_y?(param>>16)-s_gpu.clear_y+1:0;s_gpu.clear_rect_set|=2;break;

    default:
        if (method >= 0x0260 && method < 0x0280) {         /* ALPHA_ICW(i) */
            s_gpu.rc.alpha_icw[(method - 0x0260) / 4] = param;
            break;
        }
        if (method >= 0x0AC0 && method < 0x0AE0) {         /* COLOR_ICW(i) */
            s_gpu.rc.color_icw[(method - 0x0AC0) / 4] = param;
            break;
        }
        if (method >= 0x1E40 && method < 0x1E60) {         /* COLOR_OCW(i) */
            s_gpu.rc.color_ocw[(method - 0x1E40) / 4] = param;
            break;
        }
        if (method >= 0x0AA0 && method < 0x0AC0) {         /* ALPHA_OCW(i) */
            s_gpu.rc.alpha_ocw[(method - 0x0AA0) / 4] = param;
            break;
        }
        if (method >= 0x0A60 && method < 0x0A80) {         /* FACTOR0(i) */
            s_gpu.rc.factor0[(method - 0x0A60) / 4] = param;
            break;
        }
        if (method >= 0x0A80 && method < 0x0AA0) {         /* FACTOR1(i) */
            s_gpu.rc.factor1[(method - 0x0A80) / 4] = param;
            break;
        }
        /* SET_VIEWPORT_OFFSET / _SCALE. The GPU keeps these in the constant
         * file, at c[59] and c[58] -- exactly where the D3D epilogue reads
         * them -- so they are constants that happen to arrive by method.
         * Dropped, every vertex program put its whole batch at the origin:
         * no 3D anywhere, and a black car on the garage screen. */
        if (method >= 0x0A20 && method < 0x0A30) {
            nv2a_vsh_constant_component(59, (method - 0x0A20) / 4, param);
            break;
        }
        if (method >= 0x0AF0 && method < 0x0B00) {
            nv2a_vsh_constant_component(58, (method - 0x0AF0) / 4, param);
            break;
        }
        if (method >= 0x0B00 && method < 0x0B80) {    /* TRANSFORM_PROGRAM(i) */
            nv2a_vsh_program_word(param);
            break;
        }
        if (method >= 0x0B80 && method < 0x0C00) {    /* TRANSFORM_CONSTANT(i) */
            nv2a_vsh_constant_word(param);
            break;
        }
        if (method >= NV_TEX_FIRST && method <= NV_TEX_LAST)
            record_tex_reg(method, param);
        if (method >= NV097_SET_VERTEX_DATA_ARRAY_OFFSET
                && method < NV097_SET_VERTEX_DATA_ARRAY_OFFSET + NV_VERTEX_ATTRS * 4) {
            /* Resolved here, once, so every consumer -- the rasteriser's
             * attribute reads and the diagnostics alike -- sees the same
             * address. A vertex array offset is a DMA-object offset exactly
             * like a surface offset: physical, and addressable only through
             * the window when it names contiguous memory. */
            s_gpu.attr[(method - NV097_SET_VERTEX_DATA_ARRAY_OFFSET) / 4].offset =
                dma_resolve(param);
        } else if (method >= NV097_SET_VERTEX_DATA_ARRAY_FORMAT
                && method < NV097_SET_VERTEX_DATA_ARRAY_FORMAT + NV_VERTEX_ATTRS * 4) {
            VertexAttr *a = &s_gpu.attr[(method - NV097_SET_VERTEX_DATA_ARRAY_FORMAT) / 4];
            a->type   =  param        & 0x0F;
            a->size   = (param >> 4)  & 0x0F;
            a->stride = (param >> 8)  & 0xFF;
        } else if (!imm_vertex_method(method, param)) {
            note_unhandled(method, param);
        }
        break;
    }
}

/* Find where the title actually wrote its quad.
 *
 * The GPU is pointed at a buffer that stays zero, which says the data went
 * somewhere else -- and the only way to find somewhere else is to look for the
 * data. A screen-space quad for a 640x480 target contains 640.0f and 480.0f as
 * floats, which is a distinctive enough pair to search guest RAM for. Whatever
 * address that turns up is where the title's writes are landing, and the
 * difference from the programmed offset is the bug. */
static void find_quad_vertices(void)
{
    const uint32_t W = 0x44200000u;   /* 640.0f */
    const uint32_t H = 0x43F00000u;   /* 480.0f */
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;

    fprintf(stderr, "[GPU] searching guest RAM for 640.0f/480.0f pairs...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 8 && hits < 12; i++) {
        if (ram[i] != W && ram[i] != H)
            continue;
        /* Both values within a few words of each other: a lone 640.0f is
         * common, the pair much less so. */
        {
            int has_w = 0, has_h = 0;
            uint32_t k;
            for (k = 0; k < 8; k++) {
                if (ram[i + k] == W) has_w = 1;
                if (ram[i + k] == H) has_h = 1;
            }
            if (!has_w || !has_h)
                continue;
        }
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 8; k++)
                fprintf(stderr, " %08X", ram[i + k]);
        }
        fprintf(stderr, "\n");
        i += 8;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none found -- the quad is not in RAM in"
                        " that form\n");
    fflush(stderr);
}

/* Locate NaN-filled transform matrices in guest RAM.
 *
 * A matrix arriving as NaN says the maths went wrong somewhere upstream, and
 * the only way to find where is to find the matrix and watch who writes it.
 * Three consecutive real-indefinite values is a distinctive enough signature:
 * ordinary data does not contain runs of 0xFFC00000. */
static void find_nan_matrices(void)
{
    const uint32_t NAN_NEG = 0xFFC00000u;
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;

    fprintf(stderr, "[GPU] searching guest RAM for NaN matrices...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 20 && hits < 10; i++) {
        if (ram[i] != NAN_NEG || ram[i + 1] != NAN_NEG || ram[i + 2] != NAN_NEG)
            continue;
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X", ram[i + k]);
        }
        fprintf(stderr, "\n");
        i += 16;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none in RAM -- the NaNs are computed into"
                        " registers, not stored\n");
    fflush(stderr);
}

/* Print guest dwords named by RECOMP_PEEK, as hex and as float.
 *
 * Chasing a value backwards means reading it, and a value that is only wrong
 * for one frame in a thousand cannot be caught by stopping. Both
 * interpretations are printed because the question is usually "is this a
 * pointer or a number", and guessing wrong costs a run. */
static void peek_addresses(void)
{
    const char *spec = getenv("RECOMP_PEEK");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256];
    char *tok, *ctx = NULL;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t va = (uint32_t)strtoul(tok, NULL, 0);
        uint32_t v;
        float f;
        if (va < 0x1000u || va >= 0x04000000u)
            continue;
        v = *(const uint32_t *)(mem + va);
        memcpy(&f, &v, 4);
        fprintf(stderr, "  [PEEK] 0x%08X = %08X  (%g)\n", va, v, f);
    }
    fflush(stderr);
}

/* Walk a pointer chain and print every step.
 *
 * RECOMP_PEEK_CHAIN="0x1315A8,8,0x10,0" starts at that address, and for each
 * offset dereferences the current pointer and adds it. Following a chain by
 * hand costs one run per level; this costs one run for the whole chain, and
 * prints where it goes wrong when a level is null.
 */
static void peek_chain(void)
{
    const char *spec = getenv("RECOMP_PEEK_CHAIN");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256], *tok, *ctx = NULL;
    uint32_t cur = 0;
    int step = 0;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t off = (uint32_t)strtoul(tok, NULL, 0);
        if (step == 0) {
            cur = off;
            fprintf(stderr, "  [CHAIN] start 0x%08X\n", cur);
        } else {
            if (cur < 0x1000u || cur + 4 >= 0x04000000u) {
                fprintf(stderr, "  [CHAIN] step %d: 0x%08X is not a guest"
                                " pointer -- chain ends\n", step, cur);
                return;
            }
            cur = *(const uint32_t *)(mem + cur) + off;
            fprintf(stderr, "  [CHAIN] step %d: deref +0x%X -> 0x%08X\n",
                    step, off, cur);
        }
        step++;
    }
    if (cur >= 0x1000u && cur + 4 < 0x04000000u)
        fprintf(stderr, "  [CHAIN] final value at 0x%08X = 0x%08X\n",
                cur, *(const uint32_t *)(mem + cur));
    fflush(stderr);
}

void nv2a_pb_exec_report(void)
{
    raster_pool_shutdown();
    peek_addresses();
    peek_chain();
    if (getenv("RECOMP_FIND_NAN")) {
        /* Every report, not once: the matrix is fine early on and only turns
         * to NaN later, so a single scan at startup finds nothing and says
         * nothing. */
        find_nan_matrices();
    }
    if (getenv("RECOMP_FIND_QUAD")) {
        static int done;
        if (!done) { done = 1; find_quad_vertices(); }
    }
    int i, j;

    {
        /* Frames per second of guest time, from flips between reports. */
        static uint32_t last_flips;
        static unsigned long last_ms;
        unsigned long now = (unsigned long)(clock() * 1000.0 / CLOCKS_PER_SEC);
        if (last_ms && now > last_ms)
            fprintf(stderr, "[GPU] %.2f fps (%u flips)%c",
                    (s_gpu.flips - last_flips) * 1000.0 / (now - last_ms),
                    s_gpu.flips, 10);
        last_flips = s_gpu.flips;
        last_ms = now;
    }
    fprintf(stderr, "[GPU] surface 0x%08X pitch %u clip %ux%u+%u+%u"
                    " clears %u | %u unhandled methods (%d distinct)\n",
            s_gpu.color_offset, s_gpu.pitch, s_gpu.clip_w, s_gpu.clip_h,
            s_gpu.clip_x, s_gpu.clip_y, s_gpu.clears,
            s_gpu.unhandled_total, s_unhandled_count);
    fprintf(stderr, "[GPU] draws %u (%u with coordinates), %u indices;"
                    " x %.1f..%.1f  y %.1f..%.1f\n",
            s_gpu.draws, s_gpu.nonzero_draws, s_gpu.verts,
            s_gpu.min_x, s_gpu.max_x, s_gpu.min_y, s_gpu.max_y);
    /* One picture per report rather than per clear: a title clears hundreds of
     * times a second and nobody wants that many files. */
    if (!s_flip_dumping)
        dump_surface_bmp();

    /* Drawn and skipped separately: "nothing appeared" and "every batch needed
     * a vertex program we do not run" look identical on screen, and only one
     * of them means the rasteriser is broken. */
    fprintf(stderr, "[GPU] brightest pixel written 0x%08X\n", s_gpu.pixel_max);
    fprintf(stderr, "[GPU] %llu pixels written; draw surface 0x%08X"
                    " -> 0x%08X, clear surface 0x%08X -> 0x%08X\n",
            (unsigned long long)s_gpu.pixels, s_gpu.drawn_offset,
            dma_resolve(s_gpu.drawn_offset), s_gpu.color_offset,
            dma_resolve(s_gpu.color_offset));
    fprintf(stderr, "[GPU] rasterised %u triangles; %u batches skipped as not"
                    " screen-space, %u triangles fully off-surface\n",
            s_gpu.tris_drawn, s_gpu.batches_untransformed,
            s_gpu.tris_skipped_offscreen);
    fprintf(stderr, "[GPU] vertex programs: %u batches, %u vertices;"
                    " %u triangles dropped behind the eye\n",
            s_gpu.batches_program, s_gpu.verts_program, s_gpu.tris_behind);
    fprintf(stderr, "[GPU]   of the rest: %u degenerate/NaN, %u off-surface,"
                    " %u drawn; pixels %llu written, %llu depth-failed;"
                    " x %.0f..%.0f y %.0f..%.0f z %g..%g\n",
            s_gpu.xf_degenerate, s_gpu.xf_offscreen, s_gpu.xf_drawn,
            (unsigned long long)s_gpu.xf_pixels,
            (unsigned long long)s_gpu.xf_depth_fail,
            s_gpu.xf_min[0], s_gpu.xf_max[0], s_gpu.xf_min[1], s_gpu.xf_max[1],
            s_gpu.xf_min[2], s_gpu.xf_max[2]);
    {
        fprintf(stderr, "[GPU]   blend pairs (src/dst):");
        for (j = 0; j < s_gpu.blend_npairs; j++)
            fprintf(stderr, " %X/%X", s_gpu.blend_pairs[j] >> 16,
                    s_gpu.blend_pairs[j] & 0xFFFF);
        fputc(10, stderr);
        fprintf(stderr, "[GPU]   program attribute formats (slot:type/size):");
        for (j = 0; j < s_gpu.xf_nfmt; j++)
            fprintf(stderr, " v%u:%u/%u", s_gpu.xf_fmt[j] >> 8,
                    s_gpu.xf_fmt[j] & 15, (s_gpu.xf_fmt[j] >> 4) & 15);
        fprintf(stderr, "\n");
    }

    /* And of the batches that did rasterise, how many sampled anything. A menu
     * that draws its background from one texture and its text from another
     * shows both as flat colour if either half is missing, so the split is
     * what says which half. */
    fprintf(stderr, "[GPU] batches: %u textured, %u with no texcoords,"
                    " %u with texcoords but no usable stage\n",
            s_gpu.batches_textured, s_gpu.batches_no_uv, s_gpu.batches_no_tex);
    for (i = 0; i < s_tex_use_count; i++)
        fprintf(stderr, "  [TEXUSE] 0x%08X %ux%u fmt 0x%02X%s: %u batches\n",
                s_tex_use[i].offset, s_tex_use[i].width, s_tex_use[i].height,
                s_tex_use[i].color,
                d3d8_format_dxt_block_bytes(s_tex_use[i].color) ? " dxt"
                    : d3d8_format_is_swizzled(s_tex_use[i].color) ? " swz" : " lin",
                s_tex_use[i].batches);

    if (getenv("RECOMP_TEX_STATE")) {
        uint32_t k;
        for (k = 0; k < sizeof s_tex_set / sizeof s_tex_set[0]; k++)
            if (s_tex_set[k])
                fprintf(stderr, "  [TEX] 0x%04X = 0x%08X\n",
                        (unsigned)(NV_TEX_FIRST + k * 4), s_tex_reg[k]);
    }

    /* Top ten by frequency: selection sort over a small table, once every few
     * seconds, is not worth a better algorithm.
     *
     * RECOMP_PB_UNHANDLED_ALL lists every one instead. Ten is the right
     * default -- the tail is a long list of state registers nobody needs to
     * read -- but when a title stops and the question is which method it
     * stopped on, the answer is as likely to be the one seen twice as the
     * one seen a thousand times, and ten hides it. */
    {
        int shown = getenv("RECOMP_PB_UNHANDLED_ALL") ? s_unhandled_count : 10;
    for (i = 0; i < shown && i < s_unhandled_count; i++) {
        int best = i;
        for (j = i + 1; j < s_unhandled_count; j++)
            if (s_unhandled[j].count > s_unhandled[best].count)
                best = j;
        if (best != i) {
            PbUnhandled t = s_unhandled[i];
            s_unhandled[i] = s_unhandled[best];
            s_unhandled[best] = t;
        }
        {
            /* The value as well as the count. A method nobody decoded is
             * a guess until you see what it carried: screen coordinates,
             * a 0..1 texcoord and a packed colour are told apart at a
             * glance, and that is what says which vertex encoding a title
             * is using. */
            union { uint32_t u; float f; } v;
            v.u = s_unhandled[i].last_param;
            fprintf(stderr, "  [GPU]   0x%04X x%-8u last=0x%08X (%.4f)\n",
                    s_unhandled[i].method, s_unhandled[i].count,
                    v.u, v.f);
        }
    }
    }    fflush(stderr);
}
