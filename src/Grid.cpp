#include"glew.h"
#include<GLFW/glfw3.h>

#include"Misc.h"
#include"Grid.h"
#include<vector>

#include<cassert> 

#include"Timer.h"
#include"AutoTimer.h"
#include"MedianCounter.h"

#include<nmmintrin.h> 

#include<algorithm>

using Cells = uint32_t;
static constexpr auto cellsBatchSize = 4;
static constexpr auto cellsBatchLength = 32;

struct Field::FieldPimpl {
    using BufferType = bool;
    static constexpr BufferType bufCur = false;
    static constexpr BufferType bufNext = true;

    int32_t virtualWidth;
    int32_t height;
    int32_t rowLength;
    bool edgeCellsOptimization;

    std::unique_ptr<Cells> currentBuf; 
    std::unique_ptr<Cells> nextBuf;

public:
    FieldPimpl(const int32_t gridWidth, const int32_t gridHeight) {
        virtualWidth = gridWidth;
        height = gridHeight;
        rowLength = misc::intDivCeil(virtualWidth, cellsBatchLength);
        edgeCellsOptimization = rowLength * cellsBatchLength - virtualWidth >= 2; 

        auto const bufferLen = bufferLength();
        currentBuf = std::unique_ptr<Cells>{ new Cells[bufferLen]{} };
        nextBuf    = std::unique_ptr<Cells>{ new Cells[bufferLen]{} };
    }
    ~FieldPimpl() = default;

    void fixField(BufferType const type = bufCur) {
        auto grid = getBuffer(type);
        auto const paddingLen = bufferPaddingLength();
        auto const gridLen = gridLength();

        std::memcpy(
            &grid[paddingLen - rowLength],
            &(grid[paddingLen + gridLen - rowLength]),
            rowLength * cellsBatchSize
        ); //start padding

        std::memcpy(
            &grid[paddingLen + gridLen],
            &(grid[paddingLen]),
            rowLength * cellsBatchSize
        ); //end padding

        if (edgeCellsOptimization) {
            const int32_t firstEmptyCellInColIndex_int = virtualWidth / cellsBatchLength;
            const int32_t firstEmptyCellIndexInBatch = virtualWidth % cellsBatchLength;

            //one batch before first row
            {
                uint32_t& cells = grid[0];
                uint32_t& cells_nextRow = grid[rowLength];

                const uint32_t lastCell = (cells_nextRow >> (firstEmptyCellIndexInBatch - 1)) & 1;
                const uint32_t lastCellCopy_bit = lastCell << (cellsBatchLength - 1); //last cell on next row -copy> lastBit

                cells = lastCellCopy_bit;
            }

            for (int32_t row = -1; row < height; row++) {
                const int32_t row_int = row * rowLength;
                const int32_t index_int = row_int + firstEmptyCellInColIndex_int;

                uint32_t& cells = getCellsActual_int(index_int, type);
                uint32_t& cells_nextRow = getCellsActual_int(index_int + rowLength, type);
                uint32_t& cells_rowStart = getCellsActual_int(row_int, type);

                const uint32_t lastCell = (cells_nextRow >> (firstEmptyCellIndexInBatch - 1)) & 1;
                const uint32_t lastCellCopy_bit = lastCell << (cellsBatchLength - 1); //last cell on next row -copy> lastBit

                const uint32_t firstCell = cells_rowStart & 1;
                const uint32_t firstCellCopy_bit = firstCell << (firstEmptyCellIndexInBatch); //first cell -copy> firstEmptyBit (after last cell original)

                cells = (cells & (~0u >> (cellsBatchLength - firstEmptyCellIndexInBatch))) | firstCellCopy_bit | lastCellCopy_bit;
            }

            //last row
            {
                const int32_t row_int = height * rowLength;
                const int32_t index_int = row_int + firstEmptyCellInColIndex_int;

                uint32_t& cells = getCellsActual_int(index_int, type);
                uint32_t& cells_rowStart = getCellsActual_int(row_int, type);

                const uint32_t firstCell = cells_rowStart & 1;
                const uint32_t firstCellCopy_bit = firstCell << (firstEmptyCellIndexInBatch); //first cell -copy> firstEmptyBit (after last cell original)

                cells = (cells & (~0u >> (cellsBatchLength - firstEmptyCellIndexInBatch))) | firstCellCopy_bit;
            }
        }
    }

    void swapBuffers() { currentBuf.swap(nextBuf); }

    void fill(FieldCell const cell, BufferType const type = bufCur) {
        auto grid = getBuffer(type);
        const auto val = ~0u * cell;
        std::fill(&grid[0], &grid[0] + bufferLength(), val);
    }

    uint8_t cellAt_grid(int32_t const index, BufferType const type = bufCur) const {
        auto grid = getBuffer(type);
        const auto row = misc::intDivFloor(index, virtualWidth);
        const auto col = misc::mod(index, virtualWidth);

        const auto col_int = col / cellsBatchLength;
        const auto shift = col % cellsBatchLength;

        return (grid[bufferPaddingLength() + row * rowLength + col_int] >> shift) & 0b1;
    }

    uint8_t cellAt_actual(int32_t const index, BufferType const type = bufCur) const {
        auto grid = getBuffer(type);

        const auto offset = misc::intDivFloor(index, int32_t(cellsBatchLength));
        const auto shift = misc::mod(index, cellsBatchLength);

        return (grid[bufferPaddingLength() + offset] >> shift) & 0b1;
    }

    FieldCell cellAt(int32_t const index, BufferType const type = bufCur) const {
        return cellAt_grid(index, type);
    }

    void setCellAt(int32_t const index, FieldCell const cell, BufferType const type = bufCur) {
        auto grid = getBuffer(type);

        const auto row = misc::intDivFloor(index, int32_t(virtualWidth));
        const auto col = misc::mod(index, virtualWidth);

        const auto col_int = col / cellsBatchLength;
        const auto shift = col % cellsBatchLength;

        auto& cur{ grid[bufferPaddingLength() + row * rowLength + col_int] };

        cur = (cur & ~(0b1u << shift)) | (static_cast<uint32_t>(cell) << shift);
    }

    uint32_t& getCellsInt(int32_t const index, BufferType const type = bufCur) const {
        auto grid = getBuffer(type);
        const auto row = misc::intDivFloor(index, int32_t(virtualWidth));
        const auto col = misc::mod(index, virtualWidth);

        const auto col_int = col / cellsBatchLength;

        return grid[bufferPaddingLength() + row * rowLength + col_int];
    }

    uint32_t& getCellsActual_int(int32_t const index_actual_int, BufferType const type = bufCur) const {
        return getBuffer(type)[index_actual_int + bufferPaddingLength()];
    }

    uint32_t indexAsActualInt(const uint32_t index) const {
        const auto row = misc::intDivFloor(index, virtualWidth);
        const auto col = misc::mod(index, virtualWidth);

        const auto col_int = col / cellsBatchLength;

        return row * rowLength + col_int;
    }

    constexpr bool isFirstCol_Index_actual_int(const int32_t index_actual_int) const {
        const auto col = misc::mod(index_actual_int, rowLength);
        return col == 0;
    }
    constexpr bool isLastCol_Index_actual_int(int32_t const index_actual_int) const {
        const auto col = misc::mod(index_actual_int, rowLength);
        return col == (rowLength - 1);
    }

    constexpr int32_t index_actual_int_asRow(int32_t const index_actual_int) const {
        return misc::intDivFloor(index_actual_int, rowLength);
    }

    constexpr int32_t indexActualIntAsActual(int32_t const index_actual_int) const {
        return index_actual_int * int32_t(cellsBatchLength);
    }

    constexpr int32_t index_actual_intAsindex(int32_t const index_actual_int) const {
        const int32_t index_actual = indexActualIntAsActual(index_actual_int);
        const auto row = misc::intDivFloor(index_actual, rowLength);
        const auto col = misc::mod(index_actual, rowLength);
        //assert(index_actual < width_int);

        return row * virtualWidth + col;
    }

    Cells *getBuffer(BufferType const type) const {
        if(type == bufCur) return currentBuf.get();
        else return nextBuf.get();
    }
    uint32_t gridLength() const {
        return height * rowLength;
    }
    uint32_t bufferPaddingLength() const {
        return rowLength + 1/*
            extra row before/after the grid repeating the opposite row 
            and 1 cell on each side for the first/last cell's neighbour
        */;
    }
    uint32_t bufferLength() const {
        return gridLength() + 2*bufferPaddingLength();
    }
};

struct Field::GridData {
private: static const uint32_t samples = 100;
public:
    UMedianCounter gridUpdate{ samples }, bufferSend{ samples };
    uint32_t task__iteration;
    uint32_t task__index;

    std::unique_ptr<FieldPimpl>& grid;
    std::atomic_bool& interrupt_flag;
    uint32_t startBatch;
    uint32_t endBatch;
    std::unique_ptr<FieldOutput> const buffer_output;

    void generationUpdated() {
        task__iteration++;
        if (task__iteration % (samples + task__index) == 0)
            std::cout << "grid task " << task__index << ": "
            << gridUpdate.median() << ' '
            << bufferSend.median() << std::endl;
    }

public:
    GridData(
        uint32_t index_,
        std::unique_ptr<FieldPimpl>& grid_,
        std::atomic_bool& interrupt_flag_,
        uint32_t startBatch_,
        uint32_t endBatch_,
        std::unique_ptr<FieldOutput> &&output_
    ) :
        task__iteration{ 0 },
        task__index(index_),
        grid(grid_),
        interrupt_flag(interrupt_flag_),
        startBatch(startBatch_),
        endBatch  (endBatch_),
        buffer_output{ std::move(output_) }
    {}
};

struct Remainder {
    uint16_t cellsCols;
    uint8_t curCell;
};
inline static uint32_t newGenerationBatched(
    Field::FieldPimpl& field,
    const Remainder previousRemainder,
    uint32_t index_actual_batch,
    Remainder& currentRemainder_out
) {
    const auto put32At4Bits = [](const uint32_t number) -> __m128i {
        const auto extract16andMask = [](
            const __m128i num, const __m128i indeces, const uint8_t bitMask
            ) -> __m128i {
            const auto low_high_bytesToLong = [](const uint8_t b1, const uint8_t b2, const  uint8_t b3, const uint8_t b4,
                const uint8_t b5, const uint8_t b6, const  uint8_t b7, const uint8_t b8) -> uint64_t {
                    return
                        (uint64_t(b1) << 8 * 0)
                        | (uint64_t(b2) << 8 * 1)
                        | (uint64_t(b3) << 8 * 2)
                        | (uint64_t(b4) << 8 * 3)
                        | (uint64_t(b5) << 8 * 4)
                        | (uint64_t(b6) << 8 * 5)
                        | (uint64_t(b7) << 8 * 6)
                        | (uint64_t(b8) << 8 * 7);
            };
            const __m128i mask = _mm_set1_epi64x(low_high_bytesToLong(1, 2, 4, 8, 16, 32, 64, 128));
            return
                _mm_and_si128(
                    _mm_cmpeq_epi8(
                        _mm_and_si128(
                            _mm_shuffle_epi8(
                                num,
                                indeces
                            ),
                            mask
                        ),
                        mask
                    ),
                    _mm_set1_epi8(bitMask));
        };

        const __m128i indecesLower = _mm_slli_si128(_mm_set1_epi8(1), 8); // lower->higher: 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 
        const __m128i indecesHigher = _mm_add_epi8(indecesLower, _mm_set1_epi8(2));

        const __m128i num = _mm_set1_epi32(number); /* const __m128i num = _mm_castps_si128(_mm_load1_ps((float*)ptr_to_number)) is slower */

        const __m128i resultlower4 = extract16andMask(num, indecesLower, 0b1u);
        const __m128i resultHigher4 = extract16andMask(num, indecesHigher, 0b1u << 4);

        return _mm_or_si128(resultlower4, resultHigher4);
    };

    int32_t width_int = field.rowLength;

    const uint32_t topCellRow_uint_{ field.getCellsActual_int(index_actual_batch - width_int) };
    const uint32_t curCellRow_uint_{ field.getCellsActual_int(index_actual_batch) };
    const uint32_t botCellRow_uint_{ field.getCellsActual_int(index_actual_batch + width_int) };

    const __m128i topCellRow_ = put32At4Bits(topCellRow_uint_);
    const __m128i curCellRow_ = put32At4Bits(curCellRow_uint_);
    const __m128i botCellRow_ = put32At4Bits(botCellRow_uint_);

    const __m128i cellsInCols_ = _mm_add_epi8(topCellRow_, _mm_add_epi8(curCellRow_, botCellRow_)); /* _mm_add_epi8(_mm_add_epi8(topCellRow_, curCellRow_), botCellRow_) is slower */

    currentRemainder_out.curCell = (_mm_extract_epi8(curCellRow_, 15) & 0b11110000) >> 4;
    currentRemainder_out.cellsCols = (_mm_extract_epi16(cellsInCols_, 7) & 0b11110000'11110000) >> 4;


    __m128i const mask = _mm_set1_epi8(0b00001111u);
    auto const cellsInCols__carry = _mm_slli_epi16(_mm_and_si128(cellsInCols_, mask), 4);

    const __m128i cellsInCols_sl1_ = _mm_alignr_epi8(cellsInCols_, cellsInCols__carry, 15);
    const __m128i cellsInCols_sl2_ = _mm_alignr_epi8(cellsInCols_, cellsInCols__carry, 14);
    const __m128i cells3by3 =
        _mm_add_epi8(
            _mm_add_epi8(
                _mm_add_epi8(
                    cellsInCols_sl2_,
                    cellsInCols_sl1_
                ),
                cellsInCols_
            ),
            _mm_srli_si128(
                _mm_set1_epi16(previousRemainder.cellsCols + (previousRemainder.cellsCols >> 8))
                , 14
            )/* _mm_cvtsi32_si128(previousRemainder.cellsCols + (previousRemainder.cellsCols >> 8)) is slower */
        );

    __m128i const curCellRow__carry = _mm_slli_epi16(_mm_and_si128(curCellRow_, mask), 4);
    __m128i const curCellRow = _mm_or_si128(_mm_alignr_epi8(curCellRow_, curCellRow__carry, 15), _mm_srli_si128(_mm_set1_epi8(previousRemainder.curCell), 15));
    /* using _mm_or_si128 instead of _mm_add_epi8 is safe because previousRemainder.curCell is in lower 4 bits and shifted curCellRow_ is in higher 4 */

    __m128i const cellsNeighboursAlive = _mm_sub_epi8(cells3by3, curCellRow);

    __m128i const cells = _mm_or_si128(cellsNeighboursAlive, curCellRow);
    static_assert(
        (2 | 1) == 3 &&
        (3 | 1) == 3 &&
        (3 | 0) == 3 &&
        true,
        R"( must be true:
                1, 2) (2 or 3 cells) | (alive cell) == 3
                3) (3 cells) | (dead  cell) == 3
                4) other combitations != 3
                )"
        );

    __m128i const mask_lower = _mm_set1_epi8(0b1111u);
    __m128i const three = _mm_set1_epi8(3u);

    uint32_t const lower16{ static_cast<std::make_unsigned<int>::type>(
            _mm_movemask_epi8(
                _mm_cmpeq_epi8(
                    _mm_and_si128(mask_lower, cells),
                    three
                )
            )
        )
    };

    uint32_t const higher16{ static_cast<std::make_unsigned<int>::type>(
            _mm_movemask_epi8(
                _mm_cmpeq_epi8(
                    _mm_andnot_si128(mask_lower, cells),
                    _mm_slli_epi16(three, 4)
                )
            )
        )
    };

    return lower16 | (higher16 << 16);
}

static void threadUpdateGrid(Field::GridData& data) {
    Timer<> t{};
    auto& grid = *data.grid.get();
    int32_t const width_grid = static_cast<int32_t>(grid.virtualWidth);
    int32_t const width_actual = static_cast<int32_t>(grid.rowLength * cellsBatchLength);
    int32_t const width_int = static_cast<int32_t>(grid.rowLength);


    const auto setBufferCellAt = [&grid](uint32_t index, FieldCell cell) -> void { grid.setCellAt(index, cell, Field::FieldPimpl::bufNext); };

    const auto isCell_actual = [&grid](int32_t index) -> uint8_t {
        return grid.cellAt_actual(index);
    };


    int32_t const startBatch = data.startBatch;
    int32_t const endBatch = data.endBatch;
    int32_t const startIndex = startBatch * cellsBatchLength;
    int32_t const endIndex = endBatch * cellsBatchLength; //may be out of bounds padding required

    int32_t i = startBatch;

    int32_t i_actual_grid = i * int32_t(cellsBatchLength); 
     

    Remainder previousRemainder{
        uint16_t(
            isCell_actual(i_actual_grid - 2 - width_actual)
            + isCell_actual(i_actual_grid - 2 + 0)
            + isCell_actual(i_actual_grid - 2 + width_actual)
            + ((
                isCell_actual(i_actual_grid - 1 - width_actual)
                + isCell_actual(i_actual_grid - 1 + 0)
                + isCell_actual(i_actual_grid - 1 + width_actual)
            ) << 8)
        ),
        isCell_actual(i_actual_grid - 1)
    }; //out of bounds, paddint of width + 1 required

    uint64_t newGenWindow = 0;

    if (i < endBatch + 1) {
        const uint32_t newGen = newGenerationBatched(grid, previousRemainder, i, previousRemainder/*out param*/);

        newGenWindow = (uint64_t(newGen) << 31);

        ++i;
    }

    for (auto const j_count = 32; (i + j_count) < endBatch + 1;) {
        for (uint32_t j = 0; j < j_count; ++j, ++i) {
            const uint32_t newGen = newGenerationBatched(grid, previousRemainder, i, previousRemainder/*out param*/);

            newGenWindow = (newGenWindow >> 32) | (uint64_t(newGen) << 31);
            grid.getCellsActual_int(i - 1, Field::FieldPimpl::bufNext) = uint32_t(newGenWindow);

            //_mm_stream_si32((int*)&grid.getCellsActual_int<Field::FieldPimpl::buffer>(i_batch - 1), (int)uint32_t(newGenWindow));
        }

        if (data.interrupt_flag.load()) return;
    }

    for (; i < endBatch + 1; ++i) {
        const uint32_t newGen = newGenerationBatched(grid, previousRemainder, i, previousRemainder/*out param*/);

        newGenWindow = (newGenWindow >> 32) | (uint64_t(newGen) << 31);
        grid.getCellsActual_int(i - 1, Field::FieldPimpl::bufNext) = uint32_t(newGenWindow);
    }

    if (data.interrupt_flag.load()) return;


    if (grid.edgeCellsOptimization == false) {
        uint32_t const height = grid.height;
        int32_t  const lastElement = width_grid - 1;

        const uint32_t startRow = startIndex / width_actual;
        const uint32_t endRow = misc::min<uint32_t>(misc::intDivCeil(endIndex + 1, width_actual), height);

        const auto isCell = [&grid](int32_t index) -> uint8_t {
            return grid.cellAt_grid(index);
        };
        const auto cellAt = [&grid](int32_t index) -> FieldCell {
            return grid.cellAt(index);
        };

        {
            const int32_t row = startRow * width_grid;

            uint8_t //top/cur/bot + first/second/last
                tf = isCell(row + -width_grid),
                ts = isCell(row + -width_grid + 1),
                tl = isCell(row + -width_grid + lastElement),
                cf = isCell(row + 0),
                cs = isCell(row + 1),
                cl = isCell(row + lastElement);

            for (uint32_t rowIndex = startRow; rowIndex < endRow; rowIndex++) {
                const int32_t row = rowIndex * width_grid;

                uint8_t
                    bf = isCell(row + width_grid),
                    bs = isCell(row + width_grid + 1),
                    bl = isCell(row + width_grid + lastElement);
                //first element
                {
                    const int index = row;
                    const auto curCell = cellAt(index);
                    const unsigned char    topRowNeighbours = tf + ts + tl;
                    const unsigned char bottomRowNeighbours = bf + bs + bl;
                    const unsigned char aliveNeighbours = topRowNeighbours + cl + cs + bottomRowNeighbours;

                    setBufferCellAt(index, fieldCell::nextGeneration(curCell, aliveNeighbours));
                }

                tf = cf;
                ts = cs;
                tl = cl;
                cf = bf;
                cs = bs;
                cl = bl;
            }
        }
        if (data.interrupt_flag.load()) return;

        {
            const int32_t row = startRow * width_grid;

            uint8_t //top/cur/bot + first/last/pre-last
                tf = isCell(row + -width_grid),
                tl = isCell(row + -width_grid + lastElement),
                tp = isCell(row + -width_grid + lastElement - 1),
                cf = isCell(row + 0),
                cl = isCell(row + lastElement),
                cp = isCell(row + lastElement - 1);

            for (uint32_t rowIndex = startRow; rowIndex < endRow; rowIndex++) {
                const int32_t row = rowIndex * width_grid;

                uint8_t
                    bf = isCell(row + width_grid),
                    bp = isCell(row + width_grid + lastElement - 1),
                    bl = isCell(row + width_grid + lastElement);

                { //last element
                    const int index = row + lastElement;
                    const auto curCell = cellAt(index);
                    const uint8_t    topRowNeighbours = tp + tl + tf;
                    const uint8_t bottomRowNeighbours = bp + bl + bf;
                    const uint8_t     aliveNeighbours = topRowNeighbours + cp + cf + bottomRowNeighbours;

                    setBufferCellAt(index, fieldCell::nextGeneration(curCell, aliveNeighbours));
                }

                tf = cf;
                tp = cp;
                tl = cl;
                cf = bf;
                cp = bp;
                cl = bl;
            }
        }
        if (data.interrupt_flag.load()) return;
    }

    data.gridUpdate.add(t.elapsedTime());

    Timer<> t2{};
     
    data.buffer_output->write(FieldModification{ data.startBatch, data.endBatch - data.startBatch, &grid.getCellsActual_int(startBatch, Field::FieldPimpl::bufNext) });

    data.bufferSend.add(t2.elapsedTime());

    const uint32_t j = 1 << (data.task__iteration % (((grid.virtualWidth-1) % 32) + 1));
    const uint32_t zero = 0;
    if(false) {
        auto output = data.buffer_output->batched();
        output->write(FieldModification{ 0, 1, &zero });
        output->write(FieldModification{ 1, 1, &j });
        output->write(FieldModification{ 2, 1, &zero });
    }

    data.generationUpdated();
}

uint32_t Field::width_actual() const {
    return gridPimpl->rowLength * cellsBatchLength;
}

uint32_t Field::size_bytes() const {
    return gridPimpl->gridLength() * cellsBatchSize;
}

uint32_t* Field::rawData() const {
    return &this->gridPimpl->getCellsActual_int(0);
}


Field::Field(
    const uint32_t gridWidth, const uint32_t gridHeight, const size_t numberOfTasks_,
    std::function<std::unique_ptr<FieldOutput>()> current_outputs, 
    std::function<std::unique_ptr<FieldOutput>()> buffer_outputs
) :
    gridPimpl{ new FieldPimpl(gridWidth, gridHeight) },
    isStopped{ false },
    current_output{ current_outputs() },
    buffer_output{ buffer_outputs() },
    numberOfTasks(numberOfTasks_),
    gridTasks{ new std::unique_ptr<Task<GridData>>[numberOfTasks_] },
    interrupt_flag{ false },
    indecesToBrokenCells{ }
{
    assert(numberOfTasks >= 1);

    //grid update == 50 ms, buffer send == 4 ms.In my case
    //4 / 50 ~= 1 / 20 
    const double fraction = .05; // fractionOfWorkTimeToSendData
    static_assert(true, ""/*
    find amountOfWorkT1. given fraction, workload (total batches)
    amountOfWorkT1 - percent of work;

    timeForTask1 = amountOfWorkT1 + amountOfWorkT1 * fraction;
    timeForTask2 = timeForTask1 + timeForTask1 * fraction;
    timeForTask3 = timeForTask2 + timeForTask2 * fraction;
    ...

                                                                v  total time
    timeForTask1 + timeForTask2 + timeForTask3 + ... = workload * (1.0 + fraction);
    amountOfWorkT1 * (1.0 + fraction)
        + (amountOfWorkT1 * (1.0 + fraction)) * (1.0  + fraction)
        + ...                                                       = workload * (1.0 + fraction);
    amountOfWorkT1 * (1.0 + fraction) + amountOfWorkT1 * (1.0  + fraction)^2 + ...  + amountOfWorkT1 * (1.0  + fraction)^numberOfTasks = workload * (1.0 + fraction);

    amountOfWorkT1 * ((1.0 + fraction) + (1.0  + fraction)^2 + ... + (1.0  + fraction)^numberOfTasks) = workload * (1.0 + fraction);

    amountOfWorkT1 = workload / ( ( (1.0 + fraction)^numberOfTasks - 1 ) / fraction );
    amountOfWorkT1 = workload /   ( (1.0 + fraction)^numberOfTasks - 1 ) * fraction  ;
    */);

    const auto createGridTask = [this, &buffer_outputs](const uint32_t index, const uint32_t startBatch, const uint32_t endBatch) -> void {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

        gridTasks.get()[index] = std::unique_ptr<Task<GridData>>(
            new Task<GridData>{
                threadUpdateGrid,
                
                index,
                this->gridPimpl,
                this->interrupt_flag,
                startBatch,
                endBatch,
                buffer_outputs() //getting output    
            }
        );
    };

    uint32_t batchesBefore = 0;
    uint32_t remainingBatches = gridPimpl->gridLength();

    const double amountOfWorkT1 = remainingBatches / (pow((1.0 + fraction), numberOfTasks) - 1) * fraction;

    double currentTaskWork = amountOfWorkT1;

    for (uint32_t i = 0; i < numberOfTasks - 1; i++) {
        const uint32_t numberOfBatches = misc::min(remainingBatches, static_cast<uint32_t>(ceil(currentTaskWork)));
        ::std::cout << numberOfBatches << std::endl;

        createGridTask(i, batchesBefore, batchesBefore + numberOfBatches);

        batchesBefore += numberOfBatches;
        remainingBatches -= numberOfBatches;

        currentTaskWork *= (1.0 + fraction);
    }

    {//last
        const uint32_t numberOfBatches = remainingBatches;
        ::std::cout << numberOfBatches << std::endl;

        createGridTask(numberOfTasks - 1, batchesBefore, batchesBefore + numberOfBatches);

        batchesBefore += numberOfBatches;
        remainingBatches -= numberOfBatches;
    }
}


Field::~Field() = default;

void Field::fill(const FieldCell cell) {
    interrupt_flag.store(true);
    waitForGridTasks();
    interrupt_flag.store(false);

    gridPimpl->fill(cell);

    indecesToBrokenCells.clear();

    current_output->write(FieldModification{ 0, static_cast<uint32_t>(gridPimpl->gridLength()), &gridPimpl->getCellsActual_int(0) });
    

    deployGridTasks();
}

FieldCell Field::cellAtIndex(const uint32_t index) const {
    return gridPimpl->cellAt(normalizeIndex(index));
}

void Field::setCellAtIndex(const uint32_t index, FieldCell cell) {
    Cell const cell_{ cell, normalizeIndex(index) };
    setCells(&cell_, 1);
}

void Field::setCells(Cell const* const cells, size_t const count) {
    if (isStopped) {
        for (size_t i = 0; i < count; ++i) {
            auto const cell = cells[i];
            gridPimpl->setCellAt(normalizeIndex(cell.index), cell.cell);
        }
    }
    else {
        std::vector<uint32_t> cells_indeces_actual_int{};
        //std::vector<uint32_t> cells_int{};

        for (size_t i = 0; i < count; ++i) {
            auto const cell = cells[i];
            auto const index = normalizeIndex(cell.index);
            const auto index_actual_int{ gridPimpl->indexAsActualInt(index) };

            if (std::find(
                cells_indeces_actual_int.begin(), cells_indeces_actual_int.end(),
                index_actual_int
            ) == cells_indeces_actual_int.end()) {
                cells_indeces_actual_int.push_back(index_actual_int);
            }

            gridPimpl->setCellAt(index, cell.cell);
            indecesToBrokenCells.push_back(index);
        }

        for (uint32_t index_actual_int : cells_indeces_actual_int) {

            current_output->write(FieldModification{ index_actual_int, 1, &gridPimpl->getCellsActual_int(index_actual_int) });
        }
    }
}

static FieldCell updatedCell(const int32_t index, const std::unique_ptr<Field::FieldPimpl>& cellsGrid) {
    const auto width_grid = cellsGrid->virtualWidth;
    const auto height = cellsGrid->height;
    uint8_t cell = cellsGrid->cellAt_grid(index);

    uint32_t aliveNeighbours = 0;

    for (int yo = -1; yo <= 1; yo++) {
        for (int xo = -1; xo <= 1; xo++) {
            if (xo != 0 || yo != 0) {
                int x = ((index + xo) + width_grid) % width_grid,
                    y = (((index / width_grid) + yo) + height) % height;
                int offsetedIndex = x + width_grid * y;
                if (cellsGrid->cellAt_grid(offsetedIndex))
                    aliveNeighbours++;
            }
        }
    }

    return fieldCell::nextGeneration(cell, aliveNeighbours);
}

bool Field::tryFinishGeneration() {
    if(!isStopped) for(uint32_t i = 0; i < numberOfTasks; i++) {
        if(!gridTasks.get()[i]->resultReady()) return false;
    }
    interrupt_flag.store(false);

    if (indecesToBrokenCells.size() > 0) {
        std::vector<uint32_t> repairedCells_actual_int{};

        for (auto it = indecesToBrokenCells.begin(); it != indecesToBrokenCells.end(); it++) {
            const uint32_t index = *it;
            const vec2i coord = this->indexAsCoord(index);

            for (int yo = -1; yo <= 1; yo++) {
                for (int xo = -1; xo <= 1; xo++) {
                    //int x = ((index + xo) + width()) % width(),
                    //    y = (((index / width()) + yo) + height()) % height();
                    auto offsetedIndex = this->coordAsIndex(coord + vec2i(xo, yo));//this->normalizeIndex(index + xo + gridPimpl->width_grid * yo);//x + width() * y;

                    const auto index_actual_int{ gridPimpl->indexAsActualInt(offsetedIndex) };

                    if (
                        std::find(
                            repairedCells_actual_int.begin(), repairedCells_actual_int.end(), 
                            index_actual_int
                        ) == repairedCells_actual_int.end()
                    ) repairedCells_actual_int.push_back(index_actual_int);
                }
            }
        }

        gridPimpl->fixField();

        std::unique_ptr<FieldOutput> output = buffer_output->batched();
        auto& field = *this->gridPimpl.get();
        int32_t width_actual = field.rowLength * cellsBatchLength;
        int32_t width_grid = field.virtualWidth;
        for (uint32_t index_actual_int : repairedCells_actual_int) {
            bool const isEdgeOpt = gridPimpl->edgeCellsOptimization;
            auto const specialFirstCell = !isEdgeOpt && gridPimpl->isFirstCol_Index_actual_int(index_actual_int);
            auto const specialLastCell = !isEdgeOpt && gridPimpl->isLastCol_Index_actual_int(index_actual_int);

            auto previousRemainder = ([&]() -> Remainder {
                    if(specialFirstCell) return {} /* we don't need to fill the remainder as it affects only the first two cells in the batch, one of them is from the previous row, and the second one is recalculated later*/;
                    else {
                        int32_t index_actual = field.indexActualIntAsActual(index_actual_int);
                        return {
                            static_cast<uint16_t>(
                                (field.cellAt_actual(index_actual - 2 - width_actual)
                                + field.cellAt_actual(index_actual - 2 + 0)
                                + field.cellAt_actual(index_actual - 2 + width_actual)
                                ) + (
                                    (field.cellAt_actual(index_actual - 1 - width_actual)
                                    + field.cellAt_actual(index_actual - 1 + 0)
                                    + field.cellAt_actual(index_actual - 1 + width_actual)
                                    ) << 8
                                )
                            ),
                            field.cellAt_actual(index_actual - 1)
                        };
                    }
            })();

            uint64_t newGenerationWindow = newGenerationBatched(*gridPimpl.get(), previousRemainder, index_actual_int, previousRemainder);

            if(specialLastCell) {
                newGenerationWindow = (newGenerationWindow >> 1);
            }
            else {
                newGenerationWindow =
                    (newGenerationWindow >> 1) |
                    (uint64_t(newGenerationBatched(*gridPimpl.get(), previousRemainder, index_actual_int + 1, previousRemainder)) << (cellsBatchLength - 1));
            }

            uint32_t newGeneration = static_cast<uint32_t>(newGenerationWindow);

            if(specialFirstCell) {
                newGeneration = (newGeneration & ~uint32_t(1)) | (updatedCell(field.index_actual_int_asRow(index_actual_int) * width_grid, gridPimpl));
            }

            if(specialLastCell) {
                const auto lastCellCol = misc::mod(field.virtualWidth-1, cellsBatchLength);
                newGeneration = (newGeneration & ~(uint32_t(1) << lastCellCol)) 
                    | (uint32_t(updatedCell(field.index_actual_int_asRow(index_actual_int) * width_grid + field.virtualWidth - 1, gridPimpl)) << lastCellCol);
            }


            gridPimpl->getCellsActual_int(index_actual_int, Field::FieldPimpl::bufNext) = newGeneration;
            output->write(FieldModification{ index_actual_int, 1, &newGeneration });
        }
        indecesToBrokenCells.clear();
    }

    return true;
}

void Field::startNewGeneration() {
    gridPimpl->swapBuffers();
    startCurGeneration();
}

void Field::startCurGeneration() {
    gridPimpl->fixField();
    isStopped = false;
    deployGridTasks();
}

void Field::waitForGridTasks() {
    if (isStopped) return;
    for (uint32_t i = 0; i < numberOfTasks; i++) {
        gridTasks.get()[i]->waitForResult();
    }
}
void Field::deployGridTasks() {
    if(isStopped) {
        std::cerr << "trying to start task when `isStopped` is set\n";
        return;
    }
    for(uint32_t i = 0; i < numberOfTasks; i++) {
        gridTasks.get()[i]->start();
    }
}

uint32_t Field::width() const {
    return gridPimpl->virtualWidth;
}
uint32_t Field::height() const {
    return gridPimpl->height;
}
uint32_t Field::size() const {
    return gridPimpl->virtualWidth * gridPimpl->height;
}
