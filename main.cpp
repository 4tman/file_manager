/*
 * Консольный файловый менеджер
 * C++17 + Windows Console API (без сторонних зависимостей)
 *
 * Управление:
 *   Стрелки вверх/вниз  — перемещение курсора
 *   Enter               — войти в директорию
 *   Backspace           — выйти в родительскую директорию
 *   Tab                 — переключить активную панель
 *   Space               — поставить/снять отметку на файл/директорию
 *   F1                  — скопировать отмеченный элемент в текущую директорию
 *   F2                  — переместить (вырезать) отмеченный элемент
 *   F3                  — просмотр содержимого файла
 *   F4                  — переключить режим отображения (подробный/краткий)
 *   F5                  — скачать файл по HTTP-ссылке
 *   Del                 — удалить файл/директорию под курсором
 *   PgUp / PgDn         — прокрутка списка
 *   Q                   — выход
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wininet.h>
#include <shlwapi.h>

#include <filesystem>
#include <vector>
#include <string>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cassert>
#include <ctime>

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "shlwapi.lib")

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────
// Консольные утилиты
// ─────────────────────────────────────────────────────────────

static HANDLE hOut = INVALID_HANDLE_VALUE;
static HANDLE hIn = INVALID_HANDLE_VALUE;

static int g_cols = 80;
static int g_rows = 24;

static void updateConsoleSize()
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(hOut, &csbi)) {
        g_cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        g_rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    }
}

// Установить позицию курсора (0-based)
static void gotoXY(int x, int y)
{
    COORD c = { (SHORT)x, (SHORT)y };
    SetConsoleCursorPosition(hOut, c);
}

// Установить атрибут цвета (FOREGROUND_* | BACKGROUND_*)
static void setAttr(WORD attr)
{
    SetConsoleTextAttribute(hOut, attr);
}

static const WORD ATTR_NORMAL = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
static const WORD ATTR_BOLD = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
static const WORD ATTR_SELECTED = BACKGROUND_BLUE | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
static const WORD ATTR_DIR = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
static const WORD ATTR_HEADER = BACKGROUND_BLUE | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
static const WORD ATTR_STATUS = BACKGROUND_GREEN | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
static const WORD ATTR_HELP = 0 | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
static const WORD ATTR_MARK = FOREGROUND_RED | FOREGROUND_INTENSITY;

// Напечатать строку ровно width символов (обрезать или добить пробелами)
static void printFixed(int x, int y, const std::string& s, int width, WORD attr)
{
    gotoXY(x, y);
    setAttr(attr);
    std::string out = s;
    if ((int)out.size() > width) {
        out = out.substr(0, width - 1) + "~";
    }
    while ((int)out.size() < width) out += ' ';
    DWORD written;
    WriteConsoleA(hOut, out.c_str(), (DWORD)out.size(), &written, nullptr);
}

static void printAt(int x, int y, const std::string& s, WORD attr)
{
    gotoXY(x, y);
    setAttr(attr);
    DWORD written;
    WriteConsoleA(hOut, s.c_str(), (DWORD)s.size(), &written, nullptr);
}

// Скрыть/показать курсор
static void showCursor(bool show)
{
    CONSOLE_CURSOR_INFO ci;
    GetConsoleCursorInfo(hOut, &ci);
    ci.bVisible = show ? TRUE : FALSE;
    SetConsoleCursorInfo(hOut, &ci);
}

static void clearScreen()
{
    COORD origin = { 0, 0 };
    DWORD written;
    DWORD size = g_cols * g_rows;
    FillConsoleOutputCharacterA(hOut, ' ', size, origin, &written);
    FillConsoleOutputAttribute(hOut, ATTR_NORMAL, size, origin, &written);
    gotoXY(0, 0);
}

// ─────────────────────────────────────────────────────────────
// Форматирование
// ─────────────────────────────────────────────────────────────

static std::string formatSize(uintmax_t bytes)
{
    const char* units[] = { "B", "K", "M", "G", "T" };
    double val = (double)bytes;
    int u = 0;
    while (val >= 1024.0 && u < 4) { val /= 1024.0; ++u; }
    char buf[32];
    if (u == 0) snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)bytes);
    else        snprintf(buf, sizeof(buf), "%.1f %s", val, units[u]);
    return buf;
}

static std::string formatTime(const fs::file_time_type& ft)
{
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    std::time_t tt = std::chrono::system_clock::to_time_t(sctp);
    std::tm tm_buf;
    localtime_s(&tm_buf, &tt);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm_buf);
    return buf;
}

// ─────────────────────────────────────────────────────────────
// Элемент файловой системы
// ─────────────────────────────────────────────────────────────

struct Entry {
    fs::path    path;
    bool        isDir = false;
    uintmax_t   size = 0;
    std::string modTime;

    Entry() = default;
    explicit Entry(const fs::directory_entry& de)
        : path(de.path()), isDir(de.is_directory())
    {
        try {
            size = isDir ? 0 : de.file_size();
            modTime = formatTime(de.last_write_time());
        }
        catch (...) {
            size = 0;
            modTime = "??????????";
        }
    }
};

// ─────────────────────────────────────────────────────────────
// Панель
// ─────────────────────────────────────────────────────────────

struct Panel {
    fs::path         cwd;
    std::vector<Entry> entries;
    int              cursor = 0;
    int              scroll = 0;
    bool             detailed = false;

    std::string lastError; // непустая если последний loadDir завершился ошибкой

    bool loadDir(const fs::path& p)
    {
        fs::path target = fs::absolute(p);
        lastError.clear();

        try {
            // Создаём итератор — бросит filesystem_error при отказе в доступе
            fs::directory_iterator it(target,
                fs::directory_options::skip_permission_denied);

            // Доступ есть — обновляем состояние панели
            cwd = target;
            entries.clear();
            cursor = 0;
            scroll = 0;

            for (const auto& de : it)
                entries.emplace_back(de);

        }
        catch (const fs::filesystem_error&) {
            DWORD err = ::GetLastError();
            if (err == ERROR_ACCESS_DENIED || err == 0)
                lastError = "Нет доступа: " + target.filename().string();
            else
                lastError = "Ошибка открытия папки: " + target.filename().string();
            return false; // остаёмся в текущей папке
        }
        catch (...) {
            lastError = "Неизвестная ошибка при открытии папки";
            return false;
        }

        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            if (a.isDir != b.isDir) return a.isDir > b.isDir;
            return a.path.filename() < b.path.filename();
            });
        return true;
    }

    Entry* currentEntry()
    {
        if (entries.empty()) return nullptr;
        return &entries[cursor];
    }

    void adjustScroll(int visRows)
    {
        if (cursor < scroll)             scroll = cursor;
        if (cursor >= scroll + visRows)  scroll = cursor - visRows + 1;
        if (scroll < 0) scroll = 0;
    }
};

// ─────────────────────────────────────────────────────────────
// Отрисовка панели
// ─────────────────────────────────────────────────────────────

static void drawPanel(const Panel& panel,
    int startX, int width,
    int startY, int height,
    bool active,
    const fs::path& globalMark, bool hasMark)
{
    int innerW = width - 2;
    int listRows = height - 2;

    // Верхняя граница
    {
        std::string border(width, '-');
        border[0] = '+'; border[width - 1] = '+';
        printAt(startX, startY, border, active ? ATTR_BOLD : ATTR_NORMAL);
    }

    // Заголовок (путь) поверх верхней границы
    {
        std::string hdr = " " + panel.cwd.string() + " ";
        if ((int)hdr.size() > innerW) hdr = hdr.substr(0, innerW - 1) + "~";
        printAt(startX + 1, startY, hdr, ATTR_HEADER);
    }

    // Боковые границы и содержимое
    for (int row = 0; row < listRows; row++) {
        int y = startY + 1 + row;
        int idx = panel.scroll + row;

        printAt(startX, y, "|", active ? ATTR_BOLD : ATTR_NORMAL);
        printAt(startX + width - 1, y, "|", active ? ATTR_BOLD : ATTR_NORMAL);

        if (idx >= (int)panel.entries.size()) {
            printFixed(startX + 1, y, "", innerW, ATTR_NORMAL);
            continue;
        }

        const Entry& e = panel.entries[idx];
        bool isCursor = (idx == panel.cursor) && active;
        bool isMark = hasMark && (e.path == globalMark);

        std::string name = e.path.filename().string();
        if (e.isDir) name += "\\";

        std::string line;
        if (panel.detailed) {
            std::string sz = e.isDir ? "<DIR>  " : formatSize(e.size);
            int nameW = std::max(10, innerW - 26);
            std::string namePart = name.size() > (size_t)nameW
                ? name.substr(0, nameW - 1) + "~" : name;
            char buf[512];
            snprintf(buf, sizeof(buf), "%-*s  %-7s  %s",
                nameW, namePart.c_str(),
                sz.c_str(),
                e.modTime.substr(0, 16).c_str());
            line = buf;
        }
        else {
            line = name;
        }

        WORD attr;
        if (isCursor)      attr = ATTR_SELECTED;
        else if (isMark)   attr = ATTR_MARK;
        else if (e.isDir)  attr = ATTR_DIR;
        else               attr = ATTR_NORMAL;

        printFixed(startX + 1, y, line, innerW, attr);
    }

    // Нижняя граница
    {
        int y = startY + height - 1;
        std::string border(width, '-');
        border[0] = '+'; border[width - 1] = '+';
        printAt(startX, y, border, active ? ATTR_BOLD : ATTR_NORMAL);

        char info[64];
        snprintf(info, sizeof(info), " %d элем.", (int)panel.entries.size());
        printAt(startX + 1, y, info, active ? ATTR_BOLD : ATTR_NORMAL);
    }
}

// ─────────────────────────────────────────────────────────────
// Строки состояния и подсказок
// ─────────────────────────────────────────────────────────────

static void drawStatusBar(int row, const std::string& msg)
{
    std::string s = " " + msg;
    printFixed(0, row, s, g_cols, ATTR_STATUS);
}

static void drawHelpBar(int row)
{
    std::string help =
        " Tab=панель Ent=открыть BS=назад Spc=метка"
        " F1=копир F2=перемест F3=просмотр F4=вид F5=HTTP Del=удалить Q=выход";
    printFixed(0, row, help, g_cols, ATTR_HELP);
}

// ─────────────────────────────────────────────────────────────
// Ввод строки внизу экрана
// ─────────────────────────────────────────────────────────────

static std::string inputLine(const std::string& prompt)
{
    int row = g_rows - 1;
    printFixed(0, row, prompt, g_cols, ATTR_STATUS);
    gotoXY((int)prompt.size(), row);
    showCursor(true);

    // Временно переключить режим ввода
    DWORD oldMode;
    GetConsoleMode(hIn, &oldMode);
    SetConsoleMode(hIn, ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);

    char buf[1024] = {};
    DWORD read = 0;
    ReadConsoleA(hIn, buf, sizeof(buf) - 1, &read, nullptr);
    // Убрать \r\n
    while (read > 0 && (buf[read - 1] == '\n' || buf[read - 1] == '\r')) {
        buf[--read] = '\0';
    }

    SetConsoleMode(hIn, oldMode);
    showCursor(false);
    return buf;
}

// ─────────────────────────────────────────────────────────────
// Диалог подтверждения
// ─────────────────────────────────────────────────────────────

static bool confirm(const std::string& question)
{
    std::string msg = " " + question + " [Y/N] ";
    printFixed(0, g_rows - 1, msg, g_cols, ATTR_MARK);
    gotoXY((int)msg.size(), g_rows - 1);

    while (true) {
        INPUT_RECORD ir;
        DWORD read;
        ReadConsoleInputA(hIn, &ir, 1, &read);
        if (ir.EventType == KEY_EVENT && ir.Event.KeyEvent.bKeyDown) {
            char c = ir.Event.KeyEvent.uChar.AsciiChar;
            if (c == 'y' || c == 'Y') return true;
            if (c == 'n' || c == 'N' || ir.Event.KeyEvent.wVirtualKeyCode == VK_ESCAPE)
                return false;
        }
    }
}

// ─────────────────────────────────────────────────────────────
// Просмотр файла
// ─────────────────────────────────────────────────────────────

static void viewFile(const fs::path& filepath)
{
    std::vector<std::string> lines;
    std::ifstream f(filepath);
    if (!f) {
        lines.push_back("[Не удалось открыть файл]");
    }
    else {
        std::string line;
        while (std::getline(f, line)) {
            // Убрать \r если есть
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }

    int topLine = 0;

    while (true) {
        updateConsoleSize();
        clearScreen();

        // Заголовок
        std::string title = " Просмотр: " + filepath.filename().string() +
            "  [Q/Esc — назад]  [Стрелки — прокрутка]";
        printFixed(0, 0, title, g_cols, ATTR_HEADER);

        int viewRows = g_rows - 2;
        for (int i = 0; i < viewRows; i++) {
            int li = topLine + i;
            std::string row_s = (li < (int)lines.size()) ? lines[li] : "";
            printFixed(0, i + 1, row_s, g_cols, ATTR_NORMAL);
        }

        // Статус
        char status[64];
        snprintf(status, sizeof(status), " Строка %d / %d  PgUp/PgDn — прокрутка",
            topLine + 1, (int)lines.size());
        printFixed(0, g_rows - 1, status, g_cols, ATTR_STATUS);

        // Ввод
        INPUT_RECORD ir;
        DWORD read;
        ReadConsoleInputA(hIn, &ir, 1, &read);
        if (ir.EventType != KEY_EVENT || !ir.Event.KeyEvent.bKeyDown) continue;

        WORD vk = ir.Event.KeyEvent.wVirtualKeyCode;
        char c = ir.Event.KeyEvent.uChar.AsciiChar;

        if (c == 'q' || c == 'Q' || vk == VK_ESCAPE || vk == VK_F3) break;
        if (vk == VK_UP && topLine > 0) --topLine;
        if (vk == VK_DOWN && topLine + viewRows < (int)lines.size()) ++topLine;
        if (vk == VK_PRIOR) topLine = std::max(0, topLine - viewRows);
        if (vk == VK_NEXT)  topLine = std::min((int)lines.size() - 1, topLine + viewRows);
    }
}

// ─────────────────────────────────────────────────────────────
// Скачивание файла по HTTP через WinINet
// ─────────────────────────────────────────────────────────────

static std::string downloadHTTP(const std::string& url, const fs::path& destDir)
{
    // Имя файла из URL
    std::string filename = url.substr(url.rfind('/') + 1);
    // Убрать query string если есть
    auto q = filename.find('?');
    if (q != std::string::npos) filename = filename.substr(0, q);
    if (filename.empty()) filename = "downloaded_file";

    fs::path dest = destDir / filename;

    HINTERNET hInternet = InternetOpenA("FileManager/1.0",
        INTERNET_OPEN_TYPE_PRECONFIG,
        nullptr, nullptr, 0);
    if (!hInternet) return "Ошибка: InternetOpen failed";

    HINTERNET hUrl = InternetOpenUrlA(hInternet, url.c_str(),
        nullptr, 0,
        INTERNET_FLAG_RELOAD, 0);
    if (!hUrl) {
        InternetCloseHandle(hInternet);
        return "Ошибка: не удалось открыть URL";
    }

    std::ofstream out(dest, std::ios::binary);
    if (!out) {
        InternetCloseHandle(hUrl);
        InternetCloseHandle(hInternet);
        return "Ошибка: не удалось создать файл";
    }

    char buf[8192];
    DWORD bytesRead = 0;
    while (InternetReadFile(hUrl, buf, sizeof(buf), &bytesRead) && bytesRead > 0)
        out.write(buf, bytesRead);

    out.close();
    InternetCloseHandle(hUrl);
    InternetCloseHandle(hInternet);

    return "Скачано: " + dest.string();
}

// ─────────────────────────────────────────────────────────────
// Копирование директории рекурсивно
// ─────────────────────────────────────────────────────────────

static bool copyRecursive(const fs::path& src, const fs::path& dst)
{
    try {
        fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        return true;
    }
    catch (...) {
        return false;
    }
}

// ─────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────

int main()
{
    // Установить UTF-8 для консоли (опционально)
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    hIn = GetStdHandle(STD_INPUT_HANDLE);

    // Режим ввода: без эха, без буферизации строк
    DWORD oldInMode;
    GetConsoleMode(hIn, &oldInMode);
    SetConsoleMode(hIn, ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT);

    // Убрать курсор
    showCursor(false);

    // Развернуть окно консоли
    HWND hwnd = GetConsoleWindow();
    if (hwnd) ShowWindow(hwnd, SW_MAXIMIZE);

    updateConsoleSize();

    Panel panels[2];
    panels[0].loadDir(fs::current_path());
    panels[1].loadDir(fs::current_path());
    int activePanel = 0;

    fs::path globalMark;
    bool     hasGlobalMark = false;
    std::string statusMsg = "Готово";

    while (true) {
        updateConsoleSize();
        clearScreen();

        int panelHeight = g_rows - 2;
        int halfW = g_cols / 2;

        drawPanel(panels[0], 0, halfW, 0, panelHeight, activePanel == 0, globalMark, hasGlobalMark);
        drawPanel(panels[1], halfW, g_cols - halfW, 0, panelHeight, activePanel == 1, globalMark, hasGlobalMark);

        drawStatusBar(g_rows - 2, statusMsg);
        drawHelpBar(g_rows - 1);

        statusMsg.clear();

        // Ждём нажатия клавиши
        INPUT_RECORD ir;
        DWORD read;
        ReadConsoleInputA(hIn, &ir, 1, &read);

        // Обновление размера окна
        if (ir.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            updateConsoleSize();
            statusMsg = "Готово";
            continue;
        }

        if (ir.EventType != KEY_EVENT || !ir.Event.KeyEvent.bKeyDown) continue;

        WORD vk = ir.Event.KeyEvent.wVirtualKeyCode;
        char c = ir.Event.KeyEvent.uChar.AsciiChar;
        Panel& ap = panels[activePanel];
        int visRows = panelHeight - 2;

        // ── Выход ──────────────────────────────────────────────
        if (c == 'q' || c == 'Q') break;

        // ── Переключение панели ────────────────────────────────
        else if (vk == VK_TAB) {
            activePanel ^= 1;
        }

        // ── Движение курсора ───────────────────────────────────
        else if (vk == VK_UP) {
            if (ap.cursor > 0) --ap.cursor;
            ap.adjustScroll(visRows);
        }
        else if (vk == VK_DOWN) {
            if (ap.cursor < (int)ap.entries.size() - 1) ++ap.cursor;
            ap.adjustScroll(visRows);
        }
        else if (vk == VK_PRIOR) {
            ap.cursor = std::max(0, ap.cursor - visRows);
            ap.adjustScroll(visRows);
        }
        else if (vk == VK_NEXT) {
            ap.cursor = std::min((int)ap.entries.size() - 1, ap.cursor + visRows);
            ap.adjustScroll(visRows);
        }

        // ── Enter — открыть директорию ─────────────────────────
        else if (vk == VK_RETURN) {
            Entry* e = ap.currentEntry();
            if (e && e->isDir) {
                if (!ap.loadDir(e->path))
                    statusMsg = ap.lastError;
            }
        }

        // ── Backspace — родительская директория ───────────────
        else if (vk == VK_BACK) {
            fs::path parent = ap.cwd.parent_path();
            if (parent != ap.cwd) {
                if (!ap.loadDir(parent))
                    statusMsg = ap.lastError;
            }
        }

        // ── Space — отметить элемент ───────────────────────────
        else if (vk == VK_SPACE) {
            Entry* e = ap.currentEntry();
            if (e) {
                if (hasGlobalMark && globalMark == e->path) {
                    hasGlobalMark = false;
                    globalMark.clear();
                    statusMsg = "Отметка снята";
                }
                else {
                    globalMark = e->path;
                    hasGlobalMark = true;
                    statusMsg = "Отмечено: " + e->path.filename().string();
                }
            }
        }

        // ── F1 — копировать ────────────────────────────────────
        else if (vk == VK_F1) {
            if (!hasGlobalMark) {
                statusMsg = "Нет отмеченного элемента";
            }
            else {
                fs::path dest = ap.cwd / globalMark.filename();
                bool ok = copyRecursive(globalMark, dest);
                ap.loadDir(ap.cwd);
                statusMsg = ok ? "Скопировано в " + ap.cwd.string()
                    : "Ошибка копирования";
            }
        }

        // ── F2 — переместить ───────────────────────────────────
        else if (vk == VK_F2) {
            if (!hasGlobalMark) {
                statusMsg = "Нет отмеченного элемента";
            }
            else {
                fs::path dest = ap.cwd / globalMark.filename();
                try {
                    fs::rename(globalMark, dest);
                    hasGlobalMark = false;
                    globalMark.clear();
                    panels[0].loadDir(panels[0].cwd);
                    panels[1].loadDir(panels[1].cwd);
                    statusMsg = "Перемещено в " + ap.cwd.string();
                }
                catch (...) {
                    statusMsg = "Ошибка перемещения";
                }
            }
        }

        // ── F3 — просмотр файла ────────────────────────────────
        else if (vk == VK_F3) {
            Entry* e = ap.currentEntry();
            if (e && !e->isDir) {
                viewFile(e->path);
            }
            else {
                statusMsg = "Выберите файл (не директорию)";
            }
        }

        // ── F4 — режим отображения ─────────────────────────────
        else if (vk == VK_F4) {
            ap.detailed = !ap.detailed;
            statusMsg = ap.detailed ? "Режим: подробный" : "Режим: краткий";
        }

        // ── F5 — скачать по HTTP ───────────────────────────────
        else if (vk == VK_F5) {
            std::string url = inputLine("HTTP URL: ");
            if (!url.empty()) {
                statusMsg = downloadHTTP(url, ap.cwd);
                ap.loadDir(ap.cwd);
            }
        }

        // ── Del — удалить ──────────────────────────────────────
        else if (vk == VK_DELETE) {
            Entry* e = ap.currentEntry();
            if (e) {
                if (confirm("Удалить \"" + e->path.filename().string() + "\"?")) {
                    bool ok = false;
                    try {
                        fs::remove_all(e->path);
                        if (hasGlobalMark && globalMark == e->path) {
                            hasGlobalMark = false; globalMark.clear();
                        }
                        ok = true;
                    }
                    catch (...) {}
                    ap.loadDir(ap.cwd);
                    statusMsg = ok ? "Удалено" : "Ошибка удаления";
                }
            }
        }

        if (statusMsg.empty()) statusMsg = "Готово";
    }

    // Восстановить режим консоли
    SetConsoleMode(hIn, oldInMode);
    showCursor(true);
    clearScreen();
    return 0;
}