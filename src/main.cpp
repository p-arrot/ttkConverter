#include "ttk.hpp"
#include "ui_ids.h"

#include <windows.h>
#include <cderr.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwchar>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

namespace {

constexpr wchar_t windowClass[] = L"TtkConverter.Main";
constexpr UINT_PTR pollingTimer = 1;
constexpr int iconResource = 101;
constexpr int welcomeIconResource = 102;
constexpr int brandIconResource = 103;
constexpr int headingFontResource = 104;
constexpr wchar_t headingFace[] = L"ttk Rounded";

enum Command {
    Add = 1000, Filter, Queue, TaskActions, Remove, Preset, Quality, Advanced, OutputPath,
    Browse, Start, StartSelected, Stop, Retry, Open, Details,
};

enum class Phase { Checking, Ready, Converting, Done, Failed, Cancelled, Unsupported };
using TaskId = unsigned int;
using ttk::Kind;
namespace fs = ttk::fs;

struct Task {
    ttk::Job job;
    ttk::SourceInfo source;
    Phase phase = Phase::Checking;
    std::wstring status = L"等待检查";
    ttk::Result result;
};

struct WorkResult {
    ttk::SourceInfo source;
    ttk::Result conversion;
};

constexpr std::array<Kind, 5> filters{
    Kind::Unknown, Kind::Video, Kind::Audio, Kind::Image, Kind::Pdf
};

const wchar_t* kindName(Kind kind) {
    switch (kind) {
    case Kind::Video: return L"视频";
    case Kind::Audio: return L"音频";
    case Kind::Image: return L"图片";
    case Kind::Pdf: return L"PDF 转 Word";
    default: return L"未知";
    }
}

const ttk::Preset* findPreset(const std::wstring& id) {
    const auto& presets = ttk::presets();
    const auto found = std::find_if(presets.begin(), presets.end(), [&](const auto& preset) {
        return preset.id == id;
    });
    return found == presets.end() ? nullptr : &*found;
}

bool accepts(Kind source, Kind target) {
    return source == target || (source == Kind::Video && target == Kind::Audio);
}

bool commonPreset(const ttk::Preset& preset) {
    constexpr std::wstring_view common[]{
        L"mp4-h264", L"mp4-h265", L"mp4-av1", L"mkv-h264", L"mov-h264", L"webm-vp9",
        L"mp3", L"wav", L"m4a", L"flac", L"jpg", L"png", L"webp", L"gif", L"ico",
        L"pdf-docx"
    };
    return std::find(std::begin(common), std::end(common), preset.id) != std::end(common);
}

struct NumericOption {
    int id;
    int labelId;
    const wchar_t* label;
    int maximum;
    int ttk::Options::*member;
};

constexpr NumericOption numbers[] = {
    {UI_ADVANCED_WIDTH, UI_ADVANCED_WIDTH_LABEL, L"宽度", 16384, &ttk::Options::width},
    {UI_ADVANCED_HEIGHT, UI_ADVANCED_HEIGHT_LABEL, L"高度", 16384, &ttk::Options::height},
    {UI_ADVANCED_VIDEO_RATE, UI_ADVANCED_VIDEO_RATE_LABEL, L"视频码率", 500000, &ttk::Options::videoKbps},
    {UI_ADVANCED_AUDIO_RATE, UI_ADVANCED_AUDIO_RATE_LABEL, L"音频码率", 2000, &ttk::Options::audioKbps},
    {UI_ADVANCED_CHANNELS, UI_ADVANCED_CHANNELS_LABEL, L"声道", 8, &ttk::Options::channels},
    {UI_ADVANCED_THREADS, UI_ADVANCED_THREADS_LABEL, L"线程", 64, &ttk::Options::threads},
};

struct AdvancedDraft {
    Kind kind;
    ttk::Options options;
};

bool numericEnabled(int id, Kind target) {
    const bool video = target == Kind::Video;
    const bool audio = video || target == Kind::Audio;
    if (id == UI_ADVANCED_WIDTH || id == UI_ADVANCED_HEIGHT) return video || target == Kind::Image;
    return id == UI_ADVANCED_VIDEO_RATE ? video : audio;
}

bool highContrast() {
    HIGHCONTRASTW settings{sizeof(settings), 0, nullptr};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(settings), &settings, 0) &&
           (settings.dwFlags & HCF_HIGHCONTRASTON);
}

constexpr COLORREF canvasColor = RGB(249, 245, 240);
constexpr COLORREF paperColor = RGB(255, 253, 250);
constexpr COLORREF dropColor = RGB(252, 245, 238);
constexpr COLORREF inkColor = RGB(48, 36, 29);
constexpr COLORREF mutedColor = RGB(108, 88, 75);
constexpr COLORREF borderColor = RGB(226, 214, 204);
constexpr COLORREF accentColor = RGB(164, 67, 30);
constexpr COLORREF tintColor = RGB(249, 228, 212);

void fillBox(HDC dc, const RECT& area, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    if (!brush) return;
    FillRect(dc, &area, brush);
    DeleteObject(brush);
}

void roundedBox(HDC dc, const RECT& area, COLORREF fill, COLORREF line, int diameter,
                int penStyle = PS_SOLID) {
    if (IsRectEmpty(&area)) return;
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(penStyle, 1, line);
    if (brush && pen) {
        const auto previousBrush = SelectObject(dc, brush);
        const auto previousPen = SelectObject(dc, pen);
        RoundRect(dc, area.left, area.top, area.right, area.bottom, diameter, diameter);
        SelectObject(dc, previousPen);
        SelectObject(dc, previousBrush);
    }
    if (pen) DeleteObject(pen);
    if (brush) DeleteObject(brush);
}

void centeredText(HDC dc, RECT area, const wchar_t* text, COLORREF color, HFONT font) {
    const auto previousFont = font ? SelectObject(dc, font) : nullptr;
    const auto oldColor = SetTextColor(dc, color);
    const int oldMode = SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text, -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SetBkMode(dc, oldMode);
    SetTextColor(dc, oldColor);
    if (previousFont) SelectObject(dc, previousFont);
}

std::wstring windowText(HWND control) {
    std::wstring text(GetWindowTextLengthW(control) + 1, L'\0');
    text.resize(GetWindowTextW(control, text.data(), static_cast<int>(text.size())));
    return text;
}

void showText(HWND control, const std::wstring& text) {
    SetWindowTextW(control, text.c_str());
}

void addChoice(HWND combo, const wchar_t* text) {
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
}

void selectChoice(HWND combo, int index) {
    SendMessageW(combo, CB_SETCURSEL, index, 0);
}

int choice(HWND combo) {
    return static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
}

void setCheck(HWND control, bool value) {
    SendMessageW(control, BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0);
}

bool isChecked(HWND control) {
    return SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

unsigned windowDpi(HWND window) {
    using GetWindowDpi = UINT (WINAPI*)(HWND);
    const auto address = GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    GetWindowDpi modern = nullptr;
    static_assert(sizeof(modern) == sizeof(address));
    std::memcpy(&modern, &address, sizeof(modern));
    if (modern) {
        if (const auto dpi = modern(window)) return dpi;
    }
    HDC dc = GetDC(window);
    const unsigned dpi = dc ? static_cast<unsigned>(GetDeviceCaps(dc, LOGPIXELSX)) : 96;
    if (dc) ReleaseDC(window, dc);
    return dpi;
}

int CALLBACK markFontPresent(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM context) {
    *reinterpret_cast<bool*>(context) = true;
    return 0;
}

const wchar_t* chineseUiFace(HWND window) {
    HDC dc = GetDC(window);
    if (!dc) return nullptr;
    const wchar_t* family = nullptr;
    for (const auto* candidate : {L"Microsoft YaHei UI", L"Microsoft YaHei"}) {
        LOGFONTW query{};
        query.lfCharSet = DEFAULT_CHARSET;
        lstrcpynW(query.lfFaceName, candidate, LF_FACESIZE);
        bool found = false;
        EnumFontFamiliesExW(dc, &query, markFontPresent, reinterpret_cast<LPARAM>(&found), 0);
        if (found) {
            family = candidate;
            break;
        }
    }
    ReleaseDC(window, dc);
    return family;
}

HANDLE loadHeadingFont(HINSTANCE instance) {
    const HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(headingFontResource), RT_RCDATA);
    if (!resource) return nullptr;
    const HGLOBAL data = LoadResource(instance, resource);
    if (!data) return nullptr;
    void* font = LockResource(data);
    const DWORD size = SizeofResource(instance, resource);
    if (!font || !size) return nullptr;
    DWORD installed = 0;
    return AddFontMemResourceEx(font, size, nullptr, &installed);
}

RECT fitToWorkArea(RECT window) {
    MONITORINFO monitor{sizeof(monitor), {}, {}, 0};
    if (!GetMonitorInfoW(MonitorFromRect(&window, MONITOR_DEFAULTTONEAREST), &monitor)) {
        return window;
    }
    const auto& work = monitor.rcWork;
    const LONG width = std::min(window.right - window.left, work.right - work.left);
    const LONG height = std::min(window.bottom - window.top, work.bottom - work.top);
    const LONG left = std::clamp(window.left, work.left, work.right - width);
    const LONG top = std::clamp(window.top, work.top, work.bottom - height);
    return {left, top, left + width, top + height};
}

fs::path settingsFile() {
    std::array<wchar_t, MAX_PATH> directory{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT,
                                directory.data()))) return {};
    return fs::path(directory.data()) / L"ttkConverter" / L"settings.ini";
}

std::wstring readSetting(const fs::path& file, const wchar_t* key) {
    if (file.empty()) return {};
    std::wstring text(32768, L'\0');
    text.resize(GetPrivateProfileStringW(L"转换设置", key, L"", text.data(),
                                         static_cast<DWORD>(text.size()), file.c_str()));
    return text;
}

void writeSetting(const fs::path& file, const wchar_t* key, const std::wstring& value) {
    if (file.empty()) return;
    std::error_code error;
    fs::create_directories(file.parent_path(), error);
    if (error) return;
    if (!fs::exists(file, error)) {
        HANDLE fresh = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fresh != INVALID_HANDLE_VALUE) {
            const wchar_t bom = 0xfeff;
            DWORD bytes = 0;
            WriteFile(fresh, &bom, sizeof(bom), &bytes, nullptr);
            CloseHandle(fresh);
        }
    }
    WritePrivateProfileStringW(L"转换设置", key, value.c_str(), file.c_str());
}

class Window {
public:
    Window() = default;
    ~Window() {
        cancel_.store(true);
        if (worker_.joinable()) worker_.join();
        if (font_) DeleteObject(font_);
        if (titleFont_) DeleteObject(titleFont_);
        if (strongFont_) DeleteObject(strongFont_);
        if (emptyFont_) DeleteObject(emptyFont_);
        if (captionFont_) DeleteObject(captionFont_);
        if (headingFontHandle_) RemoveFontMemResourceEx(headingFontHandle_);
        clearBrushes();
    }
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        if (message == WM_NCCREATE) {
            auto* self = static_cast<Window*>(
                reinterpret_cast<const CREATESTRUCTW*>(lp)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        return self ? self->dispatch(message, wp, lp) : DefWindowProcW(window, message, wp, lp);
    }

private:
    HWND window_ = nullptr;
    HINSTANCE instance_ = GetModuleHandleW(nullptr);
    unsigned dpi_ = 96;
    HFONT font_ = nullptr;
    HFONT titleFont_ = nullptr;
    HFONT strongFont_ = nullptr;
    HFONT emptyFont_ = nullptr;
    HFONT captionFont_ = nullptr;
    HANDLE headingFontHandle_ = nullptr;
    HBRUSH canvasBrush_ = nullptr;
    HBRUSH paperBrush_ = nullptr;
    HBRUSH dropBrush_ = nullptr;
    bool contrast_ = false;
    RECT queueBox_{};
    RECT dropBox_{};
    RECT settingsBox_{};
    RECT footerBox_{};
    RECT brandBox_{};
    RECT illustrationBox_{};
    std::array<RECT, 3> stepBoxes_{};
    int step_ = 0;
    HWND title_ = nullptr;
    HWND instruction_ = nullptr;
    HWND queueTitle_ = nullptr;
    HWND add_ = nullptr;
    HWND filter_ = nullptr;
    HWND actions_ = nullptr;
    HWND queue_ = nullptr;
    HWND empty_ = nullptr;
    HWND emptyFormats_ = nullptr;
    HWND presetLabel_ = nullptr;
    HWND preset_ = nullptr;
    HWND fixedPreset_ = nullptr;
    HWND qualityLabel_ = nullptr;
    HWND quality_ = nullptr;
    HWND advanced_ = nullptr;
    HWND pathLabel_ = nullptr;
    HWND path_ = nullptr;
    HWND browse_ = nullptr;
    HWND status_ = nullptr;
    HWND start_ = nullptr;
    HWND stop_ = nullptr;
    HWND hint_ = nullptr;
    ttk::Options draftOptions_;

    fs::path settings_ = settingsFile();
    std::map<TaskId, Task> tasks_;
    std::map<TaskId, int> visibleRows_;
    std::map<Kind, std::wstring> savedPresets_;
    TaskId nextId_ = 1;
    std::deque<TaskId> probes_;
    std::deque<TaskId> scheduled_;
    std::vector<const ttk::Preset*> choices_;
    bool morePresets_ = false;
    bool rebuilding_ = false;
    bool closing_ = false;
    bool probing_ = false;
    bool stopping_ = false;
    TaskId active_ = 0;
    std::thread worker_;
    ttk::Cancel cancel_{false};
    std::mutex workMutex_;
    std::optional<WorkResult> completed_;
    int progress_ = -1;
    std::wstring progressText_;

    int px(int logical) const {
        return MulDiv(logical, static_cast<int>(dpi_), 96);
    }

    HWND control(const wchar_t* type, const wchar_t* text, DWORD style, int id = 0,
                 DWORD exStyle = 0) const {
        return CreateWindowExW(exStyle, type, text, WS_CHILD | WS_VISIBLE | style,
                               0, 0, 0, 0, window_, reinterpret_cast<HMENU>(
                                   static_cast<INT_PTR>(id)), instance_, nullptr);
    }

    HWND button(const wchar_t* label, int id, DWORD additional = 0) const {
        return control(WC_BUTTONW, label, WS_TABSTOP | additional, id);
    }

    HWND label(const wchar_t* text, DWORD style = SS_LEFT) const {
        return control(WC_STATICW, text, style | SS_NOPREFIX | SS_ENDELLIPSIS);
    }

    HWND combo(int id) const {
        return control(WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST |
                       CBS_HASSTRINGS, id);
    }

    void createControls();
    void updateFonts();
    void updateCaptionIcon() const;
    void clearBrushes();
    void updateColors();
    void layout();
    void paint();
    void paintAction(const DRAWITEMSTRUCT& item) const;
    LRESULT paintText(HDC dc, HWND control) const;
    LRESULT dispatch(UINT message, WPARAM wp, LPARAM lp);
    LRESULT notify(const NMHDR& message);
    void command(int id, int code);
    std::vector<TaskId> selection() const;
    void renderRows();
    void refreshRow(TaskId id);
    void syncSelection(bool allPresets = false);
    void refreshButtons();
    void refreshStatus();
    void pickFiles();
    void dropped(HDROP drop);
    void addFiles(const std::vector<fs::path>& files);
    void pickDirectory();
    void applySettings();
    void editAdvanced();
    static INT_PTR CALLBACK advancedDialog(HWND dialog, UINT message, WPARAM wp, LPARAM lp);
    void requestStart(bool selectedOnly);
    void startWork();
    void finishWork();
    void stopBatch();
    void taskActions();
    void retry();
    void removeSelection();
    void openResult();
    void showDetails();
    void close();
};

void Window::createControls() {
    dpi_ = windowDpi(window_);
    updateColors();
    headingFontHandle_ = loadHeadingFont(instance_);
    updateCaptionIcon();
    title_ = label(L"ttkConverter");
    instruction_ = label(L"在本机转换视频、音频、图片和 PDF");
    queueTitle_ = label(L"转换队列");
    add_ = button(L"添加文件…", Add, BS_OWNERDRAW);
    filter_ = combo(Filter);
    addChoice(filter_, L"全部任务");
    for (std::size_t index = 1; index < filters.size(); ++index) {
        addChoice(filter_, kindName(filters[index]));
        savedPresets_[filters[index]] = readSetting(settings_, kindName(filters[index]));
    }
    selectChoice(filter_, 0);
    actions_ = button(L"任务操作…", TaskActions);
    queue_ = control(WC_LISTVIEWW, L"文件任务队列", WS_TABSTOP | LVS_REPORT |
                     LVS_SHOWSELALWAYS, Queue);
    ListView_SetExtendedListViewStyle(queue_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                                    LVS_EX_LABELTIP);
    constexpr const wchar_t* columns[] = {L"文件", L"输出格式", L"状态"};
    for (int index = 0; index < 3; ++index) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<wchar_t*>(columns[index]);
        column.iSubItem = index;
        ListView_InsertColumn(queue_, index, &column);
    }
    empty_ = label(L"把文件拖到这里", SS_CENTER);
    emptyFormats_ = label(L"支持同时添加视频、音频、图片和 PDF", SS_CENTER);
    presetLabel_ = label(L"输出格式");
    preset_ = combo(Preset);
    fixedPreset_ = label(L"Word 文档（.docx）");
    qualityLabel_ = label(L"质量");
    quality_ = combo(Quality);
    addChoice(quality_, L"快速");
    addChoice(quality_, L"平衡");
    addChoice(quality_, L"较小体积");
    selectChoice(quality_, 1);
    advanced_ = button(L"高级设置…", Advanced);
    hint_ = label(L"");
    pathLabel_ = label(L"保存到");
    path_ = control(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL,
                    OutputPath, WS_EX_CLIENTEDGE);
    SendMessageW(path_, EM_SETLIMITTEXT, 32767, 0);
    SendMessageW(path_, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"默认在原文件旁创建「ttkConverter 输出」"));
    showText(path_, readSetting(settings_, L"保存目录"));
    browse_ = button(L"浏览…", Browse);
    status_ = label(L"先添加文件，即可开始");
    stop_ = button(L"停止批次", Stop);
    start_ = button(L"开始转换", Start, BS_OWNERDRAW);
    updateFonts();
    DragAcceptFiles(window_, TRUE);
    SetTimer(window_, pollingTimer, 250, nullptr);
    syncSelection();
    refreshStatus();
    layout();
}

void Window::updateFonts() {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    LOGFONTW face{};
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
        face = metrics.lfMessageFont;
    }
    if (const auto* family = chineseUiFace(window_)) {
        lstrcpynW(face.lfFaceName, family, LF_FACESIZE);
        face.lfCharSet = DEFAULT_CHARSET;
    }
    if (!face.lfFaceName[0]) lstrcpyW(face.lfFaceName, L"Segoe UI");
    face.lfWeight = FW_NORMAL;
    face.lfHeight = -px(13);
    HFONT caption = CreateFontIndirectW(&face);
    face.lfHeight = -px(14);
    HFONT normal = CreateFontIndirectW(&face);
    face.lfWeight = FW_SEMIBOLD;
    HFONT strong = CreateFontIndirectW(&face);
    face.lfHeight = -px(21);
    HFONT empty = CreateFontIndirectW(&face);
    face.lfHeight = -px(28);
    if (headingFontHandle_) {
        lstrcpynW(face.lfFaceName, headingFace, LF_FACESIZE);
        face.lfCharSet = DEFAULT_CHARSET;
        face.lfWeight = FW_BOLD;
    }
    HFONT heading = CreateFontIndirectW(&face);
    for (HWND child = GetWindow(window_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        const auto font = child == title_ ? heading : child == empty_ ? empty :
            child == instruction_ || child == emptyFormats_ || child == hint_ || child == status_
                ? caption :
            child == presetLabel_ || child == queueTitle_ || child == pathLabel_ ? strong : normal;
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    if (font_) DeleteObject(font_);
    if (titleFont_) DeleteObject(titleFont_);
    if (strongFont_) DeleteObject(strongFont_);
    if (emptyFont_) DeleteObject(emptyFont_);
    if (captionFont_) DeleteObject(captionFont_);
    font_ = normal;
    titleFont_ = heading;
    strongFont_ = strong;
    emptyFont_ = empty;
    captionFont_ = caption;
}

void Window::updateCaptionIcon() const {
    const auto size = px(16);
    auto icon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(brandIconResource),
                                              IMAGE_ICON, size, size, LR_SHARED));
    if (!icon) {
        icon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(iconResource),
                                            IMAGE_ICON, size, size, LR_SHARED));
    }
    if (icon) SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
}

void Window::layout() {
    RECT client{};
    GetClientRect(window_, &client);
    const int width = MulDiv(client.right, 96, static_cast<int>(dpi_));
    const int height = MulDiv(client.bottom, 96, static_cast<int>(dpi_));
    if (width <= 0 || height <= 0) return;
    const auto place = [this](HWND item, int x, int y, int w, int h) {
        MoveWindow(item, px(x), px(y), px(std::max(w, 0)), px(std::max(h, 0)), TRUE);
    };
    const auto box = [this](int x, int y, int w, int h) {
        return RECT{px(x), px(y), px(x + w), px(y + h)};
    };
    const auto visible = [](HWND item, bool shown) {
        ShowWindow(item, shown ? SW_SHOWNA : SW_HIDE);
    };
    const bool compact = height < 550 || width < 700;
    const int pad = compact ? 15 : 24;
    const int content = width - 2 * pad;
    const int headerY = compact ? 11 : 19;
    const int stepY = headerY + 63;
    const bool hasTasks = !tasks_.empty();
    const int queueY = hasTasks ? stepY + 45 : stepY + 2;
    const int footerY = height - pad - 87;
    const auto ids = selection();
    const bool showSettings = !choices_.empty();
    const int presetIndex = choice(preset_);
    const bool fixedFormat = presetIndex >= 0 &&
        static_cast<std::size_t>(presetIndex) < choices_.size() &&
        choices_[static_cast<std::size_t>(presetIndex)]->kind == Kind::Pdf;
    const bool batchActive = !scheduled_.empty() || (active_ && !probing_);
    const bool showRows = hasTasks && ListView_GetItemCount(queue_) > 0;
    const int settingsY = footerY - 80;
    const int queueBottom = !hasTasks ? height - pad : (showSettings ? settingsY : footerY) - 10;

    brandBox_ = box(pad, headerY + 2, 51, 51);
    place(title_, pad + 64, headerY - 2, content - 64, 41);
    place(instruction_, pad + 65, headerY + 39, content - 65, 19);
    const int stepWidth = (content - 16) / 3;
    for (int index = 0; index < 3; ++index) {
        stepBoxes_[index] = hasTasks ? box(pad + index * (stepWidth + 8), stepY,
            index == 2 ? content - 2 * (stepWidth + 8) : stepWidth, 32) : RECT{};
    }
    queueBox_ = box(pad, queueY, content, queueBottom - queueY);
    const int queueX = pad + 12;
    if (hasTasks) {
        place(queueTitle_, queueX, queueY + 13, 76, 22);
        place(filter_, queueX + 79, queueY + 10, compact ? 107 : 134, 160);
        place(actions_, width - pad - 103, queueY + 8, 91, 29);
        place(add_, width - pad - (ids.empty() ? 115 : 214), queueY + 8, 103, 29);
        place(queue_, pad + 1, queueY + 44, content - 2, queueBottom - queueY - 47);
        const int output = compact ? 138 : 189;
        const int state = compact ? 156 : 194;
        ListView_SetColumnWidth(queue_, 0, px(std::max(80, content - output - state - 21)));
        ListView_SetColumnWidth(queue_, 1, px(output));
        ListView_SetColumnWidth(queue_, 2, px(state));
        dropBox_ = {};
        illustrationBox_ = {};
        place(empty_, queueX, queueY + (queueBottom - queueY) / 2, content - 24, 28);
        showText(empty_, L"这个分类没有文件");
    } else {
        dropBox_ = box(pad + 11, queueY + 11, content - 22, queueBottom - queueY - 22);
        const int artSize = compact ? 76 : 110;
        const int groupTop = queueY + std::max(15, (queueBottom - queueY - artSize - 104) / 2);
        illustrationBox_ = box((width - artSize) / 2, groupTop, artSize, artSize);
        const int copyTop = groupTop + artSize + 7;
        place(empty_, queueX, copyTop, content - 24, 29);
        place(emptyFormats_, queueX, copyTop + 32, content - 24, 21);
        place(add_, (width - 130) / 2, copyTop + 59, 130, 36);
        showText(empty_, L"把文件拖到这里");
    }
    visible(queueTitle_, hasTasks);
    visible(filter_, hasTasks);
    visible(actions_, hasTasks && !ids.empty());
    visible(queue_, showRows);
    visible(empty_, !showRows);
    visible(emptyFormats_, !hasTasks);

    settingsBox_ = showSettings ? box(pad, settingsY, content, 70) : RECT{};
    const int inner = pad + 12;
    const int right = width - pad - 12;
    const int qualityWidth = compact ? 83 : 95;
    place(presetLabel_, inner, settingsY + 13, 70, 21);
    place(preset_, inner + 72, settingsY + 10, right - inner - 72 - qualityWidth - 154, 290);
    place(fixedPreset_, inner + 72, settingsY + 13, right - inner - 72 - 104, 21);
    place(qualityLabel_, right - qualityWidth - 140, settingsY + 13, 36, 21);
    place(quality_, right - qualityWidth - 103, settingsY + 10, qualityWidth, 110);
    place(advanced_, right - 94, settingsY + 8, 94, 29);
    place(hint_, inner, settingsY + 45, right - inner, 19);
    for (HWND item : {presetLabel_, advanced_, hint_}) visible(item, showSettings);
    for (HWND item : {preset_, qualityLabel_, quality_}) visible(item, showSettings && !fixedFormat);
    visible(fixedPreset_, showSettings && fixedFormat);

    footerBox_ = hasTasks ? box(pad, footerY, content, 87) : RECT{};
    place(pathLabel_, inner, footerY + 14, 58, 22);
    place(path_, inner + 61, footerY + 10, right - inner - 61 - 79, 28);
    place(browse_, right - 70, footerY + 9, 70, 30);
    place(status_, inner, footerY + 57, right - inner - (batchActive ? 218 : 130), 22);
    place(stop_, right - 203, footerY + 48, 81, 31);
    place(start_, right - 114, footerY + 46, 114, 35);
    for (HWND item : {pathLabel_, path_, browse_, status_, start_}) visible(item, hasTasks);
    visible(stop_, hasTasks && batchActive);
    InvalidateRect(window_, nullptr, FALSE);
}

void Window::clearBrushes() {
    for (const HBRUSH brush : {canvasBrush_, paperBrush_, dropBrush_}) {
        if (brush) DeleteObject(brush);
    }
}

void Window::updateColors() {
    clearBrushes();
    contrast_ = highContrast();
    const auto face = GetSysColor(COLOR_WINDOW);
    canvasBrush_ = CreateSolidBrush(contrast_ ? face : canvasColor);
    paperBrush_ = CreateSolidBrush(contrast_ ? face : paperColor);
    dropBrush_ = CreateSolidBrush(contrast_ ? face : dropColor);
}

void Window::paint() {
    PAINTSTRUCT state{};
    const HDC dc = BeginPaint(window_, &state);
    RECT client{};
    GetClientRect(window_, &client);
    const auto background = contrast_ ? GetSysColor(COLOR_WINDOW) : canvasColor;
    const auto surface = contrast_ ? background : paperColor;
    const auto border = contrast_ ? GetSysColor(COLOR_WINDOWTEXT) : borderColor;
    const auto text = contrast_ ? GetSysColor(COLOR_WINDOWTEXT) : inkColor;
    const auto selectedFill = contrast_ ? GetSysColor(COLOR_HIGHLIGHT) : tintColor;
    const auto selectedText = contrast_ ? GetSysColor(COLOR_HIGHLIGHTTEXT) : accentColor;
    fillBox(dc, client, background);
    roundedBox(dc, brandBox_, contrast_ ? background : tintColor, border, px(14));
    for (const auto& card : {queueBox_, settingsBox_, footerBox_}) {
        roundedBox(dc, card, surface, border, px(13));
    }

    const auto drawIcon = [&](const RECT& region, int id, int inset) {
        if (IsRectEmpty(&region)) return;
        const int size = static_cast<int>(region.right - region.left) - 2 * inset;
        auto icon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(id), IMAGE_ICON,
                                                  size, size, LR_SHARED));
        if (!icon && id != iconResource) {
            icon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(iconResource),
                                                IMAGE_ICON, size, size, LR_SHARED));
        }
        if (icon) DrawIconEx(dc, region.left + inset, region.top + inset, icon, size, size, 0,
                             nullptr, DI_NORMAL);
    };
    drawIcon(brandBox_, brandIconResource, px(6));

    if (!IsRectEmpty(&dropBox_)) {
        roundedBox(dc, dropBox_, contrast_ ? background : dropColor,
                   contrast_ ? border : RGB(198, 151, 120), px(11), PS_DOT);
        drawIcon(illustrationBox_, welcomeIconResource, 0);
    }
    constexpr const wchar_t* steps[]{L"1  添加文件", L"2  选择格式", L"3  开始转换"};
    for (int index = 0; index < 3; ++index) {
        const auto& rect = stepBoxes_[index];
        if (IsRectEmpty(&rect)) continue;
        const bool current = index == step_;
        roundedBox(dc, rect, current ? selectedFill : surface,
                   current && !contrast_ ? RGB(226, 183, 155) : border, px(9));
        centeredText(dc, rect, steps[index], current ? selectedText : text,
                     current ? strongFont_ : font_);
    }
    EndPaint(window_, &state);
}

void Window::paintAction(const DRAWITEMSTRUCT& item) const {
    const bool add = item.CtlID == Add;
    const bool primary = !add || tasks_.empty();
    const bool disabled = item.itemState & ODS_DISABLED;
    const bool down = item.itemState & ODS_SELECTED;
    const auto background = contrast_ ? GetSysColor(COLOR_WINDOW) :
        add && tasks_.empty() ? dropColor : paperColor;
    COLORREF fill = primary ? accentColor : RGB(250, 232, 220);
    COLORREF text = primary ? RGB(255, 255, 255) : accentColor;
    COLORREF edge = primary ? accentColor : RGB(232, 195, 171);
    if (down) fill = primary ? RGB(133, 48, 20) : RGB(243, 214, 194);
    if (disabled) {
        fill = RGB(236, 228, 221);
        text = RGB(119, 106, 95);
        edge = fill;
    }
    if (contrast_) {
        fill = GetSysColor(primary && !disabled ? COLOR_HIGHLIGHT : COLOR_BTNFACE);
        text = GetSysColor(disabled ? COLOR_GRAYTEXT : primary ? COLOR_HIGHLIGHTTEXT : COLOR_BTNTEXT);
        edge = GetSysColor(COLOR_WINDOWTEXT);
    }
    fillBox(item.hDC, item.rcItem, background);
    roundedBox(item.hDC, item.rcItem, fill, edge, px(9));
    centeredText(item.hDC, item.rcItem, windowText(item.hwndItem).c_str(), text, strongFont_);
    if (!disabled && (item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT)) {
        RECT focus = item.rcItem;
        InflateRect(&focus, -px(4), -px(4));
        DrawFocusRect(item.hDC, &focus);
    }
}

LRESULT Window::paintText(HDC dc, HWND control) const {
    const bool branding = control == title_ || control == instruction_;
    const bool inDrop = tasks_.empty() && (control == empty_ || control == emptyFormats_);
    const bool muted = control == instruction_ || control == status_ || control == hint_ ||
                       control == emptyFormats_;
    const auto background = contrast_ ? GetSysColor(COLOR_WINDOW) :
                            branding ? canvasColor : inDrop ? dropColor : paperColor;
    SetTextColor(dc, contrast_ ? GetSysColor(COLOR_WINDOWTEXT) : muted ? mutedColor : inkColor);
    SetBkColor(dc, background);
    return reinterpret_cast<LRESULT>(branding ? canvasBrush_ : inDrop ? dropBrush_ : paperBrush_);
}

std::vector<TaskId> Window::selection() const {
    std::vector<TaskId> ids;
    for (int row = ListView_GetNextItem(queue_, -1, LVNI_SELECTED); row >= 0;
         row = ListView_GetNextItem(queue_, row, LVNI_SELECTED)) {
        LVITEMW item{};
        item.iItem = row;
        item.mask = LVIF_PARAM;
        if (ListView_GetItem(queue_, &item)) ids.push_back(static_cast<TaskId>(item.lParam));
    }
    return ids;
}

void Window::renderRows() {
    const auto selected = selection();
    rebuilding_ = true;
    ListView_DeleteAllItems(queue_);
    visibleRows_.clear();
    const int filter = choice(filter_);
    const Kind visible = filter < 0 ? Kind::Unknown : filters[static_cast<std::size_t>(filter)];
    for (const auto& entry : tasks_) {
        const auto id = entry.first;
        const auto& task = entry.second;
        if (visible != Kind::Unknown && visible != task.source.kind) continue;
        const std::wstring name = task.job.input.filename().native();
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = ListView_GetItemCount(queue_);
        item.pszText = const_cast<wchar_t*>(name.c_str());
        item.lParam = static_cast<LPARAM>(id);
        const int row = ListView_InsertItem(queue_, &item);
        if (row < 0) continue;
        visibleRows_[id] = row;
        refreshRow(id);
        if (std::find(selected.begin(), selected.end(), id) != selected.end()) {
            ListView_SetItemState(queue_, row, LVIS_SELECTED, LVIS_SELECTED);
        }
    }
    ShowWindow(empty_, ListView_GetItemCount(queue_) == 0 ? SW_SHOWNA : SW_HIDE);
    SetWindowPos(empty_, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (ListView_GetItemCount(queue_) && selection().empty()) {
        ListView_SetItemState(queue_, 0, LVIS_SELECTED | LVIS_FOCUSED,
                             LVIS_SELECTED | LVIS_FOCUSED);
    }
    rebuilding_ = false;
    syncSelection();
    refreshStatus();
}

void Window::refreshRow(TaskId id) {
    const auto found = visibleRows_.find(id);
    if (found == visibleRows_.end()) return;
    const int row = found->second;
    const auto& task = tasks_.at(id);
    const auto* preset = findPreset(task.job.preset);
    ListView_SetItemText(queue_, row, 1, const_cast<wchar_t*>(preset ? preset->label.c_str() : L""));
    ListView_SetItemText(queue_, row, 2, const_cast<wchar_t*>(task.status.c_str()));
}

void Window::syncSelection(bool allPresets) {
    const auto ids = selection();
    choices_.clear();
    morePresets_ = false;
    SendMessageW(preset_, CB_RESETCONTENT, 0, 0);
    Kind kind = Kind::Unknown;
    const Task* task = nullptr;
    if (!ids.empty()) {
        task = &tasks_.at(ids.front());
        if (task->phase == Phase::Ready || task->phase == Phase::Failed ||
            task->phase == Phase::Cancelled) kind = task->source.kind;
    }
    int chosen = 0;
    if (kind != Kind::Unknown) {
        for (const auto& preset : ttk::presets()) {
            if (!accepts(kind, preset.kind)) continue;
            const auto matches = [this, &preset](TaskId id) {
                const auto& selectedTask = tasks_.at(id);
                return accepts(selectedTask.source.kind, preset.kind) &&
                       selectedTask.phase != Phase::Done && selectedTask.phase != Phase::Converting;
            };
            if (!std::all_of(ids.begin(), ids.end(), matches)) continue;
            if (!allPresets && preset.id != task->job.preset && !commonPreset(preset)) {
                morePresets_ = true;
                continue;
            }
            const auto index = static_cast<int>(choices_.size());
            if (preset.id == task->job.preset) chosen = index;
            choices_.push_back(&preset);
            addChoice(preset_, preset.label.c_str());
        }
    }
    if (choices_.empty()) {
        addChoice(preset_, task ? L"请选择可设置的同类任务" : L"请先添加文件");
    } else if (morePresets_) {
        addChoice(preset_, L"显示全部格式…");
    }
    selectChoice(preset_, chosen);
    draftOptions_ = task ? task->job.options : ttk::Options{};
    selectChoice(quality_, draftOptions_.quality);
    refreshButtons();
    layout();
}

void Window::refreshButtons() {
    const bool editable = !choices_.empty();
    const int current = choice(preset_);
    const auto* currentPreset = current >= 0 && static_cast<std::size_t>(current) < choices_.size()
        ? choices_[static_cast<std::size_t>(current)] : nullptr;
    const Kind target = currentPreset ? currentPreset->kind : Kind::Unknown;
    const bool pdf = target == Kind::Pdf;
    const auto presetId = currentPreset ? currentPreset->id : std::wstring{};
    if (pdf) {
        showText(hint_, L"扫描页会自动识别简体中文和英语。");
    } else if (presetId.find(L"av1") != std::wstring::npos) {
        showText(hint_, L"AV1 适合压缩存档，编码可能较慢。");
    } else if (presetId.find(L"h265") != std::wstring::npos) {
        showText(hint_, L"H.265 体积较小，旧设备兼容性有限。");
    } else if (presetId == L"mp4-h264") {
        showText(hint_, L"H.264 兼容常见播放器和设备。");
    } else {
        showText(hint_, selection().size() > 1 ? L"更改会应用到当前选中的同类任务。" : L"");
    }
    EnableWindow(preset_, editable);
    EnableWindow(quality_, editable && !pdf);
    EnableWindow(advanced_, currentPreset != nullptr);
    const bool batchActive = !scheduled_.empty() || (active_ && !probing_);
    const auto ready = std::count_if(tasks_.begin(), tasks_.end(), [](const auto& entry) {
        return entry.second.phase == Phase::Ready;
    });
    showText(start_, ready > 1 && !batchActive ? L"开始转换 (" + std::to_wstring(ready) + L")" :
                                              L"开始转换");
    const bool canStart = !active_ && probes_.empty() && !batchActive && !closing_;
    EnableWindow(start_, ready && canStart);
    EnableWindow(stop_, batchActive && !stopping_);
    EnableWindow(path_, !batchActive);
    EnableWindow(browse_, !batchActive);
}

void Window::refreshStatus() {
    std::size_t done = 0;
    std::size_t failed = 0;
    std::size_t ready = 0;
    for (const auto& entry : tasks_) {
        const auto phase = entry.second.phase;
        if (phase == Phase::Done) ++done;
        if (phase == Phase::Failed || phase == Phase::Unsupported) ++failed;
        if (phase == Phase::Ready) ++ready;
    }
    std::wstring text;
    if (closing_ || stopping_) text = L"正在停止…";
    else if (active_ && probing_) text = L"正在检查文件，检查完成后即可开始";
    else if (active_ || !scheduled_.empty()) text = L"正在转换";
    else if (ready) text = std::to_wstring(ready) + L" 个文件可转换";
    else if (tasks_.empty()) text = L"先添加文件，即可开始";
    else if (done == tasks_.size()) text = L"全部文件已转换";
    else text = L"没有待转换文件";
    if (done) text += L" · 完成 " + std::to_wstring(done);
    if (failed) text += L" · 失败 " + std::to_wstring(failed);
    step_ = tasks_.empty() || (probing_ && active_) ? 0 :
        active_ || !scheduled_.empty() || (!ready && done) ? 2 : 1;
    showText(status_, text);
    refreshButtons();
    layout();
}

void Window::pickFiles() {
    std::wstring selected(65536, L'\0');
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = L"所有文件 (*.*)\0*.*\0\0";
    dialog.lpstrFile = selected.data();
    dialog.nMaxFile = static_cast<DWORD>(selected.size());
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_NOCHANGEDIR;
    dialog.lpstrTitle = L"添加要转换的文件";
    if (!GetOpenFileNameW(&dialog)) {
        if (CommDlgExtendedError() == FNERR_BUFFERTOOSMALL) {
            MessageBoxW(window_, L"选中的文件路径太多，请分批添加。", L"添加文件", MB_OK);
        }
        return;
    }
    const wchar_t* root = selected.c_str();
    const wchar_t* leaf = root + std::wcslen(root) + 1;
    std::vector<fs::path> files;
    if (!*leaf) {
        files.emplace_back(root);
    } else {
        for (; *leaf; leaf += std::wcslen(leaf) + 1) files.push_back(fs::path(root) / leaf);
    }
    addFiles(files);
}

void Window::dropped(HDROP drop) {
    const UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
    std::vector<fs::path> files;
    files.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        std::wstring path(length + 1, L'\0');
        path.resize(DragQueryFileW(drop, index, path.data(), length + 1));
        if (!path.empty()) files.emplace_back(std::move(path));
    }
    DragFinish(drop);
    addFiles(files);
}

void Window::addFiles(const std::vector<fs::path>& files) {
    if (closing_) return;
    for (const auto& path : files) {
        const auto existing = std::find_if(tasks_.begin(), tasks_.end(), [&path](const auto& entry) {
            return CompareStringOrdinal(entry.second.job.input.c_str(), -1, path.c_str(), -1,
                                        TRUE) == CSTR_EQUAL;
        });
        if (existing != tasks_.end()) continue;
        Task task;
        task.job.input = path;
        const TaskId id = nextId_++;
        tasks_.emplace(id, std::move(task));
        probes_.push_back(id);
    }
    selectChoice(filter_, 0);
    renderRows();
    startWork();
}

void Window::pickDirectory() {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        MessageBoxW(window_, L"无法打开目录选择器，可以在保存位置里输入文件夹。", L"保存到", MB_OK);
        return;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"选择输出文件夹");
    if (SUCCEEDED(dialog->Show(window_))) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(dialog->GetResult(&folder))) {
            wchar_t* path = nullptr;
            if (SUCCEEDED(folder->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                showText(path_, path);
                writeSetting(settings_, L"保存目录", path);
                CoTaskMemFree(path);
            }
            folder->Release();
        }
    }
    dialog->Release();
}

void Window::applySettings() {
    const int index = choice(preset_);
    if (index < 0 || static_cast<std::size_t>(index) >= choices_.size()) return;
    const auto& preset = *choices_[static_cast<std::size_t>(index)];
    auto options = draftOptions_;
    options.quality = std::max(0, choice(quality_));
    for (const auto& field : numbers) {
        if (!numericEnabled(field.id, preset.kind)) options.*(field.member) = 0;
    }
    options.remux = options.remux && (preset.kind == Kind::Video || preset.kind == Kind::Audio);
    options.hardware = options.hardware && preset.kind == Kind::Video;
    options.forceOcr = options.forceOcr && preset.kind == Kind::Pdf;
    for (const TaskId id : selection()) {
        auto& task = tasks_.at(id);
        if (!accepts(task.source.kind, preset.kind)) continue;
        if (task.phase != Phase::Ready && task.phase != Phase::Failed &&
            task.phase != Phase::Cancelled) continue;
        task.job.preset = preset.id;
        task.job.options = options;
        refreshRow(id);
        if (preset.kind == task.source.kind) {
            auto& saved = savedPresets_[task.source.kind];
            if (saved != preset.id) {
                saved = preset.id;
                writeSetting(settings_, kindName(task.source.kind), saved);
            }
        }
    }
    draftOptions_ = options;
    refreshButtons();
}

void Window::editAdvanced() {
    const int index = choice(preset_);
    if (index < 0 || static_cast<std::size_t>(index) >= choices_.size()) return;
    const auto* preset = choices_[static_cast<std::size_t>(index)];
    const auto editing = selection();
    AdvancedDraft draft{preset->kind, draftOptions_};
    if (DialogBoxParamW(instance_, MAKEINTRESOURCEW(UI_ADVANCED_DIALOG), window_, advancedDialog,
                        reinterpret_cast<LPARAM>(&draft)) == IDOK && editing == selection() &&
        choice(preset_) == index && static_cast<std::size_t>(index) < choices_.size() &&
        choices_[static_cast<std::size_t>(index)] == preset) {
        draftOptions_ = draft.options;
        applySettings();
    }
}

INT_PTR CALLBACK Window::advancedDialog(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_INITDIALOG) {
        auto& draft = *reinterpret_cast<AdvancedDraft*>(lp);
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        const bool video = draft.kind == Kind::Video;
        const bool audio = draft.kind == Kind::Audio;
        const bool pdf = draft.kind == Kind::Pdf;
        const bool hw =
#ifdef TTK_MODERN
            video;
#else
            false;
#endif
        const auto setControl = [&](int id, bool shown, bool checked) {
            const auto control = GetDlgItem(dialog, id);
            ShowWindow(control, shown ? SW_SHOWNA : SW_HIDE);
            setCheck(control, checked);
        };
        const auto place = [&](int id, int x, int y, int w, int h) {
            RECT rect{x, y, x + w, y + h};
            MapDialogRect(dialog, &rect);
            MoveWindow(GetDlgItem(dialog, id), rect.left, rect.top, rect.right - rect.left,
                       rect.bottom - rect.top, FALSE);
        };
        for (const auto& field : numbers) {
            const auto control = GetDlgItem(dialog, field.id);
            const bool enabled = numericEnabled(field.id, draft.kind);
            ShowWindow(control, enabled ? SW_SHOWNA : SW_HIDE);
            ShowWindow(GetDlgItem(dialog, field.labelId), enabled ? SW_SHOWNA : SW_HIDE);
            showText(control, std::to_wstring(draft.options.*(field.member)));
            SendMessageW(control, EM_SETLIMITTEXT, 6, 0);
        }
        setControl(UI_ADVANCED_REMUX, video || audio, draft.options.remux);
        setControl(UI_ADVANCED_HARDWARE, hw, draft.options.hardware);
        setControl(UI_ADVANCED_OCR, pdf, draft.options.forceOcr);
        int dialogHeight = 144;
        if (video) {
            place(IDOK, 151, 116, 55, 16);
            place(IDCANCEL, 213, 116, 55, 16);
        } else if (pdf) {
            showText(GetDlgItem(dialog, UI_ADVANCED_HINT),
                     L"扫描页默认自动识别。文字层失真时可对每页重新识别。");
            place(UI_ADVANCED_OCR, 14, 48, 254, 15);
            place(IDOK, 151, 77, 55, 16);
            place(IDCANCEL, 213, 77, 55, 16);
            dialogHeight = 105;
        } else if (audio) {
            place(UI_ADVANCED_AUDIO_RATE_LABEL, 14, 39, 60, 11);
            place(UI_ADVANCED_AUDIO_RATE, 75, 35, 45, 15);
            place(UI_ADVANCED_CHANNELS_LABEL, 147, 39, 60, 11);
            place(UI_ADVANCED_CHANNELS, 213, 35, 55, 15);
            place(UI_ADVANCED_THREADS_LABEL, 14, 59, 60, 11);
            place(UI_ADVANCED_THREADS, 75, 55, 45, 15);
            place(UI_ADVANCED_REMUX, 14, 78, 110, 13);
            place(IDOK, 151, 99, 55, 16);
            place(IDCANCEL, 213, 99, 55, 16);
            dialogHeight = 127;
        } else {
            place(IDOK, 151, 67, 55, 16);
            place(IDCANCEL, 213, 67, 55, 16);
            dialogHeight = 95;
        }
        RECT area{}, client{0, 0, 282, dialogHeight};
        GetWindowRect(dialog, &area);
        MapDialogRect(dialog, &client);
        AdjustWindowRectEx(&client, static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_STYLE)),
                           FALSE, static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_EXSTYLE)));
        SetWindowPos(dialog, nullptr, area.left, area.top, client.right - client.left,
                     client.bottom - client.top, SWP_NOACTIVATE | SWP_NOZORDER);
        return TRUE;
    }
    if (message != WM_COMMAND) return FALSE;
    if (LOWORD(wp) == IDCANCEL) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (LOWORD(wp) != IDOK) return FALSE;
    auto& draft = *reinterpret_cast<AdvancedDraft*>(GetWindowLongPtrW(dialog, DWLP_USER));
    auto options = draft.options;
    for (const auto& field : numbers) {
        const auto control = GetDlgItem(dialog, field.id);
        const bool enabled = numericEnabled(field.id, draft.kind);
        const auto value = enabled ? windowText(control) : std::wstring{};
        wchar_t* end = nullptr;
        const long parsed = value.empty() ? 0 : std::wcstol(value.c_str(), &end, 10);
        if ((end && *end) || parsed < 0 || parsed > field.maximum) {
            const std::wstring error = std::wstring(field.label) + L"请输入 0 至 " +
                                       std::to_wstring(field.maximum) + L" 的整数。";
            MessageBoxW(dialog, error.c_str(), L"检查高级设置", MB_OK | MB_ICONWARNING);
            SetFocus(control);
            return TRUE;
        }
        options.*(field.member) = static_cast<int>(parsed);
    }
    const auto checked = [&](int id) {
        const auto control = GetDlgItem(dialog, id);
        return IsWindowVisible(control) && isChecked(control);
    };
    options.remux = checked(UI_ADVANCED_REMUX);
    options.hardware = checked(UI_ADVANCED_HARDWARE);
    options.forceOcr = checked(UI_ADVANCED_OCR);
    if (options.remux && (options.width || options.height || options.videoKbps ||
                         options.audioKbps || options.channels)) {
        MessageBoxW(dialog, L"无损换封装会保留原尺寸、码率和声道。请将这些参数填为 0，或取消勾选。",
                    L"检查高级设置", MB_OK | MB_ICONWARNING);
        return TRUE;
    }
    draft.options = options;
    EndDialog(dialog, IDOK);
    return TRUE;
}

void Window::requestStart(bool selectedOnly) {
    if (closing_ || active_ || !probes_.empty() || !scheduled_.empty()) return;
    if (std::none_of(tasks_.begin(), tasks_.end(), [](const auto& entry) {
        return entry.second.phase == Phase::Ready;
    })) return;
    const std::wstring output = windowText(path_);
    if (!output.empty()) {
        const fs::path destination(output);
        if (!destination.is_absolute()) {
            MessageBoxW(window_, L"请输入完整的输出文件夹路径，或点击浏览。", L"检查保存位置",
                        MB_OK | MB_ICONWARNING);
            SetFocus(path_);
            return;
        }
        std::error_code error;
        fs::create_directories(destination, error);
        if (error || !fs::is_directory(destination, error)) {
            MessageBoxW(window_, L"无法创建输出文件夹，请检查保存位置和写入权限。", L"检查保存位置",
                        MB_OK | MB_ICONWARNING);
            SetFocus(path_);
            return;
        }
    }
    const auto ids = selectedOnly ? selection() : std::vector<TaskId>{};
    for (auto& entry : tasks_) {
        if (entry.second.phase != Phase::Ready) continue;
        if (selectedOnly && std::find(ids.begin(), ids.end(), entry.first) == ids.end()) continue;
        entry.second.job.outputDirectory = fs::path(output);
        entry.second.status = L"排队中";
        refreshRow(entry.first);
        scheduled_.push_back(entry.first);
    }
    writeSetting(settings_, L"保存目录", output);
    startWork();
    refreshStatus();
}

void Window::startWork() {
    if (active_ || closing_) return;
    while (!probes_.empty() && !tasks_.count(probes_.front())) probes_.pop_front();
    while (!scheduled_.empty() && !tasks_.count(scheduled_.front())) scheduled_.pop_front();
    if (probes_.empty() && scheduled_.empty()) return;
    probing_ = !probes_.empty();
    auto& pending = probing_ ? probes_ : scheduled_;
    active_ = pending.front();
    pending.pop_front();
    auto& task = tasks_.at(active_);
    if (!probing_) task.phase = Phase::Converting;
    task.status = probing_ ? L"检查文件" : L"准备转换";
    refreshRow(active_);
    const auto ids = selection();
    if (std::find(ids.begin(), ids.end(), active_) != ids.end()) syncSelection();
    cancel_.store(false);
    stopping_ = false;
    progress_ = -1;
    progressText_.clear();
    try {
        worker_ = std::thread([this, job = task.job, probe = probing_] {
            WorkResult completion;
            try {
                if (probe) {
                    completion.source = ttk::identify(job.input, cancel_);
                } else {
                    completion.conversion = ttk::convert(job, cancel_,
                        [this](int percent, const std::wstring& description) {
                            const std::lock_guard<std::mutex> lock(workMutex_);
                            progress_ = percent;
                            progressText_ = description;
                        });
                }
            } catch (const std::exception& error) {
                completion.source.detail = L"无法检查此文件";
                completion.conversion = {false, cancel_.load(), L"转换任务遇到错误",
                                         ttk::fromUtf8(error.what()), {}};
            }
            const std::lock_guard<std::mutex> lock(workMutex_);
            completed_ = std::move(completion);
        });
    } catch (const std::system_error&) {
        WorkResult completion;
        completion.source.detail = L"暂时无法运行文件检查";
        completion.conversion.message = L"暂时无法启动转换，请关闭其他程序后重试。";
        completed_ = std::move(completion);
    }
    refreshStatus();
}

void Window::finishWork() {
    if (!active_) return;
    auto& task = tasks_.at(active_);
    std::optional<WorkResult> completion;
    {
        const std::lock_guard<std::mutex> lock(workMutex_);
        if (completed_) {
            completion = std::move(completed_);
            completed_.reset();
        } else if (!stopping_ && !progressText_.empty()) {
            std::wstring status = progressText_;
            if (progress_ >= 0) status += L" · " + std::to_wstring(progress_) + L"%";
            if (task.status != status) {
                task.status = std::move(status);
                refreshRow(active_);
            }
        }
    }
    if (!completion) return;
    if (worker_.joinable()) worker_.join();
    if (probing_) {
        task.source = std::move(completion->source);
        if (task.source.kind == Kind::Unknown) {
            task.phase = Phase::Unsupported;
            task.status = L"无法识别";
            task.result.message = task.source.detail.empty() ?
                L"请检查文件类型，或尝试在原应用中打开文件。" : task.source.detail;
        } else {
            task.phase = Phase::Ready;
            task.status = L"待转换";
            task.job.preset = ttk::defaultPreset(task.source.kind, task.source.animated);
            const auto it = savedPresets_.find(task.source.kind);
            if (const auto* preset = it == savedPresets_.end() ? nullptr : findPreset(it->second);
                preset && preset->kind == task.source.kind) task.job.preset = preset->id;
        }
    } else {
        task.result = std::move(completion->conversion);
        if (task.result.cancelled) {
            task.phase = Phase::Cancelled;
            task.status = L"已取消";
        } else if (task.result.ok) {
            task.phase = Phase::Done;
            task.status = L"已完成";
        } else {
            task.phase = Phase::Failed;
            task.status = L"失败 · 可查看详情";
        }
    }
    const bool rebuild = probing_ && choice(filter_) > 0;
    refreshRow(active_);
    const TaskId finished = std::exchange(active_, 0);
    stopping_ = false;
    if (closing_) {
        DestroyWindow(window_);
        return;
    }
    if (rebuild) {
        renderRows();
    } else {
        const auto ids = selection();
        if (std::find(ids.begin(), ids.end(), finished) != ids.end()) syncSelection();
    }
    startWork();
    refreshStatus();
}

void Window::stopBatch() {
    for (const TaskId id : scheduled_) {
        tasks_.at(id).status = L"待转换";
        refreshRow(id);
    }
    scheduled_.clear();
    if (active_ && !probing_) {
        stopping_ = true;
        cancel_.store(true);
        tasks_.at(active_).status = L"正在取消…";
        refreshRow(active_);
    }
    refreshStatus();
}

void Window::taskActions() {
    const auto ids = selection();
    if (ids.empty()) return;
    const auto has = [&](Phase phase) {
        return std::any_of(ids.begin(), ids.end(), [&](TaskId id) {
            return tasks_.at(id).phase == phase;
        });
    };
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    if (has(Phase::Done)) AppendMenuW(menu, MF_STRING, Open, L"打开结果");
    const bool idle = !active_ && probes_.empty() && scheduled_.empty();
    if (idle && has(Phase::Ready)) AppendMenuW(menu, MF_STRING, StartSelected, L"转换所选");
    if (idle && (has(Phase::Failed) || has(Phase::Cancelled))) {
        AppendMenuW(menu, MF_STRING, Retry, L"重试");
    }
    AppendMenuW(menu, MF_STRING, Details, L"查看详情");
    if (idle) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, Remove, L"从队列移除");
    }
    RECT button{};
    GetWindowRect(actions_, &button);
    const auto picked = TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD,
                                        button.right, button.bottom, 0, window_, nullptr);
    DestroyMenu(menu);
    if (picked) command(picked, 0);
}

void Window::retry() {
    const auto ids = selection();
    for (const TaskId id : ids) {
        auto& task = tasks_.at(id);
        if (task.phase != Phase::Failed && task.phase != Phase::Cancelled) continue;
        task.phase = Phase::Ready;
        task.status = L"待转换";
    }
    requestStart(true);
    renderRows();
}

void Window::removeSelection() {
    if (active_ || !scheduled_.empty()) return;
    for (const TaskId id : selection()) tasks_.erase(id);
    renderRows();
}

void Window::openResult() {
    for (const TaskId id : selection()) {
        const auto& result = tasks_.at(id).result;
        if (result.output.empty() || !result.ok) continue;
        const HINSTANCE opened = ShellExecuteW(window_, nullptr, result.output.c_str(),
                                               nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(opened) <= 32) {
            ShellExecuteW(window_, L"open", result.output.parent_path().c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
        }
        return;
    }
}

void Window::showDetails() {
    const auto ids = selection();
    if (ids.empty()) return;
    const auto& task = tasks_.at(ids.front());
    std::wstring message = task.job.input.native() + L"\n\n" + task.status;
    if (!task.source.detail.empty() && task.source.detail != task.result.message) {
        message += L"\n" + task.source.detail;
    }
    if (!task.result.message.empty()) message += L"\n\n" + task.result.message;
    if (!task.result.output.empty()) message += L"\n\n" + task.result.output.native();
    if (!task.result.technical.empty()) {
        const auto& log = task.result.technical;
        message += L"\n\n技术详情\n" + log.substr(log.size() > 1200 ? log.size() - 1200 : 0);
    }
    MessageBoxW(window_, message.c_str(), L"任务详情", MB_OK |
                (task.phase == Phase::Failed ? MB_ICONWARNING : MB_ICONINFORMATION));
}

void Window::close() {
    if (!active_) {
        DestroyWindow(window_);
        return;
    }
    if (closing_) return;
    const bool converting = !probing_ || !scheduled_.empty();
    if (converting && MessageBoxW(window_, L"有任务正在转换。停止当前批次并退出？", L"退出 ttkConverter",
                                  MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDYES) return;
    closing_ = true;
    stopping_ = true;
    scheduled_.clear();
    probes_.clear();
    cancel_.store(true);
    tasks_.at(active_).status = L"正在退出…";
    refreshRow(active_);
    refreshStatus();
}

LRESULT Window::notify(const NMHDR& message) {
    if (message.idFrom != Queue) return 0;
    switch (message.code) {
    case LVN_ITEMCHANGED:
        if (!rebuilding_) {
            const auto& change = reinterpret_cast<const NMLISTVIEW&>(message);
            if ((change.uOldState ^ change.uNewState) & LVIS_SELECTED) syncSelection();
        }
        break;
    case NM_DBLCLK:
    case NM_RETURN: {
        const auto ids = selection();
        if (std::any_of(ids.begin(), ids.end(), [&](TaskId id) {
            return tasks_.at(id).phase == Phase::Done;
        })) openResult();
        else showDetails();
        break;
    }
    case NM_RCLICK: taskActions(); break;
    default: break;
    }
    return 0;
}

void Window::command(int id, int code) {
    switch (id) {
    case Add: pickFiles(); break;
    case Filter: if (code == CBN_SELCHANGE) renderRows(); break;
    case TaskActions: taskActions(); break;
    case Remove: removeSelection(); break;
    case Preset:
        if (code == CBN_SELCHANGE) {
            if (morePresets_ && choice(preset_) == static_cast<int>(choices_.size())) {
                syncSelection(true);
                PostMessageW(preset_, CB_SHOWDROPDOWN, TRUE, 0);
            } else {
                applySettings();
            }
        }
        break;
    case Quality: if (code == CBN_SELCHANGE) applySettings(); break;
    case Advanced: editAdvanced(); break;
    case Browse: pickDirectory(); break;
    case Start: requestStart(false); break;
    case StartSelected: requestStart(true); break;
    case Stop: stopBatch(); break;
    case Retry: retry(); break;
    case Open: openResult(); break;
    case Details: showDetails(); break;
    }
}

LRESULT Window::dispatch(UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CREATE: createControls(); return 0;
    case WM_SIZE: if (queue_ && wp != SIZE_MINIMIZED) layout(); return 0;
    case WM_PAINT: paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_CTLCOLORSTATIC: return paintText(reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp));
    case WM_DRAWITEM: {
        const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if (item.CtlID == Add || item.CtlID == Start) {
            paintAction(item);
            return TRUE;
        }
        break;
    }
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
        updateColors();
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        break;
    case WM_GETMINMAXINFO: {
        auto& limits = *reinterpret_cast<MINMAXINFO*>(lp);
        MONITORINFO monitor{sizeof(monitor), {}, {}, 0};
        const auto target = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
        const bool found = GetMonitorInfoW(target, &monitor);
        const auto dpi = static_cast<int>(queue_ ? dpi_ : windowDpi(window_));
        const auto minWidth = static_cast<LONG>(MulDiv(560, dpi, 96));
        const auto minHeight = static_cast<LONG>(MulDiv(458, dpi, 96));
        limits.ptMinTrackSize.x = found ? std::min(minWidth, monitor.rcWork.right - monitor.rcWork.left)
                                       : minWidth;
        limits.ptMinTrackSize.y = found ? std::min(minHeight, monitor.rcWork.bottom - monitor.rcWork.top)
                                       : minHeight;
        return 0;
    }
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wp);
        updateFonts();
        updateCaptionIcon();
        const auto area = fitToWorkArea(*reinterpret_cast<const RECT*>(lp));
        SetWindowPos(window_, nullptr, area.left, area.top, area.right - area.left,
                     area.bottom - area.top, SWP_NOACTIVATE | SWP_NOZORDER);
        layout();
        return 0;
    }
    case WM_DROPFILES: dropped(reinterpret_cast<HDROP>(wp)); return 0;
    case WM_COMMAND: command(LOWORD(wp), HIWORD(wp)); return 0;
    case WM_NOTIFY: return notify(*reinterpret_cast<const NMHDR*>(lp));
    case WM_TIMER: if (wp == pollingTimer) finishWork(); return 0;
    case WM_CLOSE: close(); return 0;
    case WM_DESTROY:
        KillTimer(window_, pollingTimer);
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProcW(window_, message, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    int argc = 0;
    if (auto argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        std::vector<std::wstring> args(argv + 1, argv + argc);
        LocalFree(argv);
        if (!args.empty() && args.front().compare(0, 2, L"--") == 0) {
            return ttk::runHeadless(args);
        }
    }
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSEXW type{};
    type.cbSize = sizeof(type);
    type.lpfnWndProc = Window::procedure;
    type.hInstance = instance;
    type.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(iconResource));
    if (!type.hIcon) type.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    type.lpszClassName = windowClass;
    if (!RegisterClassExW(&type)) {
        if (SUCCEEDED(apartment)) CoUninitialize();
        return 1;
    }
    Window app;
    RECT work{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) || IsRectEmpty(&work)) {
        work = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    const int dpi = static_cast<int>(windowDpi(nullptr));
    const int screenWidth = static_cast<int>(work.right - work.left);
    const int screenHeight = static_cast<int>(work.bottom - work.top);
    const int width = std::min(screenWidth, MulDiv(920, dpi, 96));
    const int height = std::min(screenHeight, MulDiv(650, dpi, 96));
    HWND window = CreateWindowExW(0, windowClass, L"ttkConverter",
                                  WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                  work.left + (screenWidth - width) / 2,
                                  work.top + (screenHeight - height) / 2, width, height,
                                  nullptr, nullptr, instance, &app);
    if (!window) {
        if (SUCCEEDED(apartment)) CoUninitialize();
        return 1;
    }
    ShowWindow(window, showCommand);
    UpdateWindow(window);
    ACCEL keys[]{{FVIRTKEY | FCONTROL, 'O', Add}, {FVIRTKEY | FCONTROL, VK_RETURN, Start}};
    HACCEL shortcuts = CreateAcceleratorTableW(keys, static_cast<int>(std::size(keys)));
    MSG message{};
    BOOL received;
    while ((received = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (shortcuts && TranslateAcceleratorW(window, shortcuts, &message)) continue;
        if (IsDialogMessageW(window, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (shortcuts) DestroyAcceleratorTable(shortcuts);
    if (SUCCEEDED(apartment)) CoUninitialize();
    return received < 0 ? 1 : static_cast<int>(message.wParam);
}
