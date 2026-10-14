#include "gateway.h"

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
  key_ = key;
  host_ = settings::loadHost();
  if (!host_.isComplete()) {
    state_ = LinkState::NoHost;
    return;
  }
  state_ = LinkState::Connecting;
}

unsigned long Gateway::retryIn() const {
  long left = (long)(nextAttemptAt_ - millis());
  return nextAttemptAt_ && left > 0 ? left : 0;
}

void Gateway::disconnect() {
  ws_.reset();
  link_.close();
}

void Gateway::fail(LinkState state, const String& detail, bool retry) {
  disconnect();
  state_ = state;
  detail_ = detail;
  changed_ = true;
  nextAttemptAt_ = 0;
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
  if (state_ != LinkState::HostKeyUnknown || fingerprint().isEmpty()) return;
  settings::pinHostKey(fingerprint());
  Serial.printf("[gw] trusted host key %s\n", fingerprint().c_str());
  retryNow();
}

void Gateway::retryNow() {
  if (state_ == LinkState::NoHost || state_ == LinkState::Online) return;
  disconnect();
  backoffMs_ = 0;
  nextAttemptAt_ = 0;
  state_ = LinkState::Connecting;
  changed_ = true;
}

void Gateway::attempt() {
  if (WiFi.status() != WL_CONNECTED) {
    fail(LinkState::Waiting, "Wi-Fi is down", true);
    return;
  }
  Serial.printf("[gw] connecting to %s@%s:%u\n", host_.user.c_str(), host_.address.c_str(), host_.port);
  unsigned long t0 = millis();
  switch (link_.connect(host_, key_)) {
    case SshResult::Ok: break;
    case SshResult::Unreachable: fail(LinkState::Waiting, link_.error(), true); return;
    case SshResult::HostKeyUnknown:
      fail(LinkState::HostKeyUnknown, "", false);
      return;
    case SshResult::HostKeyChanged:
      fail(LinkState::HostKeyChanged, "pinned " + settings::pinnedHostKey(), false);
      return;
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
  if (!ws_.sendText("{\"watch\":{\"workspaces\":true,\"usage\":true}}")) {
    fail(LinkState::Waiting, "could not send watch", true);
    return;
  }
  Serial.printf("[gw] online: moshi-hook %s on %s, %lu ms\n", (const char*)(version["version"] | "?"),
                (const char*)(version["hostname"] | "?"), millis() - t0);
  state_ = LinkState::Online;
  detail_ = "";
  backoffMs_ = 0;
  nextAttemptAt_ = 0;
  lastPingAt_ = millis();
  changed_ = true;
}

bool Gateway::handleMessage() {
  static JsonDocument filter;
  if (filter.isNull()) Agents::buildFilter(filter);

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, ws_.message(), ws_.messageLength(), DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("[gw] frame of %u bytes: %s\n", (unsigned)ws_.messageLength(), err.c_str());
    return false;
  }
  JsonObjectConst ws = doc["workspaces"];
  return ws ? agents_.update(ws) : false;
}

bool Gateway::poll() {
  switch (state_) {
    case LinkState::Connecting:
      attempt();
      break;
    case LinkState::Online: {
      if (WiFi.status() != WL_CONNECTED) {
        fail(LinkState::Waiting, "Wi-Fi is down", true);
        break;
      }
      for (int i = 0; i < MAX_MESSAGES_PER_POLL; i++) {
        WsClient::Poll p = ws_.poll();
        if (p == WsClient::Poll::Nothing) break;
        if (p == WsClient::Poll::Closed) {
          fail(LinkState::Waiting, "connection lost", true);
          break;
        }
        if (p == WsClient::Poll::Oversized) {
          Serial.println("[gw] dropped an oversized frame");
          continue;
        }
        changed_ |= handleMessage();
      }
      if (state_ != LinkState::Online) break;
      unsigned long now = millis();
      unsigned long quiet = now - ws_.lastReceived();
      if (quiet > DEAD_AFTER_MS) {
        fail(LinkState::Waiting, "gateway stopped answering", true);
      } else if (quiet > PING_AFTER_MS && now - lastPingAt_ > PING_AFTER_MS) {
        lastPingAt_ = now;
        if (!ws_.sendPing()) fail(LinkState::Waiting, "connection lost", true);
      }
      break;
    }
    default:
      if (nextAttemptAt_ && retryIn() == 0) {
        nextAttemptAt_ = 0;
        state_ = LinkState::Connecting;  // shown first, attempted on the next poll
        changed_ = true;
      }
      break;
  }
  bool c = changed_;
  changed_ = false;
  return c;
}
