// How the terrain and the bodies look, for terrain.gdshader and
// skinned_bodies.gdshader alike: the sun and sky, and the block textures.

uniform vec3 sun_direction = vec3(0.45, 0.75, 0.35);
uniform vec3 sun_color : source_color = vec3(1.0, 0.94, 0.82);
uniform vec3 sky_color : source_color = vec3(0.45, 0.62, 0.88);
uniform vec3 horizon_color : source_color = vec3(0.78, 0.84, 0.9);
// Block textures, one layer per material (SculptTerrain.get_material_names()
// order), 16 texels per voxel. Set by world.gd.
uniform sampler2DArray block_textures : source_color, filter_nearest_mipmap, repeat_enable;

uniform float ambient_strength : hint_range(0.0, 1.0) = 0.45;

// Voxels one repeat of a block texture (and the sea's ripples) covers.
uniform float texture_size = 8.0;

// A block texture projected along the axis the surface faces most (blending
// only where two are close, so the pixel art stays crisp). p_footprint: how
// wide a pixel is there, in voxels, which picks the mip level.
vec3 block_albedo(vec3 p_position, vec3 n, int block, float p_footprint) {
	// One texture (16 texels) across texture_size voxels, wrapped to one
	// repeat here, so the coordinates stay small however far from the origin
	// (kinder to a mobile GPU's texture unit). The mip level is explicit, so
	// the wrap leaves no seam.
	vec3 p = fract(p_position / texture_size);
	float lod = max(log2(p_footprint * 16.0 / texture_size), 0.0);
	vec3 w = pow(abs(n), vec3(8.0));
	w /= w.x + w.y + w.z;
	float layer = float(block);
	vec3 c = vec3(0.0);
	if (w.x > 0.01) {
		c += w.x * textureLod(block_textures, vec3(p.z * sign(n.x), -p.y, layer), lod).rgb;
	}
	if (w.y > 0.01) {
		c += w.y * textureLod(block_textures, vec3(p.x, p.z * sign(n.y), layer), lod).rgb;
	}
	if (w.z > 0.01) {
		c += w.z * textureLod(block_textures, vec3(-p.x * sign(n.z), -p.y, layer), lod).rgb;
	}
	return c;
}


// The sky's colour in a direction, without the sun.
vec3 sky_base(vec3 rd) {
	vec3 c = mix(horizon_color, sky_color, sqrt(clamp(rd.y, 0.0, 1.0)));
	return mix(c, horizon_color * 0.6, clamp(-rd.y * 3.0, 0.0, 1.0));
}

