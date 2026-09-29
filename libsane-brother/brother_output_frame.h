/*
 * Cap on raster bytes exposed to a SANE frontend.
 *
 * The budget is the current scan's advertised frame (row stride times
 * line count). It is not a scanner-model constant. Protocol records are
 * not trimmed here; only decoded raster that would be copied to sane_read
 * is limited. Short-page padding still fills up to that same budget.
 */
#ifndef BROTHER_OUTPUT_FRAME_H
#define BROTHER_OUTPUT_FRAME_H

#include <stdint.h>

typedef struct {
	int64_t delivered; /* raster bytes already returned for this page */
	int64_t budget;    /* advertised frame size in bytes; 0 if empty */
	int armed;         /* non-zero after begin, cleared by reset */
} BrotherOutputFrame;

void brother_output_frame_reset(BrotherOutputFrame *frame);
void brother_output_frame_begin(BrotherOutputFrame *frame,
				int64_t bytes_per_line, int64_t lines);

int brother_output_frame_armed(const BrotherOutputFrame *frame);
int brother_output_frame_exhausted(const BrotherOutputFrame *frame);
int64_t brother_output_frame_remaining(const BrotherOutputFrame *frame);
int64_t brother_output_frame_delivered(const BrotherOutputFrame *frame);
int64_t brother_output_frame_budget(const BrotherOutputFrame *frame);

/*
 * Row stride sane_get_parameters announces for this color mode.
 *
 * color_type uses the brother.h COLOR_* codes (BW=0, ED=1, TG=3,
 * 24-bit color=5, color without matching=6). Those modes return the
 * SANE formula even when stored_stride disagrees; the caller must not
 * shrink the frame to the decoder stride. Modes with no SANE formula
 * use stored_stride (scanInfo.ScanAreaByte.lWidth) explicitly.
 */
int64_t brother_output_frame_row_bytes(int color_type, int64_t pixels,
				       int64_t stored_stride);

/*
 * Move an in-frame prefix of src into dst.
 * *src_len is the decoded bytes waiting; on return it is the in-frame
 * tail still waiting for a later read. *dst_len is the bytes exposed
 * to this sane_read. The return value is decoded bytes past the frame,
 * which are dropped and not kept in src.
 * dst and src must not overlap. An unarmed frame copies nothing and
 * leaves src unchanged.
 */
int brother_output_frame_emit(BrotherOutputFrame *frame,
			      char *src, int *src_len,
			      char *dst, int max_dst, int *dst_len);

/*
 * Bytes of short-page padding that still fit in the advertised frame
 * and in this read. want is the full shortfall in bytes and may exceed
 * INT_MAX; the returned count is at most max_out and is added to the
 * delivered total. A pad that fits in max_out is returned in full. A
 * pad that does not is split on whole rows, matching AddSpace.
 */
int brother_output_frame_pad(BrotherOutputFrame *frame, int64_t want,
			     int max_out, int64_t row_bytes);

#endif
