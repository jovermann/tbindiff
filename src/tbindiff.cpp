// tbindiff - Show differences between two binary files
//
// Copyright (c) 2026 Johannes Overmann
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE or copy at https://www.boost.org/LICENSE_1_0.txt)

#include "CommandLineParser.hpp"
#include "Tui.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;

enum class EditKind { Match, Substitute, Delete, Insert };

struct Edit
{
    EditKind kind{};
    uint8_t left{};
    uint8_t right{};
};

using Bytes = std::vector<uint8_t>;
using Cost = uint32_t;

static constexpr uint64_t maxDpCells = 30000000;

class Progress
{
public:
    Progress(bool enabled_, size_t width_): enabled(enabled_), width(width_) {}

    void phase(const std::string& text)
    {
        if (!enabled) return;
        clear();
        current = text;
        lastPercent = 101;
        print();
    }

    void update(size_t done, size_t total)
    {
        if (!enabled) return;
        const size_t percent = total ? (100 * done / total) : 100;
        if (percent == lastPercent) return;
        clear();
        lastPercent = percent;
        current = "Diffing: " + std::to_string(percent) + "%";
        print();
    }

    void finish()
    {
        if (!enabled) return;
        clear();
        std::cerr << "Diffing: done\n";
        lineLength = 0;
    }

private:
    void clear()
    {
        if (lineLength) std::cerr << '\r' << std::string(lineLength, ' ') << '\r';
    }

    void print()
    {
        const std::string line = ut1::tui::fitTerminalLine(current, width);
        std::cerr << line << std::flush;
        lineLength = line.size();
    }

    bool enabled{};
    size_t width{};
    size_t lineLength{};
    size_t lastPercent{101};
    std::string current;
};

static Bytes readFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        throw std::runtime_error("Cannot open '" + path.string() + "'.");
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0)
    {
        throw std::runtime_error("Cannot determine size of '" + path.string() + "'.");
    }
    in.seekg(0);
    Bytes data(static_cast<size_t>(size));
    if (!data.empty() && !in.read(reinterpret_cast<char*>(data.data()), size))
    {
        throw std::runtime_error("Cannot read '" + path.string() + "'.");
    }
    return data;
}

static bool looksAscii(const Bytes& data)
{
    size_t nonAscii = 0;
    for (uint8_t value: data) nonAscii += value >= 0x80;
    return data.empty() || static_cast<long double>(nonAscii) * 10.0L < static_cast<long double>(data.size());
}

static std::vector<Cost> lastCostRow(const uint8_t* a, size_t na, const uint8_t* b, size_t nb, bool reverse)
{
    std::vector<Cost> row(nb + 1);
    for (size_t j = 0; j <= nb; ++j)
    {
        row[j] = static_cast<Cost>(j);
    }
    for (size_t i = 1; i <= na; ++i)
    {
        Cost diagonal = row[0];
        row[0] = static_cast<Cost>(i);
        const uint8_t av = reverse ? a[na - i] : a[i - 1];
        for (size_t j = 1; j <= nb; ++j)
        {
            const Cost above = row[j];
            const uint8_t bv = reverse ? b[nb - j] : b[j - 1];
            row[j] = std::min({static_cast<Cost>(above + 1), static_cast<Cost>(row[j - 1] + 1),
                               static_cast<Cost>(diagonal + (av != bv))});
            diagonal = above;
        }
    }
    return row;
}

static void smallDiff(const uint8_t* a, size_t na, const uint8_t* b, size_t nb, std::vector<Edit>& out)
{
    if (na == 0)
    {
        for (size_t j = 0; j < nb; ++j) out.push_back({EditKind::Insert, 0, b[j]});
        return;
    }
    if (nb == 0)
    {
        for (size_t i = 0; i < na; ++i) out.push_back({EditKind::Delete, a[i], 0});
        return;
    }
    std::vector<Cost> matrix((na + 1) * (nb + 1));
    for (size_t i = 0; i <= na; ++i) matrix[i * (nb + 1)] = static_cast<Cost>(i);
    for (size_t j = 0; j <= nb; ++j) matrix[j] = static_cast<Cost>(j);
    for (size_t i = 1; i <= na; ++i)
        for (size_t j = 1; j <= nb; ++j)
            matrix[i * (nb + 1) + j] = std::min({static_cast<Cost>(matrix[(i - 1) * (nb + 1) + j] + 1),
                static_cast<Cost>(matrix[i * (nb + 1) + j - 1] + 1),
                static_cast<Cost>(matrix[(i - 1) * (nb + 1) + j - 1] + (a[i - 1] != b[j - 1]))});
    std::vector<Edit> reversed;
    size_t i = na, j = nb;
    while (i || j)
    {
        if (i && j && matrix[i * (nb + 1) + j] == matrix[(i - 1) * (nb + 1) + j - 1] + (a[i - 1] != b[j - 1]))
        {
            reversed.push_back({a[i - 1] == b[j - 1] ? EditKind::Match : EditKind::Substitute, a[i - 1], b[j - 1]});
            --i; --j;
        }
        else if (i && matrix[i * (nb + 1) + j] == matrix[(i - 1) * (nb + 1) + j] + 1)
        {
            reversed.push_back({EditKind::Delete, a[--i], 0});
        }
        else
        {
            reversed.push_back({EditKind::Insert, 0, b[--j]});
        }
    }
    out.insert(out.end(), reversed.rbegin(), reversed.rend());
}

static void hirschberg(const uint8_t* a, size_t na, const uint8_t* b, size_t nb, std::vector<Edit>& out)
{
    if (na <= 1 || nb <= 1)
    {
        smallDiff(a, na, b, nb, out);
        return;
    }
    const size_t amid = na / 2;
    auto forward = lastCostRow(a, amid, b, nb, false);
    auto backward = lastCostRow(a + amid, na - amid, b, nb, true);
    size_t split = 0;
    Cost best = std::numeric_limits<Cost>::max();
    for (size_t j = 0; j <= nb; ++j)
    {
        const Cost cost = forward[j] + backward[nb - j];
        if (cost < best)
        {
            best = cost;
            split = j;
        }
    }
    forward.clear(); forward.shrink_to_fit();
    backward.clear(); backward.shrink_to_fit();
    hirschberg(a, amid, b, split, out);
    hirschberg(a + amid, na - amid, b + split, nb - split, out);
}

static void exactDiff(const uint8_t* left, size_t leftSize, const uint8_t* right, size_t rightSize,
                      std::vector<Edit>& result)
{
    if (rightSize <= leftSize)
    {
        hirschberg(left, leftSize, right, rightSize, result);
    }
    else
    {
        std::vector<Edit> swapped;
        swapped.reserve(rightSize);
        hirschberg(right, rightSize, left, leftSize, swapped);
        for (const Edit& e: swapped)
        {
            EditKind kind = e.kind;
            if (kind == EditKind::Insert) kind = EditKind::Delete;
            else if (kind == EditKind::Delete) kind = EditKind::Insert;
            result.push_back({kind, e.right, e.left});
        }
    }
}

struct AnchorInfo
{
    size_t left{};
    size_t right{};
    unsigned leftCount{};
    unsigned rightCount{};
};

struct Anchor
{
    size_t left{};
    size_t right{};
};

static std::vector<Anchor> findAnchors(const Bytes& left, const Bytes& right, size_t anchorSize)
{
    std::unordered_map<uint64_t, AnchorInfo> table;
    table.reserve((left.size() + right.size()) / 24 + 1);
    auto scan = [&](const Bytes& data, bool isLeft) {
        if (data.size() < anchorSize) return;
        // A rolling polynomial hash keeps configurable large anchors linear-time.
        constexpr uint64_t base = 0x100000001b3ULL;
        uint64_t power = 1;
        for (size_t i = 1; i < anchorSize; ++i) power *= base;
        uint64_t hash = 0;
        for (size_t i = 0; i < anchorSize; ++i) hash = hash * base + data[i] + 1U;
        for (size_t pos = 0; pos + anchorSize <= data.size(); ++pos)
        {
            // Content-defined sampling survives insertions because it depends on bytes, not offsets.
            if ((hash & 31U) == 0)
            {
                AnchorInfo& info = table[hash];
                if (isLeft) { info.left = pos; ++info.leftCount; }
                else { info.right = pos; ++info.rightCount; }
            }
            if (pos + anchorSize < data.size())
                hash = (hash - static_cast<uint64_t>(data[pos] + 1U) * power) * base + data[pos + anchorSize] + 1U;
        }
    };
    scan(left, true);
    scan(right, false);

    std::vector<Anchor> candidates;
    for (const auto& [hash, info]: table)
    {
        (void)hash;
        if (info.leftCount == 1 && info.rightCount == 1 &&
            std::equal(left.begin() + static_cast<ptrdiff_t>(info.left),
                       left.begin() + static_cast<ptrdiff_t>(info.left + anchorSize),
                       right.begin() + static_cast<ptrdiff_t>(info.right)))
            candidates.push_back({info.left, info.right});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Anchor& a, const Anchor& b) {
        return a.left != b.left ? a.left < b.left : a.right < b.right;
    });

    // Longest increasing subsequence selects a non-crossing chain of matching anchors.
    std::vector<size_t> tails;
    std::vector<size_t> previous(candidates.size(), candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        auto it = std::lower_bound(tails.begin(), tails.end(), candidates[i].right,
            [&](size_t index, size_t value) { return candidates[index].right < value; });
        const size_t level = static_cast<size_t>(it - tails.begin());
        if (level) previous[i] = tails[level - 1];
        if (it == tails.end()) tails.push_back(i); else *it = i;
    }
    std::vector<Anchor> chain;
    if (!tails.empty())
    {
        size_t i = tails.back();
        while (i != candidates.size()) { chain.push_back(candidates[i]); i = previous[i]; }
        std::reverse(chain.begin(), chain.end());
    }
    std::vector<Anchor> nonOverlapping;
    for (const Anchor& anchor: chain)
        if (nonOverlapping.empty() || (anchor.left >= nonOverlapping.back().left + anchorSize &&
                                       anchor.right >= nonOverlapping.back().right + anchorSize))
            nonOverlapping.push_back(anchor);
    return nonOverlapping;
}

static void fastGapDiff(const uint8_t* left, size_t leftSize, const uint8_t* right, size_t rightSize,
                        bool fineDiff, uint64_t maxMemory, uint64_t& cellsLeft, std::vector<Edit>& result)
{
    if (!fineDiff)
    {
        const size_t paired = std::min(leftSize, rightSize);
        for (size_t i = 0; i < paired; ++i) result.push_back({EditKind::Substitute, left[i], right[i]});
        for (size_t i = paired; i < leftSize; ++i) result.push_back({EditKind::Delete, left[i], 0});
        for (size_t i = paired; i < rightSize; ++i) result.push_back({EditKind::Insert, 0, right[i]});
        return;
    }
    size_t prefix = 0;
    while (prefix < leftSize && prefix < rightSize && left[prefix] == right[prefix]) ++prefix;
    for (size_t i = 0; i < prefix; ++i) result.push_back({EditKind::Match, left[i], right[i]});
    if (prefix)
    {
        left += prefix;
        right += prefix;
        leftSize -= prefix;
        rightSize -= prefix;
    }
    size_t suffix = 0;
    while (suffix < leftSize && suffix < rightSize && left[leftSize - suffix - 1] == right[rightSize - suffix - 1]) ++suffix;
    const size_t shorter = std::min(leftSize - suffix, rightSize - suffix);
    const uint64_t rowMemory = 2ULL * (shorter + 1) * sizeof(Cost);
    const size_t aMiddle = leftSize - suffix;
    const size_t bMiddle = rightSize - suffix;
    const bool cellsFit = bMiddle == 0 || aMiddle <= cellsLeft / bMiddle;
    if (rowMemory <= maxMemory && cellsFit)
    {
        exactDiff(left, aMiddle, right, bMiddle, result);
        cellsLeft -= static_cast<uint64_t>(aMiddle) * bMiddle;
    }
    else
    {
        const size_t paired = std::min(aMiddle, bMiddle);
        for (size_t i = 0; i < paired; ++i)
            result.push_back({left[i] == right[i] ? EditKind::Match : EditKind::Substitute, left[i], right[i]});
        for (size_t i = paired; i < aMiddle; ++i) result.push_back({EditKind::Delete, left[i], 0});
        for (size_t i = paired; i < bMiddle; ++i) result.push_back({EditKind::Insert, 0, right[i]});
    }
    for (size_t i = suffix; i > 0; --i)
        result.push_back({EditKind::Match, left[leftSize - i], right[rightSize - i]});
}

static std::vector<Edit> makeDiff(const Bytes& left, const Bytes& right, size_t anchorSize, bool fineDiff,
                                  uint64_t maxMemory, Progress& progress)
{
    if (left == right)
    {
        std::vector<Edit> result;
        result.reserve(left.size());
        for (uint8_t value: left) result.push_back({EditKind::Match, value, value});
        return result;
    }
    progress.phase("Finding matching binary sequences...");
    const std::vector<Anchor> anchors = findAnchors(left, right, anchorSize);
    progress.phase("Diffing regions...");
    std::vector<Edit> result;
    result.reserve(std::max(left.size(), right.size()));
    size_t leftPos = 0, rightPos = 0;
    uint64_t cellsLeft = maxDpCells;
    auto at = [](const Bytes& bytes, size_t pos) { return bytes.empty() ? nullptr : bytes.data() + pos; };
    for (const Anchor& anchor: anchors)
    {
        if (anchor.left < leftPos || anchor.right < rightPos) continue;
        size_t matchLeft = anchor.left;
        size_t matchRight = anchor.right;
        while (matchLeft > leftPos && matchRight > rightPos && left[matchLeft - 1] == right[matchRight - 1])
        {
            --matchLeft;
            --matchRight;
        }
        size_t matchLength = anchorSize + anchor.left - matchLeft;
        while (matchLeft + matchLength < left.size() && matchRight + matchLength < right.size() &&
               left[matchLeft + matchLength] == right[matchRight + matchLength])
            ++matchLength;
        fastGapDiff(at(left, leftPos), matchLeft - leftPos, at(right, rightPos),
                    matchRight - rightPos, fineDiff, maxMemory, cellsLeft, result);
        for (size_t i = 0; i < matchLength; ++i)
            result.push_back({EditKind::Match, left[matchLeft + i], right[matchRight + i]});
        leftPos = matchLeft + matchLength;
        rightPos = matchRight + matchLength;
        progress.update(leftPos + rightPos, left.size() + right.size());
    }
    fastGapDiff(at(left, leftPos), left.size() - leftPos, at(right, rightPos),
                right.size() - rightPos, fineDiff, maxMemory, cellsLeft, result);
    progress.finish();
    return result;
}

static std::string hexOffset(size_t offset, size_t digits)
{
    std::ostringstream os;
    os << std::hex << std::setfill('0') << std::setw(static_cast<int>(digits)) << offset;
    return os.str();
}

static char printable(uint8_t value)
{
    return std::isprint(static_cast<unsigned char>(value)) ? static_cast<char>(value) : '.';
}

static std::string renderHex(const std::vector<Edit>& edits, size_t begin, size_t end, bool leftSide, size_t count)
{
    std::ostringstream os;
    std::string ascii;
    size_t emitted = 0;
    for (size_t i = begin; i < end; ++i)
    {
        const Edit& e = edits[i];
        const bool present = leftSide ? e.kind != EditKind::Insert : e.kind != EditKind::Delete;
        if (present)
        {
            const uint8_t value = leftSide ? e.left : e.right;
            os << std::hex << std::setw(2) << std::setfill('0') << unsigned(value) << ' ';
            ascii += printable(value);
        }
        else
        {
            os << "   ";
            ascii += ' ';
        }
        ++emitted;
    }
    while (emitted++ < count) os << "   ";
    ascii.resize(count, ' ');
    os << ' ' << ascii;
    return os.str();
}

static std::string asciiToken(uint8_t value, bool annotateSpaces)
{
    switch (value)
    {
    case 0x00: return "<NUL>";
    case 0x07: return "<BEL>";
    case 0x08: return "<BS>";
    case 0x09: return "<TAB>";
    case 0x0a: return "<LF>";
    case 0x0b: return "<VT>";
    case 0x0c: return "<FF>";
    case 0x0d: return "<CR>";
    case 0x1b: return "<ESC>";
    case 0x20: return annotateSpaces ? "<SP>" : " ";
    case 0x7f: return "<DEL>";
    default:
        if (value >= 0x21 && value <= 0x7e) return std::string(1, static_cast<char>(value));
        std::ostringstream os;
        os << "<x" << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << unsigned(value) << '>';
        return os.str();
    }
}

static std::string renderAscii(const std::vector<Edit>& edits, size_t begin, size_t end, bool leftSide,
                               size_t fieldWidth, bool annotateSpaces)
{
    std::string text;
    for (size_t i = begin; i < end; ++i)
    {
        const Edit& e = edits[i];
        const bool present = leftSide ? e.kind != EditKind::Insert : e.kind != EditKind::Delete;
        if (present) text += asciiToken(leftSide ? e.left : e.right, annotateSpaces);
    }
    text.resize(fieldWidth, ' ');
    return text;
}

static size_t asciiRenderedWidth(const Edit& edit, bool leftSide, bool annotateSpaces)
{
    const bool present = leftSide ? edit.kind != EditKind::Insert : edit.kind != EditKind::Delete;
    return present ? asciiToken(leftSide ? edit.left : edit.right, annotateSpaces).size() : 0;
}

static void printFile(const Bytes& data, const std::string& name, bool asciiMode, bool annotateSpaces,
                      size_t width, bool forceColor)
{
    const size_t offsetDigits = data.size() > 0xffffffffULL ? 16 : 8;
    const size_t fixed = offsetDigits + 2;
    const size_t contentWidth = std::max<size_t>(5, width > fixed ? width - fixed : 5);
    const size_t hexCount = std::max<size_t>(1, std::min<size_t>(16, contentWidth > 1 ? (contentWidth - 1) / 4 : 1));
    const bool color = forceColor || isatty(STDOUT_FILENO);
    if (color) std::cout << ut1::tui::ansiBold << ut1::tui::ansiCyan;
    std::cout << ut1::tui::fitTerminalLine(name, width) << '\n';
    if (color) std::cout << ut1::tui::ansiReset;

    size_t offset = 0;
    while (offset < data.size())
    {
        size_t end = std::min(data.size(), offset + hexCount);
        if (asciiMode)
        {
            end = offset;
            size_t used = 0;
            while (end < data.size())
            {
                const size_t tokenWidth = asciiToken(data[end], annotateSpaces).size();
                if (end > offset && used + tokenWidth > contentWidth) break;
                used += tokenWidth;
                const bool linefeed = data[end++] == '\n';
                if (linefeed) break;
            }
        }
        if (color) std::cout << ut1::tui::ansiCyan;
        std::cout << hexOffset(offset, offsetDigits) << ": ";
        if (color) std::cout << ut1::tui::ansiReset;
        if (asciiMode)
        {
            for (size_t i = offset; i < end; ++i) std::cout << asciiToken(data[i], annotateSpaces);
        }
        else
        {
            std::string ascii;
            for (size_t i = offset; i < end; ++i)
            {
                std::cout << std::hex << std::setw(2) << std::setfill('0') << unsigned(data[i]) << ' ';
                ascii += printable(data[i]);
            }
            for (size_t i = end; i < offset + hexCount; ++i) std::cout << "   ";
            std::cout << ' ' << ascii;
        }
        std::cout << '\n';
        offset = end;
    }
}

static const char* colorFor(EditKind kind)
{
    switch (kind)
    {
    case EditKind::Match: return ut1::tui::ansiGray;
    case EditKind::Substitute: return ut1::tui::ansiBrightYellow;
    case EditKind::Delete: return ut1::tui::ansiBrightRed;
    case EditKind::Insert: return ut1::tui::ansiBrightGreen;
    }
    return ut1::tui::ansiReset;
}

static void printDiff(const std::vector<Edit>& edits, const std::string& leftName, const std::string& rightName,
                      bool asciiMode, bool showMatches, bool annotateSpaces, size_t maxFileSize, size_t width,
                      bool forceColor)
{
    const size_t offsetDigits = maxFileSize > 0xffffffffULL ? 16 : 8;
    const size_t fixed = 2 * (offsetDigits + 2) + 3;
    const size_t hexCount = std::max<size_t>(1, std::min<size_t>(16, (width > fixed + 2 ? width - fixed - 2 : 8) / 8));
    const size_t sideWidth = asciiMode ? std::max<size_t>(5, width > fixed ? (width - fixed) / 2 : 5)
                                       : 4 * hexCount + 1;
    const bool color = forceColor || isatty(STDOUT_FILENO);
    if (color) std::cout << ut1::tui::ansiBold << ut1::tui::ansiCyan;
    const size_t headingWidth = offsetDigits + 2 + sideWidth;
    std::cout << std::left << std::setw(static_cast<int>(headingWidth))
              << ut1::tui::fitTerminalLine(leftName, headingWidth)
              << " | " << ut1::tui::fitTerminalLine(rightName, headingWidth) << '\n';
    if (color) std::cout << ut1::tui::ansiReset;

    size_t leftOffset = 0, rightOffset = 0;
    size_t i = 0;
    while (i < edits.size())
    {
        const EditKind kind = edits[i].kind;
        size_t runEnd = i;
        while (runEnd < edits.size() && edits[runEnd].kind == kind) ++runEnd;
        if (kind == EditKind::Match && !showMatches)
        {
            const size_t run = runEnd - i;
            if (color) std::cout << ut1::tui::ansiGray;
            std::cout << "... " << run << " matching byte" << (run == 1 ? "" : "s") << " ...\n";
            if (color) std::cout << ut1::tui::ansiReset;
            leftOffset += run; rightOffset += run; i = runEnd;
            continue;
        }
        while (i < runEnd)
        {
            size_t end = std::min(runEnd, i + hexCount);
            if (asciiMode)
            {
                end = i;
                size_t leftUsed = 0, rightUsed = 0;
                while (end < runEnd)
                {
                    const size_t nextLeft = leftUsed + asciiRenderedWidth(edits[end], true, annotateSpaces);
                    const size_t nextRight = rightUsed + asciiRenderedWidth(edits[end], false, annotateSpaces);
                    if (end > i && (nextLeft > sideWidth || nextRight > sideWidth)) break;
                    leftUsed = nextLeft;
                    rightUsed = nextRight;
                    const Edit& edit = edits[end++];
                    const bool leftLinefeed = edit.kind != EditKind::Insert && edit.left == '\n';
                    const bool rightLinefeed = edit.kind != EditKind::Delete && edit.right == '\n';
                    if (leftLinefeed || rightLinefeed) break;
                }
            }
            const std::string leftText = asciiMode ? renderAscii(edits, i, end, true, sideWidth, annotateSpaces) : renderHex(edits, i, end, true, hexCount);
            const std::string rightText = asciiMode ? renderAscii(edits, i, end, false, sideWidth, annotateSpaces) : renderHex(edits, i, end, false, hexCount);
            if (color) std::cout << colorFor(kind);
            std::cout << hexOffset(leftOffset, offsetDigits) << ": " << leftText << " | "
                      << hexOffset(rightOffset, offsetDigits) << ": " << rightText << '\n';
            if (color) std::cout << ut1::tui::ansiReset;
            for (size_t p = i; p < end; ++p)
            {
                if (edits[p].kind != EditKind::Insert) ++leftOffset;
                if (edits[p].kind != EditKind::Delete) ++rightOffset;
            }
            i = end;
        }
    }
}

int main(int argc, char* argv[])
{
    const char* usage = "Show a file or differences between two binary files.\n"
                        "\n"
                        "Usage: $programName [OPTIONS] FILE1 [FILE2]\n"
                        "\n"
                        "Examples:\n"
                        "  $programName firmware.bin\n"
                        "  $programName old.bin new.bin\n"
                        "  $programName --ascii --show-matches old.txt new.txt\n"
                        "  $programName --color --width 160 old.bin new.bin | less -R\n"
                        "\n"
                        "Options:";
    ut1::CommandLineParser cl("tbindiff", usage,
        "\n$programName version $version ($compileDate) *** Copyright (c) 2026 Johannes Overmann *** https://github.com/jovermann/tbindiff",
        "0.1.0");
    cl.addOption('x', "hex", "Show bytes in hexadecimal with an ASCII column.");
    cl.addOption('a', "ascii", "Show bytes as printable ASCII characters.");
    cl.addOption(' ', "annotate-spaces", "Show spaces as <SP> in ASCII mode.");
    cl.addOption('c', "color", "Force ANSI colors, for example when piping into less -R.");
    cl.addOption('w', "width", "Set output width, for example when piping into less.", "N", "0");
    cl.addOption('p', "progress", "Show progress while finding and diffing matching regions.");
    cl.addOption(' ', "fine-diff", "Use bounded dynamic programming within gaps between matching anchors.");
    cl.addOption('s', "anchor-size", "Minimum exact-match anchor size in bytes.", "N", "16");
    cl.addOption(' ', "show-matches", "Include matching bytes in the output.");
    cl.addOption(' ', "max-mem", "Rough maximum memory for dynamic-programming rows, in MiB.", "N", "10");
    try
    {
        cl.parse(argc, argv);
        if (cl.getArgs().empty() || cl.getArgs().size() > 2) cl.error("Expected one or two file names.");
        if (cl("hex") && cl("ascii")) cl.error("--hex and --ascii are mutually exclusive.");
        const uint64_t maxMemMiB = cl.getUInt("max-mem");
        if (maxMemMiB == 0 || maxMemMiB > std::numeric_limits<uint64_t>::max() / 1048576ULL)
            cl.error("--max-mem must be a positive integer.");
        const uint64_t requestedAnchorSize = cl.getUInt("anchor-size");
        if (requestedAnchorSize == 0 || requestedAnchorSize > std::numeric_limits<size_t>::max())
            cl.error("--anchor-size must be a positive integer.");
        const uint64_t requestedWidth = cl.getUInt("width");
        if (cl.isSet("width") && requestedWidth == 0) cl.error("--width must be a positive integer.");
        const size_t width = requestedWidth ? static_cast<size_t>(requestedWidth) : ut1::tui::terminalWidth();
        const Bytes left = readFile(cl.getArgs()[0]);
        if (cl.getArgs().size() == 1)
        {
            const bool asciiMode = cl("ascii") || (!cl("hex") && looksAscii(left));
            printFile(left, cl.getArgs()[0], asciiMode, cl("annotate-spaces"), width, cl("color"));
            return 0;
        }
        const Bytes right = readFile(cl.getArgs()[1]);
        const bool asciiMode = cl("ascii") || (!cl("hex") && looksAscii(left) && looksAscii(right));
        Progress progress(cl("progress"), width);
        const std::vector<Edit> edits = makeDiff(left, right, static_cast<size_t>(requestedAnchorSize),
                                                 cl("fine-diff"), maxMemMiB * 1048576ULL, progress);
        printDiff(edits, cl.getArgs()[0], cl.getArgs()[1], asciiMode, cl("show-matches"), cl("annotate-spaces"),
                  std::max(left.size(), right.size()), width, cl("color"));
        return std::any_of(edits.begin(), edits.end(), [](const Edit& e) { return e.kind != EditKind::Match; }) ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "tbindiff: Error: " << e.what() << '\n';
        return 2;
    }
}
