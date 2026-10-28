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

![File Pilot Auto View demo](https://raw.githubusercontent.com/yo-l-an/assets/main/windhawk-mods/filepilot-auto-view/demo.gif)

Opens each folder in [File Pilot](https://filepilot.tech) with a view that
suits what's inside, like Windows Explorer does for picture folders.

## How folders are matched
Each folder you open is matched against templates, by file type:

| Template | View |
|---|---|
| Images | Medium icons at 45% |
| Videos | Medium icons at 45% |
| Documents | Tree |

- A template wins when its file types make up at least 60% of the folder's
  files. If several qualify, the one with the most files wins.
- Mixed folders, and folders holding only subfolders, keep the view File
  Pilot saved for them.
- The mod reads the first 2000 items of a folder, which keeps large folders
  quick to open.

All of this is adjustable in Settings: each template's file types, view, zoom
and threshold, extra templates of your own, the number of items read, and
folders to leave alone.

### Your choices come first
- A view you pick with a File Pilot View shortcut sticks to that folder, and
  its template stops applying there. Turn off "Remember views picked with
  hotkeys" to clear them all.
- A view you pick with the mouse lasts until you leave the folder; the
  template applies again on your next visit.

## Limitations
- The first icon zoom after File Pilot starts, and sometimes the first in a
  new tab, briefly flashes File Pilot's view menu in the bottom-right corner.
  Later folders switch without it.
- When the mod can't set the zoom directly, it zooms the way Ctrl+scroll does,
  in 5.5% steps, so 45% becomes 44%.

## Compatibility
Tested with File Pilot 0.8.5 on Windows 10.

File Pilot has no plugin system, so the mod relies on details that can change
between versions: the window title, the View shortcuts and how File Pilot
stores zoom. Your shortcuts are read from File Pilot's own settings, so
remapping them is fine. If an update changes the rest, folders stop switching
views or icon zoom becomes approximate; turning the mod off in Windhawk
brings File Pilot back to normal. Check for a mod update when that happens.
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
    - view: medium
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
        For icon views: 30 (Medium) to 100 (Extra large). 0 keeps the view's
        own size. If File Pilot's view can't be found in memory, the zoom is
        rounded to Ctrl+wheel steps of 5.5% from 0.
    - threshold: 60
      $name: Match threshold (%)
      $description: Share of the folder's files these extensions must reach
  - - name: Videos
    - extensions: mp4 mkv mov avi webm wmv m4v flv mpg mpeg ts mts m2ts 3gp
    - view: medium
    - zoom: 45
    - threshold: 60
  - - name: Documents
    - extensions: pdf doc docx xls xlsx ppt pptx odt ods odp rtf txt md csv epub
    - view: tree
    - zoom: 0
    - threshold: 60
  $name: Templates
  $description: >-
    A template applies when its extensions make up at least its threshold share
    of a folder's files. When several qualify, the one with the most files wins.
- mixedView: none
  $name: View for mixed folders
  $description: Used when files exist but no template reaches its threshold
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

// Layout of this file, from the bottom up:
//   filepilot    every assumption about File Pilot; check here after an update
//   text         string and path helpers
//   Config       immutable settings snapshot, swapped on settings changes
//   ManualViews  views picked with hotkeys, persisted per folder
//   main_window  File Pilot's window and UI thread
//   fake_input   tagged messages and the modifier/cursor state they fake
//   synth        posting hotkeys and wheel zoom to File Pilot
//   ViewRecords  finding and writing File Pilot's view records in memory
//   layouts      classifying a folder and applying its layout
//   DirectoryLog, Navigator   which folder is shown, and the worker thread
//   hooks, lifecycle

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

enum class View { None = -1, Details, Tree, List, Medium, Large, XL, Count };
constexpr int kViewCount = static_cast<int>(View::Count);

// ===========================================================================
// File Pilot internals

namespace filepilot {

constexpr wchar_t kWindowClass[] = L"File Pilot";
// Titles read "<name> (<path>) - File Pilot v0.8.5". Long paths lose their
// leading folders: "photos (Local\Temp\x\photos) - File Pilot v0.8.5".
constexpr wchar_t kTitleSuffix[] = L" - File Pilot";
constexpr wchar_t kConfigFile[] =
    L"%APPDATA%\\Voidstar\\FilePilot\\FPilot-Config.json";

// "HK_View<command>" hotkey commands in the config file, indexed by View.
// Unbound ones default to Ctrl+Shift+1..6 in the same order.
constexpr const char* kViewCommands[kViewCount] = {
    "Details", "Tree", "List", "MediumIcons", "LargeIcons", "XLIcons"};

// The zoom each View hotkey sets. Zoom alone picks the view, so values in
// between are valid too: icons go from Medium (0.3) to Extra large (1.0).
constexpr float kViewZoom[kViewCount] = {0.0f, 0.1f, 0.2f, 0.3f, 0.65f, 1.0f};

// Percent per Ctrl+wheel message, whatever the message's delta.
constexpr double kZoomPerWheelMessage = 5.5;

// Each folder view lives in a heap record: this header, then columnCount
// ViewColumns whose ratios add up to 1. A new record is made on each
// navigation.
struct ViewRecordHeader {
    float zoom;
    int32_t sortedBy;
    int32_t columnCount;
};

struct ViewColumn {
    int32_t kind;
    float ratio;
};

constexpr int kMinRecordColumns = 2;
constexpr int kMaxRecordColumns = 16;
constexpr int kMaxColumnKind = 63;
constexpr int kMaxSortedBy = 63;
constexpr size_t kMaxRecordSize =
    sizeof(ViewRecordHeader) + kMaxRecordColumns * sizeof(ViewColumn);

}  // namespace filepilot

// ===========================================================================
// Text and paths

namespace text {

void LowerInPlace(std::wstring& s) {
    if (!s.empty()) {
        CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    }
}

std::wstring ToLower(std::wstring s) {
    LowerInPlace(s);
    return s;
}

bool EqualsNoCase(const wchar_t* a, size_t aLength, const wchar_t* b,
                  size_t bLength) {
    return CompareStringOrdinal(a, static_cast<int>(aLength), b,
                                static_cast<int>(bLength), TRUE) == CSTR_EQUAL;
}

bool EndsWithNoCase(const std::wstring& s, const std::wstring& suffix) {
    return s.size() >= suffix.size() &&
           EqualsNoCase(s.data() + s.size() - suffix.size(), suffix.size(),
                        suffix.data(), suffix.size());
}

// Lowercase, backslashes, no trailing separator (except "c:\"): the form
// paths are compared and stored in.
std::wstring NormalizePath(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    while (path.size() > 3 && path.back() == L'\\') {
        path.pop_back();
    }
    LowerInPlace(path);
    return path;
}

bool IsAbsolutePath(const std::wstring& s) {
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

std::wstring TakeSetting(PCWSTR value) {
    std::wstring result = value ? value : L"";
    Wh_FreeStringSetting(value);
    return result;
}

}  // namespace text

// ===========================================================================
// Views

constexpr const wchar_t* kViewNames[kViewCount] = {
    L"details", L"tree", L"list", L"medium", L"large", L"xl"};

View ParseView(const std::wstring& name) {
    for (int i = 0; i < kViewCount; i++) {
        if (name == kViewNames[i]) {
            return static_cast<View>(i);
        }
    }
    return View::None;
}

PCWSTR ViewName(View view) {
    return view == View::None ? L"none" : kViewNames[static_cast<int>(view)];
}

// ===========================================================================
// Config

struct Hotkey {
    UINT vk = 0;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
};

struct Layout {
    View view = View::None;
    int zoom = 0;  // percent, 0 keeps the view's own zoom

    bool IsIconView() const {
        return view == View::Medium || view == View::Large || view == View::XL;
    }
};

struct Rule {
    std::wstring name;
    Layout layout;
    int threshold = 0;  // percent of the folder's files
};

// A settings snapshot. It is never modified once published, so threads keep
// using the one they took while settings change.
struct Config {
    std::vector<Rule> rules;
    // Lowercase extension -> index of the first rule listing it.
    std::unordered_map<std::wstring, size_t> ruleByExtension;
    int maxScan = 2000;
    View mixedView = View::None;
    View foldersOnlyView = View::None;
    bool rememberManual = true;
    std::vector<std::wstring> excludedFolders;  // normalized
    std::array<Hotkey, kViewCount> hotkeys;

    bool IsExcluded(const std::wstring& normalizedPath) const {
        for (const std::wstring& folder : excludedFolders) {
            if (normalizedPath.compare(0, folder.size(), folder) == 0 &&
                (normalizedPath.size() == folder.size() ||
                 normalizedPath[folder.size()] == L'\\' ||
                 folder.back() == L'\\')) {
                return true;
            }
        }
        return false;
    }
};

namespace config {

constexpr int kDefaultThreshold = 60;

std::mutex g_mutex;
std::shared_ptr<const Config> g_current = std::make_shared<Config>();

std::shared_ptr<const Config> Current() {
    std::lock_guard lock(g_mutex);
    return g_current;
}

void LoadRules(Config& config) {
    for (int i = 0;; i++) {
        std::wstring name =
            text::TakeSetting(Wh_GetStringSetting(L"rules[%d].name", i));
        std::wstring extensions =
            text::TakeSetting(Wh_GetStringSetting(L"rules[%d].extensions", i));
        std::wstring view =
            text::TakeSetting(Wh_GetStringSetting(L"rules[%d].view", i));
        if (name.empty() && extensions.empty() && view.empty()) {
            break;
        }

        Rule rule{name, {ParseView(view)}, kDefaultThreshold};
        rule.layout.zoom =
            std::clamp(Wh_GetIntSetting(L"rules[%d].zoom", i), 0, 100);
        // Templates made before thresholds existed read 0 here.
        int threshold = Wh_GetIntSetting(L"rules[%d].threshold", i);
        if (threshold > 0) {
            rule.threshold = std::min(threshold, 100);
        }
        if (rule.layout.view == View::None) {
            continue;
        }

        size_t index = config.rules.size();
        bool hasExtension = false;
        std::wstring token;
        for (wchar_t c : text::ToLower(extensions) + L" ") {
            if (c == L' ' || c == L',' || c == L';' || c == L'.') {
                if (!token.empty()) {
                    // The first rule listing an extension keeps it.
                    config.ruleByExtension.emplace(token, index);
                    hasExtension = true;
                }
                token.clear();
            } else {
                token += c;
            }
        }
        if (hasExtension) {
            config.rules.push_back(std::move(rule));
        }
    }
}

Hotkey ParseHotkey(const std::string& keys) {
    Hotkey hotkey;
    size_t start = 0;
    while (start <= keys.size()) {
        size_t end = keys.find('+', start);
        if (end == std::string::npos) {
            end = keys.size();
        }
        std::string token = keys.substr(start, end - start);
        if (token == "Ctrl") {
            hotkey.ctrl = true;
        } else if (token == "Shift") {
            hotkey.shift = true;
        } else if (token == "Alt") {
            hotkey.alt = true;
        } else if (token.size() == 1 &&
                   std::isalnum(static_cast<unsigned char>(token[0]))) {
            hotkey.vk = std::toupper(static_cast<unsigned char>(token[0]));
        } else if (token.size() >= 2 && token[0] == 'F' &&
                   std::isdigit(static_cast<unsigned char>(token[1]))) {
            int n = std::atoi(token.c_str() + 1);
            if (n >= 1 && n <= 24) {
                hotkey.vk = VK_F1 + n - 1;
            }
        }
        start = end + 1;
    }
    return hotkey;
}

std::string ReadFilePilotConfig() {
    wchar_t path[MAX_PATH];
    if (!ExpandEnvironmentStringsW(filepilot::kConfigFile, path, MAX_PATH)) {
        return {};
    }
    HANDLE file = CreateFileW(path, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Wh_Log(L"Can't open %s, using default hotkeys", path);
        return {};
    }
    std::string contents(GetFileSize(file, nullptr), '\0');
    DWORD read = 0;
    ReadFile(file, contents.data(), static_cast<DWORD>(contents.size()), &read,
             nullptr);
    CloseHandle(file);
    contents.resize(read);
    return contents;
}

void LoadHotkeys(std::array<Hotkey, kViewCount>& hotkeys) {
    for (int i = 0; i < kViewCount; i++) {
        hotkeys[i] = {static_cast<UINT>('1' + i), true, true, false};
    }

    std::string contents = ReadFilePilotConfig();
    std::regex binding(
        R"re("Keys"\s*:\s*\[\s*"([^"]+)"[^\]]*\]\s*,\s*"Command"\s*:\s*"HK_View(\w+)")re");
    for (std::sregex_iterator it(contents.begin(), contents.end(), binding),
         end;
         it != end; ++it) {
        for (int i = 0; i < kViewCount; i++) {
            if ((*it)[2] != filepilot::kViewCommands[i]) {
                continue;
            }
            Hotkey hotkey = ParseHotkey((*it)[1]);
            if (hotkey.vk) {
                hotkeys[i] = hotkey;
            } else {
                Wh_Log(L"Unsupported hotkey for HK_View%S",
                       filepilot::kViewCommands[i]);
            }
        }
    }
}

void Reload() {
    auto config = std::make_shared<Config>();
    LoadRules(*config);
    config->maxScan = std::clamp(Wh_GetIntSetting(L"maxScan"), 10, 100000);
    config->mixedView =
        ParseView(text::TakeSetting(Wh_GetStringSetting(L"mixedView")));
    config->foldersOnlyView =
        ParseView(text::TakeSetting(Wh_GetStringSetting(L"foldersOnlyView")));
    config->rememberManual = Wh_GetIntSetting(L"rememberManual");
    // Missing array items read as empty strings, so stop at the first one.
    for (int i = 0;; i++) {
        std::wstring folder = text::TakeSetting(
            Wh_GetStringSetting(L"excludedFolders[%d]", i));
        if (folder.empty()) {
            break;
        }
        config->excludedFolders.push_back(text::NormalizePath(folder));
    }
    LoadHotkeys(config->hotkeys);

    std::lock_guard lock(g_mutex);
    g_current = std::move(config);
}

}  // namespace config

// ===========================================================================
// Manual views

// Views picked with View hotkeys, by normalized folder path. Stored with
// Wh_SetStringValue as "view|path" lines.
class ManualViews {
   public:
    void Load() {
        std::vector<wchar_t> buffer(kMaxStoredChars);
        Wh_GetStringValue(kValueName, buffer.data(), buffer.size());
        std::wstring data = buffer.data();

        std::lock_guard lock(mutex_);
        views_.clear();
        size_t start = 0;
        while (start < data.size()) {
            size_t end = data.find(L'\n', start);
            if (end == std::wstring::npos) {
                end = data.size();
            }
            size_t separator = data.find(L'|', start);
            if (separator < end) {
                View view = ParseView(data.substr(start, separator - start));
                if (view != View::None) {
                    views_[data.substr(separator + 1, end - separator - 1)] =
                        view;
                }
            }
            start = end + 1;
        }
    }

    View Find(const std::wstring& normalizedPath) {
        std::lock_guard lock(mutex_);
        auto it = views_.find(normalizedPath);
        return it == views_.end() ? View::None : it->second;
    }

    void Remember(const std::wstring& normalizedPath, View view) {
        std::lock_guard lock(mutex_);
        views_[normalizedPath] = view;
        SaveLocked();
    }

    void Clear() {
        std::lock_guard lock(mutex_);
        views_.clear();
        SaveLocked();
    }

   private:
    static constexpr PCWSTR kValueName = L"manualViews";
    static constexpr size_t kMaxStoredChars = 1 << 18;

    void SaveLocked() {
        std::wstring data;
        for (const auto& [path, view] : views_) {
            data += ViewName(view);
            data += L'|';
            data += path;
            data += L'\n';
        }
        Wh_SetStringValue(kValueName, data.c_str());
    }

    std::mutex mutex_;
    std::unordered_map<std::wstring, View> views_;
};

ManualViews g_manualViews;

// ===========================================================================
// Main window

namespace main_window {

std::atomic<HWND> g_hwnd{nullptr};
std::atomic<DWORD> g_threadId{0};

HWND Handle() {
    return g_hwnd;
}

bool IsUiThread() {
    return GetCurrentThreadId() == g_threadId;
}

// True for File Pilot's top-level window, which is remembered on first sight.
bool Is(HWND hwnd) {
    if (hwnd == g_hwnd) {
        return true;
    }
    wchar_t className[64];
    if (!GetClassNameW(hwnd, className, ARRAYSIZE(className)) ||
        wcscmp(className, filepilot::kWindowClass) != 0 || GetParent(hwnd)) {
        return false;
    }
    g_threadId = GetWindowThreadProcessId(hwnd, nullptr);
    g_hwnd = hwnd;
    return true;
}

// The middle of the client area, which is over the file list.
POINT ListCenter() {
    HWND hwnd = g_hwnd;
    RECT rect{};
    GetClientRect(hwnd, &rect);
    POINT point{(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
    ClientToScreen(hwnd, &point);
    return point;
}

}  // namespace main_window

// ===========================================================================
// Fake input
//
// The mod drives File Pilot with posted messages. File Pilot reads modifiers
// with GetKeyState and the mouse with GetCursorPos, so while a tagged message
// is being handled those hooks report what the message asks for:
// - keystrokes carry the tag and modifiers in reserved lParam bits 25-28;
// - mouse moves and wheels carry the tag in an unused MK_* bit, and the
//   cursor appears over the file list.
// Fake input lasts until File Pilot has drained its queue and started the
// next frame, plus a minimum hold for wheels, which are applied while
// rendering.

namespace fake_input {

constexpr LPARAM kKeyTag = 1 << 25;
constexpr int kKeyModsShift = 26;
constexpr WORD kMouseTag = 0x0800;
constexpr int kCtrl = 1, kShift = 2, kAlt = 4;
constexpr ULONGLONG kMaxMs = 1000;
constexpr ULONGLONG kWheelHoldMs = 200;
constexpr DWORD kPollMs = 10;

enum Phase { kIdle, kPosted, kDelivered, kQueueDrained };

std::atomic<int> g_phase{kIdle};
std::atomic<int> g_mods{0};
std::atomic<bool> g_cursor{false};
std::atomic<ULONGLONG> g_since{0};
std::atomic<ULONGLONG> g_holdMs{0};

int ModsOf(const Hotkey& hotkey) {
    return (hotkey.ctrl ? kCtrl : 0) | (hotkey.shift ? kShift : 0) |
           (hotkey.alt ? kAlt : 0);
}

bool IsTaggedKey(const MSG& msg) {
    return msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST &&
           (msg.lParam & kKeyTag);
}

void End() {
    g_mods = 0;
    g_cursor = false;
    g_phase = kIdle;
}

// Called by the sender before posting tagged messages.
void Begin() {
    g_since = GetTickCount64();
    g_phase = kPosted;
}

// Called by the sender: waits until File Pilot has run a frame with the tagged
// messages, so whatever comes next gets its own fake input.
void Wait() {
    while (g_phase != kIdle) {
        if (GetTickCount64() - g_since > kMaxMs) {
            Wh_Log(L"File Pilot didn't handle fake input in time");
            End();
            return;
        }
        Sleep(kPollMs);
    }
}

bool IsActive() {
    int phase = g_phase;
    if (phase != kDelivered && phase != kQueueDrained) {
        return false;
    }
    if (GetTickCount64() - g_since > kMaxMs) {
        End();
        return false;
    }
    return true;
}

bool IsModifierFaked(int vk) {
    int mods = g_mods;
    if (!mods) {
        return false;
    }
    switch (vk) {
        case VK_CONTROL:
        case VK_LCONTROL:
            return (mods & kCtrl) && IsActive();
        case VK_SHIFT:
        case VK_LSHIFT:
            return (mods & kShift) && IsActive();
        case VK_MENU:
        case VK_LMENU:
            return (mods & kAlt) && IsActive();
    }
    return false;
}

bool IsCursorFaked() {
    return g_cursor && IsActive();
}

void Deliver(int mods, bool cursor, ULONGLONG holdMs) {
    g_since = GetTickCount64();
    g_holdMs = holdMs;
    g_mods = mods;
    g_cursor = cursor;
    g_phase = kDelivered;
}

// Called for each message File Pilot takes off its queue. Returns whether it
// was one of ours.
bool OnMessage(const MSG& msg) {
    if (msg.message == WM_MOUSEWHEEL || msg.message == WM_MOUSEMOVE) {
        WORD keys = GET_KEYSTATE_WPARAM(msg.wParam);
        if (!(keys & kMouseTag)) {
            return false;
        }
        Deliver((keys & MK_CONTROL ? kCtrl : 0) | (keys & MK_SHIFT ? kShift : 0),
                true, kWheelHoldMs);
        return true;
    }
    if (IsTaggedKey(msg)) {
        Deliver((msg.lParam >> kKeyModsShift) & 7, false, 0);
        return true;
    }
    return false;
}

// File Pilot's UI thread found its queue empty: the next frame is starting.
void OnQueueDrained() {
    int delivered = kDelivered;
    g_phase.compare_exchange_strong(delivered, kQueueDrained);
}

// File Pilot's UI thread is back for more messages: that frame is done.
void OnFrameDone() {
    if (g_phase == kQueueDrained && GetTickCount64() - g_since >= g_holdMs) {
        End();
    }
}

}  // namespace fake_input

// ===========================================================================
// Synthetic input

namespace synth {

// Lets File Pilot render a few frames with the fake cursor before the wheel
// arrives. Must stay below fake_input::kWheelHoldMs.
constexpr DWORD kHoverSettleMs = 50;

LPARAM KeyLParam(const Hotkey& hotkey, bool up) {
    LPARAM lParam =
        1 | (static_cast<LPARAM>(MapVirtualKeyW(hotkey.vk, MAPVK_VK_TO_VSC))
             << 16) |
        fake_input::kKeyTag |
        (static_cast<LPARAM>(fake_input::ModsOf(hotkey))
         << fake_input::kKeyModsShift);
    if (hotkey.alt) {
        lParam |= 1 << 29;
    }
    if (up) {
        lParam |= 0xC0000000;
    }
    return lParam;
}

void PostAndWait(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    fake_input::Begin();
    PostMessageW(hwnd, message, wParam, lParam);
    fake_input::Wait();
}

void PressHotkey(HWND hwnd, const Hotkey& hotkey) {
    UINT down = hotkey.alt ? WM_SYSKEYDOWN : WM_KEYDOWN;
    UINT up = hotkey.alt ? WM_SYSKEYUP : WM_KEYUP;
    PostAndWait(hwnd, down, hotkey.vk, KeyLParam(hotkey, false));
    PostAndWait(hwnd, up, hotkey.vk, KeyLParam(hotkey, true));
}

// Sends Ctrl+wheel messages, one per notch (positive zooms in). File Pilot
// hit-tests the wheel against the previous frame's cursor, so a move to the
// file list gets a few frames first, and the fake cursor must not lapse in
// between.
void WheelZoom(HWND hwnd, int notches) {
    if (notches == 0) {
        return;
    }
    POINT screenPoint = main_window::ListCenter();
    POINT clientPoint = screenPoint;
    ScreenToClient(hwnd, &clientPoint);
    SHORT delta = notches > 0 ? WHEEL_DELTA : -WHEEL_DELTA;

    fake_input::Begin();
    PostMessageW(hwnd, WM_MOUSEMOVE, fake_input::kMouseTag,
                 MAKELPARAM(clientPoint.x, clientPoint.y));
    Sleep(kHoverSettleMs);
    for (int i = 0; i < std::abs(notches); i++) {
        PostMessageW(hwnd, WM_MOUSEWHEEL,
                     MAKEWPARAM(MK_CONTROL | fake_input::kMouseTag, delta),
                     MAKELPARAM(screenPoint.x, screenPoint.y));
    }
    fake_input::Wait();
}

}  // namespace synth

// ===========================================================================
// View records
//
// Finds File Pilot's view records by their shape (filepilot::ViewRecordHeader)
// in writable private memory. Memory may be freed while it's read, so all
// access goes through Read/WriteProcessMemory, which fail instead of
// crashing. Used by the worker thread only.

class ViewRecords {
   public:
    std::vector<uintptr_t> FindInKnownRegions() { return Scan(&regions_); }

    std::vector<uintptr_t> FindEverywhere() { return Scan(nullptr); }

    // The current folder's record, found without touching File Pilot's UI.
    // In the known regions File Pilot replaces a folder's record on each
    // navigation, so it's the one record that wasn't there last time, or the
    // only record there. 0 when neither singles one out.
    uintptr_t FindFreshInKnownRegions() {
        std::vector<uintptr_t> records = FindInKnownRegions();
        uintptr_t fresh = 0;
        int freshCount = 0;
        for (uintptr_t record : records) {
            if (std::find(seen_.begin(), seen_.end(), record) == seen_.end()) {
                fresh = record;
                freshCount++;
            }
        }
        seen_ = records;
        if (freshCount == 1) {
            return fresh;
        }
        return records.size() == 1 ? records[0] : 0;
    }

    // Marks the records now in the known regions as seen, after the current
    // record was found another way.
    void MarkKnownRegionsSeen() { seen_ = FindInKnownRegions(); }

    // Remembers the region holding a record: File Pilot keeps allocating new
    // records in the same few regions.
    void RememberRegionOf(uintptr_t record) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(record), &info,
                          sizeof(info))) {
            return;
        }
        uintptr_t region = reinterpret_cast<uintptr_t>(info.BaseAddress);
        if (std::find(regions_.begin(), regions_.end(), region) ==
            regions_.end()) {
            regions_.push_back(region);
        }
    }

    static bool ReadZoom(uintptr_t record, float* zoom) {
        BYTE data[filepilot::kMaxRecordSize];
        size_t read;
        if (!ReadMemory(record, data, sizeof(data), &read) ||
            !IsRecord(data, read)) {
            return false;
        }
        memcpy(zoom, data, sizeof(*zoom));
        return true;
    }

    static bool WriteZoom(uintptr_t record, float zoom) {
        SIZE_T written = 0;
        return WriteProcessMemory(GetCurrentProcess(),
                                  reinterpret_cast<LPVOID>(record), &zoom,
                                  sizeof(zoom), &written) &&
               written == sizeof(zoom);
    }

   private:
    static constexpr size_t kChunkSize = 4 << 20;

    static bool IsRecord(const BYTE* data, size_t available) {
        using namespace filepilot;
        if (available < sizeof(ViewRecordHeader)) {
            return false;
        }
        ViewRecordHeader header;
        memcpy(&header, data, sizeof(header));
        // The column count rules out almost every offset, so it goes first.
        if (header.columnCount < kMinRecordColumns ||
            header.columnCount > kMaxRecordColumns) {
            return false;
        }
        if (!(header.zoom >= 0.0f && header.zoom <= 1.0f) ||
            header.sortedBy < 0 || header.sortedBy > kMaxSortedBy ||
            available < sizeof(ViewRecordHeader) +
                            header.columnCount * sizeof(ViewColumn)) {
            return false;
        }

        float total = 0.0f;
        uint64_t kindsSeen = 0;
        for (int i = 0; i < header.columnCount; i++) {
            ViewColumn column;
            memcpy(&column,
                   data + sizeof(ViewRecordHeader) + i * sizeof(ViewColumn),
                   sizeof(column));
            if (column.kind < 0 || column.kind > kMaxColumnKind ||
                !(column.ratio > 0.0f && column.ratio <= 1.0f)) {
                return false;
            }
            uint64_t kindBit = 1ull << column.kind;
            if (kindsSeen & kindBit) {
                return false;
            }
            kindsSeen |= kindBit;
            total += column.ratio;
        }
        return std::fabs(total - 1.0f) < 0.02f;
    }

    static bool ReadMemory(uintptr_t address, void* buffer, size_t size,
                           size_t* read) {
        SIZE_T bytesRead = 0;
        BOOL ok = ReadProcessMemory(GetCurrentProcess(),
                                    reinterpret_cast<LPCVOID>(address), buffer,
                                    size, &bytesRead);
        *read = bytesRead;
        return ok || bytesRead > 0;
    }

    static bool IsScannable(const MEMORY_BASIC_INFORMATION& info) {
        return info.State == MEM_COMMIT && info.Type == MEM_PRIVATE &&
               info.Protect == PAGE_READWRITE;
    }

    void ScanRegion(const MEMORY_BASIC_INFORMATION& info,
                    std::vector<uintptr_t>& records) {
        uintptr_t regionStart = reinterpret_cast<uintptr_t>(info.BaseAddress);
        uintptr_t regionEnd = regionStart + info.RegionSize;
        for (uintptr_t start = regionStart; start < regionEnd;
             start += kChunkSize) {
            // Chunks overlap by one record, so none is cut in two.
            size_t size = std::min<size_t>(chunk_.size(), regionEnd - start);
            size_t read;
            if (!ReadMemory(start, chunk_.data(), size, &read)) {
                continue;
            }
            size_t scanEnd = std::min(read, kChunkSize);
            for (size_t i = 0; i < scanEnd; i += alignof(float)) {
                if (IsRecord(&chunk_[i], read - i)) {
                    records.push_back(start + i);
                }
            }
        }
    }

    // Scans the regions with these base addresses, or all writable private
    // memory when regions is null.
    std::vector<uintptr_t> Scan(const std::vector<uintptr_t>* regions) {
        ULONGLONG startTime = GetTickCount64();
        if (chunk_.empty()) {
            chunk_.resize(kChunkSize + filepilot::kMaxRecordSize);
        }
        std::vector<uintptr_t> records;
        MEMORY_BASIC_INFORMATION info;
        if (regions) {
            for (uintptr_t region : *regions) {
                if (VirtualQuery(reinterpret_cast<LPCVOID>(region), &info,
                                 sizeof(info)) &&
                    IsScannable(info)) {
                    ScanRegion(info, records);
                }
            }
        } else {
            for (uintptr_t address = 0;
                 VirtualQuery(reinterpret_cast<LPCVOID>(address), &info,
                              sizeof(info));
                 address = reinterpret_cast<uintptr_t>(info.BaseAddress) +
                           info.RegionSize) {
                if (IsScannable(info)) {
                    ScanRegion(info, records);
                }
            }
        }
        Wh_Log(L"Found %zu view records in %llu ms (%s)", records.size(),
               GetTickCount64() - startTime,
               regions ? L"known regions" : L"all memory");
        return records;
    }

    std::vector<BYTE> chunk_;
    std::vector<uintptr_t> regions_;
    std::vector<uintptr_t> seen_;  // records in the known regions last time
};

ViewRecords g_viewRecords;

// ===========================================================================
// Layouts

thread_local bool t_ownScan = false;

Layout ClassifyFolder(const std::wstring& path, const Config& config) {
    std::wstring pattern = path;
    if (pattern.back() != L'\\') {
        pattern += L'\\';
    }
    pattern += L'*';

    // Our own listing must not end up in the directory log.
    WIN32_FIND_DATAW data;
    t_ownScan = true;
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
                                   FindExSearchNameMatch, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
    t_ownScan = false;
    if (find == INVALID_HANDLE_VALUE) {
        return {};
    }

    std::vector<int> counts(config.rules.size());
    int files = 0, folders = 0;
    std::wstring extension;
    do {
        const wchar_t* name = data.cFileName;
        if (name[0] == L'.' &&
            (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'))) {
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
        const wchar_t* dot = wcsrchr(name, L'.');
        if (!dot || !dot[1]) {
            continue;
        }
        extension.assign(dot + 1);  // reuses its buffer across files
        text::LowerInPlace(extension);
        auto rule = config.ruleByExtension.find(extension);
        if (rule != config.ruleByExtension.end()) {
            counts[rule->second]++;
        }
    } while (files + folders < config.maxScan && FindNextFileW(find, &data));
    FindClose(find);

    if (files == 0) {
        return {folders ? config.foldersOnlyView : View::None};
    }
    // Among templates reaching their own threshold, the most files wins.
    size_t best = counts.size();
    for (size_t i = 0; i < counts.size(); i++) {
        bool qualifies = counts[i] * 100 >= config.rules[i].threshold * files;
        if (qualifies && (best == counts.size() || counts[i] > counts[best])) {
            best = i;
        }
    }
    if (best == counts.size()) {
        Wh_Log(L"%s: mixed (%d files)", path.c_str(), files);
        return {config.mixedView};
    }
    Wh_Log(L"%s: %d/%d files match %s", path.c_str(), counts[best], files,
           config.rules[best].name.c_str());
    return config.rules[best].layout;
}

View NearestIconPreset(float zoom) {
    View nearest = View::Medium;
    for (View view : {View::Medium, View::Large, View::XL}) {
        if (std::fabs(filepilot::kViewZoom[static_cast<int>(view)] - zoom) <
            std::fabs(filepilot::kViewZoom[static_cast<int>(nearest)] - zoom)) {
            nearest = view;
        }
    }
    return nearest;
}

// The one record at exactly this zoom, or 0.
uintptr_t FindOnlyRecordAt(const std::vector<uintptr_t>& records, float zoom) {
    uintptr_t found = 0;
    for (uintptr_t record : records) {
        float recordZoom;
        if (ViewRecords::ReadZoom(record, &recordZoom) && recordZoom == zoom) {
            if (found) {
                return 0;
            }
            found = record;
        }
    }
    return found;
}

// Finds the current folder's record by pressing the nearest icon preset and
// seeing which record takes its zoom. File Pilot shows its view popover for
// a moment, so this is only for when FindFreshInKnownRegions can't tell.
uintptr_t FindRecordWithPreset(HWND hwnd, const Config& config, float zoom) {
    View preset = NearestIconPreset(zoom);
    float presetZoom = filepilot::kViewZoom[static_cast<int>(preset)];

    // Scan before pressing the preset, so the view doesn't sit at the preset
    // while we search.
    std::vector<uintptr_t> records = g_viewRecords.FindInKnownRegions();
    if (records.empty()) {
        records = g_viewRecords.FindEverywhere();
    }
    std::vector<std::pair<uintptr_t, float>> before;
    before.reserve(records.size());
    for (uintptr_t record : records) {
        float recordZoom;
        if (ViewRecords::ReadZoom(record, &recordZoom)) {
            before.push_back({record, recordZoom});
        }
    }

    synth::PressHotkey(hwnd, config.hotkeys[static_cast<int>(preset)]);

    uintptr_t current = 0;
    int changed = 0;
    for (const auto& [record, oldZoom] : before) {
        float newZoom;
        if (ViewRecords::ReadZoom(record, &newZoom) && newZoom != oldZoom &&
            newZoom == presetZoom) {
            current = record;
            changed++;
        }
    }
    if (changed != 1) {
        // The record may be outside the known regions, or the folder was
        // already at the preset: look for the one record at the preset's zoom.
        current = FindOnlyRecordAt(g_viewRecords.FindEverywhere(), presetZoom);
        if (!current) {
            Wh_Log(L"Can't tell the view record apart (%d changed)", changed);
        }
    }
    return current;
}

// Writes the exact zoom into the current folder's view record.
bool SetZoomInMemory(HWND hwnd, const Config& config, int zoom) {
    float targetZoom = zoom / 100.0f;
    uintptr_t current = g_viewRecords.FindFreshInKnownRegions();
    if (!current) {
        Wh_Log(L"No fresh view record, finding it with a preset");
        current = FindRecordWithPreset(hwnd, config, targetZoom);
        if (!current) {
            return false;
        }
        g_viewRecords.RememberRegionOf(current);
        g_viewRecords.MarkKnownRegionsSeen();
    }

    if (!ViewRecords::WriteZoom(current, targetZoom)) {
        return false;
    }
    // Make File Pilot render the new zoom on its next frame.
    InvalidateRect(hwnd, nullptr, FALSE);
    Wh_Log(L"Zoom %d%% written to the view record at %p", zoom,
           reinterpret_cast<void*>(current));
    return true;
}

// Fallback: Ctrl+wheel up from Details (0%), in whole notches.
void SetZoomWithWheel(HWND hwnd, const Config& config, int zoom) {
    synth::PressHotkey(hwnd, config.hotkeys[static_cast<int>(View::Details)]);
    synth::WheelZoom(
        hwnd, static_cast<int>(std::lround(zoom / filepilot::kZoomPerWheelMessage)));
}

void ApplyLayout(HWND hwnd, const Config& config, const Layout& layout) {
    if (layout.IsIconView() && layout.zoom > 0) {
        if (!SetZoomInMemory(hwnd, config, layout.zoom)) {
            Wh_Log(L"Falling back to Ctrl+wheel zoom");
            SetZoomWithWheel(hwnd, config, layout.zoom);
        }
        return;
    }
    synth::PressHotkey(hwnd, config.hotkeys[static_cast<int>(layout.view)]);
}

// Picks and applies the layout for a folder. stillCurrent tells whether the
// user is still in it.
void ApplyFolderLayout(const std::wstring& path,
                       const std::function<bool()>& stillCurrent) {
    std::shared_ptr<const Config> config = config::Current();
    std::wstring normalized = text::NormalizePath(path);
    if (config->IsExcluded(normalized)) {
        return;
    }

    Layout layout;
    if (config->rememberManual) {
        layout.view = g_manualViews.Find(normalized);
        if (layout.view != View::None) {
            Wh_Log(L"%s: remembered %s", path.c_str(), ViewName(layout.view));
        }
    }
    if (layout.view == View::None) {
        layout = ClassifyFolder(path, *config);
    }

    HWND hwnd = main_window::Handle();
    if (layout.view == View::None || !hwnd || !stillCurrent()) {
        Wh_Log(L"%s: leaving view as is", path.c_str());
        return;
    }
    Wh_Log(L"%s: switching to %s, zoom %d", path.c_str(),
           ViewName(layout.view), layout.zoom);
    ApplyLayout(hwnd, *config, layout);
}

// ===========================================================================
// Directory log
//
// Directories File Pilot opened lately, so a title that only shows a path's
// tail can be resolved. File Pilot opens folders constantly while sizing
// them, so recording reuses a fixed ring of strings and doesn't allocate once
// warmed up.

class DirectoryLog {
   public:
    // Records "C:\dir" from a directory path or a listing pattern such as
    // "\\?\C:\dir\*".
    void Record(LPCWSTR pathOrPattern) {
        if (!pathOrPattern || t_ownScan) {
            return;
        }
        thread_local std::wstring dir;
        if (!ToDirectory(pathOrPattern, dir)) {
            return;
        }

        std::lock_guard lock(mutex_);
        size_t last = (next_ + kCapacity - 1) % kCapacity;
        if (count_ && entries_[last] == dir) {
            return;
        }
        entries_[next_].assign(dir);
        next_ = (next_ + 1) % kCapacity;
        count_ = std::min(count_ + 1, kCapacity);
    }

    // The newest directory whose path ends with "\<tail>".
    std::wstring FindByTail(const std::wstring& tail) {
        std::wstring needle = L"\\" + tail;
        std::replace(needle.begin(), needle.end(), L'/', L'\\');
        std::lock_guard lock(mutex_);
        for (size_t i = 1; i <= count_; i++) {
            const std::wstring& entry =
                entries_[(next_ + kCapacity - i) % kCapacity];
            if (entry.size() > needle.size() &&
                text::EndsWithNoCase(entry, needle)) {
                return entry;
            }
        }
        return {};
    }

   private:
    static constexpr size_t kCapacity = 256;

    static bool ToDirectory(LPCWSTR pathOrPattern, std::wstring& dir) {
        dir.assign(pathOrPattern);
        if (dir.find_first_of(L"*?") != std::wstring::npos) {
            size_t slash = dir.find_last_of(L"\\/");
            if (slash == std::wstring::npos) {
                return false;
            }
            dir.resize(slash + 1);
        }
        std::replace(dir.begin(), dir.end(), L'/', L'\\');
        if (dir.rfind(L"\\\\?\\UNC\\", 0) == 0) {
            dir.erase(2, 6);  // "\\?\UNC\server" -> "\\server"
        } else if (dir.rfind(L"\\\\?\\", 0) == 0) {
            dir.erase(0, 4);
        }
        while (dir.size() > 3 && dir.back() == L'\\') {
            dir.pop_back();  // keep "C:\" for drive roots
        }
        return text::IsAbsolutePath(dir);
    }

    std::mutex mutex_;
    std::array<std::wstring, kCapacity> entries_;
    size_t next_ = 0;
    size_t count_ = 0;
};

DirectoryLog g_directoryLog;

// ===========================================================================
// Navigator
//
// Tracks the folder shown in the title and hands each new one to a worker
// thread, which resolves its full path and applies its layout. A generation
// number lets the worker drop folders the user has already left.

class Navigator {
   public:
    void Start() { worker_ = std::thread(&Navigator::Run, this); }

    void Stop() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        wake_.notify_one();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    // apply: false only records the folder, for the title found at startup.
    void OnTitle(PCWSTR title, bool apply) {
        std::wstring path = PathFromTitle(title);
        std::lock_guard lock(mutex_);
        if (path == titlePath_) {
            return;
        }
        titlePath_ = path;
        resolvedPath_ = text::IsAbsolutePath(path) ? path : L"";
        if (!resolvedPath_.empty()) {
            lastResolvedPath_ = resolvedPath_;
        }
        generation_++;
        if (apply && !path.empty()) {
            pendingPath_ = path;
            hasPending_ = true;
            wake_.notify_one();
        }
    }

    // The current folder's full path, or empty while it's unknown.
    std::wstring CurrentFolder() {
        std::lock_guard lock(mutex_);
        return resolvedPath_;
    }

    std::wstring TitlePath() {
        std::lock_guard lock(mutex_);
        return titlePath_;
    }

   private:
    static constexpr int kResolveAttempts = 20;
    static constexpr DWORD kResolveRetryMs = 50;

    static std::wstring PathFromTitle(const std::wstring& title) {
        size_t suffix = title.rfind(filepilot::kTitleSuffix);
        if (suffix == std::wstring::npos || suffix < 2 ||
            title[suffix - 1] != L')') {
            return {};
        }
        std::wstring head = title.substr(0, suffix - 1);
        std::wstring fallback;
        for (size_t open = head.find(L" ("); open != std::wstring::npos;
             open = head.find(L" (", open + 1)) {
            std::wstring path = head.substr(open + 2);
            std::wstring last = text::LastComponent(path);
            if (text::EqualsNoCase(last.data(), last.size(), head.data(),
                                   open)) {
                return path;
            }
            if (fallback.empty() && text::IsAbsolutePath(path)) {
                fallback = path;
            }
        }
        return fallback;
    }

    static bool IsDirectory(const std::wstring& path) {
        DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES &&
               (attributes & FILE_ATTRIBUTE_DIRECTORY);
    }

    // Navigation mostly stays in one tree, so try "<ancestor of the last
    // folder>\<tail>" from the deepest ancestor up.
    static std::wstring FindUnderFolder(const std::wstring& tail,
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

    bool IsCurrent(unsigned generation) {
        std::lock_guard lock(mutex_);
        return generation == generation_;
    }

    // Turns a title path into a full path, giving File Pilot a moment to
    // open the folder when the title only shows its tail.
    std::wstring Resolve(const std::wstring& titlePath, unsigned generation) {
        if (text::IsAbsolutePath(titlePath)) {
            return titlePath;
        }
        std::wstring lastFolder;
        {
            std::lock_guard lock(mutex_);
            lastFolder = lastResolvedPath_;
        }
        std::wstring path = FindUnderFolder(titlePath, lastFolder);
        for (int i = 0;
             path.empty() && i < kResolveAttempts && IsCurrent(generation);
             i++) {
            path = g_directoryLog.FindByTail(titlePath);
            if (path.empty()) {
                Sleep(kResolveRetryMs);
            }
        }
        if (path.empty()) {
            Wh_Log(L"Can't resolve %s", titlePath.c_str());
        }
        return path;
    }

    void Process(const std::wstring& titlePath, unsigned generation) {
        std::wstring path = Resolve(titlePath, generation);
        if (path.empty()) {
            return;
        }
        {
            std::lock_guard lock(mutex_);
            if (generation != generation_) {
                return;
            }
            resolvedPath_ = path;
            lastResolvedPath_ = path;
        }
        ApplyFolderLayout(path, [&] { return IsCurrent(generation); });
    }

    void Run() {
        std::unique_lock lock(mutex_);
        while (true) {
            wake_.wait(lock, [this] { return stop_ || hasPending_; });
            if (stop_) {
                return;
            }
            std::wstring path = pendingPath_;
            unsigned generation = generation_;
            hasPending_ = false;
            lock.unlock();
            Process(path, generation);
            lock.lock();
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    bool stop_ = false;
    std::wstring titlePath_;         // as shown in the title, maybe a tail
    std::wstring resolvedPath_;      // the current folder's full path
    std::wstring lastResolvedPath_;  // kept across folder changes
    std::wstring pendingPath_;
    bool hasPending_ = false;
    unsigned generation_ = 0;
};

Navigator g_navigator;

// ===========================================================================
// Hooks

using SetWindowTextW_t = decltype(&SetWindowTextW);
using GetKeyState_t = decltype(&GetKeyState);
using PeekMessageW_t = decltype(&PeekMessageW);
using TranslateMessage_t = decltype(&TranslateMessage);
using GetCursorPos_t = decltype(&GetCursorPos);
using WindowFromPoint_t = decltype(&WindowFromPoint);
using CreateFileW_t = decltype(&CreateFileW);
using FindFirstFileW_t = decltype(&FindFirstFileW);
using FindFirstFileExW_t = decltype(&FindFirstFileExW);

SetWindowTextW_t SetWindowTextW_Original;
GetKeyState_t GetKeyState_Original;
PeekMessageW_t PeekMessageW_Original;
TranslateMessage_t TranslateMessage_Original;
GetCursorPos_t GetCursorPos_Original;
WindowFromPoint_t WindowFromPoint_Original;
CreateFileW_t CreateFileW_Original;
FindFirstFileW_t FindFirstFileW_Original;
FindFirstFileExW_t FindFirstFileExW_Original;

bool IsRealKeyDown(int vk) {
    return GetKeyState_Original(vk) & 0x8000;
}

// A real View hotkey press: remember the user's choice for this folder.
void RememberManualView(const MSG& msg) {
    bool isFirstKeyDown =
        (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) &&
        !(msg.lParam & 0x40000000);
    if (!isFirstKeyDown) {
        return;
    }
    std::shared_ptr<const Config> config = config::Current();
    if (!config->rememberManual) {
        return;
    }
    for (int i = 0; i < kViewCount; i++) {
        const Hotkey& hotkey = config->hotkeys[i];
        if (hotkey.vk != msg.wParam ||
            hotkey.ctrl != IsRealKeyDown(VK_CONTROL) ||
            hotkey.shift != IsRealKeyDown(VK_SHIFT) ||
            hotkey.alt != IsRealKeyDown(VK_MENU)) {
            continue;
        }
        std::wstring folder = g_navigator.CurrentFolder();
        if (!folder.empty()) {
            View view = static_cast<View>(i);
            Wh_Log(L"Remembering %s for %s", ViewName(view), folder.c_str());
            g_manualViews.Remember(text::NormalizePath(folder), view);
        }
        return;
    }
}

BOOL WINAPI SetWindowTextW_Hook(HWND hwnd, LPCWSTR text) {
    BOOL result = SetWindowTextW_Original(hwnd, text);
    if (text && main_window::Is(hwnd)) {
        g_navigator.OnTitle(text, true);
    }
    return result;
}

SHORT WINAPI GetKeyState_Hook(int vk) {
    SHORT state = GetKeyState_Original(vk);
    if (fake_input::IsModifierFaked(vk)) {
        state |= static_cast<SHORT>(0x8000);
    }
    return state;
}

BOOL WINAPI GetCursorPos_Hook(LPPOINT point) {
    if (point && fake_input::IsCursorFaked()) {
        *point = main_window::ListCenter();
        return TRUE;
    }
    return GetCursorPos_Original(point);
}

HWND WINAPI WindowFromPoint_Hook(POINT point) {
    if (fake_input::IsCursorFaked()) {
        return main_window::Handle();
    }
    return WindowFromPoint_Original(point);
}

BOOL WINAPI PeekMessageW_Hook(LPMSG msg,
                              HWND hwnd,
                              UINT filterMin,
                              UINT filterMax,
                              UINT removeMsg) {
    bool isUiThread = main_window::IsUiThread();
    if (isUiThread) {
        fake_input::OnFrameDone();
    }
    BOOL result =
        PeekMessageW_Original(msg, hwnd, filterMin, filterMax, removeMsg);
    if (result && (removeMsg & PM_REMOVE) && msg->hwnd &&
        msg->hwnd == main_window::Handle()) {
        if (!fake_input::OnMessage(*msg)) {
            RememberManualView(*msg);
        }
    } else if (!result && isUiThread) {
        fake_input::OnQueueDrained();
    }
    return result;
}

BOOL WINAPI TranslateMessage_Hook(const MSG* msg) {
    // Our keys must not turn into typed characters (e.g. into the filter box).
    if (msg && fake_input::IsTaggedKey(*msg)) {
        return TRUE;
    }
    return TranslateMessage_Original(msg);
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
        g_directoryLog.Record(fileName);
    }
    return file;
}

HANDLE WINAPI FindFirstFileW_Hook(LPCWSTR fileName,
                                  LPWIN32_FIND_DATAW findData) {
    HANDLE find = FindFirstFileW_Original(fileName, findData);
    if (find != INVALID_HANDLE_VALUE) {
        g_directoryLog.Record(fileName);
    }
    return find;
}

HANDLE WINAPI FindFirstFileExW_Hook(LPCWSTR fileName,
                                    FINDEX_INFO_LEVELS infoLevel,
                                    LPVOID findData,
                                    FINDEX_SEARCH_OPS searchOp,
                                    LPVOID searchFilter,
                                    DWORD additionalFlags) {
    HANDLE find =
        FindFirstFileExW_Original(fileName, infoLevel, findData, searchOp,
                                  searchFilter, additionalFlags);
    if (find != INVALID_HANDLE_VALUE) {
        g_directoryLog.Record(fileName);
    }
    return find;
}

struct HookSpec {
    PCWSTR module;
    const char* function;
    void* hook;
    void** original;
};

const HookSpec kHooks[] = {
    {L"user32.dll", "SetWindowTextW", reinterpret_cast<void*>(SetWindowTextW_Hook),
     reinterpret_cast<void**>(&SetWindowTextW_Original)},
    {L"user32.dll", "GetKeyState", reinterpret_cast<void*>(GetKeyState_Hook),
     reinterpret_cast<void**>(&GetKeyState_Original)},
    {L"user32.dll", "PeekMessageW", reinterpret_cast<void*>(PeekMessageW_Hook),
     reinterpret_cast<void**>(&PeekMessageW_Original)},
    {L"user32.dll", "TranslateMessage",
     reinterpret_cast<void*>(TranslateMessage_Hook),
     reinterpret_cast<void**>(&TranslateMessage_Original)},
    {L"user32.dll", "GetCursorPos", reinterpret_cast<void*>(GetCursorPos_Hook),
     reinterpret_cast<void**>(&GetCursorPos_Original)},
    {L"user32.dll", "WindowFromPoint",
     reinterpret_cast<void*>(WindowFromPoint_Hook),
     reinterpret_cast<void**>(&WindowFromPoint_Original)},
    {L"kernelbase.dll", "CreateFileW", reinterpret_cast<void*>(CreateFileW_Hook),
     reinterpret_cast<void**>(&CreateFileW_Original)},
    {L"kernelbase.dll", "FindFirstFileW",
     reinterpret_cast<void*>(FindFirstFileW_Hook),
     reinterpret_cast<void**>(&FindFirstFileW_Original)},
    {L"kernelbase.dll", "FindFirstFileExW",
     reinterpret_cast<void*>(FindFirstFileExW_Hook),
     reinterpret_cast<void**>(&FindFirstFileExW_Original)},
};

bool InstallHooks() {
    for (const HookSpec& spec : kHooks) {
        HMODULE module = GetModuleHandleW(spec.module);
        void* target =
            module ? reinterpret_cast<void*>(GetProcAddress(module, spec.function))
                   : nullptr;
        if (!target) {
            Wh_Log(L"Missing %s!%S", spec.module, spec.function);
            return false;
        }
        if (!Wh_SetFunctionHook(target, spec.hook, spec.original)) {
            Wh_Log(L"Can't hook %s!%S", spec.module, spec.function);
            return false;
        }
    }
    return true;
}

BOOL CALLBACK FindMainWindow(HWND hwnd, LPARAM) {
    DWORD processId;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId != GetCurrentProcessId() || !main_window::Is(hwnd)) {
        return TRUE;
    }
    wchar_t title[1024];
    if (GetWindowTextW(hwnd, title, ARRAYSIZE(title))) {
        g_navigator.OnTitle(title, false);
    }
    return FALSE;
}

}  // namespace

// ===========================================================================
// Mod lifecycle

BOOL Wh_ModInit() {
    config::Reload();
    g_manualViews.Load();
    if (!InstallHooks()) {
        return FALSE;
    }
    g_navigator.Start();
    return TRUE;
}

void Wh_ModAfterInit() {
    EnumWindows(FindMainWindow, 0);
    Wh_Log(L"Main window %p, folder %s", main_window::Handle(),
           g_navigator.TitlePath().c_str());
}

void Wh_ModUninit() {
    g_navigator.Stop();
    fake_input::End();
}

void Wh_ModSettingsChanged() {
    config::Reload();
    if (!config::Current()->rememberManual) {
        g_manualViews.Clear();
    }
}
