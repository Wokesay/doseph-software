// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/push.hpp"

#include <cctype>

#include "doseph/messages.hpp"

namespace doseph {

namespace {

constexpr size_t kMaxField = 256;

// No control characters anywhere: a line break in a header or a mail address would add lines of
// its own (SD-124)
bool plain(const std::string& s) {
  if (s.size() > kMaxField) return false;
  for (unsigned char c : s)
    if (c < 0x20 || c == 0x7f) return false;
  return true;
}

bool allOf(const std::string& s, size_t min, size_t max, bool (*ok)(unsigned char)) {
  if (s.size() < min || s.size() > max) return false;
  for (unsigned char c : s)
    if (!ok(c)) return false;
  return true;
}

bool topicChar(unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; }
bool tokenChar(unsigned char c) { return std::isalnum(c) || c == '_' || c == '-' || c == ':'; }
bool digit(unsigned char c) { return std::isdigit(c) != 0; }

bool url(const std::string& s, std::initializer_list<const char*> schemes) {
  if (!plain(s) || s.find(' ') != std::string::npos) return false;
  for (const char* scheme : schemes) {
    const std::string p = std::string(scheme) + "://";
    if (s.rfind(p, 0) == 0 && s.size() > p.size()) return true;
  }
  return false;
}

bool mail(const std::string& s) {
  const size_t at = s.find('@');
  return plain(s) && s.find(' ') == std::string::npos && s.find(',') == std::string::npos && s.find('<') == std::string::npos &&
         at != std::string::npos && at > 0 && at + 1 < s.size() && s.find('@', at + 1) == std::string::npos;
}

bool chatId(const std::string& s) {
  if (s.empty()) return false;
  if (s[0] == '@') return allOf(s.substr(1), 5, 32, topicChar);
  return allOf(s[0] == '-' ? s.substr(1) : s, 1, 20, digit);
}

}  // namespace

json pushToJson(const PushChannels& p, bool withSecrets) {
  json j = {{"ntfy", {{"enabled", p.ntfy.enabled}, {"server", p.ntfy.server}, {"topic", p.ntfy.topic}}},
            {"telegram", {{"enabled", p.telegram.enabled}, {"chatId", p.telegram.chatId}}},
            {"email",
             {{"enabled", p.email.enabled}, {"server", p.email.server}, {"from", p.email.from}, {"to", p.email.to}, {"user", p.email.user}}}};
  if (withSecrets) {
    j["ntfy"]["token"] = p.ntfy.token;
    j["telegram"]["token"] = p.telegram.token;
    j["email"]["password"] = p.email.password;
  } else {
    j["ntfy"]["tokenSet"] = !p.ntfy.token.empty();
    j["telegram"]["tokenSet"] = !p.telegram.token.empty();
    j["email"]["passwordSet"] = !p.email.password.empty();
  }
  return j;
}

PushChannels pushFromJson(const json& j) {
  PushChannels p;
  if (!j.is_object()) return p;
  // Read through the same checks as a change, so a damaged file gives no channel that sends
  const std::vector<Msg> errors = applyPush(p, j);
  return errors.empty() ? p : PushChannels{};
}

std::vector<Msg> applyPush(PushChannels& p, const json& change) {
  PushChannels n = p;
  std::vector<Msg> errors;
  if (!change.is_object()) return {say("api.bad_input")};
  auto text = [&](const json& o, const char* key, std::string& into) {
    if (!o.contains(key)) return;
    if (!o[key].is_string()) return errors.push_back(say("cfg.push.text", {{"field", key}}));
    into = o[key].get<std::string>();
    if (!plain(into)) errors.push_back(say("cfg.push.text", {{"field", key}}));
  };
  auto flag = [&](const json& o, bool& into) {
    if (!o.contains("enabled")) return;
    if (!o["enabled"].is_boolean()) return errors.push_back(say("api.bad_input"));
    into = o["enabled"].get<bool>();
  };
  if (change.contains("ntfy")) {
    const json& o = change["ntfy"];
    flag(o, n.ntfy.enabled);
    text(o, "server", n.ntfy.server);
    text(o, "topic", n.ntfy.topic);
    text(o, "token", n.ntfy.token);
  }
  if (change.contains("telegram")) {
    const json& o = change["telegram"];
    flag(o, n.telegram.enabled);
    text(o, "chatId", n.telegram.chatId);
    text(o, "token", n.telegram.token);
  }
  if (change.contains("email")) {
    const json& o = change["email"];
    flag(o, n.email.enabled);
    text(o, "server", n.email.server);
    text(o, "from", n.email.from);
    text(o, "to", n.email.to);
    text(o, "user", n.email.user);
    text(o, "password", n.email.password);
  }
  if (!errors.empty()) return errors;
  // What is given must fit; what an enabled channel needs must be there
  if (!url(n.ntfy.server, {"https", "http"})) errors.push_back(say("cfg.push.ntfy_server"));
  if ((n.ntfy.enabled || !n.ntfy.topic.empty()) && !allOf(n.ntfy.topic, 1, 64, topicChar)) errors.push_back(say("cfg.push.ntfy_topic"));
  if (!n.ntfy.token.empty() && !allOf(n.ntfy.token, 1, 128, tokenChar)) errors.push_back(say("cfg.push.token"));
  if ((n.telegram.enabled || !n.telegram.chatId.empty()) && !chatId(n.telegram.chatId)) errors.push_back(say("cfg.push.chat_id"));
  if ((n.telegram.enabled || !n.telegram.token.empty()) && !allOf(n.telegram.token, 20, 128, tokenChar)) errors.push_back(say("cfg.push.token"));
  if ((n.email.enabled || !n.email.server.empty()) && !url(n.email.server, {"smtps", "smtp"})) errors.push_back(say("cfg.push.email_server"));
  if ((n.email.enabled || !n.email.from.empty()) && !mail(n.email.from)) errors.push_back(say("cfg.push.email_address", {{"field", "from"}}));
  if ((n.email.enabled || !n.email.to.empty()) && !mail(n.email.to)) errors.push_back(say("cfg.push.email_address", {{"field", "to"}}));
  if (errors.empty()) p = n;
  return errors;
}

std::vector<Delivery> deliveriesFor(const Notification& n, const PushChannels& p, std::string_view lang, const std::string& device,
                                    long long& nextId, const std::string& only) {
  std::string body;
  for (const auto& l : n.lines) body += (body.empty() ? "" : "\n") + textIn(l, lang);
  const std::string title = device + ": " + textIn(n.title, lang);
  const json all = pushToJson(p, true);
  std::vector<Delivery> out;
  for (const char* ch : {"ntfy", "telegram", "email"}) {
    if (!all[ch]["enabled"].get<bool>() || (!only.empty() && only != ch)) continue;
    out.push_back({nextId++, ch, n.level, title, body, all[ch]});
  }
  return out;
}

}  // namespace doseph
