// SPDX-License-Identifier: AGPL-3.0-or-later
// Push channels without an account (SD-124): ntfy, Telegram and e-mail.
// Their settings live in a file of their own, apart from the configuration:
// a backup, the diagnostics and GET /config never carry them, and the API
// says of a token or password only whether it is set. The core turns each
// notification (alarms.hpp) into one delivery per channel, in the device's
// language; the platform sends it (the simulator through the system's curl)
// and reports back. The core itself has no network (R8).
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "doseph/alarms.hpp"
#include "doseph/common.hpp"

namespace doseph {

struct PushChannels {
  struct Ntfy {
    bool enabled = false;
    std::string server = "https://ntfy.sh";
    std::string topic;  // whoever knows it can read: chosen like a password
    std::string token;  // access token for a protected topic; secret
  } ntfy;
  struct Telegram {
    bool enabled = false;
    std::string chatId;
    std::string token;  // the bot's token; secret
  } telegram;
  struct Email {
    bool enabled = false;
    std::string server;  // smtps://host[:port] or smtp://host[:port] (STARTTLS required)
    std::string from, to, user;
    std::string password;  // secret
  } email;
};

// The file's form, with the secrets; the API's form, with only whether each is set
json pushToJson(const PushChannels& p, bool withSecrets);
PushChannels pushFromJson(const json& j);
// A change from the app: a field left out stays, a secret given as "" is
// cleared. Checks every field and what an enabled channel needs; on an error
// nothing changes and the errors are returned.
std::vector<Msg> applyPush(PushChannels& p, const json& change);

// One notification to one channel: title and text in the device's language,
// and the channel's settings with its secret, for the platform only
struct Delivery {
  long long id = 0;
  std::string channel;  // ntfy | telegram | email
  std::string level;    // critical | warning | info
  std::string title, body;
  json target;
};
std::vector<Delivery> deliveriesFor(const Notification& n, const PushChannels& p, std::string_view lang, const std::string& device,
                                    long long& nextId, const std::string& only = "");

}  // namespace doseph
