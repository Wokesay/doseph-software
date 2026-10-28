// SPDX-License-Identifier: AGPL-3.0-or-later
// Push channels without an account: settings, secrets, deliveries (SD-124).
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <thread>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "doseph/messages.hpp"
#include "doseph/push.hpp"
#include "push.hpp"
#include "simulation.hpp"

using namespace doseph;

namespace {

const std::string kBotToken = "123456789:AAH-abcdefghijklmnopqrstuvwxyz_0123";

bool hasError(const std::vector<Msg>& errors, const std::string& key) {
  for (const auto& e : errors)
    if (e.key == key) return true;
  return false;
}

}  // namespace

TEST_CASE("Push channels: every field is checked, no control characters anywhere, and an enabled channel needs what it sends with; on an error nothing changes (SD-124)") {
  PushChannels p;
  CHECK(applyPush(p, {{"ntfy", {{"enabled", true}, {"topic", "doseph-tank-7f3k"}}}}).empty());
  CHECK(p.ntfy.enabled);
  CHECK(p.ntfy.server == "https://ntfy.sh");
  // A line break would add a header of its own
  CHECK(hasError(applyPush(p, {{"ntfy", {{"topic", "a\r\nX-Evil: 1"}}}}), "cfg.push.text"));
  CHECK(hasError(applyPush(p, {{"email", {{"to", "me@example.org\r\nBcc: you@example.org"}}}}), "cfg.push.text"));
  CHECK(p.ntfy.topic == "doseph-tank-7f3k");  // unchanged
  CHECK(hasError(applyPush(p, {{"ntfy", {{"topic", "has space"}}}}), "cfg.push.ntfy_topic"));
  CHECK(hasError(applyPush(p, {{"ntfy", {{"server", "ftp://example.org"}}}}), "cfg.push.ntfy_server"));
  CHECK(applyPush(p, {{"ntfy", {{"server", "http://192.168.1.5:8080"}}}}).empty());  // an own server in the home network
  CHECK(hasError(applyPush(p, {{"telegram", {{"enabled", true}}}}), "cfg.push.chat_id"));
  CHECK(hasError(applyPush(p, {{"telegram", {{"enabled", true}, {"chatId", "12345"}}}}), "cfg.push.token"));
  CHECK(applyPush(p, {{"telegram", {{"enabled", true}, {"chatId", "-100123"}, {"token", kBotToken}}}}).empty());
  CHECK(applyPush(p, {{"telegram", {{"chatId", "@my_channel"}}}}).empty());
  CHECK(hasError(applyPush(p, {{"email", {{"enabled", true}, {"server", "https://mail.example.org"}}}}), "cfg.push.email_server"));
  CHECK(hasError(applyPush(p, {{"email", {{"enabled", true}, {"server", "smtps://mail.example.org"}, {"from", "a@b"}, {"to", "nobody"}}}}),
                 "cfg.push.email_address"));
  CHECK(applyPush(p, {{"email", {{"enabled", true}, {"server", "smtps://mail.example.org:465"}, {"from", "tank@example.org"}, {"to", "me@example.org"},
                                 {"user", "tank"}, {"password", "s3cret"}}}})
            .empty());
  CHECK(hasError(applyPush(p, {{"ntfy", {{"enabled", "yes"}}}}), "api.bad_input"));
}

TEST_CASE("Push channels: a secret is never shown, only whether it is set; left out it stays, \"\" clears it; a damaged file gives no channel (SD-124)") {
  PushChannels p;
  REQUIRE(applyPush(p, {{"telegram", {{"enabled", true}, {"chatId", "42"}, {"token", kBotToken}}},
                        {"email", {{"server", "smtp://mail.example.org:587"}, {"from", "a@example.org"}, {"to", "b@example.org"}, {"password", "pw"}}}})
              .empty());
  const json shown = pushToJson(p, false);
  CHECK(shown["telegram"]["tokenSet"] == true);
  CHECK(shown["email"]["passwordSet"] == true);
  CHECK(shown.dump().find(kBotToken) == std::string::npos);
  CHECK(shown.dump().find("\"pw\"") == std::string::npos);
  REQUIRE(applyPush(p, {{"telegram", {{"chatId", "43"}}}}).empty());
  CHECK(p.telegram.token == kBotToken);
  REQUIRE(applyPush(p, {{"email", {{"password", ""}}}}).empty());
  CHECK(p.email.password.empty());
  // The file keeps the secrets; read back it is the same, and a damaged one is no channel at all
  const PushChannels back = pushFromJson(pushToJson(p, true));
  CHECK(back.telegram.token == kBotToken);
  CHECK(back.telegram.chatId == "43");
  json bad = pushToJson(p, true);
  bad["telegram"]["chatId"] = "not a chat";
  CHECK_FALSE(pushFromJson(bad).telegram.enabled);
}

TEST_CASE("Push deliveries: one per enabled channel, titled with the device's name, in the device's language (SD-124)") {
  PushChannels p;
  REQUIRE(applyPush(p, {{"ntfy", {{"enabled", true}, {"topic", "tank"}}}, {"telegram", {{"chatId", "42"}, {"token", kBotToken}}}}).empty());
  Notification n;
  n.level = "critical";
  n.title = say("notify.still", {{"label", say("watch.latch")}});
  n.lines = {say("watch.flow_ok", {{"flow", 2.5}})};
  long long id = 7;
  auto ds = deliveriesFor(n, p, "de", "Keller", id);
  REQUIRE(ds.size() == 1);  // Telegram is set but not enabled
  CHECK(ds[0].id == 7);
  CHECK(id == 8);
  CHECK(ds[0].channel == "ntfy");
  CHECK(ds[0].title == "Keller: Weiterhin offen: Sperre");
  CHECK(ds[0].body == "2,5 L/min");
  CHECK(ds[0].target["topic"] == "tank");
  CHECK(deliveriesFor(n, p, "en", "Keller", id, "telegram").empty());
}

TEST_CASE("Push over the API: the settings without secrets, kept apart from the configuration and its backup; a test message; a channel that fails is logged once (SD-124)") {
  sim::Options o;
  o.scenario = "demo";
  o.password = "demo-password";
  o.prefillHours = 0;
  o.startEpoch = 1798761600;
  sim::Simulation s(o);
  Robot& r = s.robot();
  CHECK(r.testPush("").status == 409);  // no channel yet
  REQUIRE(r.putPush({{"ntfy", {{"enabled", true}, {"topic", "tank"}, {"token", "tk_secret"}}}}).status == 200);
  CHECK(r.putPush({{"ntfy", {{"topic", "bad topic"}}}}).status == 422);
  const json shown = r.pushSettings();
  CHECK(shown["ntfy"]["topic"] == "tank");
  CHECK(shown["ntfy"]["tokenSet"] == true);
  CHECK(shown.dump().find("tk_secret") == std::string::npos);
  CHECK(r.configJson().dump().find("tk_secret") == std::string::npos);
  CHECK(r.configJson().dump().find("\"ntfy\"") == std::string::npos);
  CHECK(r.diagnostics().dump().find("tk_secret") == std::string::npos);
  (void)r.takeDeliveries();
  REQUIRE(r.testPush("").status == 200);
  auto ds = r.takeDeliveries();
  REQUIRE(ds.size() == 1);
  CHECK(ds[0].target["token"] == "tk_secret");  // only the platform gets it
  CHECK(ds[0].title.find("Test message") != std::string::npos);
  auto failures = [&] {
    int n = 0;
    for (const auto& e : r.events(0, std::numeric_limits<Epoch>::max(), "", 1000)) n += e["title"]["key"] == "ev.push.failed";
    return n;
  };
  r.deliveryResult("ntfy", false, "curl: (6) Could not resolve host: ntfy.sh");
  r.deliveryResult("ntfy", false, "curl: (6) Could not resolve host: ntfy.sh");
  CHECK(failures() == 1);
  CHECK(r.pushSettings()["status"]["ntfy"]["ok"] == false);
  r.deliveryResult("ntfy", true, "");
  r.deliveryResult("ntfy", false, "again");
  CHECK(failures() == 2);
  // Kept across a restart, from its own file
  s.control("reboot", json::object());
  CHECK(s.robot().pushSettings()["ntfy"]["enabled"] == true);
  CHECK(s.robot().pushSettings()["ntfy"]["tokenSet"] == true);
}

TEST_CASE("Simulator pushes: curl's config for ntfy, Telegram and e-mail, the secret only in it, quotes and line breaks escaped; mail headers in UTF-8 (SD-124)") {
  Delivery d{1, "ntfy", "critical", "Keller: \"Leck\"", "Wasser\nam Boden", {{"server", "https://ntfy.sh/"}, {"topic", "tank"}, {"token", "tk"}}};
  auto j = sim::curlJob(d, "/tmp/body", "/dev/null", 1798761600);
  CHECK(j.config.find("url = \"https://ntfy.sh\"\n") != std::string::npos);
  CHECK(j.config.find("header = \"Authorization: Bearer tk\"\n") != std::string::npos);
  CHECK(j.config.find("data-binary = \"@/tmp/body\"\n") != std::string::npos);
  CHECK(j.config.find("fail\n") != std::string::npos);
  const json body = json::parse(j.body);
  CHECK(body["topic"] == "tank");
  CHECK(body["priority"] == 5);
  CHECK(body["title"] == "Keller: \"Leck\"");
  CHECK(body["message"] == "Wasser\nam Boden");
  d.channel = "telegram";
  d.level = "warning";
  d.target = {{"chatId", "42"}, {"token", kBotToken}};
  j = sim::curlJob(d, "/tmp/body", "/dev/null", 1798761600);
  CHECK(j.config.find("url = \"https://api.telegram.org/bot" + kBotToken + "/sendMessage\"\n") != std::string::npos);
  CHECK(j.config.find("data-urlencode = \"chat_id=42\"\n") != std::string::npos);
  CHECK(j.config.find("data-urlencode = \"text@/tmp/body\"\n") != std::string::npos);
  CHECK(j.body == "Keller: \"Leck\"\nWasser\nam Boden");
  d.channel = "email";
  d.target = {{"server", "smtp://mail.example.org:587"}, {"from", "tank@example.org"}, {"to", "me@example.org"}, {"user", "tank"}, {"password", "p\"w"}};
  j = sim::curlJob(d, "/tmp/body", "/dev/null", 1798761600);
  CHECK(j.config.find("ssl-reqd\n") != std::string::npos);
  CHECK(j.config.find("mail-rcpt = \"me@example.org\"\n") != std::string::npos);
  CHECK(j.config.find("user = \"tank:p\\\"w\"\n") != std::string::npos);
  CHECK(j.body.rfind("From: tank@example.org\r\nTo: me@example.org\r\nSubject: =?UTF-8?B?", 0) == 0);
  CHECK(j.body.find("Date: Fri, 01 Jan 2027 00:00:00 +0000\r\n") != std::string::npos);
  CHECK(j.body.find("\r\n\r\nWasser\r\nam Boden\r\n") != std::string::npos);
  CHECK(sim::curlQuote("a\"b\\c\nd") == "\"a\\\"b\\\\c\\nd\"");
  CHECK(sim::encodedWord("é") == "=?UTF-8?B?w6k=?=");
}

#ifndef _WIN32
TEST_CASE("Simulator pushes: curl gets only the config file on its command line, which is gone afterwards; its exit and output are the result (SD-124)") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / ("doseph-push-test-" + std::to_string(::getpid()));
  fs::create_directories(dir);
  auto script = [&](const char* name, const std::string& body) {
    const fs::path p = dir / name;
    std::ofstream(p) << "#!/bin/sh\n" << body;
    fs::permissions(p, fs::perms::owner_all);
    return p.string();
  };
  const std::string ok = script("ok.sh", "[ \"$1\" = -K ] && cp \"$2\" '" + (dir / "seen.cfg").string() + "' && echo \"$#\" > '" + (dir / "argc").string() + "'\nexit 0\n");
  const std::string fail = script("fail.sh", "echo 'curl: (6) Could not resolve host: ntfy.sh'\nexit 6\n");
  auto run = [](const std::string& curl, const Delivery& d) {
    sim::PushSender p(curl);
    p.send(d);
    for (int i = 0; i < 500; ++i) {
      auto r = p.poll();
      if (!r.empty()) return r[0];
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return sim::PushSender::Result{"", false, "timeout"};
  };
  const Delivery d{99, "ntfy", "info", "T", "B", {{"server", "https://ntfy.sh"}, {"topic", "tank"}, {"token", "tk_secret"}}};
  const auto good = run(ok, d);
  CHECK(good.ok);
  std::ifstream seen(dir / "seen.cfg");
  const std::string cfg((std::istreambuf_iterator<char>(seen)), std::istreambuf_iterator<char>());
  CHECK(cfg.find("Authorization: Bearer tk_secret") != std::string::npos);
  std::ifstream argc(dir / "argc");
  std::string n;
  argc >> n;
  CHECK(n == "2");  // -K and the file: nothing else on the command line
  for (const auto& e : fs::directory_iterator(fs::temp_directory_path()))
    CHECK(e.path().filename().string().find("-99.cfg") == std::string::npos);
  const auto bad = run(fail, d);
  CHECK_FALSE(bad.ok);
  CHECK(bad.error == "curl: (6) Could not resolve host: ntfy.sh");
  const auto none = run((dir / "no-such-curl").string(), d);
  CHECK_FALSE(none.ok);
  CHECK(none.error == "curl could not be started");
  fs::remove_all(dir);
}
#endif
