#include "settings.h"
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

Settings g_settings;

static const char* kDir  = "sdmc:/3ds/buckshot";
static const char* kFile = "sdmc:/3ds/buckshot/settings.bin";
static const u32   kMagic = 0x42434B52; // 'BCKR'
static const u32   kVersion = 2;

void settingsLoad() {
    FILE* f = fopen(kFile, "rb");
    if (!f) return;
    u32 magic = 0, ver = 0;
    fread(&magic, sizeof(magic), 1, f);
    fread(&ver,   sizeof(ver),   1, f);
    if (magic == kMagic && ver == kVersion) {
        Settings tmp;
        if (fread(&tmp, sizeof(Settings), 1, f) == 1)
            g_settings = tmp;
    }
    fclose(f);
}

void settingsSave() {
    mkdir("sdmc:/3ds", 0777);
    mkdir(kDir, 0777);
    FILE* f = fopen(kFile, "wb");
    if (!f) return;
    fwrite(&kMagic,    sizeof(kMagic),   1, f);
    fwrite(&kVersion,  sizeof(kVersion), 1, f);
    fwrite(&g_settings, sizeof(Settings), 1, f);
    fclose(f);
}

float settingsSfxGain() {
    if (g_settings.muted) return 0.f;
    return (g_settings.masterVolume / 100.f) * (g_settings.sfxVolume / 100.f);
}

float settingsMusicGain() {
    if (g_settings.muted) return 0.f;
    return (g_settings.masterVolume / 100.f) * (g_settings.musicVolume / 100.f);
}
