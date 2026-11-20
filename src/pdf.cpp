#include "ttk.hpp"
#include "pdf_layout.hpp"
#include "pdf_ocr_layout.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace ttk {
namespace {

constexpr size_t maxTextBytes = 32 * 1024 * 1024;
constexpr size_t maxXmlBytes = 64 * 1024 * 1024;
constexpr uint64_t maxRasterPixels = 16000000;
constexpr uint64_t maxDocxBytes = 2ULL * 1024 * 1024 * 1024;

struct Cell {
    int left;
    std::string text;
};

using Row = std::vector<Cell>;
using Rows = std::vector<Row>;

struct Word {
    int left;
    int top;
    int width;
    int height;
    std::string text;
};

struct PdfError {
    std::wstring message;
    std::wstring details;
};

struct PdfCancelled {};

void checkCancelled(const Cancel& cancel) {
    if (cancel.load()) throw PdfCancelled{};
}

std::string_view trim(std::string_view value) {
    constexpr std::string_view whitespace = " \t\r\n\f";
    const auto first = value.find_first_not_of(whitespace);
    if (first == std::string_view::npos) return {};
    return value.substr(first, value.find_last_not_of(whitespace) - first + 1);
}

std::vector<std::string_view> split(std::string_view text, char delimiter) {
    std::vector<std::string_view> parts;
    while (true) {
        const auto next = text.find(delimiter);
        parts.push_back(text.substr(0, next));
        if (next == std::string_view::npos) return parts;
        text.remove_prefix(next + 1);
    }
}

bool parseNumber(std::string_view value, int& number) {
    value = trim(value);
    if (value.empty()) return false;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size();
}

std::string readText(const fs::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw PdfError{L"无法读取 PDF 转换结果。", path.wstring()};
    const auto size = input.tellg();
    if (size < 0 || size > static_cast<std::streamoff>(maxTextBytes)) {
        throw PdfError{L"PDF 提取文字超过当前处理上限。", path.wstring()};
    }
    std::string content(static_cast<size_t>(size), '\0');
    input.seekg(0);
    if (!input.read(content.data(), static_cast<std::streamsize>(content.size()))) {
        throw PdfError{L"读取 PDF 转换结果时出错。", path.wstring()};
    }
    return content;
}

size_t textWidth(std::string_view text) {
    return static_cast<size_t>(std::count_if(text.begin(), text.end(), [](unsigned char c) {
        return (c & 0xC0) != 0x80;
    }));
}

bool hasUsableText(std::string_view text) {
    size_t usable = 0;
    size_t replacements = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == 0xEF && text.substr(i, 3) == "\xEF\xBF\xBD") {
            ++replacements;
            i += 2;
        } else if (c > ' ' && (c & 0xC0) != 0x80 && c != 0x7F) {
            ++usable;
        }
    }
    return usable > replacements;
}

std::vector<std::string_view> splitPages(std::string_view text, size_t expected) {
    auto pages = split(text, '\f');
    if (pages.size() == expected + 1 && trim(pages.back()).empty()) pages.pop_back();
    if (pages.size() != expected) {
        throw PdfError{L"无法可靠对应 PDF 和提取文字的页码。", {}};
    }
    if (!pages.empty() && pages.front().substr(0, 3) == "\xEF\xBB\xBF") {
        pages.front().remove_prefix(3);
    }
    return pages;
}

void appendText(std::string& text, std::string_view line) {
    if (line.empty()) return;
    if (!text.empty() && (static_cast<unsigned char>(text.back()) < 0x80 ||
                         static_cast<unsigned char>(line.front()) < 0x80)) {
        text += ' ';
    }
    text.append(line);
}

void appendXml(std::string& xml, std::string_view value) {
    for (unsigned char c : value) {
        switch (c) {
        case '&': xml += "&amp;"; break;
        case '<': xml += "&lt;"; break;
        case '>': xml += "&gt;"; break;
        case '\t': xml += "&#9;"; break;
        default:
            if (c >= 0x20) xml += static_cast<char>(c);
            break;
        }
    }
}

void appendParagraph(std::string& xml, std::string_view value) {
    xml += "<w:p><w:r><w:t xml:space=\"preserve\">";
    appendXml(xml, value);
    xml += "</w:t></w:r></w:p>";
}

void appendParagraphs(std::string& xml, std::string_view text) {
    std::string paragraph;
    for (auto line : split(text, '\n')) {
        line = trim(line);
        if (!line.empty()) appendText(paragraph, line);
        else if (!paragraph.empty()) {
            appendParagraph(xml, paragraph);
            paragraph.clear();
        }
    }
    if (!paragraph.empty()) appendParagraph(xml, paragraph);
}

void appendTable(std::string& xml, const Rows& rows, size_t first, size_t end) {
    const auto columns = rows[first].size();
    const auto columnWidth = std::to_string(9000 / columns);
    xml += "<w:tbl><w:tblPr><w:tblW w:w=\"0\" w:type=\"auto\"/><w:tblBorders>";
    for (const char* border : {"top", "left", "bottom", "right", "insideH", "insideV"}) {
        xml += "<w:";
        xml += border;
        xml += " w:val=\"single\" w:sz=\"4\" w:color=\"B5BDC7\"/>";
    }
    xml += "</w:tblBorders></w:tblPr><w:tblGrid>";
    for (size_t col = 0; col < columns; ++col) {
        xml += "<w:gridCol w:w=\"" + columnWidth + "\"/>";
    }
    xml += "</w:tblGrid>";
    for (size_t row = first; row < end; ++row) {
        if (rows[row].empty()) continue;
        xml += "<w:tr>";
        for (const auto& cell : rows[row]) {
            xml += "<w:tc><w:tcPr><w:tcW w:w=\"" + columnWidth + "\" w:type=\"dxa\"/></w:tcPr>";
            appendParagraph(xml, cell.text);
            xml += "</w:tc>";
        }
        xml += "</w:tr>";
    }
    xml += "</w:tbl>";
}

bool isSimpleRow(const Row& row) {
    return row.size() >= 2 && row.size() <= 10 &&
           std::all_of(row.begin(), row.end(), [](const Cell& cell) {
               return !cell.text.empty() && textWidth(cell.text) <= 36;
           });
}

bool sameColumns(const Row& anchor, const Row& candidate, int tolerance) {
    if (anchor.size() != candidate.size() || !isSimpleRow(candidate)) return false;
    for (size_t col = 0; col < anchor.size(); ++col) {
        if (std::abs(anchor[col].left - candidate[col].left) > tolerance) return false;
    }
    return true;
}

std::string cellsToText(const Row& row) {
    std::string line;
    for (const auto& cell : row) {
        if (!line.empty()) line += ' ';
        line += cell.text;
    }
    return line;
}

bool appendRows(std::string& xml, const Rows& rows, int tolerance) {
    std::string page;
    std::string paragraph;
    bool foundTable = false;
    for (size_t index = 0; index < rows.size();) {
        size_t end = index + 1;
        size_t matched = 1;
        if (isSimpleRow(rows[index])) {
            while (end < rows.size()) {
                const auto next = rows[end].empty() ? end + 1 : end;
                if (next >= rows.size() || !sameColumns(rows[index], rows[next], tolerance)) break;
                end = next + 1;
                ++matched;
            }
        }
        if (matched >= 2) {
            if (!paragraph.empty()) appendParagraph(page, paragraph);
            paragraph.clear();
            appendTable(page, rows, index, end);
            foundTable = true;
        } else if (rows[index].empty()) {
            if (!paragraph.empty()) appendParagraph(page, paragraph);
            paragraph.clear();
        } else {
            appendText(paragraph, cellsToText(rows[index]));
        }
        index = end;
    }
    if (!paragraph.empty()) appendParagraph(page, paragraph);
    if (foundTable) xml += page;
    return foundTable;
}

Rows layoutRows(std::string_view page) {
    Rows rows;
    for (const auto line : split(page, '\n')) {
        Row row;
        for (size_t cellStart = 0; cellStart < line.size();) {
            const size_t start = line.find_first_not_of(" \t\r", cellStart);
            if (start == std::string_view::npos) break;
            const auto separator = line.find("  ", start);
            const auto end = separator == std::string_view::npos ? line.size() : separator;
            const auto cell = trim(line.substr(start, end - start));
            if (!cell.empty()) row.push_back({static_cast<int>(textWidth(line.substr(0, start))), std::string(cell)});
            cellStart = end;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<Word> parseTsv(std::string_view text) {
    std::vector<Word> words;
    for (auto line : split(text, '\n')) {
        const auto columns = split(line, '\t');
        if (columns.size() < 12 || columns[0] != "5" || !hasUsableText(columns[11])) continue;
        Word word{0, 0, 0, 0, std::string(trim(columns[11]))};
        if (!parseNumber(columns[6], word.left) || !parseNumber(columns[7], word.top) ||
            !parseNumber(columns[8], word.width) || !parseNumber(columns[9], word.height) ||
            word.left < 0 || word.top < 0 || word.width <= 0 || word.height <= 0 ||
            word.left >= 100000 || word.top >= 100000 || word.width >= 100000 || word.height >= 100000) {
            continue;
        }
        words.push_back(std::move(word));
    }
    return words;
}

int wordCenter(const Word& word) {
    return word.top + word.height / 2;
}

Rows ocrRows(std::vector<Word> words, int& columnTolerance) {
    Rows rows;
    if (words.empty()) return rows;
    std::vector<int> heights;
    heights.reserve(words.size());
    for (const auto& word : words) heights.push_back(word.height);
    auto middle = heights.begin() + heights.size() / 2;
    std::nth_element(heights.begin(), middle, heights.end());
    const int medianHeight = *middle;
    const int lineTolerance = (std::max)(3, medianHeight / 2);
    const int cellGap = (std::max)(12, medianHeight);
    columnTolerance = (std::max)(8, medianHeight / 2);

    std::stable_sort(words.begin(), words.end(), [](const Word& a, const Word& b) {
        return wordCenter(a) < wordCenter(b);
    });
    int previousCenter = wordCenter(words.front());
    for (auto first = words.begin(); first != words.end();) {
        const int center = wordCenter(*first);
        auto end = first + 1;
        while (end != words.end() && wordCenter(*end) - center <= lineTolerance) ++end;
        if (center - previousCenter > medianHeight * 2) rows.emplace_back();
        previousCenter = center;
        std::sort(first, end, [](const Word& a, const Word& b) { return a.left < b.left; });
        Row row;
        int right = -cellGap;
        for (auto word = first; word != end; ++word) {
            if (row.empty() || word->left - right >= cellGap) {
                row.push_back({word->left, std::move(word->text)});
            } else {
                appendText(row.back().text, word->text);
            }
            right = (std::max)(right, word->left + word->width);
        }
        rows.push_back(std::move(row));
        first = end;
    }
    return rows;
}

uint32_t updateCrc(uint32_t crc, std::string_view bytes) {
    static const auto table = [] {
        std::array<uint32_t, 256> entries{};
        for (uint32_t value = 0; value < entries.size(); ++value) {
            uint32_t crc = value;
            for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320U : 0);
            entries[value] = crc;
        }
        return entries;
    }();
    for (unsigned char byte : bytes) crc = (crc >> 8) ^ table[(crc ^ byte) & 0xFF];
    return crc;
}

void write16(std::ostream& stream, uint16_t value) {
    stream.put(static_cast<char>(value & 0xFF));
    stream.put(static_cast<char>(value >> 8));
}

void write32(std::ostream& stream, uint32_t value) {
    write16(stream, static_cast<uint16_t>(value & 0xFFFF));
    write16(stream, static_cast<uint16_t>(value >> 16));
}

struct ZipEntry {
    std::string name;
    uint32_t size = 0;
    uint32_t offset = 0;
    uint32_t crc = 0;
};

void writeZipMetadata(std::ostream& output, const ZipEntry& entry) {
    write16(output, 20); // Version needed to extract.
    write16(output, 0);  // No special flags.
    write16(output, 0);  // Stored without compression.
    write16(output, 0);  // DOS time.
    write16(output, 33); // 1980-01-01 in DOS date format.
    write32(output, entry.crc);
    write32(output, entry.size);
    write32(output, entry.size);
    write16(output, static_cast<uint16_t>(entry.name.size()));
    write16(output, 0); // Extra field length.
}

class DocxPackage {
public:
    DocxPackage(const fs::path& path, const Cancel& cancel)
        : stream_(path, std::ios::binary | std::ios::trunc), path_(path), cancel_(cancel) {
        if (!stream_) throw PdfError{L"无法创建 Word 文件。", path_.wstring()};
    }

    void addXml(const std::string& name, std::string_view content) {
        const auto crc = updateCrc(0xFFFFFFFFU, content) ^ 0xFFFFFFFFU;
        beginEntry(name, content.size(), crc);
        stream_.write(content.data(), static_cast<std::streamsize>(content.size()));
        position();
    }

    void addPng(const std::string& name, const fs::path& path) {
        std::ifstream image(path, std::ios::binary | std::ios::ate);
        const auto size = image.tellg();
        if (!image || size < 24 || size > static_cast<std::streamoff>(64 * 1024 * 1024)) {
            throw PdfError{L"无法读取渲染后的 PDF 页面。", path.wstring()};
        }
        image.seekg(0);
        std::array<char, 65536> block{};
        auto crc = 0xFFFFFFFFU;
        const auto offset = beginEntry(name, static_cast<uint64_t>(size), 0);
        for (auto remaining = static_cast<uint64_t>(size); remaining;) {
            checkCancelled(cancel_);
            const auto length = static_cast<std::streamsize>(
                (std::min)(remaining, static_cast<uint64_t>(block.size())));
            if (!image.read(block.data(), length)) {
                throw PdfError{L"读取渲染后的 PDF 页面时出错。", path.wstring()};
            }
            crc = updateCrc(crc, {block.data(), static_cast<size_t>(length)});
            stream_.write(block.data(), length);
            remaining -= static_cast<uint64_t>(length);
        }
        const auto end = position();
        entries_.back().crc = crc ^ 0xFFFFFFFFU;
        stream_.seekp(offset + 14); // Update the CRC in the local ZIP header.
        write32(stream_, entries_.back().crc);
        stream_.seekp(end);
        position();
    }

    void finish(std::string_view document, std::string_view documentRelationships = {}) {
        constexpr std::string_view relationships =
            R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>)";
        std::string types =
            R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>)";
        if (!documentRelationships.empty()) {
            types += R"(<Default Extension="png" ContentType="image/png"/>)";
            addXml("word/_rels/document.xml.rels", documentRelationships);
        }
        types += R"(<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/></Types>)";
        addXml("[Content_Types].xml", types);
        addXml("_rels/.rels", relationships);
        addXml("word/document.xml", document);

        const auto directory = position();
        for (const auto& entry : entries_) {
            checkCancelled(cancel_);
            write32(stream_, 0x02014B50);
            write16(stream_, 20); // DOS creator, ZIP version 2.0.
            writeZipMetadata(stream_, entry);
            write16(stream_, 0); // Comment length.
            write16(stream_, 0); // Disk number.
            write16(stream_, 0); // Internal attributes.
            write32(stream_, 0); // External attributes.
            write32(stream_, entry.offset);
            stream_.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
        }
        const auto directorySize = position() - directory;
        write32(stream_, 0x06054B50);
        write16(stream_, 0);
        write16(stream_, 0);
        write16(stream_, static_cast<uint16_t>(entries_.size()));
        write16(stream_, static_cast<uint16_t>(entries_.size()));
        write32(stream_, directorySize);
        write32(stream_, directory);
        write16(stream_, 0);
        stream_.close();
        if (!stream_) throw PdfError{L"写入 Word 文件时出错。", path_.wstring()};
    }

private:
    uint32_t position() {
        const auto position = stream_.tellp();
        if (position < 0) throw PdfError{L"写入 Word 文件时出错。", path_.wstring()};
        if (static_cast<uint64_t>(position) > maxDocxBytes) tooLarge();
        return static_cast<uint32_t>(position);
    }

    [[noreturn]] void tooLarge() const {
        throw PdfError{L"Word 文件超出处理上限。请降低页面清晰度或拆分 PDF 后再转换。", path_.wstring()};
    }

    uint32_t beginEntry(const std::string& name, uint64_t size, uint32_t crc) {
        checkCancelled(cancel_);
        const auto offset = position();
        if (size + offset + 30 + name.size() > maxDocxBytes || entries_.size() >= 65534) tooLarge();
        entries_.push_back({name, static_cast<uint32_t>(size), offset, crc});
        write32(stream_, 0x04034B50);
        writeZipMetadata(stream_, entries_.back());
        stream_.write(name.data(), static_cast<std::streamsize>(name.size()));
        return offset;
    }

    std::ofstream stream_;
    fs::path path_;
    const Cancel& cancel_;
    std::vector<ZipEntry> entries_;
};

DWORD CALLBACK copyProgress(LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER,
                            DWORD, DWORD, HANDLE, HANDLE, LPVOID token) {
    return static_cast<const Cancel*>(token)->load() ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}

void stageFile(const fs::path& source, const fs::path& target, const Cancel& cancel,
               const wchar_t* errorMessage) {
    checkCancelled(cancel);
    if (CreateHardLinkW(target.c_str(), source.c_str(), nullptr)) return;
    if (CopyFileExW(source.c_str(), target.c_str(), copyProgress, const_cast<Cancel*>(&cancel),
                    nullptr, COPY_FILE_FAIL_IF_EXISTS)) return;
    const auto error = GetLastError();
    checkCancelled(cancel);
    throw PdfError{errorMessage, source.wstring() + L" (Win32 " + std::to_wstring(error) + L")"};
}

void stageConfig(const fs::path& scratch, const Cancel& cancel) {
    const auto config = enginePath(L"xpdfrc");
    if (!config.empty() && fs::is_regular_file(config)) {
        stageFile(config, scratch / L"xpdfrc", cancel, L"无法准备 PDF 字符映射配置。");
        const auto maps = config.parent_path() / L"xpdf-cmaps";
        if (fs::is_directory(maps)) {
            for (const auto& entry : fs::recursive_directory_iterator(maps)) {
                checkCancelled(cancel);
                const auto target = scratch / L"xpdf-cmaps" / entry.path().lexically_relative(maps);
                if (entry.is_directory()) fs::create_directories(target);
                else if (entry.is_regular_file()) {
                    fs::create_directories(target.parent_path());
                    stageFile(entry.path(), target, cancel, L"无法准备 PDF 字符映射。");
                }
            }
        }
    } else {
        std::ofstream emptyConfig(scratch / L"xpdfrc", std::ios::binary);
        if (!emptyConfig) throw PdfError{L"无法初始化 PDF 转换配置。", scratch.wstring()};
    }
}

bool isDocxPackage(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::array<char, 4> header{};
    return file.read(header.data(), header.size()) &&
        header == std::array<char, 4>{'P', 'K', 3, 4};
}

bool tryStructuredPdf(const fs::path& runtime, const fs::path& worker, const fs::path& scratch,
                      const fs::path& output, const Cancel& cancel, const Progress& progress) {
    const auto resultFile = scratch / L"output.docx";
    std::ofstream settings(scratch / L"opendoc-job.json", std::ios::binary);
    settings << R"({"mode":"fixed"})";
    settings.close();
    if (!settings) return false;
    if (progress) progress(5, L"正在重建可编辑文字和原稿版式");
    checkCancelled(cancel);
    const auto onOutput = [&](const std::string& line) {
        constexpr std::string_view prefix = "page=";
        if (!progress || line.compare(0, prefix.size(), prefix) != 0) return;
        const std::string_view value{line.data() + prefix.size(), line.size() - prefix.size()};
        const auto slash = value.find('/');
        int complete = 0, total = 0;
        if (slash == std::string_view::npos || !parseNumber(value.substr(0, slash), complete) ||
            !parseNumber(value.substr(slash + 1), total) || complete < 0 || total <= 0 || complete > total) return;
        progress(5 + static_cast<int>(static_cast<int64_t>(complete) * 90 / total),
                 L"正在还原版式 " + std::to_wstring(complete) + L"/" + std::to_wstring(total));
    };
    const auto result = runProcess(runtime, {L"-m", worker.wstring(), L"--", runtime.parent_path().wstring()},
                                   cancel, scratch, onOutput);
    if (result.cancelled || cancel.load()) throw PdfCancelled{};
    if (result.exitCode != 0 || !isDocxPackage(resultFile)) return false;
    stageFile(resultFile, output, cancel, L"无法保存高保真 Word 文件。");
    return true;
}

std::string runEngine(const fs::path& executable, const std::vector<std::wstring>& args,
                      const fs::path& scratch, const Cancel& cancel, const wchar_t* failure) {
    checkCancelled(cancel);
    if (executable.empty() || !fs::is_regular_file(executable)) {
        throw PdfError{L"缺少 PDF 转换组件，请重新安装完整离线版。", executable.wstring()};
    }
    const auto result = runProcess(executable, args, cancel, scratch);
    if (result.cancelled || cancel.load()) throw PdfCancelled{};
    if (result.exitCode != 0) {
        std::wstring details = executable.filename().wstring() + L" (" + std::to_wstring(result.exitCode) + L")";
        const auto message = trim(result.error.empty() ? result.output : result.error);
        if (!message.empty()) details += L"\n" + fromUtf8(std::string(message));
        throw PdfError{failure, std::move(details)};
    }
    return result.output;
}

int countPages(std::string_view info) {
    for (const auto line : split(info, '\n')) {
        int pages = 0;
        if (line.substr(0, 6) == "Pages:" && parseNumber(line.substr(6), pages) && pages > 0) return pages;
    }
    throw PdfError{L"无法读取 PDF 页数。", {}};
}

struct PageSize {
    double width;
    double height;
};

PageSize pageSize(std::string_view info) {
    for (const auto line : split(info, '\n')) {
        const auto sizePosition = line.find("size:");
        if (line.substr(0, 4) != "Page" || sizePosition == std::string_view::npos) continue;
        std::istringstream values{std::string(line.substr(sizePosition + 5))};
        values.imbue(std::locale::classic());
        double width = 0;
        double height = 0;
        char separator = 0;
        if (!(values >> width >> separator >> height) || separator != 'x' ||
            !std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) continue;
        const auto rotation = line.find("(rotated ");
        if (rotation != std::string_view::npos) {
            const auto degrees = line.substr(rotation + 9);
            if (degrees.substr(0, 3) == "90 " || degrees.substr(0, 4) == "270 ") {
                std::swap(width, height);
            }
        }
        return {width, height};
    }
    throw PdfError{L"无法读取 PDF 页面尺寸。", {}};
}

int rasterDpi(PageSize size, int requestedDpi) {
    const double bounded = (std::min)({static_cast<double>(requestedDpi),
                                      72 * std::sqrt(maxRasterPixels / (size.width * size.height)),
                                      72 * 12000 / size.width, 72 * 12000 / size.height});
    if (bounded >= 30.0) return static_cast<int>(bounded);
    throw PdfError{L"PDF 页面尺寸超出渲染范围。", {}};
}

void stageOcr(const fs::path& tesseract, const fs::path& scratch, const Cancel& cancel) {
    if (tesseract.empty() || !fs::is_regular_file(tesseract)) {
        throw PdfError{L"缺少离线 OCR，请重新安装完整离线版。", {}};
    }
    const auto modelDir = tesseract.parent_path() / L"tessdata";
    fs::create_directories(scratch / L"tessdata");
    for (const wchar_t* model : {L"chi_sim.traineddata", L"eng.traineddata"}) {
        stageFile(modelDir / model, scratch / L"tessdata" / model, cancel, L"缺少离线 OCR 语言模型。");
    }
}

std::wstring pageImage(int page) {
    auto number = std::to_wstring(page);
    if (number.size() < 6) number.insert(0, 6 - number.size(), L'0');
    return L"page-" + number + L".png";
}

PageSize readPageSize(int page, const fs::path& scratch, const Cancel& cancel) {
    const auto number = std::to_wstring(page);
    return pageSize(runEngine(enginePath(L"pdfinfo.exe"),
        {L"-cfg", L"xpdfrc", L"-f", number, L"-l", number, L"input.pdf"},
        scratch, cancel, L"无法读取 PDF 页面。"));
}

void renderPage(int page, int dpi, const fs::path& scratch, const Cancel& cancel, bool grayscale) {
    const auto number = std::to_wstring(page);
    std::vector<std::wstring> arguments{
        L"-cfg", L"xpdfrc", L"-f", number, L"-l", number, L"-r", std::to_wstring(dpi)};
    if (grayscale) arguments.push_back(L"-gray");
    else arguments.insert(arguments.end(), {L"-aa", L"yes", L"-aaVector", L"yes"});
    arguments.insert(arguments.end(), {L"input.pdf", L"page"});
    runEngine(enginePath(L"pdftopng.exe"), arguments, scratch, cancel, L"无法渲染 PDF 页面。");
}

void appendImageSection(std::string& xml, int width, int height, bool last) {
    xml += R"(<w:sectPr>)";
    if (!last) xml += R"(<w:type w:val="nextPage"/>)";
    xml += R"(<w:pgSz w:w=")" + std::to_string(width) + R"(" w:h=")" + std::to_string(height) + '"';
    if (width > height) xml += R"( w:orient="landscape")";
    xml += R"(/><w:pgMar w:top="0" w:right="0" w:bottom="0" w:left="0" w:header="0" w:footer="0" w:gutter="0"/></w:sectPr>)";
}

void appendImagePage(std::string& xml, PageSize size, const std::string& number, bool last) {
    // Word caps each page edge at 22 inches. Scale oversized pages uniformly.
    const auto scale = (std::min)({20.0, 31680.0 / size.width, 31680.0 / size.height});
    const auto width = (std::max)(1, static_cast<int>(std::lround(size.width * scale)));
    const auto height = (std::max)(1, static_cast<int>(std::lround(size.height * scale)));
    const auto extent = R"( cx=")" + std::to_string(width * 635) + R"(" cy=")" + std::to_string(height * 635) + '"';
    const auto name = "PDF " + number;
    xml += R"(<w:p><w:pPr><w:spacing w:before="0" w:after="0" w:line="20" w:lineRule="exact"/>)";
    if (!last) appendImageSection(xml, width, height, last);
    xml += R"(</w:pPr><w:r><w:drawing><wp:anchor distT="0" distB="0" distL="0" distR="0" simplePos="0" relativeHeight="0" behindDoc="0" locked="0" layoutInCell="1" allowOverlap="1"><wp:simplePos x="0" y="0"/><wp:positionH relativeFrom="page"><wp:posOffset>0</wp:posOffset></wp:positionH><wp:positionV relativeFrom="page"><wp:posOffset>0</wp:posOffset></wp:positionV><wp:extent)";
    xml += extent + R"(/><wp:wrapNone/><wp:docPr id=")" + number + R"(" name=")" + name;
    xml += R"("/><a:graphic><a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/picture"><pic:pic><pic:nvPicPr><pic:cNvPr id=")" + number + R"(" name=")" + name;
    xml += R"("/><pic:cNvPicPr/></pic:nvPicPr><pic:blipFill><a:blip r:embed="rId)" + number;
    xml += R"("/><a:stretch><a:fillRect/></a:stretch></pic:blipFill><pic:spPr><a:xfrm><a:off x="0" y="0"/><a:ext)";
    xml += extent + R"(/></a:xfrm><a:prstGeom prst="rect"><a:avLst/></a:prstGeom></pic:spPr></pic:pic></a:graphicData></a:graphic></wp:anchor></w:drawing></w:r></w:p>)";
    if (last) appendImageSection(xml, width, height, last);
}

Result preserveLayout(const Job& job, int pages, const fs::path& temporaryOutput,
                      const fs::path& scratch, const Cancel& cancel, const Progress& progress) {
    const int dpi = job.options.pdfPageDpi;
    if (dpi != 150 && dpi != 200 && dpi != 300 && dpi != 400) {
        throw PdfError{L"页面清晰度请选择 150、200、300 或 400 DPI。", {}};
    }
    if (job.options.forceOcr) {
        throw PdfError{L"强制 OCR 仅适用于可编辑文字模式。", {}};
    }
    if (pages > 65000) {
        throw PdfError{L"PDF 页数超出处理上限。请拆分后再转换。", {}};
    }
    std::string xml =
        R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing" xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" xmlns:pic="http://schemas.openxmlformats.org/drawingml/2006/picture"><w:body>)";
    std::string relationships =
        R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)";
    DocxPackage package(temporaryOutput, cancel);
    bool boundedDpi = false;
    for (int page = 1; page <= pages; ++page) {
        checkCancelled(cancel);
        if (progress) progress(static_cast<int>(static_cast<int64_t>(page - 1) * 95 / pages),
            L"正在保留原版式 " + std::to_wstring(page) + L"/" + std::to_wstring(pages));
        const auto size = readPageSize(page, scratch, cancel);
        const auto effectiveDpi = rasterDpi(size, dpi);
        boundedDpi = boundedDpi || effectiveDpi < dpi;
        renderPage(page, effectiveDpi, scratch, cancel, false);
        const auto path = scratch / pageImage(page);
        const auto number = std::to_string(page);
        const auto media = "media/page-" + number + ".png";
        package.addPng("word/" + media, path);
        fs::remove(path);
        appendImagePage(xml, size, number, page == pages);
        relationships += R"(<Relationship Id="rId)" + number +
            R"(" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" Target=")" + media + R"("/>)";
        if (xml.size() > maxXmlBytes) throw PdfError{L"转换结果超过当前 Word 文件处理上限。", {}};
    }
    xml += "</w:body></w:document>";
    relationships += "</Relationships>";
    if (progress) progress(96, L"正在写入保留原版式的 Word 文件");
    package.finish(xml, relationships);
    checkCancelled(cancel);
    std::wstring message = L"已按原版式生成 Word 文件。";
    if (boundedDpi) message += L" 大尺寸页面已按安全上限降低渲染分辨率。";
    return {true, false, std::move(message), {}, temporaryOutput};
}

bool appendRecognizedPage(std::string& xml, int page, int requestedDpi, const fs::path& tesseract,
                          const fs::path& scratch, const Cancel& cancel) {
    const auto dpi = rasterDpi(readPageSize(page, scratch, cancel), requestedDpi);
    renderPage(page, dpi, scratch, cancel, true);
    const auto image = pageImage(page);
    runEngine(tesseract,
        {image, L"recognized", L"--tessdata-dir", L"tessdata", L"-l", L"chi_sim+eng", L"--oem", L"1", L"--psm", L"3",
         L"-c", L"tessedit_create_tsv=1", L"-c", L"tessedit_create_txt=1"},
        scratch, cancel, L"无法完成扫描页文字识别。");
    const auto recognized = readText(scratch / L"recognized.txt");
    const bool hasText = hasUsableText(recognized);
    if (hasText) {
        const auto tsv = readText(scratch / L"recognized.tsv");
        if (!pdf_ocr_layout::appendOcrPage(xml, recognized, tsv, dpi)) {
            int tolerance = 0;
            const auto rows = ocrRows(parseTsv(tsv), tolerance);
            if (!appendRows(xml, rows, tolerance)) appendParagraphs(xml, recognized);
        }
    }
    std::error_code ignored;
    fs::remove(scratch / image, ignored);
    fs::remove(scratch / L"recognized.txt", ignored);
    fs::remove(scratch / L"recognized.tsv", ignored);
    return hasText;
}

} // namespace

bool hasStructuredPdfEngine() {
    const auto runtime = enginePath(L"pdf/opendoc/js.exe");
    const auto worker = enginePath(L"pdf/opendoc/convert.mjs");
    std::error_code error;
    return !runtime.empty() && fs::is_regular_file(runtime, error) && fs::is_regular_file(worker, error);
}

Result convertPdf(const Job& job, const fs::path& temporaryOutput, const fs::path& scratch,
                  const Cancel& cancel, const Progress& progress) {
    try {
        if (progress) progress(0, L"正在读取 PDF");
        stageFile(job.input, scratch / L"input.pdf", cancel, L"无法读取 PDF 源文件。");
        stageConfig(scratch, cancel);
        const int pages = countPages(runEngine(enginePath(L"pdfinfo.exe"),
            {L"-cfg", L"xpdfrc", L"input.pdf"}, scratch, cancel, L"无法打开 PDF，文件可能损坏或受密码保护。"));
        if (job.options.pdfMode == PreserveLayout) {
            return preserveLayout(job, pages, temporaryOutput, scratch, cancel, progress);
        }
        if (job.options.pdfMode != EditableText) throw PdfError{L"无效的 PDF 转换模式。", {}};

        std::string text;
        std::string layout;
        std::string geometry;
        std::vector<std::string_view> textPages;
        std::vector<std::string_view> layoutPages;
        std::vector<std::string_view> geometryPages;
        bool structuredFailed = false;
        if (!job.options.forceOcr) {
            const auto extractor = enginePath(L"pdftotext.exe");
            runEngine(extractor, {L"-cfg", L"xpdfrc", L"-enc", L"UTF-8", L"-eol", L"unix", L"input.pdf", L"text.txt"},
                      scratch, cancel, L"无法提取 PDF 文字。");
            text = readText(scratch / L"text.txt");
            textPages = splitPages(text, static_cast<size_t>(pages));
            if (hasStructuredPdfEngine() &&
                std::all_of(textPages.begin(), textPages.end(), hasUsableText)) {
                if (tryStructuredPdf(enginePath(L"pdf/opendoc/js.exe"), enginePath(L"pdf/opendoc/convert.mjs"),
                                     scratch, temporaryOutput, cancel, progress)) {
                    return {true, false, L"已按原稿布局生成可编辑 Word 文件。", {}, temporaryOutput};
                }
                structuredFailed = true;
                if (progress) progress(6, L"高保真引擎未能完成，正在使用基础排版");
            }
            if (hasUsableText(text)) {
                runEngine(extractor, {L"-cfg", L"xpdfrc", L"-enc", L"UTF-8", L"-eol", L"unix", L"-layout", L"input.pdf", L"layout.txt"},
                          scratch, cancel, L"无法分析 PDF 表格布局。");
                layout = readText(scratch / L"layout.txt");
                layoutPages = splitPages(layout, static_cast<size_t>(pages));
                try {
                    runEngine(extractor, {L"-ttk-layout", L"-cfg", L"xpdfrc", L"-enc", L"UTF-8",
                        L"input.pdf", L"geometry.tsv"}, scratch, cancel, L"无法读取 PDF 版面坐标。");
                    geometry = readText(scratch / L"geometry.tsv");
                    geometryPages = splitPages(geometry, static_cast<size_t>(pages));
                } catch (const PdfError&) {
                    // The ordinary text and table path also works with older PDF engines.
                }
            }
        }

        std::string xml =
            R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body>)";
        int textPagesCount = 0;
        int ocrPagesCount = 0;
        int unrecognizedPages = 0;
        fs::path tesseract;
        for (int index = 0; index < pages; ++index) {
            checkCancelled(cancel);
            if (index > 0) xml += "<w:p><w:r><w:br w:type=\"page\"/></w:r></w:p>";
            const int page = index + 1;
            const bool embedded = !job.options.forceOcr && hasUsableText(textPages[static_cast<size_t>(index)]);
            const auto status = embedded ? L"正在整理 PDF 文字 " : L"正在离线识别扫描页 ";
            if (progress) progress(static_cast<int>(static_cast<int64_t>(index) * 95 / pages),
                                    status + std::to_wstring(page) + L"/" + std::to_wstring(pages));
            if (embedded) {
                const auto p = static_cast<size_t>(index);
                const auto physical = layoutPages.empty() ? std::string_view{} : layoutPages[p];
                const auto coordinates = geometryPages.empty() ? std::string_view{} : geometryPages[p];
                if (!pdf_layout::appendEditablePage(xml, textPages[p], physical, coordinates) &&
                    (physical.empty() || !appendRows(xml, layoutRows(physical), 2))) {
                    appendParagraphs(xml, textPages[p]);
                }
                ++textPagesCount;
            } else {
                const int dpi = job.options.ocrDpi;
                if (tesseract.empty()) {
                    if (dpi != 200 && dpi != 300 && dpi != 400) {
                        throw PdfError{L"OCR 分辨率请选择 200、300 或 400 DPI。", {}};
                    }
                    tesseract = enginePath(L"tesseract.exe");
                    stageOcr(tesseract, scratch, cancel);
                }
                if (appendRecognizedPage(xml, page, dpi, tesseract, scratch, cancel)) ++ocrPagesCount;
                else {
                    appendParagraph(xml, {});
                    ++unrecognizedPages;
                }
            }
            if (xml.size() > maxXmlBytes) throw PdfError{L"转换结果超过当前 Word 文件处理上限。", {}};
        }
        if (textPagesCount == 0 && ocrPagesCount == 0) {
            throw PdfError{L"这份 PDF 没有可导出的文字。", {}};
        }
        xml += R"(<w:sectPr><w:pgSz w:w="11906" w:h="16838"/><w:pgMar w:top="1440" w:right="1440" w:bottom="1440" w:left="1440" w:header="720" w:footer="720" w:gutter="0"/></w:sectPr></w:body></w:document>)";
        if (progress) progress(96, L"正在写入可编辑 Word 文件");
        DocxPackage{temporaryOutput, cancel}.finish(xml);
        checkCancelled(cancel);
        std::wstring message = structuredFailed ? L"已用基础排版生成可编辑的 Word 文件。" : L"已生成可编辑的 Word 文件。";
        if (unrecognizedPages > 0) message += L" 有 " + std::to_wstring(unrecognizedPages) + L" 页未识别到文字。";
        return {true, false, std::move(message), {}, temporaryOutput};
    } catch (const PdfCancelled&) {
        return {false, true, L"已取消 PDF 转换。", {}, {}};
    } catch (const PdfError& error) {
        return {false, false, error.message, error.details, {}};
    } catch (const std::exception& error) {
        return {false, false, L"PDF 转换时出错。", fromUtf8(error.what()), {}};
    }
}

} // namespace ttk
