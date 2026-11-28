extends "res://scripts/features/feature.gd"
## A feature sculpted by hand in the game: build it out in the world,
## select it, Ctrl+P (or the object editor's "Export model") and save the
## .res in res://assets/features/. A subclass names the file(s) in
## shape_paths; each placement picks one.
##
## Where it meets the ground is found from the shape itself: the middle of
## its lowest solid layer (the foot of a stem, say), so it doesn't matter
## where the selection's centre was. Only its plain shape is placed (key 0:
## features are ground, without shape keys).

## The shape files (.res / .tres DensityShapes).
var shape_paths: PackedStringArray = []


func get_shapes() -> Array[DensityShape]:
	var shapes: Array[DensityShape] = []
	for path in shape_paths:
		var shape := _load(path)
		if shape != null:
			shapes.append(shape)
	return shapes


## The file's shape, moved so its foot is at its local origin.
func _load(p_path: String) -> DensityShape:
	if not ResourceLoader.exists(p_path):
		push_warning("Feature %s: no shape at %s" % [id, p_path])
		return null
	var loaded := load(p_path) as DensityShape
	if loaded == null or loaded.get_key_count() == 0:
		push_warning("Feature %s: %s isn't a density shape" % [id, p_path])
		return null
	var shape := loaded.duplicate_shape()
	while shape.get_key_count() > 1:
		shape.remove_key(shape.get_key_count() - 1)
	shape.set_origin(shape.get_origin() - foot_of(shape))
	return shape


## The middle of p_shape's lowest solid layer (in its own space).
static func foot_of(p_shape: DensityShape) -> Vector3:
	var size := p_shape.get_size()
	var vs := p_shape.get_voxel_size()
	var origin := p_shape.get_origin()
	for y in size.y:
		var sum := Vector3.ZERO
		var count := 0
		for z in size.z:
			for x in size.x:
				var p := origin + Vector3(x, y, z) * vs
				if p_shape.get_density(p) > 0.0:
					sum += p
					count += 1
		if count > 0:
			return sum / count
	return origin + Vector3(size) * vs * 0.5
