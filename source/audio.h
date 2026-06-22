#pragma once

// Procedural audio: every sound is synthesized at runtime into PCM buffers
// and played through the 3DS DSP (ndsp). No external audio files required.

enum Sfx {
    SFX_MOVE = 0,   // ui cursor move
    SFX_SELECT,     // ui confirm
    SFX_BACK,       // ui cancel
    SFX_LIVE,       // shotgun fires a live round
    SFX_BLANK,      // shotgun dry-fires a blank
    SFX_RELOAD,     // shells loaded
    SFX_ITEM,       // generic item use
    SFX_HEAL,       // cigarettes / medicine heal
    SFX_HURT,       // take damage
    SFX_BEER,       // racking / ejecting a shell
    SFX_SAW,        // sawing the barrel
    SFX_CUFF,       // handcuffs
    SFX_GLASS,      // magnifying glass
    SFX_INVERT,     // inverter
    SFX_PHONE,      // burner phone
    SFX_WIN,        // round / game won
    SFX_LOSE,       // game over
    SFX_RING,       // tinnitus ring after taking a live round
    SFX_HEART,      // heartbeat thump
    SFX_SCREECH,    // the Dealer's scowl/jumpscare screech
    SFX_COUNT
};

void audioInit();
void audioExit();
void audioPlay(Sfx s);
void audioSetMusic(bool on);     // enable/disable ambient drone
void audioApplyVolumes();        // re-apply gains from settings
void audioUpdate();              // call once per frame (music level ramps)
