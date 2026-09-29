#include "brother_output_frame.h"

#include <string.h>

void brother_output_frame_reset(BrotherOutputFrame *frame)
{
	if (!frame)
		return;
	frame->delivered = 0;
	frame->budget = 0;
	frame->armed = 0;
}

void brother_output_frame_begin(BrotherOutputFrame *frame,
				int64_t bytes_per_line, int64_t lines)
{
	if (!frame)
		return;
	frame->delivered = 0;
	frame->armed = 1;
	if (bytes_per_line <= 0 || lines <= 0 ||
	    bytes_per_line > INT64_MAX / lines) {
		frame->budget = 0;
		return;
	}
	frame->budget = bytes_per_line * lines;
}

int brother_output_frame_armed(const BrotherOutputFrame *frame)
{
	return frame && frame->armed;
}

int brother_output_frame_exhausted(const BrotherOutputFrame *frame)
{
	return frame && frame->armed && frame->delivered >= frame->budget;
}

int64_t brother_output_frame_remaining(const BrotherOutputFrame *frame)
{
	if (!frame || !frame->armed || frame->delivered >= frame->budget)
		return 0;
	return frame->budget - frame->delivered;
}

int64_t brother_output_frame_delivered(const BrotherOutputFrame *frame)
{
	if (!frame || !frame->armed)
		return 0;
	return frame->delivered;
}

int64_t brother_output_frame_budget(const BrotherOutputFrame *frame)
{
	if (!frame || !frame->armed)
		return 0;
	return frame->budget;
}

int64_t brother_output_frame_row_bytes(int color_type, int64_t pixels,
				       int64_t stored_stride)
{
	int64_t announced = 0;

	if (pixels > 0) {
		switch (color_type) {
		case 5: /* COLOR_FUL */
		case 6: /* COLOR_FUL_NOCM */
			if (pixels <= INT64_MAX / 3)
				announced = pixels * 3;
			break;
		case 3: /* COLOR_TG */
			announced = pixels;
			break;
		case 0: /* COLOR_BW */
		case 1: /* COLOR_ED */
			if (pixels <= INT64_MAX - 7)
				announced = (pixels + 7) / 8;
			break;
		default:
			break;
		}
	}

	if (announced <= 0)
		return stored_stride > 0 ? stored_stride : 0;
	if (stored_stride > 0 && stored_stride < announced)
		return stored_stride;
	return announced;
}

static void frame_take(BrotherOutputFrame *frame, int avail, int max_out,
		       int *copy_len, int *keep_len, int *discard_len)
{
	int64_t remain;
	int64_t in_frame;
	int64_t copy;
	int64_t keep;
	int64_t discard;

	*copy_len = 0;
	*keep_len = 0;
	*discard_len = 0;
	if (!frame || !frame->armed)
		return;
	if (avail < 0)
		avail = 0;
	if (max_out < 0)
		max_out = 0;

	remain = frame->budget - frame->delivered;
	if (remain < 0)
		remain = 0;

	in_frame = avail;
	if (in_frame > remain)
		in_frame = remain;

	copy = in_frame;
	if (copy > max_out)
		copy = max_out;

	keep = in_frame - copy;
	discard = (int64_t)avail - in_frame;
	frame->delivered += copy;

	*copy_len = (int)copy;
	*keep_len = (int)keep;
	*discard_len = (int)discard;
}

int brother_output_frame_emit(BrotherOutputFrame *frame,
			      char *src, int *src_len,
			      char *dst, int max_dst, int *dst_len)
{
	int avail;
	int copy = 0;
	int keep = 0;
	int discard = 0;

	if (dst_len)
		*dst_len = 0;
	if (!frame || !frame->armed)
		return 0;

	avail = src_len ? *src_len : 0;
	frame_take(frame, avail, max_dst, &copy, &keep, &discard);

	if (copy > 0 && src && dst)
		memmove(dst, src, (size_t)copy);
	if (keep > 0 && src)
		memmove(src, src + copy, (size_t)keep);
	if (src_len)
		*src_len = keep;
	if (dst_len)
		*dst_len = copy;
	return discard;
}

int brother_output_frame_pad(BrotherOutputFrame *frame, int want,
			     int max_out, int row_bytes)
{
	int64_t remain;
	int64_t authorized;
	int wrote;

	if (!frame || !frame->armed || frame->delivered >= frame->budget)
		return 0;
	if (want <= 0 || max_out <= 0 || row_bytes <= 0)
		return 0;

	remain = frame->budget - frame->delivered;
	authorized = want;
	if (authorized > remain)
		authorized = remain;
	if (authorized <= 0)
		return 0;

	if (authorized < max_out) {
		wrote = (int)authorized;
	} else {
		wrote = row_bytes * (max_out / row_bytes);
		if ((int64_t)wrote > authorized)
			wrote = (int)authorized;
	}
	if (wrote <= 0)
		return 0;

	frame->delivered += wrote;
	return wrote;
}
