#pragma once

#include <Arduino.h>

#include "agents.h"
#include "card.h"
#include "settings.h"
#include "ssh_link.h"
#include "ws.h"

enum class LinkState {
  NoHost,          // nothing configured: use the settings page
  Waiting,         // between attempts (see retryIn())
  Connecting,
  Online,
  HostKeyUnknown,  // first connection: fingerprint() needs Trust
  HostKeyChanged,  // refused until the pin is forgotten on the settings page
  AuthRefused,     // the X4's key is not in the host's authorized_keys
  GatewayDown,     // SSH works, but nothing listens on the gateway port
  Incompatible,    // moshi-hook too old or missing a needed capability
};

/**
 * The connection to one host's moshi-hook: SSH session, /events socket and
 * reconnects. Feeds workspace frames into an Agents list, and follows one
 * agent's status (its pending approval, plan or question) in a Card.
 */
class Gateway {
 public:
  explicit Gateway(Agents& agents) : agents_(agents) {}

  void begin(ssh_key key);

  // Advance the state machine and drain the socket. May block for a few
  // seconds while connecting. Returns true if anything visible changed.
  bool poll();

  LinkState state() const { return state_; }
  const HostConfig& host() const { return host_; }
  const String& detail() const { return detail_; }
  const String& fingerprint() const { return link_.fingerprint(); }
  unsigned long retryIn() const;  // ms until the next attempt, in Waiting

  // Pin the shown host key and connect.
  void trustHostKey();
  // Skip any backoff and connect now.
  void retryNow();

  // Point the one agent watch at `a` (nullptr: none). Cheap when unchanged.
  void watch(const Agent* a);
  const String& watchedId() const { return watchId_; }
  const Card& card() const { return card_; }
  // A request is waiting on the watched agent and wasn't answered from here.
  bool cardPending() const;

  enum class Answer { Sent, Stale, Failed };
  // Answer the card: `approve` for approvals, `choices` (one option index per
  // question) for plans and questions. Blocks for the request. Never retried:
  // Stale (409) means the request changed on the host and nothing was sent.
  Answer answer(bool approve, const int* choices, String& error);

 private:
  void attempt();
  void fail(LinkState state, const String& detail, bool retry);
  void disconnect();
  bool handleMessage();
  bool handleAgentStatus();
  bool sendWatch();

  Agents& agents_;
  ssh_key key_ = nullptr;
  HostConfig host_;
  SshLink link_;
  WsClient ws_;
  Card card_;
  String watchId_, watchSource_, watchSession_;
  String answeredKey_;  // last card answered (or found stale) from the X4
  LinkState state_ = LinkState::Connecting;
  String detail_;
  unsigned long nextAttemptAt_ = 0;
  unsigned long backoffMs_ = 0;
  unsigned long lastPingAt_ = 0;
  bool changed_ = false;
};
