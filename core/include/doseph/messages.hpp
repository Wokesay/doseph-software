// SPDX-License-Identifier: AGPL-3.0-or-later
// Texts the robot sends: a stable key, its values as arguments and
// an English text. The English templates live in one table
// (core/src/messages.cpp); the web app holds the German ones
// (web/src/lang/msg.ts), which the core gets as a generated copy. tools/i18n_keys.test.mjs checks that both tables
// have the same keys and placeholders and that every say("…") key exists.
#pragma once

#include <string>
#include <string_view>

#include "doseph/common.hpp"

namespace doseph {

// A message from the table, its English text filled from the arguments.
// An unknown key gives the key itself as text (and fails the key test).
Msg say(std::string_view key, json args = json::object());

// Fills {name} and {name:N} in a template. A number with N gets N
// decimals, without N as many as it has (up to 6); a missing or null value
// gives "–", never 0 (R5); a nested message gives its text; a list gives
// its items joined by ", ".
std::string render(std::string_view tmpl, const json& args);

// A message's text in a language: German ("de") from the web app's own
// templates (core/src/messages_de.inc, made by tools/gen_msg_de.mjs), with a
// decimal comma and nested messages in German too; otherwise, and for a key
// without a German template, its English text. For what the device sends
// itself, such as a push (#10, step 5).
std::string textIn(const Msg& m, std::string_view lang);

// Whether the table has a template for the key.
bool knownMessage(std::string_view key);

// Whether the arguments carry every value the key's template names (a null
// value counts: it shows as "–"). A message saved under an older template
// that does not fit keeps its own text instead (events.cpp).
bool fitsTemplate(std::string_view key, const json& args);

}  // namespace doseph
