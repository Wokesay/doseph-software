// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/history.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace doseph {

namespace {
constexpr const char* kMagic = "GCH1";
constexpr const char* kTimeTag = "GCT2";  // after the series: each series' newest sample (older files lack it)
constexpr size_t kMaxIdLen = 255, kMaxPerRecord = 1024;

std::uint32_t checksum(const char* p, size_t n) {  // FNV-1a
  std::uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<unsigned char>(p[i])) * 16777619u;
  return h;
}

template <typename T>
void put(std::string& out, const T& v) {
  out.append(reinterpret_cast<const char*>(&v), sizeof(T));
}
template <typename T>
bool get(const char*& p, const char* end, T& v) {
  if (end - p < static_cast<std::ptrdiff_t>(sizeof(T))) return false;
  std::memcpy(&v, p, sizeof(T));
  p += sizeof(T);
  return true;
}
}  // namespace

Series::Series() {
  const int steps[3] = {10, 60, 900};
  const size_t caps[3] = {8640, 10080, 35040};
  for (size_t i = 0; i < 3; ++i) {
    auto& t = tiers_[i];
    t.stepS = steps[i];
    t.cap = caps[i];
    t.minmax = i > 0;
    t.avg.assign(t.cap, NAN);
    if (t.minmax) {
      t.mn.assign(t.cap, NAN);
      t.mx.assign(t.cap, NAN);
    }
  }
}

void Series::commit(Tier& tr, std::int64_t slot) {
  size_t idx = static_cast<size_t>(slot % static_cast<std::int64_t>(tr.cap));
  tr.avg[idx] = tr.n ? static_cast<float>(tr.sum / tr.n) : NAN;
  if (tr.minmax) {
    tr.mn[idx] = tr.n ? static_cast<float>(tr.cmin) : NAN;
    tr.mx[idx] = tr.n ? static_cast<float>(tr.cmax) : NAN;
  }
  tr.head = slot;
}

void Series::add(Epoch t, double v) {
  for (auto& tr : tiers_) {
    std::int64_t slot = t / tr.stepS;
    if (tr.cur < 0) tr.cur = slot;
    if (slot < tr.cur) continue;  // the clock ran backwards: dropped
    if (slot > tr.cur) {
      commit(tr, tr.cur);
      // skipped slots become gaps (at most once around)
      std::int64_t gapFrom = std::max(tr.cur + 1, slot - static_cast<std::int64_t>(tr.cap));
      for (std::int64_t s = gapFrom; s < slot; ++s) {
        tr.sum = 0;
        tr.n = 0;
        commit(tr, s);
      }
      tr.cur = slot;
      tr.sum = 0;
      tr.n = 0;
    }
    if (std::isfinite(v)) {
      if (tr.n == 0) tr.cmin = tr.cmax = v;
      tr.cmin = std::min(tr.cmin, v);
      tr.cmax = std::max(tr.cmax, v);
      tr.sum += v;
      tr.n++;
    }
  }
}

SeriesPoints Series::query(Epoch from, Epoch to, size_t maxPoints) const {
  SeriesPoints out;
  if (to < from) return out;
  // the finest level whose retention still covers "from"
  const Tier* tr = &tiers_.back();
  for (const auto& t : tiers_) {
    std::int64_t oldest = (t.cur - static_cast<std::int64_t>(t.cap) + 1) * t.stepS;
    if (t.cur >= 0 && oldest <= from) {
      tr = &t;
      break;
    }
  }
  out.stepS = tr->stepS;
  if (tr->cur < 0) return out;
  std::int64_t s0 = from / tr->stepS;
  std::int64_t s1 = std::min<std::int64_t>(to / tr->stepS, tr->cur);
  std::int64_t oldest = tr->cur - static_cast<std::int64_t>(tr->cap) + 1;
  s0 = std::max(s0, oldest);
  for (std::int64_t s = s0; s <= s1; ++s) {
    double a, mn, mx;
    if (s == tr->cur) {  // the slot being filled, from the accumulator
      a = tr->n ? tr->sum / tr->n : NAN;
      mn = tr->n ? tr->cmin : NAN;
      mx = tr->n ? tr->cmax : NAN;
    } else if (s > tr->head) {
      continue;
    } else {
      size_t idx = static_cast<size_t>(s % static_cast<std::int64_t>(tr->cap));
      a = tr->avg[idx];
      mn = tr->minmax ? tr->mn[idx] : a;
      mx = tr->minmax ? tr->mx[idx] : a;
    }
    out.t.push_back(s * tr->stepS);
    out.avg.push_back(a);
    out.min.push_back(mn);
    out.max.push_back(mx);
  }
  if (maxPoints > 0 && out.t.size() > maxPoints) {
    size_t k = (out.t.size() + maxPoints - 1) / maxPoints;
    SeriesPoints d;
    d.stepS = out.stepS * static_cast<int>(k);
    for (size_t i = 0; i < out.t.size(); i += k) {
      double sum = 0, mn = NAN, mx = NAN;
      int n = 0;
      for (size_t j = i; j < std::min(i + k, out.t.size()); ++j) {
        if (std::isfinite(out.avg[j])) {
          sum += out.avg[j];
          n++;
        }
        if (std::isfinite(out.min[j])) mn = std::isfinite(mn) ? std::min(mn, out.min[j]) : out.min[j];
        if (std::isfinite(out.max[j])) mx = std::isfinite(mx) ? std::max(mx, out.max[j]) : out.max[j];
      }
      d.t.push_back(out.t[i]);
      d.avg.push_back(n ? sum / n : NAN);
      d.min.push_back(mn);
      d.max.push_back(mx);
    }
    return d;
  }
  return out;
}

void Series::dump(std::string& out) const {
  for (const auto& t : tiers_) {
    put(out, t.head);
    put(out, t.cur);
    put(out, t.sum);
    put(out, t.cmin);
    put(out, t.cmax);
    put(out, t.n);
    out.append(reinterpret_cast<const char*>(t.avg.data()), t.cap * sizeof(float));
    if (t.minmax) {
      out.append(reinterpret_cast<const char*>(t.mn.data()), t.cap * sizeof(float));
      out.append(reinterpret_cast<const char*>(t.mx.data()), t.cap * sizeof(float));
    }
  }
}

bool Series::load(const char*& p, const char* end) {
  for (auto& t : tiers_) {
    if (!get(p, end, t.head) || !get(p, end, t.cur) || !get(p, end, t.sum) || !get(p, end, t.cmin) ||
        !get(p, end, t.cmax) || !get(p, end, t.n))
      return false;
    size_t bytes = t.cap * sizeof(float) * (t.minmax ? 3 : 1);
    if (static_cast<size_t>(end - p) < bytes) return false;
    std::memcpy(t.avg.data(), p, t.cap * sizeof(float));
    p += t.cap * sizeof(float);
    if (t.minmax) {
      std::memcpy(t.mn.data(), p, t.cap * sizeof(float));
      p += t.cap * sizeof(float);
      std::memcpy(t.mx.data(), p, t.cap * sizeof(float));
      p += t.cap * sizeof(float);
    }
  }
  return true;
}

void History::add(const std::string& id, Epoch t, double v) {
  series_[id].add(t, v);
  Epoch& newest = newest_[id];
  newest = std::max(newest, t);
}

std::string History::record(Epoch t, const std::vector<std::pair<std::string, double>>& samples) {
  // [u32 length][u32 checksum] then the body: [i64 t][u16 n] n × [u16 id length][id][f64 value]
  std::string body;
  put(body, static_cast<std::int64_t>(t));
  const auto n = static_cast<std::uint16_t>(std::min(samples.size(), kMaxPerRecord));
  put(body, n);
  for (size_t i = 0; i < n; ++i) {
    const std::string id = samples[i].first.substr(0, kMaxIdLen);
    put(body, static_cast<std::uint16_t>(id.size()));
    body += id;
    put(body, samples[i].second);
  }
  std::string out;
  put(out, static_cast<std::uint32_t>(body.size()));
  put(out, checksum(body.data(), body.size()));
  return out + body;
}

size_t History::replay(const std::string& journal, const std::set<std::string>& ids) {
  const std::map<std::string, Epoch> held = newest_;  // the snapshot holds each series up to here
  const char* p = journal.data();
  const char* end = p + journal.size();
  size_t added = 0;
  while (p < end) {
    std::uint32_t len = 0, sum = 0;
    if (!get(p, end, len) || !get(p, end, sum) || static_cast<size_t>(end - p) < len) break;  // torn end
    if (checksum(p, len) != sum) break;  // torn, then written over: nothing after it is trusted
    const char* q = p;
    const char* rend = p + len;
    p = rend;
    std::int64_t t = 0;
    std::uint16_t n = 0;
    if (!get(q, rend, t) || !get(q, rend, n)) break;
    std::vector<std::pair<std::string, double>> samples;
    bool ok = true;
    for (std::uint16_t i = 0; i < n && ok; ++i) {
      std::uint16_t idLen = 0;
      double v = 0;
      ok = get(q, rend, idLen) && idLen <= kMaxIdLen && static_cast<size_t>(rend - q) >= idLen;
      if (!ok) break;
      std::string id(q, idLen);
      q += idLen;
      ok = get(q, rend, v) && !id.empty();
      if (ok) samples.emplace_back(std::move(id), v);
    }
    if (!ok || q != rend) break;  // damaged: nothing after it is trusted
    if (!plausibleEpoch(t)) continue;  // a time no clock can have would push the series ahead for good
    bool any = false;
    for (const auto& [id, v] : samples) {
      auto h = held.find(id);
      if (!ids.count(id) || (h != held.end() && t <= h->second)) continue;
      add(id, t, v);
      any = true;
    }
    added += any ? 1 : 0;
  }
  return added;
}

SeriesPoints History::query(const std::string& id, Epoch from, Epoch to, size_t maxPoints) const {
  auto it = series_.find(id);
  if (it == series_.end()) return {};
  return it->second.query(from, to, maxPoints);
}

std::vector<std::string> History::ids() const {
  std::vector<std::string> out;
  for (const auto& [id, s] : series_) out.push_back(id);
  return out;
}

std::string History::dump() const {
  std::string out(kMagic);
  std::uint32_t n = static_cast<std::uint32_t>(series_.size());
  put(out, n);
  for (const auto& [id, s] : series_) {
    std::uint32_t len = static_cast<std::uint32_t>(id.size());
    put(out, len);
    out += id;
    s.dump(out);
  }
  out += kTimeTag;
  put(out, static_cast<std::uint32_t>(newest_.size()));
  for (const auto& [id, t] : newest_) {
    put(out, static_cast<std::uint16_t>(std::min(id.size(), kMaxIdLen)));
    out += id.substr(0, kMaxIdLen);
    put(out, static_cast<std::int64_t>(t));
  }
  return out;
}

bool History::load(const std::string& data) {
  const char* p = data.data();
  const char* end = p + data.size();
  if (data.size() < 8 || std::memcmp(p, kMagic, 4) != 0) return false;
  p += 4;
  std::uint32_t n = 0;
  if (!get(p, end, n)) return false;
  std::map<std::string, Series> loaded;
  for (std::uint32_t i = 0; i < n; ++i) {
    std::uint32_t len = 0;
    if (!get(p, end, len) || static_cast<std::uint32_t>(end - p) < len) return false;
    std::string id(p, len);
    p += len;
    if (!loaded[id].load(p, end)) return false;
  }
  std::map<std::string, Epoch> newest;
  if (p < end) {  // a snapshot cut inside its times is no snapshot: its journal would count twice
    if (end - p < 4 || std::memcmp(p, kTimeTag, 4) != 0) return false;
    p += 4;
    std::uint32_t count = 0;
    if (!get(p, end, count) || count > loaded.size()) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
      std::uint16_t len = 0;
      std::int64_t t = 0;
      if (!get(p, end, len) || static_cast<size_t>(end - p) < len) return false;
      std::string id(p, len);
      p += len;
      if (!get(p, end, t)) return false;
      newest[id] = t;
    }
  }
  series_ = std::move(loaded);
  newest_ = std::move(newest);
  return true;
}

}  // namespace doseph
