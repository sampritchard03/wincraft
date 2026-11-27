extends "res://scripts/objects/physics_object.gd"
## An entity: the physics objects that are more than loose ground (bouncy
## balls, frogs, chiggins, and any scene the object editor exports). Only
## entities may have:
##   - shape keys, and the animations that play them (the object editor's
##     keys and timeline are for entities alone), and
##   - voxels finer than the terrain's (ENTITY_VOXEL_SIZE for the shapes
##     made for them, such as a ball's), for more detail at a small size.

## The voxel size an entity's own shapes are made at (half the terrain's).
const ENTITY_VOXEL_SIZE := 0.5


func is_entity() -> bool:
	return true


func get_object_voxel_size() -> float:
	return ENTITY_VOXEL_SIZE
