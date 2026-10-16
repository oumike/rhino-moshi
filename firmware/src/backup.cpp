#include "backup.h"

#include <ArduinoJson.h>
#include <SD.h>
#include <SPI.h>

#include "board.h"
#include "settings.h"

namespace backup {

const char* SD_PATH = "/moshi/rhino-moshi-backup.json";

namespace {

const char* FORMAT = "rhino-moshi-backup";
const int VERSION = 1;
const size_t MAX_SIZE = 16384;

// The card shares the SPI bus with the panel. Mount only for the duration of
// one operation so the card can be pulled at any other time.
bool mountSd(String& error) {
  SPI.begin(SPI_SCLK, SPI_MISO, SPI_MOSI);  // no-op if the panel already started it
  if (!SD.begin(SD_CS, SPI, 4000000)) {
    error = "No SD card found.";
    return false;
  }
  return true;
}

void hostToJson(const HostConfig& host, JsonObject h) {
  h["name"] = host.name;
  h["address"] = host.address;
  h["port"] = host.port;
  h["user"] = host.user;
  h["hostKey"] = host.hostKey;
}

bool validPort(JsonVariantConst port) {
  return port.isNull() || (port.is<int>() && port.as<int>() >= 1 && port.as<int>() <= 65535);
}

HostConfig hostFromJson(JsonVariantConst j) {
  HostConfig h;
  h.name = j["name"] | "";
  h.address = j["address"] | "";
  h.port = j["port"] | 22;
  h.user = j["user"] | "";
  h.hostKey = j["hostKey"] | "";
  if (h.name.isEmpty()) h.name = h.address;
  return h;
}

}  // namespace

String toJson() {
  JsonDocument doc;
  doc["format"] = FORMAT;
  doc["version"] = VERSION;

  WifiConfig wifi = settings::loadWifi();
  doc["wifi"]["ssid"] = wifi.ssid;
  doc["wifi"]["password"] = wifi.password;

  // "host" (the active one) is what firmware before multiple hosts restores.
  std::vector<HostConfig> hosts = settings::hosts();
  int active = settings::activeHost();
  JsonArray list = doc["hosts"].to<JsonArray>();
  for (int i = 0; i < (int)hosts.size(); i++) {
    JsonObject h = list.add<JsonObject>();
    hostToJson(hosts[i], h);
    if (i == active) hostToJson(hosts[i], doc["host"].to<JsonObject>());
  }
  doc["activeHost"] = active;

  doc["display"]["screenTimeout"] = settings::screenTimeout();
  doc["web"]["password"] = settings::webPassword();
  doc["deviceKey"] = settings::deviceKey();

  JsonArray prompts = doc["prompts"].to<JsonArray>();
  for (const String& p : settings::prompts()) prompts.add(p);
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
    error = "File is not a Rhino Moshi backup.";
    return false;
  }
  if ((doc["version"] | 0) > VERSION) {
    error = "Backup is from a newer firmware.";
    return false;
  }
  // Older backups also hold a display rotation; the orientation is fixed now,
  // so it is ignored.
  bool portsOk = validPort(doc["host"]["port"]);
  for (JsonVariantConst h : doc["hosts"].as<JsonArrayConst>()) portsOk &= validPort(h["port"]);
  if (!portsOk) {
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

  if (JsonArrayConst list = doc["hosts"]) {
    std::vector<HostConfig> hosts;
    for (JsonVariantConst h : list) hosts.push_back(hostFromJson(h));
    settings::saveHosts(hosts);
    settings::saveActiveHost(doc["activeHost"] | 0);
  } else if (doc["host"].is<JsonObjectConst>()) {  // a backup from before multiple hosts
    settings::saveHosts({hostFromJson(doc["host"])});
    settings::saveActiveHost(0);
  }

  if (doc["display"]["screenTimeout"].is<uint16_t>()) settings::saveScreenTimeout(doc["display"]["screenTimeout"]);
  if (doc["web"]["password"].is<const char*>()) settings::saveWebPassword(doc["web"]["password"].as<String>());

  String key = doc["deviceKey"] | "";
  if (key.length()) settings::saveDeviceKey(key);

  if (JsonArrayConst prompts = doc["prompts"]) {
    String lines;
    for (JsonVariantConst p : prompts) {
      if (!p.is<const char*>()) continue;
      String s = p.as<String>();
      s.replace('\n', ' ');
      lines += s + "\n";
    }
    settings::savePrompts(lines);
  }
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
