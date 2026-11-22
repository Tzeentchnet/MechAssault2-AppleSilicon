/*
 * MCPX APU DSP (GP/EP) and diagnostic mixdown fallback
 *
 * The programmable effects and output processors execute through the xemu
 * interpreter in apu_dsp_runtime.c. The previous mixdown remains available
 * during bring-up and when the guest has not enabled either processor.
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "apu_state.h"
#include "fpconv.h"
#include "../kernel/xbox_memory_layout.h"

#include <stdlib.h>
#include <string.h>

/* The MCPX boot ROM loads 0x800 24-bit program words from scratch offset
 * zero through the programmed 4 KiB scatter/gather list. This is the same
 * bootstrap layout used by xemu's gp_ep.c and dsp_c.c. Even a passthrough
 * DSP must expose the uploaded program: DirectSound checks an instruction
 * there to detect a lost processor and otherwise resets both DSPs every
 * frame, including a 10 ms hardware delay.
 *
 * Keep the program in the mapped DSP memory so bulk guest copies still use
 * ordinary RAM. Only the register page at the end of each DSP is trapped. */
bool mcpx_apu_dsp_bootstrap(MCPXAPUState *d, bool is_gp, uint32_t *program)
{
    uint32_t words[0x800];
    uint32_t base,limit;
    if (!d || !d->ram_ptr || !program) return false;
    base=qatomic_read(&d->regs[is_gp ? NV_PAPU_GPSADDR : NV_PAPU_EPSADDR]);
    limit=qatomic_read(&d->regs[is_gp ? NV_PAPU_GPSMAXSGE : NV_PAPU_EPSMAXSGE]);
    if (!base || base>0x03FFFFF0u || limit<1) return false;
    for (uint32_t page=0;page<2;page++) {
        uint32_t address=ldl_le_phys(NULL,base+page*8);
        if (address>0x03FFF000u || (address & 0xFFFu)) return false;
        memcpy((uint8_t *)words+page*0x1000,mcpx_apu_phys(address),0x1000);
    }
    for (unsigned i=0;i<0x800;i++) words[i]&=0x00FFFFFFu;
    memcpy(program,words,sizeof(words));
    return true;
}

uint64_t mcpx_apu_dsp_mmio_read(MCPXAPUState *d, uint32_t addr)
{
    uint32_t *regs=addr<0x50000 ? d->gp.regs : d->ep.regs;
    return qatomic_read(&regs[addr & 0xFFFFu]);
}

void mcpx_apu_dsp_mmio_write(MCPXAPUState *d, uint32_t addr, uint32_t value)
{
    bool is_gp=addr<0x50000;
    uint32_t *regs=is_gp ? d->gp.regs : d->ep.regs;
    uint32_t offset=addr & 0xFFFFu;
    uint32_t old=qatomic_read(&regs[offset]);
    qatomic_set(&regs[offset],value);
    DSPState *dsp=is_gp ? d->gp.dsp : d->ep.dsp;
    if(offset==NV_PAPU_GPRST && (value&3)!=3)mcpx_apu_dsp_reset_program(dsp);
    if (offset==NV_PAPU_GPRST && (value & 3)==3 && (old & 3)!=3) {
#if defined(_WIN32)
        uint32_t *mapped=(uint32_t *)(d->ram_ptr+(is_gp ? 0xFE83A000u : 0xFE85A000u));
#else
        uint32_t *mapped = xbox_McpxMemoryAlias(is_gp ? 0x3A000u : 0x5A000u,
                                                0x800u * sizeof(uint32_t));
        if (!mapped) {
            fprintf(stderr, "[APU] %s boot DMA has no MCPX alias\n", is_gp ? "GP" : "EP");
            return;
        }
#endif
        if (mcpx_apu_dsp_bootstrap(d,is_gp,mapped)) {
            if (dsp) {
                memcpy(dsp->core.pram,mapped,0x800*sizeof(uint32_t));
                memset((void *)dsp->core.pram_opcache,0,sizeof(dsp->core.pram_opcache));
            }
            static unsigned shown;
            if (shown++<4)fprintf(stderr,"[APU] %s boot DMA: 2048 program words, P[6]=%06X\n",
                is_gp ? "GP" : "EP",mapped[6]);
        } else {
            fprintf(stderr,"[APU] %s boot DMA rejected invalid scratch descriptors\n",is_gp ? "GP" : "EP");
        }
    }
}

/* ── DSP command doorbell acknowledgement ────────────────────────────────
 *
 * DirectSound does not stop at creating the device. It hands the audio DSP a
 * command block in guest RAM, writes a command word, and spins until the DSP
 * writes zero back. On real hardware the GP runs a DSP56300 program that does
 * that. Here the DSP is a passthrough stub, so the word never changes and the
 * title hangs inside DirectSound initialisation -- which on Wreckless gates the
 * entire engine, not just audio.
 *
 * RECOMP_APU_DSP_ACK=<addr>[,<addr>...] clears those guest dwords once per APU
 * frame, which is what "the command completed" looks like to the title.
 *
 * ponytail: this is a handshake acknowledgement, not a DSP. It says every
 * command succeeded instantly and computes nothing, so anything whose *result*
 * the title reads back will still be wrong. The real fix is DSP56300 emulation
 * in the GP/EP; this exists so audio init stops blocking everything behind it.
 *
 * The address is not derivable from the APU registers: GPSADDR/GPFADDR/
 * EPSADDR/EPFADDR point at the DSP's own scratch and frame memory, while the
 * command block is a DirectSound heap allocation. On Wreckless the registers
 * read 0x01504000 / 0x014EC000 / 0x0151C000 / 0x014F0000 and the doorbell is at
 * 0x014F8810 -- inside none of them. So it has to be observed: run with
 * RECOMP_WATCHDOG_SECS and the spin shows up as ebx plus the poll offset.
 */
#define APU_DSP_ACK_MAX 8
static uint32_t s_dsp_ack[APU_DSP_ACK_MAX];
static int s_dsp_ack_count = -1;

static void dsp_ack_init(void)
{
    const char *spec = getenv("RECOMP_APU_DSP_ACK");
    char buf[256], *p, *end;

    s_dsp_ack_count = 0;
    if (!spec || !*spec)
        return;
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (p = buf; *p && s_dsp_ack_count < APU_DSP_ACK_MAX; ) {
        unsigned long v = strtoul(p, &end, 0);
        if (end == p)
            break;
        if (v)
            s_dsp_ack[s_dsp_ack_count++] = (uint32_t)v;
        p = (*end == ',') ? end + 1 : end;
    }
    if (s_dsp_ack_count)
        fprintf(stderr, "[APU] DSP doorbell ack: %d address(es), first 0x%08X\n",
                s_dsp_ack_count, s_dsp_ack[0]);
}

/* SUM EVERY MIXBIN THE GUEST ROUTED TO, NOT JUST THE FIRST TWO.
 *
 * Default ON. RECOMP_APU_MIXDOWN_ALL=0 restores the previous two-bin read,
 * because this changes audible output for every title and an escape hatch
 * costs one branch. */
static int mcpx_apu_mixdown_all(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_APU_MIXDOWN_ALL");
        on = (e && *e) ? (atoi(e) != 0) : 1;
    }
    return on;
}

void mcpx_apu_dsp_set_doorbell(MCPXAPUState *d, uint32_t va)
{
    if (!d || !d->ram_ptr ||
        !((va >= 0x10000 && va <= 0x03FFFFFC) ||
          (va >= 0x80000000 && va <= 0x83FFFFFC)))
        return;
    if (qatomic_read(&d->dsp_doorbell_va) != va) {
        fprintf(stderr, "[APU] title registered DSP doorbell 0x%08X\n", va);
        qatomic_set(&d->dsp_doorbell_va, va);
    }
}

void mcpx_apu_dsp_ack_poll(MCPXAPUState *d)
{
    int i;

    if(mcpx_apu_dsp_enabled())return;

    if (s_dsp_ack_count < 0)
        dsp_ack_init();
    if (!d->ram_ptr)
        return;
    {
        uint32_t va = qatomic_read(&d->dsp_doorbell_va);
        if (va) {
            uint32_t *slot = (uint32_t *)(d->ram_ptr + va);
            if (*slot) *slot = 0;
        }
    }
    for (i = 0; i < s_dsp_ack_count; i++) {
        uint32_t *slot = (uint32_t *)(d->ram_ptr + s_dsp_ack[i]);
        if (*slot) {
            static int shown[APU_DSP_ACK_MAX];
            if (shown[i]++ < 3)
                fprintf(stderr, "[APU] DSP doorbell 0x%08X: command 0x%08X"
                                " acknowledged\n", s_dsp_ack[i], *slot);
            *slot = 0;
        }
    }
}

void mcpx_apu_dsp_init(MCPXAPUState *d)
{
    d->gp.dsp = (DSPState *)calloc(1, sizeof(DSPState));
    d->ep.dsp = (DSPState *)calloc(1, sizeof(DSPState));

    if (d->gp.dsp) d->gp.dsp->is_gp = true;
    if (d->ep.dsp) d->ep.dsp->is_gp = false;
    mcpx_apu_dsp_native_init(d);
    mcpx_apu_update_dsp_preference(d);

    d->gp.realtime = false;
    d->ep.realtime = false;

    fprintf(stderr, "[APU] DSP GP/EP initialized (%s)\n",
            mcpx_apu_dsp_enabled()?"DSP56300 interpreter":"diagnostic mixdown");
}

void mcpx_apu_update_dsp_preference(MCPXAPUState *d)
{
    if(mcpx_apu_dsp_enabled()) {
        d->monitor.point=MCPX_APU_DEBUG_MON_GP_OR_EP;
        d->gp.realtime=d->ep.realtime=true;
    }
}

void mcpx_apu_dsp_frame(MCPXAPUState *d,
                         float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    static int stats=-1;
    if(stats<0)stats=getenv("RECOMP_AUDIO_STATS")!=NULL;
    if(stats) {
        static double energy[NUM_MIXBINS];
        static float peak[NUM_MIXBINS], mixed_peak;
        static unsigned count, clipped;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            float sum[2] = {0};
            for (int b = 0; b < NUM_MIXBINS; b++) {
                float x = mixbins[b][i];
                energy[b] += (double)x * x;
                if (fabsf(x) > peak[b]) peak[b] = fabsf(x);
                sum[b & 1] += x;
            }
            for (int ch = 0; ch < 2; ch++) {
                if (fabsf(sum[ch]) > mixed_peak) mixed_peak = fabsf(sum[ch]);
                if (fabsf(sum[ch]) > 1.0f) clipped++;
            }
        }
        count += NUM_SAMPLES_PER_FRAME;
        if (count >= 48000 * 5) {
            fprintf(stderr, "[AUDIO-BINS] samples=%u preclip-peak=%.5f clipped=%.3f%%", count,
                mixed_peak, 100.0 * clipped / (count * 2.0));
            for (int b = 0; b < NUM_MIXBINS; b++)
                if (peak[b] > 0.00001f)
                    fprintf(stderr, " bin%d=%.4f/%.4f", b, peak[b], sqrt(energy[b] / count));
            fprintf(stderr, "\n"); fflush(stderr);
            memset(energy, 0, sizeof(energy)); memset(peak, 0, sizeof(peak));
            count = clipped = 0; mixed_peak = 0;
        }
    }
    if(mcpx_apu_dsp_native_frame(d,mixbins))return;

    /* Bypass DSP: take mixbin 0 (front-left) and mixbin 1 (front-right)
     * and write them directly to the monitor frame buffer as the final
     * EP output.
     *
     * The Xbox DirectSound typically routes:
     *   Mixbin 0 = Front Left
     *   Mixbin 1 = Front Right
     *   Mixbin 2 = Center (often unused in stereo)
     *   Mixbin 3 = LFE
     *   Mixbin 4-5 = Rear L/R
     *
     * For stereo output, bins 0 and 1 are what we want.
     */

    int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;

    if (d->monitor.point != MCPX_APU_DEBUG_MON_VP) {
        float mixed[NUM_SAMPLES_PER_FRAME][2], limited[NUM_SAMPLES_PER_FRAME][2];
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            /* Bins 2..31 used to be computed and then dropped on the floor.
             * On hardware the GP and EP mix the submixes down; here they are
             * stubs, so thirty of thirty-two bins were discarded every frame
             * with no counter anywhere to say so.
             *
             * Measured on Jet Set Radio Future, one 200 s gameplay run, with
             * a positive control moving beside it:
             *
             *     [APU-BIN] 2D heard=557466 lost=0
             *               3D heard=0      lost=377768
             *               lost by bin: 6,7,8,9,10
             *
             * 557,466 music voice-frames heard and none lost; 377,768 effect
             * voice-frames produced correctly and thrown away. The title's 3D
             * positional voices -- its sound effects -- are routed to bins 6
             * to 10 by the guest's own V0BIN..V3BIN, and music on 2D voices
             * lands in bins 0 and 1, which is why the music was always
             * audible and no effect ever was. Every instrument upstream of
             * this line read healthy.
             *
             * Gating the HRTF submix override was tried first and did not fix
             * it, so the defect is the width of this mixdown and nothing else.
             *
             * Even bins left, odd bins right, which preserves the stereo
             * pairing the guest set up -- bins 6/7 and 8/9 arrive with matched
             * counts. This is not what a real EP does; it is the cheapest
             * mixdown that stops discarding audio. */
            float left, right;
            if (mcpx_apu_mixdown_all()) {
                left = 0.0f;
                right = 0.0f;
                for (int b = 0; b < NUM_MIXBINS; ++b) {
                    if (b & 1) right += mixbins[b][i];
                    else       left  += mixbins[b][i];
                }
            } else {
                left = mixbins[0][i];
                right = mixbins[1][i];
            }
            mixed[i][0] = left;
            mixed[i][1] = right;
        }
        apu_limit_block(&d->monitor.limiter, mixed, limited);
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            /* Convert to 16-bit and write (not accumulate) into frame buffer.
             * Each of the 8 sub-frames writes its own 32-sample slice. */
            d->monitor.frame_buf[off + i][0] = (int16_t)(limited[i][0] * 32767.0f);
            d->monitor.frame_buf[off + i][1] = (int16_t)(limited[i][1] * 32767.0f);
        }
    }

    g_dbg.gp.cycles = 0;
    g_dbg.ep.cycles = 0;
}
