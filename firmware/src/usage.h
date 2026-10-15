#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <time.h>

#include <vector>

// One rate-limit window of an account ("5h", "7d").
struct UsageWindow {
  String label;
  int used = 0;  // percent
  time_t resetsAt = 0;

  bool operator==(const UsageWindow& o) const {
    return label == o.label && used == o.used && resetsAt == o.resetsAt;
  }
};

// One signed-in agent account on the host, as moshi-hook reports it.
struct UsageAccount {
  String agent;  // "claude-code", "codex"
  String label;  // "Pro (m•••@g•••.com)"
  time_t capturedAt = 0;
  std::vector<UsageWindow> windows;

  bool operator==(const UsageAccount& o) const {
    return agent == o.agent && label == o.label && capturedAt == o.capturedAt && windows == o.windows;
  }
};

/** The host's subscription usage, from the gateway's `{"usage":[...]}` frames. */
class Usage {
 public:
  static void buildFilter(JsonDocument& filter);
  // Replace with a frame's accounts. Returns true if anything changed.
  bool update(JsonArrayConst accounts);
  void clear();

  bool received() const { return received_; }
  const std::vector<UsageAccount>& accounts() const { return accounts_; }
  // The most-used window across accounts, or nullptr.
  const UsageWindow* highest() const;

 private:
  std::vector<UsageAccount> accounts_;
  bool received_ = false;
};

// "2026-10-15T05:20:00Z" (fractions and offsets allowed) to UTC epoch; 0 if unreadable.
time_t parseIsoTime(const char* s);
