extends "res://scripts/features/procedural_feature.gd"
## A tree, built in code: a tall trunk that leans and bends a little, roots
## flaring into the ground, branches from its upper half, and big leafy
## clumps at the branch ends and the top. Wood is the wood block; the
## leaves are a green (a little different on each variant). About 45 voxels
## tall as built, placed at 1 to 1.8 times that (the player is 16).


func _init() -> void:
	id = "tree"
	per_region = 1.6
	grows_on = PackedStringArray(["grass", "dirt"])
	max_slope = 35.0
	sink = 2.0
	scale_range = Vector2(1.0, 1.8)
	spacing = 30.0
	variants = 8


func build(rng: RandomNumberGenerator) -> DensityShape:
	var b := ShapeBuilder.new()
	var wood := block("wood")
	var leaves := color(Color.from_hsv(rng.randf_range(0.24, 0.33), rng.randf_range(0.55, 0.75), rng.randf_range(0.38, 0.52)))
	var height := rng.randf_range(28.0, 36.0)
	var base_radius := rng.randf_range(2.6, 3.4)
	var top_radius := base_radius * 0.45

	# The trunk: four pieces, each bending a little off the last.
	var points: Array[Vector3] = [Vector3(0, -2.0, 0)]
	var lean := Vector3(rng.randf_range(-1, 1), 0, rng.randf_range(-1, 1)).normalized() * rng.randf_range(0.0, 0.1)
	var dir := (Vector3.UP + lean).normalized()
	var piece := (height + 2.0) / 4.0
	for i in 4:
		dir = (dir + Vector3(rng.randf_range(-0.1, 0.1), 0, rng.randf_range(-0.1, 0.1))).normalized()
		points.append(points.back() + dir * piece)
	for i in 4:
		b.add_capsule(points[i], points[i + 1], lerpf(base_radius, top_radius, i / 4.0), lerpf(base_radius, top_radius, (i + 1) / 4.0), wood)
	var top: Vector3 = points.back()

	# Roots: flaring out at the foot, down into the ground.
	var root_count := rng.randi_range(4, 6)
	var turn := rng.randf() * TAU
	for i in root_count:
		var angle := turn + TAU * i / root_count + rng.randf_range(-0.3, 0.3)
		var out := Vector3(cos(angle), 0, sin(angle))
		b.add_capsule(Vector3(0, 3.5, 0) + out * base_radius * 0.3, out * rng.randf_range(5.0, 7.0) + Vector3(0, -2.5, 0),
				base_radius * 0.7, 0.8, wood)

	# Branches from the upper half, up and out, each with a clump of leaves.
	var branch_count := rng.randi_range(3, 5)
	turn = rng.randf() * TAU
	for i in branch_count:
		var along := rng.randf_range(0.45, 0.85)
		var segment := mini(int(along * 4.0), 3)
		var start: Vector3 = points[segment].lerp(points[segment + 1], along * 4.0 - segment)
		var angle := turn + TAU * i / branch_count + rng.randf_range(-0.4, 0.4)
		var out := Vector3(cos(angle), rng.randf_range(0.45, 0.9), sin(angle)).normalized()
		var end := start + out * rng.randf_range(7.0, 11.0)
		b.add_capsule(start, end, lerpf(base_radius, top_radius, along) * 0.6, 0.8, wood)
		_clump(b, rng, end + Vector3.UP * 2.0, rng.randf_range(6.0, 8.0), leaves)

	# The crown.
	_clump(b, rng, top + Vector3.UP * 3.0, rng.randf_range(8.0, 10.0), leaves)
	return b.build()


## A lumpy clump of leaves round p_center, about p_radius across: a squat
## ball with smaller ones bulging out of it.
func _clump(b: ShapeBuilder, rng: RandomNumberGenerator, p_center: Vector3, p_radius: float, p_material: int) -> void:
	b.add_ellipsoid(p_center, Vector3(p_radius, p_radius * 0.75, p_radius), p_material)
	for i in rng.randi_range(4, 6):
		var angle := rng.randf() * TAU
		var offset := Vector3(cos(angle), rng.randf_range(-0.3, 0.6), sin(angle)) * p_radius * 0.6
		var r := p_radius * rng.randf_range(0.45, 0.65)
		b.add_ellipsoid(p_center + offset, Vector3(r, r * 0.8, r), p_material)
