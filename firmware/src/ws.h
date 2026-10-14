#pragma once

#include <Arduino.h>
#include <libssh/libssh.h>

/**
 * A WebSocket client over an already-open SSH channel. Text frames only (plus
 * ping/pong/close); fragmented messages are reassembled. Messages above
 * MAX_MESSAGE are drained and reported, never buffered.
 */
class WsClient {
 public:
  static const size_t MAX_MESSAGE = 32768;

  enum class Poll { Nothing, Message, Oversized, Closed };

  ~WsClient() { reset(); }

  // Takes ownership of `ch` and performs the upgrade for `path`.
  bool open(ssh_channel ch, const char* path, String& error);
  void reset();
  bool isOpen() const { return ch_ != nullptr; }

  bool sendText(const String& text);
  bool sendPing();

  // Non-blocking unless data is waiting: then reads one whole message.
  Poll poll();
  const char* message() const { return buf_; }
  // Free the buffer after an unusually large message, so it isn't held.
  void trim();
  size_t messageLength() const { return len_; }

  // millis() of the last frame of any kind; the liveness signal.
  unsigned long lastReceived() const { return lastRx_; }

 private:
  bool sendFrame(uint8_t opcode, const uint8_t* data, size_t len);
  bool readExact(void* dst, size_t len);
  bool skip(size_t len);
  bool reserve(size_t cap);

  ssh_channel ch_ = nullptr;
  char* buf_ = nullptr;
  size_t cap_ = 0;
  size_t len_ = 0;
  bool oversized_ = false;
  unsigned long lastRx_ = 0;
};
