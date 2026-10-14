#pragma once

#include <Arduino.h>

#include "agents.h"
#include "buttons.h"
#include "gateway.h"

/** Screens, input and e-paper refresh policy. Portrait 480x800. */
namespace ui {

void begin();

// A full-screen message, for boot and setup before the gateway runs.
void message(const String& title, const String& body);

// Handle button presses and redraw if needed. Call every loop iteration.
// `changed` is true when the gateway reported a visible change.
void update(Gateway& gw, Agents& agents, Buttons& buttons, bool changed);

}  // namespace ui
