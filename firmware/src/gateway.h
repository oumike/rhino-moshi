#pragma once

#include <Arduino.h>

#include "agents.h"
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
 * reconnects. Feeds workspace frames into an Agents list.
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

 private:
  void attempt();
  void fail(LinkState state, const String& detail, bool retry);
  void disconnect();
  bool handleMessage();

  Agents& agents_;
  ssh_key key_ = nullptr;
  HostConfig host_;
  SshLink link_;
  WsClient ws_;
  LinkState state_ = LinkState::Connecting;
  String detail_;
  unsigned long nextAttemptAt_ = 0;
  unsigned long backoffMs_ = 0;
  unsigned long lastPingAt_ = 0;
  bool changed_ = false;
};
