#include "panel.h"

namespace panel {

void dumpBase64(const uint8_t* f, size_t n) {
  static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char line[81];
  int len = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = f[i] << 16 | (i + 1 < n ? f[i + 1] << 8 : 0) | (i + 2 < n ? f[i + 2] : 0);
    line[len++] = B64[v >> 18 & 63];
    line[len++] = B64[v >> 12 & 63];
    line[len++] = i + 1 < n ? B64[v >> 6 & 63] : '=';
    line[len++] = i + 2 < n ? B64[v & 63] : '=';
    if (len == 80 || i + 3 >= n) {
      line[len] = 0;
      Serial.println(line);
      len = 0;
    }
  }
  Serial.println("END");
}

}  // namespace panel
