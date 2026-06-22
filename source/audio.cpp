#include "audio.h"
#include "settings.h"
#include <3ds.h>
#include <cstring>
#include <cmath>
#include <cstdlib>

#define SAMPLE_RATE 32000
#define SFX_CHANNELS 6            // round-robin channels for overlapping sfx
#define MUSIC_CHANNEL 7           // dedicated ambient channel

namespace {

struct Clip {
    s16*  data = nullptr;
    u32   frames = 0;
};

Clip        g_clips[SFX_COUNT];
ndspWaveBuf g_chanWb[SFX_CHANNELS]; // one wavebuf per channel (not per clip)
int         g_nextChan = 0;
bool        g_ready = false;

// ambient drone
s16*        g_music = nullptr;
u32         g_musicFrames = 0;
ndspWaveBuf g_musicWb;
bool        g_musicOn = false;
float       g_musicCur = 0.f;     // current ramped gain
float       g_musicTarget = 0.f;

inline float frand() { return (float)rand() / (float)RAND_MAX; }
inline float clampf(float v, float lo, float hi){ return v<lo?lo:(v>hi?hi:v); }

s16* alloc16(u32 frames) {
    s16* p = (s16*)linearAlloc(frames * sizeof(s16));
    if (p) memset(p, 0, frames * sizeof(s16));
    return p;
}

// ---- tiny synth primitives -------------------------------------------------
// All buffers are mono s16 @ SAMPLE_RATE.

void synthNoise(s16* buf, u32 n, float dur, float amp, float decay, float lp) {
    // band-limited-ish noise burst with a one-pole lowpass + exp decay
    float last = 0.f;
    for (u32 i = 0; i < n; ++i) {
        float t = (float)i / SAMPLE_RATE;
        if (t > dur) break;
        float env = expf(-t * decay);
        float white = frand() * 2.f - 1.f;
        last += (white - last) * lp;          // lowpass
        float s = last * env * amp;
        buf[i] = (s16)(clampf(s, -1.f, 1.f) * 32000);
    }
}

void synthTone(s16* buf, u32 n, float dur, float f0, float f1, float amp,
               float decay, int wave /*0 sine 1 square 2 saw*/) {
    float phase = 0.f;
    for (u32 i = 0; i < n; ++i) {
        float t = (float)i / SAMPLE_RATE;
        if (t > dur) break;
        float frac = (dur > 0) ? (t / dur) : 0.f;
        float f = f0 + (f1 - f0) * frac;
        phase += 2.f * (float)M_PI * f / SAMPLE_RATE;
        float v;
        switch (wave) {
            case 1:  v = (sinf(phase) >= 0 ? 1.f : -1.f); break;
            case 2:  v = 2.f * (phase/(2*M_PI) - floorf(0.5f + phase/(2*M_PI))); break;
            default: v = sinf(phase); break;
        }
        float env = expf(-t * decay);
        float s = v * env * amp;
        // soft attack to avoid clicks
        if (t < 0.004f) s *= t / 0.004f;
        buf[i] = (s16)(clampf(buf[i]/32000.f + s, -1.f, 1.f) * 32000);
    }
}

u32 durFrames(float sec) { return (u32)(sec * SAMPLE_RATE) + 1; }

void buildClip(Clip& c, float seconds) {
    c.frames = durFrames(seconds);
    c.data   = alloc16(c.frames);
}

void finishClip(Clip& c) {
    if (!c.data) return;
    DSP_FlushDataCache(c.data, c.frames * sizeof(s16));
}

void makeSfx() {
    // GUNSHOT (live): sharp noise crack + low body thump
    {
        Clip& c = g_clips[SFX_LIVE]; buildClip(c, 0.5f);
        synthNoise(c.data, c.frames, 0.45f, 1.0f, 14.f, 0.55f);
        synthTone (c.data, c.frames, 0.30f, 130.f, 45.f, 0.6f, 11.f, 0);
        finishClip(c);
    }
    // BLANK / dry click
    {
        Clip& c = g_clips[SFX_BLANK]; buildClip(c, 0.18f);
        synthNoise(c.data, c.frames, 0.06f, 0.7f, 70.f, 0.9f);
        synthTone (c.data, c.frames, 0.05f, 900.f, 300.f, 0.3f, 60.f, 1);
        finishClip(c);
    }
    // RELOAD: two mechanical chunks
    {
        Clip& c = g_clips[SFX_RELOAD]; buildClip(c, 0.34f);
        synthNoise(c.data, c.frames, 0.34f, 0.55f, 26.f, 0.7f);
        synthTone (c.data, c.frames, 0.30f, 200.f, 120.f, 0.35f, 16.f, 2);
        finishClip(c);
    }
    // UI MOVE
    {
        Clip& c = g_clips[SFX_MOVE]; buildClip(c, 0.06f);
        synthTone(c.data, c.frames, 0.05f, 520.f, 520.f, 0.32f, 30.f, 0);
        finishClip(c);
    }
    // UI SELECT
    {
        Clip& c = g_clips[SFX_SELECT]; buildClip(c, 0.14f);
        synthTone(c.data, c.frames, 0.12f, 440.f, 760.f, 0.35f, 16.f, 0);
        finishClip(c);
    }
    // UI BACK
    {
        Clip& c = g_clips[SFX_BACK]; buildClip(c, 0.14f);
        synthTone(c.data, c.frames, 0.12f, 540.f, 240.f, 0.32f, 16.f, 0);
        finishClip(c);
    }
    // ITEM generic
    {
        Clip& c = g_clips[SFX_ITEM]; buildClip(c, 0.12f);
        synthTone(c.data, c.frames, 0.10f, 620.f, 880.f, 0.30f, 22.f, 2);
        finishClip(c);
    }
    // HEAL: gentle rising chime
    {
        Clip& c = g_clips[SFX_HEAL]; buildClip(c, 0.4f);
        synthTone(c.data, c.frames, 0.38f, 523.f, 784.f, 0.30f, 6.f, 0);
        synthTone(c.data, c.frames, 0.38f, 659.f, 988.f, 0.18f, 6.f, 0);
        finishClip(c);
    }
    // HURT: harsh down buzz
    {
        Clip& c = g_clips[SFX_HURT]; buildClip(c, 0.3f);
        synthTone (c.data, c.frames, 0.28f, 220.f, 70.f, 0.5f, 10.f, 1);
        synthNoise(c.data, c.frames, 0.15f, 0.4f, 24.f, 0.6f);
        finishClip(c);
    }
    // BEER: rack/eject — woody clack + hiss
    {
        Clip& c = g_clips[SFX_BEER]; buildClip(c, 0.3f);
        synthNoise(c.data, c.frames, 0.28f, 0.5f, 18.f, 0.5f);
        synthTone (c.data, c.frames, 0.10f, 300.f, 160.f, 0.3f, 24.f, 2);
        finishClip(c);
    }
    // SAW: gritty back-and-forth
    {
        Clip& c = g_clips[SFX_SAW]; buildClip(c, 0.55f);
        for (u32 i=0;i<c.frames;i++){
            float t=(float)i/SAMPLE_RATE; if(t>0.5f)break;
            float lfo = 0.5f+0.5f*sinf(2*M_PI*9.f*t);
            float n = (frand()*2-1)*lfo;
            float env = (t<0.45f?1.f:0.f)*expf(-t*2.f);
            c.data[i]=(s16)(clampf(n*env*0.5f,-1,1)*32000);
        }
        finishClip(c);
    }
    // CUFF: metallic ratchet
    {
        Clip& c = g_clips[SFX_CUFF]; buildClip(c, 0.3f);
        for (u32 k=0;k<4;k++){
            u32 off=(u32)(k*0.05f*SAMPLE_RATE);
            for(u32 i=0;i<durFrames(0.03f) && off+i<c.frames;i++){
                float t=(float)i/SAMPLE_RATE;
                float s=(frand()*2-1)*expf(-t*120.f)*0.6f;
                c.data[off+i]=(s16)clampf(c.data[off+i]/32000.f+s,-1,1)*32000;
            }
        }
        finishClip(c);
    }
    // GLASS: soft high ping
    {
        Clip& c = g_clips[SFX_GLASS]; buildClip(c, 0.3f);
        synthTone(c.data, c.frames, 0.28f, 1200.f, 1500.f, 0.22f, 9.f, 0);
        finishClip(c);
    }
    // INVERT: warble
    {
        Clip& c = g_clips[SFX_INVERT]; buildClip(c, 0.35f);
        for(u32 i=0;i<c.frames;i++){
            float t=(float)i/SAMPLE_RATE; if(t>0.32f)break;
            float f=500.f+300.f*sinf(2*M_PI*12.f*t);
            float s=sinf(2*M_PI*f*t)*expf(-t*7.f)*0.3f;
            c.data[i]=(s16)(clampf(s,-1,1)*32000);
        }
        finishClip(c);
    }
    // PHONE: two-tone ring
    {
        Clip& c = g_clips[SFX_PHONE]; buildClip(c, 0.4f);
        synthTone(c.data, c.frames, 0.18f, 880.f, 880.f, 0.25f, 5.f, 1);
        finishClip(c);
    }
    // WIN: triad arpeggio
    {
        Clip& c = g_clips[SFX_WIN]; buildClip(c, 0.7f);
        synthTone(c.data, c.frames, 0.6f, 523.f, 523.f, 0.25f, 3.f, 0);
        synthTone(c.data, c.frames, 0.6f, 659.f, 659.f, 0.20f, 3.f, 0);
        synthTone(c.data, c.frames, 0.6f, 784.f, 784.f, 0.18f, 3.f, 0);
        finishClip(c);
    }
    // LOSE: descending dread
    {
        Clip& c = g_clips[SFX_LOSE]; buildClip(c, 0.9f);
        synthTone (c.data, c.frames, 0.85f, 200.f, 50.f, 0.4f, 2.5f, 2);
        synthNoise(c.data, c.frames, 0.4f, 0.25f, 6.f, 0.4f);
        finishClip(c);
    }
}

void makeMusic() {
    // Looping low ambient drone (a couple of detuned sines + slow noise wash).
    float loopSec = 6.0f;
    g_musicFrames = durFrames(loopSec);
    g_music = alloc16(g_musicFrames);
    for (u32 i = 0; i < g_musicFrames; ++i) {
        float t = (float)i / SAMPLE_RATE;
        float w = 0.f;
        w += sinf(2*M_PI*55.0f*t) * 0.30f;
        w += sinf(2*M_PI*55.4f*t) * 0.22f;          // slight detune -> beating
        w += sinf(2*M_PI*82.5f*t) * 0.14f;
        w += sinf(2*M_PI*1.0f*t)  * 0.0f;
        float swell = 0.6f + 0.4f * sinf(2*M_PI*(t/loopSec));
        w *= swell;
        g_music[i] = (s16)(clampf(w, -1.f, 1.f) * 24000);
    }
    // crossfade-friendly: taper the seam slightly
    u32 fade = durFrames(0.05f);
    for (u32 i = 0; i < fade && i < g_musicFrames; ++i) {
        float g = (float)i / fade;
        g_music[i] = (s16)(g_music[i] * g);
        g_music[g_musicFrames-1-i] = (s16)(g_music[g_musicFrames-1-i] * g);
    }
    DSP_FlushDataCache(g_music, g_musicFrames * sizeof(s16));
    memset(&g_musicWb, 0, sizeof(g_musicWb));
    g_musicWb.data_vaddr = g_music;
    g_musicWb.nsamples   = g_musicFrames;
    g_musicWb.looping    = true;
    g_musicWb.status     = NDSP_WBUF_DONE;
}

void configChannel(int ch, float rate) {
    ndspChnReset(ch);
    ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
    ndspChnSetRate(ch, rate);
    ndspChnSetFormat(ch, NDSP_FORMAT_MONO_PCM16);
}

} // namespace

void audioInit() {
    if (ndspInit() != 0) { g_ready = false; return; }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    srand(svcGetSystemTick() & 0xFFFFFFFF);

    memset(g_chanWb, 0, sizeof(g_chanWb));
    for (int i = 0; i < SFX_CHANNELS; ++i)
        configChannel(i, SAMPLE_RATE);
    configChannel(MUSIC_CHANNEL, SAMPLE_RATE);

    makeSfx();
    makeMusic();
    g_ready = true;
    audioApplyVolumes();
}

void audioExit() {
    if (!g_ready) return;
    for (int i = 0; i <= MUSIC_CHANNEL; ++i) ndspChnWaveBufClear(i);
    ndspExit();
    for (int i = 0; i < SFX_COUNT; ++i)
        if (g_clips[i].data) linearFree(g_clips[i].data);
    if (g_music) linearFree(g_music);
    g_ready = false;
}

void audioApplyVolumes() {
    if (!g_ready) return;
    float sfx = settingsSfxGain();
    float mix[12]; memset(mix, 0, sizeof(mix));
    mix[0] = mix[1] = sfx;
    for (int i = 0; i < SFX_CHANNELS; ++i) ndspChnSetMix(i, mix);
    // music gain is ramped in audioUpdate
}

void audioPlay(Sfx s) {
    if (!g_ready || s < 0 || s >= SFX_COUNT) return;
    if (settingsSfxGain() <= 0.001f) return;
    Clip& c = g_clips[s];
    if (!c.data) return;
    int ch = g_nextChan;
    g_nextChan = (g_nextChan + 1) % SFX_CHANNELS;
    ndspChnWaveBufClear(ch);
    ndspWaveBuf& wb = g_chanWb[ch];
    memset(&wb, 0, sizeof(wb));
    wb.data_vaddr = c.data;
    wb.nsamples   = c.frames;
    wb.looping    = false;
    wb.status     = NDSP_WBUF_DONE;
    ndspChnWaveBufAdd(ch, &wb);
}

void audioSetMusic(bool on) {
    if (!g_ready) return;
    g_musicOn = on;
    g_musicTarget = on ? settingsMusicGain() : 0.f;
    if (on && g_musicWb.status != NDSP_WBUF_QUEUED && g_musicWb.status != NDSP_WBUF_PLAYING) {
        g_musicWb.status = NDSP_WBUF_DONE;
        ndspChnWaveBufAdd(MUSIC_CHANNEL, &g_musicWb);
    }
}

void audioUpdate() {
    if (!g_ready) return;
    g_musicTarget = g_musicOn ? settingsMusicGain() : 0.f;
    // smooth ramp to avoid pops on volume changes
    g_musicCur += (g_musicTarget - g_musicCur) * 0.08f;
    float mix[12]; memset(mix, 0, sizeof(mix));
    mix[0] = mix[1] = g_musicCur;
    ndspChnSetMix(MUSIC_CHANNEL, mix);
}
