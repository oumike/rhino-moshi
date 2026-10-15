#include "config_server.h"

#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>

#include "backup.h"
#include "settings.h"

namespace config_server {

const char* HOSTNAME = "rhino-x4";
const char* AP_SSID = "Rhino-X4-Setup";

namespace {

const unsigned long JOIN_TIMEOUT_MS = 20000;

WebServer server(80);
String keyLine;
String networkOptions;  // <option> list from a scan, setup mode only
bool setupMode = false;
unsigned long restartAt = 0;
String upload;          // body of a backup file being uploaded
bool uploadTooLarge = false;
const size_t MAX_UPLOAD = 16384;

String esc(const String& s) {
  String out;
  out.reserve(s.length() + 8);
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c;
    }
  }
  return out;
}

// Setup mode skips the page password: it only runs on a fresh install or with
// Back held at boot (physical access), and it is the recovery path for a
// forgotten page password.
bool authorized() {
  String pw = settings::webPassword();
  if (setupMode || pw.isEmpty()) return true;
  if (server.authenticate("admin", pw.c_str())) return true;
  server.requestAuthentication(BASIC_AUTH, "Rhino X4");
  return false;
}

const char STYLE[] PROGMEM = R"(<style>
body{font-family:-apple-system,system-ui,sans-serif;max-width:560px;margin:2em auto;padding:0 1em;color:#111}
h1{font-size:1.4em}h2{font-size:1.1em;margin-top:1.6em;border-bottom:1px solid #ccc}
label{display:block;margin:.7em 0 .2em;font-weight:600}
input,select,textarea{width:100%;box-sizing:border-box;padding:.45em;font-size:1em}
input[type=checkbox]{width:auto}
textarea{font-family:ui-monospace,monospace;font-size:.8em}
.hint{color:#555;font-size:.9em}button{margin-top:1.5em;padding:.6em 1.4em;font-size:1em}
</style>)";

void sendPage(const String& body) {
  String html = F("<!doctype html><html><head><meta charset=utf-8>"
                  "<meta name=viewport content='width=device-width,initial-scale=1'><title>Rhino X4</title>");
  html += FPSTR(STYLE);
  html += F("</head><body><h1>Rhino X4</h1>");
  html += body;
  html += F("</body></html>");
  server.send(200, "text/html; charset=utf-8", html);
}

void handleRoot() {
  if (!authorized()) return;
  WifiConfig wifi = settings::loadWifi();
  HostConfig host = settings::loadHost();
  String fp = settings::pinnedHostKey();
  uint8_t rot = settings::rotation();
  bool hasPw = settings::webPassword().length() > 0;

  String b;
  b.reserve(4096);
  b += F("<form method=post action=/save>");

  b += F("<h2>Wi-Fi</h2><label>Network</label><input name=w_ssid list=nets required value='");
  b += esc(wifi.ssid);
  b += F("'><datalist id=nets>");
  b += networkOptions;
  b += F("</datalist><label>Password</label><input name=w_pass type=password autocomplete=off placeholder='");
  b += wifi.ssid.length() ? F("unchanged") : F("");
  b += F("'>");

  b += F("<h2>Host</h2><p class=hint>A machine running moshi-hook, reached over SSH.</p>"
         "<label>Name</label><input name=h_name maxlength=24 value='");
  b += esc(host.name);
  b += F("'><label>Address</label><input name=h_addr maxlength=64 placeholder='192.168.1.20 or laptop.local' value='");
  b += esc(host.address);
  b += F("'><label>SSH port</label><input name=h_port type=number min=1 max=65535 value='");
  b += String(host.port);
  b += F("'><label>SSH user</label><input name=h_user maxlength=32 value='");
  b += esc(host.user);
  b += F("'><label>Host key</label><div class=hint>");
  if (fp.length()) {
    b += esc(fp);
    b += F("</div><label><input type=checkbox name=forget_fp> Forget pinned host key</label>");
  } else {
    b += F("Not pinned yet: trusted on first connection.</div>");
  }

  b += F("<h2>Device key</h2><p class=hint>Append this line to <code>~/.ssh/authorized_keys</code> on each host. "
         "It only allows a tunnel to moshi-hook's gateway: no shell, no commands, no other ports.</p>"
         "<textarea readonly rows=5 onclick='this.select()'>");
  b += esc(keyLine);
  b += F("</textarea>");

  b += F("<h2>Display</h2><label>Orientation</label><select name=rotation>");
  b += String("<option value=3") + (rot != 1 ? " selected" : "") + ">Portrait</option>";
  b += String("<option value=1") + (rot == 1 ? " selected" : "") + ">Portrait, upside down</option>";
  b += F("</select>");

  b += F("<h2>Prompts</h2><p class=hint>Canned prompts for the X4's Prompt menu, one per line, in order (up to "
         "20).</p><textarea name=prompts rows=8 style='font-family:inherit;font-size:1em'>");
  b += esc(settings::promptsText());
  b += F("</textarea>");

  b += F("<h2>This page</h2><label>Password</label><input name=web_pw type=password autocomplete=new-password "
         "placeholder='");
  b += hasPw ? F("unchanged") : F("none: anyone on the network can open this page");
  b += F("'><p class=hint>User name is <code>admin</code>. Forgot it? Hold Back while the X4 boots to enter "
         "setup mode.</p>");
  if (hasPw) b += F("<label><input type=checkbox name=clear_pw> Remove password</label>");

  b += F("<button type=submit>Save and restart</button></form>");

  b += F("<h2>Backup</h2><p class=hint>A backup holds every setting above, including the Wi-Fi and page "
         "passwords and the X4's SSH private key: whoever has the file can reach moshi-hook on your hosts. "
         "Keep it private. Restoring restarts the X4.</p>"
         "<form method=get action=/backup><button type=submit>Download backup</button></form>"
         "<form method=post action=/restore enctype=multipart/form-data>"
         "<label>Restore from file</label><input type=file name=backup accept='.json,application/json' required>"
         "<button type=submit>Restore</button></form>"
         "<p class=hint>SD card: <code>");
  b += backup::SD_PATH;
  b += F("</code></p><form method=post action=/sd/backup><button type=submit>Back up to SD card</button></form>"
         "<form method=post action=/sd/restore onsubmit=\"return confirm('Replace all settings with the backup on "
         "the SD card?')\"><button type=submit>Restore from SD card</button></form>");
  sendPage(b);
}

void sendResult(bool ok, const String& message, bool restart) {
  String b = ok ? "<p>" : "<p><b>Failed:</b> ";
  b += esc(message);
  b += "</p>";
  if (restart) {
    b += F("<p class=hint>Restarting&hellip; The settings page will be back at <a href='http://");
    b += HOSTNAME;
    b += F(".local/'>http://");
    b += HOSTNAME;
    b += F(".local</a> once the X4 rejoins Wi-Fi.</p>");
    restartAt = millis() + 1500;
  } else {
    b += F("<p><a href='/'>Back to settings</a></p>");
  }
  sendPage(b);
}

void handleBackup() {
  if (!authorized()) return;
  server.sendHeader("Content-Disposition", "attachment; filename=\"rhino-x4-backup.json\"");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", backup::toJson());
}

void handleUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    upload = "";
    uploadTooLarge = false;
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (upload.length() + up.currentSize > MAX_UPLOAD) uploadTooLarge = true;
    else upload.concat((const char*)up.buf, up.currentSize);
  }
}

void handleRestore() {
  if (!authorized()) return;
  String error;
  bool ok = !uploadTooLarge && backup::restore(upload, error);
  if (uploadTooLarge) error = "File is too large to be a backup.";
  upload = "";
  Serial.printf("Restore from upload: %s\n", ok ? "ok" : error.c_str());
  sendResult(ok, ok ? "Restored from file." : error, ok);
}

void handleSdBackup() {
  if (!authorized()) return;
  String error;
  bool ok = backup::saveToSd(error);
  Serial.printf("Backup to SD: %s\n", ok ? "ok" : error.c_str());
  sendResult(ok, ok ? String("Backed up to the SD card: ") + backup::SD_PATH : error, false);
}

void handleSdRestore() {
  if (!authorized()) return;
  String error;
  bool ok = backup::restoreFromSd(error);
  Serial.printf("Restore from SD: %s\n", ok ? "ok" : error.c_str());
  sendResult(ok, ok ? String("Restored from the SD card.") : error, ok);
}

void handleSave() {
  Serial.println("POST /save");
  if (!authorized()) return;

  WifiConfig oldWifi = settings::loadWifi();
  WifiConfig wifi;
  wifi.ssid = server.arg("w_ssid");
  wifi.ssid.trim();
  wifi.password = server.arg("w_pass");
  // Blank means "keep", but only for the same network: a new network with a
  // blank password is an open one.
  if (wifi.password.isEmpty() && wifi.ssid == oldWifi.ssid) wifi.password = oldWifi.password;
  if (wifi.ssid.isEmpty()) {
    server.send(400, "text/plain", "Wi-Fi network is required.");
    return;
  }
  settings::saveWifi(wifi);

  HostConfig host;
  host.name = server.arg("h_name");
  host.name.trim();
  host.address = server.arg("h_addr");
  host.address.trim();
  long port = server.arg("h_port").toInt();
  host.port = (port > 0 && port <= 65535) ? port : 22;
  host.user = server.arg("h_user");
  host.user.trim();
  if (host.name.isEmpty()) host.name = host.address;
  settings::saveHost(host);
  if (server.hasArg("forget_fp")) settings::forgetHostKey();

  settings::saveRotation(server.arg("rotation").toInt() == 1 ? 1 : 3);
  if (server.hasArg("prompts")) settings::savePrompts(server.arg("prompts"));

  if (server.hasArg("clear_pw")) {
    settings::saveWebPassword("");
  } else if (server.arg("web_pw").length()) {
    settings::saveWebPassword(server.arg("web_pw"));
  }

  Serial.printf("Saved settings: Wi-Fi \"%s\", host %s@%s:%u; restarting.\n", wifi.ssid.c_str(),
                host.user.c_str(), host.address.c_str(), host.port);
  sendPage(String(F("<p>Saved. Restarting&hellip;</p><p class=hint>The settings page will be at "
                    "<a href='http://")) +
           HOSTNAME + F(".local/'>http://") + HOSTNAME + F(".local</a> once the X4 joins ") + esc(wifi.ssid) +
           F(".</p>"));
  restartAt = millis() + 1500;
}

void startServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/backup", HTTP_GET, handleBackup);
  server.on("/restore", HTTP_POST, handleRestore, handleUpload);
  server.on("/sd/backup", HTTP_POST, handleSdBackup);
  server.on("/sd/restore", HTTP_POST, handleSdRestore);
  server.onNotFound([] {
    server.sendHeader("Location", "/");
    server.send(302);
  });
  server.begin();
}

[[noreturn]] void runSetupMode(const Callbacks& cb) {
  setupMode = true;
  WiFi.mode(WIFI_AP_STA);
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() && networkOptions.indexOf("'" + esc(ssid) + "'") < 0)
      networkOptions += "<option value='" + esc(ssid) + "'>";
  }
  WiFi.scanDelete();

  WiFi.softAP(AP_SSID);  // open network
  String url = "http://" + WiFi.softAPIP().toString();
  Serial.printf("Setup mode: join \"%s\", open %s\n", AP_SSID, url.c_str());
  if (cb.onSetupMode) cb.onSetupMode(AP_SSID, url.c_str());

  startServer();
  while (true) {
    handle();
    delay(2);
  }
}

}  // namespace

void begin(const String& authorizedKeysLine, bool forceSetup, const Callbacks& cb) {
  keyLine = authorizedKeysLine;
  WiFi.persistent(false);  // credentials live in our own NVS namespace

  WifiConfig wifi = settings::loadWifi();
  if (forceSetup || !wifi.isComplete()) runSetupMode(cb);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  while (true) {
    if (cb.onConnecting) cb.onConnecting(wifi.ssid.c_str());
    WiFi.begin(wifi.ssid.c_str(), wifi.password.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < JOIN_TIMEOUT_MS) delay(100);
    if (WiFi.status() == WL_CONNECTED) break;
    Serial.printf("Could not join \"%s\"; retrying. Hold Back at boot for setup.\n", wifi.ssid.c_str());
    WiFi.disconnect();
    delay(2000);
  }

  // Modem sleep makes the X4 miss ARP requests on some routers, leaving the
  // settings page unreachable.
  WiFi.setSleep(false);
  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
  startServer();
  Serial.printf("Settings: http://%s.local or http://%s\n", HOSTNAME, WiFi.localIP().toString().c_str());
}

void handle() {
  server.handleClient();
  if (restartAt && (long)(millis() - restartAt) >= 0) ESP.restart();
}

}  // namespace config_server
