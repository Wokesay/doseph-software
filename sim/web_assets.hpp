// SPDX-License-Identifier: AGPL-3.0-or-later
// The built web app inside the simulator's download (DP_EMBEDDED_WEB): each
// file gzip-compressed, written by tools/embed_web.mjs.
#pragma once

#include <cstddef>

namespace sim {

struct WebAsset {
  const char* path;  // "/index.html", "/assets/…"
  const char* type;  // Content-Type
  const unsigned char* data;
  std::size_t size;
};

extern const WebAsset kWebAssets[];
extern const std::size_t kWebAssetCount;

}  // namespace sim
