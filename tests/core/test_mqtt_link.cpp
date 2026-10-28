// SPDX-License-Identifier: AGPL-3.0-or-later
// The simulator's MQTT connection against a broker of the test's own on the loopback (SD-130).
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "doseph/mqtt.hpp"
#include "mqtt_link.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace doseph;

namespace {

#ifdef _WIN32
using Socket = SOCKET;
void closeSocket(Socket s) { closesocket(s); }
struct WinSock {
  WinSock() {
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
  }
  ~WinSock() { WSACleanup(); }
};
#else
using Socket = int;
void closeSocket(Socket s) { ::close(s); }
#endif

// Takes one client, answers its CONNECT with `code` and keeps what it publishes
struct Broker {
  explicit Broker(int code) : code_(code) {
    listener_ = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    bind(listener_, reinterpret_cast<sockaddr*>(&a), sizeof a);
    socklen_t len = sizeof a;
    getsockname(listener_, reinterpret_cast<sockaddr*>(&a), &len);
    port = ntohs(a.sin_port);
    listen(listener_, 1);
    worker_ = std::thread([this] { run(); });
  }
  ~Broker() {
    stop_ = true;
    worker_.join();
    closeSocket(listener_);
  }
  std::vector<std::pair<std::string, std::string>> published() {
    std::lock_guard<std::mutex> l(mtx_);
    return pub_;
  }
  bool has(const std::string& topic, const std::string& payload = "") {
    for (const auto& [t, p] : published())
      if (t == topic && (payload.empty() || p == payload)) return true;
    return false;
  }
  int port = 0;
  std::atomic<int> connects{0};

 private:
  void run() {
    Socket client = static_cast<Socket>(-1);
    MqttReader reader;
    while (!stop_) {
      fd_set set;
      FD_ZERO(&set);
      const Socket watch = client == static_cast<Socket>(-1) ? listener_ : client;
      FD_SET(watch, &set);
      timeval tv{0, 50000};
      if (select(static_cast<int>(watch + 1), &set, nullptr, nullptr, &tv) <= 0) continue;
      if (watch == listener_) {
        client = accept(listener_, nullptr, nullptr);
        continue;
      }
      char buf[4096];
      const auto n = recv(client, buf, sizeof buf, 0);
      if (n <= 0) {
        closeSocket(client);
        client = static_cast<Socket>(-1);
        reader = MqttReader{};
        continue;
      }
      reader.feed(std::string(buf, static_cast<size_t>(n)));
      while (auto p = reader.next()) {
        if (p->type == 1) {
          connects++;
          const char ack[] = {0x20, 0x02, 0x00, static_cast<char>(code_)};
          send(client, ack, 4, 0);
        } else if (p->type == 3) {
          std::lock_guard<std::mutex> l(mtx_);
          pub_.emplace_back(p->topic, p->payload);
        }
      }
    }
    if (client != static_cast<Socket>(-1)) closeSocket(client);
  }
  int code_;
  Socket listener_;
  std::atomic<bool> stop_{false};
  std::mutex mtx_;
  std::vector<std::pair<std::string, std::string>> pub_;
  std::thread worker_;
};

sim::Options demo() {
  sim::Options o;
  o.scenario = "demo";
  o.password = "demo-password";
  o.prefillHours = 0;
  return o;
}

template <class F>
bool within(int ms, F ok) {
  for (int i = 0; i < ms / 20; ++i) {
    if (ok()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return ok();
}

json status(sim::Simulation& s) {
  std::lock_guard<std::recursive_mutex> l(s.mutex());
  return s.robot().mqttSettings()["status"];
}

}  // namespace

TEST_CASE("Simulator MQTT: it connects with a last will, says it is online, sends the discovery and the state, and says it is offline when it ends (SD-130)") {
#ifdef _WIN32
  WinSock winsock;
#endif
  sim::Simulation s(demo());
  s.step(2 * kMinute);
  Broker broker(0);
  std::string id;
  {
    std::lock_guard<std::recursive_mutex> l(s.mutex());
    id = s.robot().deviceId();
    REQUIRE(s.robot().putMqtt({{"enabled", true}, {"host", "127.0.0.1"}, {"port", broker.port}}).status == 200);
  }
  const std::string base = "doseph/" + id;
  {
    sim::MqttLink link(s);
    CHECK(within(5000, [&] { return broker.has(base + "/state"); }));
    CHECK(broker.has(base + "/availability", "online"));
    CHECK(broker.has("homeassistant/sensor/doseph_" + id + "/ph/config"));
    CHECK(status(s)["connected"] == true);
  }
  CHECK(within(2000, [&] { return broker.has(base + "/availability", "offline"); }));
}

TEST_CASE("Simulator MQTT: a broker that refuses, or TLS the simulator has not, is said in the settings (SD-130)") {
#ifdef _WIN32
  WinSock winsock;
#endif
  sim::Simulation s(demo());
  Broker broker(5);  // not authorised
  {
    std::lock_guard<std::recursive_mutex> l(s.mutex());
    REQUIRE(s.robot().putMqtt({{"enabled", true}, {"host", "127.0.0.1"}, {"port", broker.port}, {"user", "doseph"}, {"password", "wrong"}}).status == 200);
  }
  sim::MqttLink link(s);
  CHECK(within(5000, [&] { return status(s)["error"].is_object(); }));
  CHECK(status(s)["error"]["key"] == "mqtt.rejected");
  CHECK(status(s)["connected"] == false);
  {
    std::lock_guard<std::recursive_mutex> l(s.mutex());
    REQUIRE(s.robot().putMqtt({{"tls", true}}).status == 200);
  }
  CHECK(within(3000, [&] { return status(s)["error"].is_object() && status(s)["error"]["key"] == "mqtt.sim_tls"; }));
}
