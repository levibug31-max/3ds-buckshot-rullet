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
    float blackout=0;          // screen cut-to-black when the player is hit
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

// shot / gun-aim animation (pre-resolved; the visuals play out over ~0.65s)
static bool         shotActive = false;
static float        shotT      = 0.f;     // seconds since the shot began
static bool         shotFired  = false;   // fire-moment fx already triggered
static ActionResult shotResult;
static Actor        shotShooter = ACTOR_PLAYER;
static Actor        shotTarget  = ACTOR_DEALER;

// smoothed health (lerps toward the real value so hits land with the flash)
static float dispPlayerLives = 0.f;
static float dispDealerLives = 0.f;

// shell-loading animation
static int   prevShellsLeft = 0;

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

// fire-moment effects, with sparks emitted from the muzzle position (mx,my)
static void applyShotFx(const ActionResult& r, Actor target, float mx, float my){
    fx.recoil = 1.0f;
    if (r.fired && r.wasLive){
        if (g_settings.muzzleFlash) fx.flash = 1.0f;
        if (g_settings.screenShake) fx.shake = 9.0f;
        if (target==ACTOR_DEALER){ fx.dealerHurt = 1.0f; }
        else { fx.playerHurt = 1.0f; fx.blackout = 1.0f; }  // taking a live round cuts to black
        spawnSparks(mx, my, 22, Pal::amber);
        spawnSparks(mx, my, 10, Pal::red);
    } else if (r.fired){
        if (g_settings.muzzleFlash) fx.flash = 0.35f;
        if (g_settings.screenShake) fx.shake = 3.0f;
        spawnSparks(mx, my, 6, Pal::textDim);
    }
}

static void updateFx(float dt){
    float decay = expf(-dt*6.f);
    fx.shake     *= decay;
    fx.flash     *= expf(-dt*7.f);
    fx.recoil    *= expf(-dt*9.f);
    fx.dealerHurt*= expf(-dt*4.f);
    fx.playerHurt*= expf(-dt*4.f);
    fx.blackout  *= expf(-dt*2.0f);   // holds dark, then fades back in
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

static inline u8 lerp8(u8 a, u8 b, float t){ return (u8)(a + (b-a)*t); }
static inline u32 lerpCol(u32 a, u32 b, float t){
    return C2D_Color32(
        lerp8(a&0xFF,b&0xFF,t), lerp8((a>>8)&0xFF,(b>>8)&0xFF,t),
        lerp8((a>>16)&0xFF,(b>>16)&0xFF,t), lerp8((a>>24)&0xFF,(b>>24)&0xFF,t));
}

// The Dealer: a large floating pale head + two floating hands (no body), as in
// the real game. Empty eye sockets; a toothy grin that becomes a pained scowl
// when shot; the head cracks and bleeds as it loses lives; red eyes only ignite
// once it is finally killed.
static void drawDealer(float cx, float cy, int lives, int maxLives, float hurt, bool dead){
    float dmg = maxLives>0 ? 1.f - (float)lives/(float)maxLives : 0.f;
    if (dmg<0) dmg=0;
    if (dmg>1) dmg=1;

    // recoil jitter + forward lunge when freshly shot ("emerging" scare)
    float jx = (frand()*2-1)*hurt*3.f;
    float jy = (frand()*2-1)*hurt*2.f;
    cx += jx; cy += jy;
    float lunge = 1.f + hurt*0.06f;

    // pale flesh, draining to a grey corpse pallor as damage mounts
    u32 fleshHi = lerpCol(C2D_Color32(0xdc,0xcc,0xc0,0xFF), C2D_Color32(0x9c,0x9a,0x9a,0xFF), dmg);
    u32 flesh   = lerpCol(C2D_Color32(0xc6,0xb4,0xa8,0xFF), C2D_Color32(0x84,0x82,0x82,0xFF), dmg);
    u32 fleshSh = lerpCol(C2D_Color32(0x96,0x84,0x7a,0xFF), C2D_Color32(0x56,0x55,0x56,0xFF), dmg);
    u32 hollow  = C2D_Color32(0x0a,0x08,0x0a,0xFF);
    u32 blood   = C2D_Color32(0x86,0x10,0x10,0xFF);

    float hw = 38*lunge, hh = 46*lunge;   // head half-extents

    // floating hands resting on the near table edge
    for (int s=-1; s<=1; s+=2){
        float hx = cx + s*72, hyy = 150;
        C2D_DrawEllipse(hx-13, hyy-6, 0.28f, 26, 16, fleshSh,fleshSh,flesh,flesh);
        for (int f=0; f<4; f++) RS(hx-11+f*6, hyy-12, 4, 12, flesh);  // fingers
        RS(hx + s*12 - 2, hyy-4, 5, 9, fleshSh);                      // thumb
    }

    // soft shadow/halo behind the floating head
    C2D_DrawEllipse(cx-hw-6, cy-hh-6, 0.18f, (hw+6)*2, (hh+6)*2,
        C2D_Color32(0,0,0,90),C2D_Color32(0,0,0,90),C2D_Color32(0,0,0,0),C2D_Color32(0,0,0,0));

    // head (elongated pale dome)
    C2D_DrawEllipse(cx-hw, cy-hh, 0.30f, hw*2, hh*2, fleshHi,fleshHi,flesh,flesh);
    // cheek/jaw shading
    C2D_DrawEllipse(cx-hw, cy+hh*0.1f, 0.31f, hw*2, hh*0.9f,
        fleshSh,fleshSh, C2D_Color32(0,0,0,0),C2D_Color32(0,0,0,0));
    // brow ridge
    RS(cx-26, cy-16, 52, 3, fleshSh);

    // deep hollow eye sockets (no eyes during play)
    C2D_DrawEllipse(cx-26, cy-20, 0.40f, 22, 18, hollow,hollow,hollow,hollow);
    C2D_DrawEllipse(cx+4,  cy-20, 0.40f, 22, 18, hollow,hollow,hollow,hollow);
    // socket inner shadow on the flesh
    C2D_DrawEllipse(cx-27, cy-22, 0.39f, 24, 8, fleshSh,fleshSh,
        C2D_Color32(0,0,0,0),C2D_Color32(0,0,0,0));

    // once dead: red delivery-system eyes ignite in the sockets
    if (dead){
        float ge = 0.55f + 0.45f*fx.eyeGlow;
        u32 halo = C2D_Color32(0xff,0x20,0x10,(u8)(120*ge));
        u32 core = C2D_Color32(0xff,(u8)(0x40*ge),0x20,0xFF);
        C2D_DrawCircleSolid(cx-15, cy-11, 0.45f, 7.f, halo);
        C2D_DrawCircleSolid(cx+15, cy-11, 0.45f, 7.f, halo);
        C2D_DrawCircleSolid(cx-15, cy-11, 0.47f, 3.4f, core);
        C2D_DrawCircleSolid(cx+15, cy-11, 0.47f, 3.4f, core);
    }

    // nasal cavity
    C2D_DrawTriangle(cx, cy-4, hollow, cx-4, cy+8, hollow, cx+4, cy+8, hollow, 0.44f);

    // ---- mouth: toothy grin, or a pained scowl when shot / badly hurt ----
    bool scowl = (hurt>0.22f) || (dmg>0.66f);
    float curve = scowl ? -1.f : 1.f;       // +smile / -frown
    float mx = cx, my = cy+26, mhw = 26;
    u32 tooth = lerpCol(C2D_Color32(0xe6,0xdc,0xcc,0xFF), C2D_Color32(0xb0,0xa6,0x9a,0xFF), dmg);
    int cols = 18;
    for (int i=0;i<cols;i++){
        float fx2 = (float)i/(cols-1);            // 0..1
        float xx = mx - mhw + fx2*2*mhw;
        float n  = (xx-mx)/mhw;                    // -1..1
        float yy = my + curve*7.f*(1.f - n*n);     // parabola
        float colw = (2*mhw)/cols + 1;
        // dark mouth interior
        RS(xx, yy-7, colw, 14, hollow);
        // teeth (skip some at high damage for a broken look)
        bool missing = (dmg>0.4f && (i%5)== ((int)(dmg*5))%5) ||
                       (dmg>0.75f && (i%3)==1);
        if (!missing && (i%2==0))
            RS(xx+1, yy-6, colw-2, 12, tooth);
    }
    // lips
    for (int i=0;i<cols;i++){
        float fx2=(float)i/(cols-1); float xx=mx-mhw+fx2*2*mhw; float n=(xx-mx)/mhw;
        float yy=my+curve*7.f*(1.f-n*n);
        RS(xx, yy-8, (2*mhw)/cols+1, 2, fleshSh);
    }

    // ---- progressive damage: cracks, chips and blood ----
    if (dmg>0.15f){
        // crack from the right brow
        C2D_DrawTriangle(cx+10,cy-26, hollow, cx+13,cy-26, hollow, cx+20,cy-6, hollow, 0.45f);
    }
    if (dmg>0.45f){
        // crack across the left cheek + a chipped notch on the skull edge
        C2D_DrawTriangle(cx-22,cy-2, hollow, cx-19,cy-2, hollow, cx-8,cy+14, hollow, 0.45f);
        C2D_DrawEllipse(cx-hw+2, cy-hh+8, 0.46f, 12, 12, Pal::bg1,Pal::bg1,Pal::bg1,Pal::bg1);
        // blood from the right socket
        RS(cx+13, cy-10, 3, 16+(int)(dmg*14), blood);
    }
    if (dmg>0.75f){
        // shattered jaw + heavy blood, a dark hole punched in the temple
        C2D_DrawEllipse(cx+hw-14, cy-6, 0.47f, 16, 16, hollow,hollow,hollow,hollow);
        RS(cx-3, my+6, 6, 20, blood);
        RS(cx-16, cy+18, 3, 12, blood);
        C2D_DrawTriangle(cx-10,cy+34, blood, cx+10,cy+34, blood, cx, cy+44, blood, 0.46f);
    }

    // hurt flash overlay (red), brightest right when struck
    if (hurt>0.01f)
        C2D_DrawEllipse(cx-hw-6, cy-hh-6, 0.5f, (hw+6)*2, (hh+6)*2,
            C2D_Color32(0xcc,0x1e,0x1e,(u8)(150*hurt)),C2D_Color32(0xcc,0x1e,0x1e,(u8)(150*hurt)),
            C2D_Color32(0xcc,0x1e,0x1e,(u8)(40*hurt)), C2D_Color32(0xcc,0x1e,0x1e,(u8)(40*hurt)));
}

// ---------------------------------------------------------------------------
//  Rotatable shotgun (drawn as rotated quads so it can aim in any direction)
// ---------------------------------------------------------------------------
struct V2 { float x, y; };
static inline V2 rotP(V2 p, V2 piv, float c, float s){
    float dx=p.x-piv.x, dy=p.y-piv.y;
    return { piv.x + dx*c - dy*s, piv.y + dx*s + dy*c };
}
static inline void quad(V2 a,V2 b,V2 c,V2 d,u32 col){
    C2D_DrawTriangle(a.x,a.y,col, b.x,b.y,col, c.x,c.y,col, 0.5f);
    C2D_DrawTriangle(a.x,a.y,col, c.x,c.y,col, d.x,d.y,col, 0.5f);
}
// local-space rectangle [x0,x1]x[y0,y1] rotated about pivot and drawn
static void rrect(V2 piv, float c, float s, float x0,float y0,float x1,float y1, u32 col){
    V2 a=rotP({piv.x+x0,piv.y+y0},piv,c,s);
    V2 b=rotP({piv.x+x1,piv.y+y0},piv,c,s);
    V2 d=rotP({piv.x+x1,piv.y+y1},piv,c,s);
    V2 e=rotP({piv.x+x0,piv.y+y1},piv,c,s);
    quad(a,b,d,e,col);
}
// muzzle world position for a given pose (local muzzle tip at +x)
static V2 gunMuzzle(V2 piv, float ang, float recoil){
    float c=cosf(ang), s=sinf(ang);
    return rotP({piv.x+58.f-recoil*12.f, piv.y}, piv, c, s);
}
// gun pointing along +x in local space; rotate by ang about piv.
static void drawGunRotated(V2 piv, float ang, float recoil, float flash){
    float c=cosf(ang), s=sinf(ang);
    float r = recoil*12.f;                  // recoil pushes the gun backward (-x)
    auto R=[&](float x0,float y0,float x1,float y1,u32 col){ rrect(piv,c,s,x0-r,y0,x1-r,y1,col); };

    u32 steel  = C2D_Color32(0x72,0x74,0x7c,0xFF);
    u32 steelD = C2D_Color32(0x3e,0x40,0x46,0xFF);
    u32 steelH = C2D_Color32(0xaa,0xac,0xb4,0xFF);
    u32 wood   = C2D_Color32(0x58,0x38,0x1e,0xFF);
    u32 woodD  = C2D_Color32(0x36,0x20,0x10,0xFF);
    u32 dark   = C2D_Color32(0x10,0x10,0x16,0xFF);

    // stock
    R(-50,-7,-30,9, wood);
    R(-50,-7,-30,-4, woodD);            // top shade
    // grip / trigger area
    R(-32,-9,-12,11, steelD);
    R(-30,-4,-16,4, dark);              // ejection port
    R(-32,-9,-12,-6, steelH);
    // barrel
    R(-12,-7,52,5, steel);
    R(-12,-7,52,-4, steelH);           // top highlight
    R(-12,2,52,5, steelD);             // bottom shade
    R(-12,-5,52,-4, steelH);           // rib line
    // pump/foregrip
    R(0,4,22,11, wood);
    R(0,4,22,6, woodD);
    // muzzle cap
    R(52,-8,60,6, steelD);
    R(52,-8,60,-5, steelH);

    // muzzle flash at the barrel tip, oriented along the gun
    if (flash>0.02f){
        V2 m = gunMuzzle(piv, ang, recoil);
        float k = flash;
        // forward direction
        float fx2=c, fy2=s;
        u32 f0=C2D_Color32(0xff,0xff,0xe0,(u8)(190*k));
        u32 f1=C2D_Color32(0xff,0xd0,0x60,(u8)(235*k));
        u32 f2=C2D_Color32(0xff,0x80,0x20,(u8)(170*k));
        u32 ft=C2D_Color32(0xff,0xb0,0x30,0);
        C2D_DrawCircleSolid(m.x, m.y, 0.91f, 14*k+5, f2);
        C2D_DrawCircleSolid(m.x, m.y, 0.93f, 8*k+3,  f1);
        C2D_DrawCircleSolid(m.x, m.y, 0.96f, 4*k+1,  f0);
        // flame cone
        float px=-fy2, py=fx2;          // perpendicular
        float len = 30*k+10;
        C2D_DrawTriangle(m.x+px*8, m.y+py*8, f1,
                         m.x-px*8, m.y-py*8, f1,
                         m.x+fx2*len, m.y+fy2*len, ft, 0.94f);
    }
}

// Returns the aim pose (pivot + angle) for a shot, given how far the gun is
// raised (a: 0 = resting on the table, 1 = fully aimed at the target).
static void shotPose(Actor shooter, Actor target, float a, V2& piv, float& ang){
    V2 rest = { TOP_W/2.0f, 176.f };
    V2 aimP; float aimA;
    if (shooter==ACTOR_PLAYER && target==ACTOR_DEALER){ aimP={TOP_W/2.0f, 205.f}; aimA=-1.5708f; }
    else if (shooter==ACTOR_PLAYER)                   { aimP={TOP_W/2.0f, 198.f}; aimA= 1.5708f; }
    else if (target==ACTOR_PLAYER)                    { aimP={TOP_W/2.0f, 120.f}; aimA= 1.5708f; }
    else                                              { aimP={TOP_W/2.0f, 138.f}; aimA=-1.5708f; }
    piv.x = rest.x + (aimP.x-rest.x)*a;
    piv.y = rest.y + (aimP.y-rest.y)*a;
    ang   = aimA * a;
}

// Begin a shot animation. The game state is already resolved; the visuals
// (raise -> aim -> fire -> lower) play out and the gunshot lands at the fire
// moment so it syncs with the muzzle flash.
static void beginShot(const ActionResult& r, Actor shooter, Actor target){
    shotActive  = true;
    shotT       = 0.f;
    shotFired   = false;
    shotResult  = r;
    shotShooter = shooter;
    shotTarget  = target;
}

// advance the shot animation; trigger fire-moment fx + audio once
static void updateShot(float dt, float spd){
    if (!shotActive) return;
    shotT += dt;
    if (!shotFired && shotT >= 0.30f){
        shotFired = true;
        // gunshot vs. dry click
        audioPlay(shotResult.wasLive ? SFX_LIVE : SFX_BLANK);
        // muzzle world position at the aimed pose
        V2 piv; float ang; shotPose(shotShooter, shotTarget, 1.f, piv, ang);
        V2 m = gunMuzzle(piv, ang, 0.f);
        applyShotFx(shotResult, shotTarget, m.x, m.y);
    }
    if (shotT >= 0.65f) shotActive = false;
}

// ---------------------------------------------------------------------------
//  TOP SCREEN
// ---------------------------------------------------------------------------
// shells laid out on the table during the load animation (counts only;
// the order is secret, so they're shown as anonymous brass shells)
static void drawShellLoadout(float sy, float prog){
    int total = game.announcedLive + game.announcedBlank;
    if (total<=0) return;
    float spacing = (total>8)? 22.f : 26.f;
    float startx = TOP_W/2.0f - (total-1)*spacing/2.0f;
    for (int i=0;i<total;i++){
        // each shell pops in sequentially
        float appear = prog*total - i;
        if (appear<=0) continue;
        float s = appear>1?1:appear;
        float yoff = (1-s)*-18.f;                    // drop into place
        bool live = i < game.announcedLive;          // tally colouring only
        u32 col = live? C2D_Color32(0x9a,0x20,0x20,0xFF) : C2D_Color32(0x4a,0x4e,0x56,0xFF);
        float x = startx + i*spacing;
        float y = 138.f + sy + yoff;
        // shell body
        RS(x-5, y, 10, 16, col);
        RS(x-5, y+16, 10, 7, C2D_Color32(0xc0,0x98,0x30,0xFF));   // brass
        RS(x-5, y, 10, 2, C2D_Color32(0,0,0,80));
    }
}

static void renderTopGame(){
    float sx=0, sy=0;
    if (fx.shake>0.3f){ sx=(frand()*2-1)*fx.shake; sy=(frand()*2-1)*fx.shake; }

    // ---- room background ----
    C2D_DrawRectangle(0,0,0, TOP_W, SCR_H, Pal::bg1,Pal::bg1,Pal::bg0,Pal::bg0);
    // back wall, lit faintly toward center
    C2D_DrawRectangle(0,0,0, TOP_W, 150,
        C2D_Color32(0x18,0x14,0x16,0xFF),C2D_Color32(0x18,0x14,0x16,0xFF),
        C2D_Color32(0x0c,0x09,0x0c,0xFF),C2D_Color32(0x0c,0x09,0x0c,0xFF));

    // overhead hanging lamp + cone of light onto the table
    for (int li=8; li>=0; li--){
        float w = 70.f + li*30.f;
        u8 a = (u8)(13 - li*1.2f);
        C2D_DrawTriangle(TOP_W/2.f+sx, -10, C2D_Color32(0xe0,0xc0,0x70,a),
                         TOP_W/2.f-w+sx, 175, C2D_Color32(0xe0,0xc0,0x70,0),
                         TOP_W/2.f+w+sx, 175, C2D_Color32(0xe0,0xc0,0x70,0), 0.04f);
    }
    // lamp fixture
    RS(TOP_W/2.f-1+sx, 0, 2, 8, C2D_Color32(0x30,0x28,0x20,0xFF));
    C2D_DrawTriangle(TOP_W/2.f-10+sx,16, C2D_Color32(0x20,0x1a,0x14,0xFF),
                     TOP_W/2.f+10+sx,16, C2D_Color32(0x20,0x1a,0x14,0xFF),
                     TOP_W/2.f+sx,4,     C2D_Color32(0x40,0x36,0x28,0xFF), 0.05f);
    C2D_DrawCircleSolid(TOP_W/2.f+sx, 16, 0.06f, 5, C2D_Color32(0xff,0xe6,0xa0,0xFF));

    // ---- the Dealer across the table ----
    bool dealerDead = game.over && game.playerWon;
    drawDealer(TOP_W/2.0f + sx*0.4f, 80+sy*0.4f,
               (int)(dispDealerLives+0.5f), game.dealer.maxLives, fx.dealerHurt, dealerDead);

    // ---- table (perspective: narrow at the dealer, wide at the player) ----
    u32 tTop = C2D_Color32(0x2a,0x16,0x12,0xFF);
    u32 tBot = C2D_Color32(0x16,0x0b,0x09,0xFF);
    C2D_DrawTriangle(70,150+sy, tTop, TOP_W-70,150+sy, tTop, TOP_W+40,SCR_H+sy, tBot, 0.05f);
    C2D_DrawTriangle(70,150+sy, tTop, TOP_W+40,SCR_H+sy, tBot, -40,SCR_H+sy, tBot, 0.05f);
    // felt inlay
    C2D_DrawTriangle(110,154+sy, C2D_Color32(0x16,0x2c,0x1a,0xFF),
                     TOP_W-110,154+sy, C2D_Color32(0x16,0x2c,0x1a,0xFF),
                     TOP_W-130,210+sy, C2D_Color32(0x0e,0x1c,0x10,0xFF), 0.06f);
    C2D_DrawTriangle(110,154+sy, C2D_Color32(0x16,0x2c,0x1a,0xFF),
                     TOP_W-130,210+sy, C2D_Color32(0x0e,0x1c,0x10,0xFF),
                     130,210+sy, C2D_Color32(0x0e,0x1c,0x10,0xFF), 0.06f);
    // near table edge highlight
    RS(0,150+sy,TOP_W,2, C2D_Color32(0x50,0x2c,0x18,0xFF));

    // ---- header text ----
    drawText(8, 5, 0.46f, Pal::textDim, ALN_L, "ROUND %d/%d", game.round, game.maxRounds);
    const char* turnTxt = game.over ? "" : (game.turn==ACTOR_PLAYER ? "YOUR TURN" : "DEALER");
    drawText(TOP_W/2.0f, 4, 0.52f, game.turn==ACTOR_PLAYER?Pal::green:Pal::red, ALN_C, "%s", turnTxt);
    if (g_settings.showFps) drawText(TOP_W-8, 5, 0.42f, Pal::textMute, ALN_R, "%.0f", fpsVal);

    // dealer health pips (just under the head/torso)
    drawHealthPips(TOP_W/2.0f - (game.dealer.maxLives-1)*7.f + sx*0.4f, 128+sy*0.4f,
                   (int)(dispDealerLives+0.5f), game.dealer.maxLives, Pal::red, fx.dealerHurt);
    if (game.dealer.cuffed) drawText(TOP_W/2.0f, 138, 0.36f, Pal::amber, ALN_C, "CUFFED");

    // ---- gun: resting on the table, or raised mid-shot ----
    bool loading = (loadBannerT>0);
    if (loading){
        // progress 0..1 over the banner life (banner starts at 1.6)
        float prog = 1.0f - (loadBannerT/1.6f);
        if (prog<0) prog=0;
        if (prog>1) prog=1;
        drawShellLoadout(sy, prog);
    }
    {
        V2 piv; float ang;
        if (shotActive){
            // raise (0..0.22), hold, fire ~0.30, lower (0.45..0.65)
            float a;
            if (shotT < 0.22f)      a = shotT/0.22f;
            else if (shotT < 0.45f) a = 1.f;
            else                    a = 1.f - (shotT-0.45f)/0.20f;
            if (a<0) a=0;
            if (a>1) a=1;
            shotPose(shotShooter, shotTarget, a, piv, ang);
        } else {
            piv = { TOP_W/2.0f, 176.f }; ang = 0.f;
        }
        piv.x += sx; piv.y += sy;
        if (!loading) drawGunRotated(piv, ang, fx.recoil, fx.flash);
    }

    // ---- particles (sparks) ----
    for (auto& p : fx.parts){
        float a = p.life/p.max;
        u8 al = (u8)(255*a);
        u32 c = (p.col & 0x00FFFFFF) | (al<<24);
        C2D_DrawCircleSolid(p.x+sx, p.y+sy, 0.9f, 1.6f*a+0.6f, c);
    }

    // ---- bottom HUD strip ----
    RS(0, 214, TOP_W, 26, C2D_Color32(0x08,0x06,0x08,0xCC));
    RS(0, 214, TOP_W, 1,  Pal::line);
    // player label + health
    drawText(8, 220, 0.42f, Pal::textDim, ALN_L, "YOU");
    drawHealthPips(40+sx, 226, (int)(dispPlayerLives+0.5f), game.player.maxLives, Pal::green, fx.playerHurt);
    if (game.player.cuffed) drawText(120, 220, 0.36f, Pal::amber, ALN_L, "CUFFED");
    if (game.sawActive)     drawText(168, 220, 0.36f, Pal::amber, ALN_L, "SAW x2");

    // chamber info (right of HUD)
    int knowCur = playerKnow(game, (int)game.pos);
    drawText(TOP_W-150, 220, 0.40f, Pal::red,   ALN_L, "LIVE %d", liveLeft(game));
    drawText(TOP_W-92,  220, 0.40f, Pal::steel, ALN_L, "BLANK %d", blankLeft(game));
    if (knowCur>=0)
        drawText(TOP_W-8, 220, 0.40f, knowCur==1?Pal::red:Pal::steel, ALN_R,
                 "NEXT:%s", knowCur==1?"LIVE":"BLANK");

    // remaining shells row, top-right
    int left = shellsLeft(game);
    int show = left>10?10:left;
    for (int i=0;i<show;i++){
        int kn = playerKnow(game, (int)game.pos+i);
        drawShellIcon(TOP_W-8-(show-i)*15, 26, 11, 24, (i==0)?(knowCur>=0?knowCur:-1):(kn>=0?kn:-1));
    }

    // ---- banners ----
    if (loadBannerT>0){
        float a = loadBannerT>1?1:loadBannerT;
        RS(0,58, TOP_W,34, C2D_Color32(0,0,0,(u8)(160*a)));
        drawText(TOP_W/2.f, 64, 0.6f, Pal::text, ALN_C, "%d LIVE   %d BLANK",
                 game.announcedLive, game.announcedBlank);
    }
    if (roundBannerT>0){
        float a = roundBannerT>1?1:roundBannerT;
        RS(0,92, TOP_W,52, C2D_Color32(0,0,0,(u8)(190*a)));
        drawText(TOP_W/2.f, 100, 0.95f, Pal::red, ALN_C, "ROUND %d", game.round);
        drawText(TOP_W/2.f, 124, 0.42f, Pal::textDim, ALN_C, "%d lives each", game.player.maxLives);
    }
    if (bannerT>0 && game.turn==ACTOR_DEALER && !shotActive){
        RS(60,150, TOP_W-120, 16, C2D_Color32(0,0,0,140));
        drawText(TOP_W/2.f, 151, 0.42f, Pal::textDim, ALN_C, "The Dealer %s...", banner);
    }

    // ---- post fx ----
    if (g_settings.vignette)  drawVignette(TOP_W, SCR_H, 1.0f);
    if (fx.flash>0.02f)       RS(0,0,TOP_W,SCR_H, C2D_Color32(0xff,0xe8,0xc0,(u8)(80*fx.flash)));
    if (g_settings.scanlines) drawScanlines(TOP_W, SCR_H, 0.10f);
    drawDim(TOP_W, SCR_H, (100-g_settings.brightness)/100.f*0.6f);
    // taking a live round to yourself cuts the world to black
    if (fx.blackout>0.01f){
        float b = fx.blackout>1?1:fx.blackout;
        RS(0,0,TOP_W,SCR_H, C2D_Color32(0,0,0,(u8)(255*b)));
    }
}

// ---------------------------------------------------------------------------
//  ITEM ICONS — small geometric symbols per item type
// ---------------------------------------------------------------------------
static void drawItemIcon(float cx, float cy, Item it){
    switch(it){
        case IT_GLASS:  // magnifying glass: circle + handle
            C2D_DrawCircleSolid(cx-1, cy-3, 0.6f, 7, C2D_Color32(0x1a,0x50,0x90,0xFF));
            C2D_DrawCircleSolid(cx-1, cy-3, 0.65f, 5, C2D_Color32(0x30,0x80,0xd0,0xFF));
            RS(cx+4, cy+1, 3, 9, C2D_Color32(0xb0,0x88,0x40,0xFF));
            RS(cx+5, cy+9, 5, 3, C2D_Color32(0x90,0x68,0x28,0xFF));
            break;
        case IT_CIGS:   // cigarette: white stick, orange tip
            RS(cx-10, cy-2, 18, 5, C2D_Color32(0xf0,0xec,0xe4,0xFF));
            RS(cx+7,  cy-2, 4, 5, C2D_Color32(0xe0,0x78,0x28,0xFF));
            RS(cx-10, cy-2, 2, 5, C2D_Color32(0xd0,0xcc,0xc0,0xFF));
            break;
        case IT_BEER:   // beer can
            RS(cx-5, cy-8, 10, 16, C2D_Color32(0xd8,0xa8,0x1c,0xFF));
            RS(cx-5, cy-8, 10, 3,  C2D_Color32(0xa8,0xa8,0xb0,0xFF));
            RS(cx-5, cy+5,  10, 3,  C2D_Color32(0xa8,0xa8,0xb0,0xFF));
            RS(cx-4, cy-4, 8, 8, C2D_Color32(0xf0,0xc0,0x2c,0xFF));
            break;
        case IT_CUFFS:  // two circles linked
            C2D_DrawCircleSolid(cx-6, cy, 0.6f, 5, C2D_Color32(0x98,0x98,0xa8,0xFF));
            C2D_DrawCircleSolid(cx+6, cy, 0.6f, 5, C2D_Color32(0x98,0x98,0xa8,0xFF));
            C2D_DrawCircleSolid(cx-6, cy, 0.65f, 3, C2D_Color32(0x60,0x60,0x70,0xFF));
            C2D_DrawCircleSolid(cx+6, cy, 0.65f, 3, C2D_Color32(0x60,0x60,0x70,0xFF));
            RS(cx-4, cy-1, 8, 2, C2D_Color32(0x80,0x80,0x90,0xFF));
            break;
        case IT_SAW:    // blade + teeth
            RS(cx-11, cy-2, 22, 5, C2D_Color32(0xc0,0xc2,0xcc,0xFF));
            RS(cx-11, cy-2, 22, 2, C2D_Color32(0xe0,0xe2,0xec,0xFF));
            for(int ti=0;ti<5;ti++) RS(cx-10+ti*4, cy-6, 3, 4, C2D_Color32(0xb0,0xb2,0xbc,0xFF));
            break;
        case IT_ADREN:  // syringe
            RS(cx-1, cy-9, 3, 14, C2D_Color32(0xd0,0xd8,0xff,0xFF));
            RS(cx-4, cy-5, 9, 2,  C2D_Color32(0xe0,0x40,0x40,0xFF));
            RS(cx-4, cy-1, 9, 2,  C2D_Color32(0xe0,0x40,0x40,0xFF));
            RS(cx,   cy+5, 1, 5,  C2D_Color32(0xc0,0xc8,0xe0,0xFF));
            break;
        case IT_PHONE:  // phone shape
            RS(cx-5, cy-9, 10, 18, C2D_Color32(0x28,0x28,0x38,0xFF));
            RS(cx-4, cy-8, 8, 13,  C2D_Color32(0x38,0x78,0xd0,0xFF));
            C2D_DrawCircleSolid(cx, cy+7, 0.65f, 1.5f, C2D_Color32(0xa0,0xa0,0xb0,0xFF));
            break;
        case IT_INVERT: // up+down arrows
            C2D_DrawTriangle(cx, cy-10, Pal::amber, cx-6,cy-4, Pal::amber, cx+6,cy-4, Pal::amber, 0.6f);
            C2D_DrawTriangle(cx, cy+10, Pal::amber, cx-6,cy+4, Pal::amber, cx+6,cy+4, Pal::amber, 0.6f);
            RS(cx-1, cy-4, 2, 8, Pal::amber);
            break;
        case IT_MEDICINE: // pill + cross
            C2D_DrawCircleSolid(cx, cy, 0.6f, 8, C2D_Color32(0x70,0x20,0x20,0xFF));
            RS(cx-5, cy-2, 10, 4, C2D_Color32(0xd8,0xdc,0xe0,0xFF));
            RS(cx-2, cy-5, 4, 10, C2D_Color32(0xd8,0xdc,0xe0,0xFF));
            break;
        default: break;
    }
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
            drawItemIcon(x+ITEM_W/2, y+16, it);
            drawText(x+ITEM_W/2, y+28, 0.34f, sel?Pal::brass:Pal::textDim, ALN_C, "%s", itemShort(it));
        } else {
            drawText(x+ITEM_W/2, y+15, 0.5f, Pal::textMute, ALN_C, "-");
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
    drawPanel(6, ly, BOT_W-12, SCR_H-ly-28, C2D_Color32(0x12,0x0f,0x10,0xFF), Pal::line);
    if (descIt!=IT_NONE)
        drawText(12, ly+6, 0.40f, Pal::amber, ALN_L, "%s", itemDesc(descIt));
    else if (uiSel==8) drawText(12, ly+6, 0.40f, Pal::amber, ALN_L, "Fire at the Dealer. Ends your turn.");
    else if (uiSel==9) drawText(12, ly+6, 0.40f, Pal::amber, ALN_L, "Fire at yourself. A blank keeps your turn.");

    // last log lines
    int n=(int)game.log.size();
    int lines=3;
    for (int i=0;i<lines;i++){
        int idx=n-lines+i;
        if (idx<0) continue;
        u32 c = (i==lines-1)?Pal::text:Pal::textMute;
        drawText(12, ly+24+i*16, 0.38f, c, ALN_L, "%s", game.log[idx].c_str());
    }

    // controls hint bar
    RS(6, SCR_H-24, BOT_W-12, 20, C2D_Color32(0x10,0x0c,0x0e,0xFF));
    RS(6, SCR_H-24, BOT_W-12, 1,  Pal::line);
    drawText(BOT_W/2, SCR_H-18, 0.36f, Pal::textMute, ALN_C, "X:DEALER  Y:SELF  A:USE  B:BACK");

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
    if (fx.blackout>0.01f){
        float b = fx.blackout>1?1:fx.blackout;
        RS(0,0,BOT_W,SCR_H, C2D_Color32(0,0,0,(u8)(255*b)));
    }
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
    if (first> total-maxRows) first = total-maxRows;
    if (first<0) first=0;
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
    // decorative shell icons
    drawShellIcon(BOT_W/2-36, 48, 16, 36, 1);
    drawShellIcon(BOT_W/2+20, 48, 16, 36, 0);
    drawText(BOT_W/2, 92, 0.55f, Pal::textDim, ALN_C, "%s", title);
    if (sub) drawText(BOT_W/2, 118, 0.4f, Pal::textMute, ALN_C, "%s", sub);
    // divider
    RS(40, 134, BOT_W-80, 1, Pal::line);
    // controls footer
    drawText(BOT_W/2, 148, 0.38f, Pal::textMute, ALN_C, "D-Pad: navigate");
    drawText(BOT_W/2, 165, 0.38f, Pal::textMute, ALN_C, "A: confirm   B: back");
    drawText(BOT_W/2, 182, 0.38f, Pal::textMute, ALN_C, "START: select/advance");
    if (g_settings.vignette) drawVignette(BOT_W,SCR_H,0.7f);
}

// ---------------------------------------------------------------------------
//  GAME FLOW HELPERS
// ---------------------------------------------------------------------------
static void startNewGame(){
    gameNew(game);
    uiSel=8; prevTurn=ACTOR_PLAYER; prevRound=game.round;
    dealerTimer=0; roundBannerT=1.6f; loadBannerT=1.6f; overDelay=0;
    shotActive=false; shotT=0; shotFired=false;
    dispPlayerLives = (float)game.player.lives;
    dispDealerLives = (float)game.dealer.lives;
    prevShellsLeft  = shellsLeft(game);
    audioSetMusic(true);
}

// detect transitions for banners and chamber reloads
static void postUpdateDetect(){
    if (game.round != prevRound){ roundBannerT=1.6f; prevRound=game.round; }
    if (game.turn==ACTOR_DEALER && prevTurn==ACTOR_PLAYER){ dealerTimer = 0.7f; }
    prevTurn=game.turn;
    // a fresh chamber load makes the count of remaining shells jump up
    int sl = shellsLeft(game);
    if (sl > prevShellsLeft){ loadBannerT = 1.6f; }
    prevShellsLeft = sl;
}

// ---------------------------------------------------------------------------
//  INPUT
// ---------------------------------------------------------------------------
static void useSelected(){
    if (game.over || game.turn!=ACTOR_PLAYER || shotActive) return;
    if (uiSel==8){ ActionResult r=playerShoot(game, ACTOR_DEALER); beginShot(r, ACTOR_PLAYER, ACTOR_DEALER); return; }
    if (uiSel==9){ ActionResult r=playerShoot(game, ACTOR_PLAYER); beginShot(r, ACTOR_PLAYER, ACTOR_PLAYER); return; }
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
    if (game.turn!=ACTOR_PLAYER || game.over || shotActive) return;

    if (kDown&KEY_X){ ActionResult r=playerShoot(game,ACTOR_DEALER); beginShot(r, ACTOR_PLAYER, ACTOR_DEALER); return; }
    if (kDown&KEY_Y){ ActionResult r=playerShoot(game,ACTOR_PLAYER); beginShot(r, ACTOR_PLAYER, ACTOR_PLAYER); return; }

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
    if (uiSel<0) uiSel=0;
    if (uiSel>9) uiSel=9;
    if (kDown&KEY_A) useSelected();
}

static void handleGameTouch(touchPosition tp, u32 kDown){
    if (!(kDown&KEY_TOUCH)) return;
    if (game.over || game.turn!=ACTOR_PLAYER || adrenMode || shotActive) return;
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
                    else if (menuSel==3){ goto cleanup; }
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

            updateShot(dt, spd);

            // the dealer only acts when no shot animation is playing
            if (!game.over && game.turn==ACTOR_DEALER && !shotActive){
                dealerTimer -= dt;
                if (dealerTimer<=0){
                    DealerStep s = dealerStep(game);
                    if (s.kind!=DA_NONE){
                        setBanner(s.think.c_str());
                        if (s.kind==DA_SHOOT_PLAYER) beginShot(s.result, ACTOR_DEALER, ACTOR_PLAYER);
                        else if (s.kind==DA_SHOOT_SELF) beginShot(s.result, ACTOR_DEALER, ACTOR_DEALER);
                        // DA_ITEM: applyItemEffect already played the item's sound
                    }
                    dealerTimer = 0.95f/spd;
                }
            }
            postUpdateDetect();

            // smooth the displayed health toward the real values
            dispPlayerLives += ((float)game.player.lives - dispPlayerLives) * (1.f - expf(-dt*12.f));
            dispDealerLives += ((float)game.dealer.lives - dispDealerLives) * (1.f - expf(-dt*12.f));

            // wait for any in-flight shot to finish before ending the game
            if (game.over && !shotActive){
                overDelay += dt;
                if (overDelay>1.2f){ state=ST_GAMEOVER; }
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
    return 0;
}
