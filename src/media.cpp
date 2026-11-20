#include "ttk.hpp"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <initializer_list>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
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

struct Stream {
    int index = 0;
    std::string type;
    std::string codec;
    bool attachedPicture = false;
    bool preferred = false;
    int width = 0;
    int height = 0;
    int frames = 0;
    double duration = 0;
};

struct MediaInfo {
    Stream video;
    Stream audio;
    bool hasVideo = false;
    bool hasAudio = false;
    bool hasTiles = false;
    std::wstring container;
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

MediaInfo inspect(const fs::path& path, const Cancel& cancel) {
    const auto probe = runProcess(enginePath(L"ffprobe.exe"), {
        L"-v", L"error", L"-protocol_whitelist", L"file,pipe",
        L"-show_entries", L"stream=index,codec_name,codec_type,width,height,duration,nb_frames:stream_disposition=attached_pic,default:stream_group=type:format=duration,format_name",
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
            } else if (stream.type == "audio" && !media.hasAudio) {
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
            if (key == "nb_frames") stream.frames = count(value);
            if (key == "duration") stream.duration = number(value);
            if (key == "DISPOSITION:attached_pic") stream.attachedPicture = value == "1";
            if (key == "DISPOSITION:default") stream.preferred = value == "1";
        } else {
            if (key == "format_name") media.container = fromUtf8(value);
            if (key == "duration") media.duration = number(value);
        }
    }

    if (media.duration <= 0) {
        media.duration = (std::max)(media.video.duration, media.audio.duration);
    }
    return media;
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
    return (std::clamp)(options.quality, 0, 2);
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

void audioArguments(Arguments& arguments, const MediaFormat& format, const Options& options) {
    const auto encoder = format.audioEncoder;
    add(arguments, {L"-c:a", encoder});
    if (in(encoder, {L"flac", L"alac", L"pcm_s16le", L"pcm_s16be"})) {
        if (encoder == L"flac") {
            const std::wstring_view compression[] = {L"2", L"5", L"9"};
            add(arguments, {L"-compression_level", compression[qualityIndex(options)]});
        }
    } else {
        const int preferred = encoder == L"ac3" ? 384 : encoder == L"mp2" ? 192 : 160;
        add(arguments, {L"-b:a", bitrate(options.audioKbps > 0 ? options.audioKbps : preferred)});
    }

    int channels = options.channels;
    if (channels == 0 && in(encoder, {L"libmp3lame", L"wmav2", L"mp2"})) {
        channels = 2;
    }
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
    if (auto filter = resizing(options, true); !filter.empty()) {
        add(arguments, {L"-vf", filter});
    }
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
            const std::wstring_view qp[] = {L"20", L"24", L"29"};
            if (hardwareEncoder.find(L"_nvenc") != std::wstring_view::npos) {
                add(arguments, {L"-preset", L"p4", L"-rc", L"constqp", L"-qp", qp[quality]});
            } else if (hardwareEncoder.find(L"_qsv") != std::wstring_view::npos) {
                add(arguments, {L"-global_quality", qp[quality]});
            } else {
                add(arguments, {L"-rc", L"cqp", L"-qp_i", qp[quality], L"-qp_p", qp[quality]});
            }
        }
    } else if (in(encoder, {L"libx264", L"libx265"})) {
        const std::wstring_view presets[] = {L"veryfast", L"fast", L"medium"};
        const std::wstring_view crfH264[] = {L"20", L"23", L"28"};
        const std::wstring_view crfH265[] = {L"22", L"26", L"30"};
        add(arguments, {L"-preset", presets[quality]});
        if (options.videoKbps <= 0) {
            add(arguments, {L"-crf", encoder == L"libx265" ? crfH265[quality] : crfH264[quality]});
        }
        if (encoder == L"libx265" && threadCount(options) > 0) {
            add(arguments, {L"-x265-params", L"pools=" + std::to_wstring(threadCount(options)) + L":frame-threads=1"});
        }
    } else if (encoder == L"libaom-av1") {
        const std::wstring_view crf[] = {L"28", L"32", L"39"};
#ifdef TTK_LEGACY_X86
        add(arguments, {L"-cpu-used", L"8", L"-lag-in-frames", L"0"});
#else
        const std::wstring_view speed[] = {L"8", L"6", L"5"};
        add(arguments, {L"-cpu-used", speed[quality]});
#endif
        add(arguments, {L"-row-mt", L"1"});
        if (options.videoKbps <= 0) {
            add(arguments, {L"-crf", crf[quality], L"-b:v", L"0"});
        }
    } else if (in(encoder, {L"libvpx", L"libvpx-vp9"})) {
        const std::wstring_view speed[] = {L"6", L"4", L"2"};
        const std::wstring_view crf[] = {L"22", L"30", L"38"};
        add(arguments, {L"-deadline", L"good", L"-cpu-used", speed[quality]});
        if (encoder == L"libvpx-vp9") add(arguments, {L"-row-mt", L"1"});
        if (options.videoKbps <= 0) {
            add(arguments, {L"-crf", crf[quality], L"-b:v", encoder == L"libvpx" ? L"2M" : L"0"});
        }
    } else if (options.videoKbps <= 0) {
        const std::wstring_view quantizer[] = {L"3", L"5", L"8"};
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

    auto filter = resizing(options, false);
    if (format.id == L"ico") {
        if (!filter.empty()) filter += L",";
        filter += L"scale=min(256\\,iw):min(256\\,ih):force_original_aspect_ratio=decrease:flags=lanczos";
        add(arguments, {L"-pix_fmt", L"rgba"});
    } else if (format.id == L"jpg") {
        const std::wstring_view quantizer[] = {L"2", L"3", L"5"};
        add(arguments, {L"-q:v", quantizer[quality]});
    } else if (format.id == L"webp") {
        const std::wstring_view targetQuality[] = {L"90", L"82", L"72"};
        add(arguments, {L"-quality", targetQuality[quality]});
    } else if (format.id == L"avif") {
        const std::wstring_view crf[] = {L"18", L"24", L"31"};
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
    auto filter = resizing(job.options, false);
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
                              const std::wstring& videoStream, std::wstring_view hardwareEncoder = {}) {
    auto arguments = inputArguments(job.input, threadCount(job.options));
    switch (format.kind) {
    case Kind::Video:
        if (job.options.remux) {
            if (format.id.substr(0, 4) == L"mkv-") add(arguments, {L"-map", L"0"});
            else add(arguments, {L"-map", L"0:V", L"-map", L"0:a?", L"-map", L"0:s?"});
            add(arguments, {L"-c", L"copy"});
        } else {
            add(arguments, {L"-map", videoStream, L"-map", L"0:a:0?"});
            videoArguments(arguments, format, job.options, hardwareEncoder);
            audioArguments(arguments, format, job.options);
        }
        break;
    case Kind::Audio:
        add(arguments, {L"-map", L"0:a:0", L"-vn"});
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

SourceInfo probeMedia(const fs::path& input, const Cancel& cancel) {
    const auto media = inspect(input, cancel);
    if (!media.hasVideo && !media.hasAudio) return {Kind::Unknown, false, media.diagnostic};

    const auto extension = lowercase(input.extension().wstring());
    const bool image = in(extension, {L".jpg", L".jpeg", L".png", L".apng", L".gif", L".webp", L".bmp",
        L".tiff", L".tif", L".tga", L".ico", L".avif", L".heif", L".heic", L".jfif"})
        || in(media.container, {L"image2", L"gif", L"apng", L"png_pipe", L"jpeg_pipe", L"webp_pipe"});
    const auto kind = media.hasVideo ? (image ? Kind::Image : Kind::Video) : Kind::Audio;
    const bool animated = image && (media.video.frames > 1 || media.container == L"apng"
        || (media.container == L"gif" && media.video.frames != 1));
    std::wstring detail;
    if (media.hasVideo) {
        detail = fromUtf8(media.video.codec);
        if (media.hasTiles) {
            detail += L" · 分块图像";
        } else if (media.video.width > 0 && media.video.height > 0) {
            detail += L" · " + std::to_wstring(media.video.width) + L"×" + std::to_wstring(media.video.height);
        }
    }
    if (media.hasAudio) {
        if (!detail.empty()) detail += L" · ";
        detail += fromUtf8(media.audio.codec);
    }
    return {kind, animated, detail};
}

Result convertMedia(const Job& job, const fs::path& temporaryOutput,
                    const fs::path& scratch, const Cancel& cancel, const Progress& progress) {
    const auto format = findFormat(job.preset);
    if (!format) return {false, false, L"找不到所选转换格式。", job.preset, {}};
    if (job.options.remux && (format->kind == Kind::Image || job.options.width > 0
        || job.options.height > 0 || job.options.videoKbps > 0 || job.options.audioKbps > 0
        || job.options.channels > 0)) {
        return {false, false, L"只转封装无法同时修改尺寸、码率或声道，也不适用于图像输出。", {}, {}};
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

    if (progress) progress(0, L"正在转换");
    ProcessResult child;
    const auto engine = enginePath(L"ffmpeg.exe");
    const auto onOutput = progressOutput(progress, media.duration);
    auto input = job;
    auto videoStream = L"0:" + std::to_wstring(media.video.index);
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
                child = runProcess(engine, conversionArguments(input, temporaryOutput, *format, videoStream, candidate),
                    cancel, {}, onOutput);
                if (child.exitCode == 0 || child.cancelled) break;
            }
            if (child.exitCode != 0 && !child.cancelled && !cancel.load() && progress) {
                progress(0, L"硬件编码不可用，正在使用软件编码");
            }
        }
#endif
        if (child.exitCode != 0 && !child.cancelled && !cancel.load()) {
            child = runProcess(engine, conversionArguments(input, temporaryOutput, *format, videoStream), cancel, {}, onOutput);
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
