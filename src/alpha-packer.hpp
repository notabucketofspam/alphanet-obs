#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

inline constexpr uint32_t ALPHA_LED_COLS = 120;
inline constexpr uint32_t ALPHA_LED_ROWS = 56;
inline constexpr size_t ALPHA_ROW_BYTES = 15;
inline constexpr size_t ALPHA_PLANE_BYTES = 840;
inline constexpr size_t ALPHA_FRAME_BYTES = 1680;

using AlphaFrame = std::array<uint8_t, ALPHA_FRAME_BYTES>;

// Maps display row [0..55] to Alpha Director physical buffer row index [0..55].
static constexpr uint8_t ALPHA_ROW[56] = {
	32, 36, 40, 44, 48, 52, 0,  4,  8,  12, 16, 20, 24, 28,
	33, 37, 41, 45, 49, 53, 1,  5,  9,  13, 17, 21, 25, 29,
	34, 38, 42, 46, 50, 54, 2,  6,  10, 14, 18, 22, 26, 30,
	35, 39, 43, 47, 51, 55, 3,  7,  11, 15, 19, 23, 27, 31,
};

inline void pack_alpha_frame(const uint8_t *rgba_data, uint32_t linesize, AlphaFrame &out) noexcept
{
	out.fill(0);
	if (!rgba_data)
		return;

	for (uint32_t row = 0; row < ALPHA_LED_ROWS; ++row) {
		const size_t green_row_offset = static_cast<size_t>(ALPHA_ROW[row]) * ALPHA_ROW_BYTES;
		const size_t red_row_offset = ALPHA_PLANE_BYTES + green_row_offset;
		const uint8_t *row_ptr = rgba_data + static_cast<size_t>(row) * linesize;

		for (uint32_t col = 0; col < ALPHA_LED_COLS; ++col) {
			const uint8_t *px = row_ptr + static_cast<size_t>(col) * 4;
			const size_t byte_idx = static_cast<size_t>(col / 8);
			const uint8_t mask = static_cast<uint8_t>(0x80U >> (col % 8));

			if (px[1] >= 128)
				out[green_row_offset + byte_idx] |= mask;
			if (px[0] >= 128)
				out[red_row_offset + byte_idx] |= mask;
		}
	}
}

inline AlphaFrame pack_alpha_frame(const uint8_t *rgba_data, uint32_t linesize) noexcept
{
	AlphaFrame frame{};
	pack_alpha_frame(rgba_data, linesize, frame);
	return frame;
}

