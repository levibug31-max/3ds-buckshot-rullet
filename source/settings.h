#pragma once
#include <3ds.h>

// Persisted user configuration. Saved to the SD card so it survives reboots.
struct Settings {
    // Audio
    int  masterVolume = 80;   // 0..100
    int  sfxVolume    = 90;   // 0..100
    int  musicVolume  = 55;   // 0..100
    bool muted        = false;

    // Graphics
    bool scanlines    = true;  // CRT scanline overlay (top screen)
    bool vignette     = true;  // darkened corners
    bool screenShake  = true;  // camera kick on gunshots
    bool muzzleFlash  = true;  // flash + particles on fire
    int  brightness   = 100;   // 40..100, in-app dimming overlay

    // Video
    bool showFps      = false; // on-screen frame counter
    bool smoothAnim   = true;  // eased transitions / animations
    int  gameSpeed    = 100;   // 70..130 percent, animation pacing

    // Gameplay
    int  difficulty   = 1;     // 0 Calm, 1 Standard, 2 Ruthless
};

extern Settings g_settings;

void settingsLoad();
void settingsSave();

// Effective 0..1 volume multipliers (respecting mute + master).
float settingsSfxGain();
float settingsMusicGain();
