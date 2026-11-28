extends Node3D
## Keeps the world's loose physics objects (physics_object.gd): bouncy balls
## the player throws (F; an entity, ball.gd) and plain objects (P on a
## selection, Ctrl+O, pieces cut off). Frogs and chiggins are entities too,
## but creatures.gd keeps those.
##
## Each is a DensityBody3D with a small density grid of its own: it collides
## with the terrain through its density field (no collision meshes) and with
## the other bodies, is drawn by the terrain's raymarching shader, lit and
## shadowed like the ground, and can be sculpted with the terrain's brushes
## (a sculpted one is saved with its new shape). SculptTerrain steps them
## all; one at rest falls asleep and is woken when an edit changes the
## ground near it. They're saved to objects.cfg in the world's save
## directory. Only those near the player are loaded (see stream()).

const BodyAnimation := preload("res://scripts/editor/body_animation.gd")
const PhysicsObject := preload("res://scripts/objects/physics_object.gd")
const Ball := preload("res://scripts/objects/entities/ball.gd")

@export var ball_radius := 3.0
@export var throw_speed := 220.0
@export var bounce := 0.8
@export var max_objects := 200  # loaded or not; the oldest goes
## Objects farther than this from the player are put away; they come back
## within load_distance (less, so one on the boundary isn't swapped every frame).
@export var unload_distance := 512.0
@export var load_distance := 448.0

var terrain: SculptTerrain
var bodies: Array[DensityBody3D] = []  # loaded
var unloaded: Array[Dictionary] = []  # put away (see stream()), as saved


func _init(p_terrain: SculptTerrain) -> void:
	name = "PhysicsObjects"
	terrain = p_terrain


## Throws a ball from p_from along p_dir, adding p_carry (the thrower's
## velocity). p_material: a block to make it of (its texture), or -1 for a
## random colour.
func throw(p_from: Vector3, p_dir: Vector3, p_carry: Vector3, p_material := -1) -> void:
	var color := Color.from_hsv(randf(), 0.75, 0.95)
	var ball := _make(Ball, Ball.make_shape(ball_radius, color, p_material), color, p_material)
	ball.position = p_from + p_dir * (ball_radius + 2.0)
	ball.linear_velocity = p_dir * throw_speed + p_carry
	ball.angular_velocity = Vector3(randf_range(-4, 4), randf_range(-4, 4), randf_range(-4, 4))


## Adds a plain physics object made of p_shape (a selection, an imported
## file, a piece cut off) at p_position. Plain objects have no shape keys
## and terrain-sized voxels: p_shape is made so if it isn't
## (PhysicsObject.conform_shape()). Its mass goes with its volume (a ball is
## about 1).
func add_shape_body(p_shape: DensityShape, p_position: Vector3) -> DensityBody3D:
	var shape := PhysicsObject.conform_shape(p_shape)
	var body := _make(PhysicsObject, shape, Color.WHITE, -1)
	body.mass = maxf(shape.get_volume() / 113.0, 0.2)
	body.global_position = p_position
	return body


## A ball made of p_shape as it is (another player's, which may be sculpted).
func add_ball_body(p_shape: DensityShape, p_position: Vector3) -> DensityBody3D:
	var ball := _make(Ball, p_shape, Color.WHITE, -1)
	ball.global_position = p_position
	return ball


## Removes a body: one of these (forgotten), or anything else (a frog).
func remove(p_body: Node) -> void:
	if p_body in bodies:
		_break(p_body)
	else:
		p_body.queue_free()


## Call once per physics frame: objects farther than unload_distance from
## p_player are put away (kept only as data, saved with the rest) and ones
## within load_distance come back, at whichever repeat of the wrapping world
## is nearest the player. Objects that fell out of the world break.
## p_unload false (multiplayer): only the breaking.
func stream(p_player: Vector3, p_unload := true) -> void:
	for i in range(bodies.size() - 1, -1, -1):
		var body := bodies[i]
		if body.global_position.y < -terrain.world_height * 0.5 - 64.0:
			if not body.freeze:  # (a frozen copy of another player's follows them)
				_break(body)
		elif p_unload and _offset_from(p_player, body.global_position).length() > unload_distance:
			unloaded.append(entry_of(body))
			bodies.remove_at(i)
			body.queue_free()
	for i in range(unloaded.size() - 1, -1, -1):
		var d := _offset_from(p_player, unloaded[i].get("position", Vector3.ZERO))
		if d.length() < load_distance:
			var entry: Dictionary = unloaded[i]
			unloaded.remove_at(i)
			restore(entry, p_player + d)


## Brings every put-away object back, each at the repeat of the wrapping
## world nearest p_player (multiplayer keeps them all loaded).
func load_all(p_player: Vector3) -> void:
	for entry in unloaded:
		restore(entry, p_player + _offset_from(p_player, entry.get("position", Vector3.ZERO)))
	unloaded.clear()


## p_creatures: creatures.gd's to_save(), saved beside them.
func save(p_path: String, p_creatures := {}) -> void:
	var list := unloaded.duplicate()
	for body in bodies:
		list.append(entry_of(body))
	var config := ConfigFile.new()
	config.set_value("objects", "balls", list)  # (the key from when they were all balls)
	for key in p_creatures:
		config.set_value("creatures", key, p_creatures[key])
	var path := ProjectSettings.globalize_path(p_path)
	if config.save(path + ".tmp") == OK:
		DirAccess.rename_absolute(path + ".tmp", path)


## Reads saved objects; they appear (stream()) when the player is near them.
func load_from(p_path: String) -> void:
	var config := ConfigFile.new()
	if config.load(p_path) != OK:
		return
	for entry in config.get_value("objects", "balls", []):
		if entry is Dictionary:
			unloaded.append(entry)


## Whether a body is a ball (its kind, as saved and sent to other players).
func kind_of(p_body: DensityBody3D) -> String:
	return "ball" if p_body.get_script() == Ball else "object"


func entry_of(p_body: DensityBody3D) -> Dictionary:
	return {
		"kind": kind_of(p_body),
		"position": p_body.global_position,
		"rotation": p_body.global_basis.get_rotation_quaternion(),
		"velocity": p_body.linear_velocity,
		"spin": p_body.angular_velocity,
		"color": p_body.color,
		"material": p_body.material,
		"shape": p_body.shape.to_dict() if p_body.shape.is_edited() or kind_of(p_body) != "ball" else null,
		"sleeping": p_body.sleeping,
		"mass": p_body.mass,
		"frozen": p_body.freeze,
		"key_weights": p_body.key_weights,
		"animation": BodyAnimation.to_dict(p_body),  # its animations, and the one it plays
	}


func restore(p_entry: Dictionary, p_position: Vector3) -> DensityBody3D:
	var color: Color = p_entry.get("color", Color.WHITE)
	var material: int = p_entry.get("material", -1)
	# (Saves from before kinds: a ball has a colour or a block of its own.)
	var kind: String = p_entry.get("kind", "ball" if material >= 0 or color != Color.WHITE else "object")
	var body: DensityBody3D
	var saved: DensityShape = DensityShape.from_dict(p_entry.shape) if p_entry.get("shape") is Dictionary else null
	if kind == "ball":
		body = _make(Ball, saved if saved != null else Ball.make_shape(ball_radius, color, material), color, material)
	elif saved != null:
		body = _make(PhysicsObject, PhysicsObject.conform_shape(saved), color, material)
	else:
		return null
	body.global_transform = Transform3D(Basis(p_entry.get("rotation", Quaternion.IDENTITY)), p_position)
	body.linear_velocity = p_entry.get("velocity", Vector3.ZERO)
	body.angular_velocity = p_entry.get("spin", Vector3.ZERO)
	body.sleeping = p_entry.get("sleeping", false)
	body.mass = p_entry.get("mass", 1.0)
	body.freeze = p_entry.get("frozen", false)
	body.key_weights = p_entry.get("key_weights", PackedFloat32Array())
	BodyAnimation.apply(body, p_entry.get("animation", {}))
	return body


## p_to - p_from the short way round the wrapping world (x and z).
func _offset_from(p_from: Vector3, p_to: Vector3) -> Vector3:
	var size := float(terrain.get_world_size())
	var d := p_to - p_from
	d.x = fposmod(d.x + size * 0.5, size) - size * 0.5
	d.z = fposmod(d.z + size * 0.5, size) - size * 0.5
	return d


## A new body of p_script (PhysicsObject or Ball) made of p_shape.
func _make(p_script: GDScript, p_shape: DensityShape, p_color: Color, p_material: int) -> DensityBody3D:
	if bodies.size() + unloaded.size() >= max_objects:
		if not unloaded.is_empty():
			unloaded.remove_at(0)  # the oldest put-away object
		else:
			_break(bodies[0])
	var body: DensityBody3D = p_script.new()
	body.shape = p_shape
	body.mass = 1.0
	body.bounce = bounce
	body.friction = 0.6
	body.angular_damp = 0.3
	body.drawn = true
	body.color = p_color
	body.material = p_material
	add_child(body)
	bodies.append(body)
	return body


func _break(p_body: DensityBody3D) -> void:
	bodies.erase(p_body)
	p_body.queue_free()
