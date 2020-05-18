#pragma once

#include <unordered_map>
#include <string>
#include "ufbx.h"

namespace mp {

struct vec2 { float x = 0.0f, y = 0.0f; };
struct vec3 { float x = 0.0f, y = 0.0f, z = 0.0f; };
struct vec4 { float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f; };
struct bone_weight { int index = 0; float weight = 0.0f; };

struct opts
{
	bool combine_meshes = false;
	bool transform_to_root = false;
	bool retain_all_bones = false;
	std::vector<std::string> retain_bones;

	int weights_per_vertex = 0;
	bool keep_normals = false;
	bool keep_uvs = false;
	bool keep_colors = false;
};

struct material
{
	ufbx_material *src = nullptr;
};

struct mesh_part
{
	int material_index = -1;
	std::vector<vec3> position;
	std::vector<vec3> normal;
	std::vector<vec2> uv;
	std::vector<vec4> color;
	std::vector<bone_weight> weights;
	int weights_per_vertex = 4;

	struct skin
	{
		int node_index = -1;
		ufbx_skin *src = nullptr;
	};

	bool has_position_bounds = false;
	vec3 min_position, max_position;
	bool has_uv_bounds = false;
	vec2 min_uv, max_uv;
};

struct mesh
{
	ufbx_mesh *src = nullptr;
	std::vector<mesh_part> parts;
};

struct model
{
	std::vector<material> materials;
	std::vector<mesh> meshes;
	std::unordered_map<ufbx_node*, int> bone_mapping;
};

model process(ufbx_scene *scene, opts opts);

}
