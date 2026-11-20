#include "pdf_layout.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <optional>
#include <tuple>
#include <vector>

namespace ttk::pdf_layout {
namespace {

struct Word {
    int size;
    int flags;
    bool spaceAfter;
    std::string text;
};

struct Line {
    int column;
    int paragraph;
    int index;
    int left;
    int top;
    int right;
    int bottom;
    std::vector<Word> words;
};

struct Page {
    int width = 0;
    int height = 0;
    std::vector<Line> lines;
};

std::string_view next(std::string_view& value, char delimiter) {
    const auto position = value.find(delimiter);
    const auto result = value.substr(0, position);
    value.remove_prefix(position == std::string_view::npos ? value.size() : position + 1);
    return result;
}

bool number(std::string_view field, int& value) {
    if (field.empty()) return false;
    const auto parsed = std::from_chars(field.data(), field.data() + field.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == field.data() + field.size();
}

int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

std::string decode(std::string_view text) {
    if (text.size() % 2 != 0) return {};
    std::string result;
    result.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        const int high = nibble(text[i]);
        const int low = nibble(text[i + 1]);
        if (high < 0 || low < 0) return {};
        result.push_back(static_cast<char>(high * 16 + low));
    }
    return result;
}

std::optional<Page> readGeometry(std::string_view text) {
    Page result;
    auto header = next(text, '\n');
    if (!header.empty() && header.back() == '\r') header.remove_suffix(1);
    if (next(header, '\t') != "P" || !number(next(header, '\t'), result.width) ||
        !number(header, result.height) || result.width < 1000 || result.height < 1000 ||
        result.width > 10000000 || result.height > 10000000) return std::nullopt;

    while (!text.empty()) {
        auto row = next(text, '\n');
        if (!row.empty() && row.back() == '\r') row.remove_suffix(1);
        if (next(row, '\t') != "W") return std::nullopt;
        std::array<int, 10> values{};
        for (auto& value : values) if (!number(next(row, '\t'), value)) return std::nullopt;
        const auto [column, paragraph, line, left, top, right, bottom, size, flags, space] = values;
        if (column < 0 || paragraph < 0 || line < 0 || left < -100000 || top < -100000 ||
            right < left || bottom < top || right > result.width + 100000 || bottom > result.height + 100000 ||
            flags < 0 || flags > 3 || size < 100 || size > 50000 || (space != 0 && space != 1)) return std::nullopt;
        auto word = decode(row);
        if (word.empty()) return std::nullopt;

        const auto identity = std::tie(column, paragraph, line);
        if (!result.lines.empty()) {
            auto& last = result.lines.back();
            const auto existing = std::tie(last.column, last.paragraph, last.index);
            if (identity < existing) return std::nullopt;
            if (identity == existing) {
                last.left = (std::min)(left, last.left);
                last.top = (std::min)(top, last.top);
                last.right = (std::max)(right, last.right);
                last.bottom = (std::max)(bottom, last.bottom);
                last.words.push_back({size, flags, space != 0, std::move(word)});
                continue;
            }
        }
        result.lines.push_back({column, paragraph, line, left, top, right, bottom,
                                {{size, flags, space != 0, std::move(word)}}});
    }
    if (result.lines.empty()) return std::nullopt;
    return result;
}

void countText(std::array<size_t, 256>& bytes, std::string_view text) {
    // Unwrapped text can suppress a hyphen at the end of the source line.
    for (unsigned char c : text) if (c > ' ' && c != '-') ++bytes[c];
}

bool coversText(const Page& page, std::string_view readingOrder) {
    std::array<size_t, 256> expected{}, actual{};
    countText(expected, readingOrder);
    for (const auto& line : page.lines) for (const auto& word : line.words) countText(actual, word.text);
    return actual == expected;
}

std::vector<int> cellStarts(std::string_view row) {
    std::vector<int> starts;
    size_t previous = 0;
    while ((previous = row.find_first_not_of(" \t\r", previous)) != std::string_view::npos) {
        const auto gap = row.find("  ", previous);
        const auto end = gap == std::string_view::npos ? row.size() : gap;
        const auto cell = row.substr(previous, end - previous);
        const auto length = std::count_if(cell.begin(), cell.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; });
        if (length > 36 || starts.size() == 10) return {};
        const auto spaces = row.substr(0, previous);
        starts.push_back(static_cast<int>(std::count_if(spaces.begin(), spaces.end(),
            [](unsigned char c) { return (c & 0xc0) != 0x80; })));
        previous = end;
    }
    return starts;
}

bool containsSimpleTable(std::string_view physicalLayout) {
    // Let the existing editable table writer handle regularly aligned cells.
    std::vector<int> previous;
    int blank = 0;
    while (!physicalLayout.empty()) {
        const auto line = next(physicalLayout, '\n');
        if (line.find_first_not_of(" \t\r") == std::string_view::npos) {
            if (++blank > 1) previous.clear();
            continue;
        }
        blank = 0;
        auto current = cellStarts(line);
        if (current.size() >= 2 && current.size() == previous.size() &&
            std::equal(current.begin(), current.end(), previous.begin(),
                [](int a, int b) { return std::abs(a - b) <= 2; })) return true;
        previous = std::move(current);
    }
    return false;
}

void escapeXml(std::string& xml, std::string_view value) {
    for (unsigned char c : value) {
        switch (c) {
        case '&': xml += "&amp;"; break;
        case '<': xml += "&lt;"; break;
        case '>': xml += "&gt;"; break;
        default:
            if (c >= 0x20) xml += static_cast<char>(c);
            break;
        }
    }
}

void writeRun(std::string& xml, int size, int flags, std::string_view text) {
    if (text.empty()) return;
    xml += "<w:r><w:rPr>";
    if (flags & 1) xml += "<w:b/>";
    if (flags & 2) xml += "<w:i/>";
    const auto points = std::to_string(std::clamp((size + 25) / 50, 4, 900));
    xml += "<w:sz w:val=\"" + points + "\"/><w:szCs w:val=\"" + points + "\"/>";
    xml += "</w:rPr><w:t xml:space=\"preserve\">";
    escapeXml(xml, text);
    xml += "</w:t></w:r>";
}

bool wordSpaceAtWrap(std::string_view previous, std::string_view nextText) {
    if (previous.empty() || nextText.empty()) return false;
    const auto left = static_cast<unsigned char>(previous.back());
    const auto right = static_cast<unsigned char>(nextText.front());
    return left != '-' && left != ' ' && right != ' ' && (left < 0x80 || right < 0x80);
}

int columnMargin(const Page& page, size_t start, int bodySize) {
    int margin = page.width;
    const int column = page.lines[start].column;
    for (size_t i = start; i < page.lines.size() && page.lines[i].column == column; ++i) {
        const auto& line = page.lines[i];
        const bool typical = std::any_of(line.words.begin(), line.words.end(),
            [bodySize](const Word& word) { return word.size <= bodySize * 12 / 10; });
        if (typical) margin = (std::min)(margin, line.left);
    }
    return margin;
}

void writeParagraph(std::string& xml, const Page& page, size_t start, size_t end,
                    int bodySize, int columnLeft, int previousBottom) {
    const auto& first = page.lines[start];
    int left = first.left, right = first.right, largest = 0;
    for (size_t i = start; i < end; ++i) {
        const auto& line = page.lines[i];
        left = (std::min)(left, line.left);
        right = (std::max)(right, line.right);
        for (const auto& word : line.words) largest = (std::max)(largest, word.size);
    }
    const bool shortBlock = end - start <= 2 && right - left < page.width * 4 / 5;
    const bool heading = shortBlock && largest >= (std::max)(bodySize + 150, bodySize * 12 / 10);
    const bool centered = shortBlock && std::abs(left + right - page.width) <= (std::max)(900, page.width / 25);
    const bool rightAligned = !centered && shortBlock && left > page.width / 2 && right > page.width * 3 / 4;
    const int indentation = std::clamp((left - columnLeft) / 5, 0, 1440);
    const int firstIndent = std::clamp((first.left - left) / 5, 0, 1000);
    const int before = previousBottom < 0 ? 0 :
        std::clamp((first.top - previousBottom - bodySize / 4) / 5, 0, 600);

    xml += "<w:p><w:pPr>";
    if (heading) xml += "<w:keepNext/>";
    xml += "<w:spacing w:before=\"" + std::to_string(before) + "\" w:after=\"0\"/>";
    if (!centered && !rightAligned && (indentation || firstIndent)) {
        xml += "<w:ind w:left=\"" + std::to_string(indentation) + "\" w:firstLine=\"" + std::to_string(firstIndent) + "\"/>";
    }
    if (centered) xml += "<w:jc w:val=\"center\"/>";
    if (rightAligned) xml += "<w:jc w:val=\"right\"/>";
    if (heading) xml += "<w:outlineLvl w:val=\"0\"/>";
    xml += "</w:pPr>";

    std::string run;
    int size = 0, flags = 0;
    for (size_t row = start; row < end; ++row) {
        const auto& words = page.lines[row].words;
        if (row != start && wordSpaceAtWrap(run, words.front().text)) run += ' ';
        for (size_t i = 0; i < words.size(); ++i) {
            const auto& word = words[i];
            if (word.size != size || word.flags != flags) {
                writeRun(xml, size, flags, run);
                run.clear();
                size = word.size;
                flags = word.flags;
            }
            run += word.text;
            if (word.spaceAfter && i + 1 < words.size()) run += ' ';
        }
    }
    writeRun(xml, size, flags, run);
    xml += "</w:p>";
}

} // namespace

bool appendEditablePage(std::string& xml, std::string_view readingOrder,
                        std::string_view physicalLayout, std::string_view geometry) {
    if (geometry.empty() || containsSimpleTable(physicalLayout)) return false;
    const auto page = readGeometry(geometry);
    if (!page || !coversText(*page, readingOrder)) return false;

    std::vector<int> fonts;
    for (const auto& line : page->lines) for (const auto& word : line.words) fonts.push_back(word.size);
    const auto middle = fonts.begin() + fonts.size() / 2;
    std::nth_element(fonts.begin(), middle, fonts.end());
    const int bodySize = *middle;

    int previousColumn = -1, previousBottom = -1, priorMargin = 0;
    for (size_t start = 0; start < page->lines.size();) {
        const auto& first = page->lines[start];
        size_t end = start + 1;
        while (end < page->lines.size()) {
            const auto& line = page->lines[end];
            const auto& before = page->lines[end - 1];
            if (line.column != first.column || line.paragraph != first.paragraph ||
                line.top - before.bottom > (std::max)(1400, bodySize * 4 / 3)) break;
            ++end;
        }
        if (previousColumn != first.column) {
            priorMargin = columnMargin(*page, start, bodySize);
            previousBottom = -1;
            previousColumn = first.column;
        }
        writeParagraph(xml, *page, start, end, bodySize, priorMargin, previousBottom);
        previousBottom = page->lines[end - 1].bottom;
        start = end;
    }
    return true;
}

} // namespace ttk::pdf_layout
