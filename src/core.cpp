#include "ttk.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <cwctype>
#include <string_view>
#include <system_error>

namespace ttk {
namespace {

class Handle {
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
    explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
    void reset(HANDLE value = nullptr) {
        if (*this) CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_;
};

bool makePipe(Handle& input, Handle& output) {
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE read = nullptr;
    HANDLE write = nullptr;
    if (!CreatePipe(&read, &write, &attributes, 0)) return false;
    input.reset(read);
    output.reset(write);
    return SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0) != FALSE;
}

std::wstring quoteArgument(const std::wstring& arg) {
    std::wstring quoted = L"\"";
    std::size_t slashes = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        quoted.append(slashes * (ch == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (ch == L'"') quoted += L'\\';
        quoted += ch;
    }
    quoted.append(slashes * 2, L'\\');
    quoted += L'"';
    return quoted;
}

std::string processError(const char* function, DWORD code) {
    return std::string(function) + " (Win32 " + std::to_string(code) + ")";
}

void drain(HANDLE pipe, std::string& captured, std::string* pending, const OutputLine& callback) {
    std::array<char, 4096> buffer{};
    constexpr std::size_t maxCapture = 1024 * 1024;
    for (int chunk = 0; chunk < 64; ++chunk) {
        DWORD available = 0;
        DWORD received = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || !available) return;
        if (!ReadFile(pipe, buffer.data(), std::min<DWORD>(available, buffer.size()), &received, nullptr) || !received) return;
        captured.append(buffer.data(), received);
        if (captured.size() > maxCapture) captured.erase(0, captured.size() - maxCapture);
        if (!pending || !callback) continue;
        pending->append(buffer.data(), received);
        std::size_t consumed = 0;
        while (true) {
            const auto newline = pending->find('\n', consumed);
            if (newline == std::string::npos) break;
            auto line = pending->substr(consumed, newline - consumed);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            callback(line);
            consumed = newline + 1;
        }
        pending->erase(0, consumed);
        if (pending->size() > maxCapture) pending->clear();
    }
}

std::wstring uniqueName() {
    static std::atomic<unsigned> sequence{0};
    return L"ttk-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
           std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence.fetch_add(1));
}

struct Cleanup {
    fs::path scratch;
    fs::path output;
    ~Cleanup() {
        std::error_code ignored;
        if (!output.empty()) fs::remove(output, ignored);
        if (!scratch.empty()) fs::remove_all(scratch, ignored);
    }
};

Result failure(std::wstring message, std::wstring detail = {}) {
    return {false, false, std::move(message), std::move(detail), {}};
}

Result cancelledResult() {
    return {false, true, L"已取消", {}, {}};
}

void writeOutput(const std::wstring& line, DWORD stream = STD_OUTPUT_HANDLE) {
    const auto bytes = toUtf8(line + L"\r\n");
    const auto handle = GetStdHandle(stream);
    DWORD written = 0;
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    }
}

} // namespace

std::wstring fromUtf8(const std::string& value) {
    if (value.empty()) return {};
    const auto length = static_cast<int>(value.size());
    UINT encoding = CP_UTF8;
    int count = MultiByteToWideChar(encoding, MB_ERR_INVALID_CHARS, value.data(), length, nullptr, 0);
    if (!count) {
        encoding = CP_ACP;
        count = MultiByteToWideChar(encoding, 0, value.data(), length, nullptr, 0);
    }
    if (!count) return {};
    std::wstring text(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(encoding, 0, value.data(), length, text.data(), count);
    return text;
}

std::string toUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const auto length = static_cast<int>(value.size());
    const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), length, nullptr, 0, nullptr, nullptr);
    if (!count) return {};
    std::string text(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), length, text.data(), count, nullptr, nullptr);
    return text;
}

fs::path enginePath(const std::wstring& name) {
    std::wstring path(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0 || size == path.size()) return {};
    path.resize(size);
    const auto engines = fs::path(path).parent_path() / L"engines";
    if (name == L"pdfinfo.exe" || name == L"pdftotext.exe" || name == L"pdftopng.exe" ||
        name == L"tesseract.exe" || name == L"tessdata" || name == L"xpdfrc" || name == L"xpdf-cmaps") {
        const auto pdfEngine = engines / L"pdf" / name;
        std::error_code error;
        if (fs::exists(pdfEngine, error)) return pdfEngine;
    }
    return engines / name;
}

ProcessResult runProcess(const fs::path& exe, const std::vector<std::wstring>& args,
                         const Cancel& cancel, const fs::path& cwd, const OutputLine& onOutput) {
    ProcessResult result;
    if (cancel.load()) {
        result.cancelled = true;
        return result;
    }
    Handle readOutput, writeOutputPipe, readError, writeError;
    if (!makePipe(readOutput, writeOutputPipe) || !makePipe(readError, writeError)) {
        result.error = processError("CreatePipe", GetLastError());
        return result;
    }
    SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &inheritable, OPEN_EXISTING, 0, nullptr));
    if (!input) {
        result.error = processError("CreateFileW(NUL)", GetLastError());
        return result;
    }
    std::vector<HANDLE> inherited{input.get(), writeOutputPipe.get(), writeError.get()};
    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<unsigned char> storage(attributeSize);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!attributeSize || !InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize)) {
        result.error = processError("InitializeProcThreadAttributeList", GetLastError());
        return result;
    }
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited.data(), sizeof(HANDLE) * inherited.size(), nullptr, nullptr)) {
        result.error = processError("UpdateProcThreadAttribute", GetLastError());
        DeleteProcThreadAttributeList(attributes);
        return result;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.get();
    startup.StartupInfo.hStdOutput = writeOutputPipe.get();
    startup.StartupInfo.hStdError = writeError.get();
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION info{};
    auto command = quoteArgument(exe.wstring());
    for (const auto& arg : args) command += L" " + quoteArgument(arg);
    const DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | BELOW_NORMAL_PRIORITY_CLASS;
    const BOOL started = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, flags,
                                       nullptr, cwd.empty() ? nullptr : cwd.c_str(), &startup.StartupInfo, &info);
    const auto error = GetLastError();
    DeleteProcThreadAttributeList(attributes);
    if (!started) {
        result.error = processError("CreateProcessW", error) + ": " + toUtf8(exe.wstring());
        return result;
    }
    Handle task(info.hProcess);
    Handle primaryThread(info.hThread);
    writeOutputPipe.reset();
    writeError.reset();

    Handle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    const bool grouped = job && SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                                                        &limits, sizeof(limits)) &&
                         AssignProcessToJobObject(job.get(), task.get());
    if (ResumeThread(primaryThread.get()) == static_cast<DWORD>(-1)) {
        result.error = processError("ResumeThread", GetLastError());
        TerminateProcess(task.get(), 1);
        return result;
    }

    std::string pending;
    try {
        while (true) {
            drain(readOutput.get(), result.output, &pending, onOutput);
            drain(readError.get(), result.error, nullptr, {});
            if (cancel.load()) {
                result.cancelled = true;
                if (grouped) TerminateJobObject(job.get(), 1);
                else TerminateProcess(task.get(), 1);
                WaitForSingleObject(task.get(), INFINITE);
                break;
            }
            const auto state = WaitForSingleObject(task.get(), 30);
            if (state != WAIT_TIMEOUT) break;
        }
        drain(readOutput.get(), result.output, &pending, onOutput);
        drain(readError.get(), result.error, nullptr, {});
        if (onOutput && !pending.empty()) onOutput(pending);
    } catch (...) {
        if (grouped) TerminateJobObject(job.get(), 1);
        else TerminateProcess(task.get(), 1);
        WaitForSingleObject(task.get(), INFINITE);
        throw;
    }
    DWORD code = 0;
    if (GetExitCodeProcess(task.get(), &code)) result.exitCode = static_cast<int>(code);
    return result;
}

const std::vector<Preset>& presets() {
    static const auto list = [] {
        auto entries = mediaPresets();
        entries.push_back({L"pdf-docx", Kind::Pdf, L"Word（DOCX） · 可编辑", L".docx"});
        return entries;
    }();
    return list;
}

std::wstring defaultPreset(Kind kind, bool animated) {
    switch (kind) {
        case Kind::Video: return L"mp4-h264";
        case Kind::Audio: return L"mp3";
        case Kind::Image: return animated ? L"apng" : L"png";
        case Kind::Pdf: return L"pdf-docx";
        default: return {};
    }
}

SourceInfo identify(const fs::path& input, const Cancel& cancel) {
    auto extension = input.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    if (extension == L".pdf") {
        SourceInfo info;
        info.kind = Kind::Pdf;
        return info;
    }
    return probeMedia(input, cancel);
}

Result convert(const Job& job, const Cancel& cancel, const Progress& progress) {
    if (cancel.load()) return cancelledResult();
    const auto& entries = presets();
    const auto preset = std::find_if(entries.begin(), entries.end(), [&](const Preset& entry) {
        return entry.id == job.preset;
    });
    if (preset == entries.end()) return failure(L"请选择可用的输出格式");
    try {
        const auto input = fs::absolute(job.input);
        if (!fs::is_regular_file(input)) return failure(L"找不到输入文件，请重新添加");
        const auto outputDir = job.outputDirectory.empty() ? input.parent_path() / L"ttkConverter 输出"
                                                          : fs::absolute(job.outputDirectory);
        std::error_code error;
        fs::create_directories(outputDir, error);
        if (error || !fs::is_directory(outputDir)) return failure(L"无法创建保存位置，请选择其他文件夹", outputDir.wstring());

        Cleanup temporary;
        const auto token = uniqueName();
        const auto scratch = fs::temp_directory_path() / token;
        if (!fs::create_directory(scratch)) return failure(L"无法创建临时文件夹");
        temporary.scratch = scratch;
        temporary.output = outputDir / (L"." + token + preset->extension);
        auto safeJob = job;
        safeJob.input = input;
        auto result = preset->kind == Kind::Pdf
            ? convertPdf(safeJob, temporary.output, scratch, cancel, progress)
            : convertMedia(safeJob, temporary.output, scratch, cancel, progress);
        if (cancel.load() || result.cancelled) return cancelledResult();
        if (!result.ok) return result;
        const auto size = fs::file_size(temporary.output, error);
        if (error || !size) return failure(L"转换未生成有效文件，请查看详情", result.technical);

        const auto stem = input.stem().wstring();
        for (int index = 0; index < 10000; ++index) {
            const auto suffix = index ? L" (" + std::to_wstring(index) + L")" : std::wstring{};
            auto finalPath = outputDir / (stem + suffix + preset->extension);
            if (cancel.load()) return cancelledResult();
            if (MoveFileExW(temporary.output.c_str(), finalPath.c_str(), MOVEFILE_WRITE_THROUGH)) {
                temporary.output.clear();
                result.output = std::move(finalPath);
                if (result.message.empty()) result.message = L"已完成";
                if (progress) progress(100, result.message);
                return result;
            }
            const auto moveError = GetLastError();
            if (moveError != ERROR_ALREADY_EXISTS && moveError != ERROR_FILE_EXISTS && !fs::exists(finalPath)) {
                return failure(L"无法保存结果，请检查目录权限和磁盘空间", fromUtf8(processError("MoveFileExW", moveError)));
            }
        }
        return failure(L"同名文件过多，请选择其他保存位置");
    } catch (const fs::filesystem_error& error) {
        return failure(L"文件访问失败，请检查路径、权限和磁盘空间", fromUtf8(error.what()));
    } catch (const std::exception& error) {
        return failure(L"转换失败，请查看详情", fromUtf8(error.what()));
    }
}

int runHeadless(const std::vector<std::wstring>& arguments) {
    Cancel cancel{false};
    if (arguments.size() == 1 && arguments.front() == L"--formats") {
        for (const auto& preset : presets()) writeOutput(preset.id + L"\t" + preset.extension + L"\t" + preset.label);
        return 0;
    }
    const bool probeDetails = arguments.size() == 3 && arguments[2] == L"--details";
    if ((arguments.size() == 2 || probeDetails) && arguments.front() == L"--probe") {
        const auto info = identify(arguments[1], cancel);
        writeOutput(defaultPreset(info.kind, info.animated) + L"\t" + info.detail);
        if (probeDetails) {
            writeOutput(L"container\t" + info.container);
            writeOutput(L"video_codec\t" + info.videoCodec);
            writeOutput(L"audio_codec\t" + info.audioCodec);
            writeOutput(L"width\t" + std::to_wstring(info.width));
            writeOutput(L"height\t" + std::to_wstring(info.height));
            writeOutput(std::wstring{L"has_audio\t"} + (info.hasAudio ? L"1" : L"0"));
        }
        return info.kind == Kind::Unknown ? 1 : 0;
    }
    if (arguments.size() >= 4 && arguments.front() == L"--convert") {
        Job job{arguments[2], arguments[3], arguments[1], {}};
        bool cancelOnProgress = false;
        for (std::size_t index = 4; index < arguments.size(); ++index) {
            if (arguments[index] == L"--remux") job.options.remux = true;
            else if (arguments[index] == L"--force-ocr") job.options.forceOcr = true;
            else if (arguments[index] == L"--fast") job.options.quality = 0;
            else if (arguments[index] == L"--hardware") job.options.hardware = true;
            else if (arguments[index] == L"--cancel-on-progress") cancelOnProgress = true;
            else if (arguments[index] == L"--quality" && index + 1 < arguments.size()) {
                constexpr std::array<std::wstring_view, 6> qualities{
                    L"fast", L"balanced", L"smaller", L"low", L"high", L"very-low"};
                const std::wstring_view name = arguments[++index];
                const auto quality = std::find(qualities.begin(), qualities.end(), name);
                if (quality == qualities.end()) {
                    writeOutput(L"无效的画质预设。", STD_ERROR_HANDLE);
                    return 2;
                }
                job.options.quality = static_cast<int>(quality - qualities.begin());
            } else if (arguments[index] == L"--resolution" && index + 1 < arguments.size()) {
                const auto& value = arguments[++index];
                wchar_t* end = nullptr;
                const auto resolution = std::wcstol(value.c_str(), &end, 10);
                constexpr std::array<long, 10> supported{0, 144, 240, 360, 480, 540, 720, 1080, 1440, 2160};
                if (end == value.c_str() || *end || std::find(supported.begin(), supported.end(), resolution) == supported.end()) {
                    writeOutput(L"无效的分辨率预设。请选择 0、144、240、360、480、540、720、1080、1440 或 2160。", STD_ERROR_HANDLE);
                    return 2;
                }
                job.options.resolution = static_cast<int>(resolution);
            } else {
                struct NumericSetting {
                    std::wstring_view flag;
                    int Options::*field;
                    long maximum;
                };
                constexpr std::array<NumericSetting, 4> settings{{
                    {L"--image-size", &Options::imageLongEdge, 16384},
                    {L"--audio-bitrate", &Options::audioKbps, 8192},
                    {L"--sample-rate", &Options::audioSampleRate, 192000},
                    {L"--ocr-dpi", &Options::ocrDpi, 400},
                }};
                const auto& flag = arguments[index];
                const auto setting = std::find_if(settings.begin(), settings.end(), [&](const auto& candidate) {
                    return candidate.flag == flag;
                });
                if (setting == settings.end() || index + 1 == arguments.size()) {
                    writeOutput(L"未知参数或缺少参数值 " + flag, STD_ERROR_HANDLE);
                    return 2;
                }
                const auto& value = arguments[++index];
                wchar_t* end = nullptr;
                const auto number = std::wcstol(value.c_str(), &end, 10);
                if (end == value.c_str() || *end || number < 0 || number > setting->maximum) {
                    writeOutput(L"无效的数值 " + flag, STD_ERROR_HANDLE);
                    return 2;
                }
                job.options.*(setting->field) = static_cast<int>(number);
            }
        }
        Progress onProgress;
        if (cancelOnProgress) {
            onProgress = [&](int percent, const std::wstring&) {
                if (percent > 0 && percent < 100) cancel.store(true);
            };
        }
        const auto result = convert(job, cancel, onProgress);
        if (result.ok) {
            writeOutput(result.output.wstring());
            return 0;
        }
        writeOutput(result.message + L"\r\n" + result.technical, STD_ERROR_HANDLE);
        return result.cancelled ? 130 : 1;
    }
    writeOutput(L"ttkConverter --formats | --probe INPUT [--details] | --convert PRESET INPUT OUTPUT_DIRECTORY [OPTIONS]", STD_ERROR_HANDLE);
    return 2;
}

} // namespace ttk
