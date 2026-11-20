#include "pdf_ocr_layout.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <optional>
#include <vector>

namespace ttk::pdf_ocr_layout {
namespace {

struct Word {
    int left;
    int top;
    int right;
    int bottom;
    std::string_view text;
};

struct Line {
    std::array<int, 3> id;
    int left;
    int top;
    int right;
    int bottom;
    int height = 0;
    std::vector<Word> words;
};

struct Page {
    int width = 0;
    std::vector<Line> lines;
};

struct Cell {
    int left;
    int right;
    std::string text;
};

struct Row {
    size_t first, last, count;
    int y;
    std::vector<Cell> cells;
};

std::string_view take(std::string_view& text, char delimiter) {
    const auto split = text.find(delimiter);
    const auto field = text.substr(0, split);
    text.remove_prefix(split == std::string_view::npos ? field.size() : split + 1);
    return field;
}

bool readInt(std::string_view text, int& value) {
    if (text.empty()) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

int median(std::vector<int> data) {
    auto mid = data.begin() + data.size() / 2;
    std::nth_element(data.begin(), mid, data.end());
    return *mid;
}

std::optional<Page> parsePage(std::string_view text) {
    Page page;
    take(text, '\n');
    while (!text.empty()) {
        auto row = take(text, '\n');
        if (!row.empty() && row.back() == '\r') row.remove_suffix(1);
        const auto level = take(row, '\t');
        if (level != "1" && level != "5") continue;
        std::array<int, 9> fields{};
        for (auto& field : fields) {
            if (!readInt(take(row, '\t'), field) || field < 0 || field > 100000) return std::nullopt;
        }
        const auto [pageNumber, block, para, lineIndex, wordIndex, left, top, width, height] = fields;
        if (pageNumber != 1) return std::nullopt;
        if (level == "1") {
            page.width = width;
            continue;
        }
        take(row, '\t'); // Confidence carries no layout information.
        const auto first = row.find_first_not_of(" \t\r");
        if (first == row.npos) continue; // Tesseract sometimes emits table rules as a space.
        row = row.substr(first, row.find_last_not_of(" \t\r") - first + 1);
        if (width == 0 || height == 0 || left + width > 100000 || top + height > 100000 ||
            block == 0 || para == 0 || lineIndex == 0 || wordIndex == 0) return std::nullopt;
        const std::array<int, 3> id{block, para, lineIndex};
        if (!page.lines.empty() && id < page.lines.back().id) return std::nullopt;
        if (page.lines.empty() || id != page.lines.back().id)
            page.lines.push_back({id, left, top, left + width, top + height, 0, {}});
        auto& line = page.lines.back();
        line.left = (std::min)(line.left, left);
        line.top = (std::min)(line.top, top);
        line.right = (std::max)(line.right, left + width);
        line.bottom = (std::max)(line.bottom, top + height);
        line.words.push_back({left, top, left + width, top + height, row});
    }
    if (page.width <= 0 || page.lines.empty()) return std::nullopt;
    for (auto& line : page.lines) {
        std::vector<int> heights;
        heights.reserve(line.words.size());
        for (const auto& word : line.words) heights.push_back(word.bottom - word.top);
        line.height = median(std::move(heights));
    }
    return page;
}

void countBytes(std::array<size_t, 256>& counts, std::string_view text) {
    for (unsigned char c : text) if (c > ' ') ++counts[c];
}

bool complete(const Page& page, std::string_view recognized) {
    std::array<size_t, 256> expected{}, actual{};
    if (recognized.substr(0, 3) == "\xef\xbb\xbf") recognized.remove_prefix(3);
    countBytes(expected, recognized);
    for (const auto& line : page.lines) for (const auto& word : line.words) countBytes(actual, word.text);
    return actual == expected;
}

unsigned utf8(std::string_view text, size_t start) {
    const auto first = static_cast<unsigned char>(text[start]);
    if (first < 0xc0) return first;
    const int extra = first < 0xe0 ? 1 : first < 0xf0 ? 2 : 3;
    if (start + extra >= text.size()) return first;
    unsigned value = first & (0x7f >> (extra + 1));
    for (int i = 1; i <= extra; ++i)
        value = (value << 6) | (static_cast<unsigned char>(text[start + i]) & 0x3f);
    return value;
}

bool ideographic(unsigned cp) {
    return (cp >= 0x2e80 && cp <= 0x9fff) || (cp >= 0xf900 && cp <= 0xfaff) ||
           (cp >= 0x20000 && cp <= 0x323af);
}

void appendWord(std::string& text, std::string_view next) {
    if (!text.empty()) {
        size_t last = text.size() - 1;
        while (last > 0 && (static_cast<unsigned char>(text[last]) & 0xc0) == 0x80) --last;
        const auto a = utf8(text, last), b = utf8(next, 0);
        constexpr std::u32string_view closing = U",.;:!?%)]}、。，；：！？）》」』】％";
        constexpr std::u32string_view opening = U"([{（《「『【";
        if (!(ideographic(b) && (ideographic(a) || closing.find(a) != closing.npos)) &&
            closing.find(b) == closing.npos &&
            opening.find(a) == opening.npos && !(b >= 0xff00 && b <= 0xff65) &&
            !(a >= 0xff00 && a <= 0xff65)) text += ' ';
    }
    text += next;
}

void appendTextRun(std::string& xml, std::string_view text, int halfPoints) {
    const auto size = std::to_string(halfPoints);
    xml += "<w:r><w:rPr><w:sz w:val=\"" + size + "\"/><w:szCs w:val=\"" + size +
           "\"/></w:rPr><w:t xml:space=\"preserve\">";
    for (unsigned char c : text) {
        if (c == '&') xml += "&amp;";
        else if (c == '<') xml += "&lt;";
        else if (c == '>') xml += "&gt;";
        else if (c >= ' ') xml += static_cast<char>(c);
    }
    xml += "</w:t></w:r>";
}

size_t letterCount(std::string_view text) {
    return static_cast<size_t>(std::count_if(text.begin(), text.end(), [](unsigned char c) {
        return c > ' ' && (c & 0xc0) != 0x80;
    }));
}

int position(const Line& line) {
    return line.top + (line.bottom - line.top) / 2;
}

std::vector<Row> findRows(const Page& page, int bodyHeight) {
    const auto& lines = page.lines;
    std::vector<size_t> indices(lines.size());
    for (size_t i = 0; i < indices.size(); ++i) indices[i] = i;
    std::stable_sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
        return position(lines[a]) < position(lines[b]);
    });
    std::vector<Row> rows;
    for (size_t start = 0; start < indices.size();) {
        const int y = position(lines[indices[start]]);
        Row row{indices[start], indices[start], 0, y, {}};
        std::vector<Word> words;
        do {
            const auto i = indices[start++];
            row.first = (std::min)(row.first, i);
            row.last = (std::max)(row.last, i);
            ++row.count;
            words.insert(words.end(), lines[i].words.begin(), lines[i].words.end());
        } while (start < indices.size() && position(lines[indices[start]]) - y <= (std::max)(2, bodyHeight / 3));
        std::stable_sort(words.begin(), words.end(), [](const Word& a, const Word& b) { return a.left < b.left; });
        const int gap = (std::max)(bodyHeight * 2, page.width / 32);
        for (const auto& word : words) {
            if (row.cells.empty() || word.left - row.cells.back().right >= gap)
                row.cells.push_back({word.left, word.right, {}});
            auto& cell = row.cells.back();
            cell.right = (std::max)(cell.right, word.right);
            appendWord(cell.text, word.text);
        }
        if (row.cells.size() < 2 || row.cells.size() > 8 ||
            std::any_of(row.cells.begin(), row.cells.end(), [&](const Cell& cell) {
                return letterCount(cell.text) > 24 || cell.right - cell.left > bodyHeight * 8;
            })) row.cells.clear();
        rows.push_back(std::move(row));
    }
    return rows;
}

bool sameColumns(const Row& a, const Row& b, int tolerance) {
    if (a.cells.size() != b.cells.size()) return false;
    for (size_t i = 0; i < a.cells.size(); ++i)
        if (std::abs(a.cells[i].left - b.cells[i].left) > tolerance) return false;
    return true;
}

struct Table {
    size_t firstLine, lastLine, firstRow, endRow;
};

std::vector<Table> findTables(const std::vector<Row>& rows, int height) {
    std::vector<Table> result;
    for (size_t start = 0; start < rows.size();) {
        const auto& anchor = rows[start];
        if (anchor.cells.empty()) {
            ++start;
            continue;
        }
        size_t end = start + 1;
        Table table{anchor.first, anchor.last, start, end};
        size_t lines = anchor.count;
        while (end < rows.size() && rows[end].y - rows[end - 1].y <= height * 4 &&
               sameColumns(anchor, rows[end], (std::max)(3, height * 3 / 4))) {
            table.firstLine = (std::min)(table.firstLine, rows[end].first);
            table.lastLine = (std::max)(table.lastLine, rows[end].last);
            lines += rows[end].count;
            ++end;
        }
        const bool pair = anchor.cells.size() == 2;
        // Separate OCR blocks in two parallel columns usually contain prose.
        const bool tabular = !pair || std::any_of(rows.begin() + start, rows.begin() + end, [](const Row& row) {
            const auto& label = row.cells.front().text;
            return row.count == 1 || label.back() == ':' ||
                   (label.size() >= 3 && label.compare(label.size() - 3, 3, "：") == 0);
        });
        if (tabular && end - start >= (pair ? 3 : 2) && table.lastLine - table.firstLine + 1 == lines) {
            table.endRow = end;
            result.push_back(table);
        }
        start = end;
    }
    std::sort(result.begin(), result.end(), [](const Table& a, const Table& b) {
        return a.firstLine < b.firstLine;
    });
    return result;
}

void appendTable(std::string& xml, const std::vector<Row>& rows, const Table& table, int fontSize) {
    const auto columns = rows[table.firstRow].cells.size();
    const auto width = std::to_string(9000 / columns);
    xml += "<w:tbl><w:tblPr><w:tblW w:w=\"0\" w:type=\"auto\"/><w:tblBorders>";
    for (const char* border : {"top", "left", "bottom", "right", "insideH", "insideV"})
        xml += "<w:" + std::string(border) + " w:val=\"single\" w:sz=\"4\" w:color=\"B5BDC7\"/>";
    xml += "</w:tblBorders></w:tblPr><w:tblGrid>";
    for (size_t i = 0; i < columns; ++i) xml += "<w:gridCol w:w=\"" + width + "\"/>";
    xml += "</w:tblGrid>";
    for (size_t i = table.firstRow; i < table.endRow; ++i) {
        xml += "<w:tr>";
        for (const auto& cell : rows[i].cells) {
            xml += "<w:tc><w:tcPr><w:tcW w:w=\"" + width + "\" w:type=\"dxa\"/></w:tcPr><w:p>";
            appendTextRun(xml, cell.text, fontSize);
            xml += "</w:p></w:tc>";
        }
        xml += "</w:tr>";
    }
    xml += "</w:tbl>";
}

int fontSize(int height, int dpi) {
    // Glyph boxes are shorter than their font's em square.
    return std::clamp((height * 168 + dpi / 2) / dpi, 14, 80);
}

bool centered(const Line& line, int pageWidth) {
    return std::abs(line.left + line.right - pageWidth) <= pageWidth / 14 &&
           line.right - line.left <= pageWidth * 3 / 4;
}

void appendParagraph(std::string& xml, const Page& page, size_t first, size_t end,
                     int dpi, int bodyHeight, int leftMargin) {
    const auto& line = page.lines[first];
    const auto alignCenter = centered(line, page.width) && end - first == 1;
    const int gap = first == 0 ? 0 : line.top - page.lines[first - 1].bottom;
    const int before = std::clamp(((std::max)(gap, 0) - bodyHeight / 3) * 1440 / dpi, 0, 320);
    const int inset = std::clamp((line.left - leftMargin) * 1440 / dpi, 0, 1080);
    const bool heading = end - first == 1 && line.height >= bodyHeight * 4 / 3 &&
                         line.right - line.left < page.width * 3 / 4;
    xml += "<w:p><w:pPr>";
    if (heading) xml += "<w:keepNext/>";
    xml += "<w:spacing w:before=\"" + std::to_string(before) + "\" w:after=\"0\"/>";
    if (alignCenter) xml += "<w:jc w:val=\"center\"/>";
    else if (inset >= 60) xml += "<w:ind w:left=\"" + std::to_string(inset) + "\"/>";
    if (heading) xml += "<w:outlineLvl w:val=\"0\"/>";
    xml += "</w:pPr>";
    std::string text;
    for (size_t i = first; i < end; ++i)
        for (const auto& word : page.lines[i].words) appendWord(text, word.text);
    appendTextRun(xml, text, fontSize(heading ? line.height : bodyHeight, dpi));
    xml += "</w:p>";
}

} // namespace

bool appendOcrPage(std::string& xml, std::string_view recognizedText, std::string_view tsv, int rasterDpi) {
    if (rasterDpi < 20 || rasterDpi > 1200) return false;
    auto page = parsePage(tsv);
    if (!page || !complete(*page, recognizedText)) return false;
    const auto& lines = page->lines;
    std::vector<int> heights;
    heights.reserve(lines.size());
    for (const auto& line : lines) heights.push_back(line.height);
    const int height = median(std::move(heights));
    const auto rows = findRows(*page, height);
    const auto tables = findTables(rows, height);
    auto table = tables.begin();
    int bodyMargin = page->width;
    for (const auto& line : lines)
        if (!centered(line, page->width)) bodyMargin = (std::min)(bodyMargin, line.left);
    for (size_t block = 0; block < lines.size();) {
        size_t blockEnd = block + 1;
        int margin = lines[block].left;
        while (blockEnd < lines.size() && lines[blockEnd].id[0] == lines[block].id[0]) {
            margin = (std::min)(margin, lines[blockEnd].left);
            ++blockEnd;
        }
        if (margin < page->width / 2) margin = (std::min)(margin, bodyMargin);
        for (; block < blockEnd;) {
            if (table != tables.end() && block == table->firstLine) {
                appendTable(xml, rows, *table, fontSize(height, rasterDpi));
                block = table++->lastLine + 1;
                continue;
            }
            size_t end = block + 1;
            while (end < blockEnd && (table == tables.end() || end < table->firstLine) &&
                   lines[end].id[1] == lines[block].id[1] &&
                   lines[end].top - lines[end - 1].bottom <= height &&
                   std::abs(lines[end].height - lines[block].height) <= height / 3) ++end;
            appendParagraph(xml, *page, block, end, rasterDpi, height, margin);
            block = end;
        }
    }
    return true;
}

} // namespace ttk::pdf_ocr_layout
