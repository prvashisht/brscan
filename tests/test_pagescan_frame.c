/*
 * PageScan lifecycle regressions for the output-frame cap.
 *
 * Drives the real PageScan / ScanStart / sane_read retry sequence.
 * USB reads are scripted. This is not an ARMv6 or scanner acceptance run.
 *
 * Build (from the repo root):
 *   cc -std=c11 -Wall -Wextra -Werror -Wno-pointer-sign -Wno-unused-parameter \
 *      -fsanitize=address,undefined -fno-omit-frame-pointer -g \
 *      -DNDEBUG -DBRSANESUFFIX=2 -DBRSCAN_PAGE_SCAN_TEST \
 *      -DBACKEND_NAME=brother -include unistd.h -include stdlib.h -include string.h \
 *      -I /tmp/brscan-stubs -I "$SANE_INC" -I libsane-brother -I libbrscandec \
 *      -I libbrcolm -I include \
 *      -c libsane-brother/brother_scanner.c -o /tmp/brother_scanner.o
 *   cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
 *      -I /tmp/brscan-stubs -I "$SANE_INC" -I libsane-brother -I libbrscandec \
 *      -I libbrcolm -I include \
 *      -o /tmp/test_pagescan_frame \
 *      tests/test_pagescan_frame.c tests/pagescan_stubs.c \
 *      libsane-brother/brother_output_frame.c /tmp/brother_scanner.o
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "brother.h"
#include "brother_scanner.h"
#include "brother_output_frame.h"
#include "brother_scandec.h"

extern LPSTR lpFwTempBuff;
extern int FwTempBuffLength;
extern DWORD dwFwTempBuffMaxSize;
extern LPSTR lpRxBuff;
extern LPBYTE lpRxTempBuff;
extern DWORD dwRxTempBuffLength;
extern DWORD dwRxBuffMaxSize;
extern LONG lRealY;

void brscan_test_arm_output_frame(Brother_Scanner *scanner);
int64_t brscan_test_frame_budget(void);
int64_t brscan_test_frame_delivered(void);
int brscan_test_frame_armed(void);

void brscan_test_reset_reads(void);
void brscan_test_push_read(const unsigned char *bytes, int len);
int brscan_test_reads_consumed(void);

static int g_failed;
static int g_out_width;
static int g_lines_written;
static unsigned char g_fill;

static void check(int cond, const char *file, int line, const char *expr)
{
	if (!cond) {
		fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
		g_failed = 1;
	}
}

#define CHECK(cond) check((cond), __FILE__, __LINE__, #cond)

static DWORD test_scan_dec_write(SCANDEC_WRITE *info, INT *lines)
{
	int i;

	if (g_out_width <= 0 || info == NULL || info->pWriteBuff == NULL) {
		if (lines)
			*lines = 0;
		return 0;
	}
	g_lines_written++;
	g_fill = (g_lines_written <= 4) ? 0x11 : 0x22;
	for (i = 0; i < g_out_width; i++)
		info->pWriteBuff[i] = (char)g_fill;
	if (lines)
		*lines = 1;
	return (DWORD)g_out_width;
}

static DWORD test_scan_dec_end(SCANDEC_WRITE *info, INT *lines)
{
	(void)info;
	if (lines)
		*lines = 0;
	return 0;
}

static void test_set_tbl(HANDLE a, HANDLE b)
{
	(void)a;
	(void)b;
}

static BOOL test_page_start(void)
{
	return 1;
}

static BOOL test_close(void)
{
	return 1;
}

static void attach_scandec(Brother_Scanner *scanner)
{
	scanner->scanDec.lpfnScanDecSetTbl = test_set_tbl;
	scanner->scanDec.lpfnScanDecPageStart = test_page_start;
	scanner->scanDec.lpfnScanDecWrite = test_scan_dec_write;
	scanner->scanDec.lpfnScanDecPageEnd = test_scan_dec_end;
	scanner->scanDec.lpfnScanDecClose = test_close;
}

static Brother_Scanner *new_scanner(void)
{
	Brother_Scanner *scanner = calloc(1, sizeof(*scanner));
	static dev_handle handle;

	CHECK(scanner != NULL);
	if (!scanner)
		return NULL;
	memset(&handle, 0, sizeof(handle));
	handle.device = IFTYPE_USB;
	scanner->hScanner = &handle;
	attach_scandec(scanner);
	scanner->scanState.bScanning = TRUE;
	return scanner;
}

static void set_geometry(Brother_Scanner *scanner, int color, int pixels,
			 int stored_stride, int lines)
{
	scanner->devScanInfo.wColorType = (WORD)color;
	scanner->uiSetting.wColorType = (WORD)color;
	scanner->scanInfo.ScanAreaSize.lWidth = pixels;
	scanner->scanInfo.ScanAreaSize.lHeight = lines;
	scanner->scanInfo.ScanAreaByte.lWidth = stored_stride;
	scanner->scanInfo.ScanAreaByte.lHeight = lines;
	scanner->devScanInfo.ScanAreaByte.lWidth = stored_stride;
	scanner->scanInfo.UserSelect.wResoX = 100;
	scanner->scanInfo.UserSelect.wResoY = 100;
	scanner->devScanInfo.DeviceScan.wResoX = 100;
	scanner->devScanInfo.DeviceScan.wResoY = 100;
	g_out_width = stored_stride;
}

static int sane_bytes_per_line(int color, int pixels)
{
	switch (color) {
	case COLOR_FUL:
	case COLOR_FUL_NOCM:
		return pixels * 3;
	case COLOR_TG:
		return pixels;
	case COLOR_BW:
	case COLOR_ED:
		return (pixels + 7) / 8;
	default:
		return -1;
	}
}

/* Same retry sane_read performs when the backend reports duplex sync. */
static int frontend_read(Brother_Scanner *scanner, unsigned char *buf,
			 int maxlen, int *len)
{
	int rc;

	*len = 0;
	if (scanner->scanState.bEOF)
		return SANE_STATUS_EOF;
	rc = PageScan(scanner, (char *)buf, maxlen, len);
	if (rc == SANE_STATUS_DUPLEX_ADVERSE && *len == 1)
		rc = PageScan(scanner, (char *)buf, maxlen, len);
	return rc;
}

static void queue_raster(unsigned char *data, int len)
{
	lpFwTempBuff = (LPSTR)data;
	FwTempBuffLength = len;
	dwFwTempBuffMaxSize = (DWORD)len + 64;
}

static void test_a_exact_then_eof(void)
{
	Brother_Scanner *scanner = new_scanner();
	enum { bpl = 8, lines = 5, budget = bpl * lines };
	unsigned char src[budget];
	unsigned char dst[budget + 16];
	int len = 0;
	int rc;
	int i;

	if (!scanner)
		return;
	for (i = 0; i < budget; i++)
		src[i] = (unsigned char)(0x40 + (i % 17));
	set_geometry(scanner, COLOR_TG, bpl, bpl, lines);
	brscan_test_arm_output_frame(scanner);
	lRealY = lines;
	scanner->scanState.iProcessEnd = SCAN_EOF;
	queue_raster(src, budget);

	rc = frontend_read(scanner, dst, budget + 16, &len);
	CHECK(rc == SANE_STATUS_GOOD);
	CHECK(len == budget);
	CHECK(memcmp(dst, src, budget) == 0);
	CHECK(scanner->scanState.bScanning == TRUE);
	CHECK(scanner->scanState.bEOF == FALSE);
	CHECK(brscan_test_frame_delivered() == budget);

	rc = frontend_read(scanner, dst, budget, &len);
	CHECK(rc == SANE_STATUS_EOF);
	CHECK(len == 0);
	CHECK(scanner->scanState.bEOF == TRUE);
	CHECK(scanner->scanState.bScanning == FALSE);
	free(scanner);
}

static void test_b_surplus_keeps_status(void)
{
	Brother_Scanner *scanner = new_scanner();
	enum { bpl = 8, lines = 4, budget = bpl * lines };
	unsigned char src[budget + bpl];
	unsigned char dst[budget + bpl];
	int len = 0;
	int rc;
	int i;

	if (!scanner)
		return;
	for (i = 0; i < budget; i++)
		src[i] = (unsigned char)(i + 1);
	memset(src + budget, 0xA5, bpl);
	set_geometry(scanner, COLOR_TG, bpl, bpl, lines);
	brscan_test_arm_output_frame(scanner);
	lRealY = lines + 1;
	scanner->scanState.iProcessEnd = SCAN_DOCJAM;
	queue_raster(src, budget + bpl);

	rc = frontend_read(scanner, dst, (int)sizeof(dst), &len);
	CHECK(rc == SANE_STATUS_JAMMED);
	CHECK(rc != SANE_STATUS_EOF);
	CHECK(len == budget);
	CHECK(memcmp(dst, src, budget) == 0);
	CHECK(scanner->scanState.bScanning == TRUE);
	CHECK(scanner->scanState.bEOF == FALSE);
	CHECK(brscan_test_frame_armed());

	len = 99;
	rc = frontend_read(scanner, dst, (int)sizeof(dst), &len);
	CHECK(rc == SANE_STATUS_JAMMED);
	CHECK(rc != SANE_STATUS_IO_ERROR || scanner->scanState.iProcessEnd == SCAN_DOCJAM);
	CHECK(len == 0);
	CHECK(scanner->scanState.bScanning == TRUE);
	free(scanner);
}

static void test_c_mps_page_two(void)
{
	Brother_Scanner *scanner = new_scanner();
	enum { bpl = 10, lines = 6, budget = bpl * lines };
	unsigned char page1[budget + bpl];
	unsigned char page2[budget];
	unsigned char dst[budget + bpl];
	int pixels_before;
	int lines_before;
	int len = 0;
	int rc;
	int total = 0;
	int i;

	if (!scanner)
		return;
	memset(page1, 0x31, budget);
	memset(page1 + budget, 0xEE, bpl);
	memset(page2, 0x32, budget);
	set_geometry(scanner, COLOR_TG, bpl, bpl, lines);
	brscan_test_arm_output_frame(scanner);
	CHECK(brscan_test_frame_budget() == budget);
	lRealY = lines + 1;
	scanner->scanState.iProcessEnd = SCAN_MPS;
	scanner->devScanInfo.wScanSource = MFCSCANSRC_ADF;
	queue_raster(page1, budget + bpl);

	rc = frontend_read(scanner, dst, (int)sizeof(dst), &len);
	CHECK(rc == SANE_STATUS_GOOD);
	CHECK(len == budget);
	CHECK(memcmp(dst, page1, budget) == 0);
	total = len;

	rc = frontend_read(scanner, dst, (int)sizeof(dst), &len);
	CHECK(rc == SANE_STATUS_EOF);
	CHECK(len == 0);
	CHECK(scanner->scanState.bScanning == TRUE);
	CHECK(scanner->scanState.bEOF == TRUE);
	CHECK(scanner->scanState.iProcessEnd == SCAN_MPS);
	CHECK(total == budget);

	pixels_before = (int)scanner->scanInfo.ScanAreaSize.lWidth;
	lines_before = (int)scanner->scanInfo.ScanAreaSize.lHeight;
	/* Page 2: ScanStart increments nPageCnt, so start from 1. */
	scanner->scanState.nPageCnt = 1;
	rc = (int)ScanStart(scanner);
	CHECK(rc == SANE_STATUS_GOOD);
	CHECK(rc != SANE_STATUS_DEVICE_BUSY);
	CHECK(rc != SANE_STATUS_IO_ERROR);
	CHECK(scanner->scanState.bScanning == TRUE);
	CHECK(scanner->scanState.bEOF == FALSE);
	CHECK(scanner->scanInfo.ScanAreaSize.lWidth == pixels_before);
	CHECK(scanner->scanInfo.ScanAreaSize.lHeight == lines_before);
	CHECK(brscan_test_frame_armed());
	CHECK(brscan_test_frame_delivered() == 0);
	CHECK(brscan_test_frame_budget() ==
	      (int64_t)sane_bytes_per_line(COLOR_TG, pixels_before) * lines_before);

	lRealY = lines;
	scanner->scanState.iProcessEnd = SCAN_EOF;
	queue_raster(page2, budget);
	rc = frontend_read(scanner, dst, budget, &len);
	CHECK(rc == SANE_STATUS_GOOD);
	CHECK(len == budget);
	CHECK(memcmp(dst, page2, budget) == 0);
	for (i = 0; i < budget; i++)
		CHECK(dst[i] == 0x32);
	free(scanner);
}

static unsigned char *wire_line(unsigned char header, int payload, int *out_len)
{
	unsigned char *line = malloc((size_t)payload + 3);

	CHECK(line != NULL);
	if (!line)
		return NULL;
	line[0] = header;
	line[1] = (unsigned char)(payload & 0xff);
	line[2] = (unsigned char)((payload >> 8) & 0xff);
	memset(line + 3, 0x7E, (size_t)payload);
	*out_len = payload + 3;
	return line;
}

static void test_d_duplex_retry(void)
{
	Brother_Scanner *scanner = new_scanner();
	unsigned char alone84[1] = {0x84};
	unsigned char dst[64];
	unsigned char *stored;
	unsigned char *back;
	int line_len = 0;
	int back_len = 0;
	int len = 0;
	int rc;

	if (!scanner)
		return;
	g_lines_written = 0;
	g_fill = 0x11;
	set_geometry(scanner, COLOR_TG, 8, 8, 4);
	scanner->modelInf.seriesNo = 1;
	scanner->scanState.bReadbufEnd = FALSE;
	scanner->scanState.iProcessEnd = 0;
	scanner->scanState.bEOF = FALSE;
	brscan_test_arm_output_frame(scanner);
	lRealY = 4;

	stored = malloc(4096);
	lpRxBuff = malloc(4096);
	lpRxTempBuff = malloc(4096);
	CHECK(stored && lpRxBuff && lpRxTempBuff);
	if (!stored || !lpRxBuff || !lpRxTempBuff)
		goto done;
	dwRxBuffMaxSize = 4096;
	dwRxTempBuffLength = 0;
	dwFwTempBuffMaxSize = 4096;
	memset(stored, 0x11, 32);
	queue_raster(stored, 32);

	/* Full front side is already in the transmit buffer. This read must
	 * not open USB; the following read is the 0x84 sync and its retry. */
	rc = frontend_read(scanner, dst, 32, &len);
	CHECK(rc == SANE_STATUS_GOOD);
	CHECK(len == 32);
	CHECK(dst[0] == 0x11);
	CHECK(dst[31] == 0x11);
	CHECK(brscan_test_frame_delivered() == 32);
	CHECK(scanner->scanState.bScanning == TRUE);
	CHECK(brscan_test_reads_consumed() == 0);

	back = wire_line(0x02, 2, &line_len);
	if (!back)
		goto done;
	back = realloc(back, (size_t)line_len + 1);
	CHECK(back != NULL);
	if (!back)
		goto done;
	back[line_len] = 0x80;
	back_len = line_len + 1;

	/* The next ScanDec line is the back side; earlier lines were the front. */
	g_lines_written = 4;
	brscan_test_reset_reads();
	brscan_test_push_read(alone84, 1);
	brscan_test_push_read(back, back_len);

	/* sane_read retries the 0x84 sentinel. The back side is one decoded
	 * row plus white pad up to the re-armed frame. */
	rc = frontend_read(scanner, dst, (int)sizeof(dst), &len);
	CHECK(rc == SANE_STATUS_GOOD);
	CHECK(len == 32);
	CHECK(dst[0] == 0x22);
	CHECK(dst[7] == 0x22);
	CHECK(dst[8] == 0xFF);
	CHECK(dst[31] == 0xFF);
	CHECK(scanner->scanState.bScanning == TRUE);
	CHECK(scanner->scanState.bEOF == FALSE);
	CHECK(brscan_test_reads_consumed() == 2);

	rc = frontend_read(scanner, dst, (int)sizeof(dst), &len);
	CHECK(rc == SANE_STATUS_EOF);
	CHECK(len == 0);
	CHECK(scanner->scanState.bEOF == TRUE);
	free(back);
done:
	free(stored);
	free(lpRxBuff);
	free(lpRxTempBuff);
	lpRxBuff = NULL;
	lpRxTempBuff = NULL;
	lpFwTempBuff = NULL;
	free(scanner);
}

static void expect_terminal(int process_end, int sane_status)
{
	Brother_Scanner *scanner = new_scanner();
	unsigned char extra[16];
	unsigned char dst[16];
	int len = 0;
	int rc;

	if (!scanner)
		return;
	memset(extra, 0xAB, sizeof(extra));
	set_geometry(scanner, COLOR_TG, 8, 8, 2);
	brscan_test_arm_output_frame(scanner);
	lRealY = 2;
	scanner->scanState.iProcessEnd = process_end;
	queue_raster(extra, (int)sizeof(extra));

	rc = frontend_read(scanner, dst, (int)sizeof(dst), &len);
	CHECK(rc == sane_status);
	CHECK(scanner->scanState.bScanning == TRUE);
	CHECK(brscan_test_frame_armed());
	CHECK(scanner->scanState.iProcessEnd == process_end);

	len = 7;
	rc = frontend_read(scanner, dst, (int)sizeof(dst), &len);
	CHECK(rc == sane_status);
	CHECK(scanner->scanState.bScanning == TRUE);
	CHECK(brscan_test_frame_armed());
	CHECK(rc != SANE_STATUS_IO_ERROR || sane_status == SANE_STATUS_IO_ERROR);
	free(scanner);
}

static void test_e_terminal_statuses(void)
{
	expect_terminal(SCAN_DOCJAM, SANE_STATUS_JAMMED);
	expect_terminal(SCAN_COVER_OPEN, SANE_STATUS_COVER_OPEN);
	expect_terminal(SCAN_NODOC, SANE_STATUS_NO_DOCS);
	expect_terminal(SCAN_SERVICE_ERR, SANE_STATUS_IO_ERROR);
}

static void test_f_a15(void)
{
	Brother_Scanner *scanner = new_scanner();
	const int bpl = 24;
	const int lines = 196;
	const int already = 193 * bpl;
	const int fresh = 6 * bpl;
	unsigned char *first = malloc((size_t)already);
	unsigned char *more = malloc((size_t)fresh);
	unsigned char *dst = malloc((size_t)already + fresh);
	int len = 0;
	int rc;

	CHECK(first && more && dst);
	if (!scanner || !first || !more || !dst)
		goto done;
	memset(first, 0x41, (size_t)already);
	memset(more, 0x11, (size_t)fresh);
	memset(more + 2 * bpl, 0x3C, (size_t)bpl);
	memset(more + 3 * bpl, 0x00, (size_t)(3 * bpl));
	set_geometry(scanner, COLOR_BW, 192, bpl, lines);
	brscan_test_arm_output_frame(scanner);
	CHECK(brscan_test_frame_budget() == (int64_t)bpl * lines);
	lRealY = 193;
	scanner->scanState.iProcessEnd = SCAN_EOF;
	queue_raster(first, already);

	rc = frontend_read(scanner, dst, already, &len);
	CHECK(rc == SANE_STATUS_GOOD);
	CHECK(len == already);
	CHECK(memcmp(dst, first, (size_t)already) == 0);

	lRealY = 193;
	queue_raster(more, fresh);
	rc = frontend_read(scanner, dst + already, fresh, &len);
	CHECK(rc == SANE_STATUS_GOOD);
	CHECK(len == 3 * bpl);
	CHECK(memcmp(dst + already, more, (size_t)(3 * bpl)) == 0);
	CHECK(dst[195 * bpl] == 0x3C);
	CHECK(brscan_test_frame_delivered() == (int64_t)bpl * lines);

	rc = frontend_read(scanner, dst, fresh, &len);
	CHECK(rc == SANE_STATUS_EOF);
	CHECK(len == 0);
done:
	free(first);
	free(more);
	free(dst);
	free(scanner);
}

static void test_g_b2_full_page(void)
{
	Brother_Scanner *scanner = new_scanner();
	const int bpl = 2480;
	const int lines = 3435;
	const int64_t budget = (int64_t)bpl * lines;
	unsigned char *src;
	unsigned char *dst;
	int len = 0;
	int rc;
	size_t i;

	if (!scanner)
		return;
	src = malloc((size_t)budget + (size_t)bpl);
	dst = malloc((size_t)budget + (size_t)bpl);
	CHECK(src && dst);
	if (!src || !dst)
		goto done;
	for (i = 0; i < (size_t)budget; i++)
		src[i] = (unsigned char)(i * 3u + 1u);
	memset(src + budget, 0xA5, (size_t)bpl);
	set_geometry(scanner, COLOR_TG, bpl, bpl, lines);
	brscan_test_arm_output_frame(scanner);
	CHECK(brscan_test_frame_budget() == 8518800);
	lRealY = lines + 1;
	scanner->scanState.iProcessEnd = SCAN_EOF;
	queue_raster(src, (int)budget + bpl);

	rc = frontend_read(scanner, dst, (int)budget + bpl, &len);
	CHECK(rc == SANE_STATUS_GOOD);
	CHECK(len == (int)budget);
	CHECK(memcmp(dst, src, (size_t)budget) == 0);
	CHECK(brscan_test_frame_delivered() == budget);

	rc = frontend_read(scanner, dst, bpl, &len);
	CHECK(rc == SANE_STATUS_EOF);
	CHECK(len == 0);
done:
	free(src);
	free(dst);
	free(scanner);
}

static void test_h_short_and_overflow(void)
{
	Brother_Scanner *scanner = new_scanner();
	unsigned char decoded[16];
	unsigned char dst[40];
	int len = 0;
	int rc;

	if (!scanner)
		return;

	/* 1-bit white pad is 0x00 and reaches the advertised frame. */
	memset(decoded, 0x5A, 8);
	set_geometry(scanner, COLOR_BW, 64, 8, 4);
	brscan_test_arm_output_frame(scanner);
	lRealY = 1;
	scanner->scanState.iProcessEnd = SCAN_EOF;
	queue_raster(decoded, 8);
	rc = frontend_read(scanner, dst, 40, &len);
	CHECK(rc == SANE_STATUS_GOOD);
	CHECK(len == 32);
	CHECK(memcmp(dst, decoded, 8) == 0);
	CHECK(dst[8] == 0x00);
	CHECK(dst[31] == 0x00);
	CHECK(brscan_test_frame_delivered() == 32);

	/* Gray / RGB white pad is 0xFF. */
	memset(decoded, 0x5A, 8);
	set_geometry(scanner, COLOR_TG, 8, 8, 4);
	scanner->scanState.bEOF = FALSE;
	scanner->scanState.bScanning = TRUE;
	brscan_test_arm_output_frame(scanner);
	lRealY = 1;
	scanner->scanState.iProcessEnd = SCAN_EOF;
	queue_raster(decoded, 8);
	rc = frontend_read(scanner, dst, 40, &len);
	CHECK(rc == SANE_STATUS_GOOD);
	CHECK(len == 32);
	CHECK(dst[8] == 0xFF);
	CHECK(dst[31] == 0xFF);

	set_geometry(scanner, COLOR_FUL, 4, 12, 2);
	scanner->scanState.bEOF = FALSE;
	scanner->scanState.bScanning = TRUE;
	brscan_test_arm_output_frame(scanner);
	CHECK(brscan_test_frame_budget() == 24);
	lRealY = 0;
	scanner->scanState.iProcessEnd = SCAN_EOF;
	FwTempBuffLength = 0;
	rc = frontend_read(scanner, dst, 40, &len);
	CHECK(rc == SANE_STATUS_GOOD);
	CHECK(len == 24);
	CHECK(dst[0] == 0xFF);
	CHECK(dst[23] == 0xFF);

	free(scanner);

	/* width * lines overflows a signed 32-bit multiply. */
	scanner = new_scanner();
	if (!scanner)
		return;
	{
		const int width = 65536;
		const int height = 32768;
		const int64_t budget = (int64_t)width * height;
		const int chunk = width * 512;
		unsigned char *buf = malloc((size_t)chunk);
		int64_t got = 0;

		CHECK(budget > (int64_t)INT_MAX);
		CHECK(buf != NULL);
		if (!buf) {
			free(scanner);
			return;
		}
		set_geometry(scanner, COLOR_TG, width, width, height);
		brscan_test_arm_output_frame(scanner);
		CHECK(brscan_test_frame_budget() == budget);
		lRealY = 0;
		scanner->scanState.iProcessEnd = SCAN_EOF;
		FwTempBuffLength = 0;
		lpFwTempBuff = NULL;
		while (got < budget && !g_failed) {
			rc = frontend_read(scanner, buf, chunk, &len);
			if (rc == SANE_STATUS_EOF) {
				CHECK(len == 0);
				break;
			}
			CHECK(rc == SANE_STATUS_GOOD);
			CHECK(len > 0);
			CHECK(buf[0] == 0xFF);
			CHECK(buf[len - 1] == 0xFF);
			got += len;
		}
		CHECK(got == budget);
		CHECK(brscan_test_frame_delivered() == budget);
		rc = frontend_read(scanner, buf, chunk, &len);
		CHECK(rc == SANE_STATUS_EOF);
		CHECK(len == 0);
		free(buf);
	}
	free(scanner);
}

static void test_i_cancel_and_second_scan(void)
{
	Brother_Scanner *scanner = new_scanner();
	unsigned char src[20];
	unsigned char dst[40];
	int len = 0;
	int rc;

	if (!scanner)
		return;
	memset(src, 0x44, sizeof(src));
	set_geometry(scanner, COLOR_TG, 10, 10, 4);
	brscan_test_arm_output_frame(scanner);
	CHECK(brscan_test_frame_armed());
	lRealY = 2;
	scanner->scanState.iProcessEnd = SCAN_EOF;
	queue_raster(src, 20);

	rc = frontend_read(scanner, dst, 20, &len);
	CHECK(rc == SANE_STATUS_GOOD);
	CHECK(len == 20);

	AbortPageScan(scanner);
	scanner->scanState.bScanning = FALSE;
	CHECK(!brscan_test_frame_armed());
	rc = frontend_read(scanner, dst, 20, &len);
	CHECK(rc == SANE_STATUS_IO_ERROR);

	scanner->scanState.nPageCnt = 1;
	scanner->scanState.iProcessEnd = SCAN_MPS;
	scanner->devScanInfo.wScanSource = MFCSCANSRC_ADF;
	scanner->scanState.bCanceled = FALSE;
	scanner->scanState.bScanning = TRUE;
	rc = (int)ScanStart(scanner);
	CHECK(rc == SANE_STATUS_GOOD);
	CHECK(brscan_test_frame_armed());
	CHECK(brscan_test_frame_delivered() == 0);
	CHECK(brscan_test_frame_budget() == 40);

	memset(src, 0x55, sizeof(src));
	lRealY = 2;
	scanner->scanState.iProcessEnd = SCAN_EOF;
	queue_raster(src, 20);
	rc = frontend_read(scanner, dst, 40, &len);
	CHECK(rc == SANE_STATUS_GOOD);
	CHECK(len == 40);
	CHECK(dst[0] == 0x55);
	CHECK(dst[20] == 0xFF);
	CHECK(dst[39] == 0xFF);

	lpFwTempBuff = NULL;
	FwTempBuffLength = 0;
	scanner->hScanner = NULL;
	ScanEnd(scanner);
	CHECK(!brscan_test_frame_armed());
	free(scanner);
}

static void test_j_sane_frame(void)
{
	struct {
		int color;
		int pixels;
		int stored;
		int lines;
		int sane_bpl;
	} cases[] = {
		{COLOR_BW, 192, 24, 196, 24},
		{COLOR_ED, 192, 24, 50, 24},
		{COLOR_TG, 2480, 2480, 3435, 2480},
		{COLOR_FUL, 8, 24, 5, 24},
		{COLOR_FUL_NOCM, 100, 300, 2, 300},
		{COLOR_TG, 100, 80, 3, 100},
	};
	int i;

	for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
		Brother_Scanner *scanner = new_scanner();
		int64_t expect;

		if (!scanner)
			return;
		set_geometry(scanner, cases[i].color, cases[i].pixels,
			     cases[i].stored, cases[i].lines);
		scanner->scanState.bScanning = TRUE;
		CHECK(sane_bytes_per_line(cases[i].color, cases[i].pixels) ==
		      cases[i].sane_bpl);
		CHECK(brother_output_frame_row_bytes(cases[i].color,
						     cases[i].pixels, 0) ==
		      cases[i].sane_bpl);
		CHECK(brother_output_frame_row_bytes(cases[i].color,
						     cases[i].pixels,
						     cases[i].stored) ==
		      cases[i].sane_bpl);
		brscan_test_arm_output_frame(scanner);
		expect = (int64_t)cases[i].sane_bpl * cases[i].lines;
		CHECK(brscan_test_frame_budget() == expect);
		if (cases[i].stored != cases[i].sane_bpl)
			CHECK(brscan_test_frame_budget() !=
			      (int64_t)cases[i].stored * cases[i].lines);
		free(scanner);
	}
}

int main(void)
{
	test_a_exact_then_eof();
	test_b_surplus_keeps_status();
	test_c_mps_page_two();
	test_d_duplex_retry();
	test_e_terminal_statuses();
	test_f_a15();
	test_g_b2_full_page();
	test_h_short_and_overflow();
	test_i_cancel_and_second_scan();
	test_j_sane_frame();
	if (g_failed) {
		fprintf(stderr, "pagescan frame tests failed\n");
		return 1;
	}
	printf("pagescan frame tests passed\n");
	return 0;
}
