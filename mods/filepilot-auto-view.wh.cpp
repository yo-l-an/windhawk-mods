// ==WindhawkMod==
// @id              filepilot-auto-view
// @name            File Pilot Auto View
// @description     Switch File Pilot's view mode based on what a folder contains
// @version         0.1.0
// @author          yo-l-an
// @github          https://github.com/yo-l-an
// @license         MIT
// @include         FPilot.exe
// @compilerOptions -lshell32 -lole32 -luuid
// ==/WindhawkMod==
// ==WindhawkModReadme==
/*
# File Pilot Auto View

![File Pilot Auto View demo](https://raw.githubusercontent.com/yo-l-an/assets/main/windhawk-mods/filepilot-auto-view/demo.gif)

Opens each folder in [File Pilot](https://filepilot.tech) with a view that
suits what's inside, like Windows Explorer does for picture folders.

## How folders are matched
Each folder you open is matched against templates, by file type. These are
the default templates: in Settings you can change them, turn them on or off,
add your own, or remove any.

| Template | File types | View | On by default |
|---|---|---|---|
| Medias | Images, videos, PDF and slides | Icons at 50% | Yes |
| Images | Images | Icons at 50% | No |
| Videos | Videos | Icons at 50% | No |
| Documents | PDF, office documents, text | Tree | No |

- A template wins when its file types make up at least 60% of the folder's
  files. If several qualify, the one with the most files wins.
- Mixed folders, and folders holding only subfolders, keep the view File
  Pilot saved for them.
- The mod reads the first 2000 items of a folder, which keeps large folders
  quick to open.

Settings also cover each template's file types, view, zoom and threshold,
the number of items read, and folders to leave alone.

### Your choices come first
- A view you pick with a File Pilot View shortcut sticks to that folder, and
  its template stops applying there. Turn off "Remember views picked with
  hotkeys" to clear them all.
- A view you pick with the mouse lasts until you leave the folder; the
  template applies again on your next visit.

## Limitations
- File Pilot's view menu can flash in the bottom-right corner after
  switching tabs, opening a new tab or window, starting File Pilot, or coming
  from a folder in File Pilot's default view.
- A new tab on the same folder (Ctrl+T) shows File Pilot's default view until
  you open another folder.
- When the mod can't set the zoom directly, it zooms the way Ctrl+scroll does,
  in 5.5% steps, so 50% becomes 49.5%.

## Compatibility
Tested with File Pilot 0.8.5 on Windows 10.

File Pilot has no plugin system, so the mod relies on details that can change
between versions: the window title, the View shortcuts and how File Pilot
stores views in memory. Your shortcuts are read from File Pilot's own
settings, so remapping them is fine. The mod only writes a view it
recognizes, so if an update changes how views are stored it switches views
with the shortcuts instead, flashing the view menu and zooming in 5.5% steps.
If folders stop switching altogether, turn the mod off in Windhawk to bring
File Pilot back to normal, and check for a mod update.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- rules:
  - - name: Medias
      $name: Name
    - enabled: true
      $name: Enabled
    - extensions: jpg jpeg jfif png gif webp bmp tif tiff heic heif avif svg ico psd raw dng cr2 cr3 nef arw orf rw2 mp4 mkv mov avi webm wmv m4v flv mpg mpeg ts mts m2ts 3gp pdf ppt pptx
      $name: Extensions
      $description: Separated by spaces or commas, without dots
    - view: custom
      $name: View
      $options:
      - details: Details
      - tree: Tree
      - list: List
      - medium: Medium icons
      - large: Large icons
      - xl: Extra large icons
      - custom: Custom zoom
    - zoom: 50
      $name: Custom zoom (%)
      $description: >-
        Used when View is Custom zoom, from 0 to 100. Below 30, File Pilot only
        has Details (0), Tree (10) and List (20), and rounds up to the next one
    - threshold: 60
      $name: Match threshold (%)
      $description: Share of the folder's files these extensions must reach
  - - name: Images
    - enabled: false
    - extensions: jpg jpeg jfif png gif webp bmp tif tiff heic heif avif svg ico psd raw dng cr2 cr3 nef arw orf rw2
    - view: custom
    - zoom: 50
    - threshold: 60
  - - name: Videos
    - enabled: false
    - extensions: mp4 mkv mov avi webm wmv m4v flv mpg mpeg ts mts m2ts 3gp
    - view: custom
    - zoom: 50
    - threshold: 60
  - - name: Documents
    - enabled: false
    - extensions: pdf doc docx xls xlsx ppt pptx odt ods odp rtf txt md csv epub
    - view: tree
    - zoom: 50
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
//   Navigator    which folder is shown, and the worker thread
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

#include <shlobj.h>

namespace {

enum class View { None = -1, Details, Tree, List, Medium, Large, XL, Count };
constexpr int kViewCount = static_cast<int>(View::Count);

// ===========================================================================
// File Pilot internals

namespace filepilot {

constexpr wchar_t kWindowClass[] = L"File Pilot";
// Titles read "<name> (<path>) - File Pilot v0.8.5". Under the user's
// profile, Desktop, and AppData's Local and Roaming, the path starts at that
// folder: "photos (Local\Temp\x\photos) - File Pilot v0.8.5".
constexpr wchar_t kTitleSuffix[] = L" - File Pilot";
constexpr wchar_t kConfigFile[] =
    L"%APPDATA%\\Voidstar\\FilePilot\\FPilot-Config.json";
// Holds "DefaultFolder", the view each navigation and new tab starts from
// unless the folder has a view saved in File Pilot.
constexpr wchar_t kUserDataFile[] =
    L"%APPDATA%\\Voidstar\\FilePilot\\FPilot-UserData.json";

// "HK_View<command>" hotkey commands in the config file, indexed by View.
// Unbound ones default to Ctrl+Shift+1..6 in the same order.
constexpr const char* kViewCommands[kViewCount] = {
    "Details", "Tree", "List", "MediumIcons", "LargeIcons", "XLIcons"};

// The zoom each View hotkey sets. Zoom alone picks the view, so writing one
// of these into a view record switches views, and values in between are
// valid too: icons go from Medium (0.3) to Extra large (1.0).
constexpr float kViewZoom[kViewCount] = {0.0f, 0.1f, 0.2f, 0.3f, 0.65f, 1.0f};

// Percent per Ctrl+wheel message, whatever the message's delta.
constexpr double kZoomPerWheelMessage = 5.5;

// Each folder view lives in a heap record: this header, then columnCount
// ViewColumns whose ratios add up to 1. Each tab keeps its record in a memory
// region of its own, mostly at one address, and resets it on each navigation
// to the folder's saved view or the default one, just before the title
// changes; switching tabs touches none. The kRecordStampSize bytes before a
// record change with each reset and view switch, but not with zooms within a
// view.
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
constexpr size_t kRecordStampSize = 8;

// Zooms are compared with this tolerance, so values that went through File
// Pilot's settings file still match.
constexpr float kZoomTolerance = 0.001f;

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

    bool HasCustomZoom() const { return IsIconView() && zoom > 0; }

    // The view record zoom that shows this layout.
    float RecordZoom() const {
        return HasCustomZoom() ? zoom / 100.0f
                               : filepilot::kViewZoom[static_cast<int>(view)];
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
constexpr wchar_t kCustomZoomView[] = L"custom";

// The layout a zoom (percent) shows. Below icons, File Pilot only has
// Details (0), Tree (10) and List (20), and takes other zooms up to the next
// one, so they become that view: the zoom then reads back as written.
Layout CustomZoomLayout(int zoom) {
    constexpr int kMediumIconsZoom = 30;
    if (zoom >= kMediumIconsZoom) {
        return {View::Medium, zoom};
    }
    return {static_cast<View>((zoom + 9) / 10)};
}

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
        if (!Wh_GetIntSetting(L"rules[%d].enabled", i)) {
            continue;  // kept as an example
        }

        // "custom" is the template's own zoom; other views keep their own
        // size and ignore it.
        Rule rule{name, {ParseView(view)}, kDefaultThreshold};
        if (view == kCustomZoomView) {
            rule.layout = CustomZoomLayout(
                std::clamp(Wh_GetIntSetting(L"rules[%d].zoom", i), 0, 100));
        }
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

// Reads one of File Pilot's files, never writing it.
std::string ReadFilePilotFile(PCWSTR unexpandedPath) {
    wchar_t path[MAX_PATH];
    if (!ExpandEnvironmentStringsW(unexpandedPath, path, MAX_PATH)) {
        return {};
    }
    HANDLE file = CreateFileW(path, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Wh_Log(L"Can't open %s", path);
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

    std::string contents = ReadFilePilotFile(filepilot::kConfigFile);
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
// Default view

// The zoom of File Pilot's default folder view. Read from its user data file,
// again only when the file changes. Used by the worker thread only.
class DefaultView {
   public:
    bool ReadZoom(float* zoom) {
        wchar_t path[MAX_PATH];
        WIN32_FILE_ATTRIBUTE_DATA info;
        if (!ExpandEnvironmentStringsW(filepilot::kUserDataFile, path,
                                       MAX_PATH) ||
            !GetFileAttributesExW(path, GetFileExInfoStandard, &info)) {
            return false;
        }
        if (CompareFileTime(&info.ftLastWriteTime, &readTime_) != 0) {
            readTime_ = info.ftLastWriteTime;
            hasZoom_ =
                Parse(config::ReadFilePilotFile(filepilot::kUserDataFile),
                      &zoom_);
        }
        *zoom = zoom_;
        return hasZoom_;
    }

   private:
    // Finds "DefaultFolder": {"ZoomLevel": 0.100000, ...}.
    static bool Parse(const std::string& contents, float* zoom) {
        size_t folder = contents.find("\"DefaultFolder\"");
        size_t level = contents.find("\"ZoomLevel\"", folder);
        size_t colon = contents.find(':', level);
        if (folder == std::string::npos || level == std::string::npos ||
            colon == std::string::npos) {
            return false;
        }
        const char* start = contents.c_str() + colon + 1;
        char* end;
        *zoom = std::strtof(start, &end);
        return end != start && *zoom >= 0.0f && *zoom <= 1.0f;
    }

    FILETIME readTime_ = {};
    float zoom_ = 0.0f;
    bool hasZoom_ = false;
};

DefaultView g_defaultView;

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

// Whether tagged input is posted or being handled: the only time the frame
// tracking in the PeekMessage hook has anything to do.
bool IsPending() {
    return g_phase != kIdle;
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

// Waits until the user lets go of the keys and buttons that opened the folder:
// File Pilot reads modifiers such as the Alt of Alt+Left with ours, and ignores
// a hotkey with an extra one, or any hotkey while a mouse button is down, such
// as the second click of a double-click.
void WaitForUserInputUp() {
    constexpr int kKeys[] = {VK_SHIFT,   VK_CONTROL, VK_MENU,     VK_LWIN,
                             VK_RWIN,    VK_LBUTTON, VK_RBUTTON,  VK_MBUTTON,
                             VK_XBUTTON1, VK_XBUTTON2};
    constexpr ULONGLONG kMaxWaitMs = 2000;
    constexpr DWORD kPollMs = 10;

    ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < kMaxWaitMs &&
           std::any_of(std::begin(kKeys), std::end(kKeys),
                       [](int vk) { return GetAsyncKeyState(vk) & 0x8000; })) {
        Sleep(kPollMs);
    }
}

// Returns false, sending nothing, once the user has left the folder.
bool PressHotkey(HWND hwnd,
                 const Hotkey& hotkey,
                 const std::function<bool()>& stillCurrent) {
    UINT down = hotkey.alt ? WM_SYSKEYDOWN : WM_KEYDOWN;
    UINT up = hotkey.alt ? WM_SYSKEYUP : WM_KEYUP;
    WaitForUserInputUp();
    if (!stillCurrent()) {
        return false;
    }
    // Down and up in one frame: File Pilot still sees the press, and the
    // preset shows for one frame less.
    fake_input::Begin();
    PostMessageW(hwnd, down, hotkey.vk, KeyLParam(hotkey, false));
    PostMessageW(hwnd, up, hotkey.vk, KeyLParam(hotkey, true));
    fake_input::Wait();
    return true;
}

// Sends Ctrl+wheel messages, one per notch (positive zooms in). File Pilot
// hit-tests the wheel against the previous frame's cursor, so a move to the
// file list gets a few frames first, and the fake cursor must not lapse in
// between. Sends nothing once the user has left the folder.
void WheelZoom(HWND hwnd,
               int notches,
               const std::function<bool()>& stillCurrent) {
    if (notches == 0) {
        return;
    }
    POINT screenPoint = main_window::ListCenter();
    POINT clientPoint = screenPoint;
    ScreenToClient(hwnd, &clientPoint);
    SHORT delta = notches > 0 ? WHEEL_DELTA : -WHEEL_DELTA;

    WaitForUserInputUp();
    if (!stillCurrent()) {
        return;
    }
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
// in writable private memory, and tells which one a navigation reset. Memory
// may be freed while it's read, so all access goes through
// Read/WriteProcessMemory, which fail instead of crashing.

class ViewRecords {
   public:
    // Records that changed in the known regions since the last title change.
    struct Changes {
        // The one record a navigation made or reset: the current folder's.
        uintptr_t current = 0;
        // Otherwise, the one record changed some other way. Usually the
        // current folder's, but a view change in the tab just left changes
        // that tab's record too, so it's only trusted for reading.
        uintptr_t changed = 0;
        // The title change these were taken at, for WriteZoom.
        unsigned title = 0;
        // When, in GetTickCount64 milliseconds: for the log.
        ULONGLONG takenAt = 0;
    };

    // Called on every title change, by the UI thread that has just reset the
    // record, so each change is told apart from the next, however busy the
    // worker is. Scans only the known regions: well under a millisecond.
    // Never guess past Changes: a wrong record changes another folder's view.
    Changes TakeChanges() {
        std::lock_guard lock(mutex_);
        titles_++;
        bool manualZoom = manualZoom_.exchange(false);
        std::vector<Seen> records = Snapshot(Scan(&regions_));
        uintptr_t current = 0, changed = 0;
        int currentCount = 0, changedCount = 0;
        for (const Seen& record : records) {
            auto seen = std::find_if(
                seen_.begin(), seen_.end(),
                [&](const Seen& s) { return s.address == record.address; });
            if (seen == seen_.end()) {
                current = record.address;
                currentCount++;
                continue;
            }
            bool stampChanged = seen->stamp != record.stamp;
            if (!stampChanged && seen->zoom == record.zoom) {
                continue;
            }
            // A zoom changed within a view keeps the stamp: only the user's
            // own zooms can do that besides a navigation.
            if (IsReset(record.stamp) && (stampChanged || !manualZoom)) {
                current = record.address;
                currentCount++;
            } else {
                changed = record.address;
                changedCount++;
            }
        }
        seen_ = std::move(records);
        if (currentCount) {
            return {currentCount == 1 ? current : 0, 0, titles_,
                    GetTickCount64()};
        }
        return {0, changedCount == 1 ? changed : 0, titles_, GetTickCount64()};
    }

    // The user zoomed (Ctrl+wheel or a View hotkey) since the last title
    // change. Called from the UI thread.
    void NoteManualZoom() { manualZoom_ = true; }

    std::vector<uintptr_t> FindEverywhere() { return Scan(nullptr); }

    std::vector<uintptr_t> FindInKnownRegions() {
        std::vector<uintptr_t> regions;
        {
            std::lock_guard lock(mutex_);
            regions = regions_;
        }
        return Scan(&regions);
    }

    // Remembers the region holding a record, File Pilot's for that tab, and
    // takes its records as seen.
    void RememberRegionOf(uintptr_t record) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(record), &info,
                          sizeof(info))) {
            return;
        }
        uintptr_t region = reinterpret_cast<uintptr_t>(info.BaseAddress);
        std::lock_guard lock(mutex_);
        if (std::find(regions_.begin(), regions_.end(), region) ==
            regions_.end()) {
            regions_.push_back(region);
        }
        seen_ = Snapshot(Scan(&regions_));
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

    // Whether the record's zoom is within kNearZoom of this one: close enough
    // that switching would only flash the view for a change hard to see.
    static bool IsNear(uintptr_t record, float zoom) {
        constexpr float kNearZoom = 0.02f;
        float current;
        return ReadZoom(record, &current) &&
               std::fabs(current - zoom) <= kNearZoom;
    }

    // Notes the folder a record's tab shows, once the record is known to be
    // the current folder's, so going back to that tab can be read without a
    // preset.
    void NoteFolder(uintptr_t record, const std::wstring& folder) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(record), &info,
                          sizeof(info))) {
            return;
        }
        uintptr_t region = reinterpret_cast<uintptr_t>(info.BaseAddress);
        std::lock_guard lock(mutex_);
        shown_[region] = folder;
    }

    // The record of the known tab last noted showing this folder, or 0 when
    // none or several tabs did, or its region holds other records too.
    uintptr_t RecordShowing(const std::wstring& folder) {
        std::vector<uintptr_t> regions;
        {
            std::lock_guard lock(mutex_);
            for (const auto& [region, shown] : shown_) {
                if (shown == folder) {
                    regions.push_back(region);
                }
            }
        }
        if (regions.size() != 1) {
            return 0;
        }
        std::vector<uintptr_t> records = Scan(&regions);
        return records.size() == 1 ? records[0] : 0;
    }

    // Writes only if the address still holds a view record, checked right
    // before writing, only a zoom File Pilot itself can hold, and only while
    // the title is still the one title stands for: TakeChanges runs under the
    // same lock, so a folder change can't slip in between. The new zoom is
    // taken as seen, so it doesn't pass for a navigation.
    bool WriteZoom(uintptr_t record, float zoom, unsigned title) {
        float current;
        if (!(zoom >= 0.0f && zoom <= 1.0f) || !ReadZoom(record, &current)) {
            return false;
        }
        std::lock_guard lock(mutex_);
        if (title != titles_) {
            return false;
        }
        SIZE_T written = 0;
        if (!WriteProcessMemory(GetCurrentProcess(),
                                reinterpret_cast<LPVOID>(record), &zoom,
                                sizeof(zoom), &written) ||
            written != sizeof(zoom)) {
            return false;
        }
        for (Seen& seen : seen_) {
            if (seen.address == record) {
                seen.zoom = zoom;
            }
        }
        return true;
    }

   private:
    // View records live in small regions: 64 KB per tab, plus one of 512 KB.
    // Larger regions (many of exactly 1 MB) hold images and caches: skipping
    // them cuts a full scan from ~150 MB to ~17 MB, and avoids paging them
    // back in.
    static constexpr size_t kMaxRegionSize = 512 << 10;

    struct Seen {
        uintptr_t address;
        uint64_t stamp;  // the kRecordStampSize bytes before the record
        float zoom;
    };

    // A navigation resets a record with the upper half of its stamp at 0;
    // view changes count up there.
    static bool IsReset(uint64_t stamp) { return (stamp >> 32) == 0; }

    static std::vector<Seen> Snapshot(const std::vector<uintptr_t>& records) {
        static_assert(sizeof(uint64_t) == filepilot::kRecordStampSize);
        std::vector<Seen> seen;
        seen.reserve(records.size());
        for (uintptr_t record : records) {
            Seen entry{record, 0, 0.0f};
            size_t read;
            if (ReadMemory(record - sizeof(entry.stamp), &entry.stamp,
                           sizeof(entry.stamp), &read) &&
                read == sizeof(entry.stamp) && ReadZoom(record, &entry.zoom)) {
                seen.push_back(entry);
            }
        }
        return seen;
    }

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
               info.Protect == PAGE_READWRITE &&
               info.RegionSize <= kMaxRegionSize;
    }

    static void ScanRegion(const MEMORY_BASIC_INFORMATION& info,
                           std::vector<BYTE>& buffer,
                           std::vector<uintptr_t>& records) {
        uintptr_t start = reinterpret_cast<uintptr_t>(info.BaseAddress);
        if (buffer.size() < info.RegionSize) {
            buffer.resize(info.RegionSize);
        }
        size_t read;
        if (!ReadMemory(start, buffer.data(), info.RegionSize, &read)) {
            return;
        }
        for (size_t i = 0; i + sizeof(filepilot::ViewRecordHeader) <= read;
             i += alignof(float)) {
            if (IsRecord(&buffer[i], read - i)) {
                records.push_back(start + i);
            }
        }
    }

    // Scans the regions with these base addresses, or all small writable
    // private regions when regions is null. The buffer only lives for the
    // scan and grows to the largest region read, so the mod holds no memory
    // between scans.
    static std::vector<uintptr_t> Scan(const std::vector<uintptr_t>* regions) {
        ULONGLONG startTime = GetTickCount64();
        std::vector<BYTE> buffer;
        std::vector<uintptr_t> records;
        MEMORY_BASIC_INFORMATION info;
        if (regions) {
            for (uintptr_t region : *regions) {
                if (VirtualQuery(reinterpret_cast<LPCVOID>(region), &info,
                                 sizeof(info)) &&
                    IsScannable(info)) {
                    ScanRegion(info, buffer, records);
                }
            }
        } else {
            for (uintptr_t address = 0;
                 VirtualQuery(reinterpret_cast<LPCVOID>(address), &info,
                              sizeof(info));
                 address = reinterpret_cast<uintptr_t>(info.BaseAddress) +
                           info.RegionSize) {
                if (IsScannable(info)) {
                    ScanRegion(info, buffer, records);
                }
            }
            Wh_Log(L"Found %zu view records in %llu ms", records.size(),
                   GetTickCount64() - startTime);
        }
        return records;
    }

    std::mutex mutex_;  // guards regions_ and seen_
    std::vector<uintptr_t> regions_;
    std::vector<Seen> seen_;  // records in the known regions last time
    std::atomic<bool> manualZoom_ = false;
    unsigned titles_ = 0;  // title changes seen by TakeChanges
    std::unordered_map<uintptr_t, std::wstring> shown_;  // folder by region
};

ViewRecords g_viewRecords;

// ===========================================================================
// Layouts

Layout ClassifyFolder(const std::wstring& path, const Config& config) {
    std::wstring pattern = path;
    if (pattern.back() != L'\\') {
        pattern += L'\\';
    }
    pattern += L'*';

    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
                                   FindExSearchNameMatch, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
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

// Presets in the order they're tried: the nearest at or below this zoom
// first, so the view only grows into place, then the others from nearest to
// farthest.
std::array<View, kViewCount> PresetsToTry(float zoom) {
    std::array<View, kViewCount> presets;
    for (int i = 0; i < kViewCount; i++) {
        presets[i] = static_cast<View>(i);
    }
    std::stable_sort(presets.begin(), presets.end(), [zoom](View a, View b) {
        return std::fabs(filepilot::kViewZoom[static_cast<int>(a)] - zoom) <
               std::fabs(filepilot::kViewZoom[static_cast<int>(b)] - zoom);
    });
    auto below = std::find_if(presets.begin(), presets.end(), [zoom](View v) {
        return filepilot::kViewZoom[static_cast<int>(v)] <=
               zoom + filepilot::kZoomTolerance;
    });
    std::rotate(presets.begin(), below, below + 1);
    return presets;
}

using RecordZooms = std::vector<std::pair<uintptr_t, float>>;

// These view records, with their zooms.
RecordZooms ZoomsOf(const std::vector<uintptr_t>& records) {
    RecordZooms zooms;
    for (uintptr_t record : records) {
        float zoom;
        if (ViewRecords::ReadZoom(record, &zoom)) {
            zooms.push_back({record, zoom});
        }
    }
    return zooms;
}

// The records in now that took this zoom since before: by changing to it, or
// by appearing with it, as a view switch can move a record.
std::vector<uintptr_t> TookZoom(const RecordZooms& before,
                                const RecordZooms& now,
                                float zoom) {
    std::vector<uintptr_t> records;
    for (const auto& [record, newZoom] : now) {
        auto old = std::find_if(
            before.begin(), before.end(),
            [&](const auto& r) { return r.first == record; });
        if (newZoom == zoom && (old == before.end() || old->second != zoom)) {
            records.push_back(record);
        }
    }
    return records;
}

// Waits for a preset press to reach the view records. File Pilot applies it
// a frame or two later, so the records from before and the known regions are
// read again until one takes the zoom, which then shows only until the next
// write. All memory is scanned again only if none does.
std::vector<uintptr_t> WaitForPreset(const RecordZooms& before, float zoom) {
    constexpr ULONGLONG kWaitMs = 250;
    constexpr DWORD kPollMs = 2;

    std::vector<uintptr_t> addresses;
    for (const auto& [record, recordZoom] : before) {
        addresses.push_back(record);
    }
    ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < kWaitMs) {
        std::vector<uintptr_t> nearby = g_viewRecords.FindInKnownRegions();
        for (uintptr_t record : addresses) {
            if (std::find(nearby.begin(), nearby.end(), record) ==
                nearby.end()) {
                nearby.push_back(record);
            }
        }
        std::vector<uintptr_t> reacted =
            TookZoom(before, ZoomsOf(nearby), zoom);
        if (!reacted.empty()) {
            return reacted;
        }
        Sleep(kPollMs);
    }
    return TookZoom(before, ZoomsOf(g_viewRecords.FindEverywhere()), zoom);
}

// Finds the current folder's record by pressing a view preset and seeing
// which record, in all memory, takes its zoom. File Pilot shows its view
// popover for a moment, so this is only for when TakeChanges finds no current
// record.
// A folder already at the first preset doesn't react to it: when that
// preset is the zoom wanted, there's nothing left to do and *alreadyThere is
// set, else the next preset is tried. Returns 0 unless exactly one record
// reacts.
uintptr_t FindRecordWithPreset(HWND hwnd,
                               const Config& config,
                               float zoom,
                               const std::function<bool()>& stillCurrent,
                               bool* alreadyThere) {
    *alreadyThere = false;
    constexpr int kPresetsTried = 2;

    RecordZooms before = ZoomsOf(g_viewRecords.FindEverywhere());
    std::array<View, kViewCount> presets = PresetsToTry(zoom);
    for (int i = 0; i < kPresetsTried; i++) {
        float presetZoom = filepilot::kViewZoom[static_cast<int>(presets[i])];
        if (!synth::PressHotkey(
                hwnd, config.hotkeys[static_cast<int>(presets[i])],
                stillCurrent)) {
            return 0;
        }

        std::vector<uintptr_t> reacted = WaitForPreset(before, presetZoom);
        if (!stillCurrent()) {
            return 0;  // what reacted may be the next folder's
        }
        if (reacted.size() == 1) {
            return reacted[0];
        }
        if (reacted.size() > 1) {
            Wh_Log(L"%zu view records took the preset", reacted.size());
            break;
        }
        if (std::fabs(presetZoom - zoom) < filepilot::kZoomTolerance) {
            *alreadyThere = true;
            return 0;
        }
        // The baseline for the next preset.
        before = ZoomsOf(g_viewRecords.FindEverywhere());
    }
    Wh_Log(L"Can't tell the view record apart");
    return 0;
}

// Applies a layout by writing its zoom into the folder's view record, which
// shows no popover, and only when the zoom differs. changes come from
// TakeChanges for this folder change. Without a current record (tab switches,
// new tabs and windows, and navigations that leave the record as it was) the
// record is found with a preset, unless the changed one already has the zoom.
bool ApplyThroughRecord(HWND hwnd,
                        const Config& config,
                        const std::wstring& folder,
                        const Layout& layout,
                        const ViewRecords::Changes& changes,
                        const std::function<bool()>& stillCurrent) {
    float zoom = layout.RecordZoom();
    uintptr_t record = changes.current;
    bool alreadyThere =
        record ? ViewRecords::IsNear(record, zoom)
               : changes.changed && ViewRecords::IsNear(changes.changed, zoom);
    // Back on a tab the mod set this folder in: its record can be read.
    uintptr_t shown =
        record || alreadyThere ? 0 : g_viewRecords.RecordShowing(folder);
    if (shown && ViewRecords::IsNear(shown, zoom)) {
        alreadyThere = true;
    }
    // Navigations and new tabs start from the default view, so a folder
    // with no record to read is already there.
    float defaultZoom;
    if (!record && !alreadyThere && g_defaultView.ReadZoom(&defaultZoom) &&
        std::fabs(defaultZoom - zoom) < filepilot::kZoomTolerance) {
        Wh_Log(L"%s: zoom %g is File Pilot's default", folder.c_str(), zoom);
        return true;
    }
    if (!record && !alreadyThere) {
        Wh_Log(L"%s: no current view record, finding it with a preset",
               folder.c_str());
        record = FindRecordWithPreset(hwnd, config, zoom, stillCurrent,
                                      &alreadyThere);
        if (record) {
            g_viewRecords.RememberRegionOf(record);
            g_viewRecords.NoteFolder(record, folder);
            alreadyThere = ViewRecords::IsNear(record, zoom);
        }
    }
    if (alreadyThere) {
        Wh_Log(L"%s: already at zoom %g", folder.c_str(), zoom);
        return true;
    }
    if (!record || !g_viewRecords.WriteZoom(record, zoom, changes.title)) {
        return false;
    }
    // Make File Pilot render the new zoom on its next frame.
    InvalidateRect(hwnd, nullptr, FALSE);
    Wh_Log(L"%s: zoom %g written to the view record at %p, %llu ms after "
           L"the folder opened",
           folder.c_str(), zoom, reinterpret_cast<void*>(record),
           GetTickCount64() - changes.takenAt);
    return true;
}

// Fallback: Ctrl+wheel up from Details (0%), in whole notches.
void SetZoomWithWheel(HWND hwnd,
                      const Config& config,
                      int zoom,
                      const std::function<bool()>& stillCurrent) {
    if (!synth::PressHotkey(hwnd,
                            config.hotkeys[static_cast<int>(View::Details)],
                            stillCurrent)) {
        return;
    }
    int notches =
        static_cast<int>(std::lround(zoom / filepilot::kZoomPerWheelMessage));
    synth::WheelZoom(hwnd, notches, stillCurrent);
}

// Every step checks stillCurrent first: once the user has left the folder,
// nothing more is sent or written.
void ApplyLayout(HWND hwnd,
                 const Config& config,
                 const std::wstring& folder,
                 const Layout& layout,
                 const ViewRecords::Changes& recordChanges,
                 const std::function<bool()>& stillCurrent) {
    if (ApplyThroughRecord(hwnd, config, folder, layout, recordChanges,
                           stillCurrent)) {
        return;
    }
    if (!stillCurrent()) {
        Wh_Log(L"%s: left before its view was applied", folder.c_str());
        return;
    }
    if (layout.HasCustomZoom()) {
        Wh_Log(L"Falling back to Ctrl+wheel zoom");
        SetZoomWithWheel(hwnd, config, layout.zoom, stillCurrent);
        return;
    }
    synth::PressHotkey(hwnd, config.hotkeys[static_cast<int>(layout.view)],
                       stillCurrent);
}

// Picks and applies the layout for a folder. recordChanges were taken when
// its title appeared; stillCurrent tells whether the user is still in it.
void ApplyFolderLayout(const std::wstring& path,
                       const ViewRecords::Changes& recordChanges,
                       const std::function<bool()>& stillCurrent) {
    std::shared_ptr<const Config> config = config::Current();
    std::wstring normalized = text::NormalizePath(path);
    if (recordChanges.current) {
        g_viewRecords.NoteFolder(recordChanges.current, normalized);
    }
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
    ApplyLayout(hwnd, *config, normalized, layout, recordChanges,
                stillCurrent);
}

// ===========================================================================
// Navigator
//
// Tracks the folder shown in the title and hands each new one to a worker
// thread, which resolves its full path and applies its layout. A generation
// number lets the worker drop folders the user has already left.

class Navigator {
   public:
    // When File Pilot exits, the mod is unloaded without Wh_ModUninit and its
    // threads are already gone; destroying a joinable thread would abort().
    ~Navigator() {
        if (worker_.joinable()) {
            worker_.detach();
        }
    }

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
        // Taken on every folder change, so the next one's changes stand out.
        ViewRecords::Changes changes = g_viewRecords.TakeChanges();
        resolvedPath_ = text::IsAbsolutePath(path) ? path : L"";
        if (!resolvedPath_.empty()) {
            lastResolvedPath_ = resolvedPath_;
        }
        generation_++;
        if (apply && !path.empty()) {
            pendingPath_ = path;
            pendingChanges_ = changes;
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

    // A title path such as "Desktop\x" starts with a known folder's name:
    // try "<that folder>\x" for each known folder of that name. File Pilot
    // uses the first four; the others cover what it may add.
    static std::wstring FindUnderKnownFolder(const std::wstring& tail) {
        static const KNOWNFOLDERID* const kFolders[] = {
            &FOLDERID_Desktop,  &FOLDERID_LocalAppData,
            &FOLDERID_RoamingAppData, &FOLDERID_Profile,
            &FOLDERID_Documents, &FOLDERID_Downloads,
            &FOLDERID_Pictures, &FOLDERID_Music,
            &FOLDERID_Videos,   &FOLDERID_LocalAppDataLow,
        };

        size_t slash = tail.find(L'\\');
        size_t nameLength = slash == std::wstring::npos ? tail.size() : slash;
        for (const KNOWNFOLDERID* folder : kFolders) {
            PWSTR path = nullptr;
            std::wstring candidate;
            if (SUCCEEDED(SHGetKnownFolderPath(*folder, 0, nullptr, &path))) {
                candidate = path;
            }
            CoTaskMemFree(path);
            std::wstring name = text::LastComponent(candidate);
            if (name.empty() || !text::EqualsNoCase(name.data(), name.size(),
                                                   tail.data(), nameLength)) {
                continue;
            }
            candidate += tail.substr(nameLength);
            if (IsDirectory(candidate)) {
                return candidate;
            }
        }
        return {};
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

    // Turns a title path into a full path.
    std::wstring Resolve(const std::wstring& titlePath) {
        if (text::IsAbsolutePath(titlePath)) {
            return titlePath;
        }
        std::wstring path = FindUnderKnownFolder(titlePath);
        if (path.empty()) {
            std::wstring lastFolder;
            {
                std::lock_guard lock(mutex_);
                lastFolder = lastResolvedPath_;
            }
            path = FindUnderFolder(titlePath, lastFolder);
        }
        if (path.empty()) {
            Wh_Log(L"Can't resolve %s", titlePath.c_str());
        }
        return path;
    }

    void Process(const std::wstring& titlePath,
                 const ViewRecords::Changes& changes,
                 unsigned generation) {
        std::wstring path = Resolve(titlePath);
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
        ApplyFolderLayout(path, changes,
                          [&] { return IsCurrent(generation); });
    }

    void Run() {
        std::unique_lock lock(mutex_);
        while (true) {
            wake_.wait(lock, [this] { return stop_ || hasPending_; });
            if (stop_) {
                return;
            }
            std::wstring path = pendingPath_;
            ViewRecords::Changes changes = pendingChanges_;
            unsigned generation = generation_;
            hasPending_ = false;
            lock.unlock();
            Process(path, changes, generation);
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
    ViewRecords::Changes pendingChanges_;
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

SetWindowTextW_t SetWindowTextW_Original;
GetKeyState_t GetKeyState_Original;
PeekMessageW_t PeekMessageW_Original;
TranslateMessage_t TranslateMessage_Original;
GetCursorPos_t GetCursorPos_Original;
WindowFromPoint_t WindowFromPoint_Original;

bool IsRealKeyDown(int vk) {
    return GetKeyState_Original(vk) & 0x8000;
}

// The View hotkey a real key press matches, or View::None.
View ViewOfHotkey(const MSG& msg, const Config& config) {
    bool isFirstKeyDown =
        (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) &&
        !(msg.lParam & 0x40000000);
    if (!isFirstKeyDown) {
        return View::None;
    }
    for (int i = 0; i < kViewCount; i++) {
        const Hotkey& hotkey = config.hotkeys[i];
        if (hotkey.vk == msg.wParam &&
            hotkey.ctrl == IsRealKeyDown(VK_CONTROL) &&
            hotkey.shift == IsRealKeyDown(VK_SHIFT) &&
            hotkey.alt == IsRealKeyDown(VK_MENU)) {
            return static_cast<View>(i);
        }
    }
    return View::None;
}

// The user's own zooms: noted so they don't pass for a navigation, and View
// hotkeys are remembered for the folder.
void OnUserInput(const MSG& msg) {
    if (msg.message == WM_MOUSEWHEEL &&
        (GET_KEYSTATE_WPARAM(msg.wParam) & MK_CONTROL)) {
        g_viewRecords.NoteManualZoom();
        return;
    }
    std::shared_ptr<const Config> config = config::Current();
    View view = ViewOfHotkey(msg, *config);
    if (view == View::None) {
        return;
    }
    g_viewRecords.NoteManualZoom();
    std::wstring folder = g_navigator.CurrentFolder();
    if (config->rememberManual && !folder.empty()) {
        Wh_Log(L"Remembering %s for %s", ViewName(view), folder.c_str());
        g_manualViews.Remember(text::NormalizePath(folder), view);
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
    bool isUiThread = fake_input::IsPending() && main_window::IsUiThread();
    if (isUiThread) {
        fake_input::OnFrameDone();
    }
    BOOL result =
        PeekMessageW_Original(msg, hwnd, filterMin, filterMax, removeMsg);
    if (result && (removeMsg & PM_REMOVE) && msg->hwnd &&
        msg->hwnd == main_window::Handle()) {
        if (!fake_input::OnMessage(*msg)) {
            OnUserInput(*msg);
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
