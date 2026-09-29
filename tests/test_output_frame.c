/*
 * Focused regressions for the SANE output-frame cap.
 *
 * These exercise the same account PageScan uses when it copies decoded
 * raster to sane_read. They do not claim ARMv6 or scanner acceptance.
 *
 *   A exact frame is unchanged
 *   B A15: 193 rows already out, 6 new, advertise 196
 *   C B2: 3435 gray rows plus one non-white row
 *   D short page still pads to the advertised size
 *   E a read smaller than the queued buffer cannot pass the budget
 *   F a second scan is not shortened or extended by the first
 *   G cancel/error reset leaves no frame state
 *
 * Build:
 *   cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
 *      -I libsane-brother -o test_output_frame \
 *      tests/test_output_frame.c libsane-brother/brother_output_frame.c
 */
#include "brother_output_frame.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* brother.h COLOR_* codes. brother_scanner.c fails the build if they drift. */
enum {
	COLOR_BW = 0,
	COLOR_ED = 1,
	COLOR_DTH = 2,
	COLOR_TG = 3,
	COLOR_256 = 4,
	COLOR_FUL = 5,
	COLOR_FUL_NOCM = 6
};

static int g_failed;

static void check(int cond, const char *file, int line, const char *expr)
{
	if (!cond) {
		fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
		g_failed = 1;
	}
}

#define CHECK(cond) check((cond), __FILE__, __LINE__, #cond)

static void fill_seq(uint8_t *p, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++)
		p[i] = (uint8_t)(i * 131u + 17u);
}

static int emit_to(BrotherOutputFrame *frame, uint8_t *src, int *src_len,
		   uint8_t *dst, int *dst_len, int dst_cap, int max_read)
{
	int guard = 0;

	while (*src_len > 0 && !brother_output_frame_exhausted(frame)) {
		int got = 0;
		int before = *src_len;
		int step = max_read;
		int room;

		if (*dst_len > dst_cap)
			return -1;
		room = dst_cap - *dst_len;
		if (step > room)
			step = room;
		brother_output_frame_emit(frame, (char *)src, src_len,
					  (char *)dst + *dst_len, step, &got);
		*dst_len += got;
		if (got == 0 && *src_len == before)
			return -1;
		if (++guard > 1000000)
			return -1;
	}
	return 0;
}

static void test_row_bytes(void)
{
	CHECK(brother_output_frame_row_bytes(COLOR_BW, 192, 24) == 24);
	CHECK(brother_output_frame_row_bytes(COLOR_ED, 192, 24) == 24);
	CHECK(brother_output_frame_row_bytes(COLOR_TG, 2480, 2480) == 2480);
	CHECK(brother_output_frame_row_bytes(COLOR_FUL, 100, 300) == 300);
	CHECK(brother_output_frame_row_bytes(COLOR_FUL_NOCM, 8, 24) == 24);
	/* A narrower or wider decoder stride does not change the SANE row. */
	CHECK(brother_output_frame_row_bytes(COLOR_TG, 100, 80) == 100);
	CHECK(brother_output_frame_row_bytes(COLOR_TG, 100, 120) == 100);
	CHECK(brother_output_frame_row_bytes(COLOR_BW, 192, 16) == 24);
	/* Modes sane_get_parameters does not switch on keep the stored stride. */
	CHECK(brother_output_frame_row_bytes(COLOR_DTH, 100, 17) == 17);
	CHECK(brother_output_frame_row_bytes(COLOR_256, 64, 64) == 64);
	CHECK(brother_output_frame_row_bytes(COLOR_BW, 0, 0) == 0);
}

static void test_a_exact(void)
{
	BrotherOutputFrame frame;
	enum { bpl = 32, lines = 50, n = bpl * lines };
	uint8_t src[n], dst[n], ref[n];
	int src_len = n;
	int dst_len = 0;
	int discard;

	memset(&frame, 0, sizeof(frame));
	fill_seq(src, n);
	memcpy(ref, src, n);
	brother_output_frame_begin(&frame, bpl, lines);

	discard = brother_output_frame_emit(&frame, (char *)src, &src_len,
					    (char *)dst, n + 100, &dst_len);
	CHECK(discard == 0);
	CHECK(dst_len == n);
	CHECK(src_len == 0);
	CHECK(memcmp(dst, ref, n) == 0);
	CHECK(brother_output_frame_delivered(&frame) == n);
	CHECK(brother_output_frame_exhausted(&frame));

	/* Split across buffers that are not a row multiple. Content unchanged. */
	brother_output_frame_begin(&frame, bpl, lines);
	memcpy(src, ref, n);
	src_len = n;
	dst_len = 0;
	memset(dst, 0x5A, n);
	CHECK(emit_to(&frame, src, &src_len, dst, &dst_len, n, 13) == 0);
	CHECK(dst_len == n);
	CHECK(src_len == 0);
	CHECK(memcmp(dst, ref, n) == 0);
	CHECK(brother_output_frame_exhausted(&frame));
}

static void test_b_a15(void)
{
	BrotherOutputFrame frame;
	const int bpl = (int)brother_output_frame_row_bytes(COLOR_BW, 192, 24);
	const int lines = 196;
	const int budget = bpl * lines;
	const int already = 193 * bpl;
	const int fresh = 6 * bpl;
	uint8_t *page;
	uint8_t *out;
	uint8_t fresh_rows[6 * 64];
	int src_len;
	int dst_len = 0;
	int got = 0;
	int discard;

	CHECK(bpl == 24);
	CHECK(budget == 24 * 196);

	page = malloc((size_t)budget);
	out = malloc((size_t)budget);
	CHECK(page && out);
	if (!page || !out) {
		free(page);
		free(out);
		return;
	}
	fill_seq(page, (size_t)budget);
	/* Three new in-frame rows, then three white surplus rows. */
	memset(fresh_rows, 0x11, (size_t)fresh);
	memset(fresh_rows + 2 * bpl, 0x3C, (size_t)bpl);
	memset(fresh_rows + 3 * bpl, 0x00, (size_t)(3 * bpl));

	memset(&frame, 0, sizeof(frame));
	brother_output_frame_begin(&frame, bpl, lines);
	src_len = already;
	discard = brother_output_frame_emit(&frame, (char *)page, &src_len,
					    (char *)out, already + 10, &dst_len);
	CHECK(discard == 0);
	CHECK(dst_len == already);
	CHECK(src_len == 0);
	CHECK(!brother_output_frame_exhausted(&frame));

	src_len = fresh;
	got = 0;
	discard = brother_output_frame_emit(&frame, (char *)fresh_rows, &src_len,
					    (char *)out + dst_len, fresh, &got);
	dst_len += got;
	CHECK(discard == 3 * bpl);
	CHECK(got == 3 * bpl);
	CHECK(src_len == 0);
	CHECK(dst_len == budget);
	CHECK(memcmp(out, page, (size_t)already) == 0);
	CHECK(memcmp(out + already, fresh_rows, (size_t)(3 * bpl)) == 0);
	CHECK(out[195 * bpl] == 0x3C);
	CHECK(brother_output_frame_exhausted(&frame));
	/* The read that finishes the frame still carries bytes.
	 * The following read is EOF and must not expose the white tail. */
	CHECK(got > 0);
	got = 99;
	src_len = 3 * bpl;
	memset(fresh_rows, 0x00, (size_t)(3 * bpl));
	discard = brother_output_frame_emit(&frame, (char *)fresh_rows, &src_len,
					    (char *)out, 3 * bpl, &got);
	CHECK(brother_output_frame_exhausted(&frame));
	CHECK(got == 0);
	CHECK(discard == 3 * bpl);
	CHECK(src_len == 0);

	free(page);
	free(out);
}

static void test_c_b2(void)
{
	BrotherOutputFrame frame;
	const int bpl = (int)brother_output_frame_row_bytes(COLOR_TG, 2480, 2480);
	const int lines = 3435;
	const int64_t budget = (int64_t)bpl * lines;
	const int extra = bpl;
	uint8_t *src;
	uint8_t *dst;
	int src_len;
	int dst_len = 0;
	int discard;
	size_t i;

	CHECK(bpl == 2480);
	CHECK(budget == 8518800);

	src = malloc((size_t)budget + (size_t)extra);
	dst = malloc((size_t)budget + 1);
	CHECK(src && dst);
	if (!src || !dst) {
		free(src);
		free(dst);
		return;
	}
	fill_seq(src, (size_t)budget);
	memset(src + budget, 0xA5, (size_t)extra);
	dst[budget] = 0x5A;

	memset(&frame, 0, sizeof(frame));
	brother_output_frame_begin(&frame, bpl, lines);
	src_len = (int)budget + extra;
	discard = brother_output_frame_emit(&frame, (char *)src, &src_len,
					    (char *)dst, src_len, &dst_len);
	CHECK(discard == extra);
	CHECK(dst_len == (int)budget);
	CHECK(src_len == 0);
	CHECK(memcmp(dst, src, (size_t)budget) == 0);
	CHECK(dst[budget] == 0x5A);
	for (i = 0; i < (size_t)budget; i++) {
		if (dst[i] == 0xA5 && src[i] != 0xA5) {
			CHECK(0);
			break;
		}
	}
	CHECK(brother_output_frame_delivered(&frame) == budget);
	CHECK(brother_output_frame_exhausted(&frame));
	/* Next sane_read: length 0, EOF. Further surplus stays unexposed. */
	dst_len = 7;
	src_len = extra;
	discard = brother_output_frame_emit(&frame, (char *)(src + budget), &src_len,
					    (char *)dst, extra, &dst_len);
	CHECK(dst_len == 0);
	CHECK(discard == extra);

	free(src);
	free(dst);
}

static int pad_short(BrotherOutputFrame *frame, uint8_t *dst, int *dst_len,
		     int dst_cap, int bpl, int missing_lines, int max_read,
		     uint8_t fill)
{
	int guard = 0;

	while (!brother_output_frame_exhausted(frame)) {
		int room = max_read;
		int want = bpl * missing_lines;
		int wrote;

		if (*dst_len > dst_cap)
			return -1;
		if (room > dst_cap - *dst_len)
			room = dst_cap - *dst_len;
		wrote = brother_output_frame_pad(frame, want, room, bpl);
		if (wrote <= 0)
			return -1;
		memset(dst + *dst_len, fill, (size_t)wrote);
		*dst_len += wrote;
		missing_lines -= wrote / bpl;
		if (wrote % bpl)
			missing_lines = 0;
		if (++guard > 100000)
			return -1;
	}
	return 0;
}

static void test_d_short(void)
{
	BrotherOutputFrame frame;
	const int bpl = 24;
	const int lines = 196;
	const int decoded_lines = 100;
	const int budget = bpl * lines;
	uint8_t *src = malloc((size_t)decoded_lines * bpl);
	uint8_t *ref = malloc((size_t)decoded_lines * bpl);
	uint8_t *dst = malloc((size_t)budget);
	int src_len = decoded_lines * bpl;
	int dst_len = 0;

	CHECK(src && ref && dst);
	if (!src || !ref || !dst) {
		free(src);
		free(ref);
		free(dst);
		return;
	}
	fill_seq(src, (size_t)src_len);
	memcpy(ref, src, (size_t)src_len);
	memset(&frame, 0, sizeof(frame));
	brother_output_frame_begin(&frame, bpl, lines);
	CHECK(emit_to(&frame, src, &src_len, dst, &dst_len, budget, 100) == 0);
	CHECK(dst_len == decoded_lines * bpl);
	CHECK(src_len == 0);
	CHECK(!brother_output_frame_exhausted(&frame));
	CHECK(pad_short(&frame, dst, &dst_len, budget, bpl,
			lines - decoded_lines, 100, 0x00) == 0);
	CHECK(dst_len == budget);
	CHECK(memcmp(dst, ref, (size_t)decoded_lines * bpl) == 0);
	CHECK(dst[decoded_lines * bpl] == 0x00);
	CHECK(dst[budget - 1] == 0x00);
	CHECK(brother_output_frame_delivered(&frame) == budget);

	/* True Gray padding is 0xFF and still lands on the advertised size. */
	{
		const int g_bpl = 2480;
		const int g_lines = 20;
		const int g_decoded = 7;
		const int g_budget = g_bpl * g_lines;
		uint8_t *gsrc = malloc((size_t)g_decoded * g_bpl);
		uint8_t *gref = malloc((size_t)g_decoded * g_bpl);
		uint8_t *gdst = malloc((size_t)g_budget);
		int gsrc_len = g_decoded * g_bpl;
		int gdecoded = gsrc_len;
		int gdst_len = 0;

		CHECK(gsrc && gref && gdst);
		if (gsrc && gref && gdst) {
			fill_seq(gsrc, (size_t)gsrc_len);
			memcpy(gref, gsrc, (size_t)gsrc_len);
			brother_output_frame_begin(&frame, g_bpl, g_lines);
			CHECK(emit_to(&frame, gsrc, &gsrc_len, gdst, &gdst_len,
				      g_budget, 3000) == 0);
			CHECK(pad_short(&frame, gdst, &gdst_len, g_budget, g_bpl,
					g_lines - g_decoded, 3000, 0xFF) == 0);
			CHECK(gdst_len == g_budget);
			CHECK(memcmp(gdst, gref, (size_t)gdecoded) == 0);
			CHECK(gdst[gdecoded] == 0xFF);
			CHECK(gdst[g_budget - 1] == 0xFF);
		}
		free(gsrc);
		free(gref);
		free(gdst);
	}

	free(src);
	free(ref);
	free(dst);
}

static void test_e_buffer_boundary(void)
{
	BrotherOutputFrame frame;
	enum { bpl = 8, lines = 10, budget = bpl * lines };
	uint8_t src[40];
	uint8_t dst[budget];
	uint8_t prefix[budget];
	int src_len;
	int dst_len;
	int got;
	int discard;
	int i;

	fill_seq(prefix, budget);
	memset(&frame, 0, sizeof(frame));
	brother_output_frame_begin(&frame, bpl, lines);

	/* Deliver 70 of 80 bytes, then a 40-byte queue with only 10 bytes of room. */
	src_len = 70;
	dst_len = 0;
	memcpy(src, prefix, 40);
	discard = brother_output_frame_emit(&frame, (char *)prefix, &src_len,
					    (char *)dst, 70, &dst_len);
	CHECK(discard == 0);
	CHECK(dst_len == 70);
	CHECK(brother_output_frame_remaining(&frame) == 10);

	fill_seq(src, 40);
	/* Make the queued tail differ from the in-frame prefix. */
	for (i = 10; i < 40; i++)
		src[i] = 0xA5;
	memcpy(src, prefix + 70, 10);
	src_len = 40;
	got = 0;
	discard = brother_output_frame_emit(&frame, (char *)src, &src_len,
					    (char *)dst + dst_len, 100, &got);
	CHECK(got == 10);
	CHECK(discard == 30);
	CHECK(src_len == 0);
	CHECK(memcmp(dst + 70, prefix + 70, 10) == 0);
	CHECK(brother_output_frame_delivered(&frame) == budget);
	CHECK(brother_output_frame_exhausted(&frame));

	/* Remaining room smaller than both the queue and the read size,
	 * and also split when the read itself is smaller than the room. */
	brother_output_frame_begin(&frame, bpl, lines);
	dst_len = 0;
	src_len = 70;
	discard = brother_output_frame_emit(&frame, (char *)prefix, &src_len,
					    (char *)dst, 70, &dst_len);
	CHECK(dst_len == 70);
	memcpy(src, prefix + 70, 10);
	memset(src + 10, 0xA5, 30);
	src_len = 40;
	got = 0;
	discard = brother_output_frame_emit(&frame, (char *)src, &src_len,
					    (char *)dst + dst_len, 4, &got);
	CHECK(got == 4);
	CHECK(discard == 30);
	CHECK(src_len == 6);
	CHECK(memcmp(src, prefix + 74, 6) == 0);
	dst_len += got;
	got = 0;
	discard = brother_output_frame_emit(&frame, (char *)src, &src_len,
					    (char *)dst + dst_len, 4, &got);
	CHECK(got == 4);
	CHECK(discard == 0);
	dst_len += got;
	got = 0;
	discard = brother_output_frame_emit(&frame, (char *)src, &src_len,
					    (char *)dst + dst_len, 4, &got);
	CHECK(got == 2);
	CHECK(discard == 0);
	CHECK(src_len == 0);
	dst_len += got;
	CHECK(dst_len == budget);
	CHECK(memcmp(dst, prefix, budget) == 0);
	CHECK(brother_output_frame_exhausted(&frame));
}

static void test_f_second_scan(void)
{
	BrotherOutputFrame frame;
	uint8_t src[200];
	uint8_t dst[200];
	int src_len;
	int dst_len;
	int discard;

	fill_seq(src, sizeof(src));
	memset(&frame, 0, sizeof(frame));

	/* First page exhausts a 100-byte frame. */
	brother_output_frame_begin(&frame, 10, 10);
	src_len = 150;
	dst_len = 0;
	discard = brother_output_frame_emit(&frame, (char *)src, &src_len,
					    (char *)dst, 150, &dst_len);
	CHECK(dst_len == 100);
	CHECK(discard == 50);
	CHECK(brother_output_frame_exhausted(&frame));

	/* Without a new begin, leftover state would refuse or extend. */
	src_len = 80;
	dst_len = 5;
	discard = brother_output_frame_emit(&frame, (char *)src, &src_len,
					    (char *)dst, 80, &dst_len);
	CHECK(dst_len == 0);
	CHECK(discard == 80);

	/* Shorter second page must not inherit the 100-byte budget. */
	brother_output_frame_reset(&frame);
	brother_output_frame_begin(&frame, 10, 5);
	CHECK(brother_output_frame_budget(&frame) == 50);
	CHECK(brother_output_frame_delivered(&frame) == 0);
	memcpy(src, dst, 50); /* dst still holds nothing useful; refill */
	fill_seq(src, 80);
	src_len = 80;
	dst_len = 0;
	discard = brother_output_frame_emit(&frame, (char *)src, &src_len,
					    (char *)dst, 80, &dst_len);
	CHECK(dst_len == 50);
	CHECK(discard == 30);
	CHECK(memcmp(dst, src, 50) == 0);

	/* Longer second page must not stay capped at the previous 50. */
	brother_output_frame_begin(&frame, 20, 8);
	CHECK(brother_output_frame_budget(&frame) == 160);
	fill_seq(src, 160);
	src_len = 160;
	dst_len = 0;
	discard = brother_output_frame_emit(&frame, (char *)src, &src_len,
					    (char *)dst, 160, &dst_len);
	CHECK(discard == 0);
	CHECK(dst_len == 160);
	CHECK(memcmp(dst, src, 160) == 0);
	CHECK(brother_output_frame_exhausted(&frame));
}

static void test_g_reset(void)
{
	BrotherOutputFrame frame;
	uint8_t src[40];
	uint8_t dst[40];
	uint8_t saved[40];
	int src_len = 40;
	int dst_len = 0;
	int discard;

	fill_seq(src, 40);
	memcpy(saved, src, 40);
	memset(dst, 0x2A, 40);
	memset(&frame, 0, sizeof(frame));
	brother_output_frame_begin(&frame, 10, 10);
	discard = brother_output_frame_emit(&frame, (char *)src, &src_len,
					    (char *)dst, 25, &dst_len);
	CHECK(discard == 0);
	CHECK(dst_len == 25);
	CHECK(brother_output_frame_delivered(&frame) == 25);
	CHECK(brother_output_frame_armed(&frame));
	CHECK(!brother_output_frame_exhausted(&frame));

	/* ScanEnd, AbortPageScan, cancel, and terminal PageScan errors
	 * all call reset. Nothing from this page may remain. */
	brother_output_frame_reset(&frame);
	CHECK(!brother_output_frame_armed(&frame));
	CHECK(!brother_output_frame_exhausted(&frame));
	CHECK(brother_output_frame_delivered(&frame) == 0);
	CHECK(brother_output_frame_remaining(&frame) == 0);
	CHECK(brother_output_frame_budget(&frame) == 0);

	memcpy(src, saved, 40);
	src_len = 40;
	dst_len = 3;
	discard = brother_output_frame_emit(&frame, (char *)src, &src_len,
					    (char *)dst, 40, &dst_len);
	CHECK(discard == 0);
	CHECK(dst_len == 0);
	CHECK(src_len == 40);
	CHECK(memcmp(src, saved, 40) == 0);

	brother_output_frame_begin(&frame, 4, 5);
	CHECK(brother_output_frame_armed(&frame));
	CHECK(brother_output_frame_budget(&frame) == 20);
	CHECK(brother_output_frame_remaining(&frame) == 20);
	src_len = 20;
	dst_len = 0;
	discard = brother_output_frame_emit(&frame, (char *)saved, &src_len,
					    (char *)dst, 20, &dst_len);
	CHECK(discard == 0);
	CHECK(dst_len == 20);
	CHECK(memcmp(dst, saved, 20) == 0);
}

static void test_pad_wider_than_int(void)
{
	BrotherOutputFrame frame;
	int64_t budget = (int64_t)INT_MAX + 4096;
	int wrote;

	memset(&frame, 0, sizeof(frame));
	brother_output_frame_begin(&frame, budget, 1);
	wrote = brother_output_frame_pad(&frame, (int64_t)INT_MAX + 100,
					  4096, 1024);
	CHECK(wrote == 4096);
	CHECK(brother_output_frame_delivered(&frame) == 4096);
	CHECK(!brother_output_frame_exhausted(&frame));
	CHECK(brother_output_frame_remaining(&frame) == budget - 4096);
}

static void test_rgb_and_narrow_read(void)
{
	BrotherOutputFrame frame;
	const int bpl = (int)brother_output_frame_row_bytes(COLOR_FUL, 8, 24);
	const int lines = 5;
	const int budget = bpl * lines;
	uint8_t src[24 * 6];
	uint8_t ref[24 * 5];
	uint8_t dst[24 * 5];
	int src_len = 24 * 6;
	int dst_len = 0;

	CHECK(bpl == 24);
	CHECK(budget == 120);
	fill_seq(src, sizeof(src));
	memcpy(ref, src, (size_t)budget);
	memset(src + budget, 0xEE, 24);
	memset(&frame, 0, sizeof(frame));
	brother_output_frame_begin(&frame, bpl, lines);
	CHECK(emit_to(&frame, src, &src_len, dst, &dst_len, budget, 10) == 0);
	CHECK(dst_len == budget);
	CHECK(memcmp(dst, ref, (size_t)budget) == 0);
	CHECK(brother_output_frame_exhausted(&frame));
	/* The extra RGB row was dropped instead of kept for a later read. */
	CHECK(src_len == 0);
}

int main(void)
{
	test_row_bytes();
	test_a_exact();
	test_b_a15();
	test_c_b2();
	test_d_short();
	test_e_buffer_boundary();
	test_f_second_scan();
	test_g_reset();
	test_pad_wider_than_int();
	test_rgb_and_narrow_read();
	if (g_failed) {
		fprintf(stderr, "output-frame tests failed\n");
		return 1;
	}
	printf("output-frame tests passed\n");
	return 0;
}
