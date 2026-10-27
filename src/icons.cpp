#include "icons.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>

namespace oa {

namespace {

bool ReadBits(HBITMAP bmp, int& w, int& h, std::vector<uint32_t>& out) {
    BITMAP bm{};
    if (!GetObjectW(bmp, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight == 0) return false;
    w = bm.bmWidth;
    h = std::abs(bm.bmHeight);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    out.assign((size_t)w * h, 0);
    HDC dc = GetDC(nullptr);
    const int lines = GetDIBits(dc, bmp, 0, h, out.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    return lines == h;
}

bool HasAlpha(const std::vector<uint32_t>& px) {
    return std::any_of(px.begin(), px.end(), [](uint32_t p) { return (p >> 24) != 0; });
}

// Shell bitmaps are usually premultiplied already; a channel brighter than alpha means they aren't.
void Premultiply(std::vector<uint32_t>& px) {
    const bool straight = std::any_of(px.begin(), px.end(), [](uint32_t p) {
        const uint32_t a = p >> 24;
        return ((p >> 16) & 255) > a || ((p >> 8) & 255) > a || (p & 255) > a;
    });
    if (!straight) return;
    for (auto& p : px) {
        const uint32_t a = p >> 24;
        const uint32_t r = ((p >> 16) & 255) * a / 255, g = ((p >> 8) & 255) * a / 255, b = (p & 255) * a / 255;
        p = (a << 24) | (r << 16) | (g << 8) | b;
    }
}

Icon FromHBitmap(HBITMAP bmp) {
    auto icon = std::make_shared<IconPixels>();
    if (!ReadBits(bmp, icon->width, icon->height, icon->pixels)) return nullptr;
    if (!HasAlpha(icon->pixels)) {
        for (auto& p : icon->pixels) p |= 0xFF000000u;
    }
    Premultiply(icon->pixels);
    return icon;
}

Icon FromHIcon(HICON hicon) {
    ICONINFO ii{};
    if (!GetIconInfo(hicon, &ii)) return nullptr;
    auto icon = std::make_shared<IconPixels>();
    bool ok = false;
    if (ii.hbmColor && ReadBits(ii.hbmColor, icon->width, icon->height, icon->pixels)) {
        ok = true;
        if (!HasAlpha(icon->pixels)) {
            // An old icon without alpha: transparency comes from the AND mask (white = transparent).
            int mw = 0, mh = 0;
            std::vector<uint32_t> mask;
            const bool haveMask = ii.hbmMask && ReadBits(ii.hbmMask, mw, mh, mask) && mw == icon->width &&
                                  mh >= icon->height;
            for (size_t i = 0; i < icon->pixels.size(); ++i) {
                const bool transparent = haveMask && (mask[i] & 0xFFFFFF) != 0;
                icon->pixels[i] = transparent ? 0 : (icon->pixels[i] | 0xFF000000u);
            }
        }
        Premultiply(icon->pixels);
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return ok ? icon : nullptr;
}

}  // namespace

Icon IconFromShellItem(IShellItem* item, int sizePx) {
    if (!item) return nullptr;
    IShellItemImageFactory* factory = nullptr;
    if (FAILED(item->QueryInterface(IID_PPV_ARGS(&factory)))) return nullptr;
    HBITMAP bmp = nullptr;
    HRESULT hr = factory->GetImage({sizePx, sizePx}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &bmp);
    factory->Release();
    if (FAILED(hr) || !bmp) return nullptr;
    Icon icon = FromHBitmap(bmp);
    DeleteObject(bmp);
    return icon;
}

Icon IconFromPath(const std::wstring& path, int sizePx) {
    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) return nullptr;
    Icon icon = IconFromShellItem(item, sizePx);
    item->Release();
    return icon;
}

Icon IconFromWindow(HWND hwnd, const std::wstring& exePath, int sizePx) {
    // The exe's own icon is sharper than the small window icon most apps set.
    if (!exePath.empty()) {
        if (Icon icon = IconFromPath(exePath, sizePx)) return icon;
    }
    DWORD_PTR result = 0;
    HICON hicon = nullptr;
    if (SendMessageTimeoutW(hwnd, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 50, &result) && result) {
        hicon = reinterpret_cast<HICON>(result);
    }
    if (!hicon) hicon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICON));
    return hicon ? FromHIcon(hicon) : nullptr;
}

}  // namespace oa
