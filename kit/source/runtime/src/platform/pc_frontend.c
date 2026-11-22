#include "pc_frontend.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#if defined(_WIN32)
#include <windows.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
typedef volatile LONG PcAtomic;
static int pc_load(PcAtomic *v) {
    /* Aligned LONG reads are atomic. Reading a setting must not issue a locked
     * read/modify/write on every draw or HUD label. Keep acquire ordering for
     * published input revisions; mutations and one-shot consumption below
     * remain interlocked. The compiler barrier also covers /volatile:iso. */
#if defined(_MSC_VER)
    int value=ReadAcquire(v);
    _ReadBarrier();
    return value;
#else
    return __atomic_load_n(v,__ATOMIC_ACQUIRE);
#endif
}
static void pc_store(PcAtomic *v,int n) { InterlockedExchange(v,n); }
static int pc_exchange(PcAtomic *v,int n) { return InterlockedExchange(v,n); }
static void pc_increment(PcAtomic *v) { InterlockedIncrement(v); }
#else
#include <stdatomic.h>
typedef atomic_int PcAtomic;
static int pc_load(PcAtomic *v) { return atomic_load_explicit(v,memory_order_relaxed); }
static void pc_store(PcAtomic *v,int n) { atomic_store_explicit(v,n,memory_order_relaxed); }
static int pc_exchange(PcAtomic *v,int n) { return atomic_exchange_explicit(v,n,memory_order_relaxed); }
static void pc_increment(PcAtomic *v) { atomic_fetch_add_explicit(v,1,memory_order_relaxed); }
#endif
static PcRenderCallback render_callback;
static PcMessageCallback message_callback;
static PcDeviceLostCallback device_lost_callback;
#if defined(_WIN32)
static PVOID volatile output_policy_callback;
#else
static _Atomic(PcInputOutputPolicyCallback) output_policy_callback;
#endif
static void pc_input_output_policy_changed(void) {
#if defined(_WIN32)
    PcInputOutputPolicyCallback callback=(PcInputOutputPolicyCallback)(uintptr_t)
        InterlockedCompareExchangePointer(&output_policy_callback,NULL,NULL);
#else
    PcInputOutputPolicyCallback callback=atomic_load_explicit(&output_policy_callback,memory_order_acquire);
#endif
    if(callback)callback();
}
void pc_input_register_output_policy_callback(PcInputOutputPolicyCallback callback) {
#if defined(_WIN32)
    InterlockedExchangePointer(&output_policy_callback,(PVOID)(uintptr_t)callback);
#else
    atomic_store_explicit(&output_policy_callback,callback,memory_order_release);
#endif
    pc_input_output_policy_changed();
}
static PcAtomic captured;
/* Zero means unset; nonnegative public values are stored plus one. */
static PcAtomic settings[PC_SETTING_COUNT], bindings[PC_BINDING_COUNT];
static PcAtomic input_context, input_device, input_revision;
static PcAtomic pad_buttons, pad_axes[6];
static PcAtomic menu_buttons[8],camera_capture,text_capture;
static PcAtomic display_size, render_size, movie_count, movie_skip, safe_start_movie_skip;
static PcAtomic cinematic, cinematic_view, cinematic_fps;
void pc_render_size(unsigned width,unsigned height) {
    if(width && height && width<65536 && height<65536)pc_store(&render_size,(int)((width<<16)|height));
}
void pc_render_size_get(unsigned *width,unsigned *height) {
    unsigned dimensions=(unsigned)pc_load(&render_size);
    if(!dimensions)dimensions=(unsigned)pc_load(&display_size);
    *width=dimensions?dimensions>>16:640;*height=dimensions?dimensions&65535:480;
}
void pc_display_size(unsigned width,unsigned height) {
    if(width && height && width<65536 && height<65536)pc_store(&display_size,(int)((width<<16)|height));
    if(getenv("RECOMP_DISPLAY_TRACE"))fprintf(stderr,"[PC-DISPLAY] client=%ux%u aspect=%.4f movies=%d\n",width,height,pc_display_aspect(4.f/3.f),pc_load(&movie_count));
}
void pc_display_movie(int change) {
    if(change>0 && pc_load(&movie_count)==0)pc_input_movie_skip_rearm();
    if(change && (change>0 || pc_load(&movie_count)<=1))pc_store(&movie_skip,0);
    if(change>0)pc_increment(&movie_count);
    else if(change<0)pc_store(&movie_count,pc_load(&movie_count)>0?pc_load(&movie_count)-1:0);
    if(getenv("RECOMP_DISPLAY_TRACE"))fprintf(stderr,"[PC-DISPLAY] movie change=%d count=%d aspect=%.4f\n",change,pc_load(&movie_count),pc_display_aspect(4.f/3.f));
}
int pc_display_movie_active(void) { return pc_load(&movie_count)>0; }
void pc_display_cinematic(int active) {
    active=active!=0;
    if(pc_exchange(&cinematic,active)!=active && getenv("RECOMP_DISPLAY_TRACE"))
        fprintf(stderr,"[PC-DISPLAY] cinematic=%d aspect=%.4f\n",active,pc_display_aspect(4.f/3.f));
}
int pc_display_cinematic_active(void) { return pc_load(&cinematic); }
/* View and frame cap used while a cinematic is active (stored plus one; zero = not configured). */
void pc_display_cinematic_config(int view,int fps) {
    pc_store(&cinematic_view,(view>=PC_CINEMATIC_PILLARBOX&&view<=PC_CINEMATIC_WIDE?view:PC_CINEMATIC_FILL_16_9)+1);
    pc_store(&cinematic_fps,(fps>0&&fps<=1000?fps:0)+1);
}
static int pc_cinematic_view(void) { int v=pc_load(&cinematic_view);return v?v-1:PC_CINEMATIC_PILLARBOX; }
static int pc_cinematic_fps(void) { int v=pc_load(&cinematic_fps);return v?v-1:30; }
void pc_input_movie_skip(void) { if(pc_display_movie_active())pc_store(&movie_skip,1); }
void pc_input_movie_skip_rearm(void) { pc_store(&safe_start_movie_skip,0); }
int pc_input_movie_skip_pending(int ack) {
    const char *safe=getenv("RECOMP_SAFE_START");
    if(safe && *safe && !(safe[0]=='0' && safe[1]=='\0'))
        return ack ? pc_exchange(&safe_start_movie_skip,1)==0 :
            pc_load(&safe_start_movie_skip)==0;
    return pc_display_movie_active()?(ack?pc_exchange(&movie_skip,0):pc_load(&movie_skip)):0;
}
/* Prerecorded movies keep their own shape without help from here: the game's movie player lays a
 * movie out from the screen size it is given (the widened one, sub_382C0), preserving the movie's
 * aspect -- the 720x480 NTSC intros are true 3:2, the PAL ones 4:3. Forcing 4:3 while a movie
 * played took effect only at the first frame copy, after that layout, and squeezed the widened
 * frame into 4:3: on a 2.4:1 screen the intro showed at 0.84:1 instead of 1.5:1 (2026-10-14). */
float pc_display_aspect(float original) {
    unsigned dimensions=(unsigned)pc_load(&render_size);
    if(!dimensions)dimensions=(unsigned)pc_load(&display_size);
    if(!pc_setting_get(PC_ASPECT,0) || !dimensions)return original;
    float aspect=(float)(dimensions>>16)/(dimensions&65535);
    if(pc_load(&cinematic) && pc_cinematic_view()==PC_CINEMATIC_PILLARBOX)return original;
    return aspect>original?aspect:original;
}
/* Fill views render the cinematic at the full output size (the widest aspect pc_display_aspect gives)
 * and frame each shot through its camera viewport (ma2_cinematic.c): a shot keeps its authored width
 * and is shown at the widest aspect up to this cap, cropping only its top and bottom. Returns 0 when no
 * fill view is active. */
float pc_display_cinematic_fill_cap(void) {
    if(!pc_load(&cinematic))return 0.f;
    int view=pc_cinematic_view();
    float screen=pc_display_aspect(4.f/3.f);
    if(view==PC_CINEMATIC_FILL)return screen;
    if(view==PC_CINEMATIC_FILL_16_9)return screen<16.f/9.f?screen:16.f/9.f;
    return 0.f;
}
unsigned pc_display_render_width(unsigned width,unsigned scale) {
    return (unsigned)(width*scale*(pc_display_aspect(4.f/3.f)/(4.f/3.f))+.5f);
}
void pc_display_render_extent(unsigned width,unsigned height,unsigned *pixel_width,unsigned *pixel_height) {
    unsigned selected_width,selected_height;pc_render_size_get(&selected_width,&selected_height);
    float aspect=pc_display_aspect(4.f/3.f);
    float fitted_height=(float)selected_height;
    if(fitted_height*aspect>selected_width)fitted_height=selected_width/aspect;
    int diagnostic_scale=pc_setting_get(PC_RENDER_SCALE,0);
    if(diagnostic_scale>0 && diagnostic_scale<=4)fitted_height=480.f*diagnostic_scale;
    *pixel_width=(unsigned)(width*fitted_height/480.f*aspect/(4.f/3.f)+.5f);
    *pixel_height=(unsigned)(height*fitted_height/480.f+.5f);
    if(*pixel_width>16384)*pixel_width=16384;if(*pixel_height>16384)*pixel_height=16384; /* D3D11 texture limit */
    if(!*pixel_width)*pixel_width=1;if(!*pixel_height)*pixel_height=1;
}
void pc_input_menu_button(unsigned button){if(button<8)pc_store(&menu_buttons[button],1);}
int pc_input_menu_button_pending(unsigned button,int ack){return button<8?(ack?pc_exchange(&menu_buttons[button],0):pc_load(&menu_buttons[button])):0;}
void pc_input_menu_click(void){pc_input_menu_button(0);}
int pc_input_menu_clicked(int ack){return pc_input_menu_button_pending(0,ack);}
int pc_input_camera_captured(void){return pc_load(&camera_capture);}
void pc_input_camera_capture(int capture){pc_store(&camera_capture,capture!=0);}
void pc_input_text_capture(int capture){pc_store(&text_capture,capture!=0);}
int pc_input_text_captured(void){return pc_load(&text_capture);}
void pc_frontend_register(PcRenderCallback r,PcMessageCallback m) { render_callback=r;message_callback=m; }
void pc_frontend_register_device_lost(PcDeviceLostCallback c) { device_lost_callback=c; }
void pc_frontend_device_lost(void) { if(device_lost_callback)device_lost_callback(); }
#if defined(_WIN32)
/* Cinematic frame cap (CinematicFrameCap, default 60, 0 = none): hold each presented frame to 1/cap s.
 * Runs just before Present, so a lower user frame limit still applies after
 * it. A high-resolution waitable timer covers most of the wait; the final
 * half millisecond spins. Resets after any long gap so it never bursts. */
static void pc_cinematic_pace(void) {
    static LARGE_INTEGER frequency,next;
    static HANDLE timer;
    LARGE_INTEGER now;
    if(!frequency.QuadPart)QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    int fps=pc_cinematic_fps();
    if(!pc_load(&cinematic) || fps<=0){next.QuadPart=0;return;}
    LONGLONG period=frequency.QuadPart/fps;
    if(!next.QuadPart || now.QuadPart>next.QuadPart+period*4)next=now;
    if(!timer)timer=CreateWaitableTimerExW(NULL,NULL,0x00000002/*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/,TIMER_ALL_ACCESS);
    LONGLONG remaining=next.QuadPart-now.QuadPart,spin=frequency.QuadPart/2000;
    if(remaining>spin) {
        LARGE_INTEGER due;due.QuadPart=-(LONGLONG)((double)(remaining-spin)*10000000.0/frequency.QuadPart);
        if(timer && SetWaitableTimer(timer,&due,0,NULL,NULL,FALSE))WaitForSingleObject(timer,INFINITE);
        else Sleep((DWORD)((remaining-spin)*1000/frequency.QuadPart));
    }
    do{YieldProcessor();QueryPerformanceCounter(&now);}while(now.QuadPart<next.QuadPart);
    next.QuadPart+=period;
}
#else
static void pc_cinematic_pace(void) {}
#endif
void pc_frontend_render(void *d,void *c,void *w,unsigned x,unsigned y) { if(render_callback)render_callback(d,c,w,x,y);pc_cinematic_pace(); }
int pc_frontend_message(void *w,unsigned m,uintptr_t p,intptr_t l) { return message_callback?message_callback(w,m,p,l):0; }
int pc_frontend_captured(void) { return pc_load(&captured); }
void pc_frontend_capture(int c) {
    c=c!=0;
    if(pc_exchange(&captured,c)!=c)pc_input_output_policy_changed();
}
int pc_setting_get(enum PcSetting k,int fallback) { int v=pc_load(&settings[k]);return v?v-1:fallback; }
void pc_setting_set(enum PcSetting k,int v) { pc_store(&settings[k],v+1); }
int pc_binding_get(enum PcBinding k,int fallback) { int v=pc_load(&bindings[k]);return v?v-1:fallback; }
void pc_binding_set(enum PcBinding k,int v) {
    if(pc_exchange(&bindings[k],v+1)!=v+1)pc_increment(&input_revision);
}
void pc_input_set_context(enum PcInputContext c) {
    /* The lobby's Escape-to-cursor mode must not consume the first gameplay
     * pause after entering a mission while its camera was captured. */
    if(c!=PC_INPUT_MENU)pc_store(&camera_capture,0);
    if(pc_exchange(&input_context,(int)c)!=(int)c) {
        pc_increment(&input_revision);pc_input_output_policy_changed();
    }
}
enum PcInputContext pc_input_context(void) { return (enum PcInputContext)pc_load(&input_context); }
static void pc_input_note(enum PcInputDevice d) {
    if(pc_exchange(&input_device,(int)d)!=(int)d) {
        pc_increment(&input_revision);pc_input_output_policy_changed();
    }
}
void pc_input_note_keyboard(void) { pc_input_note(PC_INPUT_KEYBOARD_MOUSE); }
enum PcInputDevice pc_input_device(void) { return (enum PcInputDevice)pc_load(&input_device); }
unsigned pc_input_revision(void) { return (unsigned)pc_load(&input_revision); }
void pc_input_note_gamepad(unsigned buttons,unsigned lt,unsigned rt,int lx,int ly,int rx,int ry)
{
    int values[6]={lt>30?(int)lt:0,rt>30?(int)rt:0,
        lx>7849||lx<-7849?lx:0,ly>7849||ly<-7849?ly:0,
        rx>8689||rx<-8689?rx:0,ry>8689||ry<-8689?ry:0};
    int active=(buttons & ~(unsigned)pc_exchange(&pad_buttons,(int)buttons))!=0;
    /* Connection, neutral reports, releases and a held stick never steal the
     * prompts back from a newly used mouse. Ignore small analog jitter. */
    for(int i=0;i<6;i++) {
        int previous=pc_load(&pad_axes[i]),delta=values[i]-previous;
        if(!values[i])pc_store(&pad_axes[i],0);
        else if(!previous || delta>(i<2?4:512) || delta<-(i<2?4:512)) {
            pc_store(&pad_axes[i],values[i]);active=1;
        }
    }
    if(active)pc_input_note(PC_INPUT_GAMEPAD);
}
