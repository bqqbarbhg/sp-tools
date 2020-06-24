#pragma once

#include <stdint.h>
#include <stdbool.h>

void premultiply_alpha(uint8_t *data, int width, int height);
void invert_channel(uint8_t *data, int width, int height);
void swizzle_rg_to_ga(uint8_t *data, int width, int height);
void insert_channel(uint8_t *data, const uint8_t *chan_data, int chan, int width, int height);

typedef struct crop_rect {
	int min_x, min_y;
	int max_x, max_y;
	bool empty;
	bool cropped;
} crop_rect;

crop_rect get_crop_rect(uint8_t *data, int width, int height);
void apply_crop_rect(uint8_t *data, int *p_width, int *p_height, crop_rect rect);
