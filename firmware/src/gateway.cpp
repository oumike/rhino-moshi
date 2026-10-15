#include "gateway.h"

#include "board.h"

#include <WiFi.h>

namespace {

const unsigned long FIRST_BACKOFF_MS = 2000;
const unsigned long MAX_BACKOFF_MS = 60000;
const unsigned long AUTH_RETRY_MS = 30000;          // the key may be added any moment
const unsigned long INCOMPATIBLE_RETRY_MS = 300000;  // an upgrade takes a while
// The gateway pushes on change only, so a quiet socket is normal: ping it,
// and give up if not even a pong comes back.
const unsigned long PING_AFTER_MS = 30000;
const unsigned long DEAD_AFTER_MS = 75000;
const int MAX_MESSAGES_PER_POLL = 4;

const char* REQUIRED_CAPS[] = {"events.watch.workspaces", "events.watch.agent-status", "approvals.answer"};

}  // namespace

void Gateway::begin(ssh_key key) {
  mutex_ = xSemaphoreCreateRecursiveMutex();
  key_ = key;
  deviceFp_ = keyFingerprint(key);
  host_ = settings::loadHost();
  state_ = host_.isComplete() ? LinkState::Connecting : LinkState::NoHost;
}

bool Gateway::takeChanged() {
  bool c = changed_;
  changed_ = false;
  return c;
}

void Gateway::switchHost(int index) {
  settings::saveActiveHost(index);
  host_ = settings::loadHost();
  agents_.clear();
  usage_.clear();
  card_.clear();
  watchId_ = watchSource_ = watchSession_ = answeredKey_ = "";
  detail_ = fingerprint_ = "";
  backoffMs_ = 0;
  nextAttemptAt_ = 0;
  state_ = host_.isComplete() ? LinkState::Connecting : LinkState::NoHost;
  gen_++;  // the network task drops the old connection
  if (reqPending_) {
    reqPending_ = false;
    result_ = {reqKind_, -1, "Switched hosts"};
    resultReady_ = true;
  }
  changed_ = true;
  Serial.printf("[gw] switched to host %d: %s\n", index, host_.name.c_str());
}

unsigned long Gateway::retryIn() const {
  long left = (long)(nextAttemptAt_ - millis());
  return nextAttemptAt_ && left > 0 ? left : 0;
}

// Network task, unlocked: closing can block on a dead link.
void Gateway::disconnect() {
  ws_.reset();
  link_.close();
}

// Network task, unlocked.
void Gateway::fail(LinkState state, const String& detail, bool retry) {
  disconnect();
  Guard g(*this);
  if (linkGen_ != gen_) return;  // a host switch overtook this attempt
  card_.clear();
  state_ = state;
  detail_ = detail;
  changed_ = true;
  nextAttemptAt_ = 0;
  if (reqPending_) {
    reqPending_ = false;
    result_ = {reqKind_, -1, "Connection lost"};
    resultReady_ = true;
  }
  if (!retry) return;
  unsigned long wait;
  if (state == LinkState::AuthRefused) {
    wait = AUTH_RETRY_MS;
  } else if (state == LinkState::Incompatible) {
    wait = INCOMPATIBLE_RETRY_MS;
  } else {
    backoffMs_ = backoffMs_ ? min(backoffMs_ * 2, MAX_BACKOFF_MS) : FIRST_BACKOFF_MS;
    wait = backoffMs_;
  }
  nextAttemptAt_ = millis() + wait;
  if (!nextAttemptAt_) nextAttemptAt_ = 1;
  Serial.printf("[gw] %s; retry in %lus\n", detail.c_str(), wait / 1000);
}

void Gateway::trustHostKey() {
  if (state_ != LinkState::HostKeyUnknown || fingerprint_.isEmpty()) return;
  settings::pinHostKey(fingerprint_);
  host_.hostKey = fingerprint_;
  Serial.printf("[gw] trusted host key %s\n", fingerprint_.c_str());
  retryNow();
}

void Gateway::retryNow() {
  if (state_ == LinkState::NoHost || state_ == LinkState::Online) return;
  backoffMs_ = 0;
  nextAttemptAt_ = 0;
  state_ = LinkState::Connecting;
  changed_ = true;
}

// Network task, unlocked.
void Gateway::attempt() {
  HostConfig host;
  {
    Guard g(*this);
    linkGen_ = gen_;
    host = host_;
  }
  disconnect();
  if (WiFi.status() != WL_CONNECTED) {
    fail(LinkState::Waiting, "Wi-Fi is down", true);
    return;
  }
  Serial.printf("[gw] connecting to %s@%s:%u\n", host.user.c_str(), host.address.c_str(), host.port);
  unsigned long t0 = millis();
  SshResult r = link_.connect(host, key_);
  {
    Guard g(*this);
    if (linkGen_ == gen_) fingerprint_ = link_.fingerprint();
  }
  switch (r) {
    case SshResult::Ok: break;
    case SshResult::Unreachable: fail(LinkState::Waiting, link_.error(), true); return;
    case SshResult::HostKeyUnknown: fail(LinkState::HostKeyUnknown, "", false); return;
    case SshResult::HostKeyChanged: fail(LinkState::HostKeyChanged, "pinned " + host.hostKey, false); return;
    case SshResult::AuthRefused: fail(LinkState::AuthRefused, link_.error(), true); return;
  }
  Serial.printf("[gw] ssh up in %lu ms\n", millis() - t0);

  String body;
  int status = gatewayRequest(link_, "GET", "/v1/version", "", body);
  if (status < 0) {
    fail(LinkState::GatewayDown, link_.error().length() ? link_.error() : String("no response"), true);
    return;
  }
  if (status == 404) {
    fail(LinkState::Incompatible, "moshi-hook is older than 0.3.1", true);
    return;
  }
  if (status != 200) {
    fail(LinkState::GatewayDown, "/v1/version: HTTP " + String(status), true);
    return;
  }
  JsonDocument version;
  if (deserializeJson(version, body)) {
    fail(LinkState::Incompatible, "unreadable /v1/version", true);
    return;
  }
  for (const char* cap : REQUIRED_CAPS) {
    bool found = false;
    for (JsonVariantConst c : version["capabilities"].as<JsonArrayConst>()) found |= c == cap;
    if (!found) {
      fail(LinkState::Incompatible, String("moshi-hook ") + (const char*)(version["version"] | "?") + " lacks " + cap,
           true);
      return;
    }
  }

  String err;
  ssh_channel ch = link_.openGateway();
  if (!ch || !ws_.open(ch, "/events", err)) {
    fail(LinkState::GatewayDown, ch ? "/events: " + err : link_.error(), true);
    return;
  }
  String watch;
  {
    Guard g(*this);
    watch = watchFrame();
    watchDirty_ = false;
  }
  if (!ws_.sendText(watch)) {
    fail(LinkState::Waiting, "could not send watch", true);
    return;
  }
  Guard g(*this);
  if (linkGen_ != gen_) return;  // switched hosts meanwhile: poll() drops this link
  Serial.printf("[gw] online: moshi-hook %s on %s, %lu ms\n", (const char*)(version["version"] | "?"),
                (const char*)(version["hostname"] | "?"), millis() - t0);
  state_ = LinkState::Online;
  detail_ = "";
  backoffMs_ = 0;
  nextAttemptAt_ = 0;
  lastPingAt_ = millis();
  changed_ = true;
}

// Network task, unlocked.
void Gateway::poll() {
  LinkState state;
  bool stale;
  {
    Guard g(*this);
    state = state_;
    stale = linkGen_ != gen_;
    if (stale) linkGen_ = gen_;
  }
  if (stale) disconnect();
  switch (state) {
    case LinkState::Connecting: attempt(); break;
    case LinkState::Online: pollOnline(); break;
    default: {
      Guard g(*this);
      if (state_ == state && nextAttemptAt_ && retryIn() == 0) {
        nextAttemptAt_ = 0;
        state_ = LinkState::Connecting;  // shown first, attempted on the next poll
        changed_ = true;
      }
      break;
    }
  }
}

// Network task, unlocked.
void Gateway::pollOnline() {
  if (WiFi.status() != WL_CONNECTED) {
    fail(LinkState::Waiting, "Wi-Fi is down", true);
    return;
  }
  String watch;
  {
    Guard g(*this);
    if (watchDirty_) watch = watchFrame();
    watchDirty_ = false;
  }
  if (watch.length() && !ws_.sendText(watch)) {
    fail(LinkState::Waiting, "connection lost", true);
    return;
  }
  runRequest();
  {
    Guard g(*this);
    if (state_ != LinkState::Online) return;  // the request found the link dead
  }
  for (int i = 0; i < MAX_MESSAGES_PER_POLL; i++) {
    WsClient::Poll p = ws_.poll();
    if (p == WsClient::Poll::Nothing) break;
    if (p == WsClient::Poll::Closed) {
      fail(LinkState::Waiting, "connection lost", true);
      return;
    }
    Guard g(*this);
    if (linkGen_ != gen_) return;
    if (p == WsClient::Poll::Oversized) {
      Serial.println("[gw] dropped an oversized frame");
      // Workspace frames stay well under the cap; a huge one is a plan.
      if (watchSession_.length()) {
        card_.markOversized();
        changed_ = true;
      }
      continue;
    }
    changed_ |= handleMessage();
    ws_.trim();
  }
  unsigned long now = millis();
  unsigned long quiet = now - ws_.lastReceived();
  if (quiet > DEAD_AFTER_MS) {
    fail(LinkState::Waiting, "gateway stopped answering", true);
  } else if (quiet > PING_AFTER_MS && now - lastPingAt_ > PING_AFTER_MS) {
    lastPingAt_ = now;
    if (!ws_.sendPing()) fail(LinkState::Waiting, "connection lost", true);
  }
}

String Gateway::watchFrame() const {
  JsonDocument w;
  JsonObject watch = w["watch"].to<JsonObject>();
  watch["workspaces"] = true;
  watch["usage"] = true;
  if (watchSession_.length()) {
    watch["agent"]["source"] = watchSource_;
    watch["agent"]["session"] = watchSession_;
  }
  String text;
  serializeJson(w, text);
  return text;
}

void Gateway::watch(const Agent* a) {
  String id = a ? a->id : String();
  String session = a ? a->session : String();
  if (id == watchId_ && session == watchSession_) return;
  watchId_ = id;
  watchSource_ = a ? a->kind : String();
  watchSession_ = session;
  card_.clear();
  watchDirty_ = true;
  changed_ = true;
  Serial.printf("[gw] watching %s\n", id.length() ? id.c_str() : "no agent");
}

bool Gateway::cardPending() const {
  if (!card_.received() || card_.key().isEmpty() || card_.key() == answeredKey_) return false;
  // Requests can outlive their dialog: after a plan menu is answered, its
  // ExitPlanMode approval lingers while the agent works and idles. Only a
  // blocked agent is waiting on anything. The frame's status alone isn't
  // enough: it reverts to "working" 30 minutes into a block, while the tree
  // (read from the screen) still says blocked.
  if (card_.oversized() || card_.blocked()) return true;
  int i = agents_.indexOf(watchId_);
  return i >= 0 && agents_[i].state == AgentState::Blocked;
}

bool Gateway::handleAgentStatus() {
  static JsonDocument filter;
  if (filter.isNull()) Card::buildFilter(filter);

  String oldKey = card_.key();
  bool wasReceived = card_.received();
  JsonDocument& doc = card_.beginUpdate();
  DeserializationError err =
      deserializeJson(doc, ws_.message(), ws_.messageLength(), DeserializationOption::Filter(filter));
  if (err == DeserializationError::NoMemory) {
    Serial.printf("[gw] agent status of %u bytes: no memory\n", (unsigned)ws_.messageLength());
    card_.markOversized();
  } else {
    // Frames for a previous watch can still be in flight after a re-watch.
    bool ours = !err && watchSession_ == (const char*)(doc["agentStatus"]["session"] | "");
    if (err) Serial.printf("[gw] agent status: %s\n", err.c_str());
    if (!card_.endUpdate(ours)) {
      if (!err) card_.clear();  // not ours: keep waiting for the right one
    } else {
      statusUpdated_ = true;
    }
  }
  return card_.key() != oldKey || card_.received() != wasReceived;
}

bool Gateway::handleMessage() {
  static JsonDocument filter;
  if (filter.isNull()) Agents::buildFilter(filter);

  // Tabs carry `"agentStatus":"working"`; only the top-level frame is an object.
  const char* msg = ws_.message();
  bool changed = false;
  if (strstr(msg, "\"workspaces\":{")) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, msg, ws_.messageLength(), DeserializationOption::Filter(filter));
    if (err) Serial.printf("[gw] frame of %u bytes: %s\n", (unsigned)ws_.messageLength(), err.c_str());
    else if (JsonObjectConst ws = doc["workspaces"]) changed |= agents_.update(ws);
  }
  if (watchSession_.length() && strstr(msg, "\"agentStatus\":{")) changed |= handleAgentStatus();
  if (strstr(msg, "\"usage\":[")) {
    static JsonDocument usageFilter;
    if (usageFilter.isNull()) Usage::buildFilter(usageFilter);
    JsonDocument doc;
    DeserializationError err =
        deserializeJson(doc, msg, ws_.messageLength(), DeserializationOption::Filter(usageFilter));
    if (err) Serial.printf("[gw] usage frame: %s\n", err.c_str());
    else if (JsonArrayConst u = doc["usage"]) {
      bool c = usage_.update(u);
      if (c) {
        time_t now = time(nullptr);
        for (const UsageAccount& a : usage_.accounts())
          for (const UsageWindow& w : a.windows)
            Serial.printf("[gw] usage: %s %s %d%%, resets in %lds\n", a.agent.c_str(), w.label.c_str(), w.used,
                          (long)(w.resetsAt - now));
      }
      changed |= c;
    }
  }
  return changed;
}

bool Gateway::startRequest(Request kind, const char* path, const String& body, String& error) {
  if (state_ != LinkState::Online) {
    error = "Not connected";
    return false;
  }
  if (reqPending_) {
    error = "Still sending";
    return false;
  }
  reqKind_ = kind;
  reqPath_ = path;
  reqBody_ = body;
  reqId_++;
  reqPending_ = true;
  return true;
}

bool Gateway::takeResult(Result& r) {
  if (!resultReady_) return false;
  resultReady_ = false;
  r = result_;
  return true;
}

// Network task, unlocked.
void Gateway::runRequest() {
  String path, body;
  uint32_t id;
  {
    Guard g(*this);
    if (!reqPending_) return;
    path = reqPath_;
    body = reqBody_;
    id = reqId_;
  }
  String error;
  int status = post(path, body, error);
  Guard g(*this);
  if (!reqPending_ || reqId_ != id) return;  // abandoned: switched hosts
  reqPending_ = false;
  if (reqKind_ == Request::Answer && (status == 200 || status == 409)) answeredKey_ = reqCardKey_;
  result_ = {reqKind_, status, error};
  resultReady_ = true;
  changed_ = true;
}

bool Gateway::answer(bool approve, const int* choices, String& error) {
  if (!cardPending()) {
    error = "Nothing to answer";
    return false;
  }
  String body;
  const char* path = card_.answerRequest(approve, choices, body);
  if (!path) {
    error = "Can't answer this from the " BOARD_NAME;
    return false;
  }
  if (!startRequest(Request::Answer, path, body, error)) return false;
  reqCardKey_ = card_.key();
  return true;
}

// Network task, unlocked. Returns the HTTP status, or -1; sets `error` to the
// gateway's message for anything but 200.
int Gateway::post(const String& path, const String& body, String& error) {
  unsigned long t0 = millis();
  String resp;
  int status = gatewayRequest(link_, "POST", path.c_str(), body, resp, 1024);
  Serial.printf("[gw] POST %s -> %d in %lu ms %s\n", path.c_str(), status, millis() - t0, resp.c_str());
  if (status == 200) return status;
  if (status < 0) {
    error = "No answer from moshi-hook";
    if (body.length() < 400) Serial.printf("[gw] request body: %s\n", body.c_str());
  } else {
    JsonDocument e;
    error = (!deserializeJson(e, resp) && e["error"].is<const char*>()) ? String((const char*)e["error"])
                                                                         : "HTTP " + String(status);
  }
  return status;
}

namespace {

bool terminalRequest(const Agent& a, JsonDocument& doc, String& error) {
  if (a.session.isEmpty()) {
    error = "Not an agent session";
    return false;
  }
  doc["source"] = a.kind;
  doc["sessionId"] = a.session;
  return true;
}

}  // namespace

bool Gateway::sendKey(const Agent& a, const char* key, String& error) {
  JsonDocument doc;
  if (!terminalRequest(a, doc, error)) return false;
  doc["keys"][0] = key;
  String body;
  serializeJson(doc, body);
  return startRequest(Request::Key, "/v1/keys", body, error);
}

bool Gateway::sendPrompt(const Agent& a, const String& text, String& error) {
  JsonDocument doc;
  if (!terminalRequest(a, doc, error)) return false;
  doc["text"] = text;
  String body;
  serializeJson(doc, body);
  return startRequest(Request::Prompt, "/v1/prompt", body, error);
}

bool Gateway::takeStatusUpdate() {
  bool u = statusUpdated_;
  statusUpdated_ = false;
  return u;
}
