#include "ws.h"

#include <esp_heap_caps.h>
#include <esp_random.h>

#include "ssh_link.h"

namespace {
const int READ_TIMEOUT_MS = 10000;  // once a frame has started, the rest is close behind
}

void WsClient::reset() {
  closeChannel(ch_);
  ch_ = nullptr;
  free(buf_);
  buf_ = nullptr;
  cap_ = len_ = 0;
  oversized_ = false;
}

bool WsClient::open(ssh_channel ch, const char* path, String& error) {
  reset();
  ch_ = ch;
  // The key only proves the server speaks WebSocket; it needn't be secret.
  String req = String("GET ") + path +
               " HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
  if (ssh_channel_write(ch_, req.c_str(), req.length()) != (int)req.length()) {
    error = "write failed";
    reset();
    return false;
  }
  String headers;
  char c;
  while (!headers.endsWith("\r\n\r\n")) {
    if (headers.length() > 2048 || ssh_channel_read_timeout(ch_, &c, 1, 0, READ_TIMEOUT_MS) != 1) {
      error = "no upgrade response";
      reset();
      return false;
    }
    headers += c;
  }
  if (headers.indexOf(" 101 ") < 0) {
    error = headers.substring(0, headers.indexOf('\r'));
    reset();
    return false;
  }
  lastRx_ = millis();
  return true;
}

bool WsClient::sendFrame(uint8_t opcode, const uint8_t* data, size_t len) {
  if (!ch_) return false;
  uint8_t hdr[14] = {(uint8_t)(0x80 | opcode)};
  size_t h = 2;
  if (len < 126) {
    hdr[1] = 0x80 | len;
  } else if (len <= 0xffff) {
    hdr[1] = 0x80 | 126;
    hdr[2] = len >> 8;
    hdr[3] = len & 0xff;
    h = 4;
  } else {
    return false;
  }
  uint32_t m = esp_random();
  uint8_t* mask = hdr + h;
  memcpy(mask, &m, 4);
  h += 4;
  if (ssh_channel_write(ch_, hdr, h) != (int)h) return false;

  uint8_t chunk[256];
  for (size_t off = 0; off < len; off += sizeof(chunk)) {
    size_t n = min(sizeof(chunk), len - off);
    for (size_t i = 0; i < n; i++) chunk[i] = data[off + i] ^ mask[(off + i) % 4];
    if (ssh_channel_write(ch_, chunk, n) != (int)n) return false;
  }
  return true;
}

bool WsClient::sendText(const String& text) { return sendFrame(0x1, (const uint8_t*)text.c_str(), text.length()); }

bool WsClient::sendPing() { return sendFrame(0x9, nullptr, 0); }

bool WsClient::readExact(void* dst, size_t len) {
  uint8_t* p = (uint8_t*)dst;
  while (len) {
    int n = ssh_channel_read_timeout(ch_, p, len, 0, READ_TIMEOUT_MS);
    if (n <= 0) return false;
    p += n;
    len -= n;
  }
  return true;
}

bool WsClient::skip(size_t len) {
  uint8_t sink[256];
  while (len) {
    size_t n = min(sizeof(sink), len);
    if (!readExact(sink, n)) return false;
    len -= n;
  }
  return true;
}

void WsClient::trim() {
  if (cap_ <= 8192) return;
  free(buf_);
  buf_ = nullptr;
  cap_ = len_ = 0;
}

bool WsClient::reserve(size_t cap) {
  if (cap <= cap_) return true;
  // A large socket (transcripts) keeps its buffer in PSRAM; the small /events
  // one stays in internal RAM. Each instance sticks to one allocator.
  char* p = maxMessage_ > MAX_MESSAGE ? (char*)heap_caps_realloc(buf_, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                      : (char*)realloc(buf_, cap);
  if (!p) return false;
  buf_ = p;
  cap_ = cap;
  return true;
}

WsClient::Poll WsClient::poll() {
  if (!ch_) return Poll::Closed;
  int avail = ssh_channel_poll(ch_, 0);
  if (avail == SSH_ERROR || avail == SSH_EOF) {
    reset();
    return Poll::Closed;
  }
  if (avail <= 0) return Poll::Nothing;

  // A message may span several frames; read until FIN on a data frame.
  len_ = 0;
  oversized_ = false;
  while (true) {
    uint8_t h[2];
    if (!readExact(h, 2)) break;
    bool fin = h[0] & 0x80;
    uint8_t opcode = h[0] & 0x0f;
    uint64_t len = h[1] & 0x7f;
    if (len == 126) {
      uint8_t e[2];
      if (!readExact(e, 2)) break;
      len = (e[0] << 8) | e[1];
    } else if (len == 127) {
      uint8_t e[8];
      if (!readExact(e, 8)) break;
      len = 0;
      for (int i = 0; i < 8; i++) len = (len << 8) | e[i];
    }
    if (h[1] & 0x80) break;  // servers must not mask
    lastRx_ = millis();

    if (opcode >= 0x8) {  // control frames: <= 125 bytes, never fragmented
      if (len > 125) break;
      uint8_t payload[125];
      if (!readExact(payload, len)) break;
      if (opcode == 0x8) {
        sendFrame(0x8, payload, len >= 2 ? 2 : 0);
        break;
      }
      if (opcode == 0x9) sendFrame(0xA, payload, len);
      if (len_ == 0 && !oversized_) return Poll::Nothing;
      continue;  // a control frame between fragments
    }

    if (oversized_ || len_ + len > maxMessage_) {
      oversized_ = true;
      if (!skip(len)) break;
    } else {
      if (!reserve(len_ + len + 1) || !readExact(buf_ + len_, len)) break;
      len_ += len;
      buf_[len_] = 0;
    }
    if (fin) return oversized_ ? Poll::Oversized : Poll::Message;
  }
  reset();
  return Poll::Closed;
}
