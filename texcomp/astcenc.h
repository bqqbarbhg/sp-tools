#pragma once

#include <stdint.h>
#include <stddef.h>

typedef void (*astcenc_progress_fn)(void *user, size_t current, size_t total);

typedef enum {
	ASTCENC_SWIZZLE_IDENTITY,
	ASTCENC_SWIZZLE_R,
	ASTCENC_SWIZZLE_G,
	ASTCENC_SWIZZLE_B,
	ASTCENC_SWIZZLE_A,
	ASTCENC_SWIZZLE_0,
	ASTCENC_SWIZZLE_1,
} astcenc_swizzle;

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
	astcenc_progress_fn progress_fn;
	void *progress_user;
	astcenc_swizzle swizzle[4];
	float rgba_weights[4];
	bool normal_map;
} astcenc_opts;

void astcenc_init();
bool astcenc_encode_image(const astcenc_opts *opts, uint8_t *dst, const uint8_t *src, int width, int height);
