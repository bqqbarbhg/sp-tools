#include "image.h"

#include <string.h>

void premultiply_alpha(uint8_t *data, int width, int height)
{
	size_t num_pixels = (size_t)width * (size_t)width;
	for (uint8_t *p = data, *e = p + num_pixels*4; p != e; p += 4) {
		unsigned a = p[3];
		p[0] = (uint8_t)((unsigned)p[0] * a / 255);
		p[1] = (uint8_t)((unsigned)p[1] * a / 255);
		p[2] = (uint8_t)((unsigned)p[2] * a / 255);
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

void apply_crop_rect(uint8_t *data, int *width, int *height, crop_rect rect)
{
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
	*width = rect.max_x - rect.min_x;
	*height = rect.max_y - rect.min_y;
}

