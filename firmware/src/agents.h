#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include <vector>

enum class AgentState : uint8_t { Blocked, Done, Working, Idle, Unknown };

/** One agent tab from the workspace tree. */
struct Agent {
  String id;         // tab id, e.g. "w2T:t1"
  String workspace;  // group label
  String tab;        // tab label
  String kind;       // "claude", "codex", "copilot", ...
  String session;    // agent session id, for the agent watch and answers
  String title;
  String cwd;
  AgentState state = AgentState::Unknown;
  double changedAt = 0;  // Unix seconds; 0 if unknown
  bool approx = false;   // changedAt is "at or before"
  bool unseen = false;   // finished since it was last opened on the X4
  bool dismissed = false;  // its card was closed with Back; cleared when it blocks anew
};

/**
 * The agents list, rebuilt from each `workspaces` frame. Sorted attention
 * first: blocked, then finished-and-unseen or done, then everything else, each
 * group newest first.
 */
class Agents {
 public:
  // Filter for deserializeJson that keeps only what update() reads.
  static void buildFilter(JsonDocument& filter);

  // Replace the list from `workspaces` (the frame's object). Returns true if
  // anything visible changed.
  bool update(JsonObjectConst workspaces);
  void clear();

  bool loaded() const { return loaded_; }
  size_t size() const { return list_.size(); }
  const Agent& operator[](size_t i) const { return list_[i]; }
  int indexOf(const String& id) const;
  size_t blockedCount() const;

  // Opening an agent on the X4 clears its unseen mark.
  bool markSeen(const String& id);

  void setDismissed(const String& id, bool dismissed);
  // Blocked agents whose cards weren't dismissed, waiting longest first.
  std::vector<int> waitingOrder() const;
  // 1-based position of `id` among the blocked agents in list order, or 0.
  int blockedPosition(const String& id) const;
  // The blocked agent after `id` in list order, wrapping around; -1 if none.
  int nextBlocked(const String& id) const;

 private:
  std::vector<Agent> list_;
  bool loaded_ = false;
};

AgentState parseState(const char* s);
const char* stateName(AgentState s);
