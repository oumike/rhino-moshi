#include "agents.h"

#include <algorithm>

AgentState parseState(const char* s) {
  if (!s) return AgentState::Unknown;
  if (!strcmp(s, "blocked")) return AgentState::Blocked;
  if (!strcmp(s, "done")) return AgentState::Done;
  if (!strcmp(s, "working")) return AgentState::Working;
  if (!strcmp(s, "idle")) return AgentState::Idle;
  return AgentState::Unknown;
}

const char* stateName(AgentState s) {
  switch (s) {
    case AgentState::Blocked: return "blocked";
    case AgentState::Done: return "done";
    case AgentState::Working: return "working";
    case AgentState::Idle: return "idle";
    default: return "unknown";
  }
}

void Agents::buildFilter(JsonDocument& filter) {
  JsonObject g = filter["workspaces"]["groups"][0].to<JsonObject>();
  g["label"] = true;
  JsonObject t = g["children"][0].to<JsonObject>();
  for (const char* k : {"id", "label", "agent", "agentStatus", "sessionId", "title", "cwd", "statusChangedAt",
                        "statusChangedAtApprox"})
    t[k] = true;
}

namespace {

int rank(const Agent& a) {
  if (a.state == AgentState::Blocked) return 0;
  if (a.unseen || a.state == AgentState::Done) return 1;
  return 2;
}

bool sameVisible(const Agent& a, const Agent& b) {
  return a.id == b.id && a.state == b.state && a.title == b.title && a.workspace == b.workspace &&
         a.kind == b.kind && a.changedAt == b.changedAt && a.unseen == b.unseen && a.tab == b.tab;
}

}  // namespace

bool Agents::update(JsonObjectConst workspaces) {
  std::vector<Agent> next;
  for (JsonObjectConst g : workspaces["groups"].as<JsonArrayConst>()) {
    for (JsonObjectConst t : g["children"].as<JsonArrayConst>()) {
      if (!t["agent"].is<const char*>()) continue;  // shell tab
      Agent a;
      a.id = t["id"] | "";
      a.workspace = g["label"] | "";
      a.tab = t["label"] | "";
      a.kind = t["agent"] | "";
      a.session = t["sessionId"] | "";
      a.title = t["title"] | "";
      a.cwd = t["cwd"] | "";
      a.state = parseState(t["agentStatus"]);
      a.changedAt = t["statusChangedAt"] | 0.0;
      a.approx = t["statusChangedAtApprox"] | false;

      int old = indexOf(a.id);
      if (old >= 0) {
        const Agent& prev = list_[old];
        bool wasBusy = prev.state == AgentState::Working || prev.state == AgentState::Blocked;
        bool nowFinished = a.state == AgentState::Done || a.state == AgentState::Idle;
        a.unseen = (prev.unseen || (wasBusy && nowFinished)) && a.state != AgentState::Working;
      }
      next.push_back(a);
    }
  }
  std::stable_sort(next.begin(), next.end(), [](const Agent& a, const Agent& b) {
    int ra = rank(a), rb = rank(b);
    if (ra != rb) return ra < rb;
    return a.changedAt > b.changedAt;
  });

  bool changed = !loaded_ || next.size() != list_.size();
  for (size_t i = 0; !changed && i < next.size(); i++) changed = !sameVisible(next[i], list_[i]);
  list_ = std::move(next);
  loaded_ = true;
  return changed;
}

void Agents::clear() {
  list_.clear();
  loaded_ = false;
}

int Agents::indexOf(const String& id) const {
  for (size_t i = 0; i < list_.size(); i++)
    if (list_[i].id == id) return i;
  return -1;
}

size_t Agents::blockedCount() const {
  size_t n = 0;
  for (const Agent& a : list_) n += a.state == AgentState::Blocked;
  return n;
}

bool Agents::markSeen(const String& id) {
  int i = indexOf(id);
  if (i < 0 || !list_[i].unseen) return false;
  list_[i].unseen = false;
  return true;
}
