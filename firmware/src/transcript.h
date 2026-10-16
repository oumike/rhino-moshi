#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#include <vector>

// ArduinoJson allocator for transcript frames: in PSRAM where there is some,
// so a large frame doesn't starve internal RAM while it's parsed.
struct TranscriptAllocator : ArduinoJson::Allocator {
#if BOARD_HAS_PSRAM
  static const uint32_t CAPS = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
  static const uint32_t CAPS = MALLOC_CAP_8BIT;
#endif
  void* allocate(size_t n) override { return heap_caps_malloc(n, CAPS); }
  void deallocate(void* p) override { heap_caps_free(p); }
  void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, CAPS); }
};

/**
 * Reads a JSON document through, cutting every string longer than `cap`
 * bytes down to `cap` and "...". Without PSRAM a transcript frame can't be
 * held whole: a Claude page can carry megabytes of tool output, and the
 * parse filter can't drop it without also dropping plain-string prompts. A
 * cap of 0 passes everything through.
 */
template <typename Reader>
class CappedJsonReader {
 public:
  CappedJsonReader(Reader& r, size_t cap) : r_(r), cap_(cap) {}

  int read() {
    if (*pend_) return *pend_++;
    for (;;) {
      int c = r_.read();
      if (c < 0 || !cap_) return c;
      if (!inString_) {
        if (c == '"') {
          inString_ = true;
          len_ = 0;
          skipping_ = false;
        }
        return c;
      }
      if (escaped_ || hex_) {  // the rest of an escape goes, or stays, with its backslash
        if (escaped_ && c == 'u') hex_ = 4;
        else if (hex_) hex_--;
        escaped_ = false;
        if (!skipping_) return c;
        continue;
      }
      if (c == '"') {
        inString_ = false;
        return c;
      }
      if (c == '\\') escaped_ = true;
      // Cut on a character boundary, not inside a UTF-8 sequence.
      if (!skipping_ && len_ >= cap_ && (c & 0xC0) != 0x80) {
        skipping_ = true;
        pend_ = "..";
        return '.';
      }
      if (skipping_) continue;
      len_++;
      return c;
    }
  }

  size_t readBytes(char* dst, size_t n) {
    size_t i = 0;
    for (; i < n; i++) {
      int c = read();
      if (c < 0) break;
      dst[i] = (char)c;
    }
    return i;
  }

 private:
  Reader& r_;
  size_t cap_;
  size_t len_ = 0;
  bool inString_ = false, skipping_ = false, escaped_ = false;
  uint8_t hex_ = 0;
  const char* pend_ = "";
};

/**
 * The watched agent's conversation, flattened from moshi-hook's
 * /v1/transcripts frames into readable rendered items: user and assistant
 * text, with tool calls collapsed to one line and tool results dropped.
 *
 * Owned by Gateway; read by the UI under the gateway lock.
 */
class Transcript {
 public:
  enum class Role : uint8_t { User, Assistant, Tool };
  struct Item {
    Role role;
    int line;  // source line of the entry this came from
    String text;
  };

#if BOARD_HAS_PSRAM
  static const size_t MAX_ITEMS = 400;
  static const size_t STRING_CAP = 0;  // frame strings are kept whole
#else
  // A handful of screens on the X4, each item at most a few STRING_CAPs long.
  static const size_t MAX_ITEMS = 40;
  static const size_t STRING_CAP = 600;
#endif

  static void buildFilter(JsonDocument& filter);

  // Fold a parsed, filtered frame into the conversation. Returns true if the
  // rendered items changed. `frameType` is set to the frame's "type".
  bool apply(JsonObjectConst frame, String& frameType);

  void clear();
  bool received() const { return received_; }
  bool empty() const { return items_.empty(); }
  const std::vector<Item>& items() const { return items_; }
  bool hasMore() const { return hasMore_; }
  // Older entries can be paged in: there are some, and (without PSRAM) room.
  bool canLoadOlder() const { return hasMore_ && (BOARD_HAS_PSRAM || items_.size() < MAX_ITEMS); }
  int oldestLine() const { return oldestLine_; }
  int totalLines() const { return totalLines_; }

 private:
  void flatten(JsonObjectConst entry, std::vector<Item>& out) const;
  void trimFront();

  std::vector<Item> items_;
  int oldestLine_ = 0;
  int totalLines_ = 0;
  bool hasMore_ = false;
  bool received_ = false;
};
