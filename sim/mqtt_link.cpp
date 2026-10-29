// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mqtt_link.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <optional>

#include "doseph/messages.hpp"
#include "doseph/mqtt.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace sim {

using doseph::json;

namespace {

#ifdef _WIN32
using Socket = SOCKET;
const Socket kNone = INVALID_SOCKET;
void closeSocket(Socket s) { closesocket(s); }
void setBlocking(Socket s, bool on) {
  u_long nb = on ? 0 : 1;
  ioctlsocket(s, FIONBIO, &nb);
}
struct WinSock {
  WinSock() {
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
  }
  ~WinSock() { WSACleanup(); }
};
#else
using Socket = int;
const Socket kNone = -1;
void closeSocket(Socket s) { ::close(s); }
void setBlocking(Socket s, bool on) {
  const int f = fcntl(s, F_GETFL, 0);
  fcntl(s, F_SETFL, on ? (f & ~O_NONBLOCK) : (f | O_NONBLOCK));
}
#endif

using Clock = std::chrono::steady_clock;
long long msSince(Clock::time_point t) { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count(); }

bool waitFor(Socket s, bool write, int timeoutMs) {
  fd_set set;
  FD_ZERO(&set);
  FD_SET(s, &set);
  timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
  const int n = select(static_cast<int>(s + 1), write ? nullptr : &set, write ? &set : nullptr, nullptr, &tv);
  return n > 0;
}

// A TCP connection within the time given, or none
Socket connectTo(const std::string& host, int port, int timeoutMs) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return kNone;
  Socket out = kNone;
  for (addrinfo* a = res; a && out == kNone; a = a->ai_next) {
    Socket s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (s == kNone) continue;
    setBlocking(s, false);
    connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen));
    int err = 1;
    socklen_t len = sizeof err;
    if (waitFor(s, true, timeoutMs) && getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) == 0 && err == 0) {
      setBlocking(s, true);
      out = s;
    } else {
      closeSocket(s);
    }
  }
  freeaddrinfo(res);
#ifdef SO_NOSIGPIPE
  if (out != kNone) {
    int one = 1;
    setsockopt(out, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
  }
#endif
  return out;
}

bool sendAll(Socket s, const std::string& data) {
#ifdef MSG_NOSIGNAL
  const int flags = MSG_NOSIGNAL;
#else
  const int flags = 0;
#endif
  size_t at = 0;
  while (at < data.size()) {
    const auto n = send(s, data.data() + at, static_cast<int>(data.size() - at), flags);
    if (n <= 0) return false;
    at += static_cast<size_t>(n);
  }
  return true;
}

// What arrived within the time given; false once the connection is closed or broken
bool receive(Socket s, int timeoutMs, std::string& out) {
  out.clear();
  if (!waitFor(s, false, timeoutMs)) return true;
  char buf[4096];
  const auto n = recv(s, buf, sizeof buf, 0);
  if (n <= 0) return false;
  out.assign(buf, static_cast<size_t>(n));
  return true;
}

}  // namespace

MqttLink::MqttLink(Simulation& s) : sim_(s), worker_([this] { run(); }) {}

MqttLink::~MqttLink() {
  stop_ = true;
  if (worker_.joinable()) worker_.join();
}

void MqttLink::run() {
#ifdef _WIN32
  WinSock winsock;
#endif
  Socket sock = kNone;
  doseph::MqttReader reader;
  std::string availability, lastState;
  int linkRev = -1, failedRev = -1, cfgRev = -1;
  doseph::Robot* linkRobot = nullptr;  // a new scenario is a new robot: it connects anew
  Clock::time_point retryAt = Clock::now(), lastPublish = Clock::now(), lastSent = Clock::now(), lastHeard = Clock::now();
  auto report = [&](bool connected, const doseph::Msg& m) {
    std::lock_guard<std::recursive_mutex> l(sim_.mutex());
    if (&sim_.robot() == linkRobot || !connected) sim_.robot().setMqttStatus(connected, m);
  };
  auto drop = [&](bool graceful) {
    if (sock == kNone) return;
    if (graceful) {
      sendAll(sock, doseph::mqttPublishPacket(availability, "offline", true));
      sendAll(sock, doseph::mqttDisconnectPacket());
    }
    closeSocket(sock);
    sock = kNone;
    reader = doseph::MqttReader{};
  };
  // The topics roles read (SD-132): subscribed once each while connected, new ones as they come
  std::vector<std::string> subscribed;
  std::uint16_t packetId = 0;
  auto subscribeValues = [&] {
    std::vector<std::string> topics;
    {
      std::lock_guard<std::recursive_mutex> l(sim_.mutex());
      topics = sim_.robot().mqttValueTopics();
    }
    std::vector<std::string> fresh;
    for (const auto& t : topics)
      if (std::find(subscribed.begin(), subscribed.end(), t) == subscribed.end()) fresh.push_back(t);
    if (fresh.empty()) return;
    packetId = static_cast<std::uint16_t>(packetId % 65535 + 1);
    if (sendAll(sock, doseph::mqttSubscribePacket(packetId, fresh))) subscribed.insert(subscribed.end(), fresh.begin(), fresh.end());
  };
  auto publish = [&](const doseph::MqttMessage& m) {
    lastSent = Clock::now();
    return sendAll(sock, doseph::mqttPublishPacket(m.topic, m.payload, m.retain));
  };
  auto lost = [&](const doseph::Msg& why) {
    drop(false);
    report(false, why);
    retryAt = Clock::now() + std::chrono::seconds(10);
    failedRev = linkRev;
  };

  while (!stop_) {
    doseph::MqttSettings m;
    int rev = 0;
    std::string id;
    doseph::Robot* robot = nullptr;
    {
      std::lock_guard<std::recursive_mutex> l(sim_.mutex());
      robot = &sim_.robot();
      m = robot->mqtt();
      rev = robot->mqttRevision();
      id = robot->deviceId();
    }
    // A change of the settings, or another robot, ends the connection; it starts anew
    if (sock != kNone && (rev != linkRev || robot != linkRobot)) drop(true);
    if (!m.enabled) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }
    if (sock == kNone) {
      if (m.tls) {
        if (failedRev != rev || robot != linkRobot) {
          linkRobot = robot;
          linkRev = failedRev = rev;
          report(false, doseph::say("mqtt.sim_tls"));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        continue;
      }
      if (failedRev == rev && robot == linkRobot && Clock::now() < retryAt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        continue;
      }
      linkRobot = robot;
      linkRev = rev;
      availability = doseph::mqttAvailability(id);
      sock = connectTo(m.host, m.port, 5000);
      if (sock == kNone) {
        lost(doseph::say("mqtt.unreachable", {{"host", m.host}, {"port", m.port}}));
        continue;
      }
      sendAll(sock, doseph::mqttConnectPacket("doseph-" + id, m.user, m.password, availability, "offline", kKeepAliveS));
      std::optional<int> code;
      const auto asked = Clock::now();
      std::string in;
      while (!code && msSince(asked) < 5000 && !stop_) {
        if (!receive(sock, 200, in)) break;
        reader.feed(in);
        if (auto p = reader.next(); p && p->type == 2) code = p->returnCode;
      }
      if (!code) {
        lost(doseph::say("mqtt.lost"));
        continue;
      }
      if (*code != 0) {
        lost(doseph::say("mqtt.rejected", {{"code", *code}}));
        continue;
      }
      report(true, {});
      publish({availability, "online", true});
      std::vector<doseph::MqttMessage> discovery;
      doseph::MqttMessage state;
      {
        std::lock_guard<std::recursive_mutex> l(sim_.mutex());
        discovery = sim_.robot().mqttDiscoveryMessages();
        state = sim_.robot().mqttStateMessage();
        cfgRev = sim_.robot().config().revision;
      }
      for (const auto& d : discovery) publish(d);
      publish(state);
      lastState = state.payload;
      lastPublish = lastHeard = Clock::now();
      // Commands need TLS (PD-461), which the simulator has not: it subscribes to no command topic.
      // Values roles read come also without TLS (SD-130, SD-132).
      subscribed.clear();
      subscribeValues();
      continue;
    }
    std::string in;
    if (!receive(sock, 200, in)) {
      lost(doseph::say("mqtt.lost"));
      continue;
    }
    if (!in.empty()) {
      lastHeard = Clock::now();
      reader.feed(in);
      while (auto p = reader.next()) {
        if (p->type != 3) continue;
        std::lock_guard<std::recursive_mutex> l(sim_.mutex());
        sim_.robot().mqttValue(p->topic, p->payload);
      }
      if (reader.broken()) {
        lost(doseph::say("mqtt.lost"));
        continue;
      }
    }
    // The state every 2 s when it changed, once a minute anyway; the discovery again when the
    // configuration changed (a canister or a recipe more or less)
    if (msSince(lastPublish) >= 2000) {
      lastPublish = Clock::now();
      subscribeValues();
      std::vector<doseph::MqttMessage> discovery;
      doseph::MqttMessage state;
      {
        std::lock_guard<std::recursive_mutex> l(sim_.mutex());
        if (sim_.robot().config().revision != cfgRev) {
          discovery = sim_.robot().mqttDiscoveryMessages();
          cfgRev = sim_.robot().config().revision;
        }
        state = sim_.robot().mqttStateMessage();
      }
      bool ok = true;
      for (const auto& d : discovery) ok = ok && publish(d);
      if (ok && (state.payload != lastState || msSince(lastSent) >= 60000)) {
        ok = publish(state);
        lastState = state.payload;
      }
      if (!ok) {
        lost(doseph::say("mqtt.lost"));
        continue;
      }
    }
    // Alive both ways: a ping when nothing went out for half the keep-alive; a broker silent for
    // one and a half is gone
    if (msSince(lastSent) >= kKeepAliveS * 500) {
      lastSent = Clock::now();
      if (!sendAll(sock, doseph::mqttPingPacket())) {
        lost(doseph::say("mqtt.lost"));
        continue;
      }
    }
    if (msSince(lastHeard) >= kKeepAliveS * 1500) lost(doseph::say("mqtt.lost"));
  }
  drop(true);
}

}  // namespace sim
