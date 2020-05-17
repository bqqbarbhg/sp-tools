#include "astcenc.h"
#include "astc/astc_codec_internals.h"
#include <math.h>

void encode_astc_image(
	const astc_codec_image* input_image,
	astc_codec_image* output_image,
	int xdim,
	int ydim,
	int zdim,
	const error_weighting_params* ewp,
	astc_decode_mode decode_mode,
	swizzlepattern swz_encode,
	swizzlepattern swz_decode,
	uint8_t* buffer,
	int pack_and_unpack,
	int threadcount,
	astcenc_progress_fn progress_fn,
	void *progress_user
);

void astcenc_init()
{
	prepare_angular_tables();
	build_quantization_mode_table();
}

bool astcenc_encode_image(const astcenc_opts *opts, uint8_t *dst, const uint8_t *src, int width, int height)
{
	int xdim = opts->block_width;
	int ydim = opts->block_height;
	int zdim = 1;

	swizzlepattern swz_encode = { 0,1,2,3 };
	if (opts->swizzle[0] != ASTCENC_SWIZZLE_IDENTITY) swz_encode.r = (uint8_t)((int)opts->swizzle[0] - 1);
	if (opts->swizzle[1] != ASTCENC_SWIZZLE_IDENTITY) swz_encode.g = (uint8_t)((int)opts->swizzle[1] - 1);
	if (opts->swizzle[2] != ASTCENC_SWIZZLE_IDENTITY) swz_encode.b = (uint8_t)((int)opts->swizzle[2] - 1);
	if (opts->swizzle[3] != ASTCENC_SWIZZLE_IDENTITY) swz_encode.a = (uint8_t)((int)opts->swizzle[3] - 1);

	swizzlepattern swz_decode = { 0,1,2,3 };

	error_weighting_params ewp = { 0 };

	ewp.rgb_power = 1.0f;
	ewp.alpha_power = 1.0f;
	ewp.rgb_base_weight = 1.0f;
	ewp.alpha_base_weight = 1.0f;
	ewp.rgb_mean_weight = 0.0f;
	ewp.rgb_stdev_weight = 0.0f;
	ewp.alpha_mean_weight = 0.0f;
	ewp.alpha_stdev_weight = 0.0f;

	ewp.rgb_mean_and_stdev_mixing = 0.0f;
	ewp.mean_stdev_radius = 0;
	ewp.enable_rgb_scale_with_alpha = 0;
	ewp.alpha_radius = 0;

	ewp.block_artifact_suppression = 0.0f;
	ewp.rgba_weights[0] = 1.0f;
	ewp.rgba_weights[1] = 1.0f;
	ewp.rgba_weights[2] = 1.0f;
	ewp.rgba_weights[3] = 1.0f;
	ewp.ra_normal_angular_scale = 0;

	if (opts->rgba_weights[0] != 0.0f || opts->rgba_weights[1] != 0.0f || opts->rgba_weights[2] != 0.0f || opts->rgba_weights[3] != 0.0f) {
		ewp.rgba_weights[0] = opts->rgba_weights[0];
		ewp.rgba_weights[1] = opts->rgba_weights[1];
		ewp.rgba_weights[2] = opts->rgba_weights[2];
		ewp.rgba_weights[3] = opts->rgba_weights[3];
	}

	ewp.max_refinement_iters = opts->quality.max_iters;
	ewp.block_mode_cutoff = (float)opts->quality.block_mode_cutoff / 100.0f;
	ewp.partition_1_to_2_limit = opts->quality.oplimit;
	ewp.lowest_correlation_cutoff = opts->quality.mincorrel;
	ewp.partition_search_limit = opts->quality.partitions_to_test;
	if (ewp.partition_search_limit > (1 << 10)) ewp.partition_search_limit = (1 << 10);

	if (opts->normal_map) {
		ewp.ra_normal_angular_scale = 1;
		ewp.partition_1_to_2_limit = 1000.0f;
		ewp.lowest_correlation_cutoff = 0.99f;
	}

	float avg_texel_error = powf(0.1f, opts->quality.dblimit * 0.1f) * 65535.0f * 65535.0f;
	ewp.texel_avg_error_limit = avg_texel_error;

	float max_color_component_weight = MAX(MAX(ewp.rgba_weights[0], ewp.rgba_weights[1]),
										   MAX(ewp.rgba_weights[2], ewp.rgba_weights[3]));
	ewp.rgba_weights[0] = MAX(ewp.rgba_weights[0], max_color_component_weight / 1000.0f);
	ewp.rgba_weights[1] = MAX(ewp.rgba_weights[1], max_color_component_weight / 1000.0f);
	ewp.rgba_weights[2] = MAX(ewp.rgba_weights[2], max_color_component_weight / 1000.0f);
	ewp.rgba_weights[3] = MAX(ewp.rgba_weights[3], max_color_component_weight / 1000.0f);

	int padding = MAX(ewp.mean_stdev_radius, ewp.alpha_radius);

	astc_codec_image *input_image = astc_img_from_unorm8x4_array(src, width, height, padding, 0);
	if (!input_image) return false;

	int xsize = input_image->xsize;
	int ysize = input_image->ysize;
	int zsize = input_image->zsize;

	int xblocks = (xsize + xdim - 1) / xdim;
	int yblocks = (ysize + ydim - 1) / ydim;
	int zblocks = (zsize + zdim - 1) / zdim;

	expand_block_artifact_suppression(xdim, ydim, zdim, &ewp);

	int linearize_srgb = 0;

	if (padding > 0 ||
		ewp.rgb_mean_weight != 0.0f || ewp.rgb_stdev_weight != 0.0f ||
		ewp.alpha_mean_weight != 0.0f || ewp.alpha_stdev_weight != 0.0f)
	{
		// Clamp texels outside the actual image area.
	fill_image_padding_area(input_image);

	compute_averages_and_variances(
		input_image,
		ewp.rgb_power,
		ewp.alpha_power,
		ewp.mean_stdev_radius,
		ewp.alpha_radius,
		linearize_srgb,
		swz_encode,
		opts->num_threads);
	}

	astc_decode_mode mode = DECODE_LDR_SRGB;
	if (opts->linear) mode = DECODE_LDR;

	// print all encoding settings unless specifically told otherwise.
	if (opts->verbose)
	{
		printf("ASTC Encoding settings:\n");
		printf("  3D Block size: %dx%dx%d (%.2f bpp)\n", xdim, ydim, zdim, 128.0 / (xdim* ydim* zdim));
		printf("  Radius for mean-and-stdev calculations: %d texels\n", ewp.mean_stdev_radius);
		printf("  RGB power: %g\n", (double)ewp.rgb_power);
		printf("  RGB base-weight: %g\n", (double)ewp.rgb_base_weight);
		printf("  RGB local-mean weight: %g\n", (double)ewp.rgb_mean_weight);
		printf("  RGB local-stdev weight: %g\n", (double)ewp.rgb_stdev_weight);
		printf("  RGB mean-and-stdev mixing across color channels: %g\n", (double)ewp.rgb_mean_and_stdev_mixing);
		printf("  Alpha power: %g\n", (double)ewp.alpha_power);
		printf("  Alpha base-weight: %g\n", (double)ewp.alpha_base_weight);
		printf("  Alpha local-mean weight: %g\n", (double)ewp.alpha_mean_weight);
		printf("  Alpha local-stdev weight: %g\n", (double)ewp.alpha_stdev_weight);
		printf("  RGB weights scale with alpha: ");
		if (ewp.enable_rgb_scale_with_alpha)
			printf("enabled (radius=%d)\n", ewp.alpha_radius);
		else
			printf("disabled\n");
		printf("  Color channel relative weighting: R=%g G=%g B=%g A=%g\n", (double)ewp.rgba_weights[0], (double)ewp.rgba_weights[1], (double)ewp.rgba_weights[2], (double)ewp.rgba_weights[3]);
		printf("  Block-artifact suppression parameter : %g\n", (double)ewp.block_artifact_suppression);
		printf("  Number of distinct partitionings to test: %d\n", ewp.partition_search_limit);
		printf("  PSNR decibel limit: 2D: %f\n", opts->quality.dblimit);
		printf("  1->2 partition limit: %f\n", (double)ewp.partition_1_to_2_limit);
		printf("  Dual-plane color-correlation cutoff: %f\n", (double)ewp.lowest_correlation_cutoff);
		printf("  Block Mode Percentile Cutoff: %f\n", (double)(ewp.block_mode_cutoff * 100.0f));
		printf("  Max refinement iterations: %d\n", ewp.max_refinement_iters);
	}

	encode_astc_image(input_image, NULL, xdim, ydim, zdim, &ewp, mode,
		swz_encode, swz_decode, dst, 0, opts->num_threads, opts->progress_fn, opts->progress_user);

	free_image(input_image);

	return true;
}
