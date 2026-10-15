#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

/**
 * The watched agent's pending request, from its `agentStatus` frames.
 *
 * The filtered frame itself is the backing store, so a plan's markdown (which
 * /v1/plans/answer needs echoed back) is held once, not copied around.
 */
class Card {
 public:
  enum class Kind : uint8_t {
    None,      // nothing pending
    Approval,  // pendingApproval: approve / deny
    Plan,      // pendingPrompt kind "plan" with its planMenu
    Question,  // pendingPrompt kind "question"
    Menu,      // a planMenu with nothing to answer it through
  };

  static const size_t MAX_QUESTIONS = 8;  // Claude asks at most 4

  static void buildFilter(JsonDocument& filter);

  // Release the old frame first, so a big plan is never held twice.
  JsonDocument& beginUpdate() {
    doc_.clear();
    oversized_ = false;
    return doc_;
  }
  // After deserializing into beginUpdate(); false if the frame was unusable.
  bool endUpdate(bool ok);
  void clear() {
    doc_.clear();
    received_ = false;
    oversized_ = false;
    key_ = "";
  }
  // A frame too big to buffer: something is pending, but it can't be shown.
  void markOversized() {
    doc_.clear();
    received_ = true;
    oversized_ = true;
    key_ = "oversized";
  }

  bool received() const { return received_; }  // a frame arrived for this watch
  bool oversized() const { return oversized_; }
  Kind kind() const;
  // Identifies the request being shown: actionId or toolUseId.
  const String& key() const { return key_; }

  const char* source() const { return status()["source"] | ""; }
  const char* session() const { return status()["session"] | ""; }
  bool blocked() const { return !strcmp(status()["status"] | "", "blocked"); }

  // Live details for the agent page.
  const char* modelName() const { return status()["modelName"] | ""; }
  int contextRemaining() const { return status()["contextRemaining"] | -1; }  // percent, or -1
  JsonArrayConst activity() const { return status()["ephemeral"]["lines"]; }   // "Thinking... (12s)"

  // Approval
  const char* toolName() const { return approval()["toolName"] | ""; }
  const char* approvalTitle() const { return approval()["title"] | ""; }
  const char* approvalMessage() const { return approval()["message"] | ""; }

  // Plan / Menu
  const char* menuTitle() const { return status()["planMenu"]["title"] | ""; }
  const char* plan() const { return prompt()["plan"] | ""; }

  // Question (q) / Plan / Menu (q ignored): options to choose from.
  size_t questionCount() const;
  const char* questionHeader(size_t q) const { return question(q)["header"] | ""; }
  const char* questionText(size_t q) const { return question(q)["question"] | ""; }
  size_t optionCount(size_t q) const;
  const char* optionLabel(size_t q, size_t i) const;
  const char* optionDescription(size_t q, size_t i) const;

  // Whether the X4 can answer this card, and if not, why.
  bool answerable(String* why = nullptr) const;

  // Request for the answer endpoint. `choices` holds one option index per
  // question (Plan uses choices[0]). Returns the path, or nullptr.
  const char* answerRequest(bool approve, const int* choices, String& body) const;

 private:
  JsonObjectConst status() const { return doc_["agentStatus"]; }
  JsonObjectConst approval() const { return status()["pendingApproval"]; }
  JsonObjectConst prompt() const { return status()["pendingPrompt"]; }
  JsonObjectConst question(size_t q) const { return prompt()["questions"][q]; }
  JsonArrayConst options(size_t q) const;

  JsonDocument doc_;
  bool received_ = false;
  bool oversized_ = false;
  String key_;
};
