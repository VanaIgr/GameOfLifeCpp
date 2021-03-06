#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include "Misc.h"
#include "Grid.h"
#include <vector>

#include <cassert> 

#include"Timer.h"
#include"AutoTimer.h"
#include"MedianCounter.h"

#include <intrin.h>

#include<algorithm>

static const size_t sizeOfUint32 = sizeof uint32_t;
constexpr uint32_t batchSize = 32;// sizeof(__m128i); //dependent on CellsGrid fullSize

bool fieldCell::isAlive(const FieldCell cell) {
	return ((misc::to_underlying(cell)) & 0b1);
}
bool fieldCell::isDead(const FieldCell cell) {
	return !isAlive(cell);
}

FieldCell fieldCell::nextGeneration(const FieldCell cell, const uint32_t aliveNeighboursCount) {
	if (fieldCell::isDead(cell) && aliveNeighboursCount == 3) {
		return FieldCell::ALIVE;
	}
	else if (fieldCell::isAlive(cell) && (aliveNeighboursCount < 2 || aliveNeighboursCount > 3)) {
		return FieldCell::DEAD;
	}
	return cell;
}


 class Field::FieldPimpl {
 public:
	 struct current {};
	 struct buffer  {};
public:
	const int32_t width_grid;
	const int32_t width_actual;
	const int32_t width_int;
	const int32_t height;
	const int32_t size_grid;
	const int32_t size_actual;
	const int32_t size_int;
	const bool edgeCellsOptimization;
private:
	const int32_t startPadding_int;
	const int32_t endPadding_int;
	const int32_t fullSize_int;

	std::unique_ptr<uint32_t[]> current_; 
	std::unique_ptr<uint32_t[]> buffer_;

public:
	FieldPimpl(const int32_t gridWidth, const int32_t gridHeight) :
		width_grid{ gridWidth },
		width_actual{ static_cast<int32_t>(misc::roundUpIntTo(width_grid, batchSize)) },
		width_int{ static_cast<int32_t>(misc::intDivCeil(width_actual, batchSize)) },

		height{ gridHeight },

		size_grid(width_grid * height),
		size_actual{ width_actual * height },
		size_int{ static_cast<int32_t>(misc::intDivCeil(width_actual * height, batchSize)) },

		edgeCellsOptimization{ (width_actual - width_grid) >= 2 },

		startPadding_int{ width_int + (edgeCellsOptimization ? 1 : 0) }, /*
			top row duplicate before the first row
			+ one batch for cell index 0 neighbours
		*/
		endPadding_int{ static_cast<int32_t>(misc::intDivCeil(width_grid, batchSize) + 1) }, /*
			bottom row duplicate after the last row
			+ extra bytes for
				memcopy/vector instructions and
				left/right boundary cells update
			all in 'void threadUpdateGrid(std::unique_ptr<Field::GridData>&)'
		*/
		fullSize_int{ startPadding_int + size_int + endPadding_int },
		current_{ new uint32_t[fullSize_int]{ } },
		buffer_{ new uint32_t[fullSize_int]{ } }
	{ };
	~FieldPimpl() = default;

	template<typename gridType = current>
	void fixField() {
		auto* grid{ getGrid<gridType>() };

		std::memcpy(
			&grid[startPadding_int - width_int],
			&(grid[startPadding_int + size_int - width_int]),
			width_int * sizeOfUint32); //start pading

		std::memcpy(
			&grid[startPadding_int + size_int],
			&(grid[startPadding_int]),
			width_int * sizeOfUint32); //end pading

		if (edgeCellsOptimization) {
			const int32_t firstEmptyCellInColIndex_int = width_grid / batchSize;
			const int32_t firstEmptyCellIndexInBatch = width_grid % batchSize;

			//one batch beforw first row
			{
				uint32_t& cells = grid[0];
				uint32_t& cells_nextRow = grid[width_int];

				const uint32_t lastCell = (cells_nextRow >> (firstEmptyCellIndexInBatch - 1)) & 1;
				const uint32_t lastCellCopy_bit = lastCell << (batchSize - 1); //last cell on next row -copy> lastBit

				cells = lastCellCopy_bit;
			}

			for (int32_t row = -1; row < height; row++) {
				const int32_t row_int = row * width_int;
				const int32_t index_int = row_int + firstEmptyCellInColIndex_int;

				uint32_t& cells = getCellsActual_int<gridType>(index_int);
				uint32_t& cells_nextRow = getCellsActual_int<gridType>(index_int + width_int);
				uint32_t& cells_rowStart = getCellsActual_int<gridType>(row_int);

				const uint32_t lastCell = (cells_nextRow >> (firstEmptyCellIndexInBatch - 1)) & 1;
				const uint32_t lastCellCopy_bit = lastCell << (batchSize - 1); //last cell on next row -copy> lastBit

				const uint32_t firstCell = cells_rowStart & 1;
				const uint32_t firstCellCopy_bit = firstCell << (firstEmptyCellIndexInBatch); //first cell -copy> firstEmptyBit (after last cell original)

				cells = (cells & (~0u >> (batchSize - firstEmptyCellIndexInBatch))) | firstCellCopy_bit | lastCellCopy_bit;
			}

			//last row
			{
				const int32_t row_int = height * width_int;
				const int32_t index_int = row_int + firstEmptyCellInColIndex_int;

				uint32_t& cells = getCellsActual_int<gridType>(index_int);
				uint32_t& cells_rowStart = getCellsActual_int<gridType>(row_int);

				const uint32_t firstCell = cells_rowStart & 1;
				const uint32_t firstCellCopy_bit = firstCell << (firstEmptyCellIndexInBatch); //first cell -copy> firstEmptyBit (after last cell original)

				cells = (cells & (~0u >> (batchSize - firstEmptyCellIndexInBatch))) | firstCellCopy_bit;
			}
		}
	}


	void prepareNext() {
		fixField<buffer>();

		current_.swap(buffer_);
	}

	template<typename gridType = current>
	void fill(FieldCell const cell) {
		auto* grid{ getGrid<gridType>() };
		const auto val = ~0u * fieldCell::isAlive(cell);
		std::fill(&grid[0], &grid[0] + fullSize_int, val);
	}

	template<typename gridType = current>
	uint8_t cellAt_grid(int32_t const index) const {
		auto* grid{ getGrid<gridType>() };
		const auto row = misc::intDivFloor(index, width_grid);
		const auto col = misc::mod(index, width_grid);

		const auto col_int = col / batchSize;
		const auto shift = col % batchSize;

		return (grid[startPadding_int + row * width_int + col_int] >> shift) & 0b1;
	}

	template<typename gridType = current>
	uint8_t cellAt_actual(int32_t const index) const {
		auto* grid{ getGrid<gridType>() };

		const auto offset = misc::intDivFloor(index, int32_t(width_actual));
		const auto shift = misc::mod(index, batchSize);

		return (grid[startPadding_int + offset] >> shift) & 0b1;
	}

	template<typename gridType = current>
	FieldCell cellAt(int32_t const index) const {
		return cellAt_grid<gridType>(index) ? FieldCell::ALIVE : FieldCell::DEAD;
	}

	template<typename gridType = current>
	void setCellAt(int32_t const index, FieldCell const cell) {
		auto* grid{ getGrid<gridType>() };

		const auto row = misc::intDivFloor(index, int32_t(width_grid));
		const auto col = misc::mod(index, width_grid);

		const auto col_int = col / batchSize;
		const auto shift = col % batchSize;

		auto& cur{ grid[startPadding_int + row * width_int + col_int] };

		cur = (cur & ~(0b1u << shift)) | (static_cast<uint32_t>(fieldCell::isAlive(cell)) << shift);
	}

	template<typename gridType = current>
	uint32_t& getCellsInt(int32_t const index) const {
		auto* grid{ getGrid<gridType>() };
		const auto row = misc::intDivFloor(index, int32_t(width_grid));
		const auto col = misc::mod(index, width_grid);

		const auto col_int = col / batchSize;

		return grid[startPadding_int + row * width_int + col_int];
	}

	template<typename gridType = current>
	uint32_t& getCellsActual_int(int32_t const index_actual_int) const {
		return getGrid<gridType>()[index_actual_int + startPadding_int];
	}

	uint32_t indexAsActualInt(const uint32_t index) const {
		const auto row = misc::intDivFloor(index, width_grid);
		const auto col = misc::mod(index, width_grid);

		const auto col_int = col / batchSize;

		return row * width_int + col_int;
	}

	constexpr bool isFirstCol_Index_actual_int(const int32_t index_actual_int) const {
		const auto col = misc::mod(index_actual_int, width_int);
		return col == 0;
	}
	constexpr bool isLastCol_Index_actual_int(int32_t const index_actual_int) const {
		const auto col = misc::mod(index_actual_int, width_int);
		return col == (width_int - 1);
	}

	constexpr int32_t index_actual_int_asRow(int32_t const index_actual_int) const {
		return misc::intDivFloor(index_actual_int, width_int);
	}

	constexpr int32_t indexActualIntAsActual(int32_t const index_actual_int) const {
		return index_actual_int * int32_t(batchSize);
	}

	constexpr int32_t index_actual_intAsindex(int32_t const index_actual_int) const {
		const int32_t index_actual = indexActualIntAsActual(index_actual_int);
		const auto row = misc::intDivFloor(index_actual, width_int);
		const auto col = misc::mod(index_actual, width_int);
		//assert(index_actual < width_int);

		return row * width_grid + col;
	}

 private:
	 template<typename gridType>
	 inline uint32_t* getGrid() const;

	 template<>
	 inline uint32_t* getGrid<current>() const {
		 return this->current_.get();
	 }

	 template<>
	 inline uint32_t* getGrid<buffer>() const {
		 return this->buffer_.get();
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

 static struct Remainder {
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

	 int32_t width_int = field.width_int;

	 const uint32_t topCellRow_uint_{ field.getCellsActual_int(index_actual_batch - width_int) };
	 const uint32_t curCellRow_uint_{ field.getCellsActual_int(index_actual_batch) };
	 const uint32_t botCellRow_uint_{ field.getCellsActual_int(index_actual_batch + width_int) };

	 const __m128i topCellRow_ = put32At4Bits(topCellRow_uint_);
	 const __m128i curCellRow_ = put32At4Bits(curCellRow_uint_);
	 const __m128i botCellRow_ = put32At4Bits(botCellRow_uint_);

	 const __m128i cellsInCols_ = _mm_add_epi8(topCellRow_, _mm_add_epi8(curCellRow_, botCellRow_)); /* _mm_add_epi8(_mm_add_epi8(topCellRow_, curCellRow_), botCellRow_) is slower */

	 currentRemainder_out.curCell = (curCellRow_.m128i_i8[15] & 0b11110000) >> 4;
	 currentRemainder_out.cellsCols = (cellsInCols_.m128i_i16[7] & 0b11110000'11110000) >> 4;


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
	 int32_t const width_grid = static_cast<int32_t>(grid.width_grid);
	 int32_t const width_actual = static_cast<int32_t>(grid.width_actual);
	 int32_t const width_int = static_cast<int32_t>(grid.width_int);


	 const auto setBufferCellAt = [&grid](uint32_t index, FieldCell cell) -> void { grid.setCellAt<Field::FieldPimpl::buffer>(index, cell); };

	 const auto isCell_actual = [&grid](int32_t index) -> uint8_t {
		 return grid.cellAt_actual(index);
	 };


	 int32_t const startBatch = data.startBatch;
	 int32_t const endBatch = data.endBatch;
	 int32_t const startIndex = startBatch * batchSize;
	 int32_t const endIndex = endBatch * batchSize; //may be out of bounds padding required

	 int32_t i = startBatch;

	 int32_t i_actual_grid = i * int32_t(batchSize); 
	 

	 Remainder previousRemainder{
		 isCell_actual(i_actual_grid - 2 - width_actual) +
		 isCell_actual(i_actual_grid - 2 + 0) +
		 isCell_actual(i_actual_grid - 2 + width_actual) +
		 (
			 uint16_t(
				 isCell_actual(i_actual_grid - 1 - width_actual) +
				 isCell_actual(i_actual_grid - 1 + 0) +
				 isCell_actual(i_actual_grid - 1 + width_actual))
			 << 8
			 ),
		 isCell_actual(i_actual_grid - 1)
	 }; //out of bounds, paddint of width + 1 required

	 uint64_t newGenWindow = 0;

	 if (i < endBatch + 1) {
		 const uint32_t newGen = newGenerationBatched(grid, previousRemainder, i, previousRemainder/*out param*/);

		 newGenWindow = (uint64_t(newGen) << 31);

		 ++i;
	 }

	 for (const uint32_t j_count = 32; (i + j_count) < endBatch + 1;) {
		 for (uint32_t j = 0; j < j_count; ++j, ++i) {
			 const uint32_t newGen = newGenerationBatched(grid, previousRemainder, i, previousRemainder/*out param*/);

			 newGenWindow = (newGenWindow >> 32) | (uint64_t(newGen) << 31);
			 grid.getCellsActual_int<Field::FieldPimpl::buffer>(i - 1) = uint32_t(newGenWindow);

			 //_mm_stream_si32((int*)&grid.getCellsActual_int<Field::FieldPimpl::buffer>(i_batch - 1), (int)uint32_t(newGenWindow));
		 }

		 if (data.interrupt_flag.load()) return;
	 }

	 for (; i < endBatch + 1; ++i) {
		 const uint32_t newGen = newGenerationBatched(grid, previousRemainder, i, previousRemainder/*out param*/);

		 newGenWindow = (newGenWindow >> 32) | (uint64_t(newGen) << 31);
		 grid.getCellsActual_int<Field::FieldPimpl::buffer>(i - 1) = uint32_t(newGenWindow);
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
	 
	 data.buffer_output->write(FieldModification{ data.startBatch, data.endBatch - data.startBatch, &grid.getCellsActual_int<Field::FieldPimpl::buffer>(startBatch) });

	 data.bufferSend.add(t2.elapsedTime());

	 const uint32_t j = 1 << (data.task__iteration % (((grid.width_grid-1) % 32) + 1));
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
	 return gridPimpl->width_actual;
 }

 uint32_t Field::size_bytes() const {
	return gridPimpl->size_int * sizeOfUint32;
}

 uint32_t Field::size_actual() const {
	 return gridPimpl->size_actual;
 }

uint32_t* Field::rawData() const {
	return &this->gridPimpl->getCellsActual_int(0);
}


Field::Field(const uint32_t gridWidth, const uint32_t gridHeight,
	const size_t numberOfTasks_, std::function<std::unique_ptr<FieldOutput>()> current_outputs, std::function<std::unique_ptr<FieldOutput>()> buffer_outputs, bool deployTasks
) :
gridPimpl{ new FieldPimpl(gridWidth, gridHeight) },
isStopped{ deployTasks == false },
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
	amountOfWorkT1 ? percent of work;

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
	uint32_t remainingBatches = gridPimpl->size_int;

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

	deployGridTasks();
}


Field::~Field() = default;

void Field::fill(const FieldCell cell) {
	interrupt_flag.store(true);
	waitForGridTasks();
	interrupt_flag.store(false);

	gridPimpl->fill(cell);

	indecesToBrokenCells.clear();

	current_output->write(FieldModification{ 0, static_cast<uint32_t>(gridPimpl->size_int), &gridPimpl->getCellsActual_int(0) });
	

	deployGridTasks();
}

FieldCell Field::cellAtIndex(const uint32_t index) const {
	return gridPimpl->cellAt<>(normalizeIndex(index));
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
	const auto width_grid = cellsGrid->width_grid;
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

	return fieldCell::nextGeneration(cell ? FieldCell::ALIVE : FieldCell::DEAD, aliveNeighbours);
}

void Field::finishGeneration() {
	waitForGridTasks();
	interrupt_flag.store(false);

	if (indecesToBrokenCells.size() > 0) {
		std::vector<uint32_t> repairedCells_actual_int{};

		for (auto it = indecesToBrokenCells.begin(); it != indecesToBrokenCells.end(); it++) {
			const uint32_t index = *it;
			const vec2i coord = this->indexAsCoord(index);

			for (int yo = -1; yo <= 1; yo++) {
				for (int xo = -1; xo <= 1; xo++) {
					//int x = ((index + xo) + width()) % width(),
					//	y = (((index / width()) + yo) + height()) % height();
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

		gridPimpl->fixField<FieldPimpl::current>();

		std::unique_ptr<FieldOutput> output = buffer_output->batched();
		auto& field = *this->gridPimpl.get();
		int32_t width_actual = field.width_actual;
		int32_t width_grid = field.width_grid;
		for (uint32_t index_actual_int : repairedCells_actual_int) {
			int32_t index_actual = field.indexActualIntAsActual(index_actual_int);
			bool const isEdgeOpt = gridPimpl->edgeCellsOptimization;

			Remainder previousRemainder;
			uint64_t newGenerationWindow;
			if (!isEdgeOpt && gridPimpl->isFirstCol_Index_actual_int(index_actual_int)) {
				previousRemainder = {};
			}
			else {
				previousRemainder = {
					static_cast<uint16_t>(
								(
									field.cellAt_actual(index_actual - 2 - width_actual) +
									field.cellAt_actual(index_actual - 2 + 0) +
									field.cellAt_actual(index_actual - 2 + width_actual)
								) +
								(static_cast<uint16_t>(
									 field.cellAt_actual(index_actual - 1 - width_actual) +
									 field.cellAt_actual(index_actual - 1 + 0) +
									 field.cellAt_actual(index_actual - 1 + width_actual))
								 << 8
								)
					),
								field.cellAt_actual(index_actual - 1)
				};
			}

			newGenerationWindow = newGenerationBatched(*gridPimpl.get(), previousRemainder, index_actual_int, previousRemainder);

			if (!isEdgeOpt && gridPimpl->isLastCol_Index_actual_int(index_actual_int)) {
				newGenerationWindow = (newGenerationWindow >> 1);
			}
			else {
				newGenerationWindow =
					(newGenerationWindow >> 1) |
					(uint64_t(newGenerationBatched(*gridPimpl.get(), previousRemainder, index_actual_int + 1, previousRemainder)) << (batchSize - 1));
			}

			uint32_t newGeneration = static_cast<uint32_t>(newGenerationWindow);

			if (!isEdgeOpt && gridPimpl->isFirstCol_Index_actual_int(index_actual_int)) {
				newGeneration = (newGeneration & ~uint32_t(1)) | (updatedCell(field.index_actual_int_asRow(index_actual_int) * width_grid, gridPimpl) == FieldCell::ALIVE);
			}
			if (!isEdgeOpt && gridPimpl->isLastCol_Index_actual_int(index_actual_int)) {
				newGeneration = 
					(newGeneration & (uint32_t(~0) >> 1)) | 
					(updatedCell(field.index_actual_int_asRow(index_actual_int) * width_grid + field.width_grid - 1, gridPimpl) == FieldCell::ALIVE ? ~(uint32_t(~0) >> 1) : 0);
			}


			gridPimpl->getCellsActual_int<Field::FieldPimpl::buffer>(index_actual_int) = newGeneration;
			output->write(FieldModification{ index_actual_int, 1, &newGeneration });
		}
		indecesToBrokenCells.clear();
	}
}

void Field::startNewGeneration() {
	gridPimpl->prepareNext();

	deployGridTasks();
}

void Field::waitForGridTasks() {
	if (isStopped) return;
	for (uint32_t i = 0; i < numberOfTasks; i++) {
		gridTasks.get()[i]->waitForResult();
	}
}
void Field::deployGridTasks() {
	if (isStopped) return;
	for (uint32_t i = 0; i < numberOfTasks; i++) {
		gridTasks.get()[i]->start();
	}
}

void Field::stopAllGridTasks() {
	interrupt_flag.store(true);
	for (uint32_t i = 0; i < numberOfTasks; i++) {
		gridTasks.get()[i]->waitForResult();
	}
	isStopped = true;
}

void Field::startAllGridTasks() {
	isStopped = false;
	interrupt_flag.store(false);
	for (uint32_t i = 0; i < numberOfTasks; i++) {
		gridTasks.get()[i]->start();
	}
}

uint32_t Field::width() const {
	return gridPimpl->width_grid;
}
uint32_t Field::height() const {
	return gridPimpl->height;
}
uint32_t Field::size() const {
	return gridPimpl->size_grid;
}