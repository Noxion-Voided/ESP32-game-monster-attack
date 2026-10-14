/*
  LIMBUS-LITE (Adafruit SSD1306 version)
  ESP32 + OLED 128x64 (SSD1306, I2C) + 4 buttons (no buzzer)
  ----------------------------------------------------------------
  Libraries (Library Manager): "Adafruit SSD1306", "Adafruit GFX Library"
                               (Adafruit BusIO is installed automatically)
  Board     : ESP32 Dev Module (Arduino core 2.x or 3.x)

  Wiring
    OLED SDA -> GPIO21, SCL -> GPIO22, VCC 3.3V, GND
    Buttons (other leg to GND, INPUT_PULLUP):
      LEFT  -> GPIO32
      RIGHT -> GPIO33
      OK    -> GPIO25
      GUARD -> GPIO26

  Rules
    - Each turn both sides roll Speed (1-6). Faster side wins clash ties and hits first.
    - Pick a skill (LEFT/RIGHT, OK) or GUARD (skip clash, take 60% less damage, +5 Sanity).
    - CLASH: press OK to flip. Power = base + coinPower per HEADS coin.
      Loser loses 1 coin, winner +5 Sanity, loser -5. Repeat until one side has 0 coins.
    - Surviving coins then hit the other side one by one.
    - Sanity (-45..+45) shifts heads chance: 50% + Sanity.
    - Enemy has a WEAK sin (x1.5) and a RESIST sin (x0.75).
    - HP <= 50% and <= 20% => STAGGER: skip next turn, take x1.5 damage.
    - Best win streak is saved in flash.
*/

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>

// Types must be declared before any function (Arduino auto-prototypes)
struct Skill { const char* name; uint8_t base, coinPow, coins, sin; };
struct Unit {
  int hp, maxHp, sanity;
  bool stagger, fresh;        // fresh = became staggered this turn
  uint8_t staggerLeft;        // thresholds remaining (50%, 20%)
};

// ======================= Hardware =======================
const uint8_t BTN_PIN[4] = {32, 33, 25, 26};
enum { B_LEFT = 0, B_RIGHT = 1, B_OK = 2, B_GUARD = 3 };

#define SCREEN_W 128
#define SCREEN_H 64
#define OLED_ADDR 0x3C            // some modules use 0x3D
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1, 400000UL, 400000UL);
Preferences prefs;

// ======================= Buttons =======================
bool btnState[4], btnFlag[4];
uint32_t btnTime[4];

void updateButtons() {
  for (int i = 0; i < 4; i++) {
    bool s = (digitalRead(BTN_PIN[i]) == LOW);
    if (s != btnState[i] && millis() - btnTime[i] > 25) {
      btnTime[i] = millis();
      btnState[i] = s;
      if (s) btnFlag[i] = true;
    }
  }
}
bool pressed(int i) {
  if (btnFlag[i]) { btnFlag[i] = false; return true; }
  return false;
}
void clearPresses() {
  updateButtons();
  for (int i = 0; i < 4; i++) btnFlag[i] = false;
}
void waitBtn(int i) {
  clearPresses();
  while (true) {
    updateButtons();
    if (pressed(i)) return;
    delay(5);
  }
}

// ======================= Game data =======================

const Skill P_SKILLS[3] = {
  {"Slash",  4, 3, 2, 0},   // Wrath
  {"Pierce", 3, 4, 2, 1},   // Lust
  {"Crush",  2, 5, 3, 2},   // Sloth
};
const Skill E_SKILLS[3] = {
  {"Claw",  4, 3, 2, 0},
  {"Sting", 3, 3, 3, 1},
  {"Smash", 2, 6, 2, 2},
};
const char* SIN_LONG[3]  = {"WRATH", "LUST", "SLOTH"};
const char* SIN_SHORT[3] = {"WRA", "LUS", "SLO"};

Unit player, enemy;

int enemyWeak, enemyResist;
int pSpeed, eSpeed;
const Skill* curE = &E_SKILLS[0];
int level = 0;                // current win streak
uint32_t bestStreak = 0;

// ======================= Helpers =======================
bool flip(int sanity) {
  int c = constrain(50 + sanity, 5, 95);
  return random(100) < c;
}
int rollPower(const Skill& s, int coins, int sanity, bool* heads) {
  int p = s.base;
  for (int i = 0; i < coins; i++) {
    heads[i] = flip(sanity);
    if (heads[i]) p += s.coinPow;
  }
  return p;
}
void addSan(Unit& u, int d) { u.sanity = constrain(u.sanity + d, -45, 45); }

// ======================= Drawing helpers =======================
// Text: y is the BASELINE (bottom of letters), sz = text size (1 = 6x8 cell, 2 = 12x16)
void txt(int x, int y, const char* s, int sz = 1) {
  display.setTextSize(sz);
  display.setCursor(x, y - 7 * sz);
  display.print(s);
}
int tw(const char* s, int sz = 1) { return (int)strlen(s) * 6 * sz; }
void txtC(const char* s, int y, int sz = 1) { txt((SCREEN_W - tw(s, sz)) / 2, y, s, sz); }

void fillEllipse(int cx, int cy, int rx, int ry) {
  for (int dy = -ry; dy <= ry; dy++) {
    float t = 1.0f - (float)(dy * dy) / (float)(ry * ry);
    int hw = (int)(rx * sqrtf(t) + 0.5f);
    display.drawFastHLine(cx - hw, cy + dy, 2 * hw + 1, SSD1306_WHITE);
  }
}

void bar(int x, int y, int w, int h, int v, int mx) {
  display.drawRect(x, y, w, h, SSD1306_WHITE);
  int f = (int)((long)(w - 2) * max(v, 0) / mx);
  if (f > 0) display.fillRect(x + 1, y + 1, f, h - 2, SSD1306_WHITE);
}

void hud() {
  char b[8];
  txt(0, 7, "FOE");
  bar(20, 0, 72, 8, enemy.hp, enemy.maxHp);
  if (enemy.stagger) txt(96, 7, "STAG!");
  else { snprintf(b, sizeof(b), "W:%s", SIN_SHORT[enemyWeak]); txt(96, 7, b); }

  txt(0, 63, "YOU");
  bar(20, 56, 72, 8, player.hp, player.maxHp);
  if (player.stagger) txt(96, 63, "STAG!");
  else { snprintf(b, sizeof(b), "S%+d", player.sanity); txt(96, 63, b); }
}

void banner(const char* a, const char* b, int ms) {
  display.clearDisplay();
  int sz = ((int)strlen(a) * 12 <= SCREEN_W) ? 2 : 1;
  txtC(a, 28, sz);
  txtC(b, 46, 1);
  display.display();
  delay(ms);
}

// state: 0 = not flipped, 1 = heads, 2 = tails ; spin >= 0 draws a spinning coin
void drawCoin(int cx, int cy, int state, int spin, int r = 5) {
  if (spin >= 0) {
    static const uint8_t w[6] = {5, 4, 2, 1, 2, 4};
    fillEllipse(cx, cy, max(1, (int)w[spin % 6] * r / 5), r);
    return;
  }
  if (state == 1) {
    display.fillCircle(cx, cy, r, SSD1306_WHITE);
  } else {
    display.drawCircle(cx, cy, r, SSD1306_WHITE);
    if (state == 2) {
      display.drawLine(cx - 2, cy - 2, cx + 2, cy + 2, SSD1306_WHITE);
      display.drawLine(cx - 2, cy + 2, cx + 2, cy - 2, SSD1306_WHITE);
    }
  }
}

void drawClash(const Skill& ps, int pc, const Skill& es, int ec,
               const bool* ph, const bool* eh, int pp, int ep, int spin, const char* foot) {
  char b[8];
  display.clearDisplay();
  txt(0, 7, ps.name);
  txt(SCREEN_W - tw(es.name), 7, es.name);
  txt(58, 7, "VS");
  display.drawFastVLine(63, 12, 38, SSD1306_WHITE);
  for (int i = 0; i < pc; i++) {
    int cx = 30 - (pc - 1) * 7 + i * 14;
    drawCoin(cx, 24, ph ? (ph[i] ? 1 : 2) : 0, spin);
  }
  for (int i = 0; i < ec; i++) {
    int cx = 97 - (ec - 1) * 7 + i * 14;
    drawCoin(cx, 24, eh ? (eh[i] ? 1 : 2) : 0, spin);
  }
  if (pp >= 0) { snprintf(b, sizeof(b), "%d", pp); txt(30 - tw(b, 2) / 2, 48, b, 2); }
  if (ep >= 0) { snprintf(b, sizeof(b), "%d", ep); txt(97 - tw(b, 2) / 2, 48, b, 2); }
  txtC(foot, 62, 1);
  display.display();
}

// ======================= Clash =======================
void doClash(const Skill& ps, int& pc, const Skill& es, int& ec) {
  bool ph[3], eh[3];
  while (pc > 0 && ec > 0) {
    drawClash(ps, pc, es, ec, nullptr, nullptr, -1, -1, -1, "OK = FLIP");
    waitBtn(B_OK);
    for (int f = 0; f < 12; f++) {
      drawClash(ps, pc, es, ec, nullptr, nullptr, -1, -1, f, "");
      delay(30);
    }
    int pp = rollPower(ps, pc, player.sanity, ph);
    int ep = rollPower(es, ec, enemy.sanity, eh);
    bool pWin = (pp > ep) || (pp == ep && pSpeed >= eSpeed);
    drawClash(ps, pc, es, ec, ph, eh, pp, ep, -1, pWin ? "WIN  +SANITY" : "LOSE  -SANITY");
    if (pWin) {
      ec--; addSan(player, 5); addSan(enemy, -5);
    } else {
      pc--; addSan(enemy, 5); addSan(player, -5);
    }
    delay(900);
  }
}

// ======================= Hits =======================
bool checkStagger(Unit& u) {
  if (u.hp <= 0) return false;
  int pct = u.hp * 100 / u.maxHp;
  if (u.staggerLeft == 2 && pct <= 50) { u.staggerLeft = 1; u.stagger = true; u.fresh = true; return true; }
  if (u.staggerLeft == 1 && pct <= 20) { u.staggerLeft = 0; u.stagger = true; u.fresh = true; return true; }
  return false;
}

void showHit(const Skill& s, int spin, bool heads, int dmg, const char* note) {
  char b[10];
  display.clearDisplay();
  hud();
  txtC(s.name, 19, 1);
  drawCoin(64, 31, spin < 0 ? (heads ? 1 : 2) : 0, spin);
  if (spin < 0) {
    snprintf(b, sizeof(b), "-%d", dmg);
    txtC(b, 52, 2);
    if (note[0]) txt(88, 48, note, 1);
  }
  display.display();
}

void doHits(const Skill& s, int coins, Unit& atk, Unit& def,
            bool toEnemy, bool defStag, bool defGuard) {
  for (int i = 0; i < coins && def.hp > 0; i++) {
    bool h = flip(atk.sanity);
    int dmg = (s.base + (h ? s.coinPow : 0)) * 2;
    const char* note = "";
    if (toEnemy && s.sin == enemyWeak)        { dmg = dmg * 3 / 2; note = "WEAK!"; }
    else if (toEnemy && s.sin == enemyResist) { dmg = dmg * 3 / 4; note = "RESIST"; }
    if (defStag)  { dmg = dmg * 3 / 2; if (!note[0]) note = "x1.5"; }
    if (defGuard) { dmg = dmg * 2 / 5; note = "GUARD"; }
    for (int f = 0; f < 6; f++) { showHit(s, f, false, 0, ""); delay(30); }
    def.hp = max(0, def.hp - dmg);
    showHit(s, -1, h, dmg, note);
    delay(650);
    if (checkStagger(def)) { banner("STAGGER!", toEnemy ? "Foe is open" : "You are open", 800); }
  }
}

// ======================= Skill menu =======================
void drawMenu(int sel) {
  char b[24];
  const Skill& s = P_SKILLS[sel];
  display.clearDisplay();
  hud();
  snprintf(b, sizeof(b), "<%s>", s.name);
  txtC(b, 24, 2);
  snprintf(b, sizeof(b), "PWR %d+%d  %s", s.base, s.coinPow, SIN_LONG[s.sin]);
  txtC(b, 33, 1);
  for (int i = 0; i < s.coins; i++) drawCoin(10 + i * 11, 42, 0, -1, 4);
  snprintf(b, sizeof(b), "SPD %d v %d", pSpeed, eSpeed);
  txt(60, 44, b);
  txt(0, 54, "G:GUARD");
  snprintf(b, sizeof(b), "FOE:%s", curE->name);
  txt(62, 54, b);
  display.display();
}

int selectSkill() {         // 0..2 skill, 3 = guard
  int sel = 0;
  clearPresses();
  drawMenu(sel);
  while (true) {
    updateButtons();
    bool dirty = false;
    if (pressed(B_LEFT))  { sel = (sel + 2) % 3; dirty = true; }
    if (pressed(B_RIGHT)) { sel = (sel + 1) % 3; dirty = true; }
    if (pressed(B_OK))    { return sel; }
    if (pressed(B_GUARD)) { return 3; }
    if (dirty) drawMenu(sel);
    delay(5);
  }
}

// ======================= Turn / battle =======================
void endTurnUnit(Unit& u) {
  if (!u.fresh) u.stagger = false;
  u.fresh = false;
}

void playTurn() {
  pSpeed = random(1, 7);
  eSpeed = random(1, 7);
  int ei = random(3);
  const Skill& es = E_SKILLS[ei];
  curE = &es;

  bool pStag = player.stagger, eStag = enemy.stagger;
  int pi;
  if (pStag) { banner("STAGGERED", "You cannot act", 900); pi = -1; }
  else pi = selectSkill();

  bool pGuard = (pi == 3);
  bool pSkill = (pi >= 0 && pi < 3);
  const Skill& ps = P_SKILLS[pSkill ? pi : 0];
  int pc = pSkill ? ps.coins : 0;
  int ec = eStag ? 0 : es.coins;

  if (pGuard) { addSan(player, 5); banner("GUARD", "Damage -60%", 500); }
  if (eStag)  { banner("FOE STAGGERED", "Free attack!", 600); }

  if (pc > 0 && ec > 0) doClash(ps, pc, es, ec);

  bool pFirst = (pSpeed >= eSpeed);
  for (int k = 0; k < 2; k++) {
    if (player.hp <= 0 || enemy.hp <= 0) break;
    bool playerActs = ((k == 0) == pFirst);
    if (playerActs && pc > 0)  doHits(ps, pc, player, enemy, true, eStag, false);
    if (!playerActs && ec > 0) doHits(es, ec, enemy, player, false, pStag, pGuard);
  }
  endTurnUnit(player);
  endTurnUnit(enemy);
}

void title() {
  char b[24];
  display.clearDisplay();
  txtC("LIMBUS", 24, 2);
  txtC("L I T E", 35, 1);
  snprintf(b, sizeof(b), "BEST STREAK: %u", (unsigned)bestStreak);
  txtC(b, 48, 1);
  txtC("PRESS OK", 61, 1);
  display.display();
  waitBtn(B_OK);
}

void battle() {
  char b[24];
  player = {100, 100, 0, false, false, 2};
  int eHp = min(80 + level * 15, 220);
  enemy = {eHp, eHp, 0, false, false, 2};
  enemyWeak = random(3);
  enemyResist = (enemyWeak + 1 + random(2)) % 3;

  snprintf(b, sizeof(b), "BATTLE %d", level + 1);
  char w[24];
  snprintf(w, sizeof(w), "Weak:%s", SIN_LONG[enemyWeak]);
  banner(b, w, 1400);

  while (player.hp > 0 && enemy.hp > 0) playTurn();

  if (enemy.hp <= 0) {
    level++;
    if ((uint32_t)level > bestStreak) { bestStreak = level; prefs.putUInt("best", bestStreak); }
    snprintf(b, sizeof(b), "Streak: %d", level);
    banner("VICTORY", b, 1800);
  } else {
    level = 0;
    snprintf(b, sizeof(b), "Best: %u", (unsigned)bestStreak);
    banner("DEFEATED", b, 1800);
  }
}

// ======================= Arduino =======================
void setup() {
  Serial.begin(115200);
  for (int i = 0; i < 4; i++) pinMode(BTN_PIN[i], INPUT_PULLUP);
  Wire.begin(21, 22);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("SSD1306 not found (check wiring / address 0x3C or 0x3D)"));
    for (;;) delay(1000);
  }
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);
  display.clearDisplay();
  display.display();
  randomSeed(esp_random());
  prefs.begin("limbus", false);
  bestStreak = prefs.getUInt("best", 0);
}

void loop() {
  title();
  battle();
}
