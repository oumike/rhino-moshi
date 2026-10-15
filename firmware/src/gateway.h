#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "agents.h"
#include "card.h"
#include "settings.h"
#include "ssh_link.h"
#include "transcript.h"
#include "usage.h"
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
 *
 * Threading: poll() runs in the network task and does all the I/O, which can block
 * for seconds. Everything else is for the UI task, called with lock() held;
 * none of it blocks. The network task takes the lock only to change state,
 * never across I/O, so holding it is cheap for the UI -- but the UI must not
 * hold it through a panel refresh.
 */
class Gateway {
 public:
  explicit Gateway(Agents& agents) : agents_(agents) {}

  void begin(ssh_key key);

  // One round of network work, in the network task. May block for seconds.
  void poll();

  void lock() { xSemaphoreTakeRecursive(mutex_, portMAX_DELAY); }
  void unlock() { xSemaphoreGiveRecursive(mutex_); }

  // Anything visible changed since the last call.
  bool takeChanged();

  LinkState state() const { return state_; }
  const HostConfig& host() const { return host_; }
  const String& detail() const { return detail_; }
  const String& fingerprint() const { return fingerprint_; }  // the host's key
  unsigned long retryIn() const;  // ms until the next attempt, in Waiting

  // Drop the current host and connect to settings::hosts()[index], making it
  // the one used from now on.
  void switchHost(int index);
  const Usage& usage() const { return usage_; }
  // The X4's own key, "SHA256:...".
  const String& deviceKeyFingerprint() const { return deviceFp_; }

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

  // Requests to the gateway run one at a time, in the background; each start
  // returns false (with `error`) if it can't. Never retried. takeResult()
  // reports how the last one went.
  enum class Request { Answer, Key, Prompt };
  struct Result {
    Request kind;
    int status;  // HTTP status; -1 if no answer
    String error;
  };
  bool busy() const { return reqPending_; }
  // Answer the card: `approve` for approvals, `choices` (one option index per
  // question) for plans and questions. Status 409 means the request changed
  // on the host and nothing was sent.
  bool answer(bool approve, const int* choices, String& error);
  // Type into agent `a`'s terminal: one key ("Escape", "C-c", "Enter",
  // "BTab", ...) or a prompt, which is submitted.
  bool sendKey(const Agent& a, const char* key, String& error);
  bool sendPrompt(const Agent& a, const String& text, String& error);
  bool takeResult(Result& r);

  // A frame for the watched agent arrived since the last call.
  bool takeStatusUpdate();

  // Follow `a`'s conversation transcript (nullptr: none). Opens a second
  // socket; cheap when unchanged. Only used on boards that show it.
  void watchTranscript(const Agent* a);
  const String& transcriptId() const { return trId_; }
  const Transcript& transcript() const { return transcript_; }
  // Ask the host for an older page; no-op if there's none or one is in flight.
  void requestOlderTranscript();
  bool takeTranscriptUpdate();

 private:
  struct Guard {
    Gateway& g;
    explicit Guard(Gateway& g) : g(g) { g.lock(); }
    ~Guard() { g.unlock(); }
  };

  void pollOnline();
  void pollTranscript();
  void runRequest();
  bool startRequest(Request kind, const char* path, const String& body, String& error);
  int post(const String& path, const String& body, String& error);
  void attempt();
  void fail(LinkState state, const String& detail, bool retry);
  void disconnect();
  bool handleMessage();
  bool handleAgentStatus();
  bool handleTranscript();
  String watchFrame() const;

  SemaphoreHandle_t mutex_ = nullptr;
  Agents& agents_;
  ssh_key key_ = nullptr;
  HostConfig host_;
  String deviceFp_, fingerprint_;
  Usage usage_;
  SshLink link_;  // network task only
  WsClient ws_;   // network task only
  WsClient tws_{96 * 1024};  // transcripts; a larger PSRAM-backed buffer
  Card card_;
  Transcript transcript_;
  String watchId_, watchSource_, watchSession_;
  bool watchDirty_ = false;  // the watch changed and isn't sent yet
  String trId_, trSource_, trSession_;
  bool trDirty_ = false;       // the transcript socket needs (re)opening
  bool trOlderReq_ = false;    // an older page was asked for
  bool trOlderPending_ = false;  // an older request is in flight
  bool transcriptUpdated_ = false;
  String answeredKey_;  // last card answered (or found stale) from the X4
  LinkState state_ = LinkState::Connecting;
  String detail_;
  unsigned long nextAttemptAt_ = 0;
  unsigned long backoffMs_ = 0;
  unsigned long lastPingAt_ = 0;
  bool changed_ = false;
  bool statusUpdated_ = false;
  // Bumped by switchHost(): work begun for an older value is dropped.
  uint32_t gen_ = 0, linkGen_ = 0;

  bool reqPending_ = false, resultReady_ = false;
  uint32_t reqId_ = 0;
  Request reqKind_ = Request::Answer;
  String reqPath_, reqBody_, reqCardKey_;
  Result result_;
};
