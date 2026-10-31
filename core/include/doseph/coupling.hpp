// SPDX-License-Identifier: AGPL-3.0-or-later
// Coupling two devices on one tank (PD-457, SD-131). One leads: it measures,
// controls and plans. The other doses on request and measures as a check.
// They pair with a code the second device shows; each then holds a peer
// token (SD-129) for the other. The link is direct, in the home network,
// without a cloud. The record of the peer, with the token this device uses
// to call it, lives in a file of its own, apart from the configuration: a
// backup never carries it. The core has no network (R8): it hands the
// platform the requests to send and takes back their answers.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "doseph/common.hpp"

namespace doseph {

struct Peer {
  std::string role;     // what this device is: "leader" or "follower"
  std::string id;       // the other device's ID
  std::string name;     // its name, as it said at pairing
  std::string address;  // where the leader reaches the follower: host or host:port; "" on the follower
  std::string token;    // the token this device sends to the other; secret
  std::string tokenId;  // the id of the token this device made for the other, to revoke it
  Epoch since = 0;
};

// The file's form, with the token; the API's form, without it
json peerToJson(const Peer& p, bool withSecret);
// A peer from the file; nothing when it cannot be read: the device stands alone
std::optional<Peer> peerFromJson(const json& j);

// A host name or an address, with a port where it has one: letters, digits, ".", "-", ":" and the
// brackets of IPv6
bool peerAddress(const std::string& s);

// The leader calls the follower this often; the follower stops dosing after this long without a
// call (SD-131)
constexpr Ms kPeerBeatMs = 2 * kSecond;
constexpr Ms kPeerLinkMs = 10 * kSecond;
// The follower measures as a check: while both pumps move water, pH or EC that differ by more than
// this for 10 min raise a warning; the leader keeps dosing by its own readings (SD-131)
constexpr double kPeerDevPh = 0.3, kPeerDevEc = 0.2;
constexpr Ms kPeerDevMs = 10 * kMinute;

// A request the platform sends to the other device and answers with peerResponse()
struct PeerRequest {
  long long id = 0;
  std::string address, method, path;
  json body = json::object();
  std::string token;  // sent as Authorization: Bearer; "" for the public join call
};

// The pairing code a device shows to be coupled as the follower: 8 characters of 32 without
// 0, 1, I and O (40 bits), shown in two groups of four, valid for 10 minutes, voided by the fifth
// wrong try. In memory only.
// The proof a leader sends instead of the pairing code (SD-149)
std::string pairingProof(const std::string& code, const std::string& leaderId, const std::string& token);

struct PairingCode {
  std::string code;  // without the dash
  Ms until = 0;
  int wrong = 0;
  bool label = false;  // the code on the device's label, armed for the function key's window (SD-149)
  static constexpr Ms kValid = 10 * kMinute;
  static constexpr int kMaxWrong = 5;
};

}  // namespace doseph
