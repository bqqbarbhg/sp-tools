#pragma once

#include <stdint.h>

typedef struct astcenc_quality {
	int partitions_to_test;
	float oplimit;
	float mincorrel;
	float dblimit;
	int block_mode_cutoff;
	int max_iters;
} astcenc_quality;

typedef struct astcenc_opts {
	bool linear;
	int num_threads;
	int block_width;
	int block_height;
	bool verbose;
	astcenc_quality quality;
} astcenc_opts;

void astcenc_init();
bool astcenc_encode_image(const astcenc_opts *opts, uint8_t *dst, const uint8_t *src, int width, int height);
