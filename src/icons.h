#pragma once

#include "common.h"

#include <shobjidl.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace oa {

// Premultiplied BGRA, top-down. Plain memory, so the indexing thread can hand it to the UI thread.
struct IconPixels {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels;
};
using Icon = std::shared_ptr<const IconPixels>;

Icon IconFromShellItem(IShellItem* item, int sizePx);
Icon IconFromPath(const std::wstring& path, int sizePx);
Icon IconFromWindow(HWND hwnd, const std::wstring& exePath, int sizePx);

}  // namespace oa
