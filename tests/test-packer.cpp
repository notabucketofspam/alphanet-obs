#include "alpha-packer.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define TEST_CHECK(expr)                                                                               \
	do {                                                                                           \
		if (!(expr)) {                                                                         \
			std::fprintf(stderr, "FAIL (%s:%d): %s\n", __FILE__, __LINE__, #expr);         \
			std::exit(1);                                                                  \
		}                                                                                      \
	} while (false)

namespace {

void set_rgba_pixel(std::vector<uint8_t> &buf, uint32_t linesize, uint32_t col, uint32_t row, uint8_t r, uint8_t g,
		    uint8_t b, uint8_t a)
{
	uint8_t *px = buf.data() + static_cast<size_t>(row) * linesize + static_cast<size_t>(col) * 4;
	px[0] = r;
	px[1] = g;
	px[2] = b;
	px[3] = a;
}

void test_all_black()
{
	constexpr uint32_t linesize = ALPHA_LED_COLS * 4;
	std::vector<uint8_t> rgba(static_cast<size_t>(ALPHA_LED_ROWS) * linesize, 0);

	const AlphaFrame packed = pack_alpha_frame(rgba.data(), linesize);
	for (size_t i = 0; i < packed.size(); ++i) {
		TEST_CHECK(packed[i] == 0);
	}
}

void test_single_red_pixel()
{
	constexpr uint32_t linesize = ALPHA_LED_COLS * 4;
	std::vector<uint8_t> rgba(static_cast<size_t>(ALPHA_LED_ROWS) * linesize, 0);
	set_rgba_pixel(rgba, linesize, 0, 0, 255, 0, 0, 255);

	const AlphaFrame packed = pack_alpha_frame(rgba.data(), linesize);
	constexpr size_t expected_idx = 840 + 32 * 15;
	TEST_CHECK(expected_idx == 1320);
	TEST_CHECK(packed[expected_idx] == 0x80);
	for (size_t i = 0; i < packed.size(); ++i) {
		if (i != expected_idx)
			TEST_CHECK(packed[i] == 0);
	}
}

void test_single_green_pixel()
{
	constexpr uint32_t linesize = ALPHA_LED_COLS * 4;
	std::vector<uint8_t> rgba(static_cast<size_t>(ALPHA_LED_ROWS) * linesize, 0);
	set_rgba_pixel(rgba, linesize, 7, 6, 0, 255, 0, 255);

	const AlphaFrame packed = pack_alpha_frame(rgba.data(), linesize);
	constexpr size_t expected_idx = 0 * 15;
	TEST_CHECK(expected_idx == 0);
	TEST_CHECK(packed[expected_idx] == 0x01);
	for (size_t i = 0; i < packed.size(); ++i) {
		if (i != expected_idx)
			TEST_CHECK(packed[i] == 0);
	}
}

void test_single_amber_pixel()
{
	constexpr uint32_t linesize = ALPHA_LED_COLS * 4;
	std::vector<uint8_t> rgba(static_cast<size_t>(ALPHA_LED_ROWS) * linesize, 0);
	set_rgba_pixel(rgba, linesize, 119, 55, 255, 255, 0, 255);

	const AlphaFrame packed = pack_alpha_frame(rgba.data(), linesize);
	constexpr size_t expected_green = 31 * 15 + 14;
	constexpr size_t expected_red = 840 + 479;
	TEST_CHECK(expected_green == 479);
	TEST_CHECK(expected_red == 1319);
	TEST_CHECK(packed[expected_green] == 0x01);
	TEST_CHECK(packed[expected_red] == 0x01);
	for (size_t i = 0; i < packed.size(); ++i) {
		if (i != expected_green && i != expected_red)
			TEST_CHECK(packed[i] == 0);
	}
}

void test_padded_linesize_and_threshold()
{
	constexpr uint32_t tight_bytes = ALPHA_LED_COLS * 4;
	constexpr uint32_t linesize = 512;
	std::vector<uint8_t> rgba(static_cast<size_t>(ALPHA_LED_ROWS) * linesize, 0);

	// Fill padding bytes with 0xFF to verify they are never read as pixels.
	for (uint32_t row = 0; row < ALPHA_LED_ROWS; ++row) {
		uint8_t *pad = rgba.data() + static_cast<size_t>(row) * linesize + tight_bytes;
		for (uint32_t b = tight_bytes; b < linesize; ++b) {
			pad[b - tight_bytes] = 0xFF;
		}
	}

	// Below threshold (127) must remain off.
	set_rgba_pixel(rgba, linesize, 1, 1, 127, 127, 255, 255);
	// At threshold (128) must turn on.
	// row=1 -> ALPHA_ROW[1]=36; col=8 -> byte 1, bit 0x80.
	set_rgba_pixel(rgba, linesize, 8, 1, 128, 0, 0, 255);
	// row=55 -> ALPHA_ROW[55]=31; col=119 -> byte 14, bit 0x01.
	set_rgba_pixel(rgba, linesize, 119, 55, 0, 128, 0, 255);

	const AlphaFrame packed = pack_alpha_frame(rgba.data(), linesize);
	constexpr size_t expected_red = 840 + 36 * 15 + 1;
	constexpr size_t expected_green = 31 * 15 + 14;
	TEST_CHECK(packed[expected_red] == 0x80);
	TEST_CHECK(packed[expected_green] == 0x01);
	for (size_t i = 0; i < packed.size(); ++i) {
		if (i != expected_red && i != expected_green)
			TEST_CHECK(packed[i] == 0);
	}
}

} // namespace

int main()
{
	test_all_black();
	test_single_red_pixel();
	test_single_green_pixel();
	test_single_amber_pixel();
	test_padded_linesize_and_threshold();
	std::printf("All alpha-packer tests passed.\n");
	return 0;
}

