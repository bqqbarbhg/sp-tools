#include "image.h"

#include <string.h>

void premultiply_alpha(uint8_t *data, int width, int height)
{
	size_t num_pixels = (size_t)width * (size_t)height;
	for (uint8_t *p = data, *e = p + num_pixels*4; p != e; p += 4) {
		unsigned a = p[3];
		p[0] = (uint8_t)((unsigned)p[0] * a / 255);
		p[1] = (uint8_t)((unsigned)p[1] * a / 255);
		p[2] = (uint8_t)((unsigned)p[2] * a / 255);
	}
}

void invert_channel(uint8_t *data, int width, int height)
{
	size_t num_pixels = (size_t)width * (size_t)height;
	for (uint8_t *p = data, *e = p + num_pixels*4; p != e; p += 4) {
		p[0] = 255 - p[0];
	}
}

void move_x(uint8_t *data, int width, int height, int amount)
{
	for (int y = 0; y < height; y++) {
		uint8_t *p = data + y * width * 4;
		for (int ix = 0; ix < width; ix++) {
			int x = ix;
			if (amount < 0) x = width - x - 1;

			int dx = x + amount;
			if (dx < 0) dx = 0;
			if (dx >= width - 1) dx = width - 1;
			p[x*4 + 0] = p[dx*4 + 0];
			p[x*4 + 1] = p[dx*4 + 1];
			p[x*4 + 2] = p[dx*4 + 2];
			p[x*4 + 3] = p[dx*4 + 3];
		}
	}
}

void move_y(uint8_t *data, int width, int height, int amount)
{
	for (int iy = 0; iy < height; iy++) {
		int y = iy;
		if (amount < 0) y = height - y - 1;

		int dy = y + amount;
		if (dy < 0) dy = 0;
		if (dy >= height - 1) dy = height - 1;
		if (dy != y) {
			uint8_t *p = data + y * width * 4;
			uint8_t *dp = data + dy * width * 4;
			memcpy(p, dp, width * 4);
		}
	}
}

void swizzle_rg_to_ga(uint8_t *data, int width, int height)
{
	size_t num_pixels = (size_t)width * (size_t)height;
	for (uint8_t *p = data, *e = p + num_pixels*4; p != e; p += 4) {
		p[3] = p[1];
		p[1] = p[0];
		p[0] = 0;
		p[2] = 0;
	}
}

void insert_channel(uint8_t *data, const uint8_t *chan_data, int chan, int width, int height)
{
	size_t num_pixels = (size_t)width * (size_t)height;
	const uint8_t *s = chan_data;
	for (uint8_t *p = data, *e = p + num_pixels*4; p != e; p += 4) {
		p[chan] = *s++;
	}
}

crop_rect get_crop_rect(uint8_t *data, int width, int height)
{
	crop_rect rect;

	const uint8_t *alpha = data + 3;
	int32_t x, y;
	int32_t w = (int32_t)width;
	int32_t h = (int32_t)height;
	uint32_t stride = 4 * width;

	for (y = 0; y < h; y++) {
		const uint8_t *a = alpha + y * w * 4;
		for (x = 0; x < w; x++) {
			if (*a != 0) {
				rect.min_y = (uint32_t)y;
				y = h;
				break;
			}
			a += 4;
		}
	}

	if (y == h) {
		// Empty rectangle
		rect.min_y = 0;
		rect.min_x = 0;
		rect.max_x = 1;
		rect.max_y = 1;
		rect.empty = true;
		rect.cropped = true;
		return rect;
	}

	rect.empty = false;

	for (y = h - 1; y >= 0; y--) {
		const uint8_t *a = alpha + y * w * 4;
		for (x = 0; x < w; x++) {
			if (*a != 0) {
				rect.max_y = (uint32_t)(y + 1);
				y = 0;
				break;
			}
			a += 4;
		}
	}

	for (x = 0; x < w; x++) {
		const uint8_t *a = alpha + x * 4;
		for (y = 0; y < h; y++) {
			if (*a != 0) {
				rect.min_x = (uint32_t)x;
				x = w;
				break;
			}
			a += stride;
		}
	}

	for (x = w - 1; x >= 0; x--) {
		const uint8_t *a = alpha + x * 4;
		for (y = 0; y < h; y++) {
			if (*a != 0) {
				rect.max_x = (uint32_t)(x + 1);
				x = 0;
				break;
			}
			a += stride;
		}
	}

	rect.cropped = (rect.min_x > 0 || rect.min_y > 0 || rect.max_x < width || rect.max_y < height); 

	return rect;
}

void apply_crop_rect(uint8_t *data, int *p_width, int *p_height, crop_rect rect)
{
	int width = *p_width, height = *p_height;
	uint8_t *dst = data;
	uint8_t *src = data + (size_t)width*(size_t)rect.min_y*4 + (size_t)rect.min_x*4;
	size_t line_size = (size_t)(rect.max_x - rect.min_x) * 4;
	size_t pitch = (size_t)width * 4;
	uint8_t *dst_end = dst + line_size * (rect.max_y - rect.min_y);
	while (dst != dst_end) {
		memmove(dst, src, line_size);
		dst += line_size;
		src += pitch;
	}
	*p_width = rect.max_x - rect.min_x;
	*p_height = rect.max_y - rect.min_y;
}

