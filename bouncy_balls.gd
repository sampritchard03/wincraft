extends Node3D
## Bouncy balls the player throws (F) and breaks by clicking them.
##
## Each ball is a DensityBody3D with a small density grid of its own
## (DensityShape.make_sphere()): it collides with the terrain through its
## density field (no collision meshes) and with the other bodies, is drawn
## by the terrain's raymarching shader, lit and shadowed like the ground,
## and can be sculpted with the terrain's brushes (a sculpted one is saved
## with its new shape). SculptTerrain steps them all; a ball at rest
## falls asleep and is woken when an edit changes the ground near it.
## Balls are saved to objects.cfg in the world's save directory. Only balls
## near the player are loaded (see stream()).

const BodyAnimation := preload("res://body_animation.gd")

@export var ball_radius := 3.0
@export var throw_speed := 220.0
@export var bounce := 0.8
@export var max_balls := 200  # loaded or not; the oldest goes
## Balls farther than this from the player are put away; they come back
## within load_distance (less, so one on the boundary isn't swapped every frame).
@export var unload_distance := 512.0
@export var load_distance := 448.0

var terrain: SculptTerrain
var balls: Array[DensityBody3D] = []  # loaded
var unloaded: Array[Dictionary] = []  # put away (see stream()), as saved

func _init(p_terrain: SculptTerrain) -> void:
	name = "BouncyBalls"
	terrain = p_terrain

## Throws a ball from p_from along p_dir, adding p_carry (the thrower's
## velocity). p_material: a block to make it of (its texture), or -1 for a
## random colour.
func throw(p_from: Vector3, p_dir: Vector3, p_carry: Vector3, p_material := -1) -> void:
	var ball := _make_ball(Color.from_hsv(randf(), 0.75, 0.95), p_material)
	ball.position = p_from + p_dir * (ball_radius + 2.0)
	ball.linear_velocity = p_dir * throw_speed + p_carry
	ball.angular_velocity = Vector3(randf_range(-4, 4), randf_range(-4, 4), randf_range(-4, 4))

## Adds a physics object made of p_shape (a selection, an imported file)
## at p_position, saved with the balls. Its mass goes with its volume (a
## ball is about 1).
func add_shape_body(p_shape: DensityShape, p_position: Vector3) -> DensityBody3D:
	var body := _make_ball(Color.WHITE, -1)
	body.shape = p_shape
	body.mass = maxf(p_shape.get_volume() / 113.0, 0.2)
	body.global_position = p_position
	return body

## Removes a body: a ball (forgotten), or anything else (a frog).
func remove(p_body: Node) -> void:
	if p_body in balls:
		_break(p_body)
	else:
		p_body.queue_free()

## Call once per physics frame: balls farther than unload_distance from
## p_player are put away (kept only as data, saved with the rest) and ones
## within load_distance come back, at whichever repeat of the wrapping world
## is nearest the player. Balls that fell out of the world break.
## p_unload false (multiplayer): only the breaking.
func stream(p_player: Vector3, p_unload := true) -> void:
	for i in range(balls.size() - 1, -1, -1):
		var ball := balls[i]
		if ball.global_position.y < -terrain.world_height * 0.5 - 64.0:
			if not ball.freeze:  # (a frozen copy of another player's ball follows them)
				_break(ball)
		elif p_unload and _offset_from(p_player, ball.global_position).length() > unload_distance:
			unloaded.append(_entry_of(ball))
			balls.remove_at(i)
			ball.queue_free()
	for i in range(unloaded.size() - 1, -1, -1):
		var d := _offset_from(p_player, unloaded[i].get("position", Vector3.ZERO))
		if d.length() < load_distance:
			var entry: Dictionary = unloaded[i]
			unloaded.remove_at(i)
			_restore(entry, p_player + d)

## Brings every put-away ball back, each at the repeat of the wrapping world
## nearest p_player (multiplayer keeps them all loaded).
func load_all(p_player: Vector3) -> void:
	for entry in unloaded:
		_restore(entry, p_player + _offset_from(p_player, entry.get("position", Vector3.ZERO)))
	unloaded.clear()

func save(p_path: String) -> void:
	var list := unloaded.duplicate()
	for ball in balls:
		list.append(_entry_of(ball))
	var config := ConfigFile.new()
	config.set_value("objects", "balls", list)
	var path := ProjectSettings.globalize_path(p_path)
	if config.save(path + ".tmp") == OK:
		DirAccess.rename_absolute(path + ".tmp", path)

## Reads saved balls; they appear (stream()) when the player is near them.
func load_from(p_path: String) -> void:
	var config := ConfigFile.new()
	if config.load(p_path) != OK:
		return
	for entry in config.get_value("objects", "balls", []):
		if entry is Dictionary:
			unloaded.append(entry)

func _entry_of(p_ball: DensityBody3D) -> Dictionary:
	return {
		"position": p_ball.global_position,
		"rotation": p_ball.global_basis.get_rotation_quaternion(),
		"velocity": p_ball.linear_velocity,
		"spin": p_ball.angular_velocity,
		"color": p_ball.color,
		"material": p_ball.material,
		"shape": p_ball.shape.to_dict() if p_ball.shape.is_edited() else null,
		"sleeping": p_ball.sleeping,
		"mass": p_ball.mass,
		"frozen": p_ball.freeze,
		"key_weights": p_ball.key_weights,
		"animation": BodyAnimation.to_dict(p_ball),  # its animations, and the one it plays
	}

func _restore(p_entry: Dictionary, p_position: Vector3) -> void:
	var ball := _make_ball(p_entry.get("color", Color.WHITE), p_entry.get("material", -1))
	if p_entry.get("shape") is Dictionary:
		ball.shape = DensityShape.from_dict(p_entry.shape)
	ball.global_transform = Transform3D(Basis(p_entry.get("rotation", Quaternion.IDENTITY)), p_position)
	ball.linear_velocity = p_entry.get("velocity", Vector3.ZERO)
	ball.angular_velocity = p_entry.get("spin", Vector3.ZERO)
	ball.sleeping = p_entry.get("sleeping", false)
	ball.mass = p_entry.get("mass", 1.0)
	ball.freeze = p_entry.get("frozen", false)
	ball.key_weights = p_entry.get("key_weights", PackedFloat32Array())
	BodyAnimation.apply(ball, p_entry.get("animation", {}))

## p_to - p_from the short way round the wrapping world (x and z).
func _offset_from(p_from: Vector3, p_to: Vector3) -> Vector3:
	var size := float(terrain.get_world_size())
	var d := p_to - p_from
	d.x = fposmod(d.x + size * 0.5, size) - size * 0.5
	d.z = fposmod(d.z + size * 0.5, size) - size * 0.5
	return d

func _make_ball(p_color: Color, p_material: int) -> DensityBody3D:
	if balls.size() + unloaded.size() >= max_balls:
		if not unloaded.is_empty():
			unloaded.remove_at(0)  # the oldest put-away ball
		else:
			_break(balls[0])
	var ball := DensityBody3D.new()
	if p_material >= 0:
		ball.shape = DensityShape.make_sphere(ball_radius, p_material)
	else:
		# A colour, with a darker band round its middle to show it roll.
		ball.shape = DensityShape.make_sphere(ball_radius, SculptTerrain.color_to_material(p_color),
				SculptTerrain.color_to_material(p_color.darkened(0.35)))
	ball.mass = 1.0
	ball.bounce = bounce
	ball.friction = 0.6
	ball.angular_damp = 0.3
	ball.drawn = true
	ball.color = p_color
	ball.material = p_material
	add_child(ball)
	balls.append(ball)
	return ball

func _break(p_ball: DensityBody3D) -> void:
	balls.erase(p_ball)
	p_ball.queue_free()
