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

/// Kind of one aligned byte-level edit operation.
enum class EditKind { Match, Substitute, Delete, Insert };

/// One element of an edit script, with bytes stored on their respective sides.
/// The unused byte is zero for an insertion or deletion.
struct Edit
{
    /// Operation represented by this element.
    EditKind kind{};
    /// Byte from the left input, or zero for an insertion.
    uint8_t left{};
    /// Byte from the right input, or zero for a deletion.
    uint8_t right{};
};

/// In-memory byte sequence used for input files.
using Bytes = std::vector<uint8_t>;
/// Dynamic-programming edit cost. Each operation has unit cost.
using Cost = uint32_t;

/// Global upper bound on DP cell evaluations across all anchor gaps.
static constexpr uint64_t maxDpCells = 30000000;

/// Offset representation used by the interactive UI.
enum class OffsetFormat { Hexadecimal, Decimal, HumanReadable };

/// Optional single-line progress reporter writing to standard error.
class Progress
{
public:
    /// Construct a reporter.
    /// \param enabled_ Whether any progress output should be emitted.
    /// \param width_ Maximum rendered line width in terminal columns.
    Progress(bool enabled_, size_t width_): enabled(enabled_), width(width_) {}

    /// Replace the current progress line with a named phase.
    /// \param text Human-readable phase description.
    void phase(const std::string& text)
    {
        if (!enabled) return;
        clear();
        current = text;
        lastPercent = 101;
        print();
    }

    /// Update percentage progress, suppressing duplicate percentages.
    /// \param done Completed work units.
    /// \param total Total work units; zero is treated as complete.
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

    /// Clear the transient line and emit the final completion message.
    void finish()
    {
        if (!enabled) return;
        clear();
        std::cerr << "Diffing: done\n";
        lineLength = 0;
    }

private:
    /// Erase the previously rendered progress line, if any.
    void clear()
    {
        if (lineLength) std::cerr << '\r' << std::string(lineLength, ' ') << '\r';
    }

    /// Render and flush current, truncated to the configured width.
    void print()
    {
        const std::string line = ut1::tui::fitTerminalLine(current, width);
        std::cerr << line << std::flush;
        lineLength = line.size();
    }

    /// Whether reporting is enabled.
    bool enabled{};
    /// Maximum progress-line width.
    size_t width{};
    /// Number of columns occupied by the current line.
    size_t lineLength{};
    /// Most recently printed percentage; 101 means no percentage was printed.
    size_t lastPercent{101};
    /// Current phase or percentage text.
    std::string current;
};

/// Read an entire file as uninterpreted bytes.
/// \param path File to open and read in binary mode.
/// \return Complete file contents, including embedded zero bytes.
/// \throws std::runtime_error if the file cannot be opened, sized, or read.
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

/// Decide whether a file is sufficiently text-like for automatic ASCII mode.
/// \param data Bytes to classify.
/// \return True for an empty file or when strictly less than 10% of bytes are
/// binary. Printable ASCII and the named controls TAB, LF, VT, FF, CR, and ESC
/// are non-binary; all other controls and bytes above 0x7f are binary.
static bool looksAscii(const Bytes& data)
{
    size_t binaryBytes = 0;
    for (uint8_t value: data)
    {
        const bool namedControl = (value >= 0x09 && value <= 0x0d) || value == 0x1b;
        const bool printableAscii = value >= 0x20 && value <= 0x7e;
        binaryBytes += !namedControl && !printableAscii;
    }
    return data.empty() || static_cast<long double>(binaryBytes) * 10.0L < static_cast<long double>(data.size());
}

/// Compute the final Levenshtein-cost row for two byte ranges.
/// \param a First range start.
/// \param na Number of bytes in the first range.
/// \param b Second range start.
/// \param nb Number of bytes in the second range and resulting row width minus one.
/// \param reverse Compare both ranges from their ends when true.
/// \return Costs for aligning all of a against every prefix of b. In reverse
/// mode, prefixes refer to reversed ranges and therefore represent suffix costs.
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

/// Produce an exact edit script for a DP base case.
/// \param a Left byte range start.
/// \param na Left range length; Hirschberg calls this when at least one side is no longer than one.
/// \param b Right byte range start.
/// \param nb Right range length.
/// \param out Destination to which operations are appended in forward order.
/// Substitution, insertion, and deletion have unit cost; a match costs zero.
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

/// Append an exact minimum-cost edit script using Hirschberg recursion.
/// \param a Left byte range start.
/// \param na Left range length.
/// \param b Right byte range start.
/// \param nb Right range length.
/// \param out Destination to which operations are appended.
/// Uses linear DP-row memory while retaining quadratic worst-case running time.
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

/// Append an exact diff while placing the shorter input on the DP row axis.
/// \param left Left byte range start.
/// \param leftSize Left range length.
/// \param right Right byte range start.
/// \param rightSize Right range length.
/// \param result Destination edit script.
/// Swapping is internal only; returned insertions and deletions retain caller orientation.
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

/// Occurrence information accumulated for one sampled anchor hash.
struct AnchorInfo
{
    /// Last position observed in the left input.
    size_t left{};
    /// Last position observed in the right input.
    size_t right{};
    /// Number of occurrences in the left input.
    unsigned leftCount{};
    /// Number of occurrences in the right input.
    unsigned rightCount{};
};

/// Corresponding positions of an exact anchor in both inputs.
struct Anchor
{
    /// Anchor start in the left input.
    size_t left{};
    /// Anchor start in the right input.
    size_t right{};
};

/// Find an ordered chain of unique exact-match anchors.
/// \param left Complete left input.
/// \param right Complete right input.
/// \param anchorSize Number of exact bytes required by each seed; must be positive.
/// \return Non-overlapping anchors ordered by both left and right offsets.
/// Windows are content-sampled at roughly 1/32 density using a rolling hash.
/// Only hashes occurring exactly once per input are candidates, hash matches are
/// byte-verified, and a longest increasing subsequence removes crossing matches.
static std::vector<Anchor> findAnchors(const Bytes& left, const Bytes& right, size_t anchorSize)
{
    std::unordered_map<uint64_t, AnchorInfo> table;
    table.reserve((left.size() + right.size()) / 24 + 1);
    /// Add sampled window occurrences from one input to table.
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
    /// Order candidates primarily by their left offsets for LIS processing.
    std::sort(candidates.begin(), candidates.end(), [](const Anchor& a, const Anchor& b) {
        return a.left != b.left ? a.left < b.left : a.right < b.right;
    });

    // Longest increasing subsequence selects a non-crossing chain of matching anchors.
    std::vector<size_t> tails;
    std::vector<size_t> previous(candidates.size(), candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        /// Compare an indexed candidate's right offset with an offset value.
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

/// Append a gap between two trusted matching regions.
/// \param left Left gap start.
/// \param leftSize Left gap length.
/// \param right Right gap start.
/// \param rightSize Right gap length.
/// \param fineDiff Enable exact DP for affordable portions of the gap.
/// \param maxMemory Approximate byte limit for the two live DP rows.
/// \param cellsLeft Remaining global DP-cell work budget, reduced by exact work.
/// \param result Destination edit script.
/// Without fineDiff, bytes are paired positionally as substitutions and excess
/// bytes become deletions or insertions; incidental equality is intentionally
/// ignored. Fine mode preserves equal prefix/suffix bytes and uses exact DP only
/// when both memory and work budgets permit, otherwise using the same fallback.
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

/// Build the complete hybrid edit script.
/// \param left Complete left input.
/// \param right Complete right input.
/// \param anchorSize Exact seed length for anchor discovery.
/// \param fineDiff Whether to use bounded DP inside anchor gaps.
/// \param maxMemory Approximate memory limit for DP rows in bytes.
/// \param progress Reporter for anchor discovery and region processing.
/// \return Forward edit script covering every byte of both inputs.
/// Identical files bypass anchor sampling. Other files use maximally expanded
/// anchors; sampling chooses seeds but never limits the final match boundaries.
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
    /// Return a range pointer without performing arithmetic on an empty vector's null data pointer.
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

/// Format a byte offset as lower-case, zero-padded hexadecimal.
/// \param offset Numeric byte offset.
/// \param digits Minimum output width, normally 8 or 16.
/// \return Formatted offset without a prefix.
static std::string hexOffset(size_t offset, size_t digits)
{
    std::ostringstream os;
    os << std::hex << std::setfill('0') << std::setw(static_cast<int>(digits)) << offset;
    return os.str();
}

/// Format an offset according to the selected UI representation.
/// \param offset Byte offset to format.
/// \param format Hexadecimal, decimal, or scaled human-readable representation.
/// \param maxFileSize Larger input size; selects one common human-readable unit.
/// \return Formatted offset without a trailing colon.
static std::string formatOffset(size_t offset, OffsetFormat format, size_t maxFileSize)
{
    if (format == OffsetFormat::Hexadecimal)
        return hexOffset(offset, maxFileSize > 0xffffffffULL ? 16 : 8);
    if (format == OffsetFormat::Decimal) return std::to_string(offset);
    static constexpr const char* units[] = {"B", "kB", "MB", "GB", "TB", "PB", "EB"};
    size_t unit = 0;
    long double factor = 1.0L;
    while (unit + 1 < std::size(units) && static_cast<long double>(maxFileSize) >= factor * 1024.0L)
    {
        factor *= 1024.0L;
        ++unit;
    }
    std::ostringstream os;
    if (unit == 0) os << offset;
    else os << std::fixed << std::setprecision(1) << static_cast<long double>(offset) / factor;
    os << units[unit];
    return os.str();
}

/// Return the fixed column width needed for offsets in a diff.
/// \param format Selected offset representation.
/// \param maxFileSize Larger input size and largest possible offset.
/// \return Width sufficient for every formatted offset in both inputs.
static size_t offsetWidth(OffsetFormat format, size_t maxFileSize)
{
    return std::max(formatOffset(0, format, maxFileSize).size(),
                    formatOffset(maxFileSize, format, maxFileSize).size());
}

/// Return a compact status-bar name for an offset representation.
/// \param format Offset representation to name.
/// \return Short stable label.
static const char* offsetFormatShortName(OffsetFormat format)
{
    switch (format)
    {
    case OffsetFormat::Hexadecimal: return "hex";
    case OffsetFormat::Decimal: return "dec";
    case OffsetFormat::HumanReadable: return "human";
    }
    return "unknown";
}

/// Convert a byte for a conventional hexdump ASCII column.
/// \param value Byte to convert.
/// \return Its printable character, or '.' when it is not printable.
static char printable(uint8_t value)
{
    return std::isprint(static_cast<unsigned char>(value)) ? static_cast<char>(value) : '.';
}

/// Render one side of a fixed-width hexadecimal diff row.
/// \param edits Complete edit script.
/// \param begin First edit to render.
/// \param end One-past-last edit to render.
/// \param leftSide Select left bytes when true, right bytes otherwise.
/// \param count Number of byte slots in the row.
/// \return Hexadecimal byte slots followed by an unadorned ASCII column.
/// Missing insertion/deletion bytes occupy blank slots to preserve alignment.
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

/// Render one byte as an ASCII-mode token.
/// \param value Byte to render.
/// \param annotateSpaces Render space as <SP> instead of a literal space.
/// \return A printable character, a named control token, or <xXX> for other bytes.
static std::string asciiToken(uint8_t value, bool annotateSpaces)
{
    switch (value)
    {
    case 0x09: return "<TAB>";
    case 0x0a: return "<LF>";
    case 0x0b: return "<VT>";
    case 0x0c: return "<FF>";
    case 0x0d: return "<CR>";
    case 0x1b: return "<ESC>";
    case 0x20: return annotateSpaces ? "<SP>" : " ";
    default:
        if (value >= 0x21 && value <= 0x7e) return std::string(1, static_cast<char>(value));
        std::ostringstream os;
        os << "<x" << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << unsigned(value) << '>';
        return os.str();
    }
}

/// Render one side of an ASCII diff row.
/// \param edits Complete edit script.
/// \param begin First edit to render.
/// \param end One-past-last edit to render.
/// \param leftSide Select left bytes when true, right bytes otherwise.
/// \param fieldWidth Width to which the result is padded.
/// \param annotateSpaces Whether spaces use the <SP> token.
/// \return Concatenated tokens padded on the right to fieldWidth.
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

/// Measure one edit's ASCII token on a selected side.
/// \param edit Edit to inspect.
/// \param leftSide Select the left side when true.
/// \param annotateSpaces Whether spaces use the <SP> token.
/// \return Token width, or zero when the edit has no byte on that side.
static size_t asciiRenderedWidth(const Edit& edit, bool leftSide, bool annotateSpaces)
{
    const bool present = leftSide ? edit.kind != EditKind::Insert : edit.kind != EditKind::Delete;
    return present ? asciiToken(leftSide ? edit.left : edit.right, annotateSpaces).size() : 0;
}

/// Print one file as a single-column viewer.
/// \param data File bytes.
/// \param name Display name printed as the heading.
/// \param asciiMode Select tokenized ASCII instead of hexadecimal hexdump output.
/// \param annotateSpaces Whether ASCII spaces use <SP>.
/// \param width Maximum desired terminal width.
/// \param forceColor Enable ANSI colors even when stdout is not a terminal.
/// ASCII rows use rendered token widths and end immediately after an LF token.
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

/// Select the ANSI foreground color associated with an edit kind.
/// \param kind Edit category to color.
/// \return Pointer to a process-lifetime ANSI SGR string.
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

/// Column dimensions shared by diff rows and their filename heading.
struct DiffLayout
{
    /// Width of each offset field.
    size_t offset{};
    /// Number of byte slots in a hexadecimal row.
    size_t hexCount{};
    /// Width of each rendered content field.
    size_t side{};
};

/// Calculate side-by-side diff columns for the active terminal and modes.
/// \param asciiMode Whether content uses variable-width ASCII tokens.
/// \param maxFileSize Larger input size, used for offset dimensions.
/// \param width Available terminal width.
/// \param offsetFormat Active offset representation.
/// \return Dimensions used by both heading and data rows.
static DiffLayout getDiffLayout(bool asciiMode, size_t maxFileSize, size_t width, OffsetFormat offsetFormat)
{
    const size_t offset = offsetWidth(offsetFormat, maxFileSize);
    const size_t fixed = 2 * (offset + 2) + 3;
    const size_t hexCount = std::max<size_t>(1, std::min<size_t>(16, (width > fixed + 2 ? width - fixed - 2 : 8) / 8));
    const size_t side = asciiMode ? std::max<size_t>(5, width > fixed ? (width - fixed) / 2 : 5)
                                  : 4 * hexCount + 1;
    return {offset, hexCount, side};
}

/// Format the filename title using the exact columns of the diff body.
/// \param leftName Left filename.
/// \param rightName Right filename.
/// \param asciiMode Whether content uses ASCII mode.
/// \param maxFileSize Larger input size.
/// \param width Available terminal width.
/// \param offsetFormat Active offset representation.
/// \return Plain title text with its separator aligned to data-row separators.
static std::string formatDiffHeading(const std::string& leftName, const std::string& rightName, bool asciiMode,
                                     size_t maxFileSize, size_t width, OffsetFormat offsetFormat)
{
    const DiffLayout layout = getDiffLayout(asciiMode, maxFileSize, width, offsetFormat);
    const size_t headingWidth = layout.offset + 2 + layout.side;
    std::ostringstream os;
    os << std::left << std::setw(static_cast<int>(headingWidth))
       << ut1::tui::fitTerminalLine(leftName, headingWidth)
       << " | " << ut1::tui::fitTerminalLine(rightName, headingWidth);
    return os.str();
}

/// Print a side-by-side edit script.
/// \param edits Complete forward edit script.
/// \param leftName Left input display name.
/// \param rightName Right input display name.
/// \param asciiMode Select tokenized ASCII instead of hexadecimal output.
/// \param showMatches Include matching regions instead of compact summaries.
/// \param annotateSpaces Whether ASCII spaces use <SP>.
/// \param maxFileSize Larger input size, used to choose offset width.
/// \param offsetFormat Representation used for byte offsets.
/// \param width Maximum desired terminal width.
/// \param forceColor Enable ANSI colors even when stdout is not a terminal.
/// \param os Stream receiving rendered output.
/// \param showHeading Print the ordinary filename heading when true.
/// \param lineOffsets Optional destination receiving the left byte offset of
/// every emitted body row; heading rows are not included.
/// Rows never mix edit kinds, and ASCII rows end after an LF on either side.
static void printDiff(const std::vector<Edit>& edits, const std::string& leftName, const std::string& rightName,
                      bool asciiMode, bool showMatches, bool annotateSpaces, size_t maxFileSize, size_t width,
                      OffsetFormat offsetFormat, bool forceColor, std::ostream& os = std::cout, bool showHeading = true,
                      std::vector<size_t>* lineOffsets = nullptr)
{
    const DiffLayout layout = getDiffLayout(asciiMode, maxFileSize, width, offsetFormat);
    const size_t offsetDigits = layout.offset;
    const size_t hexCount = layout.hexCount;
    const size_t sideWidth = layout.side;
    const bool color = forceColor || isatty(STDOUT_FILENO);
    if (showHeading)
    {
        if (color) os << ut1::tui::ansiBold << ut1::tui::ansiCyan;
        os << formatDiffHeading(leftName, rightName, asciiMode, maxFileSize, width, offsetFormat);
        if (color) os << ut1::tui::ansiReset;
        os << '\n';
    }

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
            if (lineOffsets) lineOffsets->push_back(leftOffset);
            if (color) os << ut1::tui::ansiGray;
            os << "... " << run << " matching byte" << (run == 1 ? "" : "s") << " ...";
            if (color) os << ut1::tui::ansiReset;
            os << '\n';
            leftOffset += run; rightOffset += run; i = runEnd;
            continue;
        }
        while (i < runEnd)
        {
            if (lineOffsets) lineOffsets->push_back(leftOffset);
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
            if (color) os << colorFor(kind);
            os << std::right << std::setw(static_cast<int>(offsetDigits))
               << formatOffset(leftOffset, offsetFormat, maxFileSize) << ": " << leftText << " | "
               << std::setw(static_cast<int>(offsetDigits))
               << formatOffset(rightOffset, offsetFormat, maxFileSize) << ": " << rightText;
            if (color) os << ut1::tui::ansiReset;
            os << '\n';
            for (size_t p = i; p < end; ++p)
            {
                if (edits[p].kind != EditKind::Insert) ++leftOffset;
                if (edits[p].kind != EditKind::Delete) ++rightOffset;
            }
            i = end;
        }
    }
}

/// RAII owner of the terminal alternate-screen buffer.
class AlternateScreen
{
public:
    /// Enter the alternate screen and hide the cursor.
    AlternateScreen() { ut1::tui::enterAlternateScreen(); }

    /// Restore the normal screen and show the cursor.
    ~AlternateScreen() { ut1::tui::leaveAlternateScreen(); }

    AlternateScreen(const AlternateScreen&) = delete;
    AlternateScreen& operator=(const AlternateScreen&) = delete;
};

/// Split rendered output into logical terminal lines.
/// \param text Newline-delimited rendered text, possibly containing ANSI escapes.
/// \return Lines without their newline delimiters.
static std::vector<std::string> splitRenderedLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) lines.push_back(line);
    return lines;
}

/// Render a full-width colored title or status bar.
/// \param text Plain text to truncate and display.
/// \param color ANSI foreground/background sequence for the bar.
/// \param width Terminal width in columns.
static void printUiBar(const std::string& text, const char* color, size_t width)
{
    const std::string fitted = ut1::tui::fitTerminalLine(text, width);
    std::cout << color << fitted;
    if (fitted.size() < width) std::cout << std::string(width - fitted.size(), ' ');
    std::cout << ut1::tui::ansiReset;
}

/// Run the interactive side-by-side diff pager.
/// \param edits Complete edit script to display.
/// \param leftName Left filename for the title.
/// \param rightName Right filename for the title.
/// \param asciiMode Initial ASCII/binary display mode.
/// \param showMatches Initial match visibility.
/// \param annotateSpaces Whether ASCII spaces use <SP>.
/// \param maxFileSize Larger input size, used to select offset width.
/// Cursor keys scroll one row, Page Up/Down scroll one screen, Space scrolls
/// down, and b scrolls up one screen. The a, m, h, c, and o keys toggle display
/// mode, matches, help, ANSI colors, and offset format. The s key toggles space
/// annotations in ASCII mode, p or g jumps to the beginning, G jumps to the end, and
/// The e and y keys move one line forward/backward and f moves one page forward.
/// The q key or Escape exits.
static void runUi(const std::vector<Edit>& edits, const std::string& leftName, const std::string& rightName,
                  bool asciiMode, bool showMatches, bool annotateSpaces, size_t maxFileSize)
{
    if (!isatty(STDOUT_FILENO)) throw std::runtime_error("--ui requires terminal output.");
    ut1::tui::TerminalRawMode rawMode(STDIN_FILENO);
    AlternateScreen alternateScreen;
    size_t firstLine = 0;
    bool running = true;
    bool renderDirty = true;
    size_t renderedWidth = 0;
    std::vector<std::string> lines;
    std::vector<size_t> lineOffsets;
    bool showHelp = false;
    bool showColors = false;
    OffsetFormat offsetFormat = OffsetFormat::Hexadecimal;
    size_t topOffset = 0;
    bool restoreTopOffset = false;
    while (running)
    {
        const size_t width = ut1::tui::terminalWidth();
        const size_t height = ut1::tui::terminalHeight();
        const size_t bodyHeight = height > 2 ? height - 2 : 1;
        if (!showHelp && !showColors && renderedWidth && renderedWidth != width && firstLine < lineOffsets.size())
        {
            topOffset = lineOffsets[firstLine];
            restoreTopOffset = true;
        }
        if (renderDirty || renderedWidth != width)
        {
            std::ostringstream rendered;
            lineOffsets.clear();
            printDiff(edits, leftName, rightName, asciiMode, showMatches, annotateSpaces, maxFileSize,
                      width, offsetFormat, true, rendered, false, &lineOffsets);
            lines = splitRenderedLines(rendered.str());
            renderedWidth = width;
            renderDirty = false;
        }
        if (!showHelp && !showColors && restoreTopOffset)
        {
            auto it = std::lower_bound(lineOffsets.begin(), lineOffsets.end(), topOffset);
            if (it == lineOffsets.end() || *it != topOffset)
            {
                if (it != lineOffsets.begin()) --it;
            }
            firstLine = static_cast<size_t>(it - lineOffsets.begin());
            restoreTopOffset = false;
        }
        std::vector<std::string> overlayLines;
        if (showHelp)
        {
            overlayLines = {
                "Keys",
                "",
                "Up / Down       Scroll one line",
                "e / y           Scroll one line forward / backward",
                "Page Up / b     Scroll one page backward",
                "Page Down / Space / f  Scroll one page forward",
                "p / g           Jump to beginning",
                "G               Jump to end",
                "a               Toggle ASCII and binary/hex mode",
                "m               Toggle matching regions",
                "s               Toggle <SP> annotations in ASCII mode",
                "o               Cycle hex, decimal, and human-readable offsets",
                "c               Toggle ANSI color chart",
                "h               Toggle this help",
                "q / Escape      Quit",
            };
        }
        else if (showColors)
        {
            std::ostringstream matrix;
            ut1::tui::printAnsiColorMatrix(matrix, bodyHeight, width);
            overlayLines = splitRenderedLines(matrix.str());
        }
        const std::vector<std::string>& displayedLines = (showHelp || showColors) ? overlayLines : lines;
        const size_t maxFirst = displayedLines.size() > bodyHeight ? displayedLines.size() - bodyHeight : 0;
        firstLine = std::min(firstLine, maxFirst);

        ut1::tui::clearScreen();
        printUiBar(formatDiffHeading(leftName, rightName, asciiMode, maxFileSize, width, offsetFormat),
                   ut1::tui::ansiWhiteOnBlue, width);
        std::cout << '\n';
        for (size_t row = 0; row < bodyHeight; ++row)
        {
            const size_t index = firstLine + row;
            if (index < displayedLines.size()) std::cout << displayedLines[index];
            std::cout << ut1::tui::ansiReset << "\033[K\n";
        }
        const std::string mode = asciiMode ? "ASCII" : "binary";
        const std::string matches = showMatches ? "shown" : "hidden";
        const std::string spaces = asciiMode ? " s:" + std::string(annotateSpaces ? "annotated" : "plain") : "";
        printUiBar(std::to_string(firstLine + 1) + "/" + std::to_string(std::max<size_t>(1, displayedLines.size())) +
                       " a:" + mode + " m:" + matches + spaces + " o:" + offsetFormatShortName(offsetFormat) +
                       " h:help q:quit",
                   ut1::tui::ansiBlackOnCyan, width);
        std::cout.flush();

        const int key = ut1::tui::readKey();
        if (key == 'q' || key == 'Q' || key == 27) running = false;
        else if ((key == ut1::tui::keyUp || key == 'y' || key == 'Y') && firstLine > 0) --firstLine;
        else if ((key == ut1::tui::keyDown || key == 'e' || key == 'E') && firstLine < maxFirst) ++firstLine;
        else if (key == ut1::tui::keyPageUp) firstLine = firstLine > bodyHeight ? firstLine - bodyHeight : 0;
        else if (key == ut1::tui::keyPageDown || key == ' ' || key == 'f' || key == 'F')
            firstLine = std::min(maxFirst, firstLine + bodyHeight);
        else if (key == 'b' || key == 'B') firstLine = firstLine > bodyHeight ? firstLine - bodyHeight : 0;
        else if (key == 'p' || key == 'g') firstLine = 0;
        else if (key == 'G') firstLine = maxFirst;
        else if (key == 'm' || key == 'M')
        {
            if (!showHelp && !showColors && firstLine < lineOffsets.size()) topOffset = lineOffsets[firstLine];
            showMatches = !showMatches;
            renderDirty = true;
            restoreTopOffset = !showHelp && !showColors;
        }
        else if (key == 'a' || key == 'A')
        {
            if (!showHelp && !showColors && firstLine < lineOffsets.size()) topOffset = lineOffsets[firstLine];
            asciiMode = !asciiMode;
            renderDirty = true;
            restoreTopOffset = !showHelp && !showColors;
        }
        else if ((key == 's' || key == 'S') && asciiMode)
        {
            if (!showHelp && !showColors && firstLine < lineOffsets.size()) topOffset = lineOffsets[firstLine];
            annotateSpaces = !annotateSpaces;
            renderDirty = true;
            restoreTopOffset = !showHelp && !showColors;
        }
        else if (key == 'h' || key == 'H')
        {
            if (!showHelp && !showColors && firstLine < lineOffsets.size()) topOffset = lineOffsets[firstLine];
            showHelp = !showHelp;
            if (showHelp) showColors = false;
            firstLine = 0;
            restoreTopOffset = !showHelp;
        }
        else if (key == 'c' || key == 'C')
        {
            if (!showHelp && !showColors && firstLine < lineOffsets.size()) topOffset = lineOffsets[firstLine];
            showColors = !showColors;
            if (showColors) showHelp = false;
            firstLine = 0;
            restoreTopOffset = !showColors;
        }
        else if (key == 'o' || key == 'O')
        {
            if (!showHelp && !showColors && firstLine < lineOffsets.size()) topOffset = lineOffsets[firstLine];
            if (offsetFormat == OffsetFormat::Hexadecimal) offsetFormat = OffsetFormat::Decimal;
            else if (offsetFormat == OffsetFormat::Decimal) offsetFormat = OffsetFormat::HumanReadable;
            else offsetFormat = OffsetFormat::Hexadecimal;
            renderDirty = true;
            restoreTopOffset = !showHelp && !showColors;
        }
    }
}

/// Parse command-line arguments and run viewer or diff mode.
/// \param argc Number of command-line arguments.
/// \param argv Argument vector; one file selects viewer mode and two select diff mode.
/// \return 0 for successful viewing or identical files, 1 for differences or
/// command-line usage errors, and 2 for runtime failures.
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
    cl.addOption('u', "ui", "Open an interactive terminal pager for a two-file diff.");
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
            if (cl("ui")) cl.error("--ui requires two file names.");
            const bool asciiMode = cl("ascii") || (!cl("hex") && looksAscii(left));
            printFile(left, cl.getArgs()[0], asciiMode, cl("annotate-spaces"), width, cl("color"));
            return 0;
        }
        const Bytes right = readFile(cl.getArgs()[1]);
        const bool asciiMode = cl("ascii") || (!cl("hex") && looksAscii(left) && looksAscii(right));
        Progress progress(cl("progress"), width);
        const std::vector<Edit> edits = makeDiff(left, right, static_cast<size_t>(requestedAnchorSize),
                                                 cl("fine-diff"), maxMemMiB * 1048576ULL, progress);
        if (cl("ui"))
            runUi(edits, cl.getArgs()[0], cl.getArgs()[1], asciiMode, cl("show-matches"),
                  cl("annotate-spaces"), std::max(left.size(), right.size()));
        else
            printDiff(edits, cl.getArgs()[0], cl.getArgs()[1], asciiMode, cl("show-matches"), cl("annotate-spaces"),
                      std::max(left.size(), right.size()), width, OffsetFormat::Hexadecimal, cl("color"));
        return std::any_of(edits.begin(), edits.end(), [](const Edit& e) { return e.kind != EditKind::Match; }) ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "tbindiff: Error: " << e.what() << '\n';
        return 2;
    }
}
