// SPDX-License-Identifier: AGPL-3.0-or-later
#include "push.hpp"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <utility>

#ifdef _WIN32
#include <process.h>
#define popen _popen
#define pclose _pclose
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace sim {

using doseph::json;

std::string curlQuote(std::string_view s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '\\' || c == '"') out += '\\';
    if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else out += c;
  }
  return out + "\"";
}

namespace {

std::string base64(std::string_view in) {
  static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    const unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8) | static_cast<unsigned char>(in[i + 2]);
    out += {k[v >> 18], k[(v >> 12) & 63], k[(v >> 6) & 63], k[v & 63]};
  }
  if (i + 1 == in.size()) {
    const unsigned v = static_cast<unsigned char>(in[i]) << 16;
    out += {k[v >> 18], k[(v >> 12) & 63], '=', '='};
  } else if (i + 2 == in.size()) {
    const unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8);
    out += {k[v >> 18], k[(v >> 12) & 63], k[(v >> 6) & 63], '='};
  }
  return out;
}

// A date for a mail header, in UTC
std::string mailDate(doseph::Epoch now) {
  const std::time_t t = static_cast<std::time_t>(now);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[64];
  std::strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S +0000", &tm);
  return buf;
}

std::string str(const json& j, const char* key) { return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : ""; }

// A file only the user can read, for the config with its secret
bool writePrivate(const std::filesystem::path& p, const std::string& data) {
#ifdef _WIN32
  std::ofstream f(p, std::ios::binary | std::ios::trunc);  // the user's own temporary folder
  f << data;
  return static_cast<bool>(f);
#else
  const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0) return false;
  const bool ok = ::write(fd, data.data(), data.size()) == static_cast<ssize_t>(data.size());
  ::close(fd);
  return ok;
#endif
}

}  // namespace

std::string encodedWord(std::string_view s) { return "=?UTF-8?B?" + base64(s) + "?="; }

CurlJob curlJob(const doseph::Delivery& d, const std::string& bodyPath, const std::string& nullDevice, doseph::Epoch now) {
  const json& t = d.target;
  std::string c = "silent\nshow-error\nfail\nmax-time = 20\nstderr = \"-\"\n";
  c += "output = " + curlQuote(nullDevice) + "\n";
  CurlJob job;
  if (d.channel == "ntfy") {
    std::string server = str(t, "server");
    while (!server.empty() && server.back() == '/') server.pop_back();
    const int priority = d.level == "critical" ? 5 : d.level == "warning" ? 4 : 3;
    const char* tag = d.level == "critical" ? "rotating_light" : d.level == "warning" ? "warning" : "information_source";
    c += "url = " + curlQuote(server) + "\n";
    c += "header = " + curlQuote("Content-Type: application/json") + "\n";
    if (!str(t, "token").empty()) c += "header = " + curlQuote("Authorization: Bearer " + str(t, "token")) + "\n";
    c += "data-binary = " + curlQuote("@" + bodyPath) + "\n";
    job.body = json{{"topic", str(t, "topic")}, {"title", d.title}, {"message", d.body}, {"priority", priority}, {"tags", {tag}}}.dump();
  } else if (d.channel == "telegram") {
    c += "url = " + curlQuote("https://api.telegram.org/bot" + str(t, "token") + "/sendMessage") + "\n";
    c += "data-urlencode = " + curlQuote("chat_id=" + str(t, "chatId")) + "\n";
    c += "data-urlencode = " + curlQuote("text@" + bodyPath) + "\n";
    job.body = d.title + "\n" + d.body;
  } else if (d.channel == "email") {
    c += "url = " + curlQuote(str(t, "server")) + "\n";
    c += "ssl-reqd\n";  // smtp:// only with STARTTLS
    c += "mail-from = " + curlQuote(str(t, "from")) + "\n";
    c += "mail-rcpt = " + curlQuote(str(t, "to")) + "\n";
    if (!str(t, "user").empty()) c += "user = " + curlQuote(str(t, "user") + ":" + str(t, "password")) + "\n";
    c += "upload-file = " + curlQuote(bodyPath) + "\n";
    std::string body = d.body;
    std::string crlf;
    for (char ch : body) crlf += ch == '\n' ? std::string("\r\n") : std::string(1, ch);
    job.body = "From: " + str(t, "from") + "\r\nTo: " + str(t, "to") + "\r\nSubject: " + encodedWord(d.title) + "\r\nDate: " + mailDate(now) +
               "\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; charset=UTF-8\r\nContent-Transfer-Encoding: 8bit\r\n\r\n" + crlf + "\r\n";
  }
  job.config = std::move(c);
  return job;
}

PushSender::PushSender(std::string curl) : curl_(curl.empty() ? "curl" : std::move(curl)), worker_([this] { run(); }) {}

PushSender::~PushSender() {
  {
    std::lock_guard<std::mutex> l(mtx_);
    stop_ = true;
  }
  cv_.notify_all();
  worker_.join();
}

void PushSender::send(doseph::Delivery d) {
  {
    std::lock_guard<std::mutex> l(mtx_);
    queue_.push_back(std::move(d));
  }
  cv_.notify_all();
}

std::vector<PushSender::Result> PushSender::poll() {
  std::lock_guard<std::mutex> l(mtx_);
  std::vector<Result> out;
  out.swap(results_);
  return out;
}

void PushSender::run() {
  for (;;) {
    doseph::Delivery d;
    {
      std::unique_lock<std::mutex> l(mtx_);
      cv_.wait(l, [&] { return stop_ || !queue_.empty(); });
      if (stop_) return;
      d = std::move(queue_.front());
      queue_.pop_front();
    }
    Result r = deliver(d);
    std::lock_guard<std::mutex> l(mtx_);
    results_.push_back(std::move(r));
  }
}

PushSender::Result PushSender::deliver(const doseph::Delivery& d) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path dir = fs::temp_directory_path(ec);
  if (ec) return {d.channel, false, "no temporary folder"};
#ifdef _WIN32
  const std::string pid = std::to_string(_getpid());
  const std::string nul = "NUL";
#else
  const std::string pid = std::to_string(::getpid());
  const std::string nul = "/dev/null";
#endif
  const fs::path cfg = dir / ("doseph-push-" + pid + "-" + std::to_string(d.id) + ".cfg");
  const fs::path body = dir / ("doseph-push-" + pid + "-" + std::to_string(d.id) + ".txt");
  const CurlJob job = curlJob(d, body.string(), nul, static_cast<doseph::Epoch>(std::time(nullptr)));
  if (!writePrivate(cfg, job.config) || !writePrivate(body, job.body)) {
    fs::remove(cfg, ec);
    fs::remove(body, ec);
    return {d.channel, false, "could not write the request"};
  }
  // Only file names on the command line; the secret stays in the config
  std::string cmd = "\"" + curl_ + "\" -K \"" + cfg.string() + "\" 2>&1";
#ifdef _WIN32
  cmd = "\"" + cmd + "\"";  // cmd.exe takes the whole line in quotes
#endif
  std::string out;
  int status = -1;
  if (FILE* p = popen(cmd.c_str(), "r")) {
    char buf[256];
    while (std::fgets(buf, sizeof buf, p)) out += buf;
    status = pclose(p);
  }
  fs::remove(cfg, ec);
  fs::remove(body, ec);
#ifndef _WIN32
  if (status != -1 && WIFEXITED(status)) status = WEXITSTATUS(status);
#endif
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  if (status == 0) return {d.channel, true, ""};
  if (status == 127 || status == 9009 || status == -1) return {d.channel, false, "curl could not be started"};
  return {d.channel, false, out.empty() ? "curl ended with " + std::to_string(status) : out.substr(0, 200)};
}

}  // namespace sim
