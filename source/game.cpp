#include "game.h"
#include "audio.h"
#include "settings.h"
#include <3ds.h>
#include <cstdlib>
#include <algorithm>

// ---------------------------------------------------------------------------
// item metadata
// ---------------------------------------------------------------------------
const char* itemName(Item it) {
    switch (it) {
        case IT_GLASS:    return "Magnifying Glass";
        case IT_CIGS:     return "Cigarettes";
        case IT_BEER:     return "Beer";
        case IT_CUFFS:    return "Handcuffs";
        case IT_SAW:      return "Hand Saw";
        case IT_ADREN:    return "Adrenaline";
        case IT_PHONE:    return "Burner Phone";
        case IT_INVERT:   return "Inverter";
        case IT_MEDICINE: return "Expired Medicine";
        default:          return "-";
    }
}
const char* itemShort(Item it) {
    switch (it) {
        case IT_GLASS:    return "GLASS";
        case IT_CIGS:     return "CIGS";
        case IT_BEER:     return "BEER";
        case IT_CUFFS:    return "CUFFS";
        case IT_SAW:      return "SAW";
        case IT_ADREN:    return "ADREN";
        case IT_PHONE:    return "PHONE";
        case IT_INVERT:   return "INVERT";
        case IT_MEDICINE: return "MEDS";
        default:          return "-";
    }
}
const char* itemDesc(Item it) {
    switch (it) {
        case IT_GLASS:    return "Reveal the shell in the chamber.";
        case IT_CIGS:     return "Restore one life.";
        case IT_BEER:     return "Rack the gun, ejecting the chambered shell.";
        case IT_CUFFS:    return "The opponent skips their next turn.";
        case IT_SAW:      return "Your next shot deals double damage.";
        case IT_ADREN:    return "Steal & instantly use an opponent's item.";
        case IT_PHONE:    return "Learn what an upcoming shell is.";
        case IT_INVERT:   return "Flip the chambered shell live<->blank.";
        case IT_MEDICINE: return "50%: +2 life. Otherwise: -1 life.";
        default:          return "";
    }
}

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
static Actor other(Actor a){ return a == ACTOR_PLAYER ? ACTOR_DEALER : ACTOR_PLAYER; }
static Side& sideOf(Game& g, Actor a){ return a == ACTOR_PLAYER ? g.player : g.dealer; }
static std::vector<int>& knownOf(Game& g, Actor a){ return a == ACTOR_PLAYER ? g.knownPlayer : g.knownDealer; }
static int irand(int n){ return n <= 0 ? 0 : (rand() % n); }

void Game::pushLog(const std::string& s){
    log.push_back(s);
    if (log.size() > 40) log.erase(log.begin());
}

int shellsLeft(const Game& g){ return (int)g.shells.size() - (int)g.pos; }
int liveLeft(const Game& g){
    int c=0; for (size_t i=g.pos;i<g.shells.size();++i) if (g.shells[i]==SHELL_LIVE) c++; return c;
}
int blankLeft(const Game& g){ return shellsLeft(g) - liveLeft(g); }
bool gameChamberEmpty(const Game& g){ return g.pos >= g.shells.size(); }
Shell currentShell(const Game& g){
    if (g.pos < g.shells.size()) return g.shells[g.pos];
    return SHELL_BLANK;
}
int playerKnow(const Game& g, int index){
    if (index < 0 || index >= (int)g.knownPlayer.size()) return -1;
    return g.knownPlayer[index];
}

static int livesForRound(int r){ return 2 * r; } // 2, 4, 6

// ---------------------------------------------------------------------------
// item dealing
// ---------------------------------------------------------------------------
static Item randomItemForRound(int round){
    // round 1: no items. round 2: base kit. round 3: + advanced kit.
    static const Item base[] = { IT_GLASS, IT_CIGS, IT_BEER, IT_CUFFS, IT_SAW };
    static const Item adv[]  = { IT_GLASS, IT_CIGS, IT_BEER, IT_CUFFS, IT_SAW,
                                 IT_ADREN, IT_PHONE, IT_INVERT, IT_MEDICINE };
    if (round >= 3) return adv[irand((int)(sizeof(adv)/sizeof(adv[0])))];
    return base[irand((int)(sizeof(base)/sizeof(base[0])))];
}

static void dealItems(Game& g, int count){
    if (g.round < 2) return;
    for (int n=0;n<count;++n){
        if (g.player.items.size() < 8)
            g.player.items.push_back(randomItemForRound(g.round));
        if (g.dealer.items.size() < 8)
            g.dealer.items.push_back(randomItemForRound(g.round));
    }
}

// ---------------------------------------------------------------------------
// chamber loading
// ---------------------------------------------------------------------------
void gameReloadShells(Game& g){
    int maxShells = 8;
    int total = 2 + irand(maxShells - 1);       // 2..8
    int live  = 1 + irand(total - 1);           // 1..total-1
    int blank = total - live;
    if (blank < 1) { blank = 1; live = total - 1; }

    g.shells.clear();
    for (int i=0;i<live;i++)  g.shells.push_back(SHELL_LIVE);
    for (int i=0;i<blank;i++) g.shells.push_back(SHELL_BLANK);
    // Fisher-Yates shuffle
    for (int i=(int)g.shells.size()-1;i>0;--i){
        int j = irand(i+1);
        std::swap(g.shells[i], g.shells[j]);
    }
    g.pos = 0;
    g.announcedLive = live;
    g.announcedBlank = blank;
    g.sawActive = false;
    g.knownPlayer.assign(g.shells.size(), -1);
    g.knownDealer.assign(g.shells.size(), -1);

    char buf[96];
    snprintf(buf, sizeof(buf), "Loaded: %d LIVE, %d BLANK.", live, blank);
    g.pushLog(buf);

    // deal a fresh handful of items each reload (rounds 2+)
    dealItems(g, 2 + irand(3));
    audioPlay(SFX_RELOAD);
}

static void startRound(Game& g){
    int lv = livesForRound(g.round);
    g.player.lives = g.player.maxLives = lv;
    g.dealer.lives = g.dealer.maxLives = lv;
    g.player.cuffed = g.dealer.cuffed = false;
    g.player.items.clear();
    g.dealer.items.clear();
    g.turn = ACTOR_PLAYER;
    char buf[64];
    snprintf(buf, sizeof(buf), "=== ROUND %d  (%d lives) ===", g.round, lv);
    g.pushLog(buf);
    gameReloadShells(g);
}

void gameNew(Game& g){
    srand((unsigned)(svcGetSystemTick() & 0xFFFFFFFF));
    g = Game();
    g.round = 1;
    g.maxRounds = 3;
    g.over = false;
    g.playerWon = false;
    g.pushLog("The Dealer racks the shotgun and grins.");
    startRound(g);
}

// ---------------------------------------------------------------------------
// turn flow
// ---------------------------------------------------------------------------
static void advanceTurn(Game& g){
    Actor next = other(g.turn);
    Side& ns = sideOf(g, next);
    if (ns.cuffed){
        ns.cuffed = false;
        g.pushLog(next==ACTOR_PLAYER ? "You are cuffed — you skip a turn."
                                     : "The Dealer is cuffed — it skips a turn.");
        return; // turn stays with current actor
    }
    g.turn = next;
}

// Apply a life delta to a side; resolve round/game transitions.
// Returns true if the round or game ended.
static bool applyDeathCheck(Game& g, Actor victim, ActionResult& r){
    Side& v = sideOf(g, victim);
    if (v.lives > 0) return false;
    v.lives = 0;
    if (victim == ACTOR_DEALER){
        if (g.round < g.maxRounds){
            r.roundEnded = true;
            g.pushLog("The Dealer slumps. You survive the round.");
            audioPlay(SFX_WIN);
            g.round++;
            startRound(g);
        } else {
            r.gameEnded = true;
            g.over = true;
            g.playerWon = true;
            g.pushLog("The Dealer falls. You walk out alive.");
            audioPlay(SFX_WIN);
        }
    } else {
        r.gameEnded = true;
        g.over = true;
        g.playerWon = false;
        g.pushLog("Everything goes dark. The Dealer wins.");
        audioPlay(SFX_LOSE);
    }
    return true;
}

// Discharge the chambered shell from `shooter` toward `target`.
static ActionResult resolveShot(Game& g, Actor shooter, Actor target){
    ActionResult r;
    if (gameChamberEmpty(g)) return r;
    bool live = currentShell(g) == SHELL_LIVE;
    int dmg = g.sawActive ? 2 : 1;
    g.sawActive = false;
    g.pos++;                    // consume the shell
    r.fired = true;
    r.wasLive = live;

    const bool self = (shooter == target);
    char buf[128];

    if (live){
        Side& tg = sideOf(g, target);
        tg.lives -= dmg;
        // gunshot SFX is played by the UI at the animation's fire moment
        if (target == ACTOR_PLAYER)
            snprintf(buf,sizeof(buf), self ? "You shoot yourself. LIVE! (-%d)" : "The Dealer shoots you. LIVE! (-%d)", dmg);
        else
            snprintf(buf,sizeof(buf), self ? "The Dealer shoots itself. LIVE! (-%d)" : "You shoot the Dealer. LIVE! (-%d)", dmg);
        g.pushLog(buf);
        r.text = buf;
    } else {
        // dry-fire (blank) SFX is played by the UI at the animation's fire moment
        if (target == ACTOR_PLAYER)
            snprintf(buf,sizeof(buf), self ? "You shoot yourself. Blank." : "The Dealer shoots you. Blank.");
        else
            snprintf(buf,sizeof(buf), self ? "The Dealer shoots itself. Blank." : "You shoot the Dealer. Blank.");
        g.pushLog(buf);
        r.text = buf;
    }

    // death / round transition?
    if (live){
        if (applyDeathCheck(g, target, r)) return r; // round restarted; turn already reset
    }

    // self + blank keeps the turn; everything else passes it
    r.turnPassed = !(self && !live);
    if (r.turnPassed) advanceTurn(g);

    // reload if the chamber ran dry (round still going)
    if (!g.over && gameChamberEmpty(g)) gameReloadShells(g);
    return r;
}

// ---------------------------------------------------------------------------
// item effects (shared by player, dealer, and adrenaline)
// ---------------------------------------------------------------------------
static ActionResult applyItemEffect(Game& g, Actor user, Item it){
    ActionResult r;
    Side& self = sideOf(g, user);
    Actor oppA = other(user);
    Side& opp = sideOf(g, oppA);
    auto& known = knownOf(g, user);
    char buf[128];
    const bool isPlayer = (user == ACTOR_PLAYER);

    switch (it){
        case IT_GLASS: {
            audioPlay(SFX_GLASS);
            if (g.pos < g.shells.size()) known[g.pos] = currentShell(g);
            if (isPlayer){
                snprintf(buf,sizeof(buf),"Glass: the chamber holds a %s.",
                         currentShell(g)==SHELL_LIVE?"LIVE round":"BLANK");
                g.pushLog(buf);
            } else g.pushLog("The Dealer studies the chamber.");
        } break;

        case IT_CIGS: {
            audioPlay(SFX_HEAL);
            if (self.lives < self.maxLives) self.lives++;
            g.pushLog(isPlayer ? "You smoke. (+1 life)" : "The Dealer smokes. (+1 life)");
        } break;

        case IT_BEER: {
            audioPlay(SFX_BEER);
            if (!gameChamberEmpty(g)){
                bool live = currentShell(g)==SHELL_LIVE;
                g.knownPlayer[g.pos] = currentShell(g);
                g.knownDealer[g.pos] = currentShell(g);
                g.pos++;
                snprintf(buf,sizeof(buf),"%s racks the gun: a %s flies out.",
                         isPlayer?"You":"The Dealer", live?"LIVE round":"BLANK");
                g.pushLog(buf);
                if (!g.over && gameChamberEmpty(g)) gameReloadShells(g);
            }
        } break;

        case IT_CUFFS: {
            audioPlay(SFX_CUFF);
            opp.cuffed = true;
            g.pushLog(isPlayer ? "You cuff the Dealer." : "The Dealer cuffs you.");
        } break;

        case IT_SAW: {
            audioPlay(SFX_SAW);
            g.sawActive = true;
            g.pushLog(isPlayer ? "You saw off the barrel. Next shot: x2."
                               : "The Dealer saws the barrel. Next shot: x2.");
        } break;

        case IT_PHONE: {
            audioPlay(SFX_PHONE);
            // pick a random future shell still unknown to this user
            std::vector<int> cand;
            for (size_t i=g.pos+1;i<g.shells.size();++i)
                if (known[i] < 0) cand.push_back((int)i);
            if (!cand.empty()){
                int idx = cand[irand((int)cand.size())];
                known[idx] = g.shells[idx];
                if (isPlayer){
                    int ord = idx - (int)g.pos + 1;
                    snprintf(buf,sizeof(buf),"Phone: shell #%d will be %s.",
                             ord, g.shells[idx]==SHELL_LIVE?"LIVE":"BLANK");
                    g.pushLog(buf);
                } else g.pushLog("The Dealer mutters into a phone.");
            } else {
                g.pushLog(isPlayer?"Phone: nothing left to learn." : "The Dealer pockets the phone.");
            }
        } break;

        case IT_INVERT: {
            audioPlay(SFX_INVERT);
            if (!gameChamberEmpty(g)){
                g.shells[g.pos] = (currentShell(g)==SHELL_LIVE)?SHELL_BLANK:SHELL_LIVE;
                if (g.knownPlayer[g.pos]>=0) g.knownPlayer[g.pos] = g.shells[g.pos];
                if (g.knownDealer[g.pos]>=0) g.knownDealer[g.pos] = g.shells[g.pos];
                g.pushLog(isPlayer ? "You invert the chambered shell." : "The Dealer inverts the shell.");
            }
        } break;

        case IT_MEDICINE: {
            bool good = irand(2)==0;
            if (good){
                audioPlay(SFX_HEAL);
                self.lives = std::min(self.maxLives, self.lives + 2);
                g.pushLog(isPlayer ? "Meds work! (+2 life)" : "The Dealer's meds work. (+2)");
            } else {
                audioPlay(SFX_HURT);
                self.lives -= 1;
                g.pushLog(isPlayer ? "Meds backfire! (-1 life)" : "The Dealer's meds backfire. (-1)");
                applyDeathCheck(g, user, r);
            }
        } break;

        default: break;
    }
    return r;
}

// ---------------------------------------------------------------------------
// player API
// ---------------------------------------------------------------------------
bool canUseItem(const Game& g, Actor who, Item it){
    const Side& self = (who==ACTOR_PLAYER)?g.player:g.dealer;
    const Side& opp  = (who==ACTOR_PLAYER)?g.dealer:g.player;
    switch (it){
        case IT_CIGS:     return self.lives < self.maxLives;
        case IT_MEDICINE: return true;  // gamble allowed even at full (caps at max)
        case IT_CUFFS:    return !opp.cuffed;
        case IT_SAW:      return !g.sawActive && !gameChamberEmpty(g);
        case IT_GLASS:    return !gameChamberEmpty(g);
        case IT_INVERT:   return !gameChamberEmpty(g);
        case IT_BEER:     return !gameChamberEmpty(g);
        case IT_PHONE:    return shellsLeft(g) > 1;
        case IT_ADREN:    return !opp.items.empty();
        default:          return false;
    }
}

static void removeItem(Side& s, Item it){
    for (auto i = s.items.begin(); i != s.items.end(); ++i)
        if (*i == it){ s.items.erase(i); return; }
}

ActionResult playerUseItem(Game& g, Item it, int adrenTargetIndex){
    ActionResult r;
    if (g.over || g.turn != ACTOR_PLAYER) return r;

    if (it == IT_ADREN){
        if (g.dealer.items.empty()) return r;
        int idx = adrenTargetIndex;
        if (idx < 0 || idx >= (int)g.dealer.items.size()) return r;
        Item stolen = g.dealer.items[idx];
        if (stolen == IT_ADREN) return r;          // can't steal adrenaline
        if (!canUseItem(g, ACTOR_PLAYER, stolen)) return r;
        g.dealer.items.erase(g.dealer.items.begin()+idx);
        removeItem(g.player, IT_ADREN);
        audioPlay(SFX_ITEM);
        g.pushLog("You jab adrenaline and snatch an item!");
        return applyItemEffect(g, ACTOR_PLAYER, stolen);
    }

    if (!canUseItem(g, ACTOR_PLAYER, it)) return r;
    removeItem(g.player, it);
    r = applyItemEffect(g, ACTOR_PLAYER, it);
    return r;
}

ActionResult playerShoot(Game& g, Actor target){
    if (g.over || g.turn != ACTOR_PLAYER) return ActionResult();
    return resolveShot(g, ACTOR_PLAYER, target);
}

// ---------------------------------------------------------------------------
// dealer AI  (one atomic step per call)
// ---------------------------------------------------------------------------
static bool dealerHas(const Game& g, Item it){
    for (Item x : g.dealer.items) if (x==it) return true; return false;
}

DealerStep dealerStep(Game& g){
    DealerStep st;
    if (g.over || g.turn != ACTOR_DEALER) return st;

    Side& me = g.dealer;
    auto& known = g.knownDealer;
    int diff = g_settings.difficulty;     // 0 calm, 1 standard, 2 ruthless

    int sl = shellsLeft(g);
    int cur = (g.pos < known.size()) ? known[g.pos] : -1;  // dealer's knowledge of current
    float pLive = sl>0 ? (float)liveLeft(g)/(float)sl : 0.f;

    // 1) heal when hurt
    if (me.lives < me.maxLives && dealerHas(g, IT_CIGS)){
        removeItem(me, IT_CIGS);
        st.kind = DA_ITEM; st.item = IT_CIGS; st.think = "lights a cigarette";
        st.result = applyItemEffect(g, ACTOR_DEALER, IT_CIGS);
        return st;
    }
    if (me.lives == 1 && dealerHas(g, IT_MEDICINE) && !dealerHas(g, IT_CIGS) && diff>=1){
        removeItem(me, IT_MEDICINE);
        st.kind = DA_ITEM; st.item = IT_MEDICINE; st.think = "gambles on old pills";
        st.result = applyItemEffect(g, ACTOR_DEALER, IT_MEDICINE);
        return st;
    }

    // 2) gather information when the current shell is unknown
    bool wantInfo = (cur < 0) && (sl >= 1);
    if (wantInfo && dealerHas(g, IT_GLASS) && (diff>=1 || irand(2)==0)){
        removeItem(me, IT_GLASS);
        st.kind = DA_ITEM; st.item = IT_GLASS; st.think = "peers through the glass";
        st.result = applyItemEffect(g, ACTOR_DEALER, IT_GLASS);
        return st;
    }

    // refresh knowledge after a possible glass use
    cur = (g.pos < known.size()) ? known[g.pos] : -1;

    // 3) turn a known blank into a kill with the inverter
    if (cur == SHELL_BLANK && dealerHas(g, IT_INVERT) && me.lives>0 && diff>=1){
        removeItem(me, IT_INVERT);
        st.kind = DA_ITEM; st.item = IT_INVERT; st.think = "flips the shell";
        st.result = applyItemEffect(g, ACTOR_DEALER, IT_INVERT);
        return st; // now it's live; next step will shoot the player
    }

    cur = (g.pos < known.size()) ? known[g.pos] : -1;
    bool willHitPlayer;
    if (cur == SHELL_LIVE)      willHitPlayer = true;
    else if (cur == SHELL_BLANK) willHitPlayer = false;
    else {
        float bias = (diff==2)?0.10f : (diff==0?-0.05f:0.f);
        if (pLive >= 0.999f)      willHitPlayer = true;
        else if (pLive <= 0.001f) willHitPlayer = false;
        else if (pLive + bias > 0.5f) willHitPlayer = true;
        else if (pLive + bias < 0.5f) willHitPlayer = false;
        else willHitPlayer = irand(2)==0;
    }

    if (willHitPlayer){
        // saw for extra damage when confident & worthwhile
        bool confident = (cur==SHELL_LIVE) || pLive > 0.7f;
        if (confident && dealerHas(g, IT_SAW) && !g.sawActive && g.player.lives>1){
            removeItem(me, IT_SAW);
            st.kind = DA_ITEM; st.item = IT_SAW; st.think = "saws the barrel";
            st.result = applyItemEffect(g, ACTOR_DEALER, IT_SAW);
            return st;
        }
        // cuff before shooting to chain another turn
        if (confident && dealerHas(g, IT_CUFFS) && !g.player.cuffed && sl>1){
            removeItem(me, IT_CUFFS);
            st.kind = DA_ITEM; st.item = IT_CUFFS; st.think = "snaps on the cuffs";
            st.result = applyItemEffect(g, ACTOR_DEALER, IT_CUFFS);
            return st;
        }
        st.kind = DA_SHOOT_PLAYER; st.think = "aims at you";
        st.result = resolveShot(g, ACTOR_DEALER, ACTOR_PLAYER);
        st.endsTurn = true;
        return st;
    } else {
        // known/likely blank: discard a known blank with beer to dig, else shoot self
        if (cur == SHELL_BLANK && dealerHas(g, IT_BEER) && sl>1 && diff>=2 && irand(3)==0){
            removeItem(me, IT_BEER);
            st.kind = DA_ITEM; st.item = IT_BEER; st.think = "racks a shell out";
            st.result = applyItemEffect(g, ACTOR_DEALER, IT_BEER);
            return st;
        }
        st.kind = DA_SHOOT_SELF; st.think = "turns the gun on itself";
        st.result = resolveShot(g, ACTOR_DEALER, ACTOR_DEALER);
        st.endsTurn = (g.turn != ACTOR_DEALER) || g.over; // blank self keeps turn
        return st;
    }
}
