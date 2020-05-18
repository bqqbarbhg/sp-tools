#if 0
#define _CRT_SECURE_NO_WARNINGS

#include "acl/compression/compress.h"
#include "meshoptimizer/meshoptimizer.h"
#include "ufbx.h"

#include "../sp_tools_common.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>
#include <assert.h>
#include <string.h>
#include <vector>
#include <string>
#include <regex>
#include <limits>
#include "model.h"

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

typedef enum attrib_enum {
	ATTRIB_POSITION,
	ATTRIB_NORMAL,
	ATTRIB_UV,
	ATTRIB_COLOR,
	ATTRIB_INDEX,
	ATTRIB_WEIGHT,
	ATTRIB_PAD,

	ATTRIB_COUNT,
	ATTRIB_ERROR = 0x7fffffff,
} attrib_enum;

typedef struct attrib_type {
	const char *code;
	const char *name;
	attrib_enum attrib;
} attrib_type;

attrib_type attrib_list[] = {
	{ "p", "position", ATTRIB_POSITION },
	{ "n", "normal", ATTRIB_NORMAL },
	{ "u", "uv", ATTRIB_UV },
	{ "c", "color", ATTRIB_COLOR },
	{ "i", "bone-index", ATTRIB_INDEX },
	{ "w", "bone-weight", ATTRIB_WEIGHT },
	{ "_", "(padding)", ATTRIB_PAD },
};

typedef enum type_enum {
	TYPE_FLOAT,
	TYPE_HALF_FLOAT,
	TYPE_UINT_BYTE,
	TYPE_SINT_BYTE,
	TYPE_UNORM_BYTE,
	TYPE_SNORM_BYTE,
	TYPE_UINT_SHORT,
	TYPE_SINT_SHORT,
	TYPE_UNORM_SHORT,
	TYPE_SNORM_SHORT,
	TYPE_UINT_INT,
	TYPE_SINT_INT,
	TYPE_UNORM_INT,
	TYPE_SNORM_INT,
	TYPE_UNORM_RGB10,

	TYPE_COUNT,
	TYPE_ERROR = 0x7fffffff,
} type_enum;

typedef enum type_flag {
	TYPE_FLAG_FLOAT = 0x1,
	TYPE_FLAG_INT = 0x2,
	TYPE_FLAG_NORMALIZED = 0x4,
	TYPE_FLAG_SIGNED = 0x8,
} type_flag;

typedef struct type {
	const char *magic;
	const char* code;
	const char* name;
	type_enum type;
	int size;
	int components;
	uint32_t flags;
} type;

type type_list[] = {
	{ "fp32", "f", "float32", TYPE_FLOAT, 4, 1, TYPE_FLAG_FLOAT },
	{ "fp16", "h", "float16", TYPE_HALF_FLOAT, 2, 1, TYPE_FLAG_FLOAT },
	{ "ui08", "ub", "uint8", TYPE_UINT_BYTE, 1, 1, TYPE_FLAG_INT },
	{ "si08", "ib", "sint8", TYPE_SINT_BYTE, 1, 1, TYPE_FLAG_INT | TYPE_FLAG_SIGNED },
	{ "un08", "nb", "unorm8", TYPE_UNORM_BYTE, 1, 1, TYPE_FLAG_NORMALIZED },
	{ "sn08", "sb", "snorm8", TYPE_SNORM_BYTE, 1, 1, TYPE_FLAG_NORMALIZED | TYPE_FLAG_SIGNED },
	{ "ui16", "us", "uint16", TYPE_UINT_SHORT, 2, 1, TYPE_FLAG_INT },
	{ "si16", "is", "sint16", TYPE_SINT_SHORT, 2, 1, TYPE_FLAG_INT | TYPE_FLAG_SIGNED },
	{ "un16", "ns", "unorm16", TYPE_UNORM_SHORT, 2, 1, TYPE_FLAG_NORMALIZED },
	{ "sn16", "ss", "snorm16", TYPE_SNORM_SHORT, 2, 1, TYPE_FLAG_NORMALIZED | TYPE_FLAG_SIGNED },
	{ "ui32", "ui", "uint32", TYPE_UINT_INT, 4, 1, TYPE_FLAG_INT },
	{ "si32", "ii", "sint32", TYPE_SINT_INT, 4, 1, TYPE_FLAG_INT | TYPE_FLAG_SIGNED },
	{ "un32", "ni", "unorm32", TYPE_UNORM_INT, 4, 1, TYPE_FLAG_NORMALIZED },
	{ "sn32", "si", "snorm32", TYPE_SNORM_INT, 4, 1, TYPE_FLAG_NORMALIZED | TYPE_FLAG_SIGNED },
	{ "un10", "rgb10", "unorm_rgb10", TYPE_UNORM_RGB10, 4, 4, TYPE_FLAG_NORMALIZED },
};

attrib_enum parse_attrib(const char *name)
{
	for (size_t i = 0; i < array_size(attrib_list); i++) {
		if (!strcmp(name, attrib_list[i].code)) {
			return attrib_list[i].attrib;
		}
	}
	failf("Unsupported format: %s", name);
	return ATTRIB_ERROR;
}

type_enum parse_type(const char *name)
{
	for (size_t i = 0; i < array_size(type_list); i++) {
		if (!strcmp(name, type_list[i].code)) {
			return type_list[i].type;
		}
	}
	failf("Unsupported type: %s", name);
	return TYPE_ERROR;
}

typedef struct vertex_element {
	attrib_enum attrib;
	type_enum type;
	int stream;
	int offset;
	int components;
	int size;
} vertex_element;

int parse_elements(std::vector<vertex_element> &elements, std::vector<size_t> &strides, const char *desc)
{
	std::string desc_str = desc;
	std::regex re("([a-z]+)([0-9]+)([a-z0-9]+)([,;]?)");
	std::sregex_iterator begin(desc_str.begin(), desc_str.end(), re), end;
	int offset = 0;
	int stream = 0;
	for (; begin != end; ++begin) {
		std::smatch match = *begin;
		std::string attrib = match[1];
		int components = std::stoi(match[2]);
		std::string type = match[3];
		std::string terminator = match[4];

		vertex_element elem;
		elem.attrib = parse_attrib(attrib.c_str());
		elem.components = components;
		elem.type = parse_type(type.c_str());
		elem.size = type_list[elem.type].size * elem.components;
		elem.offset = offset;
		elem.stream = stream;

		elements.push_back(elem);

		offset += elem.size;
		if (terminator == ";") {
			strides.push_back((size_t)offset);
			stream++;
			offset = 0;
		}
	}
	if (offset) strides.push_back((size_t)offset);
	return offset;
}

template <typename T>
static void push_stream(meshopt_Stream *&dst, const std::vector<T> &data, int num_per_vertex=1)
{
	if (data.size()) {
		dst->data = data.data();
		dst->size = dst->stride = sizeof(T) * num_per_vertex;
		++dst;
	}
}

template <typename T>
T clamp_d(double d)
{
	double min = (double)std::numeric_limits<T>().lowest();
	double max = (double)std::numeric_limits<T>().max();
	if (d < min) return (T)min;
	if (d > max) return (T)max;
	return (T)d;
}

template <typename T>
void normalize_quantized_imp(char *dst, size_t num)
{
	int64_t total = 0;
	T *t = (T*)dst;
	for (size_t i = 0; i < num; i++) {
		total += (int64_t)t[i];
	}
	if (total == 0) return;

	int64_t delta = (int64_t)std::numeric_limits<T>().max() - total;
	if (delta == 0) return;

	for (size_t i = 0; i < num; i++) {
		if (delta > 0) {
			if (t[i] > 0) {
				t[i] += (T)delta;
				break;
			}
		} else {
			if ((int64_t)t[i] > -delta) {
				t[i] -= (T)-delta;
				break;
			} else {
				delta += (int64_t)t[i];
				t[i] = 0;
			}
		}
	}
}

void encode_vertex_imp(vertex_element &elem, char *dst, const double *data, const double *min, const double *max, int float_bits, bool normalize_quantized=false)
{
	char *ptr = dst;
	type t = type_list[elem.type];
	for (int i = 0; i < elem.components; i++) {
		switch (elem.type) {
		case TYPE_FLOAT: *(float*)ptr = meshopt_quantizeFloat((float)data[i], float_bits); break;
		case TYPE_HALF_FLOAT: *(short*)ptr = meshopt_quantizeHalf((float)data[i]); break;
		case TYPE_UINT_BYTE: *(uint8_t*)ptr = clamp_d<uint8_t>(data[i]); break;
		case TYPE_SINT_BYTE: *(int8_t*)ptr = clamp_d<int8_t>(data[i]); break;
		case TYPE_UNORM_BYTE: *(uint8_t*)ptr = (uint8_t)meshopt_quantizeUnorm((float)((data[i] - min[i]) / (max[i] - min[i])), 8); break;
		case TYPE_SNORM_BYTE: *(int8_t*)ptr = (int8_t)meshopt_quantizeSnorm((float)((data[i] - min[i]) / (max[i] - min[i])), 8); break;
		case TYPE_UINT_SHORT: *(uint16_t*)ptr = clamp_d<uint16_t>(data[i]); break;
		case TYPE_SINT_SHORT: *(int16_t*)ptr = clamp_d<int16_t>(data[i]); break;
		case TYPE_UNORM_SHORT: *(uint16_t*)ptr = (uint16_t)meshopt_quantizeUnorm((float)((data[i] - min[i]) / (max[i] - min[i])), 16); break;
		case TYPE_SNORM_SHORT: *(int16_t*)ptr = (int16_t)meshopt_quantizeSnorm((float)((data[i] - min[i]) / (max[i] - min[i])), 16); break;
		case TYPE_UINT_INT: *(uint32_t*)ptr = clamp_d<uint32_t>(data[i]); break;
		case TYPE_SINT_INT: *(int32_t*)ptr = clamp_d<int32_t>(data[i]); break;
		case TYPE_UNORM_INT: *(uint32_t*)ptr = (uint32_t)meshopt_quantizeUnorm((float)((data[i] - min[i]) / (max[i] - min[i])), 32); break;
		case TYPE_SNORM_INT: *(int32_t*)ptr = (int32_t)meshopt_quantizeSnorm((float)((data[i] - min[i]) / (max[i] - min[i])), 32); break;
		case TYPE_UNORM_RGB10: {
			int base = i * 3;
			double r = (data[base + 0] - min[base + 0]) / (max[base + 0] - min[base + 0]);
			double g = (data[base + 1] - min[base + 1]) / (max[base + 1] - min[base + 1]);
			double b = (data[base + 2] - min[base + 2]) / (max[base + 2] - min[base + 2]);
			uint32_t v = 0;
			v |= (uint32_t)meshopt_quantizeUnorm((float)b, 10) << 0;
			v |= (uint32_t)meshopt_quantizeUnorm((float)g, 10) << 10;
			v |= (uint32_t)meshopt_quantizeUnorm((float)r, 10) << 20;
			*(uint32_t*)ptr = v;
		} break;
		}
		ptr += t.size;
	}

	// Normalize quantized result if necessary
	if (normalize_quantized) {
		switch (elem.type) {
		case TYPE_UNORM_BYTE: normalize_quantized_imp<uint8_t>(dst, elem.components); break;
		case TYPE_UNORM_SHORT: normalize_quantized_imp<uint16_t>(dst, elem.components); break;
		case TYPE_UNORM_INT: normalize_quantized_imp<uint32_t>(dst, elem.components); break;
		}
		ptr += t.size;
	}
}

struct TempVertex
{
	float pos[3];
	uint8_t index[4];
	uint8_t weight[4];
};

void encode_vertices(vertex_element &elem, char *dst, mp::mesh_part &part, int float_bits, size_t num_vertices, unsigned *remap, size_t stride)
{
	char *ptr = dst + elem.offset;

	double data[4] = { 0.0, 0.0, 0.0, 0.0 }, min[4] = { 0.0, 0.0, 0.0, 0.0 }, max[4] = { 1.0, 1.0, 1.0, 1.0 };
	switch (elem.attrib) {

	case ATTRIB_POSITION: if (part.position.size()) {
		min[0] = part.min_position.x; min[1] = part.min_position.y; min[2] = part.min_position.z;
		max[0] = part.max_position.x; max[1] = part.max_position.y; max[2] = part.max_position.z;
		for (size_t i = 0; i < num_vertices; i++) {
			mp::vec3 &v = part.position[remap[i]];
			data[0] = v.x; data[1] = v.y; data[2] = v.z;
			encode_vertex_imp(elem, ptr, data, min, max, float_bits);
			ptr += stride;
		}
	} break;

	case ATTRIB_NORMAL: if (part.normal.size()) {
		min[0] = min[1] = min[2] = -1.0;
		for (size_t i = 0; i < num_vertices; i++) {
			mp::vec3 &v = part.normal[remap[i]];
			data[0] = v.x; data[1] = v.y; data[2] = v.z;
			encode_vertex_imp(elem, ptr, data, min, max, float_bits);
			ptr += stride;
		}
	} break;

	case ATTRIB_UV: if (part.uv.size()) {
		min[0] = part.min_uv.x; min[1] = part.min_uv.y;
		max[0] = part.max_uv.x; max[1] = part.max_uv.y;
		for (size_t i = 0; i < num_vertices; i++) {
			mp::vec2 &v = part.uv[remap[i]];
			data[0] = v.x; data[1] = v.y;
			encode_vertex_imp(elem, ptr, data, min, max, float_bits);
			ptr += stride;
		}
	} break;

	case ATTRIB_COLOR: if (part.color.size()) {
		for (size_t i = 0; i < num_vertices; i++) {
			mp::vec4 &v = part.color[remap[i]];
			data[0] = v.x; data[1] = v.y; data[2] = v.z; data[3] = v.w;
			encode_vertex_imp(elem, ptr, data, min, max, float_bits);
			ptr += stride;
		}
	} break;

	case ATTRIB_INDEX: if (part.weights.size()) {
		std::vector<double> vdata;
		std::vector<double> vmin;
		std::vector<double> vmax;

		vdata.resize((size_t)part.weights_per_vertex);
		vmin.resize((size_t)part.weights_per_vertex, 0.0);
		vmax.resize((size_t)part.weights_per_vertex, 255.0);

		for (size_t i = 0; i < num_vertices; i++) {
			mp::bone_weight *weight = part.weights.data() + remap[i] * part.weights_per_vertex;
			for (size_t j = 0; j < part.weights_per_vertex; j++) {
				vdata[j] = (double)weight[j].index;
			}
			encode_vertex_imp(elem, ptr, vdata.data(), vmin.data(), vmax.data(), float_bits);
			ptr += stride;
		}
	} break;

	case ATTRIB_WEIGHT: if (part.weights.size()) {
		std::vector<double> vdata;
		std::vector<double> vmin;
		std::vector<double> vmax;

		vdata.resize((size_t)part.weights_per_vertex);
		vmin.resize((size_t)part.weights_per_vertex, 0.0);
		vmax.resize((size_t)part.weights_per_vertex, 1.0);

		for (size_t i = 0; i < num_vertices; i++) {
			mp::bone_weight *weight = part.weights.data() + remap[i] * part.weights_per_vertex;
			for (size_t j = 0; j < part.weights_per_vertex; j++) {
				vdata[j] = (double)weight[j].weight;
			}
			encode_vertex_imp(elem, ptr, vdata.data(), vmin.data(), vmax.data(), float_bits, true);
			ptr += stride;
		}
	} break;

	}
}

// spmdl_header
// spmdl_node[num_nodes]
// spmdl_mesh[num_meshes]
// spmdl_bone[num_bones]
// spmdl_material[num_materials]
// char[string_data_size]
// char[geometry_data_size]

spmdl_vec3 ufbx_to_spmdl(ufbx_vec3 v) { return { (float)v.x, (float)v.y, (float)v.z }; }
spmdl_vec4 ufbx_to_spmdl(ufbx_vec4 v) { return { (float)v.x, (float)v.y, (float)v.z, (float)v.w }; }
spmdl_matrix ufbx_to_spmdl(const ufbx_matrix &v) {
	return {
		ufbx_to_spmdl(v.cols[0]), ufbx_to_spmdl(v.cols[1]), ufbx_to_spmdl(v.cols[2]), ufbx_to_spmdl(v.cols[3]),
	};
}

struct spmdl_builder
{
	sp_compression_type compression = SP_COMPRESSION_ZSTD;
	int level = 10;
	bool meshopt_vertices = false;
	bool meshopt_indices = false;

	std::vector<spmdl_node> nodes;
	std::vector<spmdl_mesh> meshes;
	std::vector<spmdl_bone> bones;
	std::vector<spmdl_material> materials;
	std::unordered_map<ufbx_node*, uint32_t> node_mapping;
	std::unordered_map<std::string, uint32_t> string_data;
	std::vector<char> string_buf;
	std::vector<char> geometry_buf;

	spmdl_string add_string(const std::string &str)
	{
		uint32_t offset = (uint32_t)string_buf.size();
		auto res = string_data.insert(std::make_pair(str, offset));
		if (res.second) {
			string_buf.insert(string_buf.end(), str.begin(), str.end());
			string_buf.push_back('\0');
			return { offset, (uint32_t)str.size() };
		} else {
			return { res.first->second, (uint32_t)str.size() };
		}
	}

	spmdl_string add_string(const ufbx_string &str)
	{
		return add_string(std::string(str.data, str.data + str.length));
	}

	uint32_t add_node(ufbx_node *node)
	{
		uint32_t index = (uint32_t)nodes.size();
		auto res = node_mapping.insert(std::make_pair(node, index));
		if (!res.second) return res.first->second;

		uint32_t parent_ix = ~0u;
		if (node->parent) parent_ix = add_node(node->parent);

		spmdl_node dst;
		dst.parent = parent_ix;
		dst.name = add_string(node->name);
		dst.translation = ufbx_to_spmdl(node->transform.translation);
		dst.rotation = ufbx_to_spmdl(node->transform.rotation);
		dst.scale = ufbx_to_spmdl(node->transform.scale);
		dst.self_to_parent = ufbx_to_spmdl(node->to_parent);
		dst.self_to_root = ufbx_to_spmdl(node->to_root);
		nodes.push_back(dst);
		return index;
	}

	void add_material(ufbx_material *material)
	{
		spmdl_material mat;
		if (material) {
			mat.name = add_string(material->name);
		} else {
			mat.name = add_string("");
		}
		materials.push_back(mat);
	}

	spmdl_buffer add_geometry(const void *data, size_t size, size_t stride, bool vertex)
	{
		uint32_t flags = 0;
		size_t uncompressed_size = size;

		const void *encoded_data = data;
		size_t encoded_size = uncompressed_size;

		std::vector<char> meshopt_data;
		if (vertex && meshopt_vertices) {
			flags |= 0x1;
			meshopt_data.resize(meshopt_encodeVertexBufferBound(size / stride, stride));
			encoded_size = meshopt_encodeVertexBuffer((unsigned char*)meshopt_data.data(), meshopt_data.size(), data, size / stride, stride);
			encoded_data = meshopt_data.data();
		}

		if (!vertex && meshopt_indices) {
			assert(stride == 4);
			meshopt_data.resize(meshopt_encodeIndexBufferBound(size / stride, stride));
			encoded_size = meshopt_encodeIndexBuffer((unsigned char*)meshopt_data.data(), meshopt_data.size(), (const unsigned int*)data, size / stride);
			encoded_data = meshopt_data.data();
		}

		size_t compress_bound = sp_get_compression_bound(compression, encoded_size);

		size_t offset = geometry_buf.size();
		geometry_buf.insert(geometry_buf.end(), compress_bound, 0);

		size_t compressed_size = sp_compress_buffer(compression, geometry_buf.data() + offset, compress_bound, encoded_data, encoded_size, level);
		geometry_buf.erase(geometry_buf.end() - (compress_bound - compressed_size), geometry_buf.end());

		spmdl_buffer buffer;
		buffer.data_offset = (uint32_t)offset;
		buffer.compression = compression;
		buffer.flags = flags;
		buffer.uncompressed_size = (uint32_t)uncompressed_size;
		buffer.encoded_size = (uint32_t)encoded_size;
		buffer.compressed_size = (uint32_t)compressed_size;
		buffer.stride = (uint32_t)stride;
		return buffer;
	}

	void write_data(FILE *f, const void *data, size_t size)
	{
		fwrite(data, 1, size, f);
	}

	template <typename T>
	void write_pod(FILE *f, T &t)
	{
		write_data(f, &t, sizeof(T));
	}

	template <typename T>
	void write_pod_vec(FILE *f, std::vector<T> &t)
	{
		write_data(f, t.data(), sizeof(T) * t.size());
	}

	void write_to(FILE *f)
	{
		spmdl_header header;
		memcpy(header.magic, "spmd", 4);
		header.version = 1;
		header.num_nodes = (uint32_t)nodes.size();
		header.num_meshes = (uint32_t)meshes.size();
		header.num_bones = (uint32_t)bones.size();
		header.num_materials = (uint32_t)materials.size();
		header.string_data_size = (uint32_t)string_buf.size();
		header.geometry_data_size = (uint32_t)geometry_buf.size();

		write_pod(f, header);
		write_pod_vec(f, nodes);
		write_pod_vec(f, meshes);
		write_pod_vec(f, bones);
		write_pod_vec(f, materials);
		write_pod_vec(f, string_buf);
		write_pod_vec(f, geometry_buf);
	}
};


int main(int argc, char **argv)
{
	const char *input_file = NULL;
	const char *output_file = NULL;
	const char *mesh_format = NULL;
	bool verbose = false;
	bool show_help = false;
	int level = 10;
	int num_threads = 1;

	mp::opts mp_opts;

	std::vector<vertex_element> elements;
	std::vector<size_t> strides;
	int vertex_size = 0;

	// -- Parse arguments

	for (int argi = 1; argi < argc; argi++) {
		const char *arg = argv[argi];
		int left = argc - argi - 1;

		if (!strcmp(arg, "-v") || !strcmp(arg, "--verbose")) {
			verbose = true;
		} else if (!strcmp(arg, "--help")) {
			show_help = true;
		} else if (!strcmp(arg, "--combine-meshes")) {
			mp_opts.combine_meshes = true;
		} else if (left >= 1) {
			if (!strcmp(arg, "-i") || !strcmp(arg, "--input")) {
				input_file = argv[++argi];
			} else if (!strcmp(arg, "-o") || !strcmp(arg, "--output")) {
				output_file = argv[++argi];
			} else if (!strcmp(arg, "-l") || !strcmp(arg, "--level")) {
				level = atoi(argv[++argi]);
				if (level <= 0 || level > 20) {
					failf("Invalid level %d, must be between 1-20", level);
				}
			} else if (!strcmp(arg, "-j") || !strcmp(arg, "--threads")) {
				num_threads = atoi(argv[++argi]);
				if (num_threads <= 0 || num_threads > 10000) failf("Bad number of threads: %d");
			} else if (!strcmp(arg, "--mesh")) {
				mesh_format = argv[++argi];
			}
		}
	}

	if (show_help) {
		printf("%s",
			"Usage: sf-model -i <input> -o <output> --mesh p3f:n3f:u2f [options]\n"
			"    -i / --input <path>: Input filename in any format stb_image supports\n"
			"    -o / --output <path>: Destination filename\n"
			"    -j / --threads <num>: Number of threads to use\n"
			"    -v / --verbose: Verbose output\n"
		);

		return 0;
	}

	// -- Validate arguments

	if (!input_file) failf("Input file required: -i <input>");
	if (!output_file) failf("Output file required: -o <output>");

	if (mesh_format) {
		vertex_size = parse_elements(elements, strides, mesh_format);
		if (verbose) {
			printf("Vertex format '%s'\n", mesh_format);
			int stream_ix = 0;
			for (size_t stride : strides) {
				printf("Stream %d: %zu bytes\n", stream_ix, stride);
				stream_ix++;
			}
			for (vertex_element &elem : elements) {
				attrib_type a = attrib_list[elem.attrib];
				type t = type_list[elem.type];
				printf("  %d + %2d: %s %dx %s\n", elem.stream, elem.offset, a.name, elem.components, t.name);
			}
		}
	}

	mp_opts.keep_normals = false;
	mp_opts.keep_uvs = false;
	mp_opts.keep_colors = false;
	mp_opts.weights_per_vertex = 0;

	for (vertex_element &elem : elements) {
		switch (elem.attrib) {
		case ATTRIB_NORMAL: mp_opts.keep_normals = true; break;
		case ATTRIB_UV: mp_opts.keep_uvs = true; break;
		case ATTRIB_COLOR: mp_opts.keep_colors = true; break;
		case ATTRIB_INDEX: mp_opts.weights_per_vertex = elem.components * type_list[elem.type].components; break;
		}
	}

	// -- Load input FBX

#if 0
	char *file_data = (char*)malloc(4096*1024);
	FILE *f = fopen(input_file, "rb");
	size_t file_size = fread(file_data, 1, 4096*1024, f);
	fclose(f);
#endif

	ufbx_load_opts ufbx_opts = { };
	ufbx_error error;
	ufbx_scene *scene = ufbx_load_file(input_file, &ufbx_opts, &error);
	// ufbx_scene *scene = ufbx_load_memory(file_data, file_size, &ufbx_opts, &error);
	if (!scene) {
		fprintf(stderr, "Failed to load FBX file: %s\n", input_file);
		for (size_t i = 0; i < error.stack_size; i++) {
			ufbx_error_frame f = error.stack[i];
			fprintf(stderr, "> %s (%s line %u)\n", f.description, f.function, f.source_line);
		}
		exit(1);
	}

	mp::model model = mp::process(scene, mp_opts);

	float overdraw_threshold = 1.05f;
	int float_bits = 23;

	spmdl_builder mdl;

	for (mp::bone &bone : model.bones) {
		mdl.add_node(bone.src);
	}

	for (mp::material &mat : model.materials) {
		mdl.add_material(mat.src);
	}

	for (mp::mesh &mesh : model.meshes) {
		for (mp::mesh_part &part : mesh.parts) {
			size_t num_indices = part.position.size();

			std::vector<unsigned> indices;
			indices.resize(num_indices);

			meshopt_Stream streams[32], *dst = streams;

			push_stream(dst, part.position);
			push_stream(dst, part.normal);
			push_stream(dst, part.uv);
			push_stream(dst, part.color);
			push_stream(dst, part.weights, mp_opts.weights_per_vertex);
			assert(dst - streams < array_size(streams));

			size_t num_vertices = meshopt_generateVertexRemapMulti(indices.data(), NULL, num_indices, num_indices, streams, dst - streams);

			std::vector<mp::vec3> positions;
			positions.resize(num_vertices);

			std::vector<unsigned> vertex_remap;
			vertex_remap.reserve(num_indices);
			for (unsigned i = 0; i < num_indices; i++) {
				vertex_remap.push_back(i);
			}

			meshopt_remapVertexBuffer(positions.data(), part.position.data(), num_indices, sizeof(mp::vec3), indices.data());
			meshopt_remapVertexBuffer(vertex_remap.data(), vertex_remap.data(), num_indices, sizeof(unsigned), indices.data());

			meshopt_spatialSortTriangles(indices.data(), indices.data(), num_indices, (float*)positions.data(), num_vertices, sizeof(mp::vec3));

			meshopt_optimizeVertexCache(indices.data(), indices.data(), num_indices, num_vertices);
			meshopt_optimizeOverdraw(indices.data(), indices.data(), num_indices, (float*)positions.data(), num_vertices, sizeof(mp::vec3), overdraw_threshold);

			meshopt_optimizeVertexFetch(vertex_remap.data(), indices.data(), num_indices, vertex_remap.data(), num_vertices, sizeof(unsigned));

			std::vector<char> vertex_data[SPMDL_MAX_VERTEX_BUFFERS];

			for (size_t i = 0; i < strides.size(); i++) {
				vertex_data[i].resize(strides[i] * num_vertices);
			}

			for (vertex_element &elem : elements) {
				std::vector<char> &data = vertex_data[elem.stream];
				size_t stride = strides[elem.stream];
				encode_vertices(elem, data.data(), part, float_bits, num_vertices, vertex_remap.data(), stride);
			}

			spmdl_mesh mdl_mesh = { };

			mdl_mesh.num_indices = (uint32_t)num_indices;
			mdl_mesh.num_vertices = (uint32_t)num_vertices;
			mdl_mesh.num_vertex_buffers = (uint32_t)strides.size();
			mdl_mesh.num_elements = (uint32_t)elements.size();

			// TODO: Split meshes
			mdl_mesh.index_buffer = mdl.add_geometry(indices.data(), indices.size(), 4, false);

			mdl_mesh.node = mdl.add_node(&mesh.src->node);
			mdl_mesh.material = (uint32_t)part.material_index;
			for (size_t i = 0; i < strides.size(); i++) {
				mdl_mesh.vertex_buffers[i] = mdl.add_geometry(vertex_data[i].data(), vertex_data[i].size(), strides[i], true);
			}
			mdl.meshes.push_back(mdl_mesh);

			for (size_t i = 0; i < elements.size(); i++) {
				type t = type_list[elements[i].type];
				spmdl_element &mdl_elem = mdl_mesh.elements[i];
				memcpy(mdl_elem.type, t.magic, 4);
				mdl_elem.offset	= elements[i].offset;
				mdl_elem.num_components = elements[i].components;
			}
		}
	}

	return 0;
}
#endif
