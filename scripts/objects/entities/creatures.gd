extends Node
## Creatures: frogs, chiggins, and anything else dropped from a scene (a
## DensityBody3D whose scene_file_path is set), in the world beside the
## player (world.gd's camera's parent).
##
## Every creature of a kind (the same scene) shares one shape: what's done
## to one in the object editor (M: sculpting it, giving it keys) changes
## them all, and so does undoing that. Sculpted out in the world, a creature
## gets a shape of its own instead (undone, it has the kind's again). Their
## animations are the scene's, one AnimationLibrary shared by them all, so
## an animation made or changed on one plays on every one.
##
## Saved with the world's objects (objects.cfg, beside the balls): each
## creature (where it is, how it's moving, its pose, its own shape if it has
## one) and each kind's shape and animations, so what you sculpted and
## keyframed on a chiggin is still there next time. save_as_default() makes
## a kind's shape and animations the game's own (its scene's files). Multiplayer: a kind's shape goes to the other players
## when it's changed here (network.gd, send_kind_shape()).

const BodyAnimation := preload("res://scripts/editor/body_animation.gd")
const META_SEEN := "creature_shape"  # the shape the creature had when last looked at
const META_VERSION := "creature_version"  # and its version (edits in place)

var world: Node  # world.gd
## Scene path -> the shape every creature of that kind has now.
var kind_shapes: Dictionary = {}
## Scene path -> the kind's earlier shapes (newest last, a few): a creature
## going back to one (undo) takes the whole kind back with it.
var kind_history: Dictionary = {}
const HISTORY := 32
## Scene path -> the AnimationLibrary its creatures share.
var kind_libraries: Dictionary = {}
## Scene path -> its facing_offset (degrees, creature.gd): every one of the kind's.
var kind_facing: Dictionary = {}
## Scene path -> {"shape": dict or null, "animation": BodyAnimation.to_dict()},
## loaded and not yet put on a creature of that kind.
var loaded_kinds: Dictionary = {}
## Loaded creatures waiting for the world (made in restore_loaded()).
var loaded_creatures: Array = []


func _init(p_world: Node) -> void:
	name = "Creatures"
	world = p_world


## Every creature now in the world.
func all() -> Array[DensityBody3D]:
	var list: Array[DensityBody3D] = []
	for child in world.camera.get_parent().get_children():
		if child is DensityBody3D and not child is DensityCharacter3D and child.scene_file_path != "" \
				and child.shape != null and not child.is_queued_for_deletion():
			list.append(child)
	return list


## A new creature from p_scene at p_position (the kind's shape, as all of them have it).
func spawn(p_scene: PackedScene, p_position: Vector3, p_yaw := 0.0) -> DensityBody3D:
	var body := p_scene.instantiate() as DensityBody3D
	world.camera.get_parent().add_child(body)
	body.global_position = p_position
	body.rotation.y = p_yaw
	_meet(body)
	return body


func _process(_delta: float) -> void:
	var changed := {}  # scene path -> the kind's shape before: a creature of that kind changed it
	var creatures := all()
	for body in creatures:
		if not body.has_meta(META_SEEN):
			_meet(body)
			continue
		# (A creature with a shape of its own, edited in the object editor: in
		# place, the same shape, so it's told by the version.)
		var version: int = body.shape.get_version()
		var edited_in_editor: bool = body.has_meta("editing") and body.get_meta(META_VERSION, version) != version
		body.set_meta(META_VERSION, version)
		if body.get_meta(META_SEEN) == body.shape and not edited_in_editor:
			continue
		body.set_meta(META_SEEN, body.shape)
		if not body.get_meta("net_sim", true):
			continue  # another player's creature, as they have it (their changes come with send_kind_shape())
		var path := body.scene_file_path
		var kind: DensityShape = kind_shapes.get(path)
		if body.shape == kind or changed.has(path):
			continue
		# Edited in the object editor (or undone back to an earlier shape of
		# the kind's): the whole kind's. Sculpted out in the world: its own.
		var history: Array = kind_history.get(path, [])
		if body.has_meta("editing") or history.has(body.shape):
			history.erase(body.shape)
			history.append(kind)
			while history.size() > HISTORY:
				history.pop_front()
			kind_history[path] = history
			kind_shapes[path] = body.shape
			changed[path] = kind
	for path in changed:
		_share(path, creatures, changed[path])
		world.network.send_kind_shape(path, kind_shapes[path])


## A creature seen for the first time: the first of its kind sets the
## kind's shape (from the save, if it has one) and its animations; the
## others take it on. (Another player's creature keeps the shape they sent.)
func _meet(p_body: DensityBody3D) -> void:
	var path := p_body.scene_file_path
	if not kind_shapes.has(path):
		var saved: Dictionary = loaded_kinds.get(path, {})
		loaded_kinds.erase(path)
		if saved.get("shape") is Dictionary:
			kind_shapes[path] = DensityShape.from_dict(saved.shape)
		else:
			kind_shapes[path] = p_body.shape
		kind_facing[path] = float(saved.get("facing", p_body.get("facing_offset") if p_body.get("facing_offset") != null else 0.0))
		if saved.get("animation") is Dictionary:
			BodyAnimation.apply(p_body, saved.animation)  # (into the library every one of its kind shares)
		var player := BodyAnimation.player_of(p_body)
		if player != null and player.has_animation_library(""):
			kind_libraries[path] = player.get_animation_library("")
	if p_body.get_meta("net_sim", true) and p_body.shape != kind_shapes[path]:
		p_body.shape = kind_shapes[path]
	if p_body.get("facing_offset") != null:
		p_body.set("facing_offset", kind_facing.get(path, 0.0))
	p_body.set_meta(META_SEEN, p_body.shape)


## Every creature of a kind that had its old shape (p_old) takes on the
## new one (those with shapes of their own keep them; other players'
## creatures come as they send them).
func _share(p_path: String, p_creatures: Array[DensityBody3D], p_old: DensityShape) -> void:
	var shape: DensityShape = kind_shapes[p_path]
	for body in p_creatures:
		if body.scene_file_path == p_path and body.shape == p_old and body.get_meta("net_sim", true):
			body.shape = shape
			body.set_meta(META_SEEN, shape)


## Another player changed a kind's shape.
func take_kind_shape(p_path: String, p_shape: DensityShape) -> void:
	var old: DensityShape = kind_shapes.get(p_path)
	kind_shapes[p_path] = p_shape
	loaded_kinds.erase(p_path)
	_share(p_path, all(), old)


# --- Saving -------------------------------------------------------------------------

## What to save: {"list": [each creature], "kinds": {scene path: {"shape",
## "animation"}}}.
func to_save() -> Dictionary:
	var list := []
	for body in all():
		var frozen: bool = body.freeze if body.get_meta("net_sim", true) else body.get_meta("net_frozen", false)
		if body.has_meta("editing"):
			frozen = world.editor.was_frozen  # (held still by the editor meanwhile)
		var entry := {
			"scene": body.scene_file_path,
			"position": body.global_position,
			"rotation": body.global_basis.get_rotation_quaternion(),
			"velocity": body.linear_velocity,
			"spin": body.angular_velocity,
			"frozen": frozen,
			"pose": body.pose,
			"key_weights": body.key_weights,
		}
		if body.shape != kind_shapes.get(body.scene_file_path):
			entry["shape"] = body.shape.to_dict()  # (sculpted: its own)
		list.append(entry)
	list.append_array(loaded_creatures)  # (not made yet: kept)
	var kinds := loaded_kinds.duplicate()  # (kinds not met this time: kept as they were)
	for path in kind_shapes:
		var kind := {"shape": (kind_shapes[path] as DensityShape).to_dict(), "facing": kind_facing.get(path, 0.0)}
		var library: AnimationLibrary = kind_libraries.get(path)
		if library != null:
			var animations := {}
			for anim_name in library.get_animation_list():
				animations[str(anim_name)] = BodyAnimation.animation_to_dict(library.get_animation(anim_name))
			kind["animation"] = {"animations": animations, "plays": ""}
		kinds[path] = kind
	return {"list": list, "kinds": kinds}


func load_from(p_config: ConfigFile) -> void:
	loaded_kinds = p_config.get_value("creatures", "kinds", {})
	loaded_creatures = p_config.get_value("creatures", "list", [])


## Makes the loaded creatures, each at the repeat of the wrapping world
## nearest p_player.
func restore_loaded(p_player: Vector3) -> void:
	var entries := loaded_creatures
	loaded_creatures = []
	for entry in entries:
		if not entry is Dictionary or not ResourceLoader.exists(entry.get("scene", "")):
			continue
		var scene := load(entry.scene) as PackedScene
		if scene == null:
			continue
		var at: Vector3 = entry.get("position", Vector3.ZERO)
		var body := spawn(scene, p_player + world.balls._offset_from(p_player, at))
		if entry.get("shape") is Dictionary:
			body.shape = DensityShape.from_dict(entry.shape)
			body.set_meta(META_SEEN, body.shape)
		body.global_basis = Basis(entry.get("rotation", Quaternion.IDENTITY))
		body.linear_velocity = entry.get("velocity", Vector3.ZERO)
		body.angular_velocity = entry.get("spin", Vector3.ZERO)
		body.freeze = entry.get("frozen", false)
		body.pose = entry.get("pose", 0.0)
		body.key_weights = entry.get("key_weights", PackedFloat32Array())


# --- The game's own ------------------------------------------------------------------

## Makes p_body's kind (its scene) start out as it is now, for every world:
## its shape (and shape keys) into the scene's shape file, its animations
## into their files (res://<scene name>/<animation>.tres, where the scene
## keeps them there), or into the scene itself. Returns what it did, or
## what went wrong. (Only while running from the project: an exported
## game's files can't be written.)
## Every one of a kind faces p_degrees off (creature.gd, facing_offset): here,
## and for the other players unless p_from_network.
func set_kind_facing(p_path: String, p_degrees: float, p_from_network := false) -> void:
	kind_facing[p_path] = p_degrees
	for body in all():
		if body.scene_file_path == p_path and body.get("facing_offset") != null:
			body.set("facing_offset", p_degrees)
	if not p_from_network:
		world.network.send_kind_facing(p_path, p_degrees)


func save_as_default(p_body: DensityBody3D) -> String:
	var path := p_body.scene_file_path
	var scene := load(path) as PackedScene if path != "" else null
	if scene == null:
		return "Not a creature (it has no scene of its own)"
	var root := scene.instantiate() as DensityBody3D
	if root == null:
		return "%s isn't a DensityBody3D scene" % path
	var failed := []
	# The shape: over the scene's shape file (or a new one beside the scene).
	var shape_path: String = root.shape.resource_path if root.shape != null else ""
	if shape_path == "" or shape_path.contains("::"):
		shape_path = path.get_basename() + "_shape.res"
	var shape: DensityShape = (kind_shapes.get(path, p_body.shape) as DensityShape).duplicate_shape()
	if ResourceSaver.save(shape, shape_path) == OK:
		shape.take_over_path(shape_path)
		root.shape = shape
	else:
		failed.append(shape_path)
	# The animations: each into its file, where the scene keeps them in files.
	var library: AnimationLibrary = kind_libraries.get(path)
	var player := BodyAnimation.player_of(root)
	var files := 0
	if library != null and player != null:
		var folder := path.get_basename()  # res://scenes/entities/chiggin.tscn -> res://chiggin/
		var in_files := DirAccess.dir_exists_absolute(folder)
		for anim_name in library.get_animation_list():
			var anim := library.get_animation(anim_name)
			var file := anim.resource_path
			if file == "" or file.contains("::"):
				file = folder.path_join(str(anim_name) + ".tres") if in_files else ""
			if file == "":
				continue  # (kept in the scene)
			if ResourceSaver.save(anim, file) == OK:
				anim.take_over_path(file)
				files += 1
			else:
				failed.append(file)
		if player.has_animation_library(""):
			if player.get_animation_library("") != library:
				player.remove_animation_library("")
				player.add_animation_library("", library)
		else:
			player.add_animation_library("", library)
	if root.get("facing_offset") != null:
		root.set("facing_offset", kind_facing.get(path, p_body.get("facing_offset")))
	# The scene, with them.
	root.scene_file_path = ""
	var packed := PackedScene.new()
	var error := packed.pack(root)
	root.free()
	if error == OK:
		error = ResourceSaver.save(packed, path)
	if error == OK:
		packed.take_over_path(path)
	else:
		failed.append(path)
	if not failed.is_empty():
		return "Couldn't write %s" % ", ".join(failed)
	return "Saved as the game's own %s: shape (%d keys) in %s, %d animations%s" % [path.get_file().get_basename(),
			shape.get_key_count(), shape_path.get_file(), library.get_animation_list().size() if library != null else 0,
			" (%d in files)" % files if files > 0 else " (in the scene)"]
