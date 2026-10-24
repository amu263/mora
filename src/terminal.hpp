#pragma once
#include "core.hpp"
namespace mora {
    struct Cell {
        wchar_t c = L' ';
        RGB fg, bg;
        int action = 0;
        bool wide = false;
    };
    class Canvas {
        int w, h;
        std::vector<Cell> cells;
        Palette colors;

      public:
        Canvas(int width, int height, Palette p);
        int width() const {
            return w;
        }
        int height() const {
            return h;
        }
        void pixel(int x, int y, wchar_t ch, RGB fg, RGB bg, int action = 0);
        void text(int x, int y, std::wstring_view s, RGB fg, int maxWidth = 10000);
        void line(int x, int y, int length, RGB color);
        void rect(int x, int y, int width, int height, RGB bg);
        void button(int x, int y, int width, std::wstring_view label, int action,
                    bool active = false);
        void mark(int x, int y, int width, int action);
        int hit(int x, int y) const;
        std::wstring selectedText(int x1, int y1, int x2, int y2) const;
        void highlight(int x1, int y1, int x2, int y2);
        std::wstring ansi(const Canvas* previous = nullptr) const;
        std::wstring plain() const;
    };
    class Console {
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD oldIn = 0, oldOut = 0;
        bool valid = false;

      public:
        Console() {
            valid = GetConsoleMode(in, &oldIn) && GetConsoleMode(out, &oldOut);
            if (valid) {
                SetConsoleOutputCP(CP_UTF8);
                SetConsoleCP(CP_UTF8);
                SetConsoleMode(in,
                               ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT);
                SetConsoleMode(out, oldOut | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                                        DISABLE_NEWLINE_AUTO_RETURN);
                write(L"\x1b[?1049h\x1b[?25l\x1b[2J");
            }
        }
        ~Console() {
            if (valid) {
                write(L"\x1b[0m\x1b[?25h\x1b[?1049l");
                SetConsoleMode(in, oldIn);
                SetConsoleMode(out, oldOut);
            }
        }
        bool interactive() const {
            return valid;
        }
        void write(std::wstring_view s) {
            DWORD n;
            if (valid)
                WriteConsoleW(out, s.data(), (DWORD)s.size(), &n, nullptr);
        }
        void cursor(int x, int y, bool visible) {
            if (valid)
                write(visible ? L"\x1b[" + std::to_wstring(y + 1) + L";" + std::to_wstring(x + 1) +
                                    L"H\x1b[?25h"
                              : L"\x1b[?25l");
        }
        std::pair<int, int> size() {
            CONSOLE_SCREEN_BUFFER_INFO b{};
            if (valid && GetConsoleScreenBufferInfo(out, &b))
                return {std::clamp(int(b.srWindow.Right - b.srWindow.Left + 1), 40, 240),
                        std::clamp(int(b.srWindow.Bottom - b.srWindow.Top + 1), 15, 100)};
            return {116, 36};
        }
        std::vector<INPUT_RECORD> events() {
            std::vector<INPUT_RECORD> r;
            if (!valid)
                return r;
            DWORD n = 0;
            GetNumberOfConsoleInputEvents(in, &n);
            if (n) {
                r.resize(std::min(DWORD(64), n));
                ReadConsoleInputW(in, r.data(), (DWORD)r.size(), &n);
                r.resize(n);
            }
            return r;
        }
    };

} // namespace mora
