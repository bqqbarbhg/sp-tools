#pragma once

#include <stdint.h>

typedef struct astcenc_opts {
	bool linear;
	int num_threads;
	int block_width;
	int block_height;
} astcenc_opts;

bool astcenc_encode_image(const astcenc_opts *opts, uint8_t *dst, const uint8_t *src, int width, int height);
