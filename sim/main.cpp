// SPDX-License-Identifier: AGPL-3.0-or-later
// The simulator's host server: serves the web app, binds the core's REST
// API to HTTP and sends the live state as Server-Sent Events. On the device
// its own server takes this role.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#include <httplib.h>

#include "doseph/embedded.hpp"
#include "doseph/messages.hpp"
#include "folder_lock.hpp"
#include "mqtt_link.hpp"
#include "push.hpp"
#include "simulation.hpp"
#ifndef DP_EMBEDDED_WEB
#define DP_EMBEDDED_WEB 0
#endif
#if DP_EMBEDDED_WEB
#include "web_assets.hpp"
#endif

namespace {

std::atomic<bool> g_running{true};

void onSignal(int) { g_running = false; }

std::string tokenOf(const httplib::Request& req) {
  return doseph::sessionToken(req.get_header_value("Authorization"), req.get_header_value("Cookie"));
}

doseph::ApiRequest toApi(const httplib::Request& req) {
  doseph::ApiRequest r;
  r.method = req.method;
  r.path = req.path;
  for (const auto& [k, v] : req.params) r.query[k] = v;
  r.body = req.body;
  r.token = tokenOf(req);
  r.client = doseph::clientKey(req.remote_addr);  // the socket's peer; never X-Forwarded-For
  return r;
}

// Opens the page in the default browser: a download started by a double-click
// has no console to read the address from.
void openBrowser(const std::string& url) {
  std::thread([url] {
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
#if defined(_WIN32)
    const std::string cmd = "start \"\" \"" + url + "\"";
#elif defined(__APPLE__)
    const std::string cmd = "open \"" + url + "\" >/dev/null 2>&1";
#else
    const std::string cmd = "xdg-open \"" + url + "\" >/dev/null 2>&1";
#endif
    const int rc = std::system(cmd.c_str());
    (void)rc;
  }).detach();
}

void usage() {
  std::cout << "Doseph simulator " << doseph::embedded::kVersion << "\n"
            << "  --port N            HTTP port (8080; without it the next free one up to 8089)\n"
            << "  --host ADDR         address (127.0.0.1)\n"
            << "  --scenario NAME     demo | new (demo)\n"
            << "  --profile NAME      example device: home | pro (home)\n"
            << "  --data DIR          keep the data in this folder (else in memory only)\n"
            << "  --web DIR           built web app (web/dist; the download has it built in)\n"
            << "  --speed X           time-lapse factor (1)\n"
            << "  --password PW       password of the demo scenario\n"
            << "  --prefill H         hours of history the demo computes ahead (24)\n"
            << "  --allow-reset       change the scenario without signing in (automated tests only)\n";
}

}  // namespace

int main(int argc, char** argv) {
  sim::Options opts;
  int port = 8080;
  std::string host = "127.0.0.1", web;
  double speed = 1.0;
  bool allowReset = false, portGiven = false;
  // Started without arguments (a double-click on the download): the next free port, and the browser opens
  const bool plain = argc == 1;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        usage();
        std::exit(2);
      }
      return argv[++i];
    };
    try {
      if (a == "--port") {
        port = std::stoi(next());
        portGiven = true;
      }
      else if (a == "--host") host = next();
      else if (a == "--scenario") opts.scenario = next();
      else if (a == "--profile") opts.profile = next();
      else if (a == "--data") opts.dataDir = next();
      else if (a == "--web") web = next();
      else if (a == "--speed") speed = std::stod(next());
      else if (a == "--password") opts.password = next();
      else if (a == "--prefill") opts.prefillHours = std::stod(next());
      else if (a == "--allow-reset") allowReset = true;
      else if (a == "--help" || a == "-h") {
        usage();
        return 0;
      } else {
        usage();
        return 2;
      }
    } catch (const std::exception&) {
      usage();
      return 2;
    }
  }
  if ((opts.scenario != "demo" && opts.scenario != "new") || !sim::exampleProfile(opts.profile)) {
    usage();
    return 2;
  }

  // A second start on a data folder in use ends here, before it reads a file
  std::optional<sim::FolderLock> lock;
  if (!opts.dataDir.empty()) {
    lock.emplace(opts.dataDir);
    if (lock->state() == sim::FolderLock::State::Taken) {
      std::cerr << "Another simulator is using this data folder: " << opts.dataDir << "\n";
      return 1;
    }
    if (lock->state() == sim::FolderLock::State::None)
      std::cerr << "The data folder cannot be locked (" << lock->why() << "): a second simulator could use it too.\n";
  }

  std::cout << "Starting simulator (scenario " << opts.scenario << ") …" << std::flush;
  // The console shows setup codes; automated tests (--allow-reset) set the first password without one
  opts.showSetupCodes = !allowReset;
  // The code on the device's label (SD-149): made once per run, as the factory prints one per device;
  // it survives a factory reset within the run
  if (opts.showSetupCodes) {
    static constexpr char kAlphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
    std::uint8_t r[16];
    sim::randomBytes(r, sizeof r);
    for (int i = 0; i < 16; ++i) opts.labelCode += (i > 0 && i % 4 == 0 ? "-" : "") + std::string(1, kAlphabet[r[i] & 31]);
  }
  sim::Simulation simulation(opts);
  simulation.speed = std::clamp(speed, 0.0, sim::Simulation::kMaxSpeed);
  std::cout << " ready.\n";

  // A port in use: without --port the next nine are tried. A failed bind leaves a server unusable,
  // so each try has its own.
  std::unique_ptr<httplib::Server> server;
  for (int p = port; p < port + (portGiven ? 1 : 10) && p <= 65535; ++p) {
    auto s = std::make_unique<httplib::Server>();
    if (s->bind_to_port(host, p)) {
      server = std::move(s);
      port = p;
      break;
    }
  }
  if (!server) {
    std::cerr << "Port " << port << (portGiven ? "" : " and the next nine") << " not available\n";
    return 1;
  }
  httplib::Server& svr = *server;
  svr.set_default_headers({{"X-Content-Type-Options", "nosniff"},
                           {"X-Frame-Options", "DENY"},
                           {"Referrer-Policy", "no-referrer"},
                           {"Content-Security-Policy",
                            "default-src 'self'; img-src 'self' data:; style-src 'self' 'unsafe-inline'; "
                            "connect-src 'self'; frame-ancestors 'none'"}});
  svr.set_payload_max_length(1 << 20);
  // The origin is checked before any handler runs (DNS rebinding, CSRF).
  svr.set_pre_routing_handler([&](const httplib::Request& req, httplib::Response& res) {
    const bool ok = doseph::hostAllowed(req.get_header_value("Host"), host) &&
                    doseph::writeAllowed(req.method, req.get_header_value("Sec-Fetch-Site"), req.get_header_value("Origin"),
                                         req.get_header_value("Host"));
    if (ok) return httplib::Server::HandlerResponse::Unhandled;
    res.status = 403;
    res.set_content(doseph::json{{"error", doseph::say("api.origin")}}.dump(), "application/json");
    return httplib::Server::HandlerResponse::Handled;
  });

  auto apiHandler = [&](const httplib::Request& req, httplib::Response& res) {
    doseph::ApiResponse r;
    {
      std::lock_guard<std::recursive_mutex> l(simulation.mutex());
      r = simulation.api().handle(toApi(req));
    }
    res.status = r.status;
    for (const auto& [k, v] : r.headers) res.set_header(k, v);
    res.set_header("Cache-Control", "no-store");
    res.set_content(r.body, r.contentType);
  };

  // The simulator's controls (here only, never on the device)
  svr.Get("/api/v1/sim", [&](const httplib::Request& req, httplib::Response& res) {
    std::lock_guard<std::recursive_mutex> l(simulation.mutex());
    if (!simulation.api().authorized(toApi(req))) {
      res.status = 401;
      res.set_content(doseph::json{{"error", doseph::say("auth.required")}}.dump(), "application/json");
      return;
    }
    res.set_content(simulation.simState().dump(), "application/json");
  });
  svr.Post(R"(/api/v1/sim/(\w+))", [&](const httplib::Request& req, httplib::Response& res) {
    std::lock_guard<std::recursive_mutex> l(simulation.mutex());
    std::string action = req.matches[1];
    // A scenario change or a reset at the device without signing in only with --allow-reset
    // (Playwright); otherwise the console stands for the device (SD-139)
    const bool open = (action == "scenario" || action == "atDevice") && allowReset;
    const auto role = simulation.api().access(toApi(req));
    if (!open && !role) {
      res.status = 401;
      res.set_content(doseph::json{{"error", doseph::say("auth.required")}}.dump(), "application/json");
      return;
    }
    // A reset at the device stands for someone at it: the owner's alone here, never a token's (SD-139)
    const bool refused = !open && (action == "atDevice" ? *role != doseph::Role::Owner : !doseph::permitted(*role, "POST", {"sim", action}));
    if (refused) {  // as any other change (SD-129)
      res.status = 403;
      res.set_content(doseph::json{{"error", doseph::say("auth.role", {{"role", doseph::say(std::string("auth.role.") + doseph::roleName(*role))}})}}.dump(),
                      "application/json");
      return;
    }
    auto body = doseph::json::parse(req.body.empty() ? "{}" : req.body, nullptr, false);
    if (!body.is_object()) body = doseph::json::object();
    auto out = simulation.control(action, body);
    res.status = out.contains("error") ? 422 : 200;
    res.set_content(out.dump(), "application/json");
  });

  // Live updates: Server-Sent Events, the state once a second
  svr.Get("/api/v1/events/stream", [&](const httplib::Request& req, httplib::Response& res) {
    {
      std::lock_guard<std::recursive_mutex> l(simulation.mutex());
      if (!simulation.api().authorized(toApi(req))) {
        res.status = 401;
        return;
      }
    }
    std::string token = tokenOf(req);
    res.set_header("Cache-Control", "no-store");
    res.set_chunked_content_provider("text/event-stream", [&simulation, token](size_t, httplib::DataSink& sink) {
      std::string msg;
      {
        std::lock_guard<std::recursive_mutex> l(simulation.mutex());
        doseph::ApiRequest r;
        r.token = token;
        if (!simulation.api().authorized(r)) return false;
        doseph::json st = simulation.robot().state();
        st["sim"] = {{"speed", simulation.speed}, {"scenario", simulation.scenario()}};
        msg = "event: state\ndata: " + st.dump() + "\n\n";
      }
      if (!sink.write(msg.data(), msg.size())) return false;
      for (int i = 0; i < 10 && g_running; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      return g_running.load();
    });
  });

  svr.Get(R"(/api/v1/.*)", apiHandler);
  svr.Post(R"(/api/v1/.*)", apiHandler);
  svr.Put(R"(/api/v1/.*)", apiHandler);
  svr.Patch(R"(/api/v1/.*)", apiHandler);
  svr.Delete(R"(/api/v1/.*)", apiHandler);

  // The web app from its folder (development) or built in (the download), falling back to
  // index.html (a single-page app)
  if (web.empty() && !DP_EMBEDDED_WEB) web = "web/dist";
  std::error_code webErr;
  const bool haveDir = !web.empty() && std::filesystem::exists(std::filesystem::path(web) / "index.html", webErr);
  const bool haveWeb = haveDir || DP_EMBEDDED_WEB;
  if (haveDir) svr.set_mount_point("/", web);
  svr.Get(R"(/(?!api/).*)", [&](const httplib::Request& req, httplib::Response& res) {
#if DP_EMBEDDED_WEB
    if (!haveDir) {
      const sim::WebAsset* hit = nullptr;
      const sim::WebAsset* index = nullptr;
      for (std::size_t i = 0; i < sim::kWebAssetCount; ++i) {
        if (req.path == sim::kWebAssets[i].path) hit = &sim::kWebAssets[i];
        if (std::string(sim::kWebAssets[i].path) == "/index.html") index = &sim::kWebAssets[i];
      }
      if (!hit) hit = index;
      if (hit) {
        res.set_header("Content-Encoding", "gzip");
        res.set_header("Cache-Control", hit == index ? "no-cache" : "public, max-age=31536000, immutable");
        res.set_content(std::string(reinterpret_cast<const char*>(hit->data), hit->size), hit->type);
        return;
      }
    }
#else
    (void)req;
#endif
    if (!haveDir) {
      res.set_content("Web app not built: cd web && npm ci && npm run build", "text/plain; charset=utf-8");
      return;
    }
    std::ifstream f(std::filesystem::path(web) / "index.html");
    std::stringstream ss;
    ss << f.rdbuf();
    res.set_content(ss.str(), "text/html; charset=utf-8");
  });

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  // Pushes go out through the system's curl, on a thread of their own (SD-124); DP_CURL names another program
  const char* curlEnv = std::getenv("DP_CURL");
  sim::PushSender pushes(curlEnv ? curlEnv : "");

  // MQTT to a broker in the home network, without TLS, on a thread of its own (SD-130)
  sim::MqttLink mqtt(simulation);

  // Requests to the coupled device (SD-131) on a thread of their own: plain HTTP in the home
  // network, so the robot's loop never waits on the other device
  std::thread peers([&] {
    while (g_running) {
      std::vector<doseph::PeerRequest> reqs;
      {
        std::lock_guard<std::recursive_mutex> l(simulation.mutex());
        reqs = simulation.robot().takePeerRequests();
      }
      for (const auto& r : reqs) {
        httplib::Client cli("http://" + r.address);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(5);
        httplib::Headers headers;
        if (!r.token.empty()) headers.emplace("Authorization", "Bearer " + r.token);
        auto res = r.method == "DELETE" ? cli.Delete(r.path, headers) : cli.Post(r.path, headers, r.body.dump(), "application/json");
        const int status = res ? res->status : 0;
        const doseph::json body = res ? doseph::json::parse(res->body, nullptr, false) : doseph::json(nullptr);
        std::lock_guard<std::recursive_mutex> l(simulation.mutex());
        simulation.robot().peerResponse(r.id, status, body);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });

  // The tick: every 100 ms of real time, simulated time × time-lapse
  std::thread loop([&] {
    while (g_running) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      std::lock_guard<std::recursive_mutex> l(simulation.mutex());
      simulation.step(static_cast<doseph::Ms>(100 * simulation.speed));
      // A setup code armed for a first password (an access or a factory reset), shown here to whoever
      // sits at the simulator, as the device shows its own (SD-129, SD-139)
      if (const std::string code = simulation.robot().takeCodeToShow(); !code.empty())
        std::cout << "Setup code for the first password: " << code << "\n" << std::flush;
      for (auto& d : simulation.robot().takeDeliveries()) pushes.send(std::move(d));
      for (const auto& r : pushes.poll()) simulation.robot().deliveryResult(r.channel, r.ok, r.error);
    }
    svr.stop();
  });

  const std::string url = "http://" + (host == "0.0.0.0" ? std::string("127.0.0.1") : host) + ":" + std::to_string(port);
  std::cout << "Doseph simulator " << doseph::embedded::kVersion << " running: " << url << "\n";
  {
    std::lock_guard<std::recursive_mutex> l(simulation.mutex());
    if (simulation.scenario() == "demo" && !simulation.demoPassword().empty())
      std::cout << "Demo password: " << simulation.demoPassword() << " (simulator only)\n";
    // The first password needs a setup code shown to the owner alone: here the console
    if (const std::string code = simulation.robot().takeCodeToShow(); !code.empty())
      std::cout << "Setup code for the first password: " << code << "\n";
    if (!opts.labelCode.empty())
      std::cout << "Code on the device label: " << opts.labelCode << " (type \"key\" and Enter to press the function key)\n";
  }
  if (!haveWeb) std::cout << "Note: no web app found at " << web << "\n";
  std::cout << "To stop: press Ctrl+C or close this window.\n"
            << "Forgot the password? Type \"reset access\" and Enter here, as at the device; \"reset factory\" starts it as new.\n"
            << std::flush;
  // The console stands for the device: a reset asked for here is one at the device (SD-139). Without
  // a console (a closed input) the reader just ends.
  std::thread([&simulation] {
    std::string line;
    while (g_running && std::getline(std::cin, line)) {
      const bool access = line == "reset access", factory = line == "reset factory", key = line == "key";
      if (!access && !factory && !key) continue;
      std::lock_guard<std::recursive_mutex> l(simulation.mutex());
      if (key) {
        simulation.control("keyPress", doseph::json::object());
        std::cout << "Function key pressed: for 5 min the device takes the code on its label.\n" << std::flush;
        continue;
      }
      simulation.control("atDevice", {{"reset", access ? "access" : "factory"}});
      std::cout << (access ? "Access reset: set a new password in the browser.\n" : "Factory reset: the simulator starts as new.\n") << std::flush;
    }
  }).detach();
  if (plain && haveWeb) openBrowser(url);
  svr.listen_after_bind();
  g_running = false;
  loop.join();
  peers.join();
  return 0;
}
