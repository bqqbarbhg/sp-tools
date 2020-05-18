#if 0
#include "model.h"
#include <assert.h>

namespace mp {

vec2 from_ufbx(const ufbx_vec2 &v) { return { (float)v.x, (float)v.y }; }
vec3 from_ufbx(const ufbx_vec3 &v) { return { (float)v.x, (float)v.y, (float)v.z }; }
vec4 from_ufbx(const ufbx_vec4 &v) { return { (float)v.x, (float)v.y, (float)v.z, (float)v.w }; }

static vec2 min(vec2 a, vec2 b) {
	return { a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y };
}
static vec2 max(vec2 a, vec2 b) {
	return { a.x < b.x ? b.x : a.x, a.y < b.y ? b.y : a.y };
}

static vec3 min(vec3 a, vec3 b) {
	return { a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z };
}
static vec3 max(vec3 a, vec3 b) {
	return { a.x < b.x ? b.x : a.x, a.y < b.y ? b.y : a.y, a.z < b.z ? b.z : a.z };
}

static void retain_bone(model &model, ufbx_node *node)
{
	if (!node) return;
	if (model.bone_mapping.find(node) != model.bone_mapping.end()) return;
	retain_bone(model, node->parent);

	bone b;
	b.src = node;

	model.bone_mapping[node] = (int)model.bones.size();
	model.bones.push_back(b);
}

struct vertex_ctx {
	model &model;
	ufbx_mesh &src;
	opts opts;
	ufbx_vec3 *positions;
	ufbx_vec3 *normals;
	bone_weight *weights = nullptr;
};

static void add_vertex(vertex_ctx &ctx, mesh_part &part, uint32_t index)
{
	part.position.push_back(from_ufbx(ctx.positions[ctx.src.vertex_position.indices[index]]));
	if (ctx.src.vertex_normal.data && ctx.opts.keep_normals) {
		part.normal.push_back(from_ufbx(ctx.normals[ctx.src.vertex_normal.indices[index]]));
	}
	if (ctx.src.vertex_uv.data && ctx.opts.keep_uvs) {
		part.uv.push_back(from_ufbx(ufbx_get_vertex_vec2(&ctx.src.vertex_uv, index)));
	}
	if (ctx.src.vertex_color.data && ctx.opts.keep_colors) {
		part.color.push_back(from_ufbx(ufbx_get_vertex_vec4(&ctx.src.vertex_color, index)));
	}
	if (ctx.weights) {
		bone_weight *weights = ctx.weights + ctx.src.vertex_position.indices[index] * ctx.opts.weights_per_vertex;
		for (int i = 0; i < ctx.opts.weights_per_vertex; i++) {
			part.weights.push_back(weights[i]);
		}
	}
}

model process(ufbx_scene *scene, opts opts)
{
	model model;

	mesh shared_mesh;
	bool has_null_material = false;

	// Gather materials
	for (ufbx_material &src : scene->materials) {
		material dst;
		dst.src = &src;
		model.materials.push_back(dst);
	}

	// Gather retained bones
	for (ufbx_mesh &src : scene->meshes) {
		for (ufbx_skin &skin : src.skins) {
			retain_bone(model, skin.bone);
		}
	}

	for (ufbx_bone &bone : scene->bones) {
		bool retain = opts.retain_all_bones;
		if (!retain) {
			for (std::string &name : opts.retain_bones) {
				if (name == bone.node.name.data) {
					retain = true;
					break;
				}
			}
		}
		if (retain) {
			retain_bone(model, &bone.node);
		}
	}

	// Process meshes
	for (ufbx_mesh &src : scene->meshes) {
		mesh unique_mesh;
		mesh &dst = opts.combine_meshes ? shared_mesh : unique_mesh;
		dst.src = &src;

		std::vector<ufbx_vec3> root_positions;
		std::vector<ufbx_vec3> root_normals;

		std::vector<bone_weight> weights;
		unique_mesh.src = &src;

		vertex_ctx ctx = { model, src, opts, src.vertex_position.data, src.vertex_normal.data };

		// Generate weights
		if (opts.weights_per_vertex > 0 && src.skins.size > 0) {
			weights.resize(opts.weights_per_vertex * src.num_vertices);
			ctx.weights = weights.data();

			for (ufbx_skin &skin : src.skins) {

				auto it = model.bone_mapping.find(skin.bone);
				assert(it != model.bone_mapping.end());

				int boneIndex = it->second;

				size_t num_weights = skin.num_weights;
				for (size_t i = 0; i < num_weights; i++) {
					bone_weight *dst = weights.data() + skin.indices[i] * opts.weights_per_vertex;
					int index = boneIndex;
					float weight = (float)skin.weights[i];
					for (int c = 0; c < opts.weights_per_vertex; c++) {
						if (weight > dst[c].weight) {
							std::swap(weight, dst[c].weight);
							std::swap(index, dst[c].index);
						}
					}
				}
			}

			// Normalize weights
			for (size_t i = 0; i < src.num_vertices; i++) {
				bone_weight *w = weights.data() + i * opts.weights_per_vertex;
				float total = 0.0f;
				for (int j = 0; j < opts.weights_per_vertex; j++) {
					total += w[j].weight;
				}
				if (total > 0.0f) {
					for (int j = 0; j < opts.weights_per_vertex; j++) {
						w[j].weight /= total;
					}
				}
			}
		}

		// Transform to world space
		if (opts.transform_to_root) {
			ufbx_matrix normal_mat = ufbx_get_normal_matrix(&src.node.to_root);

			root_positions.reserve(src.vertex_position.num_elements);
			root_normals.reserve(src.vertex_normal.num_elements);
			for (uint32_t i = 0; i < src.vertex_position.num_elements; i++) {
				root_positions.push_back(ufbx_transform_position(&src.node.to_root, src.vertex_position.data[i]));
			}
			for (uint32_t i = 0; i < src.vertex_normal.num_elements; i++) {
				root_normals.push_back(ufbx_transform_direction(&normal_mat, src.vertex_normal.data[i]));
			}

			ctx.positions = root_positions.data();
			ctx.normals = root_normals.data();
		}

		vec3 pos_min, pos_max;
		vec2 uv_min, uv_max;

		// Calculate bounds
		if (src.vertex_position.num_elements > 0) {
			pos_min = pos_max = from_ufbx(src.vertex_position.data[0]);
			for (size_t i = 1; i < src.vertex_position.num_elements; i++) {
				vec3 v = from_ufbx(src.vertex_position.data[i]);
				pos_min = min(pos_min, v);
				pos_max = max(pos_max, v);
			}
		}
		if (src.vertex_uv.num_elements > 0) {
			uv_min = uv_max = from_ufbx(src.vertex_uv.data[0]);
			for (size_t i = 1; i < src.vertex_uv.num_elements; i++) {
				vec2 v = from_ufbx(src.vertex_uv.data[i]);
				uv_min = min(uv_min, v);
				uv_max = max(uv_max, v);
			}
		}

		// Add vertices
		for (size_t fi = 0; fi < src.num_faces; fi++) {
			ufbx_face face = src.faces[fi];
			int mat_ix;
			if (src.face_material) {
				mat_ix = (int)(src.materials.data[src.face_material[fi]] - scene->materials.data);
			} else {
				if (!has_null_material) {
					material null_mat;
					null_mat.src = NULL;
					model.materials.push_back(null_mat);
					has_null_material = true;
				}
				mat_ix = (int)model.materials.size() - 1;
			}

			mesh_part *part = NULL;
			for (mesh_part &p : dst.parts) {
				if (p.material_index == mat_ix) {
					part = &p;
					break;
				}
			}
			if (!part) {
				mesh_part p;
				p.material_index = mat_ix;
				dst.parts.push_back(std::move(p));
				part = &dst.parts.back();
			}

			part->weights_per_vertex = opts.weights_per_vertex;

			if (src.vertex_position.num_elements > 0) {
				if (part->has_position_bounds) {
					part->min_position = min(part->min_position, pos_min);
					part->max_position = max(part->max_position, pos_max);
				} else {
					part->has_position_bounds = true;
					part->min_position = pos_min;
					part->max_position = pos_max;
				}
			}

			if (src.vertex_uv.num_elements > 0) {
				if (part->has_uv_bounds) {
					part->min_uv = min(part->min_uv, uv_min);
					part->max_uv = max(part->max_uv, uv_max);
				} else {
					part->has_uv_bounds = true;
					part->min_uv = uv_min;
					part->max_uv = uv_max;
				}
			}

			for (uint32_t bi = 1; bi + 2 <= face.num_indices; bi++) {
				add_vertex(ctx, *part, face.index_begin + 0);
				add_vertex(ctx, *part, face.index_begin + bi + 0);
				add_vertex(ctx, *part, face.index_begin + bi + 1);
			}
		}

		if (!opts.combine_meshes) {
			model.meshes.push_back(std::move(unique_mesh));
		}

	}

	if (opts.combine_meshes) {
		model.meshes.push_back(std::move(shared_mesh));
	}

	return model;
}

}
#endif
