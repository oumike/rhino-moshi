#include "ui.h"

#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <WiFi.h>
#include <time.h>

#include <vector>

#include "board.h"
#include "config_server.h"
#include "fonts/FreeSans7pt7b.h"
#if HAS_TRANSCRIPT
#include "fonts/TermMono14.h"
#endif
#include "panel.h"
#include "settings.h"

namespace ui {
namespace {

Adafruit_GFX& display = panel::gfx();

// The layout is the X4's, drawn for its 480x800 portrait screen. The M9's
// 320x240 one is two thirds as wide, so there every measurement goes through
// S() and the fonts are a size down; a few heights are set outright.
#if LAYOUT_COMPACT
constexpr int S(int n) { return (n * 2 + (n >= 0 ? 1 : -1)) / 3; }
const GFXfont* F_TITLE = &FreeSansBold12pt7b;
const GFXfont* F_BOLD = &FreeSansBold9pt7b;
const GFXfont* F_TEXT = &FreeSans9pt7b;
const GFXfont* F_SMALL = &FreeSans7pt7b;
const int HEADER_H = 32;
const int FOOTER_H = 0;  // M9 has physical keys; no on-screen hint row
const int ROW_H = 46;
const int OPTION_H = 28;       // option row without a description
const int OPTION_DESC_H = 42;  // with one
// Baselines within a row; the smaller fonts shrink less than two thirds.
const int ROW_NAME_Y = 20, ROW_SUB_Y = 38;
const int OPT_LABEL_Y = 18, OPT_HINT_Y = 33;
#else
constexpr int S(int n) { return n; }
const GFXfont* F_TITLE = &FreeSansBold18pt7b;
const GFXfont* F_BOLD = &FreeSansBold12pt7b;
const GFXfont* F_TEXT = &FreeSans12pt7b;
const GFXfont* F_SMALL = &FreeSans9pt7b;
const int HEADER_H = 54;
const int FOOTER_H = 58;
const int ROW_H = 76;
const int OPTION_H = 44;
const int OPTION_DESC_H = 66;
const int ROW_NAME_Y = 32, ROW_SUB_Y = 62;
const int OPT_LABEL_Y = 29, OPT_HINT_Y = 54;
#endif

const int MARGIN = S(14);
const int ICON_X = S(30);  // icon centre
const int TEXT_X = S(54);  // row text start

// Partial refreshes leave ghosting; clean it up with a full one now and then.
const int FULL_EVERY = 15;
// Gateway pushes can arrive several times a second; batch them.
const unsigned long MIN_REDRAW_MS = 1000;
const unsigned long NOTICE_MS = 4000;
// With several agents blocked and nothing answerable on the watched one,
// move the watch on after this long.
const unsigned long ROTATE_MS = 10000;
// While an agent works its status line ticks every second; redraw for that
// no more often than this.
const unsigned long ACTIVITY_MS = 10000;
const unsigned long HOLD_MS = Buttons::HOLD_MS;  // Back held this long cancels the keyboard

enum class View { None, Message, Status, List, Detail, Card, Keys, Prompts, Keyboard, Menu, Usage, Hosts, Device, Home };
enum class Screen { List, Detail, Card, Keys, Prompts, Keyboard, Menu, Usage, Hosts, Device, Home };

// Screens about one agent; the rest work without any.
bool agentScreen(Screen s) { return s != Screen::List && s < Screen::Menu; }

uint8_t rotation = DEFAULT_ROTATION;
int partials = 0;
View lastView = View::None;
String lastMessage;

// Keyboard builds (M9) have a Home key and boot to a dashboard of every
// session; the X4 boots to the agent list.
#define HAS_DASHBOARD HAS_TRANSCRIPT
#if HAS_DASHBOARD
const Screen HOME = Screen::Home;
#else
const Screen HOME = Screen::List;
#endif
Screen screen = HOME;
String selectedId;
int cursor = 0;
bool pending = false;  // gateway changes not drawn yet
bool redrawNow = false;  // a send finished: show its result
unsigned long lastDrawAt = 0;
unsigned long inputAt = 0;  // when the press being drawn was made, for timing
// When the card on screen was first drawn, and for which request: presses
// made before it was visible were meant for something else.
unsigned long cardShownAt = 0;
String shownCardKey;
long lastMinute = -1;

String notice;  // a short result line under the header, e.g. "Approved"
unsigned long noticeUntil = 0;

// Card state. The selection belongs to the request `cardKey`; a new request
// starts over.
Screen cardReturn = Screen::List;
String cardKey;
size_t qIndex = 0;
int choices[Card::MAX_QUESTIONS];
int scroll = 0;           // first text line shown (message or plan)
bool scrollMore = false;  // the last textArea() had more lines below
int pageLines = 1;        // lines the last textArea() showed
enum class Send { None, Approve, Deny, Choice, Key, Prompt } sendAfterDraw = Send::None, inFlight = Send::None;

// Keys and Prompts menus, and the keyboard.
struct KeyItem {
  const char* label;
  const char* key;  // gateway key name
  const char* hint;
};
const KeyItem KEYS[] = {
    {"Esc", "Escape", "Stop, or back out of a dialog"},
    {"Ctrl-C", "C-c", "Interrupt"},
    {"Enter", "Enter", ""},
    {"Shift-Tab", "BTab", "Next mode (Claude: plan, auto, ...)"},
    {"Tab", "Tab", ""},
    {"Up", "Up", ""},
    {"Down", "Down", ""},
};
const int KEY_COUNT = sizeof(KEYS) / sizeof(KEYS[0]);
int menuSel = 0;
std::vector<String> canned;  // the Prompts menu, below "Type a prompt..."
const char* sendKeyName = "";
String sendLabel;  // key label, for the notice
String sendText;   // prompt being sent

const int KB_UNITS = 10;  // keys per row; the bottom row's keys span several
const int KB_ROWS = 5;
const int KB_KEY_H = 56;
const char* const KB_LETTERS[4] = {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.?"};
const char* const KB_SYMBOLS[4] = {"1234567890", "!@#$%^&*()", "-_=+/\\:;\"`", "[]{}<>|~'?"};
struct WideKey {
  const char* label;
  int from, to;  // units
};
enum { KB_SHIFT, KB_SYM, KB_SPACE, KB_SEND };
const WideKey KB_BOTTOM[4] = {{"Shift", 0, 2}, {"#+=", 2, 4}, {"space", 4, 7}, {"Send", 7, 10}};
String draft;
int kbRow = 1, kbX = 0;  // kbX: unit under the selection, kept across rows
bool kbShift = false, kbSym = false, backHoldFired = false;

// The M9 conversation view: scroll position in rendered lines, whether to keep
// following the newest, and what the last draw measured (for the input task).
int trScroll = 0;
bool trFollow = true;
// A second tap of the same direction this soon after the first pages instead.
const unsigned long DOUBLE_TAP_MS = 400;
int trLastDir = 0;
unsigned long trLastDirAt = 0;
int trMaxScroll = 0, trRows = 0;
bool trHasMore = false;

// The menu and its screens.
enum { M_USAGE, M_HOSTS, M_DEVICE, M_FLIP, M_SCREEN, M_RESTART, M_COUNT };
const char* const MENU_ITEMS[M_COUNT] = {"Usage", "Hosts", "Device", "Flip screen", "Screen off", "Restart"};
// The M9's panel has one good orientation, so it drops Flip; every other item
// is in order. The X4 keeps all of them.
#if defined(BOARD_M9)
const int MENU_ORDER[] = {M_USAGE, M_HOSTS, M_DEVICE, M_SCREEN, M_RESTART};
#else
const int MENU_ORDER[] = {M_USAGE, M_HOSTS, M_DEVICE, M_FLIP, M_RESTART};
#endif
const int MENU_LEN = sizeof(MENU_ORDER) / sizeof(MENU_ORDER[0]);
int mainSel = 0;

// The backlight goes off after this long without a key press (0: never). Any
// key turns it back on, and is swallowed; so does an agent starting to wait.
uint16_t screenTimeoutS = settings::DEFAULT_SCREEN_TIMEOUT;
unsigned long lastKeyAt = 0;
bool screenOff = false;
size_t lastBlocked = 0;

void setScreenOn(bool on) {
  if (on == !screenOff) return;
  screenOff = !on;
  panel::setBacklight(on);
  Serial.printf("[ui] screen %s\n", on ? "on" : "off");
}
int hostSel = 0, hostActive = 0;
std::vector<HostConfig> hostList;  // read when the menu opens
int usageFirst = 0;      // first account shown
bool usageMore = false;  // accounts below the last one shown
bool restartAfterDraw = false;

// What the agent page last drew from the live status, to redraw on change.
String drawnInfo, drawnActivity;
bool statusDirty = false;

// Which blocked agent the list-mode watch is on; see ROTATE_MS.
size_t rotate = 0;
String lastWatched;
unsigned long watchedSince = 0;

#if LAYOUT_COMPACT
int W() { return LAYOUT_W; }
int H() { return LAYOUT_H; }
#else
int W() { return display.width(); }
int H() { return display.height(); }
#endif

// The built-in fonts are 7-bit ASCII; map the punctuation agents like to use
// and drop the rest (emoji, CJK) rather than print garbage.
String ascii(const String& s, bool keepNewlines = false) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length();) {
    uint8_t c = s[i];
    if (c < 0x80) {
      out += c == '\n' && keepNewlines ? '\n' : c < 0x20 ? ' ' : (char)c;
      i++;
      continue;
    }
    int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    uint32_t cp = 0;
    if (n == 2 && i + 1 < s.length()) cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F);
    if (n == 3 && i + 2 < s.length()) cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
    switch (cp) {
      case 0x2026: out += "..."; break;
      case 0x2013:
      case 0x2014:
      case 0x2022:
      case 0x00B7: out += '-'; break;
      case 0x2018:
      case 0x2019: out += '\''; break;
      case 0x201C:
      case 0x201D: out += '"'; break;
      case 0x00A0: out += ' '; break;
      case 0x2192: out += "->"; break;
      case 0x2190: out += "<-"; break;
    }
    i += n;
  }
  return out;
}

int textWidth(const String& s) {
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return w;
}

// Truncate with "..." to fit `maxW` in the current font.
String fit(String s, int maxW) {
  if (s.length() > 200) s.remove(200);  // nothing wider fits a line anyway
  if (textWidth(s) <= maxW) return s;
  while (s.length() && textWidth(s + "...") > maxW) s.remove(s.length() - 1);
  return s + "...";
}

std::vector<String> wrap(const String& text, int maxW, int maxLines) {
  std::vector<String> lines;
  String line;
  int i = 0, n = text.length();
  while (i < n) {
    while (i < n && text[i] == ' ') i++;
    int j = i;
    while (j < n && text[j] != ' ') j++;
    if (i == j) break;
    String word = text.substring(i, j);
    String candidate = line.length() ? line + " " + word : word;
    if (textWidth(candidate) <= maxW) {
      line = candidate;
      i = j;
      continue;
    }
    if (line.isEmpty()) {  // a single word wider than the line: hard-break it
      String part;
      int k = i;
      while (k < j && textWidth(part + text[k]) <= maxW) part += text[k++];
      if (part.isEmpty()) part += text[k++];
      lines.push_back(part);
      i = k;
    } else {
      lines.push_back(line);
      line = "";
    }
    if ((int)lines.size() == maxLines) {
      lines.back() = fit(lines.back() + " " + text.substring(i), maxW);
      return lines;
    }
  }
  if (line.length()) lines.push_back(line);
  return lines;
}

// Wraps text that may hold newlines; blank lines collapse to one. Stops after
// `maxLines`.
std::vector<String> layout(const String& s, int maxW, size_t maxLines) {
  std::vector<String> out;
  int start = 0, n = s.length();
  while (start <= n && out.size() < maxLines) {
    int nl = s.indexOf('\n', start);
    if (nl < 0) nl = n;
    String para = s.substring(start, nl);
    para.trim();
    if (para.isEmpty()) {
      if (out.size() && out.back().length()) out.push_back("");
    } else {
      for (const String& line : wrap(para, maxW, 10000)) {
        if (out.size() >= maxLines) break;
        out.push_back(line);
      }
    }
    start = nl + 1;
  }
  return out;
}

void text(int x, int baseline, const String& s, const GFXfont* font, uint16_t color = color::FG) {
  display.setFont(font);
  display.setTextColor(color);
  display.setCursor(x, baseline);
  display.print(s);
}

// Draws wrapped text and returns the baseline after the last line.
int paragraph(int y, const String& s, const GFXfont* font, int maxLines, int lineH) {
  display.setFont(font);
  for (const String& line : wrap(ascii(s), W() - 2 * MARGIN, maxLines)) {
    text(MARGIN, y, line, font);
    y += lineH;
  }
  return y;
}

// Set while update() holds the gateway lock; the refresh itself runs without
// it so the network task isn't held up for the half second the panel takes.
Gateway* locked = nullptr;

bool refresh() {
  if (locked) locked->unlock();
  bool more = panel::nextPage();
  if (locked) locked->lock();
  return more;
}

template <typename F>
void paint(bool full, F body) {
  unsigned long t0 = millis();
  if (full) partials = 0;
  else partials++;
  panel::startFrame(full, rotation);
  unsigned long drawMs = 0;
  do {
    unsigned long d0 = millis();
    display.fillScreen(color::BG);
    body();
    drawMs += millis() - d0;
  } while (refresh());
  if (DISPLAY_EPAPER) Serial.printf("[ui] %s refresh %lu ms (draw %lu)\n", full ? "full" : "partial", millis() - t0, drawMs);
  else Serial.printf("[ui] refresh %lu ms (draw %lu)\n", millis() - t0, drawMs);
}

String age(double since, bool approx) {
  time_t now = time(nullptr);
  if (now < 1700000000 || since <= 0) return "";
  long d = max(0L, (long)(now - (long)since));
  String s = d < 60 ? String("<1m") : d < 3600 ? String(d / 60) + "m" : d < 86400 ? String(d / 3600) + "h"
                                                                                    : String(d / 86400) + "d";
  return approx && d >= 60 ? ">" + s : s;
}

uint16_t stateColor(AgentState st) {
  switch (st) {
    case AgentState::Blocked: return color::BLOCKED;
    case AgentState::Working: return color::WORKING;
    case AgentState::Done: return color::DONE;
    case AgentState::Idle: return color::IDLE;
    default: return color::MUTED;
  }
}

#if LAYOUT_COMPACT
void icon(const Agent& a, int cx, int cy, uint16_t fg, uint16_t bg) {
  switch (a.state) {
    case AgentState::Blocked:
      display.fillCircle(cx, cy, 8, fg);
      display.fillRect(cx - 1, cy - 5, 3, 6, bg);
      display.fillRect(cx - 1, cy + 3, 3, 3, bg);
      break;
    case AgentState::Working:
      display.drawCircle(cx, cy, 7, fg);
      display.drawCircle(cx, cy, 6, fg);
      display.fillCircle(cx, cy, 3, fg);
      break;
    case AgentState::Done:
      for (int t = 0; t <= 1; t++) {
        display.drawLine(cx - 7, cy + t, cx - 2, cy + 5 + t, fg);
        display.drawLine(cx - 2, cy + 5 + t, cx + 7, cy - 5 + t, fg);
      }
      break;
    case AgentState::Idle:
      display.fillRect(cx - 6, cy - 1, 12, 3, fg);
      break;
    default:
      display.drawCircle(cx, cy, 6, fg);
      break;
  }
  if (a.unseen) display.fillCircle(cx, cy + 15, 3, fg);
}
#else
void icon(const Agent& a, int cx, int cy, uint16_t fg, uint16_t bg) {
  switch (a.state) {
    case AgentState::Blocked:
      display.fillCircle(cx, cy, 12, fg);
      display.fillRect(cx - 2, cy - 8, 5, 10, bg);
      display.fillRect(cx - 2, cy + 5, 5, 4, bg);
      break;
    case AgentState::Working:
      display.drawCircle(cx, cy, 11, fg);
      display.drawCircle(cx, cy, 10, fg);
      display.fillCircle(cx, cy, 4, fg);
      break;
    case AgentState::Done:
      for (int t = -1; t <= 1; t++) {
        display.drawLine(cx - 10, cy + t, cx - 3, cy + 7 + t, fg);
        display.drawLine(cx - 3, cy + 7 + t, cx + 10, cy - 8 + t, fg);
      }
      break;
    case AgentState::Idle:
      display.fillRect(cx - 9, cy - 1, 18, 3, fg);
      break;
    default:
      display.drawCircle(cx, cy, 9, fg);
      break;
  }
  if (a.unseen) display.fillCircle(cx, cy + 24, 5, fg);
}
#endif

String usageSummary(const Usage& u) {
  const UsageWindow* w = u.highest();
  return w ? ascii(w->label) + " " + String(w->used) + "%" : String();
}

void header(Gateway& gw, Agents& agents) {
  String right;
  uint16_t rc = color::MUTED;
  switch (gw.state()) {
    case LinkState::Online:
      if (!agents.loaded()) right = "loading";
      else if (agents.blockedCount()) right = String(agents.blockedCount()) + " waiting", rc = color::BLOCKED;
      else right = "online", rc = color::DONE;
      break;
    case LinkState::Connecting: right = "connecting"; break;
    default: right = "offline"; break;
  }
  const GFXfont* rf = LAYOUT_COMPACT ? F_SMALL : F_TEXT;
  display.setFont(rf);
  int rw = textWidth(right);
  text(W() - MARGIN - rw, S(36), right, rf, rc);
  String use = usageSummary(gw.usage());
  if (use.length()) {
    display.setFont(F_SMALL);
    rw += textWidth(use) + S(16);
    text(W() - MARGIN - rw, S(35), use, F_SMALL, color::MUTED);
  }
  display.setFont(F_BOLD);
  text(MARGIN, S(36), fit(ascii(gw.host().name), W() - 3 * MARGIN - rw), F_BOLD);
  display.fillRect(0, HEADER_H - S(3), W(), S(3), color::RULE);
}

bool noticeShown() { return notice.length() && (long)(noticeUntil - millis()) > 0; }

void showNotice(const String& s) {
  notice = s;
  noticeUntil = millis() + NOTICE_MS;
}

// Drawn last, over the top of the content.
void drawNotice() {
  if (!noticeShown()) return;
  display.fillRect(0, HEADER_H, W(), S(44), color::NOTICE_BG);
  display.setFont(F_BOLD);
  text(MARGIN, HEADER_H + S(30), fit(ascii(notice), W() - 2 * MARGIN), F_BOLD, color::NOTICE_FG);
}

#if LAYOUT_COMPACT
// The M9 has a full physical keyboard, so no on-screen key hints are drawn.
void footer(const char*, const char*, const char*, const char*) {}
#else
// Labels for the row of keys under the screen, each drawn above its key.
void footer(const char* back, const char* confirm, const char* left, const char* right) {
  const char* labels[BTN_COUNT] = {};
  labels[BTN_BACK] = back;
  labels[BTN_CONFIRM] = confirm;
  labels[BTN_LEFT] = left;
  labels[BTN_RIGHT] = right;
  int top = H() - FOOTER_H;
  int slot = W() / 4;
  display.fillRect(0, top, W(), 2, color::RULE);
  display.setFont(F_BOLD);
  for (int i = 0; i < 4; i++) {
    const char* label = labels[BOTTOM_ROW_ORDER[i]];
    if (i) display.drawFastVLine(i * slot, top + 10, FOOTER_H - 20, color::RULE);
    if (!label || !*label) continue;
    String s = fit(label, slot - 8);
    text(i * slot + (slot - textWidth(s)) / 2, top + 37, s, F_BOLD);
  }
}
#endif

int rowsPerPage() { return (H() - HEADER_H - FOOTER_H) / ROW_H; }

void drawList(Gateway& gw, Agents& agents) {
  header(gw, agents);
  int per = rowsPerPage();
  int pages = max(1, ((int)agents.size() + per - 1) / per);
  int page = cursor / per;

  if (!agents.loaded() || agents.size() == 0) {
    display.setFont(F_TEXT);
    String msg = agents.loaded() ? "No agents running." : "Loading agents...";
    text((W() - textWidth(msg)) / 2, H() / 2, msg, F_TEXT);
  }

  for (int r = 0; r < per; r++) {
    int i = page * per + r;
    if (i >= (int)agents.size()) break;
    const Agent& a = agents[i];
    int y = HEADER_H + r * ROW_H;
    bool sel = i == cursor;
    uint16_t fg = sel ? color::SEL_FG : color::FG, bg = sel ? color::SEL_BG : color::BG;
    if (sel) display.fillRect(0, y, W(), ROW_H, color::SEL_BG);
    icon(a, ICON_X, y + S(26), sel ? fg : stateColor(a.state), bg);

    String right = ascii(a.kind);
    String ago = age(a.changedAt, a.approx);
    if (ago.length()) right += "  " + ago;
    display.setFont(F_SMALL);
    int rw = textWidth(right);
    text(W() - MARGIN - rw, y + ROW_NAME_Y - 1, right, F_SMALL, sel ? fg : color::MUTED);

    String name = ascii(a.workspace);
    if (a.tab.length() && a.tab != "1") name += " / " + ascii(a.tab);
    display.setFont(F_BOLD);
    text(TEXT_X, y + ROW_NAME_Y, fit(name, W() - TEXT_X - MARGIN - rw - S(10)), F_BOLD, fg);

    String sub = a.title.length() ? ascii(a.title) : String(stateName(a.state));
    display.setFont(F_TEXT);
    text(TEXT_X, y + ROW_SUB_Y, fit(sub, W() - TEXT_X - MARGIN), F_TEXT, fg);
    if (!sel) display.drawFastHLine(TEXT_X, y + ROW_H - 1, W() - TEXT_X - MARGIN, color::RULE);
  }

  String pageInfo = pages > 1 ? String(page + 1) + "/" + String(pages) : String();
  footer("Menu", agents.size() ? "Open" : "", pages > 1 ? "Prev" : "", pages > 1 ? "Next" : "");
  if (pageInfo.length()) {
    display.setFont(F_SMALL);
    text(W() - MARGIN - textWidth(pageInfo), H() - FOOTER_H - S(8), pageInfo, F_SMALL, color::MUTED);
  }
}

String stateLine(const Agent& a) {
  String ago = age(a.changedAt, a.approx);
  switch (a.state) {
    case AgentState::Blocked: return ago.length() ? "Waiting for you, " + ago : "Waiting for you";
    case AgentState::Working: return ago.length() ? "Working for " + ago : "Working";
    case AgentState::Done: return ago.length() ? "Finished " + ago + " ago" : "Finished";
    case AgentState::Idle: return ago.length() ? "Idle for " + ago : "Idle";
    default: return "Status unknown";
  }
}

// "workspace / tab", "1 of 3" and "claude - waiting 5m" over a card.
// Returns the top of the space below.
int cardHead(Gateway& gw, Agents& agents, const Agent& a) {
  header(gw, agents);
  int y = HEADER_H + S(34);
  int pos = agents.blockedPosition(a.id);
  String right = pos ? String(pos) + " of " + String(agents.blockedCount()) : String();
  display.setFont(F_TEXT);
  int rw = right.length() ? textWidth(right) : 0;
  if (rw) text(W() - MARGIN - rw, y, right, F_TEXT, color::BLOCKED);
  String name = ascii(a.workspace);
  if (a.tab.length() && a.tab != "1") name += " / " + ascii(a.tab);
  display.setFont(F_BOLD);
  text(MARGIN, y, fit(name, W() - 3 * MARGIN - rw), F_BOLD);
  y += S(26);
  String sub = ascii(a.kind);
  String ago = age(a.changedAt, a.approx);
  if (a.state == AgentState::Blocked && ago.length()) sub += " - waiting " + ago;
  text(MARGIN, y, sub, F_SMALL, color::MUTED);
  y += S(14);
  display.fillRect(MARGIN, y, W() - 2 * MARGIN, S(2), color::RULE);
  return y + S(12);
}

// Text that may not fit: lines from `first` on, between top and bottom.
// Records whether more follows (scrollMore) and how many lines fit.
int textArea(int top, int bottom, const String& s, const GFXfont* font, int lineH, int first, bool hint = true) {
  display.setFont(font);
  int visible = max(1, (bottom - top) / lineH);
  std::vector<String> lines = layout(ascii(s, true), W() - 2 * MARGIN, first + visible + 1);
  if (first > 0 && first >= (int)lines.size()) {  // the text shrank under us
    scroll = 0;
    return textArea(top, bottom, s, font, lineH, 0, hint);
  }
  int y = top + lineH * 3 / 4;
  for (int i = first; i < (int)lines.size() && i < first + visible; i++, y += lineH) text(MARGIN, y, lines[i], font);
  scrollMore = (int)lines.size() > first + visible;
  pageLines = visible;
  if (scrollMore && hint) {
    display.setFont(F_SMALL);
    text(W() - MARGIN - textWidth("more..."), bottom + S(14), "more...", F_SMALL, color::MUTED);
  }
  return y - lineH * 3 / 4;
}

// Selectable rows from `top`, scrolled to keep `sel` (or -1) in view. A row
// with a hint is taller; all rows share one height.
void menuRows(const std::vector<String>& labels, const std::vector<String>& hints, int sel, int top, int bottom) {
  int n = labels.size();
  bool anyHint = false;
  for (const String& h : hints) anyHint |= h.length() > 0;
  int rowH = anyHint ? OPTION_DESC_H : OPTION_H;
  int rows = max(1, (bottom - top) / rowH);
  int first = sel >= rows ? sel - rows + 1 : 0;
  for (int r = 0; r < rows && first + r < n; r++) {
    int i = first + r, y = top + r * rowH;
    bool on = i == sel;
    uint16_t fg = on ? color::SEL_FG : color::FG;
    if (on) display.fillRect(0, y, W(), rowH - S(4), color::SEL_BG);
    else display.drawRect(MARGIN / 2, y, W() - MARGIN, rowH - S(4), color::RULE);
    display.setFont(F_BOLD);
    text(MARGIN, y + (anyHint ? OPT_LABEL_Y : S(29)), fit(labels[i], W() - 2 * MARGIN), F_BOLD, fg);
    if (i < (int)hints.size() && hints[i].length()) {
      display.setFont(F_SMALL);
      text(MARGIN + S(26), y + OPT_HINT_Y, fit(hints[i], W() - 2 * MARGIN - S(26)), F_SMALL, on ? fg : color::MUTED);
    }
  }
  if (first + rows < n) {
    display.setFont(F_SMALL);
    text(W() - MARGIN - textWidth("more..."), top + rows * rowH + S(12), "more...", F_SMALL, color::MUTED);
  }
}

// Numbered option rows for a card's question `q`.
void optionRows(const Card& c, size_t q, int sel, int top, int bottom) {
  std::vector<String> labels, hints;
  for (size_t i = 0; i < c.optionCount(q); i++) {
    labels.push_back(String(i + 1) + ". " + ascii(c.optionLabel(q, i)));
    hints.push_back(ascii(c.optionDescription(q, i)));
  }
  menuRows(labels, hints, sel, top, bottom);
}

// Why the device can't answer, above the footer.
void reasonLine(int bottom, const String& why) {
  display.setFont(F_BOLD);
  int y = bottom - S(36);
  for (const String& line : wrap(ascii(why), W() - 2 * MARGIN, 2)) {
    text(MARGIN, y, line, F_BOLD, color::BLOCKED);
    y += S(28);
  }
}

const char* nextLabel(Agents& agents) {
  int n = agents.nextBlocked(selectedId);
  return n >= 0 && agents[n].id != selectedId ? "Next" : "";
}

void drawCard(Gateway& gw, Agents& agents) {
  int ai = agents.indexOf(selectedId);
  if (ai < 0) return;
  const Agent& a = agents[ai];
  int y = cardHead(gw, agents, a);
  int bottom = H() - FOOTER_H - S(22);
  const Card& c = gw.card();
  const char* next = nextLabel(agents);

  if (!gw.cardPending()) {
    String msg = !c.received()                   ? "Loading..."
                 : a.state == AgentState::Blocked ? "It's waiting for you, but not on anything the " BOARD_NAME
                                                    " can answer. Check the terminal."
                                                  : "Nothing waiting.";
    textArea(y + S(20), bottom, msg, F_TEXT, S(30), 0);
    footer("Back", "", "", next);
    return;
  }

  String why;
  bool can = c.answerable(&why);
  int textBottom = can ? bottom : bottom - S(70);
  switch (c.kind()) {
    case Card::Kind::Approval: {
      String tool = *c.toolName() ? ascii(c.toolName()) : String("Permission");
      display.setFont(F_TITLE);
      text(MARGIN, y + S(34), fit(tool, W() - 2 * MARGIN), F_TITLE, color::BLOCKED);
      y += S(50);
      if (*c.approvalTitle()) y = textArea(y, y + 2 * S(30), c.approvalTitle(), F_TEXT, S(30), 0, false) + S(8);
      display.drawFastHLine(MARGIN, y, W() - 2 * MARGIN, color::RULE);
      textArea(y + S(8), textBottom, c.approvalMessage(), F_TEXT, S(28), scroll);
      if (!can) reasonLine(bottom, why);
      footer("Back", can ? "Approve" : "", can ? "Deny" : "", next);
      break;
    }

    case Card::Kind::Plan:
    case Card::Kind::Menu: {
      String title = *c.menuTitle() ? c.menuTitle() : "Plan ready";
      y = textArea(y, y + 3 * S(28), title, F_BOLD, S(28), 0, false) + S(8);
      int n = c.optionCount(0);
      int optsH = min(n, LAYOUT_COMPACT ? 3 : 5) * OPTION_H;
      int optsTop = textBottom - optsH;
      bool more = false;
      if (*c.plan()) {
        textArea(y, optsTop - S(12), c.plan(), F_SMALL, S(22), scroll);
        more = scrollMore || scroll > 0;
      }
      optionRows(c, 0, can ? choices[0] : -1, optsTop, textBottom);
      if (!can) reasonLine(bottom, why);
      footer("Back", can ? "Send" : "", more ? "More" : "", next);
      break;
    }

    case Card::Kind::Question: {
      size_t nq = c.questionCount();
      size_t q = min(qIndex, nq - 1);
      String head = ascii(c.questionHeader(q));
      if (nq > 1) head += (head.length() ? "  -  " : "") + String("question ") + (q + 1) + " of " + nq;
      if (head.length()) {
        text(MARGIN, y + S(16), head, F_SMALL, color::MUTED);
        y += S(24);
      }
      y = textArea(y, y + 4 * S(30), c.questionText(q), F_BOLD, S(30), 0, false) + S(12);
      optionRows(c, q, can ? choices[q] : -1, y, textBottom);
      if (!can) reasonLine(bottom, why);
      footer("Back", can ? (q + 1 < nq ? "Choose" : "Send") : "", can && q > 0 ? "Prev" : "", next);
      break;
    }

    default:  // too big to hold
      textArea(y + S(20), textBottom, "This request is too big for the " BOARD_NAME ".", F_TEXT, S(30), 0);
      reasonLine(bottom, why);
      footer("Back", "", "", next);
      break;
  }
}

bool hasSession(const Agent& a) { return a.session.length() > 0; }

// The watched agent's live status line ("Thinking... (12s)"), if any.
String activityText(Gateway& gw, const Agent& a) {
  if (gw.watchedId() != a.id || !gw.card().received()) return "";
  String s;
  for (JsonVariantConst line : gw.card().activity()) {
    if (s.length()) s += ' ';
    s += line | "";
  }
  s = ascii(s);
  s.trim();
  return s;
}

// "opus-5-5 - 95% context left"
String infoText(Gateway& gw, const Agent& a) {
  if (gw.watchedId() != a.id || !gw.card().received()) return "";
  const Card& c = gw.card();
  String s = ascii(c.modelName());
  int ctx = c.contextRemaining();
  if (ctx >= 0) s += String(s.length() ? " - " : "") + String(ctx) + "% context left";
  return s;
}

#if HAS_TRANSCRIPT
// The conversation and its compose row read like a terminal: a bitmap
// monospace font, tightly leaded.
const GFXfont* F_MONO = &TermMono14;
const int MONO_LINE_H = 16;

uint16_t roleColor(Transcript::Role r) {
  switch (r) {
    case Transcript::Role::User: return color::WORKING;      // blue: what you asked
    case Transcript::Role::Assistant: return color::FG;      // bright: the reply
    default: return color::MUTED;                            // dim: a tool step
  }
}

struct ConvLine {
  String text;
  uint16_t color;
  const GFXfont* font;
};

// Wrap every transcript item into drawable lines, oldest first, a blank line
// between messages.
void buildConvLines(const Transcript& t, int maxW, std::vector<ConvLine>& out) {
  for (const Transcript::Item& it : t.items()) {
    bool tool = it.role == Transcript::Role::Tool;
    display.setFont(F_MONO);
    String txt = ascii(tool ? "- " + it.text : it.text, true);
    for (const String& line : layout(txt, maxW, 2000)) out.push_back({line, roleColor(it.role), F_MONO});
    out.push_back({String(), 0, F_MONO});  // spacer
  }
  if (!out.empty()) out.pop_back();
}

// M9: the agent page is its scrollable conversation with an inline compose row.
void drawDetail(Gateway& gw, Agents& agents) {
  header(gw, agents);
  int i = agents.indexOf(selectedId);
  if (i < 0) return;
  const Agent& a = agents[i];

  int top = HEADER_H;
  display.setFont(F_BOLD);
  text(MARGIN, top + S(18), fit(ascii(a.workspace) + " - " + ascii(a.kind), W() - 2 * MARGIN), F_BOLD);
  int convTop = top + S(26);
  int composeH = S(42);
  int convBottom = H() - composeH;
  int lineH = MONO_LINE_H;
  trRows = max(1, (convBottom - convTop) / lineH);

  const Transcript& t = gw.transcript();
  trHasMore = t.hasMore();
  std::vector<ConvLine> lines;
  buildConvLines(t, W() - 2 * MARGIN, lines);
  trMaxScroll = max(0, (int)lines.size() - trRows);
  if (trFollow) trScroll = trMaxScroll;
  trScroll = constrain(trScroll, 0, trMaxScroll);

  if (lines.empty()) {
    text(MARGIN, convTop + lineH, t.received() ? "No conversation yet." : "Loading conversation...", F_MONO,
         color::MUTED);
  } else {
    int y = convTop + lineH;
    for (int r = 0; r < trRows && trScroll + r < (int)lines.size(); r++) {
      const ConvLine& cl = lines[trScroll + r];
      if (cl.text.length()) text(MARGIN, y, cl.text, cl.font, cl.color);
      y += lineH;
    }
  }
  if (trScroll > 0 || t.hasMore()) text(W() - MARGIN - S(8), convTop + S(4), "^", F_SMALL, color::MUTED);
  if (trScroll < trMaxScroll) text(W() - MARGIN - S(8), convBottom - S(6), "v", F_SMALL, color::MUTED);

  display.fillRect(0, convBottom, W(), S(2), color::RULE);
  display.setFont(F_MONO);
  int composeY = convBottom + S(26);
  if (draft.length()) {
    String shown = draft;
    int w = W() - 2 * MARGIN - textWidth("> ");
    while (shown.length() && textWidth(shown + "_") > w) shown.remove(0, 1);
    text(MARGIN, composeY, "> " + shown + "_", F_MONO);
  } else {
    text(MARGIN, composeY, "> Type to send a prompt", F_MONO, color::MUTED);
  }
}
#else
void drawDetail(Gateway& gw, Agents& agents) {
  header(gw, agents);
  int i = agents.indexOf(selectedId);
  if (i < 0) return;
  const Agent& a = agents[i];

  int y = HEADER_H + S(46);
  display.setFont(F_TITLE);
  text(MARGIN, y, fit(ascii(a.workspace), W() - 2 * MARGIN), F_TITLE);
  y += S(36);
  text(MARGIN, y, "Tab " + ascii(a.tab) + " - " + ascii(a.kind), F_TEXT, color::MUTED);
  y += S(48);
  icon(a, MARGIN + S(12), y - S(8), stateColor(a.state), color::BG);
  text(MARGIN + S(36), y, stateLine(a), F_BOLD, stateColor(a.state));
  y += S(40);

  drawnInfo = infoText(gw, a);
  drawnActivity = activityText(gw, a);
  if (drawnInfo.length()) {
    display.setFont(F_TEXT);
    text(MARGIN, y, fit(drawnInfo, W() - 2 * MARGIN), F_TEXT);
    y += S(36);
  }
  if (drawnActivity.length()) y = paragraph(y, "> " + drawnActivity, F_TEXT, LAYOUT_COMPACT ? 1 : 2, S(30)) + S(6);
  y += S(10);

  if (a.title.length()) y = paragraph(y, a.title, F_TEXT, LAYOUT_COMPACT ? 2 : 6, S(30)) + S(20);
  if (a.cwd.length() && (!LAYOUT_COMPACT || y + S(24) <= H() - FOOTER_H)) {
    text(MARGIN, y, "Folder", F_SMALL, color::MUTED);
    paragraph(y + S(24), a.cwd, F_SMALL, LAYOUT_COMPACT ? 1 : 3, S(22));
  }
  bool s = hasSession(a);
  footer("Back", gw.cardPending() && gw.watchedId() == a.id ? "Answer" : "", "", s ? "Prompt" : "");
}
#endif

void drawKeys(Gateway& gw, Agents& agents) {
  int ai = agents.indexOf(selectedId);
  if (ai < 0) return;
  int top = cardHead(gw, agents, agents[ai]);
  text(MARGIN, top + S(24), "Send a key", F_BOLD);
  std::vector<String> labels, hints;
  for (const KeyItem& k : KEYS) {
    labels.push_back(k.label);
    hints.push_back(k.hint);
  }
  menuRows(labels, hints, menuSel, top + S(40), H() - FOOTER_H - S(20));
  footer("Back", "Send", "", "");
}

void drawPrompts(Gateway& gw, Agents& agents) {
  int ai = agents.indexOf(selectedId);
  if (ai < 0) return;
  int top = cardHead(gw, agents, agents[ai]);
  text(MARGIN, top + S(24), "Send a prompt", F_BOLD);
  std::vector<String> labels;
#if CAN_COMPOSE
  labels.push_back(HAS_KEYBOARD ? "Type a prompt... (or just type)" : "Type a prompt...");
#endif
  for (const String& p : canned) labels.push_back(ascii(p));
  menuRows(labels, {}, menuSel, top + S(40), H() - FOOTER_H - S(20));
#if CAN_COMPOSE
  footer("Back", menuSel ? "Send" : "Type", "", menuSel ? "Edit" : "");
#else
  footer("Back", canned.size() ? "Send" : "", "", "");
#endif
}

int kbBottomKey(int x) {
  for (int k = 0; k < 4; k++)
    if (x >= KB_BOTTOM[k].from && x < KB_BOTTOM[k].to) return k;
  return 0;
}

char kbChar(int row, int x) {
  char c = (kbSym ? KB_SYMBOLS : KB_LETTERS)[row][x];
  return kbShift && c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

void drawKeyboard(Gateway& gw, Agents& agents) {
  int ai = agents.indexOf(selectedId);
  if (ai < 0) return;
  int top = cardHead(gw, agents, agents[ai]);
#if HAS_KEYBOARD
  int boxBottom = H() - FOOTER_H - S(34);
#else
  int unit = W() / KB_UNITS;
  int left = (W() - unit * KB_UNITS) / 2;
  int kbTop = H() - FOOTER_H - KB_ROWS * KB_KEY_H - 8;
  int boxBottom = kbTop - 34;
#endif

  // The draft, scrolled to its end, with a cursor.
  display.drawRect(MARGIN / 2, top, W() - MARGIN, boxBottom - top, color::RULE);
  display.setFont(F_TEXT);
  int lineH = S(30), visible = max(1, (boxBottom - top - S(12)) / lineH);
  std::vector<String> lines = wrap(ascii(draft) + "_", W() - 2 * MARGIN - S(8), 1000);
  int first = max(0, (int)lines.size() - visible);
  int y = top + S(30);
  for (int i = first; i < (int)lines.size(); i++, y += lineH) text(MARGIN + S(4), y, lines[i], F_TEXT);
  display.setFont(F_SMALL);
  text(MARGIN, boxBottom + S(22), HAS_KEYBOARD ? "Hold Del to cancel" : "Hold Back to cancel", F_SMALL, color::MUTED);
  String count = String(draft.length()) + "/" + String(settings::MAX_PROMPT_LEN);
  text(W() - MARGIN - textWidth(count), boxBottom + S(22), count, F_SMALL, color::MUTED);
#if HAS_KEYBOARD
  footer("Delete", "Send", "", "");
#else

  for (int r = 0; r < KB_ROWS; r++) {
    int ky = kbTop + r * KB_KEY_H;
    int keys = r < 4 ? KB_UNITS : 4;
    for (int k = 0; k < keys; k++) {
      int from = r < 4 ? k : KB_BOTTOM[k].from, to = r < 4 ? k + 1 : KB_BOTTOM[k].to;
      bool on = r == kbRow && (r < 4 ? k == kbX : k == kbBottomKey(kbX));
      bool latched = r == 4 && ((k == KB_SHIFT && kbShift) || (k == KB_SYM && kbSym));
      int kx = left + from * unit + 2, kw = (to - from) * unit - 4, kh = KB_KEY_H - 4;
      uint16_t fg = on ? color::SEL_FG : color::FG;
      if (on) display.fillRect(kx, ky, kw, kh, color::SEL_BG);
      else display.drawRect(kx, ky, kw, kh, color::RULE);
      if (latched) display.drawRect(kx + 3, ky + 3, kw - 6, kh - 6, fg);
      String label = r < 4 ? String(kbChar(r, k)) : String(k == KB_SYM && kbSym ? "abc" : KB_BOTTOM[k].label);
      const GFXfont* font = r < 4 ? F_BOLD : F_TEXT;
      display.setFont(font);
      text(kx + (kw - textWidth(label)) / 2, ky + kh / 2 + 8, label, font, fg);
    }
  }

  const char* confirm = "Type";
  if (kbRow == 4) {
    int k = kbBottomKey(kbX);
    confirm = k == KB_SHIFT ? "Shift" : k == KB_SYM ? (kbSym ? "Letters" : "Symbols") : k == KB_SPACE ? "Space" : "Send";
  }
  footer("Del", confirm, "<", ">");
#endif
}

struct StatusText {
  String title;
  String body;
  const char* confirm = "";
};

StatusText statusText(Gateway& gw) {
  String host = ascii(gw.host().name);
  String page = "http://" + WiFi.localIP().toString();
  StatusText s;
  switch (gw.state()) {
    case LinkState::NoHost:
      s.title = "No host set up";
      s.body = "Open " + page + " to add the machine running moshi-hook.";
      break;
    case LinkState::Connecting:
      s.title = "Connecting";
      s.body = "Connecting to " + host + "...";
      break;
    case LinkState::Waiting:
      s.title = "Can't reach " + host;
      s.body = gw.detail() + "\n\nRetrying automatically.";
      s.confirm = "Retry";
      break;
    case LinkState::HostKeyUnknown:
      s.title = "New host";
      s.body = host + " presented this host key:\n\n" + gw.fingerprint() +
               "\n\nCheck it on the host with:\nssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub\n\nTrust it only if "
               "they match.";
      s.confirm = "Trust";
      break;
    case LinkState::HostKeyChanged:
      s.title = "Host key changed!";
      s.body = host + " now presents\n" + gw.fingerprint() + "\n\nbut the " BOARD_NAME " trusted\n" +
               settings::pinnedHostKey() +
               "\n\nRefusing to connect. If the host was reinstalled, forget the pinned key at " + page + ".";
      break;
    case LinkState::AuthRefused:
      s.title = "Key not authorized";
      s.body = host + " refused the " BOARD_NAME "'s key. Copy the authorized_keys line from " + page +
               " and append it to ~/.ssh/authorized_keys on " + host + ".\n\nRetrying every 30 s.";
      s.confirm = "Retry";
      break;
    case LinkState::GatewayDown:
      s.title = "moshi-hook not answering";
      s.body = "SSH to " + host + " works, but nothing answers on the gateway. Is moshi-hook running?\n\n" +
               gw.detail();
      s.confirm = "Retry";
      break;
    case LinkState::Incompatible:
      s.title = "Upgrade moshi-hook";
      s.body = gw.detail() + "\n\nbrew upgrade moshi-hook on " + host + ".";
      s.confirm = "Retry";
      break;
    case LinkState::Online: break;
  }
  return s;
}

void drawStatus(Gateway& gw, Agents& agents) {
  header(gw, agents);
  StatusText s = statusText(gw);
  int y = HEADER_H + S(54);
  display.setFont(F_TITLE);
  for (const String& line : wrap(ascii(s.title), W() - 2 * MARGIN, 2)) {
    text(MARGIN, y, line, F_TITLE);
    y += S(40);
  }
  y += S(14);
  int start = 0;
  String body = s.body + "\n";
  while (start < (int)body.length() && y < H() - FOOTER_H - S(20)) {
    int nl = body.indexOf('\n', start);
    String para = body.substring(start, nl);
    if (para.length()) y = paragraph(y, para, F_TEXT, 8, S(30));
    else y += S(14);
    start = nl + 1;
  }
  footer("Menu", s.confirm, "", "");
}

void title(const char* s) { text(MARGIN, HEADER_H + S(46), s, F_TITLE); }

void drawMenu(Gateway& gw, Agents& agents) {
  header(gw, agents);
  title("Menu");
  const Usage& u = gw.usage();
  String use = usageSummary(u);
  String host = hostActive < (int)hostList.size() ? ascii(hostList[hostActive].name) : String("None set up");
  if (hostList.size() > 1) host += " - " + String(hostList.size()) + " hosts";
  String allHints[M_COUNT] = {
      use.length() ? "Most used: " + use : u.received() ? String("No accounts reported") : String("Nothing reported yet"),
      host,
      "Settings page, Wi-Fi, key, battery",
      "Turn the screen upside down",
      (screenTimeoutS ? "After " + settings::screenTimeoutName(screenTimeoutS) : String("Never")) + ", Enter changes it",
      "Start the " BOARD_NAME " over",
  };
  std::vector<String> labels, hints;
  for (int i = 0; i < MENU_LEN; i++) {
    labels.push_back(MENU_ITEMS[MENU_ORDER[i]]);
    hints.push_back(allHints[MENU_ORDER[i]]);
  }
  menuRows(labels, hints, mainSel, HEADER_H + S(70), H() - FOOTER_H - S(10));
  footer("Back", "Open", "", "");
}

String agentName(const String& agent) {
  if (agent == "claude-code") return "Claude Code";
  if (agent == "codex") return "Codex";
  return ascii(agent);
}

String span(long d) {
  if (d < 3600) return String(max(1L, d / 60)) + "m";
  if (d < 86400) return String(d / 3600) + "h " + String(d % 3600 / 60) + "m";
  return String(d / 86400) + "d " + String(d % 86400 / 3600) + "h";
}

#if HAS_DASHBOARD
// The host, its limits, and this device at a glance; the sessions themselves
// are a key away (Messages, or Enter for the list). Monospace rows: a muted
// label column, then the value.
void drawHome(Gateway& gw, Agents& agents) {
  header(gw, agents);
  const int LH = MONO_LINE_H, CW = 8;  // TermMono cell
  const int VX = MARGIN + 8 * CW;      // values start after an 8-cell label
  const int RX = W() - MARGIN;
  int y = HEADER_H + 2;
  display.setFont(F_MONO);

  auto label = [&](const String& l) { text(MARGIN, y + 12, l, F_MONO, color::MUTED); };
  auto value = [&](const String& v, uint16_t c = color::FG) {
    text(VX, y + 12, fit(v, RX - VX), F_MONO, c);
  };
  // A value drawn as a bar with text after it, e.g. "[====    ] 27% 2.05".
  auto bar = [&](int x, float frac, const String& right, uint16_t rc, bool warn) {
    int rx = RX - right.length() * CW;
    text(rx, y + 12, right, F_MONO, rc);
    int bw = rx - CW - x;
    if (bw < 12) return;
    display.drawRect(x, y + 4, bw, 8, color::RULE);
    display.fillRect(x + 2, y + 6, (int)((bw - 4) * constrain(frac, 0.0f, 1.0f)), 4, warn ? color::BLOCKED : color::BAR);
  };
  auto rule = [&] {
    y += 3;
    display.drawFastHLine(MARGIN, y, W() - 2 * MARGIN, color::RULE);
    y += 2;
  };
  auto pad = [](String s, unsigned n) {
    while (s.length() < n) s = " " + s;
    return s;
  };

  // moshi-hook and the host's load.
  const HostInfo& info = gw.hostInfo();
  label("MOSHI");
  {
    String v = info.version.length() ? info.version : String("?");
    if (info.doctor) v += info.pro ? "  Pro" : "  Free";
    value(v);
    if (info.updateAvailable) {
      String up = info.latest + " available";
      text(RX - up.length() * CW, y + 12, up, F_MONO, color::BLOCKED);
    } else if (info.updateChecked) {
      text(RX - 10 * CW, y + 12, "up to date", F_MONO, color::DONE);
    }
  }
  y += LH;
#if HAS_HOST_STATS
  const HostStats& st = gw.hostStats();
  label("CPU");
  if (st.ok) {
    char load[12];
    snprintf(load, sizeof(load), "%.2f", st.load);
    bar(VX, st.cpu / 100, pad(String((int)(st.cpu + 0.5f)) + "%", 4) + " ld " + load, color::FG, st.cpu >= 90);
    y += LH;
    label("MEM");
    char mem[24];
    snprintf(mem, sizeof(mem), "%.1f/%.0fG", st.memUsedMB / 1024.0f, st.memTotalMB / 1024.0f);
    float frac = st.memTotalMB ? (float)st.memUsedMB / st.memTotalMB : 0;
    bar(VX, frac, mem, color::FG, false);  // macOS counts its cache as used, so it always looks full
  } else {
    value(st.tried ? st.error : String("reading..."), st.tried ? color::BLOCKED : color::MUTED);
    if (st.tried) {
      y += LH;
      value("re-paste the device key line", color::MUTED);
    }
  }
  y += LH;
#endif

  // Plan limits, as many windows as fit.
  rule();
  time_t now = time(nullptr);
  bool clock = now > 1700000000;
  const std::vector<UsageAccount>& accs = gw.usage().accounts();
  const int BOTTOM_ROWS = 5;  // agents, spaces, servers, sims, device
  int room = max(1, (H() - y - 10 - BOTTOM_ROWS * LH) / LH);
  int shown = 0, total = 0;
  for (const UsageAccount& a : accs) total += a.windows.size();
  if (!total) {
    label("USAGE");
    value(gw.usage().received() ? "no accounts" : "waiting...", color::MUTED);
    y += LH;
  }
  for (const UsageAccount& a : accs) {
    for (const UsageWindow& w : a.windows) {
      if (shown == room - 1 && total > room) {
        label("USAGE");
        value("+" + String(total - shown) + " more in Menu > Usage", color::MUTED);
        y += LH;
        shown = total;
        break;
      }
      if (shown >= room) break;
      String name = agentName(a.agent);
      label(name.length() > 7 ? name.substring(0, 7) : name);
      String wl = ascii(w.label);
      text(VX, y + 12, wl, F_MONO);
      String reset = clock && w.resetsAt > 0 ? span(max(0L, (long)(w.resetsAt - now))) : String();
      bar(VX + (wl.length() + 1) * CW, w.used / 100.0f, pad(String(w.used) + "%", 4) + pad(reset, 7),
          w.used >= 90 ? color::BLOCKED : color::FG, w.used >= 90);
      y += LH;
      shown++;
    }
  }

  // What's running on the host.
  rule();
  label("AGENTS");
  {
    int n[5] = {};
    for (size_t i = 0; i < agents.size(); i++) n[(int)agents[i].state]++;
    int x = VX;
    auto part = [&](const String& t, uint16_t c) {
      if (x + (int)t.length() * CW > RX) return;
      text(x, y + 12, t, F_MONO, c);
      x += (t.length() + 1) * CW;
    };
    if (!agents.loaded()) part("loading...", color::MUTED);
    else {
      part(String(agents.size()), color::FG);
      if (n[(int)AgentState::Blocked]) part(String(n[(int)AgentState::Blocked]) + " waiting", color::BLOCKED);
      if (n[(int)AgentState::Working]) part(String(n[(int)AgentState::Working]) + " working", color::WORKING);
      if (n[(int)AgentState::Done]) part(String(n[(int)AgentState::Done]) + " done", color::DONE);
      if (n[(int)AgentState::Idle]) part(String(n[(int)AgentState::Idle]) + " idle", color::IDLE);
    }
  }
  y += LH;
  label("SPACES");
  if (agents.loaded()) {
    int ws = agents.workspaceCount(), tabs = agents.tabCount(), panes = agents.paneCount();
    value(String(ws) + (ws == 1 ? " workspace " : " workspaces ") + String(tabs) + (tabs == 1 ? " tab " : " tabs ") +
          String(panes) + (panes == 1 ? " pane" : " panes"));
  }
  y += LH;
  label("SERVERS");
  if (!info.doctor) value("...", color::MUTED);
  else if (info.servers.empty()) value("none", color::MUTED);
  else {
    String v;
    for (const HostInfo::Server& sv : info.servers) {
      String one = ":" + String(sv.port) + " " + ascii(sv.process.length() ? sv.process : sv.name);
      v += (v.length() ? "  " : "") + one;
    }
    value(v);
  }
  y += LH;
  label("SIMS");
  if (!info.doctor) value("...", color::MUTED);
  else if (info.simulators.empty()) value("none", color::MUTED);
  else {
    String v;
    for (const String& m : info.simulators) v += (v.length() ? ", " : "") + ascii(m);
    value(v);
  }
  y += LH;

  // This device.
  rule();
  label("M9");
  {
    float volts = analogReadMilliVolts(PIN_BATTERY_ADC) * BATTERY_DIVIDER / 1000.0f;
    char v[48];
    snprintf(v, sizeof(v), "%ddBm %.2fV up %s", (int)WiFi.RSSI(), volts, span(millis() / 1000).c_str());
    String s = v;
#if HAS_HOST_STATS
    if (st.tried && st.rttMs) s += " rtt " + String(st.rttMs) + "ms";
#endif
    value(s);
  }
}
#endif

void drawUsage(Gateway& gw, Agents& agents) {
  header(gw, agents);
  title("Usage");
  int y = HEADER_H + S(60);
  int bottom = H() - FOOTER_H - S(24);
  const Usage& u = gw.usage();
  const std::vector<UsageAccount>& accs = u.accounts();
  usageFirst = constrain(usageFirst, 0, max(0, (int)accs.size() - 1));
  usageMore = false;
  if (accs.empty()) {
    String host = ascii(gw.host().name);
    String msg = gw.state() != LinkState::Online ? "Not connected to " + host + "."
                 : u.received()                   ? "moshi-hook reports no accounts on " + host + "."
                                                  : String("Waiting for moshi-hook to report usage.");
    textArea(y + S(10), bottom, msg + "\n\nUsage comes from the Claude Code and Codex accounts signed in on the host.",
             F_TEXT, S(30), 0);
  }
  time_t now = time(nullptr);
  bool clock = now > 1700000000;
  for (int i = usageFirst; i < (int)accs.size(); i++) {
    const UsageAccount& a = accs[i];
    if (i > usageFirst && y + S(90) + (LAYOUT_COMPACT ? 37 : 90) * (int)a.windows.size() > bottom) {
      usageMore = true;
      break;
    }
    y += S(40);
    String ago = age(a.capturedAt, false);
    String right = ago.length() ? "updated " + ago + (ago == "<1m" ? "" : " ago") : String();
    display.setFont(F_SMALL);
    int rw = right.length() ? textWidth(right) : 0;
    if (rw) text(W() - MARGIN - rw, y, right, F_SMALL, color::MUTED);
    display.setFont(F_BOLD);
    text(MARGIN, y, fit(agentName(a.agent), W() - 3 * MARGIN - rw), F_BOLD);
    y += S(26);
    String label = a.label;
    label.replace("\xE2\x80\xA2", "*");  // the masked e-mail's bullets
    display.setFont(F_SMALL);
    text(MARGIN, y, fit(ascii(label), W() - 2 * MARGIN), F_SMALL, color::MUTED);
    y += S(14);
    for (const UsageWindow& w : a.windows) {
#if LAYOUT_COMPACT
      {
      // One line per window ("5h  resets in 4h ... 12% used") over a thin bar.
      y += 18;
      String wl = ascii(w.label);
      display.setFont(F_BOLD);
      text(MARGIN, y, wl, F_BOLD);
      int lx = MARGIN + textWidth(wl) + 8;
      String pct = String(w.used) + "% used";
      display.setFont(F_TEXT);
      text(W() - MARGIN - textWidth(pct), y, pct, F_TEXT);
      if (clock && w.resetsAt > 0) {
        long left = (long)(w.resetsAt - now);
        text(lx, y, left > 0 ? "resets in " + span(left) : String("resetting now"), F_SMALL, color::MUTED);
      }
      y += 5;
      int bw = W() - 2 * MARGIN, bh = 10;
      display.drawRect(MARGIN, y, bw, bh, color::RULE);
      display.fillRect(MARGIN + 2, y + 2, (bw - 4) * min(w.used, 100) / 100, bh - 4,
                       w.used >= 90 ? color::BLOCKED : color::BAR);
      y += bh + 4;
      continue;
      }
#endif
      y += S(30);
      text(MARGIN, y, ascii(w.label), F_BOLD);
      String pct = String(w.used) + "% used";
      display.setFont(F_TEXT);
      text(W() - MARGIN - textWidth(pct), y, pct, F_TEXT);
      y += S(12);
      int bw = W() - 2 * MARGIN, bh = S(18);
      display.drawRect(MARGIN, y, bw, bh, color::RULE);
      display.fillRect(MARGIN + S(3), y + S(3), (bw - 2 * S(3)) * min(w.used, 100) / 100, bh - 2 * S(3),
                       w.used >= 90 ? color::BLOCKED : color::BAR);
      y += bh;
      if (clock && w.resetsAt > 0) {
        long left = (long)(w.resetsAt - now);
        y += S(22);
        text(MARGIN, y, left > 0 ? "resets in " + span(left) : String("resetting now"), F_SMALL, color::MUTED);
      }
      y += S(8);
    }
    y += S(12);
    display.drawFastHLine(MARGIN, y, W() - 2 * MARGIN, color::RULE);
  }
  if (usageMore) {
    display.setFont(F_SMALL);
    text(W() - MARGIN - textWidth("more..."), bottom + S(14), "more...", F_SMALL, color::MUTED);
  }
  footer("Back", "", "", "");
}

void drawHosts(Gateway& gw, Agents& agents) {
  header(gw, agents);
  title("Hosts");
  int top = HEADER_H + S(70);
  if (hostList.empty()) {
    textArea(top, H() - FOOTER_H - S(24),
             "No hosts set up. Add them on the settings page:\n\nhttp://" + WiFi.localIP().toString(), F_TEXT, S(30), 0);
    footer("Back", "", "", "");
    return;
  }
  std::vector<String> labels, hints;
  for (int i = 0; i < (int)hostList.size(); i++) {
    const HostConfig& h = hostList[i];
    labels.push_back(ascii(h.name.length() ? h.name : h.address));
    String hint = ascii(h.user + "@" + h.address);
    if (h.port != 22) hint += ":" + String(h.port);
    if (i == hostActive) hint += gw.state() == LinkState::Online ? " - connected" : " - current";
    hints.push_back(hint);
  }
  menuRows(labels, hints, hostSel, top, H() - FOOTER_H - S(10));
  bool current = hostSel == hostActive;
  footer("Back", !current ? "Connect" : gw.state() != LinkState::Online ? "Retry" : "", "", "");
}

void drawDevice(Gateway& gw, Agents& agents) {
  header(gw, agents);
  title("Device");
  int y = HEADER_H + S(60);
#if LAYOUT_COMPACT
  const int LABEL_W = 84, LINE_H = 15;
  y += 12 - LINE_H;
  auto row = [&](const char* label, const String& value, const GFXfont*, int) {
    y += LINE_H;
    text(MARGIN, y, label, F_SMALL, color::MUTED);
    display.setFont(F_SMALL);
    bool first = true;
    for (const String& line : wrap(value, W() - 2 * MARGIN - LABEL_W, 2)) {
      if (!first) y += LINE_H;
      text(MARGIN + LABEL_W, y, line, F_SMALL);
      first = false;
    }
  };
  row("Settings", String("http://") + config_server::HOSTNAME + ".local", F_TEXT, 0);
  row("", "http://" + WiFi.localIP().toString(), F_TEXT, 0);
#else
  auto row = [&](const char* label, const String& value, const GFXfont* font, int lineH) {
    y += S(30);
    text(MARGIN, y, label, F_SMALL, color::MUTED);
    y += lineH - S(4);
    display.setFont(font);
    for (const String& line : wrap(value, W() - 2 * MARGIN, 3)) {
      text(MARGIN, y, line, font);
      y += lineH;
    }
    y -= lineH;
  };
  row("Settings page", String("http://") + config_server::HOSTNAME + ".local", F_TEXT, S(30));
  y += S(30);
  text(MARGIN, y, "http://" + WiFi.localIP().toString(), F_TEXT);
#endif
  row("Wi-Fi", ascii(WiFi.SSID()) + "  (" + String(WiFi.RSSI()) + " dBm)", F_TEXT, S(30));
  row("Device key", gw.deviceKeyFingerprint(), F_SMALL, S(24));
  float volts = analogReadMilliVolts(PIN_BATTERY_ADC) * BATTERY_DIVIDER / 1000.0f;
  row("Battery", String(volts, 2) + " V", F_TEXT, S(30));
  row("Memory", String(ESP.getFreeHeap() / 1024) + " KB free, lowest " + String(ESP.getMinFreeHeap() / 1024) + " KB",
      F_TEXT, S(30));
  row("Running for", span(millis() / 1000), F_TEXT, S(30));
  row("Firmware", "built " __DATE__, F_TEXT, S(30));
  footer("Back", "", "", "");
}

void clampCursor(Agents& agents) {
  int i = selectedId.length() ? agents.indexOf(selectedId) : -1;
  if (i >= 0) cursor = i;
  cursor = constrain(cursor, 0, max(0, (int)agents.size() - 1));
  selectedId = agents.size() ? agents[cursor].id : String();
}

void moveCursor(Agents& agents, int delta) {
  if (!agents.size()) return;
  cursor = constrain(cursor + delta, 0, (int)agents.size() - 1);
  selectedId = agents[cursor].id;
}

void openCard(Screen from) {
  screen = Screen::Card;
  cardReturn = from;
  cardKey = "";
}

void closeCard() {
  screen = cardReturn;
  cardKey = "";
}

bool cardInput(Gateway& gw, Agents& agents, Buttons& b) {
  if (b.wasPressed(BTN_BACK)) {
    agents.setDismissed(selectedId, true);  // until it blocks on something new
    closeCard();
    return true;
  }
  if (b.wasPressed(BTN_RIGHT)) {
    int n = agents.nextBlocked(selectedId);
    if (n < 0 || agents[n].id == selectedId) return false;
    selectedId = agents[n].id;
    cursor = n;
    agents.setDismissed(selectedId, false);
    cardKey = "";
    return true;
  }
  if (!gw.cardPending()) return false;
  const Card& c = gw.card();
  bool can = c.answerable();
  switch (c.kind()) {
    case Card::Kind::Approval:
      if (!can) break;
      if (b.wasPressed(BTN_CONFIRM)) sendAfterDraw = Send::Approve;
      else if (b.wasPressed(BTN_LEFT)) sendAfterDraw = Send::Deny;
      else break;
      showNotice(sendAfterDraw == Send::Approve ? "Approving..." : "Denying...");
      return true;

    case Card::Kind::Plan:
    case Card::Kind::Menu: {
      if (b.wasPressed(BTN_LEFT)) {
        scroll = scrollMore ? scroll + pageLines : 0;  // page through, then back to the top
        return true;
      }
      if (!can) return false;
      int n = c.optionCount(0);
      if (b.wasPressed(BTN_DOWN)) choices[0] = min(choices[0] + 1, n - 1);
      else if (b.wasPressed(BTN_UP)) choices[0] = max(choices[0] - 1, 0);
      else if (b.wasPressed(BTN_CONFIRM)) {
        sendAfterDraw = Send::Choice;
        showNotice("Sending: " + String(c.optionLabel(0, choices[0])));
      } else return false;
      return true;
    }

    case Card::Kind::Question: {
      if (!can) return false;
      size_t nq = c.questionCount();
      qIndex = min(qIndex, nq - 1);
      int n = c.optionCount(qIndex);
      int& sel = choices[qIndex];
      if (b.wasPressed(BTN_DOWN)) sel = min(sel + 1, n - 1);
      else if (b.wasPressed(BTN_UP)) sel = max(sel - 1, 0);
      else if (b.wasPressed(BTN_LEFT) && qIndex > 0) qIndex--;
      else if (b.wasPressed(BTN_CONFIRM)) {
        if (qIndex + 1 < nq) {
          qIndex++;
        } else {
          sendAfterDraw = Send::Choice;
          showNotice(nq > 1 ? String("Sending answers...") : "Sending: " + String(c.optionLabel(qIndex, sel)));
        }
      } else return false;
      return true;
    }

    default: break;
  }
  // Read-only text and approvals: Up/Down scroll.
  if (b.wasPressed(BTN_DOWN) && scrollMore) scroll += 3;
  else if (b.wasPressed(BTN_UP) && scroll > 0) scroll = max(0, scroll - 3);
  else return false;
  return true;
}

// Picks the agent to watch: the open one, else a blocked one (rotating
// through them while the watched one has nothing this device can answer).
void steerWatch(Gateway& gw, Agents& agents) {
  const Agent* target = nullptr;
  bool automatic = false;
  int sel = agents.indexOf(selectedId);
  if (agentScreen(screen) && sel >= 0) {
    target = &agents[sel];
  } else {
    std::vector<int> waiting = agents.waitingOrder();
    if (waiting.size()) {
      if (rotate >= waiting.size()) rotate = 0;
      target = &agents[waiting[rotate]];
      automatic = true;
    }
  }
  gw.watch(target);
  if (gw.watchedId() != lastWatched) {
    lastWatched = gw.watchedId();
    watchedSince = millis();
  }
#if HAS_TRANSCRIPT
  // Follow the open agent's conversation across its screens; a new one resets
  // the scroll back to the newest message.
  const Agent* trTarget = (agentScreen(screen) && sel >= 0) ? &agents[sel] : nullptr;
  String before = gw.transcriptId();
  gw.watchTranscript(trTarget);
  if (gw.transcriptId() != before) {
    trScroll = 0;
    trFollow = true;
  }
#endif
  if (!automatic) return;

  if (screen == Screen::List && gw.cardPending() && !target->dismissed) {  // take over the list
    selectedId = target->id;
    cursor = agents.indexOf(selectedId);
    openCard(Screen::List);
  } else if (gw.card().received() && !gw.cardPending() && agents.waitingOrder().size() > 1 &&
             millis() - watchedSince > ROTATE_MS) {
    rotate++;
  }
}

// Starts the send just chosen; finishSend() reports how it went.
void startSend(Gateway& gw, Agents& agents, Send what) {
  String err;
  bool ok;
  if (what == Send::Key || what == Send::Prompt) {
    int i = agents.indexOf(selectedId);
    if (i < 0) {
      showNotice("Not sent: the tab closed");
      return;
    }
    ok = what == Send::Key ? gw.sendKey(agents[i], sendKeyName, err) : gw.sendPrompt(agents[i], sendText, err);
  } else {
    ok = gw.answer(what == Send::Approve, choices, err);
  }
  if (ok) inFlight = what;
  else showNotice("Not sent: " + err);
}

void finishSend(const Gateway::Result& r) {
  Send what = inFlight;
  inFlight = Send::None;
  bool ok = r.status == 200;
  switch (r.kind) {
    case Gateway::Request::Answer:
      if (ok || r.status == 409) {
        showNotice(!ok ? "Changed on the host: nothing sent"
                       : what == Send::Approve ? "Approved"
                       : what == Send::Deny    ? "Denied"
                                               : "Sent");
        if (screen == Screen::Card) closeCard();
      } else {
        showNotice("Not sent: " + r.error);
      }
      break;
    case Gateway::Request::Key: showNotice(ok ? "Sent " + sendLabel : "Not sent: " + r.error); break;
    case Gateway::Request::Prompt:
      if (ok) {
        showNotice("Prompt sent");
        draft = "";
        if (screen == Screen::Keyboard || screen == Screen::Prompts) screen = Screen::Detail;
      } else {
        showNotice("Not sent: " + r.error);  // a typed draft stays on the keyboard
      }
      break;
  }
}

void openKeyboard(const String& text) {
  draft = text.substring(0, settings::MAX_PROMPT_LEN);
  kbRow = 1;
  kbX = 0;
  kbShift = kbSym = backHoldFired = false;
  screen = Screen::Keyboard;
}

bool menuMove(Buttons& b, int count, int& sel = menuSel) {
  if (b.wasPressed(BTN_DOWN)) sel = min(sel + 1, count - 1);
  else if (b.wasPressed(BTN_UP)) sel = max(sel - 1, 0);
  else return false;
  return true;
}

bool keysInput(Buttons& b) {
  if (b.wasPressed(BTN_BACK)) {
    screen = Screen::Detail;
    return true;
  }
  if (menuMove(b, KEY_COUNT)) return true;
  if (!b.wasPressed(BTN_CONFIRM)) return false;
  sendKeyName = KEYS[menuSel].key;
  sendLabel = KEYS[menuSel].label;
  sendAfterDraw = Send::Key;
  showNotice("Sending " + sendLabel + "...");
  return true;
}

bool promptsInput(Buttons& b) {
  if (b.wasPressed(BTN_BACK)) {
    screen = Screen::Detail;
    return true;
  }
#if CAN_COMPOSE
  if (menuMove(b, canned.size() + 1)) return true;
  if (b.wasPressed(BTN_RIGHT) && menuSel > 0) {
    openKeyboard(canned[menuSel - 1]);
    return true;
  }
  if (!b.wasPressed(BTN_CONFIRM)) return false;
  if (menuSel == 0) {
    openKeyboard("");
    return true;
  }
  sendText = canned[menuSel - 1];
#else
  if (menuMove(b, canned.size())) return true;
  if (!b.wasPressed(BTN_CONFIRM) || canned.empty()) return false;
  sendText = canned[menuSel];
#endif
  sendAfterDraw = Send::Prompt;
  showNotice("Sending: " + sendText);
  return true;
}

// Drops the last character, whole (a canned prompt may hold UTF-8).
void deleteLast(String& s) {
  int n = s.length();
  while (n > 0 && ((uint8_t)s[n - 1] & 0xC0) == 0x80) n--;
  s.remove(max(0, n - 1));
}

// Queues the draft to send; false if there is nothing to.
void sendDraft() {
  String t = draft;
  t.trim();
  if (t.isEmpty()) {
    showNotice("Nothing to send");
    return;
  }
  sendText = t;
  sendAfterDraw = Send::Prompt;
  showNotice("Sending prompt...");
}

void typeChar(char c) {
  if (draft.length() >= settings::MAX_PROMPT_LEN) showNotice("Prompt is full");
  else draft += c;
}

bool keyboardInput(Buttons& b) {
  // Back acts on release: a tap deletes, a hold cancels.
  if (b.wasHeld(BTN_BACK) && !backHoldFired) {
    backHoldFired = true;
    draft = "";
    screen = Screen::Prompts;
    return true;
  }
  if (b.wasReleased(BTN_BACK)) {
    if (backHoldFired) {
      backHoldFired = false;
      return false;
    }
    // In case the hold event itself was dropped from a full queue.
    if (draft.isEmpty() || b.heldTime() >= HOLD_MS) {
      if (b.heldTime() >= HOLD_MS) draft = "";
      screen = Screen::Prompts;
    } else {
      deleteLast(draft);
    }
    return true;
  }
#if HAS_KEYBOARD
  if (char c = b.typed()) {
    typeChar(c);
    return true;
  }
  if (!b.wasPressed(BTN_CONFIRM)) return false;
  sendDraft();
  return true;
#endif
  if (b.wasPressed(BTN_UP)) kbRow = (kbRow + KB_ROWS - 1) % KB_ROWS;
  else if (b.wasPressed(BTN_DOWN)) kbRow = (kbRow + 1) % KB_ROWS;
  else if (b.wasPressed(BTN_LEFT) || b.wasPressed(BTN_RIGHT)) {
    int step = b.wasPressed(BTN_RIGHT) ? 1 : -1;
    if (kbRow < 4) kbX = (kbX + step + KB_UNITS) % KB_UNITS;
    else kbX = KB_BOTTOM[(kbBottomKey(kbX) + step + 4) % 4].from;
  } else if (b.wasPressed(BTN_CONFIRM)) {
    char c = 0;
    if (kbRow < 4) {
      c = kbChar(kbRow, kbX);
    } else {
      switch (kbBottomKey(kbX)) {
        case KB_SHIFT: kbShift = !kbShift; break;
        case KB_SYM: kbSym = !kbSym; break;
        case KB_SPACE: c = ' '; break;
        case KB_SEND: sendDraft(); break;
      }
    }
    if (c) {
      typeChar(c);
      if (c != ' ') kbShift = false;
    }
  } else {
    return false;
  }
  return true;
}

void openMenu() {
  hostList = settings::hosts();
  hostActive = settings::activeHost();
  screen = Screen::Menu;
}

bool menuInput(Buttons& b) {
  if (b.wasPressed(BTN_BACK)) {
    screen = HOME;
    return true;
  }
  if (menuMove(b, MENU_LEN, mainSel)) return true;
  if (!b.wasPressed(BTN_CONFIRM)) return false;
  switch (MENU_ORDER[mainSel]) {
    case M_USAGE:
      usageFirst = 0;
      screen = Screen::Usage;
      break;
    case M_HOSTS:
      hostSel = hostActive;
      screen = Screen::Hosts;
      break;
    case M_DEVICE: screen = Screen::Device; break;
    case M_FLIP:
#if !defined(PREVIEW_COMPACT)  // the preview is pinned landscape; don't save its flip for the X4 build
      rotation = rotation == 1 ? 3 : 1;
      settings::saveRotation(rotation);
#endif
      lastView = View::None;  // redraw all of it, the other way up
      break;
    case M_SCREEN: {
      const size_t n = sizeof(settings::SCREEN_TIMEOUTS) / sizeof(settings::SCREEN_TIMEOUTS[0]);
      size_t i = 0;
      while (i < n && settings::SCREEN_TIMEOUTS[i] != screenTimeoutS) i++;
      screenTimeoutS = settings::SCREEN_TIMEOUTS[(i + 1) % n];  // an unlisted value starts over
      settings::saveScreenTimeout(screenTimeoutS);
      lastKeyAt = millis();
      break;
    }
    case M_RESTART:
      restartAfterDraw = true;
      showNotice("Restarting...");
      break;
  }
  return true;
}

bool usageInput(Buttons& b) {
  if (b.wasPressed(BTN_BACK)) screen = Screen::Menu;
  else if (b.wasPressed(BTN_DOWN) && usageMore) usageFirst++;
  else if (b.wasPressed(BTN_UP) && usageFirst > 0) usageFirst--;
  else return false;
  return true;
}

bool hostsInput(Gateway& gw, Buttons& b) {
  if (b.wasPressed(BTN_BACK)) {
    screen = Screen::Menu;
    return true;
  }
  if (hostList.empty()) return false;
  if (menuMove(b, hostList.size(), hostSel)) return true;
  if (!b.wasPressed(BTN_CONFIRM)) return false;
  if (hostSel != hostActive) {
    gw.switchHost(hostSel);
    hostActive = hostSel;
    selectedId = "";
    cursor = 0;
  } else if (gw.state() != LinkState::Online) {
    gw.retryNow();
  } else {
    return false;
  }
  screen = HOME;
  return true;
}

// Returns true if the input changed what is on screen.
bool handleInput(View view, Gateway& gw, Agents& agents, Buttons& b) {
#if HAS_KEYBOARD
  if (b.wasPressed(BTN_HOME) && screen != HOME) {
    draft = "";
    screen = HOME;
    return true;
  }
  // The Messages key opens the selected session's conversation; from there,
  // or with none to open, the session list.
  if (b.wasPressed(BTN_MESSAGES)) {
    draft = "";
    if (screen != Screen::Detail && agents.size() && agents.indexOf(selectedId) >= 0) {
      screen = Screen::Detail;
      agents.markSeen(selectedId);
      agents.setDismissed(selectedId, false);
    } else if (screen != Screen::List) {
      screen = Screen::List;
    } else return false;
    return true;
  }
  // Dedicated buttons jump straight to an agent's Keys or Prompt screen.
  if ((b.wasPressed(BTN_PROMPT) || b.wasPressed(BTN_KEYS)) &&
      (view == View::Detail || view == View::Keys || view == View::Prompts)) {
    int i = agents.indexOf(selectedId);
    if (i < 0 || !hasSession(agents[i])) return false;
    menuSel = 0;
    if (b.wasPressed(BTN_PROMPT)) {
      canned = settings::prompts();
      screen = Screen::Prompts;
    } else {
      screen = Screen::Keys;
    }
    return true;
  }
  // Typing on an agent's screens starts a prompt to it.
  if (b.typed() && (view == View::Detail || view == View::Keys || view == View::Prompts)) {
    int i = agents.indexOf(selectedId);
    if (i < 0 || !hasSession(agents[i])) return false;
#if HAS_TRANSCRIPT
    // The conversation view composes inline, under the transcript.
    if (view == View::Detail) {
      typeChar(b.typed());
      return true;
    }
#endif
    openKeyboard(String(b.typed()));
    return true;
  }
#endif
  switch (view) {
    case View::Status:
      if (b.wasPressed(BTN_BACK)) {
        openMenu();
        return true;
      }
      if (b.wasPressed(BTN_CONFIRM)) {
        if (gw.state() == LinkState::HostKeyUnknown) gw.trustHostKey();
        else gw.retryNow();
        return true;
      }
      return false;
#if HAS_DASHBOARD
    case View::Home:
      if (b.wasPressed(BTN_BACK)) openMenu();
      else if (b.wasPressed(BTN_CONFIRM) || b.wasPressed(BTN_RIGHT)) screen = Screen::List;
      else return false;
      return true;
#endif
    case View::List: {
      int per = rowsPerPage();
      if (b.wasPressed(BTN_BACK)) openMenu();
      else if (b.wasPressed(BTN_DOWN)) moveCursor(agents, 1);
      else if (b.wasPressed(BTN_UP)) moveCursor(agents, -1);
      else if (b.wasPressed(BTN_RIGHT)) moveCursor(agents, per - cursor % per);
      else if (b.wasPressed(BTN_LEFT)) moveCursor(agents, -(cursor % per) - (cursor >= per ? per : 0));
      else if (b.wasPressed(BTN_CONFIRM) && agents.size()) {
        screen = Screen::Detail;
        agents.markSeen(selectedId);
        agents.setDismissed(selectedId, false);  // opening it asks for its card again
      } else return false;
      return true;
    }
    case View::Detail: {
#if HAS_TRANSCRIPT
      const int STEP = 2;
      if (b.wasPressed(BTN_BACK)) {
        if (draft.length()) deleteLast(draft);
        else screen = Screen::List;
      } else if (b.wasPressed(BTN_CONFIRM)) {
        if (draft.length()) sendDraft();
        else if (gw.cardPending() && gw.watchedId() == selectedId) {
          agents.setDismissed(selectedId, false);
          openCard(Screen::Detail);
        } else return false;
      } else if (b.wasPressed(BTN_UP) || b.wasPressed(BTN_DOWN)) {
        int dir = b.wasPressed(BTN_DOWN) ? 1 : -1;
        bool page = dir == trLastDir && b.eventAt() - trLastDirAt < DOUBLE_TAP_MS;
        trLastDir = dir;
        trLastDirAt = b.eventAt();
        int step = page ? max(1, trRows - 1) : STEP;
        if (dir < 0) {
          if (trScroll <= 0) {
            if (trHasMore) gw.requestOlderTranscript();
            else return false;
          } else {
            trScroll = max(0, trScroll - step);
            trFollow = false;
          }
        } else {
          if (trScroll >= trMaxScroll) return false;
          trScroll = min(trMaxScroll, trScroll + step);
          trFollow = trScroll >= trMaxScroll;
        }
      } else return false;
      agents.markSeen(selectedId);
      return true;
#else
      int i = agents.indexOf(selectedId);
      bool session = i >= 0 && hasSession(agents[i]);
      if (b.wasPressed(BTN_BACK)) screen = Screen::List;
      else if (b.wasPressed(BTN_DOWN)) moveCursor(agents, 1);
      else if (b.wasPressed(BTN_UP)) moveCursor(agents, -1);
      else if (b.wasPressed(BTN_CONFIRM) && gw.cardPending() && gw.watchedId() == selectedId) {
        agents.setDismissed(selectedId, false);
        openCard(Screen::Detail);
      } else if (b.wasPressed(BTN_RIGHT) && session) {
        menuSel = 0;
        canned = settings::prompts();
        screen = Screen::Prompts;
      } else return false;
      agents.markSeen(selectedId);
      return true;
#endif
    }
    case View::Card: return cardInput(gw, agents, b);
    case View::Keys: return keysInput(b);
    case View::Prompts: return promptsInput(b);
    case View::Keyboard: return keyboardInput(b);
    case View::Menu: return menuInput(b);
    case View::Usage: return usageInput(b);
    case View::Hosts: return hostsInput(gw, b);
    case View::Device:
      if (!b.wasPressed(BTN_BACK)) return false;
      screen = Screen::Menu;
      return true;
    default: return false;
  }
}

}  // namespace

void begin() {
#ifdef PREVIEW_COMPACT
  rotation = 0;  // landscape, so the M9's layout sits the right way round
#else
  rotation = settings::rotation();
#endif
  panel::begin(rotation);
  screenTimeoutS = settings::screenTimeout();
  lastKeyAt = millis();
}

void dumpScreen() { panel::dump(rotation); }

void message(const String& title, const String& body) {
  String key = title + "\n" + body;
  if (lastView == View::Message && key == lastMessage) return;
  lastMessage = key;
  lastView = View::Message;
  paint(true, [&] {
    int y = S(120) - (LAYOUT_COMPACT ? 40 : 0);
    display.setFont(F_TITLE);
    for (const String& line : wrap(ascii(title), W() - 2 * MARGIN, 3)) {
      text(MARGIN, y, line, F_TITLE);
      y += S(42);
    }
    y += S(20);
    int start = 0;
    String b = body + "\n";
    while (start < (int)b.length()) {
      int nl = b.indexOf('\n', start);
      String para = b.substring(start, nl);
      if (para.length()) y = paragraph(y, para, F_TEXT, 8, S(32));
      else y += S(16);
      start = nl + 1;
    }
  });
}

View viewFor(LinkState st, Agents& agents) {
  switch (screen) {
    case Screen::Menu: return View::Menu;
    case Screen::Usage: return View::Usage;
    case Screen::Hosts: return View::Hosts;
    case Screen::Device: return View::Device;
    default: break;
  }
  bool reconnecting = (st == LinkState::Waiting || st == LinkState::Connecting) && agents.loaded();
  if (st != LinkState::Online && !reconnecting) return View::Status;
  switch (screen) {
    case Screen::Detail: return View::Detail;
    case Screen::Card: return View::Card;
    case Screen::Keys: return View::Keys;
    case Screen::Prompts: return View::Prompts;
    case Screen::Keyboard: return View::Keyboard;
    case Screen::Home: return View::Home;
    default: return View::List;
  }
}

static void updateLocked(Gateway& gw, Agents& agents, Buttons& buttons, bool changed) {
  buttons.setSwapUpDown(rotation == 1);
  Gateway::Result result;
  if (gw.takeResult(result)) {
    finishSend(result);
    redrawNow = true;
  }
  clampCursor(agents);
  if (agentScreen(screen) && agents.indexOf(selectedId) < 0) screen = Screen::List;  // the tab closed
  steerWatch(gw, agents);

  if (screen == Screen::Detail && gw.cardPending() && gw.watchedId() == selectedId &&
      !agents[agents.indexOf(selectedId)].dismissed)
    openCard(Screen::Detail);
  if (screen == Screen::Card && gw.watchedId() == selectedId) {
    if (gw.cardPending() && gw.card().key() != cardKey) {  // a new request: start over
      cardKey = gw.card().key();
      qIndex = 0;
      scroll = 0;
      for (int& c : choices) c = 0;
    } else if (!gw.cardPending() && gw.card().received() && cardKey.length()) {
      showNotice("Answered on the host");  // answered elsewhere wins
      closeCard();
    }
  }

  View view = viewFor(gw.state(), agents);
  if (view == View::Detail) changed |= agents.markSeen(selectedId);

  // Every press is applied, in order, before drawing, so a burst of presses
  // costs one refresh and none is lost. On a decision card only presses made
  // after it was on screen count; the rest were aimed at whatever was there
  // before. Sends start at once and report back through finishSend().
  bool dirty = false;
#if HAS_BACKLIGHT
  bool waking = false;
  if (buttons.hasEvent()) {
    lastKeyAt = millis();
    waking = screenOff;
    setScreenOn(true);
  }
  size_t blocked = agents.blockedCount();
  if (blocked > lastBlocked) {
    lastKeyAt = millis();
    setScreenOn(true);
  }
  lastBlocked = blocked;
  if (!screenOff && screenTimeoutS && millis() - lastKeyAt >= screenTimeoutS * 1000UL) setScreenOn(false);
#endif
  do {
#if HAS_BACKLIGHT
    if (waking) continue;  // the press that woke the screen does nothing else
#endif
    if (buttons.hasEvent() && view == View::Card &&
        (lastView != View::Card || shownCardKey != cardKey || buttons.eventAt() < cardShownAt))
      continue;
    if (handleInput(view, gw, agents, buttons)) {
      dirty = true;
      if (!inputAt) inputAt = buttons.eventAt();
      if (sendAfterDraw != Send::None) {
        startSend(gw, agents, sendAfterDraw);
        sendAfterDraw = Send::None;
      }
      view = viewFor(gw.state(), agents);
    }
  } while (buttons.next());
  pending |= changed;
  unsigned long now = millis();
  // Don't slow typing down with redraws for the gateway.
  bool typing = view == View::Keyboard;
#if HAS_DASHBOARD
  gw.wantHostStats(view == View::Home);
#endif
  if (pending && !typing && now - lastDrawAt >= MIN_REDRAW_MS) dirty = true;
  if (redrawNow) dirty = true;
  if (notice.length() && !noticeShown()) {
    notice = "";
    dirty = true;
  }

  statusDirty |= gw.takeStatusUpdate();
#if HAS_TRANSCRIPT
  if (gw.takeTranscriptUpdate() && view == View::Detail) dirty = true;
#endif
  if (statusDirty && view == View::Detail) {
    int i = agents.indexOf(selectedId);
    String info = i >= 0 ? infoText(gw, agents[i]) : String();
    String act = i >= 0 ? activityText(gw, agents[i]) : String();
    if (info != drawnInfo || act.isEmpty() != drawnActivity.isEmpty()) dirty = true;
    else if (act == drawnActivity) statusDirty = false;
    else if (now - lastDrawAt >= ACTIVITY_MS) dirty = true;  // only the ticking status line changed
  }

  time_t t = time(nullptr);
  long minute = t > 1700000000 ? t / 60 : -1;
  if (minute != lastMinute && view != View::Status && !typing) dirty = true;

  bool changedView = view != lastView;
  if (!dirty && !changedView) return;

  // Moving between screens is a partial refresh too; a full one (2.4 s and a
  // flash) is for leaving a message screen and for clearing the ghosting
  // partials leave -- done on a screen change where possible, never mid-typing.
  bool full = lastView == View::None || lastView == View::Message || lastView == View::Status ||
              view == View::Status;
  if (partials >= FULL_EVERY && changedView) full = true;
  if (partials >= 2 * FULL_EVERY && !typing) full = true;

  auto draw = [&](void (*body)(Gateway&, Agents&)) {
    paint(full, [&] {
      body(gw, agents);
      drawNotice();
    });
  };
  switch (view) {
    case View::List: draw(drawList); break;
    case View::Detail: draw(drawDetail); break;
    case View::Card: draw(drawCard); break;
    case View::Keys: draw(drawKeys); break;
    case View::Prompts: draw(drawPrompts); break;
    case View::Keyboard: draw(drawKeyboard); break;
    case View::Menu: draw(drawMenu); break;
    case View::Usage: draw(drawUsage); break;
    case View::Hosts: draw(drawHosts); break;
    case View::Device: draw(drawDevice); break;
#if HAS_DASHBOARD
    case View::Home: draw(drawHome); break;
#endif
    default: draw(drawStatus); break;
  }
  if (inputAt) Serial.printf("[ui] press to screen %lu ms\n", millis() - inputAt);
  inputAt = 0;
  if (view == View::Card && (lastView != View::Card || shownCardKey != cardKey)) {
    cardShownAt = millis();
    shownCardKey = cardKey;
  }
  lastView = view;
  lastMinute = minute;
  lastDrawAt = millis();
  pending = false;
  if (view == View::Detail) statusDirty = false;


  if (restartAfterDraw) {
    Serial.println("[ui] restart from the menu");
    delay(200);
    ESP.restart();
  }
  redrawNow = false;
}

void update(Gateway& gw, Agents& agents, Buttons& buttons, bool changed) {
  locked = &gw;
  updateLocked(gw, agents, buttons, changed);
  locked = nullptr;
}

}  // namespace ui
