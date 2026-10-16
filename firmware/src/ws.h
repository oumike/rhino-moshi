#pragma once

#include <Arduino.h>
#include <libssh/libssh.h>

/**
 * A WebSocket client over an already-open SSH channel. Text frames only (plus
 * ping/pong/close); fragmented messages are reassembled. Messages above
 * MAX_MESSAGE are drained and reported, never buffered.
 *
 * A Streamed client buffers nothing: poll() returns Stream at the start of a
 * message, the caller reads its payload through read()/readBytes() (so it can
 * go straight into deserializeJson with a filter) and then calls endMessage().
 * That suits feeds whose messages have no useful size bound.
 */
class WsClient {
 public:
  static const size_t MAX_MESSAGE = 32768;

  enum class Poll { Nothing, Message, Oversized, Closed, Stream };
  enum class Mode { Buffered, Streamed };

  explicit WsClient(Mode mode = Mode::Buffered) : streamed_(mode == Mode::Streamed) {}

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

  // Streamed mode, after poll() returned Stream: the message's payload, then
  // -1 / a short count at its end or on a dropped link.
  int read();
  size_t readBytes(char* dst, size_t n);
  // Discards whatever of the message is left unread.
  void endMessage();

  // millis() of the last frame of any kind; the liveness signal.
  unsigned long lastReceived() const { return lastRx_; }

 private:
  bool sendFrame(uint8_t opcode, const uint8_t* data, size_t len);
  bool readHeader(bool& fin, uint8_t& opcode, uint64_t& len);
  bool control(uint8_t opcode, uint64_t len);
  bool fill();
  bool readExact(void* dst, size_t len);
  bool skip(size_t len);
  bool reserve(size_t cap);

  ssh_channel ch_ = nullptr;
  char* buf_ = nullptr;
  size_t cap_ = 0;
  size_t len_ = 0;
  bool oversized_ = false;

  bool streamed_;
  bool inMessage_ = false;
  bool fin_ = true;          // the current fragment is the message's last
  uint64_t remaining_ = 0;   // unread bytes of the current fragment
  uint8_t chunk_[1024];      // read-ahead, so the parser isn't a read per byte
  size_t chunkPos_ = 0, chunkLen_ = 0;
  unsigned long lastRx_ = 0;
};
