#pragma once

#include <stdint.h>
#include <stdbool.h>

void premultiply_alpha(uint8_t *data, int width, int height);

typedef struct crop_rect {
	int min_x, min_y;
	int max_x, max_y;
	bool empty;
	bool cropped;
} crop_rect;

crop_rect get_crop_rect(uint8_t *data, int width, int height);
void apply_crop_rect(uint8_t *data, int *width, int *height, crop_rect rect);
