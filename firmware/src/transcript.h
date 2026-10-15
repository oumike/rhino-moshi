#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#include <vector>

// ArduinoJson allocator backed by PSRAM, so a large transcript frame doesn't
// starve internal RAM while it's parsed.
struct PsRamAllocator : ArduinoJson::Allocator {
  void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
  void deallocate(void* p) override { heap_caps_free(p); }
  void* reallocate(void* p, size_t n) override {
    return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
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

  static const size_t MAX_ITEMS = 400;

  static void buildFilter(JsonDocument& filter);

  // Fold a parsed, filtered frame into the conversation. Returns true if the
  // rendered items changed. `frameType` is set to the frame's "type".
  bool apply(JsonObjectConst frame, String& frameType);

  void clear();
  bool received() const { return received_; }
  bool empty() const { return items_.empty(); }
  const std::vector<Item>& items() const { return items_; }
  bool hasMore() const { return hasMore_; }
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
