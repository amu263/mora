#include "terminal.hpp"
namespace mora {
    static bool eq(RGB a, RGB b) {
        return a.r == b.r && a.g == b.g && a.b == b.b;
    }
    Canvas::Canvas(int width, int height, Palette p)
        : w(std::max(1, width)), h(std::max(1, height)), cells(w * h), colors(p) {
        for (auto& c : cells) {
            c.fg = p.text;
            c.bg = p.background;
        }
    }
    void Canvas::pixel(int x, int y, wchar_t ch, RGB fg, RGB bg, int action) {
        if (x < 0 || x >= w || y < 0 || y >= h)
            return;
        cells[y * w + x] = {ch, fg, bg, action, false};
    }
    void Canvas::text(int x, int y, std::wstring_view text, RGB fg, int maxWidth) {
        if (y < 0 || y >= h)
            return;
        auto s = truncate(sanitize(std::wstring(text)), std::min(maxWidth, w - x));
        for (auto c : s) {
            int width = displayWidth(std::wstring_view(&c, 1));
            if (x + width > w)
                break;
            if (x >= 0) {
                auto bg = cells[y * w + x].bg;
                pixel(x, y, c, fg, bg);
                if (width == 2 && x + 1 < w) {
                    pixel(x + 1, y, 0, fg, bg);
                    cells[y * w + x + 1].wide = true;
                }
            }
            x += width;
        }
    }
    void Canvas::line(int x, int y, int length, RGB color) {
        for (int i = 0; i < length; ++i)
            pixel(x + i, y, L'─', color, colors.background);
    }
    void Canvas::rect(int x, int y, int width, int height, RGB bg) {
        for (int j = 0; j < height; ++j)
            for (int i = 0; i < width; ++i)
                pixel(x + i, y + j, L' ', colors.text, bg);
    }
    void Canvas::button(int x, int y, int width, std::wstring_view label, int action, bool active) {
        rect(x, y, width, 1, active ? colors.accent : colors.surface);
        text(x + 1, y, label, active ? colors.background : colors.muted, width - 2);
        for (int i = 0; i < width; ++i)
            if (x + i >= 0 && x + i < w && y >= 0 && y < h)
                cells[y * w + x + i].action = action;
    }
    void Canvas::mark(int x, int y, int width, int action) {
        for (int i = 0; i < width; ++i)
            if (x + i >= 0 && x + i < w && y >= 0 && y < h)
                cells[y * w + x + i].action = action;
    }
    int Canvas::hit(int x, int y) const {
        return x >= 0 && x < w && y >= 0 && y < h ? cells[y * w + x].action : 0;
    }
    std::wstring Canvas::selectedText(int x1, int y1, int x2, int y2) const {
        x1 = std::clamp(x1, 0, w - 1);
        x2 = std::clamp(x2, 0, w - 1);
        y1 = std::clamp(y1, 0, h - 1);
        y2 = std::clamp(y2, 0, h - 1);
        if (y1 > y2 || (y1 == y2 && x1 > x2)) {
            std::swap(x1, x2);
            std::swap(y1, y2);
        }
        std::wstring out;
        for (int y = y1; y <= y2; ++y) {
            std::wstring line;
            int first = y == y1 ? x1 : 0, last = y == y2 ? x2 : w - 1;
            for (int x = first; x <= last; ++x) {
                const auto& cell = cells[y * w + x];
                if (cell.wide) {
                    if (x == first && x > 0)
                        line += cells[y * w + x - 1].c;
                } else if (cell.c)
                    line += cell.c;
            }
            while (!line.empty() && line.back() == L' ')
                line.pop_back();
            if (y > y1)
                out += L'\n';
            out += line;
        }
        return out;
    }
    void Canvas::highlight(int x1, int y1, int x2, int y2) {
        x1 = std::clamp(x1, 0, w - 1);
        x2 = std::clamp(x2, 0, w - 1);
        y1 = std::clamp(y1, 0, h - 1);
        y2 = std::clamp(y2, 0, h - 1);
        if (y1 > y2 || (y1 == y2 && x1 > x2)) {
            std::swap(x1, x2);
            std::swap(y1, y2);
        }
        for (int y = y1; y <= y2; ++y)
            for (int x = y == y1 ? x1 : 0; x <= (y == y2 ? x2 : w - 1); ++x) {
                auto& cell = cells[y * w + x];
                std::swap(cell.fg, cell.bg);
            }
    }
    std::wstring Canvas::ansi(const Canvas* previous) const {
        auto color = [](RGB c, int code) {
            return L"\x1b[" + std::to_wstring(code) + L";2;" + std::to_wstring(c.r) + L";" +
                   std::to_wstring(c.g) + L";" + std::to_wstring(c.b) + L"m";
        };
        std::wstring out;
        RGB fg{-1, -1, -1}, bg{-1, -1, -1};
        for (int y = 0; y < h; ++y) {
            bool same = previous && previous->w == w && previous->h == h;
            if (same)
                for (int x = 0; x < w; ++x) {
                    auto& a = cells[y * w + x];
                    auto& b = previous->cells[y * w + x];
                    if (a.c != b.c || !eq(a.fg, b.fg) || !eq(a.bg, b.bg)) {
                        same = false;
                        break;
                    }
                }
            if (same)
                continue;
            out += L"\x1b[" + std::to_wstring(y + 1) + L";1H";
            for (int x = 0; x < w; ++x) {
                auto& c = cells[y * w + x];
                if (c.wide)
                    continue;
                if (!eq(fg, c.fg)) {
                    out += color(c.fg, 38);
                    fg = c.fg;
                }
                if (!eq(bg, c.bg)) {
                    out += color(c.bg, 48);
                    bg = c.bg;
                }
                out += c.c ? c.c : L' ';
            }
        }
        return out;
    }
    std::wstring Canvas::plain() const {
        std::wstring out;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x)
                if (!cells[y * w + x].wide)
                    out += cells[y * w + x].c;
            out += L'\n';
        }
        return out;
    }

} // namespace mora
