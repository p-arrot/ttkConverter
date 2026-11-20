#include "ttk.hpp"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <numeric>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ttk {
namespace {

using Arguments = std::vector<std::wstring>;

void add(Arguments& arguments, std::initializer_list<std::wstring_view> values) {
    for (const auto value : values) {
        arguments.emplace_back(value);
    }
}

std::wstring lowercase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
    return value;
}

bool in(std::wstring_view value, std::initializer_list<std::wstring_view> choices) {
    return std::find(choices.begin(), choices.end(), value) != choices.end();
}

bool in(int value, std::initializer_list<int> choices) {
    return std::find(choices.begin(), choices.end(), value) != choices.end();
}

struct MediaFormat {
    std::wstring_view id;
    std::wstring_view extension;
    std::wstring_view muxer;
    Kind kind;
    std::wstring_view videoEncoder;
    std::wstring_view audioEncoder;
    std::wstring_view label;
};

constexpr MediaFormat formats[] = {
    {L"mp4-h264", L".mp4", L"mp4", Kind::Video, L"libx264", L"aac", L"MP4 · H.264"},
    {L"mp4-h265", L".mp4", L"mp4", Kind::Video, L"libx265", L"aac", L"MP4 · H.265"},
#ifndef TTK_LEGACY_X86
    {L"mp4-h265-10", L".mp4", L"mp4", Kind::Video, L"libx265", L"aac", L"MP4 · H.265 10 位"},
#endif
    {L"mp4-av1", L".mp4", L"mp4", Kind::Video, L"libaom-av1", L"aac", L"MP4 · AV1"},
    {L"mkv-h264", L".mkv", L"matroska", Kind::Video, L"libx264", L"aac", L"MKV · H.264"},
    {L"mkv-h265", L".mkv", L"matroska", Kind::Video, L"libx265", L"aac", L"MKV · H.265"},
#ifndef TTK_LEGACY_X86
    {L"mkv-h265-10", L".mkv", L"matroska", Kind::Video, L"libx265", L"aac", L"MKV · H.265 10 位"},
#endif
    {L"mkv-av1", L".mkv", L"matroska", Kind::Video, L"libaom-av1", L"aac", L"MKV · AV1"},
    {L"mov-h264", L".mov", L"mov", Kind::Video, L"libx264", L"aac", L"MOV · H.264"},
    {L"mov-h265", L".mov", L"mov", Kind::Video, L"libx265", L"aac", L"MOV · H.265"},
#ifndef TTK_LEGACY_X86
    {L"mov-h265-10", L".mov", L"mov", Kind::Video, L"libx265", L"aac", L"MOV · H.265 10 位"},
#endif
    {L"webm-vp8", L".webm", L"webm", Kind::Video, L"libvpx", L"libvorbis", L"WebM · VP8"},
    {L"webm-vp9", L".webm", L"webm", Kind::Video, L"libvpx-vp9", L"libopus", L"WebM · VP9"},
    {L"webm-av1", L".webm", L"webm", Kind::Video, L"libaom-av1", L"libopus", L"WebM · AV1"},
    {L"avi", L".avi", L"avi", Kind::Video, L"mpeg4", L"libmp3lame", L"AVI · MPEG-4"},
    {L"wmv", L".wmv", L"asf", Kind::Video, L"wmv2", L"wmav2", L"WMV · WMV2"},
    {L"mpg", L".mpg", L"mpeg", Kind::Video, L"mpeg2video", L"mp2", L"MPG · MPEG-2"},
    {L"ts", L".ts", L"mpegts", Kind::Video, L"libx264", L"aac", L"TS · H.264"},
    {L"flv", L".flv", L"flv", Kind::Video, L"libx264", L"aac", L"FLV · H.264"},
    {L"mp3", L".mp3", L"mp3", Kind::Audio, L"", L"libmp3lame", L"MP3"},
    {L"aac", L".aac", L"adts", Kind::Audio, L"", L"aac", L"AAC"},
    {L"m4a", L".m4a", L"ipod", Kind::Audio, L"", L"aac", L"M4A · AAC"},
    {L"m4a-alac", L".m4a", L"ipod", Kind::Audio, L"", L"alac", L"M4A · ALAC 无损"},
    {L"wav", L".wav", L"wav", Kind::Audio, L"", L"pcm_s16le", L"WAV"},
    {L"flac", L".flac", L"flac", Kind::Audio, L"", L"flac", L"FLAC 无损"},
    {L"aiff", L".aiff", L"aiff", Kind::Audio, L"", L"pcm_s16be", L"AIFF"},
    {L"ogg", L".ogg", L"ogg", Kind::Audio, L"", L"libvorbis", L"OGG · Vorbis"},
    {L"opus", L".opus", L"ogg", Kind::Audio, L"", L"libopus", L"Opus"},
    {L"ac3", L".ac3", L"ac3", Kind::Audio, L"", L"ac3", L"AC-3"},
    {L"jpg", L".jpg", L"image2", Kind::Image, L"mjpeg", L"", L"JPEG"},
    {L"png", L".png", L"image2", Kind::Image, L"png", L"", L"PNG"},
    {L"webp", L".webp", L"webp", Kind::Image, L"libwebp", L"", L"WebP"},
    {L"bmp", L".bmp", L"image2", Kind::Image, L"bmp", L"", L"BMP"},
    {L"tiff", L".tiff", L"image2", Kind::Image, L"tiff", L"", L"TIFF"},
    {L"tga", L".tga", L"image2", Kind::Image, L"targa", L"", L"TGA"},
    {L"ico", L".ico", L"ico", Kind::Image, L"png", L"", L"ICO"},
    {L"avif", L".avif", L"avif", Kind::Image, L"libaom-av1", L"", L"AVIF"},
    {L"gif", L".gif", L"gif", Kind::Image, L"gif", L"", L"GIF 动图"},
    {L"apng", L".apng", L"apng", Kind::Image, L"apng", L"", L"APNG 动图"},
};

const MediaFormat* findFormat(std::wstring_view id) {
    const auto match = std::find_if(std::begin(formats), std::end(formats),
        [id](const MediaFormat& format) { return format.id == id; });
    return match == std::end(formats) ? nullptr : match;
}

bool isLosslessAudio(std::wstring_view encoder) {
    return in(encoder, {L"flac", L"alac", L"pcm_s16le", L"pcm_s16be"});
}

std::vector<int> sampleRatesForEncoder(std::wstring_view encoder) {
    if (encoder == L"aac") return {0, 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 64000, 88200, 96000};
    if (encoder == L"libmp3lame") return {0, 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
    if (encoder == L"libvorbis") return {0, 8000, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
    if (encoder == L"mp2") return {0, 16000, 22050, 24000, 32000, 44100, 48000};
    if (in(encoder, {L"ac3", L"wmav2"})) return {0, 32000, 44100, 48000};
    // Opus files use a 48 kHz time base regardless of the encoder input rate.
    if (encoder == L"libopus") return {0, 48000};
    if (isLosslessAudio(encoder)) return {0, 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000, 192000};
    return {};
}

bool supportedVorbisBitrate(int sampleRate, int kbps, int channels) {
    // libvorbis's managed modes set minimum and maximum rates per channel.
    const int monoMinimum = sampleRate <= 9000 ? 8 : sampleRate <= 15000 ? 12
        : sampleRate <= 26000 ? 16 : sampleRate <= 40000 ? 30 : 32;
    const int monoMaximum = sampleRate <= 9000 ? 42 : sampleRate <= 15000 ? 50 : sampleRate <= 19000 ? 100
        : sampleRate <= 26000 ? 90 : sampleRate <= 40000 ? 190 : 240;
    const int stereoMinimum = sampleRate <= 9000 ? 12 : sampleRate <= 15000 ? 16 : sampleRate <= 19000 ? 24
        : sampleRate <= 26000 ? 30 : sampleRate <= 40000 ? 36 : 45;
    if (channels == 0) return kbps >= stereoMinimum && kbps <= monoMaximum;
    if (channels == 2) return kbps >= stereoMinimum && kbps <= 2 * monoMaximum;
    if (channels == 6 && sampleRate > 40000 && kbps >= 84 && kbps <= 1440) return true;
    return kbps >= channels * monoMinimum && kbps <= channels * monoMaximum;
}

bool supportedBitrate(std::wstring_view encoder, int sampleRate, int kbps, int channels) {
    if (kbps <= 0) return kbps == 0;
    if (encoder == L"libmp3lame") {
        if (sampleRate < 16000) return in(kbps, {8, 16, 24, 32, 40, 48, 56, 64});
        if (sampleRate < 32000) return in(kbps, {8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160});
        return in(kbps, {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320});
    }
    if (encoder == L"mp2") {
        if (sampleRate < 32000) return in(kbps, {8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160});
        return in(kbps, {32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384});
    }
    if (encoder == L"ac3") {
        return in(kbps, {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 576, 640});
    }
    const int effectiveChannels = (std::max)(1, channels);
    if (encoder == L"aac") return kbps <= 6 * effectiveChannels * sampleRate / 1000;
    if (encoder == L"libopus") return kbps <= 256 * effectiveChannels;
    if (encoder == L"libvorbis") return supportedVorbisBitrate(sampleRate, kbps, channels);
    if (encoder == L"wmav2") return kbps >= 24 && kbps <= 2000;
    return false;
}

int compatibleAudioRate(std::wstring_view encoder, int inputRate, int kbps, int channels) {
    int best = 0;
    for (const int rate : sampleRatesForEncoder(encoder)) {
        if (!rate || !supportedBitrate(encoder, rate, kbps, channels)) continue;
        if (!best || std::abs(rate - inputRate) < std::abs(best - inputRate)) best = rate;
    }
    return best;
}

int audioChannelOverride(std::wstring_view encoder, int channels) {
    if (channels == 0 && in(encoder, {L"libmp3lame", L"wmav2", L"mp2"})) return 2;
    return channels;
}

int qualityAudioBitrate(std::wstring_view encoder, int sampleRate, int channels, int quality) {
    constexpr int standard[] = {128, 160, 96, 64, 256, 32};
    constexpr int ac3[] = {320, 384, 256, 192, 512, 128};
    const int target = encoder == L"ac3" ? ac3[quality] : standard[quality];
    int best = 0;
    for (const int bitrate : {8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160,
                              192, 224, 256, 320, 384, 448, 512, 576, 640, 768, 896, 1024}) {
        if (!supportedBitrate(encoder, sampleRate, bitrate, channels)) continue;
        if (!best || std::abs(bitrate - target) < std::abs(best - target)) best = bitrate;
    }
    return best;
}

struct Stream {
    int index = 0;
    std::string type;
    std::string codec;
    bool attachedPicture = false;
    bool preferred = false;
    int width = 0;
    int height = 0;
    int rotation = 0;
    int sampleAspectNumerator = 0;
    int sampleAspectDenominator = 0;
    int frames = 0;
    int sampleRate = 0;
    int channels = 0;
    double duration = 0;
};

struct MediaInfo {
    Stream video;
    Stream audio;
    bool hasVideo = false;
    bool hasAudio = false;
    bool hasTiles = false;
    std::wstring container;
    std::wstring majorBrand;
    double duration = 0;
    std::wstring diagnostic;
};

struct Tile {
    int member = -1;
    int x = -1;
    int y = -1;
};

struct TileGrid {
    bool preferred = false;
    int expectedTiles = 0;
    int width = 0;
    int height = 0;
    int codedWidth = 0;
    int codedHeight = 0;
    int left = 0;
    int top = 0;
    std::vector<Tile> tiles;
    std::vector<Stream> streams;
};

double number(const std::string& value) {
    std::istringstream parser(value);
    parser.imbue(std::locale::classic());
    double parsed = 0;
    return (parser >> parsed) && std::isfinite(parsed) ? parsed : 0;
}

int count(const std::string& value) {
    std::istringstream parser(value);
    int parsed = 0;
    return parser >> parsed ? parsed : 0;
}

void readSampleAspect(const std::string& value, Stream& stream) {
    std::istringstream parser(value);
    int numerator = 0, denominator = 0;
    char separator = 0;
    if (parser >> numerator >> separator >> denominator && separator == ':' && numerator > 0 && denominator > 0) {
        stream.sampleAspectNumerator = numerator;
        stream.sampleAspectDenominator = denominator;
    }
}

MediaInfo inspect(const fs::path& path, const Cancel& cancel) {
    const auto probe = runProcess(enginePath(L"ffprobe.exe"), {
        L"-v", L"error", L"-protocol_whitelist", L"file,pipe",
        L"-show_entries", L"stream=index,codec_name,codec_type,width,height,sample_aspect_ratio,duration,nb_frames,sample_rate,channels:"
            L"stream_disposition=attached_pic,default:stream_side_data=rotation:stream_group=type:"
            L"format=duration,format_name:format_tags=major_brand",
        L"-of", L"default", L"-i", path.wstring()
    }, cancel);

    MediaInfo media;
    if (probe.exitCode != 0 || probe.cancelled) {
        media.diagnostic = fromUtf8(probe.error);
        return media;
    }

    bool readingStream = false;
    bool readingGroup = false;
    Stream stream;
    std::istringstream fields(probe.output);
    for (std::string line; std::getline(fields, line);) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line == "[STREAM_GROUP]") readingGroup = true;
        if (line == "[/STREAM_GROUP]") readingGroup = false;
        if (readingGroup) {
            if (line == "type=Tile Grid") media.hasTiles = true;
            continue;
        }
        if (line == "[STREAM]") {
            stream = {};
            readingStream = true;
            continue;
        }
        if (line == "[/STREAM]") {
            if (stream.type == "video" && !stream.attachedPicture &&
                (!media.hasVideo || (stream.preferred && !media.video.preferred))) {
                media.video = stream;
                media.hasVideo = true;
            } else if (stream.type == "audio" && (!media.hasAudio || (stream.preferred && !media.audio.preferred))) {
                media.audio = stream;
                media.hasAudio = true;
            }
            readingStream = false;
            continue;
        }

        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        const auto key = line.substr(0, separator);
        const auto value = line.substr(separator + 1);
        if (readingStream) {
            if (key == "index") stream.index = count(value);
            if (key == "codec_name") stream.codec = value;
            if (key == "codec_type") stream.type = value;
            if (key == "width") stream.width = count(value);
            if (key == "height") stream.height = count(value);
            if (key == "rotation") stream.rotation = count(value);
            if (key == "sample_aspect_ratio") readSampleAspect(value, stream);
            if (key == "nb_frames") stream.frames = count(value);
            if (key == "sample_rate") stream.sampleRate = count(value);
            if (key == "channels") stream.channels = count(value);
            if (key == "duration") stream.duration = number(value);
            if (key == "DISPOSITION:attached_pic") stream.attachedPicture = value == "1";
            if (key == "DISPOSITION:default") stream.preferred = value == "1";
        } else {
            if (key == "format_name") media.container = fromUtf8(value);
            if (key == "TAG:major_brand") media.majorBrand = lowercase(fromUtf8(value));
            if (key == "duration") media.duration = number(value);
        }
    }

    if (media.duration <= 0) {
        media.duration = (std::max)(media.video.duration, media.audio.duration);
    }
    return media;
}

bool isFormat(std::wstring_view names, std::wstring_view expected) {
    while (!names.empty()) {
        const auto separator = names.find(L',');
        if (names.substr(0, separator) == expected) return true;
        if (separator == std::wstring_view::npos) break;
        names.remove_prefix(separator + 1);
    }
    return false;
}

std::wstring containerLabel(const MediaInfo& media, std::wstring_view extension) {
    const auto& name = media.container;
    if (isFormat(name, L"mov")) {
        const auto& brand = media.majorBrand;
        if (brand == L"qt  ") return L"MOV";
        if (in(brand, {L"avif", L"avis"})) return L"AVIF";
        if (in(brand, {L"heic", L"heix", L"hevc", L"hevx", L"heim", L"heis", L"hevm", L"hevs"})) return L"HEIC";
        if (in(brand, {L"mif1", L"msf1"})) return extension == L".heic" ? L"HEIC" : L"HEIF";
        if (brand.substr(0, 3) == L"3gp") return L"3GP";
        if (brand.substr(0, 3) == L"3g2") return L"3G2";
        if (in(brand, {L"mjp2", L"mj2s"})) return L"MJ2";
        if (brand == L"m4a " || (!media.hasVideo && extension == L".m4a")) return L"M4A";
        if (brand == L"m4v ") return L"M4V";
        if (brand.substr(0, 3) == L"iso" || brand.substr(0, 3) == L"mp4") return L"MP4";
        if (extension == L".mov") return L"MOV";
        if (extension == L".3gp") return L"3GP";
        if (extension == L".3g2") return L"3G2";
        if (extension == L".heic") return L"HEIC";
        if (extension == L".heif") return L"HEIF";
        if (extension == L".avif") return L"AVIF";
        if (extension == L".mp4") return L"MP4";
        return L"ISO 基础媒体文件";
    }
    if (isFormat(name, L"matroska")) {
        if (extension == L".webm") return L"WebM";
        if (extension == L".mkv") return L"MKV";
        if (extension == L".mka") return L"MKA";
        return L"Matroska / WebM";
    }
    if (name == L"asf" && extension == L".wmv") return L"WMV";
    if (name == L"asf" && extension == L".wma") return L"WMA";
    if (name == L"mpeg" && extension == L".vob") return L"VOB";
    if (name == L"rm" && extension == L".rmvb") return L"RMVB";

    constexpr std::pair<std::wstring_view, std::wstring_view> labels[] = {
        {L"avi", L"AVI"}, {L"asf", L"ASF"}, {L"mpeg", L"MPEG-PS"}, {L"mpegts", L"MPEG-TS"},
        {L"mpegtsraw", L"MPEG-TS"}, {L"flv", L"FLV"}, {L"rm", L"RealMedia"}, {L"ivf", L"IVF"},
        {L"mxf", L"MXF"}, {L"m4v", L"MPEG-4 裸流"}, {L"h264", L"H.264 裸流"}, {L"hevc", L"H.265 裸流"},
        {L"obu", L"AV1 OBU"}, {L"av1", L"AV1 裸流"}, {L"aac", L"ADTS"}, {L"mp3", L"MP3"},
        {L"aiff", L"AIFF"}, {L"wav", L"WAV"}, {L"flac", L"FLAC"}, {L"ogg", L"Ogg"},
        {L"ac3", L"AC-3"}, {L"eac3", L"E-AC-3"}, {L"gif", L"GIF"}, {L"apng", L"APNG"},
        {L"jpeg_pipe", L"JPEG"}, {L"png_pipe", L"PNG"}, {L"webp_pipe", L"WebP"},
        {L"bmp_pipe", L"BMP"}, {L"tiff_pipe", L"TIFF"}, {L"ico", L"ICO"},
    };
    for (const auto& [format, label] : labels) {
        if (name == format) return std::wstring(label);
    }
    return name;
}

std::wstring codecLabel(const std::string& name) {
    constexpr std::pair<std::string_view, std::wstring_view> labels[] = {
        {"h264", L"H.264"}, {"hevc", L"H.265"}, {"av1", L"AV1"}, {"vp8", L"VP8"}, {"vp9", L"VP9"},
        {"h263", L"H.263"}, {"h263i", L"H.263"}, {"h263p", L"H.263"}, {"mpeg4", L"MPEG-4"},
        {"mpeg1video", L"MPEG-1"}, {"mpeg2video", L"MPEG-2"}, {"flv1", L"FLV1"}, {"vc1", L"VC-1"},
        {"wmv1", L"WMV1"}, {"wmv2", L"WMV2"}, {"wmv3", L"WMV3"}, {"prores", L"ProRes"}, {"mjpeg", L"MJPEG"},
        {"aac", L"AAC"}, {"aac_latm", L"AAC"}, {"alac", L"ALAC"}, {"mp3", L"MP3"}, {"mp2", L"MP2"},
        {"flac", L"FLAC"}, {"vorbis", L"Vorbis"}, {"opus", L"Opus"}, {"ac3", L"AC-3"}, {"eac3", L"E-AC-3"},
        {"wmav1", L"WMA1"}, {"wmav2", L"WMA2"}, {"wmapro", L"WMA Pro"}, {"wmalossless", L"WMA Lossless"},
        {"dts", L"DTS"}, {"truehd", L"Dolby TrueHD"}, {"png", L"PNG"}, {"apng", L"APNG"}, {"webp", L"WebP"},
        {"gif", L"GIF"}, {"bmp", L"BMP"}, {"tiff", L"TIFF"}, {"targa", L"TGA"},
    };
    for (const auto& [codec, label] : labels) {
        if (name == codec) return std::wstring(label);
    }
    if (name.compare(0, 4, "pcm_") == 0) return L"PCM";
    return name == "unknown" ? L"" : fromUtf8(name);
}

TileGrid inspectGrid(const fs::path& input, const Cancel& cancel) {
    const auto child = runProcess(enginePath(L"ffprobe.exe"), {
        L"-v", L"error", L"-protocol_whitelist", L"file,pipe", L"-show_stream_groups",
        L"-of", L"default", L"-i", input.wstring()
    }, cancel);
    if (child.cancelled || child.exitCode != 0) return {};

    TileGrid best;
    TileGrid grid;
    Tile tile;
    Stream stream;
    bool isGrid = false;
    enum class Section { Group, Component, Tile, Stream };
    auto section = Section::Group;
    std::istringstream fields(child.output);
    for (std::string line; std::getline(fields, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "[STREAM_GROUP]") { grid = {}; isGrid = false; section = Section::Group; }
        if (line == "[/STREAM_GROUP]" && isGrid && (grid.preferred || best.tiles.empty())) best = grid;
        if (line == "[COMPONENT]") section = Section::Component;
        if (line == "[/COMPONENT]") section = Section::Group;
        if (line == "[SUBCOMPONENT]") { tile = {}; section = Section::Tile; }
        if (line == "[/SUBCOMPONENT]") { grid.tiles.push_back(tile); section = Section::Component; }
        if (line == "[STREAM]") { stream = {}; section = Section::Stream; }
        if (line == "[/STREAM]") { grid.streams.push_back(stream); section = Section::Group; }

        const auto separator = line.find('=');
        if (separator == std::string::npos) continue;
        const auto key = line.substr(0, separator);
        const auto value = line.substr(separator + 1);
        switch (section) {
        case Section::Group:
            if (key == "type") isGrid = value == "Tile Grid";
            if (key == "DISPOSITION:default") grid.preferred = value == "1";
            break;
        case Section::Component:
            if (key == "nb_tiles") grid.expectedTiles = count(value);
            if (key == "coded_width") grid.codedWidth = count(value);
            if (key == "coded_height") grid.codedHeight = count(value);
            if (key == "width") grid.width = count(value);
            if (key == "height") grid.height = count(value);
            if (key == "horizontal_offset") grid.left = count(value);
            if (key == "vertical_offset") grid.top = count(value);
            break;
        case Section::Tile:
            if (key == "stream_index") tile.member = count(value);
            if (key == "tile_horizontal_offset") tile.x = count(value);
            if (key == "tile_vertical_offset") tile.y = count(value);
            break;
        case Section::Stream:
            if (key == "index") stream.index = count(value);
            if (key == "width") stream.width = count(value);
            if (key == "height") stream.height = count(value);
            break;
        }
    }
    return best;
}

std::wstring gridFilter(const TileGrid& grid) {
#ifdef TTK_LEGACY_X86
    constexpr long long pixelBudget = 40000000;
#else
    constexpr long long pixelBudget = 100000000;
#endif
    const auto tileCount = grid.tiles.size();
    if (tileCount == 0 || tileCount > 256 || static_cast<size_t>(grid.expectedTiles) != tileCount ||
        grid.width <= 0 || grid.height <= 0 || grid.left < 0 || grid.top < 0 ||
        grid.codedWidth <= 0 || grid.codedHeight <= 0 || grid.width > grid.codedWidth - grid.left ||
        grid.height > grid.codedHeight - grid.top || static_cast<long long>(grid.codedWidth) * grid.codedHeight > pixelBudget) {
        return {};
    }

    std::wstring inputs;
    std::wstring layout;
    for (size_t index = 0; index < tileCount; ++index) {
        const auto& tile = grid.tiles[index];
        if (tile.member < 0 || static_cast<size_t>(tile.member) >= grid.streams.size() || tile.x < 0 || tile.y < 0) return {};
        const auto& stream = grid.streams[tile.member];
        if (stream.width <= 0 || stream.height <= 0 || stream.width > grid.codedWidth - tile.x ||
            stream.height > grid.codedHeight - tile.y) return {};
        inputs += L"[0:" + std::to_wstring(stream.index) + L"]";
        if (index != 0) layout += L"|";
        layout += std::to_wstring(tile.x) + L"_" + std::to_wstring(tile.y);
    }

    if (tileCount > 1) {
        inputs += L"xstack=inputs=" + std::to_wstring(tileCount) + L":layout=" + layout + L":fill=black,";
    }
    return inputs + L"crop=" + std::to_wstring(grid.width) + L":" + std::to_wstring(grid.height) + L":"
        + std::to_wstring(grid.left) + L":" + std::to_wstring(grid.top) + L":exact=1[image]";
}

std::wstring bitrate(int kilobits) {
    return std::to_wstring(kilobits) + L"k";
}

int qualityIndex(const Options& options) {
    return (std::clamp)(options.quality, 0, 5);
}

int threadCount(const Options& options) {
#ifdef TTK_LEGACY_X86
    return options.threads > 0 ? (std::min)(options.threads, 4) : 2;
#else
    return options.threads > 0 ? (std::min)(options.threads, 64) : 0;
#endif
}

Arguments inputArguments(const fs::path& path, int threads, bool autoRotate = true) {
    Arguments arguments;
    add(arguments, {L"-hide_banner", L"-nostdin", L"-nostats", L"-loglevel", L"error",
        L"-y", L"-abort_on", L"empty_output", L"-progress", L"pipe:1", L"-filter_threads", L"1",
        L"-filter_complex_threads", L"1", L"-protocol_whitelist", L"file,pipe"});
    if (threads > 0) {
        add(arguments, {L"-threads:v", std::to_wstring(threads)});
    }
    if (!autoRotate) add(arguments, {L"-noautorotate"});
    add(arguments, {L"-i", path.wstring()});
    return arguments;
}

std::wstring resizing(const Options& options, bool even) {
    std::wstring filter;
    if (options.width > 0 || options.height > 0) {
        const auto inferred = even ? L"-2" : L"-1";
        filter = L"scale=" + (options.width > 0 ? std::to_wstring(options.width) : inferred)
            + L":" + (options.height > 0 ? std::to_wstring(options.height) : inferred)
            + L":flags=lanczos";
    }
    if (even) {
        if (!filter.empty()) filter += L",";
        filter += L"pad=ceil(iw/2)*2:ceil(ih/2)*2";
    }
    return filter;
}

bool supportedVideoResolution(int value) {
    return in(value, {144, 240, 360, 480, 540, 720, 1080, 1440, 2160});
}

int videoResolutionLongEdge(int shortEdge) {
    return ((shortEdge * 16 + 9) / 18) * 2;
}

std::wstring fitVideoResolution(int resolution) {
    const auto shortEdge = std::to_wstring(resolution);
    const auto longEdge = std::to_wstring(videoResolutionLongEdge(resolution));
    // FFmpeg supplies the display ratio after applying input rotation. Scale
    // preserves that ratio through its output sample aspect ratio, including
    // input with non-square pixels and rounding to an even number of pixels.
    const auto width = L"if(gte(dar,1),min(" + longEdge + L"," + shortEdge + L"*dar),min("
        + shortEdge + L"," + longEdge + L"*dar))";
    const auto height = L"if(gte(dar,1),min(" + shortEdge + L"," + longEdge + L"/dar),min("
        + longEdge + L"," + shortEdge + L"/dar))";
    return L"scale=w='max(2,round((" + width + L")/2)*2)':h='max(2,round((" + height
        + L")/2)*2)':flags=lanczos";
}

using Aspect = std::pair<long long, long long>;

Aspect reduceAspect(long long numerator, long long denominator) {
    const auto common = std::gcd(numerator, denominator);
    return {numerator / common, denominator / common};
}

Aspect applyVideoPadding(Aspect aspect, int width, int height, int paddedWidth, int paddedHeight) {
    auto [numerator, denominator] = aspect;
    auto [widthFactor, heightFactor] = reduceAspect(1LL * paddedWidth * height, 1LL * paddedHeight * width);
    const auto cross1 = std::gcd(numerator, heightFactor);
    const auto cross2 = std::gcd(denominator, widthFactor);
    numerator /= cross1;
    heightFactor /= cross1;
    denominator /= cross2;
    widthFactor /= cross2;
    constexpr auto maximum = (std::numeric_limits<long long>::max)();
    if (numerator > maximum / widthFactor || denominator > maximum / heightFactor) return {};
    return {numerator * widthFactor, denominator * heightFactor};
}

long long scaledEvenDimension(int requested, int sourceOther, int sourceRequested) {
    const auto scaled = 1LL * requested * sourceOther;
    const auto divisor = 2LL * sourceRequested;
    return (scaled / divisor + (scaled % divisor >= divisor / 2)) * 2;
}

std::wstring imageResize(const Options& options) {
    if (options.imageLongEdge == 0) return resizing(options, false);
    const auto edge = std::to_wstring(options.imageLongEdge);
    return L"scale=w='if(gte(dar,1)," + edge + L",max(1,round(" + edge
        + L"*dar)))':h='if(gte(dar,1),max(1,round(" + edge + L"/dar))," + edge + L")':flags=lanczos";
}

void audioArguments(Arguments& arguments, const MediaFormat& format, const Options& options) {
    const auto encoder = format.audioEncoder;
    add(arguments, {L"-c:a", encoder});
    if (isLosslessAudio(encoder)) {
        if (encoder == L"flac") {
            const std::wstring_view compression[] = {L"2", L"5", L"9", L"9", L"5", L"9"};
            add(arguments, {L"-compression_level", compression[qualityIndex(options)]});
        }
    } else if (options.audioKbps > 0) {
        add(arguments, {L"-b:a", bitrate(options.audioKbps)});
    } else if (encoder == L"libvorbis" && options.audioSampleRate > 0 && options.audioSampleRate < 32000) {
        // Low-rate Vorbis cannot use the usual 160 kb/s setting for every channel layout.
        add(arguments, {L"-q:a", L"4"});
    } else {
        const bool lowSampleRate = options.audioSampleRate > 0 && options.audioSampleRate <= 24000;
        const int preferred = encoder == L"ac3" ? 384 : encoder == L"mp2" && !lowSampleRate ? 192 : 160;
        add(arguments, {L"-b:a", bitrate(preferred)});
    }
    if (options.audioSampleRate > 0) add(arguments, {L"-ar", std::to_wstring(options.audioSampleRate)});

    const int channels = audioChannelOverride(encoder, options.channels);
    if (channels > 0) {
        add(arguments, {L"-ac", std::to_wstring(channels)});
    }
}

bool hevc10(const MediaFormat& format) {
    return in(format.id, {L"mp4-h265-10", L"mkv-h265-10", L"mov-h265-10"});
}

void videoArguments(Arguments& arguments, const MediaFormat& format, const Options& options,
                    std::wstring_view hardwareEncoder) {
    const auto encoder = format.videoEncoder;
    const auto quality = qualityIndex(options);
    add(arguments, {L"-c:v", hardwareEncoder.empty() ? encoder : hardwareEncoder});
    if (const auto threads = threadCount(options); threads > 0) {
        add(arguments, {L"-threads:v", std::to_wstring(threads)});
    }
    const auto filter = options.resolution > 0 ? fitVideoResolution(options.resolution) : resizing(options, true);
    add(arguments, {L"-vf", filter});
    if (hevc10(format)) {
        add(arguments, {L"-pix_fmt", hardwareEncoder.empty() ? L"yuv420p10le" : L"p010le", L"-profile:v", L"main10"});
    } else {
        add(arguments, {L"-pix_fmt", L"yuv420p"});
    }
    if (encoder == L"libx265" && in(format.muxer, {L"mp4", L"mov"})) {
        add(arguments, {L"-tag:v", L"hvc1"});
    }

    if (!hardwareEncoder.empty()) {
        if (options.videoKbps <= 0) {
            const std::wstring_view qp[] = {L"20", L"24", L"29", L"38", L"16", L"46"};
            if (hardwareEncoder.find(L"_nvenc") != std::wstring_view::npos) {
                add(arguments, {L"-preset", L"p4", L"-rc", L"constqp", L"-qp", qp[quality]});
            } else if (hardwareEncoder.find(L"_qsv") != std::wstring_view::npos) {
                add(arguments, {L"-global_quality", qp[quality]});
            } else {
                add(arguments, {L"-rc", L"cqp", L"-qp_i", qp[quality], L"-qp_p", qp[quality]});
            }
        }
    } else if (in(encoder, {L"libx264", L"libx265"})) {
        const std::wstring_view presets[] = {L"veryfast", L"fast", L"medium", L"medium", L"medium", L"medium"};
        const std::wstring_view crfH264[] = {L"20", L"23", L"28", L"37", L"16", L"45"};
        const std::wstring_view crfH265[] = {L"22", L"26", L"30", L"39", L"18", L"47"};
        add(arguments, {L"-preset", presets[quality]});
        if (options.videoKbps <= 0) {
            add(arguments, {L"-crf", encoder == L"libx265" ? crfH265[quality] : crfH264[quality]});
        }
        if (encoder == L"libx265" && threadCount(options) > 0) {
            add(arguments, {L"-x265-params", L"pools=" + std::to_wstring(threadCount(options)) + L":frame-threads=1"});
        }
    } else if (encoder == L"libaom-av1") {
        const std::wstring_view crf[] = {L"28", L"32", L"39", L"50", L"21", L"59"};
#ifdef TTK_LEGACY_X86
        // A short lookahead improves inter-frame compression in the low-quality presets
        // without enabling libaom's full lookahead on 32-bit builds.
        const auto lag = options.videoKbps <= 0 && (quality == 3 || quality == 5) ? L"2" : L"0";
        add(arguments, {L"-cpu-used", L"8", L"-lag-in-frames", lag});
#else
        const std::wstring_view speed[] = {L"8", L"6", L"5", L"5", L"5", L"5"};
        add(arguments, {L"-cpu-used", speed[quality]});
#endif
        add(arguments, {L"-row-mt", L"1"});
        if (options.videoKbps <= 0) {
            add(arguments, {L"-crf", crf[quality], L"-b:v", L"0"});
        }
    } else if (in(encoder, {L"libvpx", L"libvpx-vp9"})) {
        const std::wstring_view speed[] = {L"6", L"4", L"2", L"2", L"2", L"2"};
        const std::wstring_view crf[] = {L"22", L"30", L"38", L"50", L"16", L"60"};
        add(arguments, {L"-deadline", L"good", L"-cpu-used", speed[quality]});
        if (encoder == L"libvpx-vp9") add(arguments, {L"-row-mt", L"1"});
        if (options.videoKbps <= 0) {
            // VP8 uses constrained quality and needs room to reach the high preset's CRF.
            const auto rate = encoder == L"libvpx" ? (quality == 4 ? L"8M" : L"2M") : L"0";
            add(arguments, {L"-crf", crf[quality], L"-b:v", rate});
        }
    } else if (options.videoKbps <= 0) {
        const std::wstring_view quantizer[] = {L"3", L"5", L"8", L"18", L"2", L"28"};
        add(arguments, {L"-q:v", quantizer[quality]});
    }

    if (options.videoKbps > 0) add(arguments, {L"-b:v", bitrate(options.videoKbps)});
}

void imageArguments(Arguments& arguments, const MediaFormat& format, const Options& options,
                    const std::wstring& videoStream) {
    const auto quality = qualityIndex(options);
    add(arguments, {L"-map", videoStream, L"-an", L"-c:v", format.videoEncoder});
    if (format.id != L"apng") add(arguments, {L"-frames:v", L"1"});
    if (format.muxer == L"image2") add(arguments, {L"-update", L"1"});

    auto filter = imageResize(options);
    if (format.id == L"ico") {
        if (options.imageLongEdge == 0) {
            if (!filter.empty()) filter += L",";
            filter += L"scale=min(256\\,iw):min(256\\,ih):force_original_aspect_ratio=decrease:flags=lanczos";
        } else {
            const auto size = std::to_wstring(options.imageLongEdge);
            filter += L",format=rgba,pad=" + size + L":" + size + L":(ow-iw)/2:(oh-ih)/2:color=black@0";
        }
        add(arguments, {L"-pix_fmt", L"rgba"});
    } else if (format.id == L"jpg") {
        const std::wstring_view quantizer[] = {L"2", L"3", L"5", L"12", L"1", L"23"};
        add(arguments, {L"-q:v", quantizer[quality]});
    } else if (format.id == L"webp") {
        const std::wstring_view targetQuality[] = {L"90", L"82", L"72", L"43", L"95", L"18"};
        add(arguments, {L"-quality", targetQuality[quality]});
    } else if (format.id == L"avif") {
        const std::wstring_view crf[] = {L"18", L"24", L"31", L"43", L"12", L"55"};
        add(arguments, {L"-still-picture", L"1", L"-cpu-used", L"8", L"-crf", crf[quality],
            L"-b:v", L"0", L"-pix_fmt", L"yuv420p"});
    } else if (format.id == L"apng") {
        add(arguments, {L"-plays", L"0", L"-fps_mode", L"passthrough"});
    }
    if (!filter.empty()) add(arguments, {L"-vf", filter});
}

void outputArguments(Arguments& arguments, const MediaFormat& format, const fs::path& path) {
    if (in(format.muxer, {L"mp4", L"mov", L"ipod"})) {
        add(arguments, {L"-movflags", L"+faststart"});
    }
    if (format.id == L"wav") add(arguments, {L"-rf64", L"auto"});
    add(arguments, {L"-f", format.muxer, path.wstring()});
}

OutputLine progressOutput(const Progress& progress, double duration, int first = 0, int last = 99) {
    if (!progress || duration <= 0) return {};
    return [=](const std::string& line) {
        const auto prefix = std::string_view("out_time_us=");
        if (line.compare(0, prefix.size(), prefix) != 0) return;
        const auto elapsed = number(line.substr(prefix.size())) / 1000000;
        const auto fraction = (std::clamp)(elapsed / duration, 0.0, 1.0);
        progress(first + static_cast<int>(fraction * (last - first)), L"正在转换");
    };
}

ProcessResult gifConversion(const Job& job, const fs::path& output, const fs::path& scratch,
                            const Cancel& cancel, const Progress& progress, double duration,
                            const std::wstring& videoStream) {
    const auto engine = enginePath(L"ffmpeg.exe");
    const int threads = threadCount(job.options);
    const auto palette = scratch / L"gif-palette.png";
    auto filter = imageResize(job.options);
    if (!filter.empty()) filter += L",";
    filter += L"fps=12:round=up:eof_action=pass";

    auto paletteArgs = inputArguments(job.input, threads);
    add(paletteArgs, {L"-map", videoStream, L"-vf", filter + L",palettegen", L"-frames:v", L"1",
        L"-an", L"-f", L"image2", L"-update", L"1", palette.wstring()});
    const auto prepared = runProcess(engine, paletteArgs, cancel, {}, progressOutput(progress, duration, 0, 25));
    if (prepared.cancelled || prepared.exitCode != 0) return prepared;

    auto encodeArgs = inputArguments(job.input, threads);
    add(encodeArgs, {L"-protocol_whitelist", L"file,pipe", L"-i", palette.wstring(), L"-filter_complex",
        L"[" + videoStream + L"]" + filter + L"[frames];[frames][1:v:0]paletteuse[gif]", L"-map", L"[gif]",
        L"-an", L"-c:v", L"gif", L"-loop", L"0", L"-f", L"gif", output.wstring()});
    return runProcess(engine, encodeArgs, cancel, {}, progressOutput(progress, duration, 25, 99));
}

Arguments conversionArguments(const Job& job, const fs::path& output, const MediaFormat& format,
                              const std::wstring& videoStream, const std::wstring& audioStream,
                              std::wstring_view hardwareEncoder = {}) {
    auto arguments = inputArguments(job.input, threadCount(job.options));
    switch (format.kind) {
    case Kind::Video:
        if (job.options.remux) {
            if (format.id.substr(0, 4) == L"mkv-") add(arguments, {L"-map", L"0"});
            else add(arguments, {L"-map", L"0:V", L"-map", L"0:a?", L"-map", L"0:s?"});
            add(arguments, {L"-c", L"copy"});
        } else {
            add(arguments, {L"-map", videoStream});
            videoArguments(arguments, format, job.options, hardwareEncoder);
            if (!audioStream.empty()) {
                add(arguments, {L"-map", audioStream});
                audioArguments(arguments, format, job.options);
            }
        }
        break;
    case Kind::Audio:
        add(arguments, {L"-map", audioStream, L"-vn"});
        if (job.options.remux) add(arguments, {L"-c:a", L"copy"});
        else audioArguments(arguments, format, job.options);
        break;
    case Kind::Image:
        imageArguments(arguments, format, job.options, videoStream);
        break;
    default:
        break;
    }
    outputArguments(arguments, format, output);
    return arguments;
}

} // namespace

const std::vector<Preset>& mediaPresets() {
    static const auto presets = [] {
        std::vector<Preset> result;
        for (const auto& format : formats) {
            result.push_back({std::wstring(format.id), format.kind, std::wstring(format.label),
                              std::wstring(format.extension)});
        }
        return result;
    }();
    return presets;
}

bool hasAdjustableAudioBitrate(const std::wstring& preset) {
    const auto format = findFormat(preset);
    return format && !format->audioEncoder.empty() && !isLosslessAudio(format->audioEncoder);
}

std::vector<int> audioSampleRates(const std::wstring& preset) {
    const auto format = findFormat(preset);
    return format ? sampleRatesForEncoder(format->audioEncoder) : std::vector<int>{};
}

bool supportsAudioBitrate(const std::wstring& preset, int sampleRate, int bitrate) {
    const auto format = findFormat(preset);
    if (!format || format->audioEncoder.empty()) return false;
    const auto encoder = format->audioEncoder;
    if (sampleRate == 0) return bitrate == 0 || compatibleAudioRate(encoder, 44100, bitrate, 0) != 0;
    const auto rates = sampleRatesForEncoder(encoder);
    return std::find(rates.begin(), rates.end(), sampleRate) != rates.end()
        && supportedBitrate(encoder, sampleRate, bitrate, 0);
}

std::vector<int> audioBitrates(const std::wstring& preset, int sampleRate) {
    std::vector<int> result;
    for (const int bitrate : {32, 48, 64, 96, 128, 160, 192, 256, 320}) {
        if (supportsAudioBitrate(preset, sampleRate, bitrate)) result.push_back(bitrate);
    }
    return result;
}

VideoGeometry predictVideoGeometry(const SourceInfo& source, const Options& options) {
    if (source.width <= 0 || source.height <= 0) return {};
    const auto originalAspect = source.aspectNumerator > 0 && source.aspectDenominator > 0
        ? Aspect{source.aspectNumerator, source.aspectDenominator} : reduceAspect(source.width, source.height);
    const auto [numerator, denominator] = originalAspect;
    if (options.remux) return {source.width, source.height, numerator, denominator};

    const bool customSize = options.width > 0 || options.height > 0;
    if (options.resolution != 0) {
        if (customSize || !supportedVideoResolution(options.resolution)) return {};
        const int shortEdge = options.resolution;
        const int longEdge = videoResolutionLongEdge(shortEdge);
        const double dar = static_cast<double>(numerator) / denominator;
        const auto widthLimit = dar >= 1 ? (std::min)(static_cast<double>(longEdge), shortEdge * dar)
                                        : (std::min)(static_cast<double>(shortEdge), longEdge * dar);
        const auto heightLimit = dar >= 1 ? (std::min)(static_cast<double>(shortEdge), longEdge / dar)
                                         : (std::min)(static_cast<double>(longEdge), shortEdge / dar);
        const int width = (std::max)(2, static_cast<int>(std::round(widthLimit / 2)) * 2);
        const int height = (std::max)(2, static_cast<int>(std::round(heightLimit / 2)) * 2);
        const auto aspect = source.hasSampleAspect ? originalAspect : reduceAspect(width, height);
        return {width, height, aspect.first, aspect.second};
    }

    long long width = source.width;
    long long height = source.height;
    if (customSize) {
        width = options.width > 0 ? options.width : scaledEvenDimension(options.height, source.width, source.height);
        height = options.height > 0 ? options.height : scaledEvenDimension(options.width, source.height, source.width);
    }
    constexpr int maximumEven = (std::numeric_limits<int>::max)() - 1;
    if (width < 1 || height < 1 || width > maximumEven || height > maximumEven) return {};
    const int paddedWidth = static_cast<int>(width + width % 2);
    const int paddedHeight = static_cast<int>(height + height % 2);
    const auto aspect = source.hasSampleAspect
        ? applyVideoPadding(originalAspect, static_cast<int>(width), static_cast<int>(height), paddedWidth, paddedHeight)
        : reduceAspect(paddedWidth, paddedHeight);
    return {paddedWidth, paddedHeight, aspect.first, aspect.second};
}

SourceInfo probeMedia(const fs::path& input, const Cancel& cancel) {
    const auto media = inspect(input, cancel);
    SourceInfo source;
    if (!media.hasVideo && !media.hasAudio) {
        source.detail = media.diagnostic;
        return source;
    }

    const auto extension = lowercase(input.extension().wstring());
    source.container = containerLabel(media, extension);
    const bool image = in(extension, {L".jpg", L".jpeg", L".png", L".apng", L".gif", L".webp", L".bmp",
        L".tiff", L".tif", L".tga", L".ico", L".avif", L".heif", L".heic", L".jfif"})
        || in(media.container, {L"image2", L"gif", L"apng", L"png_pipe", L"jpeg_pipe", L"webp_pipe"})
        || in(source.container, {L"HEIC", L"HEIF", L"AVIF"});
    source.kind = media.hasVideo ? (image ? Kind::Image : Kind::Video) : Kind::Audio;
    source.animated = image && (media.video.frames > 1 || media.container == L"apng"
        || (media.container == L"gif" && media.video.frames != 1));
    source.videoCodec = codecLabel(media.video.codec);
    source.audioCodec = codecLabel(media.audio.codec);
    source.hasAudio = media.hasAudio;
    if (media.hasVideo && !media.hasTiles) {
        source.width = media.video.width;
        source.height = media.video.height;
        auto pixelWidth = media.video.sampleAspectNumerator;
        auto pixelHeight = media.video.sampleAspectDenominator;
        source.hasSampleAspect = pixelWidth > 0 && pixelHeight > 0;
        if (std::abs(media.video.rotation % 180) == 90) {
            std::swap(source.width, source.height);
            std::swap(pixelWidth, pixelHeight);
        }
        if (source.width > 0 && source.height > 0) {
            const auto [numerator, denominator] = source.hasSampleAspect
                ? reduceAspect(1LL * source.width * pixelWidth, 1LL * source.height * pixelHeight)
                : reduceAspect(source.width, source.height);
            source.aspectNumerator = numerator;
            source.aspectDenominator = denominator;
        }
    }

    const auto append = [&source](std::wstring_view value) {
        if (value.empty()) return;
        if (!source.detail.empty()) source.detail += L" · ";
        source.detail += value;
    };
    append(source.container);
    append(source.videoCodec);
    if (source.width > 0 && source.height > 0) {
        append(std::to_wstring(source.width) + L"×" + std::to_wstring(source.height));
    } else if (media.hasTiles) {
        append(L"分块图像");
    }
    if (source.audioCodec != source.container) append(source.audioCodec);
    return source;
}

Result convertMedia(const Job& job, const fs::path& temporaryOutput,
                    const fs::path& scratch, const Cancel& cancel, const Progress& progress) {
    const auto format = findFormat(job.preset);
    if (!format) return {false, false, L"找不到所选转换格式。", job.preset, {}};
    const bool videoOutput = format->kind == Kind::Video;
    const bool customSize = job.options.width > 0 || job.options.height > 0;
    const bool resolutionPreset = videoOutput && job.options.resolution != 0;
    if (resolutionPreset) {
        if (!supportedVideoResolution(job.options.resolution)) {
            return {false, false, L"所选视频分辨率无效。", {}, {}};
        }
        if (customSize) return {false, false, L"请选择分辨率预设或自定义宽高，两者只能选一项。", {}, {}};
    }
    if (job.options.remux && (format->kind == Kind::Image || (videoOutput && (customSize
        || resolutionPreset || job.options.videoKbps > 0)) || job.options.audioKbps > 0
        || job.options.channels > 0 || job.options.audioSampleRate != 0)) {
        return {false, false, L"只转封装无法同时修改尺寸、码率、采样率或声道，也不适用于图像输出。", {}, {}};
    }
    if (format->kind == Kind::Image && job.options.imageLongEdge != 0) {
        constexpr int pictures[] = {320, 640, 960, 1280, 1920, 2560, 3840};
        constexpr int icons[] = {16, 32, 48, 64, 128, 256};
        const auto edge = job.options.imageLongEdge;
        const bool supported = format->id == L"ico"
            ? std::find(std::begin(icons), std::end(icons), edge) != std::end(icons)
            : std::find(std::begin(pictures), std::end(pictures), edge) != std::end(pictures);
        if (!supported) return {false, false, L"所选图片尺寸无效。", {}, {}};
        if (customSize) return {false, false, L"请选择图片尺寸预设或自定义宽高，两者只能选一项。", {}, {}};
    }
    if (job.options.audioSampleRate != 0) {
        const auto rates = sampleRatesForEncoder(format->audioEncoder);
        if (std::find(rates.begin(), rates.end(), job.options.audioSampleRate) == rates.end()) {
            return {false, false, L"所选格式不支持此音频采样率。", {}, {}};
        }
    }

    if (progress) progress(-1, L"正在检查文件");
    const auto media = inspect(job.input, cancel);
    if (cancel.load()) return {false, true, L"已取消", {}, {}};
    if ((format->kind == Kind::Audio && !media.hasAudio) || (format->kind != Kind::Audio && !media.hasVideo)) {
        if (!media.hasVideo && !media.hasAudio) {
            return {false, false, L"无法读取媒体文件。", media.diagnostic, {}};
        }
        return {false, false, format->kind == Kind::Audio ? L"输入文件没有可转换的音轨。" :
            L"输入文件没有可转换的画面。", {}, {}};
    }

    auto input = job;
    if (media.hasAudio && !format->audioEncoder.empty() && !job.options.remux) {
        const auto encoder = format->audioEncoder;
        const int channelOverride = audioChannelOverride(encoder, job.options.channels);
        const int channels = channelOverride > 0 ? channelOverride : media.audio.channels;
        auto& rate = input.options.audioSampleRate;
        auto& kbps = input.options.audioKbps;
        if (kbps != 0) {
            if (rate == 0) rate = compatibleAudioRate(encoder, media.audio.sampleRate, kbps, channels);
            if (!rate || !supportedBitrate(encoder, rate, kbps, channels)) {
                return {false, false, L"所选音频码率与格式、采样率或声道不兼容。请选择其他码率或使用自动设置。", {}, {}};
            }
        } else if (format->kind == Kind::Audio && !isLosslessAudio(encoder) && qualityIndex(job.options) != 1) {
            if (rate == 0) rate = compatibleAudioRate(encoder, media.audio.sampleRate, 0, channels);
            kbps = qualityAudioBitrate(encoder, rate, channels, qualityIndex(job.options));
        }
    }
    if (progress) progress(0, L"正在转换");
    ProcessResult child;
    const auto engine = enginePath(L"ffmpeg.exe");
    const auto onOutput = progressOutput(progress, media.duration);
    auto videoStream = L"0:" + std::to_wstring(media.video.index);
    auto audioStream = media.hasAudio ? L"0:" + std::to_wstring(media.audio.index) : std::wstring{};
    if (media.hasTiles && format->kind != Kind::Audio) {
        const auto grid = inspectGrid(job.input, cancel);
        if (cancel.load()) return {false, true, L"已取消", {}, {}};
        const auto filter = gridFilter(grid);
        if (filter.empty() || job.options.remux) {
            return {false, false, L"此 HEIC 的分块布局无法合成，请在原设备导出为 JPEG 或 PNG。", {}, {}};
        }
        if (progress) progress(-1, L"正在合成 HEIC 分块图像");
        input.input = scratch / L"heic-image.png";
        videoStream = L"0:0";
        audioStream.clear();
        auto args = inputArguments(job.input, 1, false);
        add(args, {L"-filter_complex", filter, L"-map", L"[image]", L"-frames:v", L"1",
            L"-c:v", L"png", L"-compression_level", L"1", L"-threads:v", L"1",
            L"-f", L"image2", L"-update", L"1", input.input.wstring()});
        const auto merged = runProcess(engine, args, cancel);
        if (merged.cancelled || cancel.load()) return {false, true, L"已取消", {}, {}};
        if (merged.exitCode != 0) {
            return {false, false, L"无法合成此 HEIC 的分块，请在原设备导出为 JPEG 或 PNG。", fromUtf8(merged.error), {}};
        }
    }
    if (format->id == L"gif") {
        child = gifConversion(input, temporaryOutput, scratch, cancel, progress, media.duration, videoStream);
    } else {
#ifdef TTK_MODERN
        if (job.options.hardware && !job.options.remux && format->kind == Kind::Video &&
            in(format->videoEncoder, {L"libx264", L"libx265"})) {
            const std::wstring_view avc[] = {L"h264_nvenc", L"h264_qsv", L"h264_amf"};
            const std::wstring_view hevc[] = {L"hevc_nvenc", L"hevc_qsv", L"hevc_amf"};
            const auto candidates = format->videoEncoder == L"libx264" ? avc : hevc;
            const auto support = runProcess(engine, {L"-hide_banner", L"-encoders"}, cancel);
            for (size_t index = 0; index < std::size(avc) && !cancel.load(); ++index) {
                const auto candidate = candidates[index];
                if (support.output.find(toUtf8(std::wstring(candidate))) == std::string::npos) continue;
                child = runProcess(engine, conversionArguments(input, temporaryOutput, *format, videoStream, audioStream, candidate),
                    cancel, {}, onOutput);
                if (child.exitCode == 0 || child.cancelled) break;
            }
            if (child.exitCode != 0 && !child.cancelled && !cancel.load() && progress) {
                progress(0, L"硬件编码不可用，正在使用软件编码");
            }
        }
#endif
        if (child.exitCode != 0 && !child.cancelled && !cancel.load()) {
            child = runProcess(engine, conversionArguments(input, temporaryOutput, *format, videoStream, audioStream),
                cancel, {}, onOutput);
        }
    }

    if (child.cancelled || cancel.load()) return {false, true, L"已取消", {}, {}};
    std::error_code error;
    const auto bytes = fs::file_size(temporaryOutput, error);
    if (child.exitCode != 0 || error || bytes == 0) {
        return {false, false, job.options.remux ? L"所选封装无法直接写入这些数据流。" : L"媒体转换失败。",
            fromUtf8(child.error), {}};
    }
    return {true, false, L"转换完成", {}, temporaryOutput};
}

} // namespace ttk
