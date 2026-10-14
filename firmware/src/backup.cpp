#include "backup.h"

#include <ArduinoJson.h>
#include <SD.h>
#include <SPI.h>

#include "board.h"
#include "settings.h"

namespace backup {

const char* SD_PATH = "/moshi/rhino-x4-backup.json";

namespace {

const char* FORMAT = "rhino-x4-backup";
const int VERSION = 1;
const size_t MAX_SIZE = 8192;

// The card shares the SPI bus with the panel. Mount only for the duration of
// one operation so the card can be pulled at any other time.
bool mountSd(String& error) {
  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI);  // no-op if the panel already started it
  if (!SD.begin(SD_CS, SPI, 4000000)) {
    error = "No SD card found.";
    return false;
  }
  return true;
}

}  // namespace

String toJson() {
  JsonDocument doc;
  doc["format"] = FORMAT;
  doc["version"] = VERSION;

  WifiConfig wifi = settings::loadWifi();
  doc["wifi"]["ssid"] = wifi.ssid;
  doc["wifi"]["password"] = wifi.password;

  HostConfig host = settings::loadHost();
  JsonObject h = doc["host"].to<JsonObject>();
  h["name"] = host.name;
  h["address"] = host.address;
  h["port"] = host.port;
  h["user"] = host.user;
  h["hostKey"] = settings::pinnedHostKey();

  doc["display"]["rotation"] = settings::rotation();
  doc["web"]["password"] = settings::webPassword();
  doc["deviceKey"] = settings::deviceKey();

  String out;
  serializeJsonPretty(doc, out);
  return out;
}

bool restore(const String& json, String& error) {
  if (json.length() > MAX_SIZE) {
    error = "File is too large to be a backup.";
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, json)) {
    error = "File is not valid JSON.";
    return false;
  }
  if (doc["format"] != FORMAT) {
    error = "File is not a Rhino X4 backup.";
    return false;
  }
  if ((doc["version"] | 0) > VERSION) {
    error = "Backup is from a newer firmware.";
    return false;
  }
  JsonVariantConst rot = doc["display"]["rotation"];
  if (!rot.isNull() && rot != 1 && rot != 3) {
    error = "Backup has an invalid orientation.";
    return false;
  }
  JsonVariantConst port = doc["host"]["port"];
  if (!port.isNull() && (!port.is<int>() || port.as<int>() < 1 || port.as<int>() > 65535)) {
    error = "Backup has an invalid SSH port.";
    return false;
  }

  JsonVariantConst wifi = doc["wifi"];
  if (wifi.is<JsonObjectConst>()) {
    WifiConfig w = settings::loadWifi();
    w.ssid = wifi["ssid"] | w.ssid;
    w.password = wifi["password"] | w.password;
    settings::saveWifi(w);
  }

  JsonVariantConst host = doc["host"];
  if (host.is<JsonObjectConst>()) {
    HostConfig h = settings::loadHost();
    h.name = host["name"] | h.name;
    h.address = host["address"] | h.address;
    h.port = host["port"] | h.port;
    h.user = host["user"] | h.user;
    settings::saveHost(h);
    // After saveHost, which drops the pin when the address changes.
    String fp = host["hostKey"] | "";
    if (fp.length()) settings::pinHostKey(fp);
    else settings::forgetHostKey();
  }

  if (!rot.isNull()) settings::saveRotation(rot.as<uint8_t>());
  if (doc["web"]["password"].is<const char*>()) settings::saveWebPassword(doc["web"]["password"].as<String>());

  String key = doc["deviceKey"] | "";
  if (key.length()) settings::saveDeviceKey(key);
  return true;
}

bool saveToSd(String& error) {
  if (!mountSd(error)) return false;
  bool ok = false;
  if (!SD.exists("/moshi") && !SD.mkdir("/moshi")) {
    error = "Could not create /moshi on the SD card.";
  } else {
    File f = SD.open(SD_PATH, FILE_WRITE);
    String json = toJson();
    if (!f) {
      error = "Could not open the backup file for writing.";
    } else if (f.print(json) != json.length()) {
      error = "Could not write the backup file (card full?).";
      f.close();
    } else {
      f.close();
      ok = true;
    }
  }
  SD.end();
  return ok;
}

bool restoreFromSd(String& error) {
  if (!mountSd(error)) return false;
  String json;
  File f = SD.open(SD_PATH, FILE_READ);
  if (!f) {
    error = String("No backup at ") + SD_PATH + " on the SD card.";
  } else if (f.size() > MAX_SIZE) {
    error = "File is too large to be a backup.";
  } else {
    json = f.readString();
    if (json.isEmpty()) error = "The backup file on the SD card is empty.";
  }
  if (f) f.close();
  SD.end();
  return json.length() && restore(json, error);
}

}  // namespace backup
