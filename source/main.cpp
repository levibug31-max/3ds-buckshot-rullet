#include <3ds.h>
#include <citro2d.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>

#include "render.h"
#include "audio.h"
#include "settings.h"
#include "game.h"

// ===========================================================================
//  Buckshot Roulette — 3DS homebrew (original implementation)
// ===========================================================================

#define TICKS_PER_SEC 268111856.0

enum AppState { ST_SPLASH, ST_MENU, ST_HOWTO, ST_SETTINGS, ST_GAME, ST_GAMEOVER };

// ---- effects state ----
struct Particle { float x,y,vx,vy,life,max; u32 col; };
struct Fx {
    float shake=0, flash=0, dealerHurt=0, playerHurt=0, recoil=0, eyeGlow=0;
    std::vector<Particle> parts;
};

static C3D_RenderTarget* g_top;
static C3D_RenderTarget* g_bot;
static Fx   fx;
static Game game;

static AppState state = ST_SPLASH;
static float splashT = 0.f;
static float fpsVal = 60.f;

// menu
static int   menuSel = 0;
static const char* kMenuItems[] = { "NEW GAME", "HOW TO PLAY", "SETTINGS", "QUIT" };
static const int   kMenuCount = 4;

// howto scroll
static float howScroll = 0.f;

// settings
static int setSel = 0;

// game ui
static int   uiSel = 0;            // 0..7 items, 8 shoot dealer, 9 shoot self
static float bannerT = 0.f;
static char  banner[96] = "";
static float dealerTimer = 0.f;
static Actor prevTurn = ACTOR_PLAYER;
static int   prevRound = 1;
static float roundBannerT = 0.f;
static float loadBannerT = 0.f;
static float overDelay = 0.f;
// adrenaline targeting
static bool  adrenMode = false;
static int   adrenSel = 0;

// ---------------------------------------------------------------------------
static float frand(){ return (float)rand()/(float)RAND_MAX; }

static void setBanner(const char* s){ strncpy(banner, s, sizeof(banner)-1); banner[sizeof(banner)-1]=0; bannerT=1.6f; }

static void spawnSparks(float x, float y, int n, u32 col){
    if (!g_settings.muzzleFlash) return;
    for (int i=0;i<n;i++){
        Particle p;
        float a = frand()*6.2831853f;
        float sp = 40.f + frand()*160.f;
        p.x=x; p.y=y; p.vx=cosf(a)*sp; p.vy=sinf(a)*sp - 20.f;
        p.max=p.life=0.25f+frand()*0.35f; p.col=col;
        fx.parts.push_back(p);
    }
}

static void applyShotFx(const ActionResult& r, Actor target){
    fx.recoil = 1.0f;
    if (r.fired && r.wasLive){
        if (g_settings.muzzleFlash) fx.flash = 1.0f;
        if (g_settings.screenShake) fx.shake = 9.0f;
        if (target==ACTOR_DEALER) fx.dealerHurt = 1.0f; else fx.playerHurt = 1.0f;
        spawnSparks(280, 150, 22, Pal::amber);
        spawnSparks(280, 150, 10, Pal::red);
    } else if (r.fired){
        if (g_settings.muzzleFlash) fx.flash = 0.35f;
        if (g_settings.screenShake) fx.shake = 3.0f;
        spawnSparks(280, 150, 6, Pal::textDim);
    }
}

static void updateFx(float dt){
    float decay = expf(-dt*6.f);
    fx.shake     *= decay;
    fx.flash     *= expf(-dt*7.f);
    fx.recoil    *= expf(-dt*9.f);
    fx.dealerHurt*= expf(-dt*4.f);
    fx.playerHurt*= expf(-dt*4.f);
    fx.eyeGlow = 0.5f + 0.5f*sinf((float)svcGetSystemTick()/TICKS_PER_SEC*2.0f);
    for (size_t i=0;i<fx.parts.size();){
        Particle& p = fx.parts[i];
        p.life -= dt;
        if (p.life<=0){ fx.parts[i]=fx.parts.back(); fx.parts.pop_back(); continue; }
        p.vy += 240.f*dt;            // gravity
        p.x += p.vx*dt; p.y += p.vy*dt;
        ++i;
    }
}

// ---------------------------------------------------------------------------
//  drawing primitives for the scene
// ---------------------------------------------------------------------------
static void drawShellIcon(float x, float y, float w, float h, int type /*-1 unknown,0 blank,1 live*/){
    u32 brass = Pal::brass;
    u32 body  = (type==1)? Pal::red : (type==0)? Pal::steel : C2D_Color32(0x4a,0x42,0x44,0xFF);
    float bh = h*0.34f;
    RS(x, y, w, h-bh, body);                 // plastic
    RS(x, y+h-bh, w, bh, brass);             // brass
    RS(x, y, w, 2, C2D_Color32(0,0,0,90));   // crimp shadow
    if (type<0) drawText(x+w*0.5f, y+h*0.2f, 0.5f, Pal::text, ALN_C, "?");
}

static void drawHealthPips(float x, float y, int lives, int maxLives, u32 col, float flash){
    float r=5.f, gap=14.f;
    for (int i=0;i<maxLives;i++){
        float cx = x + i*gap;
        bool on = i < lives;
        u32 c = on ? col : C2D_Color32(0x33,0x2a,0x2c,0xFF);
        if (on && flash>0.01f)
            c = C2D_Color32(0xff, (u8)(0x40+0x80*(1-flash)), 0x40, 0xFF);
        C2D_DrawCircleSolid(cx, y, 0.5f, r, c);
        C2D_DrawCircleSolid(cx, y, 0.6f, r-2.f, C2D_Color32(0,0,0,60));
        if (on) C2D_DrawCircleSolid(cx, y, 0.7f, r-2.5f, c);
    }
}

static void drawDealer(float cx, float cy, float hurt){
    // chair shadow
    C2D_DrawEllipse(cx-46, cy+44, 0.1f, 92, 18, C2D_Color32(0,0,0,90),C2D_Color32(0,0,0,90),C2D_Color32(0,0,0,0),C2D_Color32(0,0,0,0));
    // torso
    u32 coat = C2D_Color32(0x1a,0x15,0x18,0xFF);
    drawPanel(cx-38, cy-6, 76, 58, coat, Pal::line);
    // shoulders
    C2D_DrawTriangle(cx-38, cy+8, coat, cx-58, cy+44, coat, cx-30, cy+44, coat, 0.2f);
    C2D_DrawTriangle(cx+38, cy+8, coat, cx+58, cy+44, coat, cx+30, cy+44, coat, 0.2f);
    // neck
    RS(cx-7, cy-14, 14, 12, C2D_Color32(0x12,0x10,0x12,0xFF));
    // head
    C2D_DrawCircleSolid(cx, cy-26, 0.3f, 20, C2D_Color32(0x14,0x12,0x14,0xFF));
    // hat brim + top
    RS(cx-26, cy-40, 52, 5, Pal::black);
    RS(cx-16, cy-58, 32, 20, C2D_Color32(0x0c,0x0a,0x0c,0xFF));
    // glowing eyes
    float g = 0.45f + 0.55f*fx.eyeGlow;
    u32 eye = C2D_Color32((u8)(0xc0+0x30*g),(u8)(0x20*g),(u8)(0x20*g),0xFF);
    C2D_DrawCircleSolid(cx-8, cy-26, 0.4f, 3.2f, eye);
    C2D_DrawCircleSolid(cx+8, cy-26, 0.4f, 3.2f, eye);
    // hurt flash overlay
    if (hurt>0.01f)
        RS(cx-58, cy-60, 116, 116, C2D_Color32(0xc8,0x20,0x20,(u8)(120*hurt)));
}

static void drawShotgun(float cx, float cy, float recoil, float flash){
    float ox = -recoil*10.f;
    cx += ox;
    u32 steel = C2D_Color32(0x6a,0x6c,0x72,0xFF);
    u32 steelD= C2D_Color32(0x3c,0x3e,0x44,0xFF);
    u32 wood  = C2D_Color32(0x55,0x36,0x20,0xFF);
    // stock
    C2D_DrawTriangle(cx-92,cy-6,wood, cx-92,cy+14,wood, cx-58,cy+10,wood, 0.2f);
    RS(cx-66, cy-8, 30, 18, wood);
    // receiver
    RS(cx-40, cy-9, 34, 20, steelD);
    // barrel
    RS(cx-8, cy-7, 96, 12, steel);
    RS(cx-8, cy-7, 96, 3, C2D_Color32(0x9a,0x9c,0xa2,0xFF));
    RS(cx-8, cy+2, 96, 3, steelD);
    // pump
    RS(cx+6, cy+5, 28, 8, C2D_Color32(0x2a,0x1c,0x12,0xFF));
    // muzzle
    RS(cx+86, cy-9, 6, 16, steelD);
    // muzzle flash
    if (flash>0.02f){
        float s = flash;
        u32 f1 = C2D_Color32(0xff,0xe0,0x80,(u8)(220*s));
        u32 f2 = C2D_Color32(0xff,0x90,0x30,(u8)(160*s));
        C2D_DrawCircleSolid(cx+96, cy-1, 0.9f, 10*s+4, f2);
        C2D_DrawCircleSolid(cx+96, cy-1, 0.95f, 6*s+2, f1);
        C2D_DrawTriangle(cx+92,cy-8,f1, cx+92,cy+6,f1, cx+96+26*s,cy-1,C2D_Color32(0xff,0xc0,0x40,0),0.9f);
    }
}

// ---------------------------------------------------------------------------
//  TOP SCREEN
// ---------------------------------------------------------------------------
static void renderTopGame(){
    float sx=0, sy=0;
    if (fx.shake>0.3f){ sx=(frand()*2-1)*fx.shake; sy=(frand()*2-1)*fx.shake; }

    // background
    C2D_DrawRectangle(0,0,0, TOP_W, SCR_H, Pal::bg0,Pal::bg0,Pal::bg1,Pal::bg1);
    // table
    C2D_DrawRectangle(0,150+sy,0.05f, TOP_W, 90, C2D_Color32(0x2a,0x16,0x12,0xFF),C2D_Color32(0x2a,0x16,0x12,0xFF),C2D_Color32(0x15,0x0b,0x09,0xFF),C2D_Color32(0x15,0x0b,0x09,0xFF));
    RS(0,150+sy,TOP_W,2, C2D_Color32(0x55,0x2c,0x18,0xFF));

    // header
    drawText(8, 6, 0.5f, Pal::textDim, ALN_L, "ROUND %d / %d", game.round, game.maxRounds);
    const char* turnTxt = game.over ? "" : (game.turn==ACTOR_PLAYER ? "YOUR TURN" : "DEALER'S TURN");
    drawText(TOP_W/2.0f, 6, 0.62f, game.turn==ACTOR_PLAYER?Pal::green:Pal::red, ALN_C, "%s", turnTxt);
    if (g_settings.showFps) drawText(TOP_W-8, 6, 0.45f, Pal::textMute, ALN_R, "%.0f fps", fpsVal);

    // dealer
    drawDealer(TOP_W/2.0f + sx, 78+sy, fx.dealerHurt);
    drawText(TOP_W/2.0f+sx, 110+sy, 0.45f, Pal::textDim, ALN_C, "THE DEALER");
    drawHealthPips(TOP_W/2.0f - (game.dealer.maxLives-1)*7.f + sx, 124+sy,
                   game.dealer.lives, game.dealer.maxLives, Pal::red, fx.dealerHurt);

    // shotgun on the table
    drawShotgun(TOP_W/2.0f + sx, 168+sy, fx.recoil, fx.flash);
    for (auto& p : fx.parts){
        float a = p.life/p.max;
        u8 al = (u8)(255*a);
        u32 c = (p.col & 0x00FFFFFF) | (al<<24);
        C2D_DrawCircleSolid(p.x+sx, p.y+sy, 0.9f, 1.6f*a+0.6f, c);
    }

    // saw / cuff indicators
    float iy=196+sy;
    if (game.sawActive)      { drawText(10, iy, 0.42f, Pal::amber, ALN_L, "SAWED x2"); }
    if (game.dealer.cuffed)  { drawText(TOP_W/2.0f, 128+sy, 0.4f, Pal::amber, ALN_C, "[CUFFED]"); }
    if (game.player.cuffed)  { drawText(TOP_W/2.0f, 224+sy, 0.4f, Pal::amber, ALN_C, "[CUFFED]"); }

    // player health
    drawText(8, 224+sy, 0.45f, Pal::textDim, ALN_L, "YOU");
    drawHealthPips(40+sx, 228+sy, game.player.lives, game.player.maxLives, Pal::green, fx.playerHurt);

    // chamber / shell info panel (right)
    float px=TOP_W-118, py=150+sy;
    drawPanel(px, py-2, 116, 56, C2D_Color32(0x18,0x12,0x14,0xCC), Pal::line);
    drawText(px+8, py+2, 0.42f, Pal::textDim, ALN_L, "CHAMBER");
    drawText(px+8, py+16, 0.46f, Pal::red,   ALN_L, "LIVE  %d", game.announcedLive);
    drawText(px+8, py+30, 0.46f, Pal::steel, ALN_L, "BLANK %d", game.announcedBlank);
    int knowCur = playerKnow(game, (int)game.pos);
    if (knowCur>=0)
        drawText(px+108, py+16, 0.44f, knowCur==1?Pal::red:Pal::steel, ALN_R, "NEXT:%s", knowCur==1?"LIVE":"BLANK");

    // remaining shells as a row of icons
    int left = shellsLeft(game);
    int show = left>8?8:left;
    float bx=8, by=158+sy;
    for (int i=0;i<show;i++){
        int kn = playerKnow(game, (int)game.pos+i);
        drawShellIcon(bx+i*16, by, 11, 26, (i==0 && knowCur>=0)?knowCur:(kn>=0?kn:-1));
    }
    if (left>8) drawText(8+8*16+2, by+8, 0.4f, Pal::textDim, ALN_L, "+%d", left-8);

    // banners
    if (loadBannerT>0){
        float a = loadBannerT>1?1:loadBannerT;
        RS(0,70, TOP_W,40, C2D_Color32(0,0,0,(u8)(150*a)));
        drawText(TOP_W/2.f, 80, 0.7f, Pal::text, ALN_C, "%d LIVE   %d BLANK", game.announcedLive, game.announcedBlank);
    }
    if (roundBannerT>0){
        float a = roundBannerT>1?1:roundBannerT;
        RS(0,96, TOP_W,48, C2D_Color32(0,0,0,(u8)(180*a)));
        drawText(TOP_W/2.f, 104, 0.95f, Pal::red, ALN_C, "ROUND %d", game.round);
        drawText(TOP_W/2.f, 126, 0.45f, Pal::textDim, ALN_C, "%d lives each", game.player.maxLives);
    }
    if (bannerT>0 && game.turn==ACTOR_DEALER){
        drawText(TOP_W/2.f, 142+sy, 0.46f, Pal::textDim, ALN_C, "The Dealer %s...", banner);
    }

    if (g_settings.vignette)  drawVignette(TOP_W, SCR_H, 1.0f);
    if (fx.flash>0.02f)       drawDim(TOP_W, SCR_H, -0.0f), RS(0,0,TOP_W,SCR_H, C2D_Color32(0xff,0xe8,0xc0,(u8)(70*fx.flash)));
    if (g_settings.scanlines) drawScanlines(TOP_W, SCR_H, 0.10f);
    drawDim(TOP_W, SCR_H, (100-g_settings.brightness)/100.f*0.6f);
}

// ---------------------------------------------------------------------------
//  BOTTOM SCREEN — controls
// ---------------------------------------------------------------------------
static const float ITEM_X=6, ITEM_Y=30, ITEM_W=48, ITEM_H=40, ITEM_GX=50, ITEM_GY=44;
static const float BTN_X=212, BTN_W=102, BTN_SH=44;

static void itemSlotRect(int i, float& x, float& y){
    int col=i%4, row=i/4;
    x=ITEM_X+col*ITEM_GX; y=ITEM_Y+row*ITEM_GY;
}

static void renderBottomGame(){
    C2D_DrawRectangle(0,0,0, BOT_W, SCR_H, Pal::bg0,Pal::bg0,Pal::bg1,Pal::bg1);

    // header / hint
    if (game.turn==ACTOR_PLAYER && !game.over)
        drawText(6,6,0.5f, Pal::green, ALN_L, "YOUR MOVE");
    else
        drawText(6,6,0.5f, Pal::textMute, ALN_L, "WAIT...");

    // selected item description
    Item descIt = IT_NONE;
    if (uiSel<8 && uiSel < (int)game.player.items.size()) descIt = game.player.items[uiSel];
    if (descIt!=IT_NONE)
        drawText(BOT_W-6,6,0.42f, Pal::textDim, ALN_R, "%s", itemName(descIt));

    // items panel
    drawPanel(ITEM_X-4, ITEM_Y-4, 4*ITEM_GX, 2*ITEM_GY+2, C2D_Color32(0x16,0x12,0x14,0xFF), Pal::line);
    for (int i=0;i<8;i++){
        float x,y; itemSlotRect(i,x,y);
        bool has = i < (int)game.player.items.size();
        bool sel = (uiSel==i && !adrenMode);
        u32 fill = sel? Pal::panelHi : Pal::panel;
        drawPanel(x,y,ITEM_W,ITEM_H, fill, sel?Pal::brass:Pal::line);
        if (has){
            Item it=game.player.items[i];
            drawText(x+ITEM_W/2, y+8, 0.42f, Pal::text, ALN_C, "%s", itemShort(it));
            drawText(x+ITEM_W/2, y+24, 0.34f, Pal::textMute, ALN_C, "%s", it==IT_SAW?"x2": it==IT_CIGS?"+1":"");
        } else {
            drawText(x+ITEM_W/2, y+14, 0.5f, Pal::textMute, ALN_C, "-");
        }
    }

    // action buttons
    bool selD = (uiSel==8 && !adrenMode);
    bool selS = (uiSel==9 && !adrenMode);
    drawPanel(BTN_X, ITEM_Y, BTN_W, BTN_SH, selD?C2D_Color32(0x6a,0x18,0x18,0xFF):Pal::redDim, selD?Pal::white:Pal::red);
    drawText(BTN_X+BTN_W/2, ITEM_Y+8, 0.52f, Pal::text, ALN_C, "SHOOT");
    drawText(BTN_X+BTN_W/2, ITEM_Y+24, 0.46f, Pal::text, ALN_C, "DEALER");
    drawPanel(BTN_X, ITEM_Y+BTN_SH+6, BTN_W, BTN_SH, selS?Pal::panelHi:Pal::panel, selS?Pal::white:Pal::line);
    drawText(BTN_X+BTN_W/2, ITEM_Y+BTN_SH+14, 0.52f, Pal::text, ALN_C, "SHOOT");
    drawText(BTN_X+BTN_W/2, ITEM_Y+BTN_SH+30, 0.46f, Pal::textDim, ALN_C, "SELF");

    // description / log panel
    float ly=126;
    drawPanel(6, ly, BOT_W-12, SCR_H-ly-6, C2D_Color32(0x12,0x0f,0x10,0xFF), Pal::line);
    if (descIt!=IT_NONE)
        drawText(12, ly+6, 0.40f, Pal::amber, ALN_L, "%s", itemDesc(descIt));
    else if (uiSel==8) drawText(12, ly+6, 0.40f, Pal::amber, ALN_L, "Fire at the Dealer. Ends your turn.");
    else if (uiSel==9) drawText(12, ly+6, 0.40f, Pal::amber, ALN_L, "Fire at yourself. A blank keeps your turn.");

    // last log lines
    int n=(int)game.log.size();
    int lines=4;
    for (int i=0;i<lines;i++){
        int idx=n-lines+i;
        if (idx<0) continue;
        u32 c = (i==lines-1)?Pal::text:Pal::textMute;
        drawText(12, ly+24+i*16, 0.38f, c, ALN_L, "%s", game.log[idx].c_str());
    }

    // controls hint
    drawText(BOT_W/2, SCR_H-2, 0.34f, Pal::textMute, ALN_C, "");

    // adrenaline overlay
    if (adrenMode){
        RS(0,0,BOT_W,SCR_H, C2D_Color32(0,0,0,180));
        drawText(BOT_W/2, 30, 0.6f, Pal::amber, ALN_C, "STEAL AN ITEM");
        int n=(int)game.dealer.items.size();
        for (int i=0;i<n;i++){
            float x=20+(i%5)*58, y=70+(i/5)*54;
            bool sel=(adrenSel==i);
            Item it=game.dealer.items[i];
            bool ok = it!=IT_ADREN && canUseItem(game, ACTOR_PLAYER, it);
            drawPanel(x,y,52,46, sel?Pal::panelHi:Pal::panel, sel?Pal::brass:Pal::line);
            drawText(x+26,y+14,0.4f, ok?Pal::text:Pal::textMute, ALN_C, "%s", itemShort(it));
            if (!ok) drawText(x+26,y+28,0.3f,Pal::redDim,ALN_C,"x");
        }
        drawText(BOT_W/2, SCR_H-20, 0.4f, Pal::textDim, ALN_C, "A: take   B: cancel");
    }

    if (g_settings.vignette) drawVignette(BOT_W, SCR_H, 0.7f);
    drawDim(BOT_W, SCR_H, (100-g_settings.brightness)/100.f*0.6f);
}

// ---------------------------------------------------------------------------
//  MENUS
// ---------------------------------------------------------------------------
static void renderSplash(){
    float a = splashT<1?splashT:1;
    C2D_DrawRectangle(0,0,0, TOP_W, SCR_H, Pal::bg0,Pal::bg0,Pal::bg1,Pal::bg1);
    drawShellIcon(TOP_W/2-40, 86, 22, 52, 1);
    drawShellIcon(TOP_W/2+18, 86, 22, 52, 0);
    drawText(TOP_W/2, 40, 1.2f, C2D_Color32(0xe8,0xe0,0xd8,(u8)(255*a)), ALN_C, "BUCKSHOT");
    drawText(TOP_W/2, 150, 1.2f, C2D_Color32(0xc8,0x2c,0x2c,(u8)(255*a)), ALN_C, "ROULETTE");
    if (fmodf(splashT,1.2f) < 0.7f)
        drawText(TOP_W/2, 200, 0.5f, Pal::textDim, ALN_C, "PRESS  A  OR  START");
    if (g_settings.vignette) drawVignette(TOP_W,SCR_H,1.0f);
    if (g_settings.scanlines) drawScanlines(TOP_W,SCR_H,0.10f);
}

static void renderMenu(){
    C2D_DrawRectangle(0,0,0, TOP_W, SCR_H, Pal::bg0,Pal::bg0,Pal::bg1,Pal::bg1);
    drawText(TOP_W/2, 24, 0.7f, Pal::text, ALN_C, "BUCKSHOT ROULETTE");
    drawText(TOP_W/2, 50, 0.4f, Pal::textMute, ALN_C, "a duel against the Dealer");
    for (int i=0;i<kMenuCount;i++){
        float y=92+i*30;
        bool sel=(i==menuSel);
        if (sel){ drawPanel(TOP_W/2-90, y-4, 180, 26, Pal::panelHi, Pal::brass);
                  drawText(TOP_W/2-78, y, 0.55f, Pal::brass, ALN_L, ">"); }
        drawText(TOP_W/2, y, 0.55f, sel?Pal::text:Pal::textDim, ALN_C, "%s", kMenuItems[i]);
    }
    drawText(TOP_W/2, SCR_H-16, 0.36f, Pal::textMute, ALN_C, "Original homebrew port");
    if (g_settings.vignette) drawVignette(TOP_W,SCR_H,1.0f);
    if (g_settings.scanlines) drawScanlines(TOP_W,SCR_H,0.10f);
}

static void renderHowto(){
    C2D_DrawRectangle(0,0,0, TOP_W, SCR_H, Pal::bg0,Pal::bg0,Pal::bg1,Pal::bg1);
    static const char* lines[] = {
        "HOW TO PLAY",
        "",
        "The shotgun is loaded with LIVE and BLANK",
        "shells in a secret order. The counts are",
        "announced; the order is not.",
        "",
        "On your turn, use items then fire at the",
        "DEALER or at YOURSELF.",
        " - Shoot yourself with a BLANK: keep turn.",
        " - Shoot yourself with a LIVE : take damage.",
        " - Shoot the Dealer: turn passes either way.",
        "",
        "Lose all your lives and it's over. Drop the",
        "Dealer to win the round. Survive 3 rounds.",
        "",
        "ITEMS",
        " GLASS  - reveal the chambered shell",
        " CIGS   - restore 1 life",
        " BEER   - eject the chambered shell",
        " CUFFS  - opponent skips a turn",
        " SAW    - next shot deals x2",
        " ADREN  - steal & use an opponent item",
        " PHONE  - learn an upcoming shell",
        " INVERT - flip the chambered shell",
        " MEDS   - 50%: +2 life, else -1 life",
        "",
        "CONTROLS",
        " D-Pad / touch : select    A : use/fire",
        " X : shoot Dealer   Y : shoot self",
        " B : back",
    };
    int count = sizeof(lines)/sizeof(lines[0]);
    for (int i=0;i<count;i++){
        float y = 14 + i*15 - howScroll;
        if (y< -16 || y>SCR_H) continue;
        u32 c = (i==0)?Pal::red : (lines[i][0]==' '?Pal::textDim:Pal::text);
        float sc = (i==0)?0.6f:0.42f;
        drawText(16, y, sc, c, ALN_L, "%s", lines[i]);
    }
    drawText(TOP_W-8, SCR_H-14, 0.36f, Pal::textMute, ALN_R, "Up/Down scroll  B back");
    if (g_settings.vignette) drawVignette(TOP_W,SCR_H,1.0f);
}

// ---- settings model ----
enum SetKind { S_TOGGLE, S_SLIDER, S_CHOICE };
struct SetItem { const char* label; SetKind kind; void* val; int lo, hi, step; const char** choices; const char* group; };

static const char* diffNames[] = {"Calm","Standard","Ruthless"};

static std::vector<SetItem> buildSettings(){
    std::vector<SetItem> v;
    v.push_back({"-- AUDIO --", S_TOGGLE, nullptr,0,0,0,nullptr,"h"});
    v.push_back({"Mute All",     S_TOGGLE,&g_settings.muted,0,1,1,nullptr,""});
    v.push_back({"Master Volume",S_SLIDER,&g_settings.masterVolume,0,100,10,nullptr,""});
    v.push_back({"SFX Volume",   S_SLIDER,&g_settings.sfxVolume,0,100,10,nullptr,""});
    v.push_back({"Music Volume", S_SLIDER,&g_settings.musicVolume,0,100,10,nullptr,""});
    v.push_back({"-- GRAPHICS --",S_TOGGLE,nullptr,0,0,0,nullptr,"h"});
    v.push_back({"Scanline CRT", S_TOGGLE,&g_settings.scanlines,0,1,1,nullptr,""});
    v.push_back({"Vignette",     S_TOGGLE,&g_settings.vignette,0,1,1,nullptr,""});
    v.push_back({"Screen Shake", S_TOGGLE,&g_settings.screenShake,0,1,1,nullptr,""});
    v.push_back({"Muzzle Flash", S_TOGGLE,&g_settings.muzzleFlash,0,1,1,nullptr,""});
    v.push_back({"Brightness",   S_SLIDER,&g_settings.brightness,40,100,10,nullptr,""});
    v.push_back({"-- VIDEO --",  S_TOGGLE,nullptr,0,0,0,nullptr,"h"});
    v.push_back({"Show FPS",     S_TOGGLE,&g_settings.showFps,0,1,1,nullptr,""});
    v.push_back({"Smooth Anim",  S_TOGGLE,&g_settings.smoothAnim,0,1,1,nullptr,""});
    v.push_back({"Game Speed",   S_SLIDER,&g_settings.gameSpeed,70,130,10,nullptr,""});
    v.push_back({"-- GAMEPLAY --",S_TOGGLE,nullptr,0,0,0,nullptr,"h"});
    v.push_back({"Dealer AI",    S_CHOICE,&g_settings.difficulty,0,2,1,diffNames,""});
    return v;
}
static std::vector<SetItem> g_set;

static void renderSettings(){
    C2D_DrawRectangle(0,0,0, TOP_W, SCR_H, Pal::bg0,Pal::bg0,Pal::bg1,Pal::bg1);
    drawText(TOP_W/2, 8, 0.6f, Pal::text, ALN_C, "SETTINGS");
    float top=34, rh=13;
    // keep selection visible
    int total=(int)g_set.size();
    float viewH = SCR_H-50;
    int maxRows = (int)(viewH/rh);
    int first = setSel - maxRows/2; if (first<0) first=0;
    if (first> total-maxRows) first = total-maxRows; if (first<0) first=0;
    for (int i=first;i<total && (i-first)<maxRows;i++){
        SetItem& s=g_set[i];
        float y=top+(i-first)*rh;
        bool isHead = (s.group && s.group[0]=='h');
        bool sel=(i==setSel);
        if (sel && !isHead){ RS(8,y-1,TOP_W-16,rh, Pal::panelHi); }
        if (isHead){ drawText(14,y,0.42f,Pal::brass,ALN_L,"%s",s.label); continue; }
        drawText(18,y,0.42f, sel?Pal::text:Pal::textDim, ALN_L,"%s",s.label);
        char vbuf[32];
        if (s.kind==S_TOGGLE){ snprintf(vbuf,sizeof(vbuf), *(bool*)s.val?"ON":"OFF"); }
        else if (s.kind==S_SLIDER){ snprintf(vbuf,sizeof(vbuf), "%d", *(int*)s.val); }
        else { snprintf(vbuf,sizeof(vbuf), "%s", s.choices[*(int*)s.val]); }
        // slider bar
        if (s.kind==S_SLIDER){
            float bx=TOP_W-150, bw=90;
            float fr=(float)(*(int*)s.val - s.lo)/(s.hi-s.lo);
            drawBar(bx,y+2,bw,7,fr, Pal::red, Pal::panel);
        }
        drawText(TOP_W-14,y,0.42f, sel?Pal::amber:Pal::textDim, ALN_R,"%s",vbuf);
    }
    drawText(TOP_W/2, SCR_H-12, 0.36f, Pal::textMute, ALN_C, "L/R or Left/Right adjust   B: save & back");
    if (g_settings.vignette) drawVignette(TOP_W,SCR_H,1.0f);
    if (g_settings.scanlines) drawScanlines(TOP_W,SCR_H,0.10f);
}

static void renderGameOver(){
    C2D_DrawRectangle(0,0,0, TOP_W, SCR_H, Pal::bg0,Pal::bg0,Pal::bg1,Pal::bg1);
    if (game.playerWon){
        drawText(TOP_W/2, 70, 1.1f, Pal::green, ALN_C, "YOU SURVIVED");
        drawText(TOP_W/2, 120, 0.5f, Pal::textDim, ALN_C, "The Dealer is beaten. You walk away.");
    } else {
        drawText(TOP_W/2, 70, 1.1f, Pal::red, ALN_C, "GAME OVER");
        drawText(TOP_W/2, 120, 0.5f, Pal::textDim, ALN_C, "The Dealer collects his due.");
    }
    drawText(TOP_W/2, 180, 0.5f, Pal::text, ALN_C, "A: menu    START: play again");
    if (g_settings.vignette) drawVignette(TOP_W,SCR_H,1.0f);
    if (g_settings.scanlines) drawScanlines(TOP_W,SCR_H,0.10f);
}

static void renderInfoBottom(const char* title, const char* sub){
    C2D_DrawRectangle(0,0,0, BOT_W, SCR_H, Pal::bg0,Pal::bg0,Pal::bg1,Pal::bg1);
    drawText(BOT_W/2, 90, 0.6f, Pal::textDim, ALN_C, "%s", title);
    if (sub) drawText(BOT_W/2, 120, 0.4f, Pal::textMute, ALN_C, "%s", sub);
    if (g_settings.vignette) drawVignette(BOT_W,SCR_H,0.7f);
}

// ---------------------------------------------------------------------------
//  GAME FLOW HELPERS
// ---------------------------------------------------------------------------
static void startNewGame(){
    gameNew(game);
    uiSel=8; prevTurn=ACTOR_PLAYER; prevRound=game.round;
    dealerTimer=0; roundBannerT=1.6f; loadBannerT=1.3f; overDelay=0;
    audioSetMusic(true);
}

// detect transitions for banners
static void postUpdateDetect(){
    if (game.round != prevRound){ roundBannerT=1.6f; prevRound=game.round; }
    if (game.turn==ACTOR_DEALER && prevTurn==ACTOR_PLAYER){ dealerTimer = 0.7f; }
    prevTurn=game.turn;
}

// ---------------------------------------------------------------------------
//  INPUT
// ---------------------------------------------------------------------------
static void useSelected(){
    if (game.over || game.turn!=ACTOR_PLAYER) return;
    if (uiSel==8){ ActionResult r=playerShoot(game, ACTOR_DEALER); audioPlay(SFX_SELECT); applyShotFx(r, ACTOR_DEALER); return; }
    if (uiSel==9){ ActionResult r=playerShoot(game, ACTOR_PLAYER); audioPlay(SFX_SELECT); applyShotFx(r, ACTOR_PLAYER); return; }
    if (uiSel < (int)game.player.items.size()){
        Item it=game.player.items[uiSel];
        if (it==IT_ADREN){
            if (!game.dealer.items.empty()){ adrenMode=true; adrenSel=0; audioPlay(SFX_SELECT);} else audioPlay(SFX_BACK);
            return;
        }
        if (canUseItem(game, ACTOR_PLAYER, it)){
            audioPlay(SFX_ITEM);
            playerUseItem(game, it);
            if (uiSel >= (int)game.player.items.size()) uiSel = game.player.items.empty()?8:(int)game.player.items.size()-1;
        } else audioPlay(SFX_BACK);
    }
}

static void handleGameInput(u32 kDown){
    if (adrenMode){
        int n=(int)game.dealer.items.size();
        if (n==0){ adrenMode=false; return; }
        if (kDown&(KEY_LEFT|KEY_UP)){ adrenSel=(adrenSel-1+n)%n; audioPlay(SFX_MOVE);}
        if (kDown&(KEY_RIGHT|KEY_DOWN)){ adrenSel=(adrenSel+1)%n; audioPlay(SFX_MOVE);}
        if (kDown&KEY_A){
            playerUseItem(game, IT_ADREN, adrenSel);
            adrenMode=false;
            if (uiSel>=(int)game.player.items.size()) uiSel=game.player.items.empty()?8:(int)game.player.items.size()-1;
        }
        if (kDown&KEY_B){ adrenMode=false; audioPlay(SFX_BACK);}
        return;
    }
    if (game.turn!=ACTOR_PLAYER || game.over) return;

    if (kDown&KEY_X){ ActionResult r=playerShoot(game,ACTOR_DEALER); audioPlay(SFX_SELECT); applyShotFx(r,ACTOR_DEALER); return; }
    if (kDown&KEY_Y){ ActionResult r=playerShoot(game,ACTOR_PLAYER); audioPlay(SFX_SELECT); applyShotFx(r,ACTOR_PLAYER); return; }

    // navigation grid: items 0..7 (2x4), 8 dealer, 9 self
    if (kDown&KEY_RIGHT){
        if (uiSel<8){ if (uiSel%4==3 || uiSel+1>= (uiSel<4?4:8)) uiSel=(uiSel<4)?8:9; else uiSel++; }
        audioPlay(SFX_MOVE);
    }
    if (kDown&KEY_LEFT){
        if (uiSel==8) uiSel=3; else if (uiSel==9) uiSel=7; else if (uiSel%4!=0) uiSel--;
        audioPlay(SFX_MOVE);
    }
    if (kDown&KEY_DOWN){
        if (uiSel<4) uiSel+=4; else if (uiSel==8) uiSel=9; else if (uiSel>=4&&uiSel<8) uiSel=9;
        audioPlay(SFX_MOVE);
    }
    if (kDown&KEY_UP){
        if (uiSel>=4&&uiSel<8) uiSel-=4; else if (uiSel==9) uiSel=8;
        audioPlay(SFX_MOVE);
    }
    if (uiSel<0) uiSel=0; if (uiSel>9) uiSel=9;
    if (kDown&KEY_A) useSelected();
}

static void handleGameTouch(touchPosition tp, u32 kDown){
    if (!(kDown&KEY_TOUCH)) return;
    if (game.over || game.turn!=ACTOR_PLAYER || adrenMode) return;
    float tx=tp.px, ty=tp.py;
    for (int i=0;i<8;i++){
        float x,y; itemSlotRect(i,x,y);
        if (tx>=x&&tx<=x+ITEM_W&&ty>=y&&ty<=y+ITEM_H){ uiSel=i; useSelected(); return; }
    }
    if (tx>=BTN_X&&tx<=BTN_X+BTN_W){
        if (ty>=ITEM_Y&&ty<=ITEM_Y+BTN_SH){ uiSel=8; useSelected(); return; }
        if (ty>=ITEM_Y+BTN_SH+6&&ty<=ITEM_Y+2*BTN_SH+6){ uiSel=9; useSelected(); return; }
    }
}

// ---------------------------------------------------------------------------
//  MAIN
// ---------------------------------------------------------------------------
int main(int argc, char** argv){
    romfsInit(); // harmless if no romfs
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    g_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    g_bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    gfxTextInit();

    settingsLoad();
    audioInit();
    audioApplyVolumes();
    g_set = buildSettings();

    srand((unsigned)(svcGetSystemTick()&0xFFFFFFFF));

    u64 last = svcGetSystemTick();

    while (aptMainLoop()){
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();
        touchPosition tp; hidTouchRead(&tp);

        u64 now = svcGetSystemTick();
        float dt = (float)((now-last)/TICKS_PER_SEC);
        last=now;
        if (dt>0.1f) dt=0.1f;
        if (dt>0.0001f) fpsVal = fpsVal*0.9f + (1.f/dt)*0.1f;
        float spd = g_settings.gameSpeed/100.f;

        // ---- global quit ----
        if ((kDown&KEY_START) && state==ST_MENU && menuSel==3) break;

        // ---- per-state input ----
        switch (state){
            case ST_SPLASH:
                splashT += dt;
                if (kDown&(KEY_A|KEY_START)){ audioPlay(SFX_SELECT); state=ST_MENU; }
                break;
            case ST_MENU:
                if (kDown&KEY_UP){ menuSel=(menuSel-1+kMenuCount)%kMenuCount; audioPlay(SFX_MOVE);}
                if (kDown&KEY_DOWN){ menuSel=(menuSel+1)%kMenuCount; audioPlay(SFX_MOVE);}
                if (kDown&KEY_A){
                    audioPlay(SFX_SELECT);
                    if (menuSel==0){ startNewGame(); state=ST_GAME; }
                    else if (menuSel==1){ howScroll=0; state=ST_HOWTO; }
                    else if (menuSel==2){ setSel=1; state=ST_SETTINGS; }
                    else if (menuSel==3){ aptSetChainloader(0,0); goto cleanup; }
                }
                break;
            case ST_HOWTO:
                if (kHeld&KEY_DOWN) howScroll+=180.f*dt;
                if (kHeld&KEY_UP)   howScroll-=180.f*dt;
                if (howScroll<0) howScroll=0;
                if (howScroll>320) howScroll=320;
                if (kDown&KEY_B){ audioPlay(SFX_BACK); state=ST_MENU; }
                break;
            case ST_SETTINGS: {
                int total=(int)g_set.size();
                if (kDown&KEY_UP){ do{ setSel=(setSel-1+total)%total; }while(g_set[setSel].group&&g_set[setSel].group[0]=='h'); audioPlay(SFX_MOVE);}
                if (kDown&KEY_DOWN){ do{ setSel=(setSel+1)%total; }while(g_set[setSel].group&&g_set[setSel].group[0]=='h'); audioPlay(SFX_MOVE);}
                SetItem& s=g_set[setSel];
                int delta=0;
                if (kDown&(KEY_LEFT|KEY_L)) delta=-1;
                if (kDown&(KEY_RIGHT|KEY_R)) delta=1;
                if ((kDown&KEY_A) && s.kind==S_TOGGLE) delta=1;
                if (delta!=0 && s.val){
                    if (s.kind==S_TOGGLE){ bool* b=(bool*)s.val; *b=!*b; }
                    else if (s.kind==S_SLIDER){ int* v=(int*)s.val; *v+=delta*s.step; if(*v<s.lo)*v=s.lo; if(*v>s.hi)*v=s.hi; }
                    else if (s.kind==S_CHOICE){ int* v=(int*)s.val; *v+=delta; if(*v<s.lo)*v=s.hi; if(*v>s.hi)*v=s.lo; }
                    audioPlay(SFX_MOVE);
                    audioApplyVolumes();
                }
                if (kDown&KEY_B){ audioPlay(SFX_BACK); settingsSave(); audioApplyVolumes(); state=ST_MENU; }
                break; }
            case ST_GAME:
                handleGameInput(kDown);
                handleGameTouch(tp, kDown);
                if (kDown&KEY_START && !game.over){ /* pause to menu */ settingsSave(); }
                if (kDown&KEY_SELECT){ /* reserved */ }
                break;
            case ST_GAMEOVER:
                if (kDown&KEY_START){ audioPlay(SFX_SELECT); startNewGame(); state=ST_GAME; }
                else if (kDown&KEY_A){ audioPlay(SFX_SELECT); audioSetMusic(false); state=ST_MENU; }
                break;
        }

        // ---- game simulation update ----
        if (state==ST_GAME){
            if (bannerT>0) bannerT-=dt;
            if (roundBannerT>0) roundBannerT-=dt;
            if (loadBannerT>0) loadBannerT-=dt;

            if (!game.over && game.turn==ACTOR_DEALER){
                dealerTimer -= dt;
                if (dealerTimer<=0){
                    DealerStep s = dealerStep(game);
                    if (s.kind!=DA_NONE){
                        setBanner(s.think.c_str());
                        if (s.kind==DA_SHOOT_PLAYER) applyShotFx(s.result, ACTOR_PLAYER);
                        else if (s.kind==DA_SHOOT_SELF) applyShotFx(s.result, ACTOR_DEALER);
                    }
                    dealerTimer = 0.95f/spd;
                }
            }
            postUpdateDetect();

            if (game.over){
                overDelay += dt;
                if (overDelay>1.4f){ state=ST_GAMEOVER; }
            }
        }

        updateFx(dt);
        audioUpdate();

        // ---- render ----
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        gfxTextFrame();

        C2D_TargetClear(g_top, Pal::bg1);
        C2D_SceneBegin(g_top);
        switch (state){
            case ST_SPLASH:   renderSplash(); break;
            case ST_MENU:     renderMenu(); break;
            case ST_HOWTO:    renderHowto(); break;
            case ST_SETTINGS: renderSettings(); break;
            case ST_GAME:     renderTopGame(); break;
            case ST_GAMEOVER: renderGameOver(); break;
        }

        C2D_TargetClear(g_bot, Pal::bg1);
        C2D_SceneBegin(g_bot);
        switch (state){
            case ST_SPLASH:   renderInfoBottom("BUCKSHOT ROULETTE", "press A to begin"); break;
            case ST_MENU:     renderInfoBottom("MAIN MENU", "D-Pad + A"); break;
            case ST_HOWTO:    renderInfoBottom("HOW TO PLAY", "read on the top screen"); break;
            case ST_SETTINGS: renderInfoBottom("SETTINGS", "adjust on the top screen"); break;
            case ST_GAME:     renderBottomGame(); break;
            case ST_GAMEOVER: renderInfoBottom(game.playerWon?"VICTORY":"DEFEAT", "A: menu  START: again"); break;
        }
        C3D_FrameEnd(0);
    }

cleanup:
    settingsSave();
    audioExit();
    gfxTextExit();
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    romfsExit();
    return 0;
}
