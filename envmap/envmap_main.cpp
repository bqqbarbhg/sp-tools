#define _CRT_SECURE_NO_WARNINGS
#define TINYEXR_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define NOMINMAX

#include "tinyexr.h"
#include "sp_tools_common.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>
#include <assert.h>
#include <string.h>
#include <thread>
#include <atomic>
#include <vector>

static const double PI = 3.14159265358979323846;

struct vec2 {
	double x, y;
	vec2() : x(0.0f), y(0.0f) { }
	vec2(double v) : x(v), y(v) { }
	vec2(double x, double y) : x(x), y(y) { }
};

struct vec3 {
	double x, y, z;
	vec3() : x(0.0f), y(0.0f), z(0.0f) { }
	vec3(double v) : x(v), y(v), z(v) { }
	vec3(double x, double y, double z) : x(x), y(y), z(z) { }
	vec3(const vec2 &v, double z) : x(v.x), y(v.y), z(z) { }
};

inline vec2 operator+(const vec2 &a, const vec2 &b) { return vec2{ a.x + b.x, a.y + b.y }; }
inline vec2 operator-(const vec2 &a, const vec2 &b) { return vec2{ a.x - b.x, a.y - b.y }; }
inline vec2 operator*(const vec2 &a, const vec2 &b) { return vec2{ a.x * b.x, a.y * b.y }; }
inline vec2 operator/(const vec2 &a, const vec2 &b) { return vec2{ a.x / b.x, a.y / b.y }; }
inline vec2 operator*(const vec2 &a, double b) { return vec2{ a.x * b, a.y * b }; }
inline vec2 operator/(const vec2 &a, double b) { return vec2{ a.x / b, a.y / b }; }
inline double dot(const vec2 &a, const vec2 &b) { return a.x*b.x + a.y*b.y; }
inline double length(const vec2 &a) { return sqrt(dot(a, a)); }
inline vec2 normalize(const vec2 &a) { return a / length(a); }
inline vec2 abs(const vec2 &a) { return vec2{ fabs(a.x), fabs(a.y) }; }

inline vec3 operator+(const vec3 &a, const vec3 &b) { return vec3{ a.x + b.x, a.y + b.y, a.z + b.z }; }
inline vec3 operator-(const vec3 &a, const vec3 &b) { return vec3{ a.x - b.x, a.y - b.y, a.z - b.z }; }
inline vec3 operator*(const vec3 &a, const vec3 &b) { return vec3{ a.x * b.x, a.y * b.y, a.z * b.z }; }
inline vec3 operator/(const vec3 &a, const vec3 &b) { return vec3{ a.x / b.x, a.y / b.y, a.z / b.z }; }
inline vec3 operator*(const vec3 &a, double b) { return vec3{ a.x * b, a.y * b, a.z * b }; }
inline vec3 operator/(const vec3 &a, double b) { return vec3{ a.x / b, a.y / b, a.z / b }; }
inline double dot(const vec3 &a, const vec3 &b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline double length(const vec3 &a) { return sqrt(dot(a, a)); }
inline vec3 normalize(const vec3 &a) { return a / length(a); }
inline vec3 abs(const vec3 &a) { return vec3{ fabs(a.x), fabs(a.y), fabs(a.z) }; }
inline vec3 cross(const vec3 &a, const vec3 &b) { return vec3{ a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x }; }

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
	FORMAT_R11G11B10F,

	FORMAT_COUNT,
	FORMAT_ERROR = 0x7fffffff,
} format_enum;

typedef struct pixel_format {
	format_enum format;
	sp_format sp_format;
	const char *name;
	const char *description;
} pixel_format;

const pixel_format format_list[] = {
	{ FORMAT_R11G11B10F, SP_FORMAT_R11G11B10_FLOAT, "r11g11b10f", "Uncompressed 11/10-bits per float per channel" },
};

typedef enum container_enum {
	CONTAINER_SPTEX,
	CONTAINER_DDS,

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
	{ "sptex", ".sptex", CONTAINER_SPTEX, "SP-tools container" },
	{ "dds", ".dds", CONTAINER_DDS, "DirectDraw Surface" },
};

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

static bool g_verbose;

static void progress_update(void *user, size_t current, size_t total)
{
	if (g_verbose) {
		printf("%10zu / %zu\r", current, total);
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
		std::atomic_int a_index { 0 };
		for (int thread_i = 0; thread_i < num_threads - 1; thread_i++) {
			threads.emplace_back([&]() {
				for (;;) {
					int index = a_index.fetch_add(1, std::memory_order_relaxed);
					if (index >= num) return;
					f(index);
				}
			});
		}

		for (;;) {
			int index = a_index.fetch_add(1, std::memory_order_relaxed);
			if (index >= num) break;
			f(index);

			progress_update(nullptr, (size_t)index + 1, (size_t)num);
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

struct cubemap
{
	struct face
	{
		int32_t width;
		int32_t height;
		int32_t channels;
		float *data;
	};

	face faces[6];

	vec3 sample(const vec3 &v) const;
};

// https://www.gamedev.net/forums/topic/687535-implementing-a-cube-map-lookup-function/

vec3 cubemap::sample(const vec3 &v) const
{
	uint32_t faceIndex;
	vec3 vAbs = abs(v);
	double ma;
	vec2 uv;
	if(vAbs.z >= vAbs.x && vAbs.z >= vAbs.y)
	{
		faceIndex = v.z < 0.0 ? 5 : 4;
		ma = 0.5 / vAbs.z;
		uv = vec2(v.z < 0.0 ? -v.x : v.x, -v.y);
	}
	else if(vAbs.y >= vAbs.x)
	{
		faceIndex = v.y < 0.0 ? 3 : 2;
		ma = 0.5 / vAbs.y;
		uv = vec2(v.x, v.y < 0.0 ? -v.z : v.z);
	}
	else
	{
		faceIndex = v.x < 0.0 ? 1 : 0;
		ma = 0.5 / vAbs.x;
		uv = vec2(v.x < 0.0 ? v.z : -v.z, -v.y);
	}
	uv = uv * ma + 0.5;

	const face &face = faces[faceIndex];
	int32_t x = (int32_t)(uv.x * (double)face.width);
	int32_t y = (int32_t)(uv.y * (double)face.height);
	if (x < 0) x = 0;
	if (x >= face.width - 1) x = face.width - 1;
	if (y < 0) y = 0;
	if (y >= face.height - 1) y = face.height - 1;

	int32_t ix = (y * face.width + x) * face.channels;
	vec3 col;
	if (face.channels >= 3) {
		col.x = face.data[ix + 0];
		col.y = face.data[ix + 1];
		col.z = face.data[ix + 2];
	} else {
		col = vec3(face.data[ix]);
	}
	return col;
}

// https://learnopengl.com/PBR/IBL/Specular-IBL

double RadicalInverse_VdC(uint32_t bits) 
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return double(bits) * 2.3283064365386963e-10; // / 0x100000000
}

vec2 Hammersley(uint32_t i, uint32_t N)
{
    return vec2(double(i)/double(N), RadicalInverse_VdC(i));
}  

vec3 ImportanceSampleGGX(vec2 Xi, vec3 N, double roughness)
{
    double a = roughness*roughness;
	
    double phi = 2.0 * PI * Xi.x;
    double cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a*a - 1.0) * Xi.y));
    double sinTheta = sqrt(1.0 - cosTheta*cosTheta);
	
    // from spherical coordinates to cartesian coordinates
    vec3 H;
    H.x = cos(phi) * sinTheta;
    H.y = sin(phi) * sinTheta;
    H.z = cosTheta;
	
    // from tangent-space vector to world-space sample vector
    vec3 up        = abs(N.z) < 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent   = normalize(cross(up, N));
    vec3 bitangent = cross(N, tangent);
	
    vec3 sampleVec = tangent * H.x + bitangent * H.y + N * H.z;
    return normalize(sampleVec);
}  

vec3 prefilter_cube(const cubemap &cube, const vec3 &dir, double roughness, uint32_t samples)
{
    vec3 N = normalize(dir);    
    vec3 R = N, V = N;

	vec3 total;
	double weight = 0.0;

	for (uint32_t i = 0; i < samples; i++) {
        vec2 Xi = Hammersley(i, samples);
        vec3 H  = ImportanceSampleGGX(Xi, N, roughness);
        vec3 L  = normalize(2.0 * dot(V, H) * H - V);
        double NdotL = dot(N, L);
        if(NdotL > 0.0)
        {
            total = total + cube.sample(L) * NdotL;
            weight += NdotL;
        }
	}

	return total / weight;
}

int32_t clamp(int32_t v, int32_t min, int32_t max) {
	if (v < min) return min;
	if (v > max) return max;
	return v;
}

void encode_format(char *dst, sp_format format, const float *data, int32_t width, int32_t height, int num_threads)
{
	switch (format) {
	case SP_FORMAT_R11G11B10_FLOAT:
		parallel_for(num_threads, height, [&](int32_t y) {
			uint32_t *dst_row = (uint32_t*)dst + y * width;
			const float *row = data + y * width * 3;
			for (int32_t x = 0; x < width; x++) {
				uint32_t r, g, b;
				memcpy(&r, &row[x * 3 + 0], 4);
				memcpy(&g, &row[x * 3 + 1], 4);
				memcpy(&b, &row[x * 3 + 2], 4);

				int32_t re = (int32_t)((r >> 23) & 0xff) - 127;
				int32_t ge = (int32_t)((g >> 23) & 0xff) - 127;
				int32_t be = (int32_t)((b >> 23) & 0xff) - 127;
				uint32_t rm = (r >> (23u - 6u)) & ((1u << 6u) - 1);
				uint32_t gm = (g >> (23u - 6u)) & ((1u << 6u) - 1);
				uint32_t bm = (b >> (23u - 5u)) & ((1u << 5u) - 1);
				if (re < -14) { re = -14; rm = 0; } else if (re > 15) { re = 15; rm = (1u << 6u) - 1; }
				if (ge < -14) { ge = -14; gm = 0; } else if (ge > 15) { ge = 15; gm = (1u << 6u) - 1; }
				if (be < -14) { be = -14; bm = 0; } else if (be > 15) { be = 15; bm = (1u << 5u) - 1; }

				uint32_t rv = (re + 15) << 6 | rm;
				uint32_t gv = (ge + 15) << 6 | gm;
				uint32_t bv = (be + 15) << 5 | bm;
				dst_row[x] = rv | gv << 11 | bv << 22;
			}
		});
		break;
	}
}

int main(int argc, char **argv)
{
	int resolution = 256;
	const char *input_file[6] = { 0 };
	const char *output_file = NULL;
	format_enum format = FORMAT_ERROR;
	bool verbose = false;
	bool show_help = argc <= 1;
	int level = 10;
	int num_threads = 1;
	uint32_t num_samples = 1024;
	container_enum container = CONTAINER_ERROR;

	assert(array_size(format_list) == (size_t)FORMAT_COUNT);

	// -- Parse arguments

	for (int argi = 1; argi < argc; argi++) {
		const char *arg = argv[argi];
		int left = argc - argi - 1;

		if (!strcmp(arg, "-v") || !strcmp(arg, "--verbose")) {
			verbose = true;
			g_verbose = true;
		} else if (!strcmp(arg, "--help")) {
			show_help = true;
		} else if (left >= 1) {
			if (!strcmp(arg, "-i") || !strcmp(arg, "--input")) {
				if (left >= 6) {
					for (uint32_t i = 0; i < 6; i++) {
						input_file[i] = argv[++argi];
					}
				}
			} else if (!strcmp(arg, "-o") || !strcmp(arg, "--output")) {
				output_file = argv[++argi];
			} else if (!strcmp(arg, "-f") || !strcmp(arg, "--format")) {
				format = parse_format(argv[++argi]);
			} else if (!strcmp(arg, "-l") || !strcmp(arg, "--level")) {
				level = atoi(argv[++argi]);
				if (level <= 0 || level > 20) {
					failf("Invalid level %d, must be between 1-20", level);
				}
			} else if (!strcmp(arg, "-r") || !strcmp(arg, "--resolution")) {
				resolution = atoi(argv[++argi]);
			} else if (!strcmp(arg, "-j") || !strcmp(arg, "--threads")) {
				num_threads = atoi(argv[++argi]);
				if (num_threads <= 0 || num_threads > 10000) failf("Bad number of threads: %d");
			} else if (!strcmp(arg, "--samples")) {
				num_samples = (uint32_t)atoi(argv[++argi]);
			}
		}
	}

	if (show_help) {
		printf("%s",
			"Usage: sf-envmap -i <+x> <-x> <+y> <-y> <+z> <-z>  -o <output> -f <format> [options]\n"
			"    -r / --resolutiuon <size>: Cubemap face extent"
			"    -o / --output <path>: Destination filename (use :pattern: to substitute variables (see below)\n"
			"    -f / --format <format>: Compressed texture pixel format (see below)\n"
			"    -c / --container <type>: Output container format (detected from filename if absent, see below)\n"
			"    -j / --threads <num>: Number of threads to use\n"
			"    -v / --verbose: Verbose output\n"
			"    -l / --level <level>: Compression level 1-20 (default 10)\n"
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

	// -- Validate arguments
	if (format == FORMAT_ERROR) failf("Format required: -f <format> (see --help for available formats)");
	if (!input_file[0]) failf("Input file required: -i <x+> <x-> <y+> <y-> <z+> <z->\n");
	if (!output_file) failf("Output file required: -o <output>");

	// -- Load cubemap faces

	cubemap src_cube;
	for (uint32_t i = 0; i < 6; i++) {
		const char *err = "(unknown error)";
		float *data;
		int width, height;
		if (LoadEXR(&data, &width, &height, input_file[i], &err) < 0) {
			failf("Failed to load EXR %s: %s\n", input_file[i], err);
		}

		cubemap::face &face = src_cube.faces[i];
		face.data = data;
		face.channels = 4;
		face.width = (int32_t)width;
		face.height = (int32_t)height;
	}

	cubemap mips[20];
	int num_mips = 0;

	{
		uint32_t r = resolution;
		while (r > 0) {
			cubemap &cube = mips[num_mips++];
			for (uint32_t i = 0; i < 6; i++) {
				cubemap::face &face = cube.faces[i];
				face.width = face.height = r;
				face.channels = 3;
				face.data = (float*)malloc(sizeof(float) * face.width * face.height * face.channels);
			}
			r /= 2;
		}
	}

	vec3 cube_basis[][2] = {
		{ { +1,0,0 }, { 0,+1,0 } },
		{ { -1,0,0 }, { 0,+1,0 } },
		{ { 0,+1,0 }, { 0,0,+1 } },
		{ { 0,-1,0 }, { 0,0,-1 } },
		{ { 0,0,-1 }, { 0,+1,0 } },
		{ { 0,0,+1 }, { 0,+1,0 } },
	};

	for (int mip_i = 0; mip_i < num_mips; mip_i++) {
		cubemap &dst_cube = mips[mip_i];
		double roughness = (double)mip_i / (double)(num_mips - 1);

		for (int face_i = 0; face_i < 6; face_i++) {
			cubemap::face &dst_face = dst_cube.faces[face_i];
			parallel_for(num_threads, dst_face.height, [&](int32_t y) {
				vec3 *b = cube_basis[face_i];
				vec3 right = cross(b[0], b[1]);
				float *dst_row = dst_face.data + y * 3 * dst_face.width;
				for (int32_t x = 0; x < dst_face.width; x++) {
					double dy = (double)(y + 0.5) / (double)dst_face.height * 2.0 - 1.0;
					double dx = (double)(x + 0.5) / (double)dst_face.height * 2.0 - 1.0;
					dy = -dy;
					vec3 dir = right * dx + b[1] * dy + b[0];
					vec3 value = prefilter_cube(src_cube, dir, roughness, num_samples);
					dst_row[x * 3 + 0] = (float)value.x;
					dst_row[x * 3 + 1] = (float)value.y;
					dst_row[x * 3 + 2] = (float)value.z;
				}
			});
		}
	}

	const pixel_format &pxfmt = format_list[format];
	const sp_format_info &format_info = sp_format_infos[pxfmt.sp_format];

	uint32_t top_mip_size = format_info.block_size * ((resolution * resolution) / (format_info.block_x * format_info.block_y));

	char *encoded_data[20][6] = { };
	uint32_t encoded_size[20];

	for (int mip_i = 0; mip_i < num_mips; mip_i++) {
		cubemap &dst_cube = mips[mip_i];
		double roughness = (double)mip_i / (double)(num_mips - 1);
		uint32_t res = resolution >> mip_i;
		uint32_t mip_size = format_info.block_size * ((res * res) / (format_info.block_x * format_info.block_y));
		if (mip_size == 0) mip_size = format_info.block_size;
		encoded_size[mip_i] = mip_size;

		char *data = (char*)malloc(mip_size * 6);

		for (int face_i = 0; face_i < 6; face_i++) {
			cubemap::face &dst_face = dst_cube.faces[face_i];

			encode_format(data, pxfmt.sp_format, dst_face.data, dst_face.width, dst_face.height, num_threads);

			encoded_data[mip_i][face_i] = data;
			data += mip_size;
		}

	}

	// TODO: Windows UTF-16
	FILE *f = fopen(output_file, "wb");
	if (!f) failf("Failed to open output file: %s", output_file);

	switch (container) {

	case CONTAINER_SPTEX: {
		if (num_mips > 16) {
			failf("sptex supports only up to 16 mip levels");
		}

		sp_compression_type compression_type = SP_COMPRESSION_ZSTD;
		size_t bound = 0;
		for (int i = 0; i < num_mips; i++) {
			// Padding
			bound += 16;

			bound += sp_get_compression_bound(compression_type, encoded_size[i] * 6);
		}
		char *compress_buf = (char*)malloc(bound);
		if (!compress_buf) failf("Failed to allocate lossless compression buffer");

		sptex_header header;
		header.header.magic = SPFILE_HEADER_SPTEX;
		header.header.version = 1;
		header.header.header_info_size = sizeof(sptex_info);
		header.header.num_sections = num_mips;
		header.info.format = pxfmt.sp_format;
		header.info.width = (uint16_t)resolution;
		header.info.height = (uint16_t)resolution;
		header.info.uncropped_width = (uint16_t)resolution;
		header.info.uncropped_height = (uint16_t)resolution;
		header.info.crop_min_x = (uint16_t)0;
		header.info.crop_min_y = (uint16_t)0;
		header.info.crop_max_x = (uint16_t)resolution;
		header.info.crop_max_y = (uint16_t)resolution;
		header.info.num_mips = num_mips;
		header.info.num_slices = 6;

		uint32_t header_size = sizeof(spfile_header) + sizeof(sptex_info) + sizeof(spfile_section) * num_mips;
		size_t compress_offset = 0;

		for (int i = 0; i < num_mips; i++) {
			while ((compress_offset + header_size) % 16 != 0) {
				compress_offset++;
				compress_buf[compress_offset] = '\0';
			}

			spfile_section *s_mip = &header.s_mips[i];
			size_t data_size = encoded_size[i] * 6;
			size_t compressed_size = sp_compress_buffer(compression_type,
				compress_buf + compress_offset, bound - compress_offset,
				encoded_data[i][0], data_size, level);

			double no_compress_ratio = 1.05;
			sp_compression_type mip_type = compression_type;
			if ((double)data_size / (double)compressed_size < no_compress_ratio) {
				mip_type = SP_COMPRESSION_NONE;
				memcpy(compress_buf + compress_offset, encoded_data[i][0], data_size);
				compressed_size = data_size;
			}

			if (verbose) {
				if (data_size > 1000) {
					printf("Compressed mip %u from %.1fkB to %.1fkB, ratio %.2f\n",
						i, (double)data_size / 1000.0, (double)compressed_size / 1000.0,
						(double)data_size / (double)compressed_size);
				} else {
					printf("Compressed mip %d from %zub to %zub, ratio %.2f\n",
						i, data_size, compressed_size,
						(double)data_size / (double)compressed_size);
				}
			}

			s_mip->magic = SPFILE_SECTION_MIP;
			s_mip->index = i;
			s_mip->compression_type = mip_type;
			s_mip->uncompressed_size = (uint32_t)data_size;
			s_mip->compressed_size = (uint32_t)compressed_size;
			s_mip->offset = (uint32_t)compress_offset + header_size;

			compress_offset += compressed_size;
		}

		write_data(f, &header, header_size);
		write_data(f, compress_buf, compress_offset);

		if (compress_offset + header_size < sizeof(sptex_header)) {
			char zero_buf[sizeof(sptex_header)] = { };
			write_data(f, zero_buf, sizeof(sptex_header) - (compress_offset + header_size));
		}

		free(compress_buf);

	} break;

	case CONTAINER_DDS: {

		dds_header header = { 0 };
		memcpy(header.magic, "DDS ", 4);
		header.size = 124;
		header.flags = 0xa1007; // CAPS|HEIGHT|WIDTH|PIXELFORMAT|MIPMAPCOUNT|LINEARSIZE
		header.height = (uint32_t)resolution;
		header.width = (uint32_t)resolution;
		header.pitch_or_linear_size = (uint32_t)top_mip_size;
		header.depth = 1;
		header.mip_map_count = (uint32_t)num_mips;
		header.pixelformat_flags = 0x4; // FOURCC
		header.pixelformat_size = 32;
		header.caps[0] = 0x1000; // TEXTURE
		header.caps[1] = 0xfe00; // CUBEMAP_ALLFACES
		if (num_mips > 1) {
			header.caps[0] |= 0x400008; // COMPLEX|MIPMAP
		}

		memcpy(header.pixelformat_fourcc, "DX10", 4);
		switch (format) {
		case FORMAT_R11G11B10F: header.dxgi_format = 26; break; // R11G11B10_FLOAT
		default: header.dxgi_format = 0; break;
		}
		header.resource_dimension = 3; // D3D10_RESOURCE_DIMENSION_TEXTURE2D
		header.array_size = 6;
		header.misc_flags2 = 0x4; // TEXTURECUBE

		for (int face_i = 0; face_i < 6; face_i++) {
			for (int mip_i = 0; mip_i < num_mips; mip_i++) {
				write_data(f, encoded_data[mip_i][face_i], encoded_size[mip_i]);
			}
		}

	} break;

	}

	if (fclose(f) != 0) {
		failf("Failed to flush output file: %s", output_file);
	}

	return 0;
}
