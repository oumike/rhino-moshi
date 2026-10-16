#include "transcript.h"

namespace {

// First line of a string, trimmed, capped for a one-line tool summary.
String oneLine(const char* s, size_t cap = 76) {
  String out;
  for (const char* p = s; *p && *p != '\n'; p++) out += *p;
  out.trim();
  if (out.length() > cap) {
    out.remove(cap);
    out += "...";
  }
  return out;
}

const char* baseName(const char* path) {
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

// A readable one-liner for a tool_use block: "$ cmd", "Read file.cpp", etc.
String summarizeTool(JsonObjectConst block) {
  const char* name = block["name"] | "tool";
  JsonObjectConst in = block["input"];
  if (!strcmp(name, "Bash")) {
    const char* cmd = in["command"] | "";
    if (*cmd) return "$ " + oneLine(cmd);
    return "$ " + oneLine(in["description"] | "");
  }
  if (!strcmp(name, "Read") || !strcmp(name, "Write") || !strcmp(name, "Edit") || !strcmp(name, "NotebookEdit")) {
    const char* fp = in["file_path"] | (in["path"] | "");
    return String(name) + " " + baseName(fp);
  }
  if (!strcmp(name, "Grep") || !strcmp(name, "Glob")) return String(name) + " " + oneLine(in["pattern"] | "");
  const char* desc = in["description"] | "";
  if (*desc) return String(name) + ": " + oneLine(desc);
  return name;
}

String joinText(const String& a, const char* b) {
  String t = a;
  if (t.length()) t += "\n\n";
  t += b;
  return t;
}

String joinOptions(JsonArrayConst opts, const char* key = nullptr) {
  String s;
  for (JsonVariantConst o : opts) {
    const char* v = key ? (o[key] | "") : (o | "");
    if (!*v) continue;
    s += (s.length() ? " / " : "") + String(v);
  }
  return s;
}

// The question an agent asked, as readable text, or "" if `block` isn't one.
// Copilot's ask_user carries a message and a form; Claude Code's
// AskUserQuestion carries questions with labelled options.
String questionText(JsonObjectConst block) {
  const char* name = block["name"] | "";
  JsonObjectConst in = block["input"];
  String t;
  if (!strcmp(name, "ask_user")) {
    t = in["message"] | "";
    for (JsonPairConst f : in["requestedSchema"]["properties"].as<JsonObjectConst>()) {
      JsonObjectConst p = f.value();
      String line = "- " + String(p["title"] | f.key().c_str());
      String opts = joinOptions(p["enum"]);
      if (!opts.length()) opts = joinOptions(p["oneOf"], "title");
      if (!opts.length() && !strcmp(p["type"] | "", "boolean")) opts = "yes / no";
      if (opts.length()) line += " (" + opts + ")";
      t += "\n" + line;
    }
  } else if (!strcmp(name, "AskUserQuestion")) {
    for (JsonObjectConst q : in["questions"].as<JsonArrayConst>()) {
      String line = q["question"] | "";
      String opts = joinOptions(q["options"], "label");
      if (opts.length()) line += "\n(" + opts + ")";
      t = joinText(t, line.c_str());
    }
  }
  t.trim();
  return t;
}

}  // namespace

void Transcript::buildFilter(JsonDocument& filter) {
  filter["type"] = true;
  filter["startLine"] = true;
  filter["totalLines"] = true;
  filter["hasMore"] = true;
  JsonObject e = filter["entries"][0].to<JsonObject>();
  e["line"] = true;
  JsonObject raw = e["raw"].to<JsonObject>();
  raw["type"] = true;
  JsonObject msg = raw["message"].to<JsonObject>();
  msg["role"] = true;
  msg["content"] = true;  // string, or blocks (text/tool_use/tool_result)
}

void Transcript::clear() {
  items_.clear();
  oldestLine_ = 0;
  totalLines_ = 0;
  hasMore_ = false;
  received_ = false;
  tokensLeft_ = -1;
  tokensLine_ = -1;
}

// Keeps the newest "<total_tokens>N tokens left</total_tokens>" reminder.
void Transcript::noteTokens(JsonVariantConst content, int line) {
  if (line <= tokensLine_ || !content.is<JsonArrayConst>()) return;
  for (JsonObjectConst block : content.as<JsonArrayConst>()) {
    const char* t = block["text"] | "";
    const char* p = strstr(t, "<total_tokens>");
    if (!p) continue;
    p += strlen("<total_tokens>");
    if (*p < '0' || *p > '9') continue;
    tokensLeft_ = strtoll(p, nullptr, 10);
    tokensLine_ = line;
    return;
  }
}

void Transcript::flatten(JsonObjectConst entry, std::vector<Item>& out) {
  int line = entry["line"] | 0;
  JsonObjectConst msg = entry["raw"]["message"];
  const char* type = entry["raw"]["type"] | "";
  // Claude Code logs each API request as an "api-request-blob" (system prompt,
  // <total_tokens> reminders, ...); those aren't turns of the conversation.
  if (!strcmp(type, "api-request-blob")) {
    noteTokens(msg["content"], line);
    return;
  }
  if (!strcmp(msg["role"] | "", "system")) return;
  const char* role = msg["role"] | type;
  Role r = strcmp(role, "assistant") == 0 ? Role::Assistant : Role::User;

  JsonVariantConst content = msg["content"];
  if (content.is<const char*>()) {
    String t = content.as<const char*>();
    t.trim();
    if (t.length()) out.push_back({r, line, t});
    return;
  }
  if (!content.is<JsonArrayConst>()) return;

  String text;
  std::vector<Item> tools;
  for (JsonObjectConst block : content.as<JsonArrayConst>()) {
    const char* bt = block["type"] | "";
    if (!strcmp(bt, "text")) {
      const char* t = block["text"] | "";
      if (*t) text = joinText(text, t);
    } else if (!strcmp(bt, "tool_use")) {
      String q = questionText(block);
      if (q.length()) text = joinText(text, q.c_str());
      else tools.push_back({Role::Tool, line, summarizeTool(block)});
    }
    // tool_result blocks are dropped: they're the terminal output, not the turn.
  }
  text.trim();
  if (STRING_CAP && text.length() > 2 * STRING_CAP) {  // several capped text blocks
    text.remove(2 * STRING_CAP);
    text += "...";
  }
  if (text.length()) out.push_back({r, line, text});
  for (const Item& it : tools) out.push_back(it);
}

void Transcript::trimFront() {
  if (items_.size() <= MAX_ITEMS) return;
  items_.erase(items_.begin(), items_.begin() + (items_.size() - MAX_ITEMS));
  oldestLine_ = items_.front().line;
  hasMore_ = true;  // older items were dropped locally; they can be re-fetched
}

bool Transcript::apply(JsonObjectConst frame, String& frameType) {
  frameType = frame["type"] | "";
  received_ = true;

  if (frameType == "backlog" || frameType == "resumed" || frameType == "reset") {
    items_.clear();
    oldestLine_ = frame["startLine"] | 0;
    totalLines_ = frame["totalLines"] | 0;
    hasMore_ = frame["hasMore"] | false;
    for (JsonObjectConst e : frame["entries"].as<JsonArrayConst>()) flatten(e, items_);
    return true;
  }

  if (frameType == "older") {
    std::vector<Item> older;
    for (JsonObjectConst e : frame["entries"].as<JsonArrayConst>()) flatten(e, older);
    if (frame["startLine"].is<int>()) oldestLine_ = frame["startLine"];
    hasMore_ = frame["hasMore"] | false;
    if (older.empty()) return true;
    items_.insert(items_.begin(), older.begin(), older.end());
    return true;
  }

  if (frameType == "append") {
    size_t before = items_.size();
    for (JsonObjectConst e : frame["entries"].as<JsonArrayConst>()) flatten(e, items_);
    if (frame["totalLines"].is<int>()) totalLines_ = frame["totalLines"];
    trimFront();
    return items_.size() != before;
  }

  return false;  // subagents, error, etc.
}
