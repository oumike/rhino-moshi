#include "ui.h"

#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <GxEPD2_BW.h>
#include <WiFi.h>
#include <time.h>

#include <vector>

#include "board.h"
#include "settings.h"

namespace ui {
namespace {

using EpdDisplay = GxEPD2_BW<GxEPD2_426_GDEQ0426T82, GxEPD2_426_GDEQ0426T82::HEIGHT>;
EpdDisplay display(GxEPD2_426_GDEQ0426T82(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

const GFXfont* F_TITLE = &FreeSansBold18pt7b;
const GFXfont* F_BOLD = &FreeSansBold12pt7b;
const GFXfont* F_TEXT = &FreeSans12pt7b;
const GFXfont* F_SMALL = &FreeSans9pt7b;

const int MARGIN = 14;
const int HEADER_H = 54;
const int FOOTER_H = 58;
const int ROW_H = 76;
const int ICON_X = 30;  // icon centre
const int TEXT_X = 54;  // row text start

// Partial refreshes leave ghosting; clean it up with a full one now and then.
const int FULL_EVERY = 15;
// Gateway pushes can arrive several times a second; batch them.
const unsigned long MIN_REDRAW_MS = 1000;

enum class View { None, Message, Status, List, Detail };
enum class Screen { List, Detail };

uint8_t rotation = DEFAULT_ROTATION;
int partials = 0;
View lastView = View::None;
String lastMessage;

Screen screen = Screen::List;
String selectedId;
int cursor = 0;
bool pending = false;  // gateway changes not drawn yet
unsigned long lastDrawAt = 0;
long lastMinute = -1;

int W() { return display.width(); }
int H() { return display.height(); }

// The built-in fonts are 7-bit ASCII; map the punctuation agents like to use
// and drop the rest (emoji, CJK) rather than print garbage.
String ascii(const String& s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length();) {
    uint8_t c = s[i];
    if (c < 0x80) {
      out += c < 0x20 ? ' ' : (char)c;
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

void text(int x, int baseline, const String& s, const GFXfont* font, uint16_t color = GxEPD_BLACK) {
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

template <typename F>
void paint(bool full, F body) {
  display.setRotation(rotation);
  if (full || partials >= FULL_EVERY) {
    display.setFullWindow();
    partials = 0;
  } else {
    display.setPartialWindow(0, 0, W(), H());
    partials++;
  }
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    body();
  } while (display.nextPage());
}

String age(double since, bool approx) {
  time_t now = time(nullptr);
  if (now < 1700000000 || since <= 0) return "";
  long d = max(0L, (long)(now - (long)since));
  String s = d < 60 ? String("<1m") : d < 3600 ? String(d / 60) + "m" : d < 86400 ? String(d / 3600) + "h"
                                                                                    : String(d / 86400) + "d";
  return approx && d >= 60 ? ">" + s : s;
}

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

void header(Gateway& gw, Agents& agents) {
  String right;
  switch (gw.state()) {
    case LinkState::Online:
      if (!agents.loaded()) right = "loading";
      else if (agents.blockedCount()) right = String(agents.blockedCount()) + " waiting";
      else right = "online";
      break;
    case LinkState::Connecting: right = "connecting"; break;
    default: right = "offline"; break;
  }
  display.setFont(F_TEXT);
  int rw = textWidth(right);
  text(W() - MARGIN - rw, 36, right, F_TEXT);
  display.setFont(F_BOLD);
  text(MARGIN, 36, fit(ascii(gw.host().name), W() - 3 * MARGIN - rw), F_BOLD);
  display.fillRect(0, HEADER_H - 3, W(), 3, GxEPD_BLACK);
}

// Labels for the row of keys under the screen, each drawn above its key.
void footer(const char* back, const char* confirm, const char* left, const char* right) {
  const char* labels[BTN_COUNT] = {};
  labels[BTN_BACK] = back;
  labels[BTN_CONFIRM] = confirm;
  labels[BTN_LEFT] = left;
  labels[BTN_RIGHT] = right;
  int top = H() - FOOTER_H;
  int slot = W() / 4;
  display.fillRect(0, top, W(), 2, GxEPD_BLACK);
  display.setFont(F_BOLD);
  for (int i = 0; i < 4; i++) {
    const char* label = labels[BOTTOM_ROW_ORDER[i]];
    if (i) display.drawFastVLine(i * slot, top + 10, FOOTER_H - 20, GxEPD_BLACK);
    if (!label || !*label) continue;
    String s = fit(label, slot - 8);
    text(i * slot + (slot - textWidth(s)) / 2, top + 37, s, F_BOLD);
  }
}

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
    uint16_t fg = sel ? GxEPD_WHITE : GxEPD_BLACK, bg = sel ? GxEPD_BLACK : GxEPD_WHITE;
    if (sel) display.fillRect(0, y, W(), ROW_H, GxEPD_BLACK);
    icon(a, ICON_X, y + 26, fg, bg);

    String right = ascii(a.kind);
    String ago = age(a.changedAt, a.approx);
    if (ago.length()) right += "  " + ago;
    display.setFont(F_SMALL);
    int rw = textWidth(right);
    text(W() - MARGIN - rw, y + 31, right, F_SMALL, fg);

    String name = ascii(a.workspace);
    if (a.tab.length() && a.tab != "1") name += " / " + ascii(a.tab);
    display.setFont(F_BOLD);
    text(TEXT_X, y + 32, fit(name, W() - TEXT_X - MARGIN - rw - 10), F_BOLD, fg);

    String sub = a.title.length() ? ascii(a.title) : String(stateName(a.state));
    display.setFont(F_TEXT);
    text(TEXT_X, y + 62, fit(sub, W() - TEXT_X - MARGIN), F_TEXT, fg);
    if (!sel) display.drawFastHLine(TEXT_X, y + ROW_H - 1, W() - TEXT_X - MARGIN, GxEPD_BLACK);
  }

  String pageInfo = pages > 1 ? String(page + 1) + "/" + String(pages) : String();
  footer("", agents.size() ? "Open" : "", pages > 1 ? "Prev" : "", pages > 1 ? "Next" : "");
  if (pageInfo.length()) {
    display.setFont(F_SMALL);
    text(W() - MARGIN - textWidth(pageInfo), H() - FOOTER_H - 8, pageInfo, F_SMALL);
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

void drawDetail(Gateway& gw, Agents& agents) {
  header(gw, agents);
  int i = agents.indexOf(selectedId);
  if (i < 0) return;
  const Agent& a = agents[i];

  int y = HEADER_H + 46;
  display.setFont(F_TITLE);
  text(MARGIN, y, fit(ascii(a.workspace), W() - 2 * MARGIN), F_TITLE);
  y += 36;
  text(MARGIN, y, "Tab " + ascii(a.tab) + " - " + ascii(a.kind), F_TEXT);
  y += 48;
  icon(a, MARGIN + 12, y - 8, GxEPD_BLACK, GxEPD_WHITE);
  text(MARGIN + 36, y, stateLine(a), F_BOLD);
  y += 50;
  if (a.title.length()) y = paragraph(y, a.title, F_TEXT, 6, 30) + 20;
  if (a.cwd.length()) {
    text(MARGIN, y, "Folder", F_SMALL);
    paragraph(y + 24, a.cwd, F_SMALL, 3, 22);
  }
  footer("Back", "", "", "");
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
      s.body = host + " now presents\n" + gw.fingerprint() + "\n\nbut the X4 trusted\n" +
               settings::pinnedHostKey() +
               "\n\nRefusing to connect. If the host was reinstalled, forget the pinned key at " + page + ".";
      break;
    case LinkState::AuthRefused:
      s.title = "Key not authorized";
      s.body = host + " refused the X4's key. Copy the authorized_keys line from " + page +
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
  int y = HEADER_H + 54;
  display.setFont(F_TITLE);
  for (const String& line : wrap(ascii(s.title), W() - 2 * MARGIN, 2)) {
    text(MARGIN, y, line, F_TITLE);
    y += 40;
  }
  y += 14;
  int start = 0;
  String body = s.body + "\n";
  while (start < (int)body.length() && y < H() - FOOTER_H - 20) {
    int nl = body.indexOf('\n', start);
    String para = body.substring(start, nl);
    if (para.length()) y = paragraph(y, para, F_TEXT, 8, 30);
    else y += 14;
    start = nl + 1;
  }
  footer("", s.confirm, "", "");
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

// Returns true if the input changed what is on screen.
bool handleInput(View view, Gateway& gw, Agents& agents, Buttons& b) {
  switch (view) {
    case View::Status:
      if (b.wasPressed(BTN_CONFIRM)) {
        if (gw.state() == LinkState::HostKeyUnknown) gw.trustHostKey();
        else gw.retryNow();
        return true;
      }
      return false;
    case View::List: {
      int per = rowsPerPage();
      if (b.wasPressed(BTN_DOWN)) moveCursor(agents, 1);
      else if (b.wasPressed(BTN_UP)) moveCursor(agents, -1);
      else if (b.wasPressed(BTN_RIGHT)) moveCursor(agents, per - cursor % per);
      else if (b.wasPressed(BTN_LEFT)) moveCursor(agents, -(cursor % per) - (cursor >= per ? per : 0));
      else if (b.wasPressed(BTN_CONFIRM) && agents.size()) {
        screen = Screen::Detail;
        agents.markSeen(selectedId);
      } else return false;
      return true;
    }
    case View::Detail:
      if (b.wasPressed(BTN_BACK)) screen = Screen::List;
      else if (b.wasPressed(BTN_DOWN)) moveCursor(agents, 1);
      else if (b.wasPressed(BTN_UP)) moveCursor(agents, -1);
      else return false;
      agents.markSeen(selectedId);
      return true;
    default: return false;
  }
}

}  // namespace

void begin() {
  rotation = settings::rotation();
  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS);
  display.init(0);
}

void message(const String& title, const String& body) {
  String key = title + "\n" + body;
  if (lastView == View::Message && key == lastMessage) return;
  lastMessage = key;
  lastView = View::Message;
  paint(true, [&] {
    int y = 120;
    display.setFont(F_TITLE);
    for (const String& line : wrap(ascii(title), W() - 2 * MARGIN, 3)) {
      text(MARGIN, y, line, F_TITLE);
      y += 42;
    }
    y += 20;
    int start = 0;
    String b = body + "\n";
    while (start < (int)b.length()) {
      int nl = b.indexOf('\n', start);
      String para = b.substring(start, nl);
      if (para.length()) y = paragraph(y, para, F_TEXT, 8, 32);
      else y += 16;
      start = nl + 1;
    }
  });
}

void update(Gateway& gw, Agents& agents, Buttons& buttons, bool changed) {
  LinkState st = gw.state();
  bool reconnecting = (st == LinkState::Waiting || st == LinkState::Connecting) && agents.loaded();
  View view = (st == LinkState::Online || reconnecting) ? (screen == Screen::Detail ? View::Detail : View::List)
                                                        : View::Status;

  clampCursor(agents);
  if (view == View::Detail && agents.indexOf(selectedId) < 0) {
    screen = Screen::List;
    view = View::List;
  }
  if (view == View::Detail) changed |= agents.markSeen(selectedId);

  bool dirty = handleInput(view, gw, agents, buttons);
  if (dirty) {  // input may have switched screens
    view = (view == View::Status) ? View::Status : (screen == Screen::Detail ? View::Detail : View::List);
  }
  pending |= changed;
  unsigned long now = millis();
  if (pending && now - lastDrawAt >= MIN_REDRAW_MS) dirty = true;

  time_t t = time(nullptr);
  long minute = t > 1700000000 ? t / 60 : -1;
  if (minute != lastMinute && (view == View::List || view == View::Detail)) dirty = true;

  bool full = view != lastView;
  if (!dirty && !full) return;

  switch (view) {
    case View::List: paint(full, [&] { drawList(gw, agents); }); break;
    case View::Detail: paint(full, [&] { drawDetail(gw, agents); }); break;
    default: paint(full, [&] { drawStatus(gw, agents); }); break;
  }
  lastView = view;
  lastMinute = minute;
  lastDrawAt = millis();
  pending = false;
  buttons.resync();
}

}  // namespace ui
