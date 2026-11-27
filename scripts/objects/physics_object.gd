extends DensityBody3D
## A physics object: a loose DensityBody3D in the world (P on a selection,
## an imported shape, a piece cut off), kept by physics_objects.gd.
##
## Plain physics objects are ground set loose: one shape (no shape keys) of
## terrain-sized voxels, so P puts one back into the terrain voxel for voxel.
## Entities (entity.gd: bouncy balls, frogs, chiggins) are the subcategory
## that may have more: shape keys (and the animations that play them) and
## finer voxels.
##
##   PhysicsObject (this)
##   └─ Entity (entity.gd)
##      ├─ Ball (ball.gd)
##      └─ Creature (creature.gd)
##         ├─ Frog (forg.gd)
##         └─ Chiggin (chiggin.gd)

const ShapeBuilder := preload("res://scripts/objects/shape_builder.gd")

## A plain physics object's voxels: the terrain's.
const VOXEL_SIZE := 1.0


## Whether it may have shape keys and finer voxels (entity.gd says yes).
func is_entity() -> bool:
	return false


## The voxel size its shapes are made at.
func get_object_voxel_size() -> float:
	return VOXEL_SIZE


## Whether p_body is an entity (a body without this script, such as another
## player's avatar or the floating selection, isn't).
static func body_is_entity(p_body: Object) -> bool:
	return p_body is DensityBody3D and p_body.has_method("is_entity") and p_body.is_entity()


## p_shape as a plain physics object may have it: key 0 alone (the rest
## shape), at terrain-sized voxels (resampled if finer). p_shape itself if it
## already is; otherwise a new shape.
static func conform_shape(p_shape: DensityShape) -> DensityShape:
	if p_shape == null:
		return null
	var shape := p_shape
	if shape.get_voxel_size() < VOXEL_SIZE - 1e-4:
		shape = ShapeBuilder.resample(shape, VOXEL_SIZE)
	if shape.get_key_count() > 1:
		if shape == p_shape:
			shape = shape.duplicate_shape()
		while shape.get_key_count() > 1:
			shape.remove_key(shape.get_key_count() - 1)
	return shape

