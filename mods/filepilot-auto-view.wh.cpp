// ==WindhawkMod==
// @id              filepilot-auto-view
// @name            File Pilot Auto View
// @description     Switch File Pilot's view mode based on what a folder contains
// @version         0.1.0
// @author          yo-l-an
// @github          https://github.com/yo-l-an
// @license         MIT
// @include         FPilot.exe
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# File Pilot Auto View

Picks a view mode for each folder you open in File Pilot, based on the files
inside it. A folder that is mostly images opens in extra large icons, mostly
videos in large icons, mostly documents in details, and so on.

## How it works
- Hooks `SetWindowTextW` to read the current folder from the window title.
- Scans the folder's file names and matches their extensions against the
  templates in the settings.
- Presses the matching View hotkey from `FPilot-Config.json`
  (Ctrl+Shift+1..6 by default) inside the process. `GetKeyState` is hooked so
  File Pilot sees the modifiers without touching your real keyboard.
- For a custom icon zoom, switches to Extra large icons (100%) and sends
  Ctrl+wheel notches (5.5% each) with the cursor faked over the file list.
- Long paths are shortened in the title, so the full path is recovered from
  the folders File Pilot opens.

## Manual control
- Switching view with a View hotkey remembers that view for the folder, so
  the template no longer overrides it there. Turn off "Remember views picked
  with hotkeys" to forget all remembered views.
- Views picked with the mouse are not remembered; the template is re-applied
  the next time you open the folder.
- Add folders to "Excluded folders" to leave them alone.
- Disable the mod in Windhawk to turn detection off entirely.

## Limitations
Relies on File Pilot's window title format (`Name (Path) - File Pilot vX`) and
its View hotkey commands. A File Pilot update that changes either may break it.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- rules:
  - - name: Images
      $name: Name
    - extensions: jpg jpeg jfif png gif webp bmp tif tiff heic heif avif svg ico psd raw dng cr2 cr3 nef arw orf rw2
      $name: Extensions
      $description: Separated by spaces or commas, without dots
    - view: xl
      $name: View
      $options:
      - details: Details
      - tree: Tree
      - list: List
      - medium: Medium icons
      - large: Large icons
      - xl: Extra large icons
    - zoom: 45
      $name: Icon zoom (%)
      $description: >-
        For icon views. 0 keeps the view's own size. Other values start from
        Extra large icons (100%) and zoom out in Ctrl+wheel steps of 5.5%, so
        the result is the nearest of 100, 94.5, 89, ... 45, 39.5, ...
  - - name: Videos
    - extensions: mp4 mkv mov avi webm wmv m4v flv mpg mpeg ts mts m2ts 3gp
    - view: large
    - zoom: 45
  - - name: Documents
    - extensions: pdf doc docx xls xlsx ppt pptx odt ods odp rtf txt md csv epub
    - view: tree
    - zoom: 0
  $name: Templates
  $description: >-
    A template applies when its extensions make up at least the threshold share
    of a folder's files. When several qualify, the one with the most files wins.
- threshold: 60
  $name: Match threshold (%)
- mixedView: none
  $name: View for mixed folders
  $description: Used when files exist but no template reaches the threshold
  $options:
  - none: FilePilot saved default
  - details: Details
  - tree: Tree
  - list: List
  - medium: Medium icons
  - large: Large icons
  - xl: Extra large icons
- foldersOnlyView: none
  $name: View for folders without files
  $options:
  - none: FilePilot saved default
  - details: Details
  - tree: Tree
  - list: List
  - medium: Medium icons
  - large: Large icons
  - xl: Extra large icons
- maxScan: 2000
  $name: Maximum entries to scan
  $description: Larger folders are judged on their first entries only
- rememberManual: true
  $name: Remember views picked with hotkeys
  $description: >-
    Switching view with a View hotkey keeps that view for the folder instead of
    the template. Turning this off forgets every remembered view.
- excludedFolders:
  - C:\Windows
  $name: Excluded folders
  $description: These folders and their subfolders are never changed, for example C:\Windows
*/
// ==/WindhawkModSettings==

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <map>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

enum class View { None = -1, Details, Tree, List, Medium, Large, XL, Count };

struct Hotkey {
    UINT vk = 0;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
};

struct Layout {
    View view = View::None;
    int zoom = 0;  // percent, 0 keeps the view's own zoom
};

struct Rule {
    std::wstring name;
    std::unordered_set<std::wstring> extensions;
    Layout layout;
};

struct Settings {
    std::vector<Rule> rules;
    int threshold = 60;
    int maxScan = 2000;
    View mixedView = View::None;
    View foldersOnlyView = View::None;
    bool rememberManual = true;
    std::vector<std::wstring> excludedFolders;
};

constexpr wchar_t kWindowClass[] = L"File Pilot";
constexpr wchar_t kTitleSuffix[] = L" - File Pilot";
// Synthetic input is tagged so File Pilot sees fake modifiers for it only.
// Keystrokes: reserved lParam bits 25-28 hold the tag and the modifiers.
// Mouse move and wheel: an unused MK_* bit holds the tag, MK_CONTROL/MK_SHIFT
// the rest.
constexpr LPARAM kKeyMark = 1 << 25;
constexpr int kKeyModsShift = 26;
constexpr WORD kMouseMark = 0x0800;
constexpr int kModCtrl = 1, kModShift = 2, kModAlt = 4;
// Fake input lasts until File Pilot's next frame (its message loop drained
// the queue and came back), or this long at most.
constexpr ULONGLONG kFakeInputMaxMs = 1000;
// Wheel zoom is applied while rendering rather than when the message is
// handled, so its fake input is held at least this long.
constexpr ULONGLONG kWheelHoldMs = 200;
// Time for File Pilot to render a few frames with the fake cursor before the
// wheel arrives. Must stay below kWheelHoldMs.
constexpr DWORD kHoverSettleMs = 50;
constexpr DWORD kFakeInputPollMs = 10;
// Extra large icons is 100% zoom; each wheel notch (120) zooms 5.5%.
constexpr double kXLZoom = 100.0;
constexpr double kZoomPerNotch = 5.5;

std::mutex g_settingsMutex;
Settings g_settings;
Hotkey g_hotkeys[(int)View::Count];

std::mutex g_manualMutex;
std::map<std::wstring, View> g_manualViews;  // lowercased path -> view

std::atomic<HWND> g_mainWnd{nullptr};

// Directories File Pilot listed recently, newest last. Long titles show only
// the tail of the path, which is resolved against these.
constexpr size_t kMaxListedDirs = 64;
constexpr int kResolveAttempts = 20;
constexpr DWORD kResolveRetryMs = 50;
std::mutex g_listedMutex;
std::deque<std::wstring> g_listedDirs;
thread_local bool t_ownScan = false;

std::mutex g_navMutex;
std::condition_variable g_navCv;
std::wstring g_titlePath;     // as shown in the title, possibly a tail
std::wstring g_resolvedPath;  // full path of the current folder, once known
std::wstring g_lastResolvedPath;  // last known full path, kept across changes
std::wstring g_pendingPath;
bool g_hasPending = false;
bool g_stopWorker = false;
std::atomic<unsigned> g_navGeneration{0};
std::thread g_worker;

enum FakePhase { kFakeIdle, kFakePosted, kFakeDelivered, kFakeQueueDrained };
std::atomic<int> g_fakePhase{kFakeIdle};
std::atomic<int> g_fakeMods{0};
std::atomic<ULONGLONG> g_fakeSince{0};
std::atomic<ULONGLONG> g_fakeHoldMs{0};
// While a tagged wheel message is handled, the cursor appears to be over the
// middle of the window so the zoom lands on the file list.
std::atomic<bool> g_fakeCursor{false};
std::atomic<DWORD> g_mainThreadId{0};

using SetWindowTextW_t = decltype(&SetWindowTextW);
using GetKeyState_t = decltype(&GetKeyState);
using PeekMessageW_t = decltype(&PeekMessageW);
using TranslateMessage_t = decltype(&TranslateMessage);
using FindFirstFileW_t = decltype(&FindFirstFileW);
using FindFirstFileExW_t = decltype(&FindFirstFileExW);
using CreateFileW_t = decltype(&CreateFileW);
using GetCursorPos_t = decltype(&GetCursorPos);
using WindowFromPoint_t = decltype(&WindowFromPoint);
GetCursorPos_t GetCursorPos_Original;
WindowFromPoint_t WindowFromPoint_Original;
SetWindowTextW_t SetWindowTextW_Original;
CreateFileW_t CreateFileW_Original;
FindFirstFileW_t FindFirstFileW_Original;
FindFirstFileExW_t FindFirstFileExW_Original;
GetKeyState_t GetKeyState_Original;
PeekMessageW_t PeekMessageW_Original;
TranslateMessage_t TranslateMessage_Original;

// ---------------------------------------------------------------------------
// Helpers

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return (wchar_t)std::towlower(c); });
    return s;
}

std::wstring TakeString(PCWSTR value) {
    std::wstring result = value ? value : L"";
    Wh_FreeStringSetting(value);
    return result;
}

std::wstring NormalizePath(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    while (path.size() > 3 && path.back() == L'\\') {
        path.pop_back();
    }
    return ToLower(path);
}

const wchar_t* kViewNames[] = {L"details", L"tree", L"list",
                               L"medium",  L"large", L"xl"};

View ParseView(const std::wstring& name) {
    for (int i = 0; i < (int)View::Count; i++) {
        if (name == kViewNames[i]) {
            return (View)i;
        }
    }
    return View::None;
}

PCWSTR ViewName(View view) {
    return view == View::None ? L"none" : kViewNames[(int)view];
}

bool IsMainWindow(HWND hwnd) {
    if (hwnd == g_mainWnd) {
        return true;
    }
    wchar_t className[64];
    if (!GetClassNameW(hwnd, className, ARRAYSIZE(className)) ||
        wcscmp(className, kWindowClass) != 0 || GetParent(hwnd)) {
        return false;
    }
    g_mainWnd = hwnd;
    g_mainThreadId = GetWindowThreadProcessId(hwnd, nullptr);
    return true;
}

// ---------------------------------------------------------------------------
// Settings and File Pilot hotkeys

void LoadSettings() {
    Settings settings;
    for (int i = 0;; i++) {
        std::wstring name = TakeString(Wh_GetStringSetting(L"rules[%d].name", i));
        std::wstring extensions =
            TakeString(Wh_GetStringSetting(L"rules[%d].extensions", i));
        std::wstring view = TakeString(Wh_GetStringSetting(L"rules[%d].view", i));
        if (name.empty() && extensions.empty() && view.empty()) {
            break;
        }

        int zoom = std::clamp(Wh_GetIntSetting(L"rules[%d].zoom", i), 0, 100);
        Rule rule{name, {}, {ParseView(view), zoom}};
        std::wstring token;
        for (wchar_t c : ToLower(extensions) + L" ") {
            if (c == L' ' || c == L',' || c == L';' || c == L'.') {
                if (!token.empty()) {
                    rule.extensions.insert(token);
                }
                token.clear();
            } else {
                token += c;
            }
        }
        if (rule.layout.view != View::None && !rule.extensions.empty()) {
            settings.rules.push_back(std::move(rule));
        }
    }

    settings.threshold = std::clamp(Wh_GetIntSetting(L"threshold"), 1, 100);
    settings.maxScan = std::clamp(Wh_GetIntSetting(L"maxScan"), 10, 100000);
    settings.mixedView = ParseView(TakeString(Wh_GetStringSetting(L"mixedView")));
    settings.foldersOnlyView =
        ParseView(TakeString(Wh_GetStringSetting(L"foldersOnlyView")));
    settings.rememberManual = Wh_GetIntSetting(L"rememberManual");

    // Missing array items read as empty strings, so stop at the first one.
    for (int i = 0;; i++) {
        std::wstring folder =
            TakeString(Wh_GetStringSetting(L"excludedFolders[%d]", i));
        if (folder.empty()) {
            break;
        }
        settings.excludedFolders.push_back(NormalizePath(folder));
    }

    std::lock_guard lock(g_settingsMutex);
    g_settings = std::move(settings);
}

Hotkey ParseHotkey(const std::string& text) {
    Hotkey hotkey;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('+', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string token = text.substr(start, end - start);
        if (token == "Ctrl") {
            hotkey.ctrl = true;
        } else if (token == "Shift") {
            hotkey.shift = true;
        } else if (token == "Alt") {
            hotkey.alt = true;
        } else if (token.size() == 1 && std::isalnum((unsigned char)token[0])) {
            hotkey.vk = (UINT)std::toupper((unsigned char)token[0]);
        } else if (token.size() >= 2 && token[0] == 'F' &&
                   std::isdigit((unsigned char)token[1])) {
            int n = std::atoi(token.c_str() + 1);
            if (n >= 1 && n <= 24) {
                hotkey.vk = VK_F1 + n - 1;
            }
        }
        start = end + 1;
    }
    return hotkey;
}

void LoadHotkeys() {
    for (int i = 0; i < (int)View::Count; i++) {
        g_hotkeys[i] = {(UINT)('1' + i), true, true, false};
    }

    wchar_t appData[MAX_PATH];
    if (!GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH)) {
        return;
    }
    std::wstring configPath =
        std::wstring(appData) + L"\\Voidstar\\FilePilot\\FPilot-Config.json";
    HANDLE file = CreateFileW(configPath.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Wh_Log(L"Can't open %s, using default hotkeys", configPath.c_str());
        return;
    }
    std::string config(GetFileSize(file, nullptr), '\0');
    DWORD read = 0;
    ReadFile(file, config.data(), (DWORD)config.size(), &read, nullptr);
    CloseHandle(file);
    config.resize(read);

    static const std::pair<const char*, View> kCommands[] = {
        {"Details", View::Details},         {"Tree", View::Tree},
        {"List", View::List},               {"MediumIcons", View::Medium},
        {"LargeIcons", View::Large},        {"XLIcons", View::XL}};
    std::regex pattern(
        R"re("Keys"\s*:\s*\[\s*"([^"]+)"[^\]]*\]\s*,\s*"Command"\s*:\s*"HK_View(\w+)")re");
    for (std::sregex_iterator it(config.begin(), config.end(), pattern), end;
         it != end; ++it) {
        for (auto& [command, view] : kCommands) {
            if ((*it)[2] == command) {
                Hotkey hotkey = ParseHotkey((*it)[1]);
                if (hotkey.vk) {
                    g_hotkeys[(int)view] = hotkey;
                } else {
                    Wh_Log(L"Unsupported hotkey for HK_View%S", command);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Remembered manual views

void SaveManualViews() {
    std::wstring data;
    for (auto& [path, view] : g_manualViews) {
        data += ViewName(view);
        data += L'|';
        data += path;
        data += L'\n';
    }
    Wh_SetStringValue(L"manualViews", data.c_str());
}

void LoadManualViews() {
    std::vector<wchar_t> buffer(1 << 18);
    Wh_GetStringValue(L"manualViews", buffer.data(), buffer.size());
    std::wstring data = buffer.data();

    std::lock_guard lock(g_manualMutex);
    g_manualViews.clear();
    size_t start = 0;
    while (start < data.size()) {
        size_t end = data.find(L'\n', start);
        if (end == std::wstring::npos) {
            end = data.size();
        }
        std::wstring line = data.substr(start, end - start);
        size_t separator = line.find(L'|');
        if (separator != std::wstring::npos) {
            View view = ParseView(line.substr(0, separator));
            if (view != View::None) {
                g_manualViews[line.substr(separator + 1)] = view;
            }
        }
        start = end + 1;
    }
}

void RecordManualView(View view) {
    {
        std::lock_guard lock(g_settingsMutex);
        if (!g_settings.rememberManual) {
            return;
        }
    }
    std::wstring path;
    {
        std::lock_guard lock(g_navMutex);
        path = g_resolvedPath;
    }
    if (path.empty()) {
        return;
    }
    Wh_Log(L"Remembering %s for %s", ViewName(view), path.c_str());
    std::lock_guard lock(g_manualMutex);
    g_manualViews[NormalizePath(path)] = view;
    SaveManualViews();
}

// ---------------------------------------------------------------------------
// Folder classification

bool IsExcluded(const std::wstring& normalizedPath, const Settings& settings) {
    for (auto& folder : settings.excludedFolders) {
        if (normalizedPath.compare(0, folder.size(), folder) == 0 &&
            (normalizedPath.size() == folder.size() ||
             normalizedPath[folder.size()] == L'\\' || folder.back() == L'\\')) {
            return true;
        }
    }
    return false;
}

Layout ClassifyFolder(const std::wstring& path, const Settings& settings) {
    std::wstring pattern = path;
    if (pattern.back() != L'\\') {
        pattern += L'\\';
    }
    pattern += L'*';

    WIN32_FIND_DATAW data;
    t_ownScan = true;
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
                                   FindExSearchNameMatch, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
    t_ownScan = false;
    if (find == INVALID_HANDLE_VALUE) {
        return {};
    }

    std::vector<int> counts(settings.rules.size());
    int files = 0, folders = 0;
    do {
        if (wcscmp(data.cFileName, L".") == 0 ||
            wcscmp(data.cFileName, L"..") == 0) {
            continue;
        }
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            folders++;
            continue;
        }
        if (data.dwFileAttributes &
            (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) {
            continue;
        }
        files++;
        const wchar_t* dot = wcsrchr(data.cFileName, L'.');
        if (!dot) {
            continue;
        }
        std::wstring extension = ToLower(dot + 1);
        for (size_t i = 0; i < settings.rules.size(); i++) {
            if (settings.rules[i].extensions.count(extension)) {
                counts[i]++;
                break;
            }
        }
    } while (files + folders < settings.maxScan && FindNextFileW(find, &data));
    FindClose(find);

    if (files == 0) {
        return {folders ? settings.foldersOnlyView : View::None};
    }
    size_t best = std::max_element(counts.begin(), counts.end()) - counts.begin();
    if (best < counts.size() &&
        counts[best] * 100 >= settings.threshold * files) {
        Wh_Log(L"%s: %d/%d files match %s", path.c_str(), counts[best], files,
               settings.rules[best].name.c_str());
        return settings.rules[best].layout;
    }
    Wh_Log(L"%s: mixed (%d files)", path.c_str(), files);
    return {settings.mixedView};
}

// ---------------------------------------------------------------------------
// Synthetic hotkey presses

LPARAM KeyLParam(const Hotkey& hotkey, bool up) {
    int mods = (hotkey.ctrl ? kModCtrl : 0) | (hotkey.shift ? kModShift : 0) |
               (hotkey.alt ? kModAlt : 0);
    LPARAM lParam = 1 |
                    ((LPARAM)MapVirtualKeyW(hotkey.vk, MAPVK_VK_TO_VSC) << 16) |
                    kKeyMark | ((LPARAM)mods << kKeyModsShift);
    if (hotkey.alt) {
        lParam |= 1 << 29;
    }
    if (up) {
        lParam |= 0xC0000000;
    }
    return lParam;
}

void BeginFakeInput() {
    g_fakeSince = GetTickCount64();
    g_fakePhase = kFakePosted;
}

// Waits until File Pilot has run a frame with the tagged messages, so
// whatever comes next gets its own fake modifiers.
void WaitForFakeInput() {
    while (g_fakePhase != kFakeIdle) {
        if (GetTickCount64() - g_fakeSince > kFakeInputMaxMs) {
            Wh_Log(L"File Pilot didn't handle fake input in time");
            g_fakePhase = kFakeIdle;
            break;
        }
        Sleep(kFakeInputPollMs);
    }
}

void PostTagged(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    BeginFakeInput();
    PostMessageW(hwnd, message, wParam, lParam);
    WaitForFakeInput();
}

void PressHotkey(HWND hwnd, const Hotkey& hotkey) {
    UINT down = hotkey.alt ? WM_SYSKEYDOWN : WM_KEYDOWN;
    UINT up = hotkey.alt ? WM_SYSKEYUP : WM_KEYUP;
    PostTagged(hwnd, down, hotkey.vk, KeyLParam(hotkey, false));
    PostTagged(hwnd, up, hotkey.vk, KeyLParam(hotkey, true));
}

POINT FakeCursorPos() {
    HWND hwnd = g_mainWnd;
    RECT rect{};
    GetClientRect(hwnd, &rect);
    POINT point{(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
    ClientToScreen(hwnd, &point);
    return point;
}

// Zooms like Ctrl+wheel, starting from Extra large icons (100%). File Pilot
// zooms a fixed step per wheel message whatever its delta, so it gets one
// message per notch.
void SetIconZoom(HWND hwnd, int zoom) {
    PressHotkey(hwnd, g_hotkeys[(int)View::XL]);
    int notches = (int)std::lround((kXLZoom - zoom) / kZoomPerNotch);
    if (notches <= 0) {
        return;
    }
    // File Pilot hit-tests the wheel against the previous frame's cursor, so
    // a move to the file list gets a few frames first. The fake cursor must
    // not lapse in between, or the wheel lands outside the list.
    POINT point = FakeCursorPos();
    POINT clientPoint = point;
    ScreenToClient(hwnd, &clientPoint);
    BeginFakeInput();
    PostMessageW(hwnd, WM_MOUSEMOVE, kMouseMark,
                 MAKELPARAM(clientPoint.x, clientPoint.y));
    Sleep(kHoverSettleMs);
    for (int i = 0; i < notches; i++) {
        PostMessageW(hwnd, WM_MOUSEWHEEL,
                     MAKEWPARAM(MK_CONTROL | kMouseMark, (SHORT)-WHEEL_DELTA),
                     MAKELPARAM(point.x, point.y));
    }
    WaitForFakeInput();
}

void ApplyLayout(HWND hwnd, const Layout& layout) {
    bool isIconView = layout.view == View::Medium ||
                      layout.view == View::Large || layout.view == View::XL;
    if (isIconView && layout.zoom > 0) {
        SetIconZoom(hwnd, layout.zoom);
    } else {
        PressHotkey(hwnd, g_hotkeys[(int)layout.view]);
    }
}

// ---------------------------------------------------------------------------
// Current folder detection

bool LooksLikePath(const std::wstring& s) {
    return (s.size() >= 2 && std::iswalpha(s[0]) && s[1] == L':') ||
           s.rfind(L"\\\\", 0) == 0;
}

std::wstring LastComponent(std::wstring path) {
    while (path.size() > 1 && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// "TOOLSDEV (F:\_WORK\TOOLSDEV) - File Pilot v0.8.5" -> "F:\_WORK\TOOLSDEV".
// Long paths lose their leading folders: "photos (Local\Temp\x\photos) - ..."
// -> "Local\Temp\x\photos".
std::wstring PathFromTitle(const std::wstring& title) {
    size_t suffix = title.rfind(kTitleSuffix);
    if (suffix == std::wstring::npos || suffix < 2 || title[suffix - 1] != L')') {
        return {};
    }
    std::wstring head = title.substr(0, suffix - 1);
    std::wstring fallback;
    for (size_t open = head.find(L" ("); open != std::wstring::npos;
         open = head.find(L" (", open + 1)) {
        std::wstring path = head.substr(open + 2);
        if (ToLower(LastComponent(path)) == ToLower(head.substr(0, open))) {
            return path;
        }
        if (fallback.empty() && LooksLikePath(path)) {
            fallback = path;
        }
    }
    return fallback;
}

// Records "C:\dir" from a listing pattern such as "\\?\C:\dir\*".
void RecordListedDir(LPCWSTR pattern) {
    if (!pattern || t_ownScan) {
        return;
    }
    std::wstring dir = pattern;
    if (dir.find_first_of(L"*?") != std::wstring::npos) {
        size_t slash = dir.find_last_of(L"\\/");
        if (slash == std::wstring::npos) {
            return;
        }
        dir.resize(slash + 1);
    }
    std::replace(dir.begin(), dir.end(), L'/', L'\\');
    if (dir.back() != L'\\') {
        dir += L'\\';
    }
    if (dir.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        dir = L"\\\\" + dir.substr(8);
    } else if (dir.rfind(L"\\\\?\\", 0) == 0) {
        dir = dir.substr(4);
    }
    if (dir.size() > 3) {
        dir.pop_back();  // keep "C:\" for drive roots
    }

    if (!LooksLikePath(dir)) {
        return;
    }

    std::lock_guard lock(g_listedMutex);
    if (!g_listedDirs.empty() && g_listedDirs.back() == dir) {
        return;
    }
    g_listedDirs.push_back(std::move(dir));
    if (g_listedDirs.size() > kMaxListedDirs) {
        g_listedDirs.pop_front();
    }
}

std::wstring FindListedDir(const std::wstring& tail) {
    std::wstring needle = L"\\" + NormalizePath(tail);
    std::lock_guard lock(g_listedMutex);
    for (auto it = g_listedDirs.rbegin(); it != g_listedDirs.rend(); ++it) {
        std::wstring dir = ToLower(*it);
        if (dir.size() > needle.size() &&
            dir.compare(dir.size() - needle.size(), needle.size(), needle) == 0) {
            return *it;
        }
    }
    return {};
}

bool IsDirectory(const std::wstring& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// Navigation mostly stays in one tree, so try "<ancestor of the last folder>\
// <tail>" from the deepest ancestor up.
std::wstring FindUnderLastFolder(const std::wstring& tail,
                                 std::wstring ancestor) {
    while (!ancestor.empty()) {
        std::wstring candidate = ancestor;
        if (candidate.back() != L'\\') {
            candidate += L'\\';
        }
        candidate += tail;
        if (IsDirectory(candidate)) {
            return candidate;
        }
        size_t slash = ancestor.find_last_of(L'\\', ancestor.size() - 2);
        if (slash == std::wstring::npos) {
            break;
        }
        ancestor.resize(slash + 1);
    }
    return {};
}

// Turns a title path into a full path, giving File Pilot a moment to list
// the folder when the title only shows its tail.
std::wstring ResolveTitlePath(const std::wstring& titlePath,
                              unsigned generation) {
    if (LooksLikePath(titlePath)) {
        return titlePath;
    }
    std::wstring lastFolder;
    {
        std::lock_guard lock(g_navMutex);
        lastFolder = g_lastResolvedPath;
    }
    std::wstring path = FindUnderLastFolder(titlePath, lastFolder);
    for (int i = 0; path.empty() && i < kResolveAttempts &&
                    generation == g_navGeneration;
         i++) {
        path = FindListedDir(titlePath);
        if (path.empty()) {
            Sleep(kResolveRetryMs);
        }
    }
    if (!path.empty()) {
        return path;
    }
    Wh_Log(L"Can't resolve %s", titlePath.c_str());
    return {};
}

void OnTitleChanged(PCWSTR title, bool apply) {
    std::wstring path = PathFromTitle(title);
    std::lock_guard lock(g_navMutex);
    if (path == g_titlePath) {
        return;
    }
    g_titlePath = path;
    g_resolvedPath = LooksLikePath(path) ? path : L"";
    if (!g_resolvedPath.empty()) {
        g_lastResolvedPath = g_resolvedPath;
    }
    g_navGeneration++;
    if (apply && !path.empty()) {
        g_pendingPath = path;
        g_hasPending = true;
        g_navCv.notify_one();
    }
}

void ApplyView(const std::wstring& titlePath, unsigned generation) {
    std::wstring path = ResolveTitlePath(titlePath, generation);
    if (path.empty()) {
        return;
    }
    {
        std::lock_guard lock(g_navMutex);
        if (generation != g_navGeneration) {
            return;
        }
        g_resolvedPath = path;
        g_lastResolvedPath = path;
    }

    Settings settings;
    {
        std::lock_guard lock(g_settingsMutex);
        settings = g_settings;
    }
    std::wstring normalized = NormalizePath(path);
    if (IsExcluded(normalized, settings)) {
        return;
    }

    Layout layout;
    if (settings.rememberManual) {
        std::lock_guard lock(g_manualMutex);
        auto it = g_manualViews.find(normalized);
        if (it != g_manualViews.end()) {
            layout.view = it->second;
            Wh_Log(L"%s: remembered %s", path.c_str(), ViewName(layout.view));
        }
    }
    if (layout.view == View::None) {
        layout = ClassifyFolder(path, settings);
    }

    HWND hwnd = g_mainWnd;
    if (layout.view == View::None || !hwnd || generation != g_navGeneration) {
        Wh_Log(L"%s: leaving view as is", path.c_str());
        return;
    }
    Wh_Log(L"%s: switching to %s, zoom %d", path.c_str(), ViewName(layout.view),
           layout.zoom);
    ApplyLayout(hwnd, layout);
}

void WorkerLoop() {
    std::unique_lock lock(g_navMutex);
    while (true) {
        g_navCv.wait(lock, [] { return g_stopWorker || g_hasPending; });
        if (g_stopWorker) {
            return;
        }
        std::wstring path = g_pendingPath;
        g_hasPending = false;
        unsigned generation = g_navGeneration;
        lock.unlock();
        ApplyView(path, generation);
        lock.lock();
    }
}

// ---------------------------------------------------------------------------
// Hooks

BOOL WINAPI SetWindowTextW_Hook(HWND hwnd, LPCWSTR text) {
    BOOL result = SetWindowTextW_Original(hwnd, text);
    if (text && IsMainWindow(hwnd)) {
        OnTitleChanged(text, true);
    }
    return result;
}

HANDLE WINAPI FindFirstFileW_Hook(LPCWSTR fileName,
                                  LPWIN32_FIND_DATAW findData) {
    HANDLE find = FindFirstFileW_Original(fileName, findData);
    if (find != INVALID_HANDLE_VALUE) {
        RecordListedDir(fileName);
    }
    return find;
}

// Folders are opened with backup semantics to be listed or watched.
HANDLE WINAPI CreateFileW_Hook(LPCWSTR fileName,
                               DWORD desiredAccess,
                               DWORD shareMode,
                               LPSECURITY_ATTRIBUTES securityAttributes,
                               DWORD creationDisposition,
                               DWORD flagsAndAttributes,
                               HANDLE templateFile) {
    HANDLE file = CreateFileW_Original(fileName, desiredAccess, shareMode,
                                       securityAttributes, creationDisposition,
                                       flagsAndAttributes, templateFile);
    if (file != INVALID_HANDLE_VALUE &&
        (flagsAndAttributes & FILE_FLAG_BACKUP_SEMANTICS)) {
        RecordListedDir(fileName);
    }
    return file;
}

HANDLE WINAPI FindFirstFileExW_Hook(LPCWSTR fileName,
                                    FINDEX_INFO_LEVELS infoLevel,
                                    LPVOID findData,
                                    FINDEX_SEARCH_OPS searchOp,
                                    LPVOID searchFilter,
                                    DWORD additionalFlags) {
    HANDLE find = FindFirstFileExW_Original(fileName, infoLevel, findData,
                                            searchOp, searchFilter,
                                            additionalFlags);
    if (find != INVALID_HANDLE_VALUE) {
        RecordListedDir(fileName);
    }
    return find;
}

bool IsFakedModifier(int vk, int mods) {
    switch (vk) {
        case VK_CONTROL:
        case VK_LCONTROL:
            return mods & kModCtrl;
        case VK_SHIFT:
        case VK_LSHIFT:
            return mods & kModShift;
        case VK_MENU:
        case VK_LMENU:
            return mods & kModAlt;
    }
    return false;
}

void EndFakeInput() {
    g_fakeMods = 0;
    g_fakeCursor = false;
    g_fakePhase = kFakeIdle;
}

bool IsFakingInput() {
    int phase = g_fakePhase;
    if (phase != kFakeDelivered && phase != kFakeQueueDrained) {
        return false;
    }
    if (GetTickCount64() - g_fakeSince > kFakeInputMaxMs) {
        EndFakeInput();
        return false;
    }
    return true;
}

SHORT WINAPI GetKeyState_Hook(int vk) {
    SHORT state = GetKeyState_Original(vk);
    int mods = g_fakeMods;
    if (mods && IsFakingInput() && IsFakedModifier(vk, mods)) {
        state |= (SHORT)0x8000;
    }
    return state;
}

BOOL WINAPI GetCursorPos_Hook(LPPOINT point) {
    if (point && g_fakeCursor && IsFakingInput()) {
        *point = FakeCursorPos();
        return TRUE;
    }
    return GetCursorPos_Original(point);
}

HWND WINAPI WindowFromPoint_Hook(POINT point) {
    if (g_fakeCursor && IsFakingInput()) {
        return g_mainWnd;
    }
    return WindowFromPoint_Original(point);
}

bool IsRealKeyDown(int vk) {
    return GetKeyState_Original(vk) & 0x8000;
}

void FakeInput(int mods, bool cursor, ULONGLONG holdMs) {
    g_fakeSince = GetTickCount64();
    g_fakeHoldMs = holdMs;
    g_fakeMods = mods;
    g_fakeCursor = cursor;
    g_fakePhase = kFakeDelivered;
}

bool IsTaggedKey(const MSG& msg) {
    return msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST &&
           (msg.lParam & kKeyMark);
}

void InspectInputMessage(const MSG& msg) {
    if (msg.message == WM_MOUSEWHEEL || msg.message == WM_MOUSEMOVE) {
        WORD keys = GET_KEYSTATE_WPARAM(msg.wParam);
        if (keys & kMouseMark) {
            FakeInput((keys & MK_CONTROL ? kModCtrl : 0) |
                          (keys & MK_SHIFT ? kModShift : 0),
                      true, kWheelHoldMs);
        }
        return;
    }

    bool isDown = msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN;
    bool isUp = msg.message == WM_KEYUP || msg.message == WM_SYSKEYUP;
    if (!isDown && !isUp) {
        return;
    }

    if (IsTaggedKey(msg)) {
        FakeInput((msg.lParam >> kKeyModsShift) & 7, false, 0);
        return;
    }

    // A real View hotkey press: remember the user's choice for this folder.
    if (!isDown || (msg.lParam & 0x40000000)) {
        return;
    }
    for (int i = 0; i < (int)View::Count; i++) {
        const Hotkey& hotkey = g_hotkeys[i];
        if (hotkey.vk == msg.wParam && hotkey.ctrl == IsRealKeyDown(VK_CONTROL) &&
            hotkey.shift == IsRealKeyDown(VK_SHIFT) &&
            hotkey.alt == IsRealKeyDown(VK_MENU)) {
            RecordManualView((View)i);
            return;
        }
    }
}

// File Pilot drains its queue, runs a frame, then peeks again. Fake input
// for a tagged message ends when that frame is done.
BOOL WINAPI PeekMessageW_Hook(LPMSG msg, HWND hwnd, UINT filterMin,
                              UINT filterMax, UINT removeMsg) {
    bool isMainThread = GetCurrentThreadId() == g_mainThreadId;
    if (isMainThread && g_fakePhase == kFakeQueueDrained &&
        GetTickCount64() - g_fakeSince >= g_fakeHoldMs) {
        EndFakeInput();
    }
    BOOL result =
        PeekMessageW_Original(msg, hwnd, filterMin, filterMax, removeMsg);
    if (result && (removeMsg & PM_REMOVE) && msg->hwnd &&
        msg->hwnd == g_mainWnd) {
        InspectInputMessage(*msg);
    } else if (!result && isMainThread && g_fakePhase == kFakeDelivered) {
        g_fakePhase = kFakeQueueDrained;
    }
    return result;
}

BOOL WINAPI TranslateMessage_Hook(const MSG* msg) {
    // Our keys must not turn into typed characters (e.g. into the filter box).
    if (msg && IsTaggedKey(*msg)) {
        return TRUE;
    }
    return TranslateMessage_Original(msg);
}

// ---------------------------------------------------------------------------
// Mod lifecycle

BOOL CALLBACK FindMainWindow(HWND hwnd, LPARAM) {
    DWORD processId;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId != GetCurrentProcessId() || !IsMainWindow(hwnd)) {
        return TRUE;
    }
    wchar_t title[1024];
    if (GetWindowTextW(hwnd, title, ARRAYSIZE(title))) {
        OnTitleChanged(title, false);
    }
    return FALSE;
}

template <typename T>
bool HookExport(HMODULE module, const char* name, void* hook, T* original) {
    void* target = module ? (void*)GetProcAddress(module, name) : nullptr;
    if (!target) {
        Wh_Log(L"Missing export %S", name);
        return false;
    }
    return Wh_SetFunctionHook(target, hook, (void**)original);
}

BOOL Wh_ModInit() {
    LoadSettings();
    LoadHotkeys();
    LoadManualViews();

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
    if (!HookExport(user32, "SetWindowTextW", (void*)SetWindowTextW_Hook,
                    &SetWindowTextW_Original) ||
        !HookExport(user32, "GetKeyState", (void*)GetKeyState_Hook,
                    &GetKeyState_Original) ||
        !HookExport(user32, "PeekMessageW", (void*)PeekMessageW_Hook,
                    &PeekMessageW_Original) ||
        !HookExport(user32, "TranslateMessage", (void*)TranslateMessage_Hook,
                    &TranslateMessage_Original) ||
        !HookExport(user32, "GetCursorPos", (void*)GetCursorPos_Hook,
                    &GetCursorPos_Original) ||
        !HookExport(user32, "WindowFromPoint", (void*)WindowFromPoint_Hook,
                    &WindowFromPoint_Original) ||
        !HookExport(kernelBase, "FindFirstFileW", (void*)FindFirstFileW_Hook,
                    &FindFirstFileW_Original) ||
        !HookExport(kernelBase, "FindFirstFileExW", (void*)FindFirstFileExW_Hook,
                    &FindFirstFileExW_Original) ||
        !HookExport(kernelBase, "CreateFileW", (void*)CreateFileW_Hook,
                    &CreateFileW_Original)) {
        return FALSE;
    }

    g_worker = std::thread(WorkerLoop);
    return TRUE;
}

void Wh_ModAfterInit() {
    EnumWindows(FindMainWindow, 0);
    std::lock_guard lock(g_navMutex);
    Wh_Log(L"Main window %p, folder %s", (HWND)g_mainWnd, g_titlePath.c_str());
}

void Wh_ModUninit() {
    {
        std::lock_guard lock(g_navMutex);
        g_stopWorker = true;
    }
    g_navCv.notify_one();
    if (g_worker.joinable()) {
        g_worker.join();
    }
    EndFakeInput();
}

void Wh_ModSettingsChanged() {
    LoadSettings();
    LoadHotkeys();
    bool rememberManual;
    {
        std::lock_guard lock(g_settingsMutex);
        rememberManual = g_settings.rememberManual;
    }
    if (!rememberManual) {
        std::lock_guard lock(g_manualMutex);
        g_manualViews.clear();
        SaveManualViews();
    }
}
