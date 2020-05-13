#include "astcenc.h"
#include "astc/astc_codec_internals.h"

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
	int threadcount
);

bool astcenc_encode_image(const astcenc_opts *opts, uint8_t *dst, const uint8_t *src, int width, int height)
{
	astc_codec_image *input_image = astc_img_from_unorm8x4_array(src, width, height, 0, 0);
	if (!input_image) return false;

	int xdim = opts->block_width;
	int ydim = opts->block_height;
	int zdim = 1;

	swizzlepattern swz_encode = { 0,1,2,3 };
	swizzlepattern swz_decode = { 0,1,2,3 };
	// TODO: Normal map swizzle

	int xsize = input_image->xsize;
	int ysize = input_image->ysize;
	int zsize = input_image->zsize;

	int xblocks = (xsize + xdim - 1) / xdim;
	int yblocks = (ysize + ydim - 1) / ydim;
	int zblocks = (zsize + zdim - 1) / zdim;

	error_weighting_params ewp;

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

	astc_decode_mode mode = DECODE_LDR_SRGB;
	if (opts->linear) mode = DECODE_LDR;

	encode_astc_image(input_image, NULL, xdim, ydim, zdim, &ewp, mode,
		swz_encode, swz_decode, dst, 0, opts->num_threads);

	free_image(input_image);

	return true;
}
