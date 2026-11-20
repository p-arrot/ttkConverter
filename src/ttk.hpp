#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ttk {

namespace fs = std::filesystem;
using Cancel = std::atomic_bool;
using Progress = std::function<void(int, const std::wstring&)>; // -1, or 0–100

enum class Kind { Video, Audio, Image, Pdf, Unknown };

struct Preset {
    std::wstring id;
    Kind kind;
    std::wstring label;
    std::wstring extension; // Including the leading dot.
};

struct SourceInfo {
    Kind kind = Kind::Unknown;
    bool animated = false;
    std::wstring detail;
    std::wstring container;
    std::wstring videoCodec;
    std::wstring audioCodec;
    int width = 0;
    int height = 0;
    bool hasAudio = false;
};

struct Options {
    int quality = 1; // 0 = fast, 1 = balanced, 2 = smaller, 3 = low, 4 = high, 5 = very low.
    int width = 0;
    int height = 0;
    int videoKbps = 0;
    int audioKbps = 0;
    int channels = 0;
    int threads = 0;
    bool remux = false;
    bool hardware = false;
    bool forceOcr = false;
    int resolution = 0; // Nominal short edge in pixels. Zero keeps the input size.
    int imageLongEdge = 0; // Zero keeps the input size, with a maximum of 256 for ICO.
    int audioSampleRate = 0; // Hertz. Zero lets the encoder select the rate.
    int ocrDpi = 300;
};

struct Job {
    fs::path input;
    fs::path outputDirectory; // Empty = ttkConverter 输出 beside input.
    std::wstring preset;
    Options options;
};

struct Result {
    bool ok = false;
    bool cancelled = false;
    std::wstring message;
    std::wstring technical;
    fs::path output;
};

struct ProcessResult {
    int exitCode = -1;
    bool cancelled = false;
    std::string output;
    std::string error;
};

using OutputLine = std::function<void(const std::string&)>;

ProcessResult runProcess(const fs::path& exe, const std::vector<std::wstring>& args,
                         const Cancel& cancel, const fs::path& cwd = {},
                         const OutputLine& onOutput = {});
fs::path enginePath(const std::wstring& name);
std::wstring fromUtf8(const std::string& value);
std::string toUtf8(const std::wstring& value);

const std::vector<Preset>& mediaPresets();
bool hasAdjustableAudioBitrate(const std::wstring& preset);
std::vector<int> audioSampleRates(const std::wstring& preset);
bool supportsAudioBitrate(const std::wstring& preset, int sampleRate, int bitrate);
std::vector<int> audioBitrates(const std::wstring& preset, int sampleRate);
SourceInfo probeMedia(const fs::path& input, const Cancel& cancel);
Result convertMedia(const Job& job, const fs::path& temporaryOutput,
                    const fs::path& scratch, const Cancel& cancel, const Progress& progress);
Result convertPdf(const Job& job, const fs::path& temporaryOutput,
                  const fs::path& scratch, const Cancel& cancel, const Progress& progress);

const std::vector<Preset>& presets();
std::wstring defaultPreset(Kind kind, bool animated = false);
SourceInfo identify(const fs::path& input, const Cancel& cancel);
Result convert(const Job& job, const Cancel& cancel, const Progress& progress);
int runHeadless(const std::vector<std::wstring>& arguments);

} // namespace ttk
