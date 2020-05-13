#define _CRT_SECURE_NO_WARNINGS
#define RGBCX_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION

#include "stb_image.h"
#include "stb_image_resize.h"
#include "bc7enc.h"
#include "rgbcx.h"
#include "astcenc.h"
#include "image.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>
#include <assert.h>
#include <thread>
#include <atomic>
#include <vector>

void failf(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);

	vfprintf(stderr, fmt, args);
	putc('\n', stderr);

	va_end(args);

	exit(1);
}

#define array_size(arr) (sizeof(arr) / sizeof(*(arr)))

typedef enum format_enum {
	FORMAT_RGBA8,
	FORMAT_BC1,
	FORMAT_BC3,
	FORMAT_BC4,
	FORMAT_BC5,
	FORMAT_BC7,
	FORMAT_ASTC_4X4,

	FORMAT_COUNT,
	FORMAT_ERROR = 0x7fffffff,
} format_enum;

typedef struct pixel_format {
	char magic[4];
	format_enum format;
	int block_width;
	int block_height;
	int block_size;
	const char *name;
	const char *description;
} pixel_format;

const pixel_format format_list[] = {
	{ { 'r','a','8',' ' }, FORMAT_RGBA8, 1,1,4, "rgba8", "Uncompressed 8-bits per channel" },
	{ { 'b','c','1',' ' }, FORMAT_BC1, 4,4,8, "bc1", "RGB Direct3D Block Compression" },
	{ { 'b','c','3',' ' }, FORMAT_BC3, 4,4,16, "bc3", "RGB+A Direct3D Block Compression" },
	{ { 'b','c','4',' ' }, FORMAT_BC4, 4,4,8, "bc4", "LDR R Direct3D Block Compression" },
	{ { 'b','c','5',' ' }, FORMAT_BC5, 4,4,16, "bc5", "R+G Direct3D Block Compression" },
	{ { 'b','c','7',' ' }, FORMAT_BC7, 4,4,16, "bc7", "RGB(+A) Direct3D Block Compression" },
	{ { 'a','s','4','4' }, FORMAT_ASTC_4X4, 4,4,16, "astc4x4", "RGB(+A) ASTC Compression (4x4 blocks)" },
};

typedef enum container_enum {
	CONTAINER_NONE,
	CONTAINER_SPTEX,
	CONTAINER_DDS,
	CONTAINER_KTX,

	CONTAINER_COUNT,
	CONTAINER_ERROR = 0x7fffffff,
} container_enum;

typedef struct container_type {
	const char *name;
	const char *extension;
	container_enum type;
	const char *description;
} container_type;

const container_type container_list[] = {
	{ "none", NULL, CONTAINER_NONE, "Raw mipmap data wihtout any headers" },
	{ "sptex", ".sptex", CONTAINER_SPTEX, "SP-tools container" },
	{ "dds", ".dds", CONTAINER_DDS, "DirectDraw Surface" },
	{ "ktx", ".ktx", CONTAINER_KTX, "Khronos KTX container" },
};

typedef struct edge_mode {
	const char *name;
	stbir_edge value;
} edge_mode;

static const edge_mode edge_list[] = {
	{ "", (stbir_edge)0 },
	{ "clamp", STBIR_EDGE_CLAMP },
	{ "reflect", STBIR_EDGE_REFLECT },
	{ "wrap", STBIR_EDGE_WRAP },
	{ "zero", STBIR_EDGE_ZERO },
};

typedef struct filter_mode {
	const char *name;
	stbir_filter value;
} filter_mode;

static const filter_mode filter_list[] = {
	{ "default", STBIR_FILTER_DEFAULT },
	{ "box", STBIR_FILTER_BOX },
	{ "triangle", STBIR_FILTER_TRIANGLE },
	{ "b-spline", STBIR_FILTER_CUBICBSPLINE },
	{ "catmull-rom", STBIR_FILTER_CATMULLROM },
	{ "mitchell", STBIR_FILTER_MITCHELL },
};

typedef struct resize_opts {
	stbir_edge edge_h, edge_v;
	stbir_filter filter;
	int flags;
	bool linear;
} resize_opts;

format_enum parse_format(const char *name)
{
	for (size_t i = 0; i < array_size(format_list); i++) {
		if (!strcmp(name, format_list[i].name)) {
			return format_list[i].format;
		}
	}
	failf("Unsupported format: %s", name);
	return FORMAT_ERROR;
}

container_enum parse_container(const char *name)
{
	for (size_t i = 0; i < array_size(format_list); i++) {
		if (!strcmp(name, container_list[i].name)) {
			return container_list[i].type;
		}
	}
	failf("Unsupported container: %s", name);
	return CONTAINER_ERROR;
}

stbir_edge parse_edge(const char *name)
{
	for (size_t i = 1; i < array_size(edge_list); i++) {
		if (!strcmp(name, edge_list[i].name)) {
			return edge_list[i].value;
		}
	}
	failf("Unsupported edge mode: %s", name);
	return STBIR_EDGE_CLAMP;
}

stbir_filter parse_filter(const char *name)
{
	for (size_t i = 0; i < array_size(filter_list); i++) {
		if (!strcmp(name, filter_list[i].name)) {
			return filter_list[i].value;
		}
	}
	failf("Unsupported filter: %s", name);
	return STBIR_FILTER_DEFAULT;
}

static void image_resize(resize_opts opts, uint8_t *dst, int dst_width, int dst_height, const uint8_t *src, int src_width, int src_height)
{
	stbir_resize(
		src, src_width, src_height, 0,
		dst, dst_width, dst_height, 0,
		STBIR_TYPE_UINT8, 4, 3, opts.flags,
		opts.edge_h, opts.edge_v,
		opts.filter, opts.filter,
		opts.linear ? STBIR_COLORSPACE_LINEAR : STBIR_COLORSPACE_SRGB,
		NULL);

}

typedef struct mip_data {
	uint8_t *data;
	size_t data_offset;
	size_t data_size;
	int width;
	int height;
	int blocks_x;
	int blocks_y;
} mip_data;

static const uint32_t level_to_rgbcx[] = {
	~0u,
	0,1,2,3,4,5,6,7,8,9,10,
	10,11,12,13,14,15,16,17,18
};

bc7enc_compress_block_params level_to_bc7_params[] = {
	{ 0 }, // 0 (invalid)
	{  0, {0,0,0,0}, 0, 0, 1, 1, 0, 0, }, // 1
	{  2, {0,0,0,0}, 0, 0, 1, 1, 0, 0, }, // 2
	{  4, {0,0,0,0}, 0, 0, 0, 1, 0, 0, }, // 3
	{  6, {0,0,0,0}, 0, 0, 0, 1, 0, 0, }, // 4
	{  8, {0,0,0,0}, 0, 0, 0, 1, 0, 0, }, // 5
	{ 10, {0,0,0,0}, 0, 0, 0, 1, 0, 0, }, // 6
	{ 14, {0,0,0,0}, 0, 0, 0, 1, 1, 1, }, // 7
	{ 18, {0,0,0,0}, 0, 0, 0, 1, 1, 1, }, // 8
	{ 22, {0,0,0,0}, 0, 0, 0, 1, 1, 1, }, // 9
	{ 26, {0,0,0,0}, 0, 0, 0, 1, 1, 1, }, // 10
	{ 30, {0,0,0,0}, 0, 0, 0, 1, 1, 1, }, // 11
	{ 34, {0,0,0,0}, 0, 0, 0, 1, 1, 1, }, // 12
	{ 38, {0,0,0,0}, 0, 0, 0, 1, 1, 1, }, // 13
	{ 42, {0,0,0,0}, 0, 0, 0, 1, 1, 1, }, // 14
	{ 46, {0,0,0,0}, 0, 0, 0, 1, 1, 1, }, // 15
	{ 50, {0,0,0,0}, 0, 0, 0, 1, 1, 1, }, // 16
	{ 54, {0,0,0,0}, 1, 0, 0, 1, 1, 1, }, // 17
	{ 58, {0,0,0,0}, 2, 0, 0, 1, 1, 1, }, // 18
	{ 62, {0,0,0,0}, 3, 0, 0, 1, 1, 1, }, // 19
	{ 64, {0,0,0,0}, 4, 0, 0, 1, 1, 1, }, // 20
};

static void fetch_4x4(uint8_t dst[4*4*4], const uint8_t *src, int width, int height, int block_x, int block_y)
{
	int x = block_x * 4, y = block_y * 4;
	for (int row = 0; row < 4; row++) {
		const uint8_t *line;
		if (y + row < height) {
			line = src + (y + row)*width*4;
		} else {
			line = src + (height - 1)*width*4;
		}

		uint8_t *d = dst + row * (4*4);
		if (x + 4 < width) {
			memcpy(d, line + x * 4, 16);
		} else {
			for (int col = 0; col < 4; col++) {
				int si = x + col < width ? x + col : width - 1;
				const uint8_t *s = line + si * 4;
				d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
			}
		}
	}
}

template <typename F>
static void parallel_for(int num_threads, int num, F f) {
	if (num_threads > num) num_threads = num;
	if (num_threads <= 1 || num <= 1) {
		for (int i = 0; i < num; i++) {
			f(i);
		}
	} else {
		std::vector<std::thread> threads;
		threads.reserve(num_threads - 1);
		std::atomic_int a_index = 0;
		for (int thread_i = 0; thread_i < num_threads - 1; thread_i++) {
			threads.push_back(std::thread([&]() {
				for (;;) {
					int index = a_index.fetch_add(1, std::memory_order_relaxed);
					if (index > num) return;
					f(index);
				}
			}));
		}

		for (;;) {
			int index = a_index.fetch_add(1, std::memory_order_relaxed);
			if (index > num) return;
			f(index);
		}

		for (std::thread &thread : threads) {
			thread.join();
		}
	}
}

static void write_data(FILE *f, const void *data, size_t size)
{
	size_t num = fwrite(data, 1, size, f);
	if (num != size) {
		fclose(f);
		failf("Failed to write output data");
	}
}

static void write_mips(FILE *f, const mip_data *mips, int num_mips)
{
	for (int i = 0; i < num_mips; i++) {
		write_data(f, mips[i].data, mips[i].data_size);
	}
}

typedef struct sptex_mip {
	uint32_t width, height;
	uint32_t data_offset;
	uint32_t data_size;
} sptex_mip;

typedef struct sptex_header {
	char file_magic[4];
	uint32_t file_version;
	char fmt_magic[4];
	uint32_t file_size;
	uint32_t width, height;
	uint32_t uncropped_width, uncropped_height;
	uint32_t crop_min_x, crop_min_y;
	uint32_t crop_max_x, crop_max_y;
	uint32_t num_mips;
	sptex_mip mips[16];
} sptex_header;

typedef struct dds_header {
	char magic[4];
	uint32_t size;
	uint32_t flags;
	uint32_t height;
	uint32_t width;
	uint32_t pitch_or_linear_size;
	uint32_t depth;
	uint32_t mip_map_count;
	uint32_t reserved[11];
	uint32_t pixelformat_size;
	uint32_t pixelformat_flags;
	char pixelformat_fourcc[4];
	uint32_t pixelformat_bitcount;
	uint32_t pixelformat_r_mask;
	uint32_t pixelformat_g_mask;
	uint32_t pixelformat_b_mask;
	uint32_t pixelformat_a_mask;
	uint32_t caps[4];
	uint32_t reserved2;
	uint32_t dxgi_format;
	uint32_t resource_dimension;
	uint32_t misc_flag;
	uint32_t array_size;
	uint32_t misc_flags2;
} dds_header;

int main(int argc, char **argv)
{
	int max_extent = -1;
	int max_mips = -1;
	const char *input_file = NULL;
	const char *output_file = NULL;
	format_enum format = FORMAT_ERROR;
	bool verbose = false;
	bool show_help = argc <= 1;
	bool crop_alpha = false;
	bool premultiply = false;
	bool output_ignores_alpha = false;
	int res_width = -1;
	int res_height = -1;
	int level = 10;
	int num_threads = 1;
	resize_opts res_opts = { STBIR_EDGE_CLAMP, STBIR_EDGE_CLAMP, STBIR_FILTER_DEFAULT };
	rgbcx::bc1_approx_mode bc1_approx = rgbcx::bc1_approx_mode::cBC1Ideal;
	container_enum container = CONTAINER_ERROR;

	assert(array_size(format_list) == (size_t)FORMAT_COUNT);

	// -- Parse arguments

	for (int argi = 1; argi < argc; argi++) {
		const char *arg = argv[argi];
		int left = argc - argi - 1;

		if (!strcmp(arg, "-v") || !strcmp(arg, "--verbose")) {
			verbose = true;
		} else if (!strcmp(arg, "--help")) {
			show_help = true;
		} else if (!strcmp(arg, "--crop-alpha")) {
			crop_alpha = true;
		} else if (!strcmp(arg, "--linear")) {
			res_opts.linear = true;
		} else if (!strcmp(arg, "--premultiply")) {
			premultiply = true;
		} else if (!strcmp(arg, "--output-ignores-alpha")) {
			output_ignores_alpha = true;
		} else if (left >= 1) {
			if (left >= 2 && !strcmp(arg, "--resolution")) {
				res_width = atoi(argv[++argi]);
				res_height = atoi(argv[++argi]);
			} else if (!strcmp(arg, "-i") || !strcmp(arg, "--input")) {
				input_file = argv[++argi];
			} else if (!strcmp(arg, "-o") || !strcmp(arg, "--output")) {
				output_file = argv[++argi];
			} else if (!strcmp(arg, "-f") || !strcmp(arg, "--format")) {
				format = parse_format(argv[++argi]);
			} else if (!strcmp(arg, "--level")) {
				level = atoi(argv[++argi]);
				if (level <= 0 || level > 20) {
					failf("Invalid level %d, must be between 1-20", level);
				}
			} else if (!strcmp(arg, "--max-extent")) {
				max_extent = atoi(argv[++argi]);
			} else if (!strcmp(arg, "--width")) {
				max_extent = atoi(argv[++argi]);
			} else if (!strcmp(arg, "--max-mips")) {
				max_mips = atoi(argv[++argi]);
			} else if (!strcmp(arg, "--edge")) {
				res_opts.edge_h = res_opts.edge_v = parse_edge(argv[++argi]);
			} else if (!strcmp(arg, "--edge-h")) {
				res_opts.edge_h = parse_edge(argv[++argi]);
			} else if (!strcmp(arg, "--edge-v")) {
				res_opts.edge_v = parse_edge(argv[++argi]);
			} else if (!strcmp(arg, "--filter")) {
				res_opts.filter = parse_filter(argv[++argi]);
			} else if (!strcmp(arg, "--target")) {
				const char *target = argv[++argi];
				if (!strcmp(target, "amd")) {
					bc1_approx = rgbcx::bc1_approx_mode::cBC1AMD;
				}
				if (!strcmp(target, "nvidia")) {
					bc1_approx = rgbcx::bc1_approx_mode::cBC1NVidia;
				}
			} else if (!strcmp(arg, "-j") || !strcmp(arg, "--threads")) {
				num_threads = atoi(argv[++argi]);
				if (num_threads <= 0 || num_threads > 10000) failf("Bad number of threads: %d");
			}
		}
	}

	if (show_help) {
		printf("%s",
			"Usage: sf-texcomp -i <input> -o <output> -f <format> [options]\n"
			"    -i / --input <path>: Input filename in any format stb_image supports\n"
			"    -o / --output <path>: Destination filename\n"
			"    -f / --format <format>: Compressed texture pixel format (see below)\n"
			"    -c / --container <type>: Output container format (detected from filename if absent, see below)\n"
			"    -j / --threads <num>: Number of threads to use\n"
			"    -v / --verbose: Verbose output\n"
			"    --level <level>: Compression level 1-20 (default 10)\n"
			"    --max-extent <extent>: Clamp the resolution of the image in pixels\n"
			"                  Maintains aspect ratio.\n"
			"    --resolution <width> <height>: Force output resolution to a specific size\n"
			"                          Will resize the image larger if necessary\n"
			"    --max-mips <num>: Maximum number of mipmaps to generate\n"
			"    --crop-alpha: Crop the transparent areas around the image\n"
			"    --linear: Treat the data as linear instead of sRGB\n"
			"    --premultiply: Premultiply the input RGB by alpha\n"
			"    --edge <mode>: Edge addressing mode (clamp, reflect, wrap, zero)\n"
			"    --edge-h <mode>: Horizontal edge addressing mode (clamp, reflect, wrap, zero)\n"
			"    --edge-v <mode>: Vertical edge addressing mode (clamp, reflect, wrap, zero)\n"
			"    --filter <filter>: Filtering mode (default, box, triangle, b-spline, catmull-rom, mitchell)\n"
			"    --target <target>: Target GPU to optimize for (amd, nvidia)\n"
			"    --output-ignores-alpha: The output textures may have alpha even for opaque colors\n"
		);

		printf("Supported formats:\n");
		for (size_t i = 0; i < array_size(format_list); i++) {
			const pixel_format *fmt = &format_list[i];
			printf("  %8s: %s\n", fmt->name, fmt->description);
		}

		printf("Supported containers:\n");
		for (size_t i = 0; i < array_size(container_list); i++) {
			const container_type *type = &container_list[i];
			if (type->extension) {
				printf("  %8s: %s %s\n", type->name, type->extension, type->description);
			} else {
				printf("  %8s: %s\n", type->name, type->description);
			}
		}

		return 0;
	}

	// -- Validate arguments

	if (!input_file) failf("Input file required: -i <input>");
	if (!output_file) failf("Output file required: -o <output>");
	if (format == FORMAT_ERROR) failf("Format required: -f <format> (see --help for available formats)");
	if (max_extent == 0) failf("Maximum extent can't be zero, don't specify anything or use -1 to disable");
	if (max_extent < 0) max_extent = -1;
	if (max_mips == 0) failf("Maximum mipmap count can't be zero, don't specify anything or use -1 to disable");
	if (max_mips < 0) max_mips = -1;
	if (res_width == 0) failf("Output resolution width is zero");
	if (res_height == 0) failf("Output resolution height is zero");

	if (premultiply) res_opts.flags |= STBIR_FLAG_ALPHA_PREMULTIPLIED;

	// -- Guesstimate container from filename

	if (container == CONTAINER_ERROR) {
		// Find the rightmost matching extension
		const char *best_pos = NULL;
		for (size_t i = 0; i < array_size(container_list); i++) {
			const char *ext = container_list[i].extension;
			if (!ext) continue;
			for (const char *pos = output_file; (pos = strstr(pos, ext)) != NULL; pos++) {
				if (pos > best_pos) {
					best_pos = pos;
					container = (container_enum)i;
				}
			}
		}

		if (container == CONTAINER_ERROR) {
			failf("Could not identify container format from output filename.\n"
				"Specify one explicitly using --container <format>\n");
		}
	}

	if (verbose) {
		printf("input_file: %s\n", input_file);
		printf("output_file: %s\n", output_file);
		printf("format: %s\n", format_list[format].name);
		printf("container: %s\n", container_list[container].name);
		printf("level: %d\n", level);
		printf("max_extent: %d\n", max_extent);
		printf("crop_alpha: %s\n", crop_alpha ? "true" : "false");
		printf("linear: %s\n", res_opts.linear ? "true" : "false");
		printf("premultiply: %s\n", premultiply ? "true" : "false");
		printf("edge_h: %s\n", edge_list[res_opts.edge_h].name);
		printf("edge_v: %s\n", edge_list[res_opts.edge_v].name);
		printf("filter: %s\n", filter_list[res_opts.filter].name);
	}

	// -- Load image data

	int input_width, input_height;
	uint8_t *pixels = (uint8_t*)stbi_load(input_file, &input_width, &input_height, NULL, 4);
	if (!pixels) failf("Failed to load input file: %s", input_file);
	if (verbose) {
		printf("Loaded input file: %dx%d\n", input_width, input_height);
	}

	// -- Premultiply input data if necessary

	if (premultiply) {
		if (verbose) {
			printf("Premultiplying input data (--premultiply)\n");
		}

		premultiply_alpha(pixels, input_width, input_height);
	}

	// -- Resize input data

	int original_width = input_width, original_height = input_height;

	if (res_width > 0 && res_height > 0 && (res_width != input_width || res_height != input_height)) {
		input_width = res_width;
		input_height = res_height;

		if (verbose) {
			printf("Resizing from %dx%d to %dx%d (--resolution)\n",
				original_width, original_height,
				input_width, input_height);
		}
	} else if (max_extent > 0 && (input_width > max_extent || input_height > max_extent)) {
		if (input_width > input_height) {
			input_height = (int)(max_extent * ((double)input_height / (double)input_width));
			input_width = max_extent;
		} else {
			input_width = (int)(max_extent * ((double)input_width / (double)input_height));
			input_height = max_extent;
		}

		if (verbose) {
			printf("Resizing from %dx%d to %dx%d (--max-extent %d)\n",
				original_width, original_height,
				input_width, input_height,
				max_extent);
		}
	}

	if (input_width != original_width || input_height != original_height) {
		uint8_t *new_pixels = (uint8_t*)malloc((size_t)input_width * (size_t)input_height * 4);
		if (!new_pixels) failf("Failed to allocate memory for resize target");

		image_resize(res_opts, new_pixels, input_width, input_height, pixels, original_width, original_height);
		
		free(pixels);
		pixels = new_pixels;
	}

	// -- Crop alpha

	crop_rect input_rect = {
		0, 0, input_width, input_height,
	};

	int uncropped_width = input_width, uncropped_height = input_height;
	if (crop_alpha) {
		input_rect = get_crop_rect(pixels, input_width, input_height);
		if (verbose) {
			printf("Cropping to (%d,%d), (%d,%d) (--crop-alpha)\n",
				input_rect.min_x, input_rect.min_y,
				input_rect.max_x, input_rect.max_y);
		}

		apply_crop_rect(pixels, &input_width, &input_height, input_rect);
	}

	// -- Generate mips and compress

	uint8_t *mip_resize_pixels = NULL;

	pixel_format fmt = format_list[format];
	assert(fmt.format == format);

	int mip_width = input_width, mip_height = input_height;
	int num_mips = 0;
	mip_data mips[32];

	switch (format) {

	case FORMAT_BC1:
	case FORMAT_BC3:
	case FORMAT_BC4:
	case FORMAT_BC5:
		rgbcx::init(bc1_approx);
		break;

	case FORMAT_BC7:
		bc7enc_compress_block_init();
		break;

	}

	size_t mip_data_offset = 0;
	while (max_mips <= 0 || num_mips < max_mips) {
		int mip_ix = num_mips++;

		uint8_t *mip_pixels;
		if (mip_width != input_width || mip_height != input_height) {
			if (verbose) {
				printf("Resizing mip %d (%dx%d)\n", mip_ix, mip_width, mip_height);
			}

			if (!mip_resize_pixels) {
				mip_resize_pixels = (uint8_t*)malloc((size_t)mip_width * (size_t)mip_height * 4);
				if (!mip_resize_pixels) failf("Failed to allocate memory for mip resize target");
			}

			image_resize(res_opts, mip_resize_pixels, mip_width, mip_height, pixels, input_width, input_height);
			mip_pixels = mip_resize_pixels;
		} else {
			mip_pixels = pixels;
		}

		mip_data *mip = &mips[mip_ix];
		mip->width = mip_width;
		mip->height = mip_height;
		mip->blocks_x = (mip_width + fmt.block_width - 1) / fmt.block_width;
		mip->blocks_y = (mip_height + fmt.block_height - 1) / fmt.block_height;
		mip->data_offset = mip_data_offset;
		mip->data_size = (size_t)mip->blocks_x * (size_t)mip->blocks_y * (size_t)fmt.block_size;
		mip_data_offset += mip->data_size;

		if (verbose) {
			printf("Compressing mip %d (%dx%d)\n", mip_ix, mip_width, mip_height);
		}

		mip->data = (uint8_t*)malloc(mip->data_size);
		if (!mip->data) failf("Failed to allocate memory for compressed data");

		size_t block_stride = mip->blocks_x * fmt.block_size;

		int blocks_x = mip->blocks_x, blocks_y = mip->blocks_y;
		switch (format) {

		case FORMAT_RGBA8: {
			memcpy(mip->data, mip_pixels, mip->data_size);
		} break;

		case FORMAT_BC1: {
			uint32_t l = level_to_rgbcx[level];
			parallel_for(num_threads, blocks_y, [&](int y) {
				uint8_t *dst = mip->data + (size_t)y * block_stride;
				for (int x = 0; x < blocks_x; x++) {
					uint8_t src[4*4*4];
					fetch_4x4(src, mip_pixels, mip_width, mip_height, x, y);
					rgbcx::encode_bc1(l, dst, src, true, output_ignores_alpha);
					dst += 8;
				}
			});
		} break;

		case FORMAT_BC3: {
			uint32_t l = level_to_rgbcx[level];
			parallel_for(num_threads, blocks_y, [&](int y) {
				uint8_t *dst = mip->data + (size_t)y * block_stride;
				for (int x = 0; x < blocks_x; x++) {
					uint8_t src[4*4*4];
					fetch_4x4(src, mip_pixels, mip_width, mip_height, x, y);
					rgbcx::encode_bc3(l, dst, src);
					dst += 16;
				}
			});
		} break;

		case FORMAT_BC4: {
			uint32_t l = level_to_rgbcx[level];
			parallel_for(num_threads, blocks_y, [&](int y) {
				uint8_t *dst = mip->data + (size_t)y * block_stride;
				for (int x = 0; x < blocks_x; x++) {
					uint8_t src[4*4*4];
					fetch_4x4(src, mip_pixels, mip_width, mip_height, x, y);
					rgbcx::encode_bc4(dst, src);
					dst += 8;
				}
			});
		} break;

		case FORMAT_BC5: {
			uint32_t l = level_to_rgbcx[level];
			parallel_for(num_threads, blocks_y, [&](int y) {
				uint8_t *dst = mip->data + (size_t)y * block_stride;
				for (int x = 0; x < blocks_x; x++) {
					uint8_t src[4*4*4];
					fetch_4x4(src, mip_pixels, mip_width, mip_height, x, y);
					rgbcx::encode_bc5(dst, src);
					dst += 16;
				}
			});
		} break;

		case FORMAT_BC7: {
			bc7enc_compress_block_params bc7_params = level_to_bc7_params[level];
			if (res_opts.linear) {
				bc7enc_compress_block_params_init_linear_weights(&bc7_params);
			} else {
				bc7enc_compress_block_params_init_perceptual_weights(&bc7_params);
			}

			parallel_for(num_threads, blocks_y, [&](int y) {
				uint8_t *dst = mip->data + (size_t)y * block_stride;
				for (int x = 0; x < blocks_x; x++) {
					uint8_t src[4*4*4];
					fetch_4x4(src, mip_pixels, mip_width, mip_height, x, y);
					bc7enc_compress_block(dst, src, &bc7_params);
					dst += 16;
				}
			});
		} break;

		case FORMAT_ASTC_4X4: {
			astcenc_opts opts = { 0 };
			opts.linear = res_opts.linear;
			opts.num_threads = num_threads;
			opts.block_width = fmt.block_width;
			opts.block_height = fmt.block_height;

			if (!astcenc_encode_image(&opts, mip->data, mip_pixels, mip_width, mip_height)) {
				failf("Failed to allocate memory for ASTC source image");
			}
		} break;

		}

		if (mip_width == 1 && mip_height == 1) break;
		mip_width = mip_width > 1 ? mip_width / 2 : 1;
		mip_height = mip_height > 1 ? mip_height / 2 : 1;
	}

	free(mip_resize_pixels);
	free(pixels);

	// TODO: Windows UTF-16
	FILE *f = fopen(output_file, "wb");
	if (!f) failf("Failed to open output file: %s", output_file);

	switch (container) {

	case CONTAINER_NONE: {
		write_mips(f, mips, num_mips);
	} break;

	case CONTAINER_SPTEX: {
		if (num_mips > 16) {
			failf("sptex supports only up to 16 mip levels");
		}

		sptex_header header;
		memcpy(header.file_magic, "sptx", 4);
		header.file_version = 1;
		header.file_size = sizeof(sptex_header) + mip_data_offset;
		memcpy(header.fmt_magic, fmt.magic, 4);
		header.width = (uint32_t)input_width;
		header.height = (uint32_t)input_height;
		header.uncropped_width = (uint32_t)uncropped_width;
		header.uncropped_height = (uint32_t)uncropped_height;
		header.crop_min_x = (uint32_t)input_rect.min_x;
		header.crop_min_y = (uint32_t)input_rect.min_y;
		header.crop_max_x = (uint32_t)input_rect.max_x;
		header.crop_max_y = (uint32_t)input_rect.max_y;
		header.num_mips = num_mips;
		for (int i = 0; i < 16; i++) {
			sptex_mip *mip = &header.mips[i];
			if (i < num_mips) {
				mip->data_size = mips[i].data_size;
				mip->data_offset = mips[i].data_offset;
				mip->width = mips[i].width;
				mip->height = mips[i].height;
			} else {
				memset(mip, 0, sizeof(sptex_mip));
			}
		}

		write_data(f, &header, sizeof(header));
		write_mips(f, mips, num_mips);

	} break;

	case CONTAINER_DDS: {

		dds_header header = { 0 };
		memcpy(header.magic, "DDS ", 4);
		header.size = 124;
		header.flags = 0xa1007; // CAPS|HEIGHT|WIDTH|PIXELFORMAT|MIPMAPCOUNT|LINEARSIZE
		header.height = (uint32_t)input_height;
		header.width = (uint32_t)input_width;
		header.pitch_or_linear_size = (uint32_t)mips[0].data_size;
		header.depth = 0;
		header.mip_map_count = (uint32_t)num_mips;
		if (format == FORMAT_RGBA8) {
			header.pixelformat_flags = 0x41; // RGB|ALPHAPIXELS
			header.pixelformat_bitcount = 32;
			header.pixelformat_r_mask = 0x000000ff;
			header.pixelformat_g_mask = 0x0000ff00;
			header.pixelformat_b_mask = 0x00ff0000;
			header.pixelformat_a_mask = 0xff000000;
		} else {
			header.pixelformat_flags = 0x4; // FOURCC
		}
		header.pixelformat_size = 32;
		memcpy(header.pixelformat_fourcc, "DX10", 4);
		switch (format) {
		case FORMAT_RGBA8: header.dxgi_format = res_opts.linear ? 28 : 29; break; // R8G8B8A8_UNORM(_SRGB)
		case FORMAT_BC1: header.dxgi_format = res_opts.linear ? 71 : 72; break; // BC1_UNORM(_SRGB)
		case FORMAT_BC3: header.dxgi_format = res_opts.linear ? 77 : 78; break; // BC3_UNORM(_SRGB)
		case FORMAT_BC4: header.dxgi_format = 80; break; // BC4_UNORM
		case FORMAT_BC5: header.dxgi_format = 83; break; // BC5_UNORM TODO: snorm?
		case FORMAT_BC7: header.dxgi_format = res_opts.linear ? 98 : 99; break; // BC7_UNORM(_SRGB)
		default: header.dxgi_format = 0; break;
		}
		header.resource_dimension = 3; // D3D10_RESOURCE_DIMENSION_TEXTURE2D
		header.array_size = 1;

		write_data(f, &header, sizeof(header));
		write_mips(f, mips, num_mips);

	} break;

	}

	if (fclose(f) != 0) {
		failf("Failed to flush output file");
	}

	for (int i = 0; i < num_mips; i++) {
		free(mips[i].data);
	}

	return 0;
}
