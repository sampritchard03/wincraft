extends "res://scripts/objects/entities/entity.gd"
## A bouncy ball (F throws one; physics_objects.gd makes and keeps them):
## an entity, so it's made of fine voxels and can be given shape keys and
## animations in the object editor.

## Its look: a colour (with a darker band round its middle to show it roll)
## or a block's texture (p_material >= 0).
static func make_shape(p_radius: float, p_color: Color, p_material: int) -> DensityShape:
	if p_material >= 0:
		return DensityShape.make_sphere(p_radius, p_material, -1, ENTITY_VOXEL_SIZE)
	return DensityShape.make_sphere(p_radius, SculptTerrain.color_to_material(p_color),
			SculptTerrain.color_to_material(p_color.darkened(0.35)), ENTITY_VOXEL_SIZE)
