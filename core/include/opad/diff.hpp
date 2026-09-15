#pragma once
#include "document.hpp"
#include "render.hpp"

namespace opad {
// Op-level and body-store diff between two documents.
json diff_documents(const Document& a, const Document& b);
// Geometric diff image: unchanged grey, only-in-a red, only-in-b green.
Image render_diff(const Document& a, const Document& b, const RenderOptions& opt);
}  // namespace opad
