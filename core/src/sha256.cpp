// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/sha256.hpp"

#include <cstring>

namespace doseph {

namespace {

constexpr std::uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

struct Ctx {
  std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::uint8_t buf[64] = {};
  size_t bufLen = 0;
  std::uint64_t total = 0;

  void block(const std::uint8_t* p) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = (std::uint32_t(p[4 * i]) << 24) | (std::uint32_t(p[4 * i + 1]) << 16) |
             (std::uint32_t(p[4 * i + 2]) << 8) | std::uint32_t(p[4 * i + 3]);
    for (int i = 16; i < 64; ++i) {
      std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      std::uint32_t ch = (e & f) ^ (~e & g);
      std::uint32_t t1 = hh + S1 + ch + K[i] + w[i];
      std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      std::uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
      std::uint32_t t2 = S0 + mj;
      hh = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }

  void update(const std::uint8_t* p, size_t n) {
    total += n;
    while (n > 0) {
      size_t take = std::min(n, 64 - bufLen);
      std::memcpy(buf + bufLen, p, take);
      bufLen += take;
      p += take;
      n -= take;
      if (bufLen == 64) {
        block(buf);
        bufLen = 0;
      }
    }
  }

  Digest finish() {
    std::uint64_t bits = total * 8;
    std::uint8_t pad = 0x80;
    update(&pad, 1);
    std::uint8_t zero = 0;
    while (bufLen != 56) update(&zero, 1);
    std::uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = std::uint8_t(bits >> (56 - 8 * i));
    update(len, 8);
    Digest out;
    for (int i = 0; i < 8; ++i) {
      out[4 * i] = std::uint8_t(h[i] >> 24);
      out[4 * i + 1] = std::uint8_t(h[i] >> 16);
      out[4 * i + 2] = std::uint8_t(h[i] >> 8);
      out[4 * i + 3] = std::uint8_t(h[i]);
    }
    return out;
  }
};

}  // namespace

Digest sha256(const std::uint8_t* data, size_t len) {
  Ctx c;
  c.update(data, len);
  return c.finish();
}

Digest sha256(const std::string& s) { return sha256(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()); }

Digest hmacSha256(const std::vector<std::uint8_t>& keyIn, const std::uint8_t* data, size_t len) {
  std::uint8_t key[64] = {};
  if (keyIn.size() > 64) {
    Digest d = sha256(keyIn.data(), keyIn.size());
    std::memcpy(key, d.data(), 32);
  } else if (!keyIn.empty()) {
    std::memcpy(key, keyIn.data(), keyIn.size());
  }
  std::uint8_t ipad[64], opad[64];
  for (int i = 0; i < 64; ++i) {
    ipad[i] = key[i] ^ 0x36;
    opad[i] = key[i] ^ 0x5c;
  }
  Ctx inner;
  inner.update(ipad, 64);
  inner.update(data, len);
  Digest ih = inner.finish();
  Ctx outer;
  outer.update(opad, 64);
  outer.update(ih.data(), ih.size());
  return outer.finish();
}

std::vector<std::uint8_t> pbkdf2Sha256(const std::string& password, const std::vector<std::uint8_t>& salt,
                                       std::uint32_t iterations, size_t outLen) {
  std::vector<std::uint8_t> key(password.begin(), password.end());
  std::vector<std::uint8_t> out;
  for (std::uint32_t block = 1; out.size() < outLen; ++block) {
    std::vector<std::uint8_t> msg(salt);
    msg.push_back(std::uint8_t(block >> 24));
    msg.push_back(std::uint8_t(block >> 16));
    msg.push_back(std::uint8_t(block >> 8));
    msg.push_back(std::uint8_t(block));
    Digest u = hmacSha256(key, msg.data(), msg.size());
    Digest t = u;
    for (std::uint32_t i = 1; i < iterations; ++i) {
      u = hmacSha256(key, u.data(), u.size());
      for (size_t k = 0; k < t.size(); ++k) t[k] ^= u[k];
    }
    for (size_t k = 0; k < t.size() && out.size() < outLen; ++k) out.push_back(t[k]);
  }
  return out;
}

std::string toHex(const std::uint8_t* data, size_t len) {
  static const char* hex = "0123456789abcdef";
  std::string s;
  s.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    s += hex[data[i] >> 4];
    s += hex[data[i] & 15];
  }
  return s;
}

std::vector<std::uint8_t> fromHex(const std::string& hex) {
  std::vector<std::uint8_t> out;
  auto val = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    int a = val(hex[i]), b = val(hex[i + 1]);
    if (a < 0 || b < 0) return {};
    out.push_back(std::uint8_t(a * 16 + b));
  }
  return out;
}

}  // namespace doseph
