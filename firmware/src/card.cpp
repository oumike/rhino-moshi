#include "card.h"

namespace {

// Appends `s` as a JSON string literal.
void jsonString(String& out, const char* s) {
  out += '"';
  for (; *s; s++) {
    char c = *s;
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if ((uint8_t)c < 0x20) {
      char esc[8];
      snprintf(esc, sizeof(esc), "\\u%04x", (uint8_t)c);
      out += esc;
    } else {
      out += c;
    }
  }
  out += '"';
}

void field(String& out, const char* name, const char* value) {
  jsonString(out, name);
  out += ':';
  jsonString(out, value);
}

}  // namespace

void Card::buildFilter(JsonDocument& filter) {
  JsonObject s = filter["agentStatus"].to<JsonObject>();
  s["source"] = true;
  s["session"] = true;
  s["status"] = true;
  JsonObject a = s["pendingApproval"].to<JsonObject>();
  for (const char* k : {"actionId", "title", "message", "toolName"}) a[k] = true;
  s["planMenu"]["title"] = true;
  s["planMenu"]["options"] = true;
  JsonObject p = s["pendingPrompt"].to<JsonObject>();
  for (const char* k : {"kind", "toolUseId", "plan"}) p[k] = true;
  JsonObject q = p["questions"][0].to<JsonObject>();
  for (const char* k : {"question", "header", "multiSelect"}) q[k] = true;
  q["options"][0]["label"] = true;
  q["options"][0]["description"] = true;
}

bool Card::endUpdate(bool ok) {
  received_ = true;
  if (!ok || !status()) {
    doc_.clear();
    key_ = "";
    return false;
  }
  switch (kind()) {
    case Kind::Approval: key_ = String("a:") + (approval()["actionId"] | ""); break;
    case Kind::Plan: key_ = String("p:") + (prompt()["toolUseId"] | ""); break;
    case Kind::Question: key_ = String("q:") + (prompt()["toolUseId"] | ""); break;
    case Kind::Menu: key_ = String("m:") + menuTitle(); break;
    case Kind::None: key_ = ""; break;
  }
  return true;
}

Card::Kind Card::kind() const {
  if (oversized_ || !status()) return Kind::None;
  // The native question/plan card wins over an approval.
  const char* k = prompt()["kind"] | "";
  if (!strcmp(k, "question")) return Kind::Question;
  if (!strcmp(k, "plan")) return Kind::Plan;
  // A permission dialog also matches the plan-menu scraper; the approval wins.
  if (approval()) return Kind::Approval;
  if (status()["planMenu"]) return Kind::Menu;
  return Kind::None;
}

size_t Card::questionCount() const {
  Kind k = kind();
  if (k == Kind::Question) return prompt()["questions"].size();
  return k == Kind::Plan || k == Kind::Menu ? 1 : 0;
}

JsonArrayConst Card::options(size_t q) const {
  if (kind() == Kind::Question) return question(q)["options"];
  return status()["planMenu"]["options"];
}

size_t Card::optionCount(size_t q) const { return options(q).size(); }

const char* Card::optionLabel(size_t q, size_t i) const {
  JsonVariantConst o = options(q)[i];
  if (o.is<const char*>()) return o.as<const char*>();
  return o["label"] | "";
}

const char* Card::optionDescription(size_t q, size_t i) const { return options(q)[i]["description"] | ""; }

bool Card::answerable(String* why) const {
  String reason;
  if (oversized_) {
    reason = "Too big for the X4 to show. Answer this in the terminal.";
  } else {
    switch (kind()) {
      case Kind::None: reason = "Nothing to answer."; break;
      case Kind::Approval:
        if (!*(approval()["actionId"] | "")) reason = "Answer this in the terminal.";
        break;
      case Kind::Menu: reason = "Answer this in the terminal."; break;
      case Kind::Plan:
        if (!optionCount(0)) reason = "Waiting for the plan menu...";
        else if (!*plan() || !*(prompt()["toolUseId"] | "")) reason = "Answer this in the terminal.";
        break;
      case Kind::Question: {
        size_t n = questionCount();
        if (!n || n > MAX_QUESTIONS || !*(prompt()["toolUseId"] | "")) reason = "Answer this in the terminal.";
        for (size_t q = 0; q < n && reason.isEmpty(); q++) {
          if (question(q)["multiSelect"] | false) reason = "Multiple choice: answer this in the terminal.";
          else if (!optionCount(q)) reason = "Answer this in the terminal.";
        }
        break;
      }
    }
  }
  if (why) *why = reason;
  return reason.isEmpty();
}

const char* Card::answerRequest(bool approve, const int* choices, String& body) const {
  if (!answerable()) return nullptr;
  body = "{";
  field(body, "source", source());
  body += ',';
  field(body, "sessionId", session());
  body += ',';
  switch (kind()) {
    case Kind::Approval:
      field(body, "actionId", approval()["actionId"] | "");
      body += ',';
      field(body, "decision", approve ? "approve" : "deny");
      body += '}';
      return "/v1/approvals/answer";

    case Kind::Plan: {
      size_t n = optionCount(0);
      if (choices[0] < 0 || choices[0] >= (int)n) return nullptr;
      const char* p = plan();
      body.reserve(body.length() + strlen(p) + 512);
      field(body, "toolUseId", prompt()["toolUseId"] | "");
      body += ",\"prompt\":{";
      field(body, "title", menuTitle());
      body += ',';
      field(body, "plan", p);
      body += ",\"options\":[";
      for (size_t i = 0; i < n; i++) {
        if (i) body += ',';
        jsonString(body, optionLabel(0, i));
      }
      body += "],\"optionIndex\":" + String(choices[0]) + "}}";
      return "/v1/plans/answer";
    }

    case Kind::Question: {
      size_t n = questionCount();
      field(body, "toolUseId", prompt()["toolUseId"] | "");
      body += ",\"questions\":[";
      for (size_t q = 0; q < n; q++) {
        if (q) body += ',';
        body += '{';
        field(body, "question", questionText(q));
        body += ",\"multiSelect\":false,\"options\":[";
        for (size_t i = 0; i < optionCount(q); i++) {
          if (i) body += ',';
          jsonString(body, optionLabel(q, i));
        }
        body += "]}";
      }
      body += "],\"answers\":[";
      for (size_t q = 0; q < n; q++) {
        if (choices[q] < 0 || choices[q] >= (int)optionCount(q)) return nullptr;
        if (q) body += ',';
        body += "{\"optionIndexes\":[" + String(choices[q]) + "]}";
      }
      body += "]}";
      return "/v1/questions/answer";
    }

    default: return nullptr;
  }
}
