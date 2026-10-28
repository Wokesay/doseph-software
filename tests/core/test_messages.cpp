// SPDX-License-Identifier: AGPL-3.0-or-later
// Texts the robot sends: key, arguments and English text.
#include <doctest/doctest.h>

#include "doseph/messages.hpp"

using namespace doseph;

TEST_CASE("Messages: numbers with and without decimals, a missing value is never 0") {
  CHECK(render("{a:2} {b} {c:0}", {{"a", 5.8}, {"b", 1.5}, {"c", 12.4}}) == "5.80 1.5 12");
  CHECK(render("{n} rounds", {{"n", 3}}) == "3 rounds");
  CHECK(render("{a:1} and {b}", {{"a", nullptr}}) == "– and –");  // R5
  CHECK(render("{a:2}", {{"a", kNaN}}) == "–");
  CHECK(render("{name}", {{"name", "Part A"}}) == "Part A");
  // Ties round away from zero, as in the web app
  CHECK(render("{t:1} {u:2} {z:1} {s:1} {h:0}", {{"t", 20.25}, {"u", -0.125}, {"z", -0.04}, {"s", 20.65}, {"h", 12.5}}) ==
        "20.3 -0.13 0.0 20.6 13");
  CHECK(render("{v} {w} {x}", {{"v", 1e20}, {"w", true}, {"x", json::array({1})}}) == "100000000000000000000 – 1");
}

TEST_CASE("Messages: a cycle line with a missing value shows a dash (R5)") {
  Msg m = say("cycle.hold", {{"ph", numOrNull(kNaN)}, {"ec", 1.6}});
  CHECK(m.args["ph"].is_null());
  CHECK(m.text == "On target: pH –, EC 1.60 mS/cm");
}

TEST_CASE("Messages: braces that are no placeholder stay as they are") {
  CHECK(render("{ x} {x {} {x:a}", {{"x", 1}}) == "{ x} {x {} {x:a}");
  CHECK(render("no args", json::object()) == "no args");
}

TEST_CASE("Messages: a message inside an argument gives its text") {
  Msg inner = say("truth.uncalibrated");
  Msg m = say("cycle.wait.ec", {{"reason", inner}});
  CHECK(m.key == "cycle.wait.ec");
  CHECK(m.text == "Waiting: EC – Not calibrated");
  CHECK(m.args["reason"]["key"] == "truth.uncalibrated");
}

TEST_CASE("Messages: an empty message inside an argument counts as missing, as in the web app (R5)") {
  CHECK(render("Reason: {reason}", {{"reason", Msg{}}}) == "Reason: –");
}

TEST_CASE("Messages: key and arguments travel with the English text") {
  Msg m = say("watch.flow_ok", {{"flow", 12.0}});
  CHECK(m.text == "12.0 L/min");
  json j = m;
  CHECK(j["key"] == "watch.flow_ok");
  CHECK(j["args"]["flow"] == 12.0);
  CHECK(knownMessage("watch.flow_ok"));
}

TEST_CASE("Messages: an unknown key shows the key, never an empty text") {
  CHECK_FALSE(knownMessage("no.such.key"));
  CHECK(say("no.such.key").text == "no.such.key");
}

TEST_CASE("Messages in German: the web app's templates, a decimal comma, nested messages in German too; English and unknown keys keep their text (#10, step 5)") {
  const Msg flow = say("watch.flow_ok", {{"flow", 12.5}});
  CHECK(textIn(flow, "de") == "12,5 L/min");
  CHECK(textIn(flow, "en") == "12.5 L/min");
  const Msg still = say("notify.still", {{"label", say("watch.latch")}});
  CHECK(textIn(still, "de") == "Weiterhin offen: Sperre");
  CHECK(textIn(still, "en") == "Still open: Lock");
  CHECK(textIn(say("no.such.key"), "de") == "no.such.key");
  CHECK(textIn(say("watch.flow_ok", {{"flow", nullptr}}), "de") == "– L/min");
}
