#include "usage.h"

namespace {

const size_t MAX_ACCOUNTS = 6;
const size_t MAX_WINDOWS = 4;

// Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's algorithm).
long daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468;
}

}  // namespace

time_t parseIsoTime(const char* s) {
  if (!s) return 0;
  int y, mo, d, h, mi, sec;
  int n = 0;
  if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &sec, &n) != 6) return 0;
  if (mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
  const char* p = s + n;
  if (*p == '.')
    while (isdigit((unsigned char)*++p)) {
    }
  long offset = 0;
  if (*p == '+' || *p == '-') {
    int oh = 0, om = 0;
    if (sscanf(p + 1, "%2d:%2d", &oh, &om) >= 1) offset = (oh * 3600L + om * 60L) * (*p == '-' ? -1 : 1);
  }
  return (time_t)(daysFromCivil(y, mo, d) * 86400L + h * 3600L + mi * 60L + sec - offset);
}

void Usage::buildFilter(JsonDocument& filter) {
  JsonObject a = filter["usage"][0].to<JsonObject>();
  a["agent"] = true;
  a["accountLabel"] = true;
  a["capturedAt"] = true;
  JsonObject w = a["windows"][0].to<JsonObject>();
  w["label"] = true;
  w["usedPercentage"] = true;
  w["resetsAt"] = true;
}

bool Usage::update(JsonArrayConst accounts) {
  std::vector<UsageAccount> next;
  for (JsonObjectConst a : accounts) {
    if (next.size() >= MAX_ACCOUNTS) break;
    UsageAccount acc;
    acc.agent = a["agent"] | "";
    acc.label = a["accountLabel"] | "";
    acc.capturedAt = parseIsoTime(a["capturedAt"] | "");
    for (JsonObjectConst w : a["windows"].as<JsonArrayConst>()) {
      if (acc.windows.size() >= MAX_WINDOWS) break;
      UsageWindow win;
      win.label = w["label"] | "";
      win.used = constrain((int)lround(w["usedPercentage"] | 0.0), 0, 999);
      win.resetsAt = parseIsoTime(w["resetsAt"] | "");
      acc.windows.push_back(win);
    }
    next.push_back(acc);
  }
  bool changed = !received_ || next != accounts_;
  accounts_ = std::move(next);
  received_ = true;
  return changed;
}

void Usage::clear() {
  accounts_.clear();
  received_ = false;
}

const UsageWindow* Usage::highest() const {
  const UsageWindow* best = nullptr;
  for (const UsageAccount& a : accounts_)
    for (const UsageWindow& w : a.windows)
      if (!best || w.used > best->used) best = &w;
  return best;
}
