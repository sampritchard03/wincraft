extends RefCounted
## Builds a DensityShape out of simple solids in GDScript: balls,
## ellipsoids and tapered capsules (cones with round ends), each of a
## material. Solids are unioned (the most solid wins, and its material where
## it's the surface). Procedural features (scripts/features/) are made with
## it.
##
##   var b := ShapeBuilder.new()
##   b.add_capsule(Vector3.ZERO, Vector3(0, 20, 0), 2.5, 1.5, wood)
##   b.add_ball(Vector3(0, 22, 0), 8.0, leaves)
##   var shape := b.build()
##
## Positions are in the shape's own space (the voxel size is only how finely
## it's sampled). A shape holds up to DensityShape.MAX_SIZE points a side:
## build() makes the voxels coarser if the solids don't fit.

const STEP := 16.0  # a saved density's steps per voxel (DENSITY_STEP is 1/16)
const LIMIT := 127  # the largest saved density step (DENSITY_CLAMP is LIMIT / STEP voxels)
const MAX_SIZE := 64  # DensityShape.MAX_SIZE

var voxel_size := 1.0
var _solids: Array = []


func _init(p_voxel_size := 1.0) -> void:
	voxel_size = p_voxel_size


func add_ball(p_center: Vector3, p_radius: float, p_material: int) -> void:
	add_ellipsoid(p_center, Vector3.ONE * p_radius, p_material)


## An ellipsoid of half-sizes p_radii along x, y and z.
func add_ellipsoid(p_center: Vector3, p_radii: Vector3, p_material: int) -> void:
	_solids.append({"type": 0, "a": p_center, "r": p_radii, "material": p_material})


## A capsule from p_a to p_b, p_radius_a thick at p_a and p_radius_b at p_b.
func add_capsule(p_a: Vector3, p_b: Vector3, p_radius_a: float, p_radius_b: float, p_material: int) -> void:
	_solids.append({"type": 1, "a": p_a, "b": p_b, "ra": p_radius_a, "rb": p_radius_b, "material": p_material})


func is_empty() -> bool:
	return _solids.is_empty()


## The shape: one key (no shape keys), at voxel_size or coarser (built
## natively: DensityShape.make_solids()).
func build() -> DensityShape:
	if _solids.is_empty():
		return null
	return DensityShape.make_solids(_solids, voxel_size)


## A DensityShape from a grid of densities (in voxels, positive inside) and
## materials (x fastest, then y, then z).
static func _make(p_size: Vector3i, p_origin: Vector3, p_voxel_size: float, p_density: PackedFloat32Array, p_materials: PackedInt32Array) -> DensityShape:
	var count := p_size.x * p_size.y * p_size.z
	var rest := PackedByteArray()
	rest.resize(count)
	var mats := PackedByteArray()
	mats.resize(count * 2)
	for i in count:
		rest[i] = clampi(roundi(p_density[i] * STEP), -LIMIT, LIMIT) & 0xFF
		var m := p_materials[i]
		mats[2 * i] = m & 0xFF
		mats[2 * i + 1] = (m >> 8) & 0xFF
	var shape := DensityShape.new()
	shape.set_voxel_size(p_voxel_size)
	shape.set_size(p_size)
	shape.set_origin(p_origin)
	shape.set_rest(rest)
	shape.set_materials(mats)
	return shape


## p_shape (its rest shape: key 0) sampled again at p_voxel_size: covering
## the same space, one key.
static func resample(p_shape: DensityShape, p_voxel_size: float) -> DensityShape:
	var old_size := p_shape.get_size()
	var old_vs := p_shape.get_voxel_size()
	var origin := p_shape.get_origin()
	var extent := Vector3(old_size - Vector3i.ONE) * old_vs
	var vs := p_voxel_size
	var size := Vector3i((extent / vs).ceil()) + Vector3i.ONE
	while size.x > MAX_SIZE or size.y > MAX_SIZE or size.z > MAX_SIZE:
		vs *= 2.0
		size = Vector3i((extent / vs).ceil()) + Vector3i.ONE
	var count := size.x * size.y * size.z
	var density := PackedFloat32Array()
	density.resize(count)
	var materials := PackedInt32Array()
	materials.resize(count)
	var i := 0
	for z in size.z:
		for y in size.y:
			for x in size.x:
				var p := origin + Vector3(x, y, z) * vs
				density[i] = p_shape.get_density(p) / vs
				materials[i] = maxi(p_shape.get_material_at(p), 0)
				i += 1
	return _make(size, origin, vs, density, materials)
