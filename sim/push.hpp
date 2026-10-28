// SPDX-License-Identifier: AGPL-3.0-or-later
// The simulator's way to send pushes (SD-124): the system's curl, so the
// prototype needs no TLS library of its own. Each delivery becomes a curl
// config file (`-K`) that holds the address and the secret, readable by the
// user only and deleted after the run; the command line names only files.
// A thread of its own sends, so the robot's loop never waits on the network.
#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "doseph/push.hpp"

namespace sim {

// A string as curl's config file reads it: in quotes, with \ and " escaped
// and no line break of its own
std::string curlQuote(std::string_view s);
// A mail header's text in UTF-8 (RFC 2047)
std::string encodedWord(std::string_view s);

// What curl gets for one delivery: the config, and the body it reads from `bodyPath`
struct CurlJob {
  std::string config;
  std::string body;
};
CurlJob curlJob(const doseph::Delivery& d, const std::string& bodyPath, const std::string& nullDevice, doseph::Epoch now);

class PushSender {
 public:
  // `curl`: the program to run, "curl" from the PATH unless DP_CURL names another
  explicit PushSender(std::string curl);
  ~PushSender();
  PushSender(const PushSender&) = delete;
  PushSender& operator=(const PushSender&) = delete;

  void send(doseph::Delivery d);
  struct Result {
    std::string channel;
    bool ok = false;
    std::string error;
  };
  // How the deliveries sent since the last call fared
  std::vector<Result> poll();

 private:
  void run();
  Result deliver(const doseph::Delivery& d);
  std::string curl_;
  std::mutex mtx_;
  std::condition_variable cv_;
  std::deque<doseph::Delivery> queue_;
  std::vector<Result> results_;
  bool stop_ = false;
  std::thread worker_;
};

}  // namespace sim
