#include "./solver.hpp"

#include <iostream>     // cout, endl
#include <iomanip>      // setbase, setw,
#include <fstream>      // ifstream,
#include <ctime>        // clock,
#include <numeric>      // iota,
#include <algorithm>    // random_shuffle,
#include <string>       // string,


template <Sudoku::Order O>
std::ostream& operator<<(std::ostream& os, Sudoku::Solver::Solver<O> const& s) {
    using namespace Sudoku::Solver;
    using length_t = typename Sudoku::Solver::Solver<O>::length_t;
    #define PRINT_GRID0_TILE(PRINTER_STATEMENT) {\
        for (length_t col = 0; col < s.length; col++) {\
            if (isPretty && (col % s.order) == 0) os << Ansi::DIM.ON << " |" << Ansi::DIM.OFF;\
            PRINTER_STATEMENT;\
        }}
    #define PRINT_GRID_TILE(PRINTER_STATEMENT) {\
        if (isPretty) os << Ansi::DIM.ON << " |" << Ansi::DIM.OFF;\
        os << GRID_SEP;\
        PRINT_GRID0_TILE(PRINTER_STATEMENT)}

    const bool isPretty = &os == &std::cout;
    const auto printBlkRowSepString = [&](){
        if (!isPretty) return;
        os << '\n' << Ansi::DIM.ON;
        os << s.blkRowSepString;
        if constexpr (cbt) os << GRID_SEP << s.blkRowSepString;
        os << Ansi::DIM.OFF;
    };
    for (length_t row = 0; row < s.length; row++) {
        if (row % s.order == 0) {
            printBlkRowSepString();
        }
        os << '\n';
        // Tile content:
        #define index (row * s.length + col)
        PRINT_GRID0_TILE(os << ' ' << s.grid[index])
        if constexpr (cbt) {
            PRINT_GRID_TILE(s.printShadedBacktrackStat(s.backtrackCounts[index]))
        }
        // PRINT_GRID_TILE(os << std::setw(2) << grid[index].biasIndex)
        // PRINT_GRID_TILE(os << ' ' << rowBiases[row][col])
        #undef index
        if (isPretty) os << Ansi::DIM.ON << " |" << Ansi::DIM.OFF;
    }
    printBlkRowSepString();
    #undef PRINT_GRID_TILE
    #undef PRINT_GRID0_TILE
    return os;
}


namespace Sudoku::Solver {

// Guards accesses to RMG. I currently only
// use this when shuffling generator biases.
std::mutex RANDOM_MUTEX;
std::mt19937 VALUE_RNG;

struct MyNumpunct : std::numpunct<char> {
    std::string do_grouping(void) const {
        return "\03";
    }
};

#if SOLVER_THREADS_SHARE_GENPATH
template <Order O>
GenPath::E Solver<O>::genPath = initializeGenPath();
template <Order O>
std::array<typename Solver<O>::area_t, Solver<O>::area> Solver<O>::traversalOrder;
#endif


// Mechanism to statically toggle printing alignment:
// (#undef-ed before the end of this namespace)
#define STATW_I << std::setw(this->STATS_WIDTH)
#define STATW_D << std::setw(this->STATS_WIDTH + 4)


template <Order O>
Solver<O>::Solver(std::ostream& os):
    os          (os),
    isPretty    (&os == &std::cout)
{
    for (auto& rowBias : rowBiases) {
        std::iota(rowBias.begin(), rowBias.end(), 0);
    }
    #if !SOLVER_THREADS_SHARE_GENPATH
    initializeGenPath();
    #endif

    // Output formatting:
    if (isPretty) {
        os.imbue(std::locale(os.getloc(), new MyNumpunct()));
    }
    os.precision(3);
    os << std::fixed;
}


template <Order O>
void Solver<O>::print(void) const {
    if (&os != &std::cout) {
        std::cout << *this;
    }
    os << *this;
}


template <Order O>
void Solver<O>::printShadedBacktrackStat(const backtrack_t count) const {
    const std::array<std::string, 4> GREYSCALE_BLOCK_CHARS = {
        // NOTE: Make sure that the initializer list size matches that
        // of the corresponding template argument. Compilers won't warn.
        // See https://cppreference.com/w/cpp/language/sizeof...#Example
        // for an example utility function I can make to avoid this problem.
        u8"\u2591", u8"\u2592", u8"\u2593", u8"\u2588",
    };

    if constexpr (cbt) {
        const unsigned int relativeIntensity
            = (double)(count - 1)
            * GREYSCALE_BLOCK_CHARS.size()
            / maxBacktrackCount;
        auto const& intensityChar
            = (count != 0)
            ? GREYSCALE_BLOCK_CHARS[relativeIntensity]
            : " ";
        os << intensityChar << intensityChar;
    } else {
        // Complain if found in call-path when backtrack-counting is off:
        static_assert(cbt, "not avaliable when backtrack-counting is off.");
    }
}


template <Order O>
template <bool USE_PUZZLE>
void Solver<O>::clear(void) {
    for (area_t i = 0; i < area; i++) {
        // Clear all non-givens. Their values should already have been
        // set by `loadPuzzleFromString`. Recall that biasIndex for
        // givens must not be used (see Tile documentation).
        if ((USE_PUZZLE) ? (!isTileForGiven[i]) : true) grid[i].clear();
    }
    rowSymbolOccMasks.fill(0);
    colSymbolOccMasks.fill(0);
    blkSymbolOccMasks.fill(0);
    if constexpr (USE_PUZZLE) {
        // Fill back in the occmasks for givens:
        for (area_t i = 0; i < area; i++) {
            if (isTileForGiven[i]) {
                const occmask_t turnOnBitMask = (occmask_t)(0b1) << grid[i].value;
                rowSymbolOccMasks[getRow(i)] |= turnOnBitMask;
                colSymbolOccMasks[getCol(i)] |= turnOnBitMask;
                blkSymbolOccMasks[getBlk(i)] |= turnOnBitMask;
            }
        }
    }
    if constexpr (cbt) {
        backtrackCounts.fill(0u);
        maxBacktrackCount = 0u;
    }
    // Scramble each row's value-guessing-order:
    RANDOM_MUTEX.lock();
    for (auto& rowBias : rowBiases) {
        std::shuffle(rowBias.begin(), rowBias.end(), VALUE_RNG);
    }
    RANDOM_MUTEX.unlock();
    // Do not clear seeds here. That can be done when reading in givens.
}


template <Order O>
bool Solver<O>::loadPuzzleFromString(const std::string& puzzleString) {
    // This length check will be done again later, but might as well
    // do it now as a quick short-circuiter.
    if (puzzleString.length() != area) return false;

    // Clear any is-given=markers set for previous puzzles:
    isTileForGiven.reset();

    const PuzzleStrBlanksFmt blanksFmt
        = (puzzleString.find(' ') != std::string::npos)
        ? PuzzleStrBlanksFmt::SPACE
        : PuzzleStrBlanksFmt::ZERO;
    for (area_t i = 0; i < area; i++) {
        const char valueChar = puzzleString[i];
        switch (blanksFmt) {
        case PuzzleStrBlanksFmt::SPACE:
            if (valueChar != ' ') {
                registerGivenValue(i, Tile::VALUE_FROM_CHAR(valueChar));
            } break;
        case PuzzleStrBlanksFmt::ZERO:
            if (valueChar != '0') {
                registerGivenValue(i, Tile::VALUE_FROM_CHAR(valueChar) - 1);
            } break;
        }
    }
    return true;
}


template <Order O>
void Solver<O>::registerGivenValue(const area_t index, const value_t value) {
    isTileForGiven[index] = true;
    grid[index].value = value;
}


template <Order O>
template <bool USE_PUZZLE>
opcount_t Solver<O>::generateSolution(ExitStatus& exitStatus, const bool contPrev) {
    opcount_t numOperations = 0u;
    TvsDirection direction = TvsDirection::FORWARD;
    area_t tvsIndex = 0u;

    if (__builtin_expect(contPrev, false)) {
        if (prevGenTvsIndex == area) {
            // Previously succeeded.
            tvsIndex = area - 1;
            direction = TvsDirection::BACK;
        } else if (prevGenTvsIndex == 0
            && (grid[traversalOrder[0]].biasIndex == length)) {
            // TODO [test] This.
            // Previously realized nothing left to find.
            exitStatus = ExitStatus::IMPOSSIBLE;
            return 0u;
        } else {
            // Previously gave up.
            direction = TvsDirection::FORWARD;
        }
    } else {
        // Not continuing. Do something entirely new!
        this->template clear<USE_PUZZLE>();
    }

    while (tvsIndex < area) {
        const area_t gridIndex = traversalOrder[tvsIndex];
        if constexpr (USE_PUZZLE) {
            // Immediately pass over consecutive tiles containing givens.
            // This logic block ensures that this entire loop never exits
            // at a non-boundary traversal-index over a given-Tile.
            if (__builtin_expect(isTileForGiven[gridIndex], false)) {
                if (direction == TvsDirection::BACK) {
                    if (tvsIndex == 0) break;
                    else --tvsIndex;
                } else {
                    ++tvsIndex;
                }
                continue;
            }
        }
        // Try something at the current tile:
        direction = setNextValid(gridIndex);
        numOperations++;
        if (direction == TvsDirection::BACK) {
            // Pop and step backward:
            if constexpr (cbt) {
                if (++backtrackCounts[gridIndex] > maxBacktrackCount) {
                    maxBacktrackCount = backtrackCounts[gridIndex];
                }
            }
            if (__builtin_expect(tvsIndex == 0, false)) {
                break;
            }
            --tvsIndex;
        } else {
            // (direction == TvsDirection::FORWARD)
            ++tvsIndex;
        }
        // Check whether the give-up-condition has been met:
        const opcount_t giveupCondVar
            = (gum == GUM::E::OPERATIONS) ? numOperations
            : (gum == GUM::E::BACKTRACKS) ? maxBacktrackCount
            : [](){ throw "unhandled GUM case"; return ~0; }();
        if (__builtin_expect(giveupCondVar >= GIVEUP_THRESHOLD, false)) {
            break;
        }
    }
    // Return:
    prevGenTvsIndex = tvsIndex;
    exitStatus = (tvsIndex == 0)
        ? ExitStatus::IMPOSSIBLE
        : ((tvsIndex == area)
            ? ExitStatus::SUCCESS
            : ExitStatus::GIVEUP
        );
    return numOperations;
}


template <Order O>
TvsDirection Solver<O>::setNextValid(const area_t index) {
    occmask_t& rowBin = rowSymbolOccMasks[getRow(index)];
    occmask_t& colBin = colSymbolOccMasks[getCol(index)];
    occmask_t& blkBin = blkSymbolOccMasks[getBlk(index)];

    Tile& t = grid[index];
    if (!t.isClear()) {
        // If the tile is currently already set, clear it:
        // NOTE: this is the same as "if backtracked to here".
        const occmask_t eraseMask = ~(0b1 << t.value);
        rowBin &= eraseMask;
        colBin &= eraseMask;
        blkBin &= eraseMask;
    }

    const occmask_t invalidBin = (rowBin | colBin | blkBin);
    // NOTE: these do not improve time-scaling performance, but I wish they did.
    /*
    if (__builtin_expect(occmask_popcount(invalidBin) == length, false)) {
        t.clear();
        return BACK;
    } else if (occmask_popcount(invalidBin) == length - 1) {
        const value_t value = occmask_ctz(!invalidBin);
        const occmask_t valueBit = 0b1 << value;
        rowBin |= valueBit;
        colBin |= valueBit;
        blkBin |= valueBit;
        t.value = value;
        t.biasIndex = length;
        return FORWARD;
    }
    */
    for (value_t biasIndex = t.biasIndex; biasIndex < length; biasIndex++) {
        const value_t value = rowBiases[getRow(index)][biasIndex];
        const occmask_t valueBit = 0b1 << value;
        if (!(invalidBin & valueBit)) {
            // If a valid value is found for this tile:
            rowBin |= valueBit;
            colBin |= valueBit;
            blkBin |= valueBit;
            t.value = value;
            t.biasIndex = (biasIndex + 1);
            return TvsDirection::FORWARD;
        }
    }
    // Backtrack:
    // - turning back: The above loop never entered the return-block.
    // - continuing back: The above loop was completely skipped-over.
    t.clear();
    return TvsDirection::BACK;
}


template <Order O>
GenPath::E Solver<O>::setGenPath(const GenPath::E newGenPath, const bool force) noexcept {
    if (!force && newGenPath == getGenPath()) {
        // Short circuit:
        return getGenPath();
    }
    switch (newGenPath) {
        case GenPath::E::ROW_MAJOR:
            std::iota(traversalOrder.begin(), traversalOrder.end(), 0);
            break;
        case GenPath::E::BLOCK_COLS: {
            area_t i = 0;
            for (order_t blkCol = 0; blkCol < order; blkCol++) {
                for (length_t row = 0; row < length; row++) {
                    for (order_t bCol = 0; bCol < order; bCol++) {
                        traversalOrder[i++] = (blkCol * order) + (row * length) + (bCol);
                    }
                }
            }
            break; }
    }
    const GenPath::E oldGenPath = getGenPath();
    genPath = newGenPath;
    return oldGenPath;
}


template <Order O>
GenPath::E Solver<O>::setGenPath(std::string const& newGenPathString) noexcept {
    std::cout << "\ngenerator path is ";
    if (newGenPathString.empty()) {
        std::cout << "currently set to: " << getGenPath() << std::endl;
        return getGenPath();
    }
    for (unsigned i = 0; i < GenPath::size; i++) {
        if (newGenPathString.compare(GenPath::NAMES[i]) == 0) {
            if (GenPath::E{i} == getGenPath()) {
                std::cout << "already set to: ";
            } else {
                std::cout << "now set to: ";
                setGenPath(GenPath::E{i});
            }
            std::cout << getGenPath() << std::endl;
            return getGenPath();
        }
    }
    // unsuccessful return:
    std::cout << getGenPath() << " (unchanged).\n" << Ansi::RED.ON << '"' << newGenPathString
        << "\" is not a valid generator path name." << Ansi::RED.OFF << std::endl;
    std::cout << GenPath::OPTIONS_MENU << std::endl;
    return getGenPath();
}


template <Order O>
GenPath::E Solver<O>::initializeGenPath(void) noexcept {
    GenPath::E defaultGenPath;
    // Interesting: Smaller-order grids perform better with ROW_MAJOR as genPath.
    if ((order < 4) || (order == 4 && gum == GUM::E::BACKTRACKS)) {
        defaultGenPath = GenPath::E::ROW_MAJOR;
    } else {
        defaultGenPath = GenPath::E::BLOCK_COLS;
    }
    setGenPath(defaultGenPath, true);
    return defaultGenPath;
}




template <Order O>
void Solver<O>::printMessageBar(
    std::string const& msg,
    unsigned barLength,
    const char fillChar
) const {
    if (barLength < msg.length() + 8) {
        barLength = msg.length() + 8;
    }
    std::string bar(barLength, fillChar);
    if (!msg.empty()) {
        bar.replace(4, msg.length(), msg);
        bar.at(3) = ' ';
        bar.at(4 + msg.length()) = ' ';
    }
    os << '\n' <<bar;
}


template <Order O>
void Solver<O>::printMessageBar(std::string const& msg, const char fillChar) const {
    const unsigned gridBarLength = (isPretty)
        ? ((length + order + 1) * 2)
        : (length * 2);
    constexpr unsigned numGrids = 1 + unsigned(cbt);
    unsigned allBarLength = (numGrids * gridBarLength);
    if (numGrids > 1) allBarLength += (numGrids - 1) * GRID_SEP.length();
    return printMessageBar(msg, allBarLength + 1, fillChar);
}

#undef STATW_I
#undef STATW_D


template <Order O>
const std::string Solver<O>::blkRowSepString = [](const unsigned order) {
    std::string vbar = " " + std::string((((order * (order + 1)) + 1) * 2 - 1), '-');
    for (unsigned i = 0; i <= order; i++) {
        // Insert crosses at vbar intersections.
        vbar[(2 * (order + 1) * i) + 1] = '+';
    }
    return vbar;
}(O);

} // End of Sudoku namespace.
