#pragma once

#include <Arduino.h>

/**
 * Settings backup as a JSON document: Wi-Fi, host, pinned host key,
 * orientation, page password and the X4's SSH private key. Restoring a backup
 * onto a new or reflashed X4 makes it the same device as far as hosts are
 * concerned, so the file must be kept private.
 */
namespace backup {

extern const char* SD_PATH;

String toJson();

// Applies the fields present in `json`. On failure nothing is changed and
// `error` says why. A restart is needed for the result to take full effect.
bool restore(const String& json, String& error);

bool saveToSd(String& error);
bool restoreFromSd(String& error);

}  // namespace backup
