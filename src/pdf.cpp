#include "ttk.hpp"

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

uint32_t crc32(std::string_view bytes) {
    static const auto table = [] {
        std::array<uint32_t, 256> entries{};
        for (uint32_t value = 0; value < entries.size(); ++value) {
            uint32_t crc = value;
            for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320U : 0);
            entries[value] = crc;
        }
        return entries;
    }();
    uint32_t crc = 0xFFFFFFFFU;
    for (unsigned char byte : bytes) crc = (crc >> 8) ^ table[(crc ^ byte) & 0xFF];
    return crc ^ 0xFFFFFFFFU;
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
    std::string_view name;
    std::string_view bytes;
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
    write32(output, static_cast<uint32_t>(entry.bytes.size()));
    write32(output, static_cast<uint32_t>(entry.bytes.size()));
    write16(output, static_cast<uint16_t>(entry.name.size()));
    write16(output, 0); // Extra field length.
}

void writeDocx(const fs::path& outputPath, std::string_view document, const Cancel& cancel) {
    constexpr std::string_view types =
        R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/></Types>)";
    constexpr std::string_view relationships =
        R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>)";
    std::array<ZipEntry, 3> entries{{
        {"[Content_Types].xml", types}, {"_rels/.rels", relationships}, {"word/document.xml", document}
    }};
    std::ofstream stream(outputPath, std::ios::binary | std::ios::trunc);
    if (!stream) throw PdfError{L"无法创建 Word 文件。", outputPath.wstring()};
    for (auto& entry : entries) {
        checkCancelled(cancel);
        entry.offset = static_cast<uint32_t>(stream.tellp());
        entry.crc = crc32(entry.bytes);
        write32(stream, 0x04034B50);
        writeZipMetadata(stream, entry);
        stream.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
        stream.write(entry.bytes.data(), static_cast<std::streamsize>(entry.bytes.size()));
    }
    const auto directory = static_cast<uint32_t>(stream.tellp());
    for (const auto& entry : entries) {
        write32(stream, 0x02014B50);
        write16(stream, 20); // DOS creator, ZIP version 2.0.
        writeZipMetadata(stream, entry);
        write16(stream, 0); // Comment length.
        write16(stream, 0); // Disk number.
        write16(stream, 0); // Internal attributes.
        write32(stream, 0); // External attributes.
        write32(stream, entry.offset);
        stream.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
    }
    const auto directorySize = static_cast<uint32_t>(stream.tellp()) - directory;
    write32(stream, 0x06054B50);
    write16(stream, 0);
    write16(stream, 0);
    write16(stream, static_cast<uint16_t>(entries.size()));
    write16(stream, static_cast<uint16_t>(entries.size()));
    write32(stream, directorySize);
    write32(stream, directory);
    write16(stream, 0);
    stream.close();
    if (!stream) throw PdfError{L"写入 Word 文件时出错。", outputPath.wstring()};
}

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

int rasterDpi(std::string_view info, int requestedDpi) {
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
        const double bounded = (std::min)({static_cast<double>(requestedDpi),
                                           72 * std::sqrt(maxRasterPixels / (width * height)),
                                           72 * 12000 / width, 72 * 12000 / height});
        if (bounded >= 30.0) return static_cast<int>(bounded);
        throw PdfError{L"扫描页尺寸超出 OCR 处理范围。", {}};
    }
    throw PdfError{L"无法读取扫描页尺寸。", {}};
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

bool appendRecognizedPage(std::string& xml, int page, int requestedDpi, const fs::path& tesseract,
                          const fs::path& scratch, const Cancel& cancel) {
    const auto pageNumber = std::to_wstring(page);
    const auto info = runEngine(enginePath(L"pdfinfo.exe"),
        {L"-cfg", L"xpdfrc", L"-f", pageNumber, L"-l", pageNumber, L"input.pdf"},
        scratch, cancel, L"无法读取 PDF 页面。");
    const auto dpi = std::to_wstring(rasterDpi(info, requestedDpi));
    runEngine(enginePath(L"pdftopng.exe"),
        {L"-cfg", L"xpdfrc", L"-f", pageNumber, L"-l", pageNumber, L"-r", dpi, L"-gray", L"input.pdf", L"page"},
        scratch, cancel, L"无法渲染扫描页。");
    const auto image = pageImage(page);
    runEngine(tesseract,
        {image, L"recognized", L"--tessdata-dir", L"tessdata", L"-l", L"chi_sim+eng", L"--oem", L"1", L"--psm", L"3",
         L"-c", L"tessedit_create_tsv=1", L"-c", L"tessedit_create_txt=1"},
        scratch, cancel, L"无法完成扫描页文字识别。");
    const auto recognized = readText(scratch / L"recognized.txt");
    int tolerance = 0;
    const auto rows = ocrRows(parseTsv(readText(scratch / L"recognized.tsv")), tolerance);
    const bool hasText = hasUsableText(recognized);
    if (hasText && !appendRows(xml, rows, tolerance)) appendParagraphs(xml, recognized);
    std::error_code ignored;
    fs::remove(scratch / image, ignored);
    fs::remove(scratch / L"recognized.txt", ignored);
    fs::remove(scratch / L"recognized.tsv", ignored);
    return hasText;
}

} // namespace

Result convertPdf(const Job& job, const fs::path& temporaryOutput, const fs::path& scratch,
                  const Cancel& cancel, const Progress& progress) {
    try {
        if (progress) progress(0, L"正在读取 PDF");
        stageFile(job.input, scratch / L"input.pdf", cancel, L"无法读取 PDF 源文件。");
        stageConfig(scratch, cancel);
        const int pages = countPages(runEngine(enginePath(L"pdfinfo.exe"),
            {L"-cfg", L"xpdfrc", L"input.pdf"}, scratch, cancel, L"无法打开 PDF，文件可能损坏或受密码保护。"));

        std::string text;
        std::string layout;
        std::vector<std::string_view> textPages;
        std::vector<std::string_view> layoutPages;
        if (!job.options.forceOcr) {
            const auto extractor = enginePath(L"pdftotext.exe");
            runEngine(extractor, {L"-cfg", L"xpdfrc", L"-enc", L"UTF-8", L"-eol", L"unix", L"input.pdf", L"text.txt"},
                      scratch, cancel, L"无法提取 PDF 文字。");
            text = readText(scratch / L"text.txt");
            textPages = splitPages(text, static_cast<size_t>(pages));
            if (hasUsableText(text)) {
                runEngine(extractor, {L"-cfg", L"xpdfrc", L"-enc", L"UTF-8", L"-eol", L"unix", L"-layout", L"input.pdf", L"layout.txt"},
                          scratch, cancel, L"无法分析 PDF 表格布局。");
                layout = readText(scratch / L"layout.txt");
                layoutPages = splitPages(layout, static_cast<size_t>(pages));
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
                if (layoutPages.empty() || !appendRows(xml, layoutRows(layoutPages[p]), 2)) {
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
        writeDocx(temporaryOutput, xml, cancel);
        checkCancelled(cancel);
        std::wstring message = L"已生成可编辑的 Word 文件。";
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
