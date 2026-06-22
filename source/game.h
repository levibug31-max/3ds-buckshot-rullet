#pragma once
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// Original implementation of a turn-based shotgun-duel ruleset:
//  - a shotgun is loaded with an announced mix of LIVE and BLANK shells in a
//    secret order; you and the Dealer take turns and may shoot yourself or the
//    opponent; blanks on yourself keep your turn.
//  - items add tactical depth. The Dealer is an AI that remembers revealed
//    shells and plays the odds.
// All mechanics, text and AI here are written from scratch.
// ---------------------------------------------------------------------------

enum Shell { SHELL_BLANK = 0, SHELL_LIVE = 1 };

enum Item {
    IT_NONE = 0,
    IT_GLASS,     // magnifying glass: reveal the chambered shell to you
    IT_CIGS,      // cigarettes: +1 life
    IT_BEER,      // beer: rack the shotgun, ejecting (and showing) the chambered shell
    IT_CUFFS,     // handcuffs: opponent skips their next turn
    IT_SAW,       // hand saw: next shot deals 2 damage
    IT_ADREN,     // adrenaline: steal & immediately use an opponent's item
    IT_PHONE,     // burner phone: learn a random future shell
    IT_INVERT,    // inverter: flip the chambered shell live<->blank
    IT_MEDICINE,  // expired medicine: 50% +2 life, else -1 life
    IT_MAX
};

const char* itemName(Item it);
const char* itemDesc(Item it);
const char* itemShort(Item it);

enum Actor { ACTOR_PLAYER = 0, ACTOR_DEALER = 1 };

struct Side {
    int  lives = 2;
    int  maxLives = 2;
    bool cuffed = false;        // skips next turn
    bool cuffPending = false;   // just cuffed this turn (don't release immediately)
    std::vector<Item> items;
};

// A single planned dealer action, surfaced so the UI can animate it.
enum DealerActKind { DA_NONE, DA_ITEM, DA_SHOOT_SELF, DA_SHOOT_PLAYER };

struct Game {
    Side player;
    Side dealer;

    std::vector<Shell> shells;  // chamber order; index 0 is next to fire
    size_t pos = 0;             // current position into shells
    int announcedLive = 0;
    int announcedBlank = 0;

    bool sawActive = false;     // next shot deals 2 (set by whoever sawed)
    Actor turn = ACTOR_PLAYER;
    int round = 1;              // 1..3
    int maxRounds = 3;

    // Knowledge tracking (what each side legitimately knows about specific shells)
    std::vector<int> knownPlayer;  // -1 unknown, else Shell value
    std::vector<int> knownDealer;

    bool over = false;
    bool playerWon = false;

    // log of recent events (newest last) for the bottom-screen ticker
    std::vector<std::string> log;

    void pushLog(const std::string& s);
};

// Lifecycle
void gameNew(Game& g);              // start a fresh run (round 1)
void gameReloadShells(Game& g);     // load a new chamber + deal items
bool gameChamberEmpty(const Game& g);

// Queries
int  shellsLeft(const Game& g);
int  liveLeft(const Game& g);
int  blankLeft(const Game& g);
Shell currentShell(const Game& g);

// Player actions (return a short result string for the log/feedback)
// `outFired` set true if a shell was discharged (so UI can do gun fx).
struct ActionResult {
    bool fired = false;
    bool wasLive = false;
    bool turnPassed = false;
    bool roundEnded = false;
    bool gameEnded = false;
    std::string text;
};

bool canUseItem(const Game& g, Actor who, Item it);
ActionResult playerUseItem(Game& g, Item it, int adrenTargetIndex = -1);
ActionResult playerShoot(Game& g, Actor target); // target self or dealer

// Dealer turn: produce one atomic step (call repeatedly until turn passes).
struct DealerStep {
    DealerActKind kind = DA_NONE;
    Item item = IT_NONE;
    ActionResult result;
    std::string think;   // short "tell" for flavor
    bool endsTurn = false;
};
DealerStep dealerStep(Game& g);

// helpers exposed for UI reveal
int playerKnow(const Game& g, int index);  // -1 unknown
