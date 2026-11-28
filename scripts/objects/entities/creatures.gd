extends Node
## Creatures: frogs, chiggins, and anything else dropped from a scene (a
## DensityBody3D whose scene_file_path is set), in the world beside the
## player (world.gd's camera's parent).
##
## Every creature of a kind (the same scene) shares one shape, one
## AnimationLibrary and one facing: the kind's. What's done to one (in the
## object editor, M, or sculpted out in the world) is its own, until "Update
## others" in the editor (update_others()) gives it to the rest of its group:
## every other one of its kind, or, for one marked unique, only those of its
## kind with the same unique name (one unique with no name has no group).
## That's one undo step.
##
## Saved with the world's objects (objects.cfg, beside the balls): each
## creature (where it is, how it's moving, its pose, its own shape if it has
## one) and each kind's shape and animations, so what you sculpted and
## keyframed on a chiggin is still there next time. save_as_default() makes
## a kind's shape and animations the game's own (its scene's files). Multiplayer: a kind's shape goes to the other players
## when it's changed here (network.gd, send_kind_shape()).

const BodyAnimation := preload("res://scripts/editor/body_animation.gd")
## Where kinds' scenes were before the project's files were tidied into
## folders: saves made then name them so.
const MOVED_SCENES := {
	"res://forg.tscn": "res://scenes/entities/forg.tscn",
	"res://chiggin.tscn": "res://scenes/entities/chiggin.tscn",
}
const META_SEEN := "creature_shape"  # the shape the creature had when last looked at
## Unique (the object editor's "Is unique"): Update others on another of
## its kind leaves it be; its group is those with its unique name.
const META_UNIQUE := "unique"
const META_UNIQUE_NAME := "unique_name"

var world: Node  # world.gd
## Scene path -> the shape every creature of that kind has now.
var kind_shapes: Dictionary = {}
## Scene path -> the AnimationLibrary its creatures share.
var kind_libraries: Dictionary = {}
## Scene path -> its facing_offset (degrees, creature.gd): every one of the kind's.
var kind_facing: Dictionary = {}
## Scene path -> its facing_pitch (degrees, creature.gd).
var kind_pitch: Dictionary = {}
## Scene path -> {"shape": dict or null, "animation": BodyAnimation.to_dict()},
## loaded and not yet put on a creature of that kind.
var loaded_kinds: Dictionary = {}
## Loaded creatures waiting for the world (made in restore_loaded()).
var loaded_creatures: Array = []
## Scene path -> _fingerprint() of the kind as the game makes it (its
## scene's shape, animations and facing). A save keeps a kind's shape and
## animations with the fingerprint they were made from; once the game's
## own changes ("Make game default", a new build), the save's old copy is
## dropped and every world gets the new one.
var kind_defaults: Dictionary = {}


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
	# (What's done to one in the object editor stays its own until "Update
	# others": update_others().)
	for body in all():
		if not body.has_meta(META_SEEN):
			_meet(body)


## A creature seen for the first time: the first of its kind sets the
## kind's shape (from the save, if it has one) and its animations; the
## others take it on. (Another player's creature keeps the shape they sent.)
func _meet(p_body: DensityBody3D) -> void:
	var path := p_body.scene_file_path
	if not kind_shapes.has(path):
		var player := BodyAnimation.player_of(p_body)
		var library: AnimationLibrary = player.get_animation_library("") if player != null and player.has_animation_library("") else null
		kind_defaults[path] = _fingerprint(p_body.shape, library, p_body.get("facing_offset"), p_body.get("facing_pitch"))
		var saved: Dictionary = loaded_kinds.get(path, {})
		loaded_kinds.erase(path)
		if saved.get("default", "") != kind_defaults[path]:
			saved = {}  # (saved from an older game default, or a save from before they were told apart)
		if saved.get("shape") is Dictionary:
			kind_shapes[path] = DensityShape.from_dict(saved.shape)
		else:
			kind_shapes[path] = p_body.shape
		kind_facing[path] = float(saved.get("facing", p_body.get("facing_offset") if p_body.get("facing_offset") != null else 0.0))
		kind_pitch[path] = float(saved.get("facing_pitch", p_body.get("facing_pitch") if p_body.get("facing_pitch") != null else 0.0))
		if saved.get("animation") is Dictionary:
			BodyAnimation.apply(p_body, saved.animation)  # (into the library every one of its kind shares)
		if player != null and player.has_animation_library(""):
			kind_libraries[path] = player.get_animation_library("")
	if p_body.get_meta("net_sim", true) and p_body.shape != kind_shapes[path]:
		p_body.shape = kind_shapes[path]
	if p_body.get("facing_offset") != null:
		p_body.set("facing_offset", kind_facing.get(path, 0.0))
	if p_body.get("facing_pitch") != null:
		p_body.set("facing_pitch", kind_pitch.get(path, 0.0))
	p_body.set_meta(META_SEEN, p_body.shape)


## Another player changed a kind's shape (Update others there): every one
## of the kind simulated here that isn't unique takes it.
func take_kind_shape(p_path: String, p_shape: DensityShape) -> void:
	kind_shapes[p_path] = p_shape
	loaded_kinds.erase(p_path)
	for body in all():
		if body.scene_file_path == p_path and not is_unique(body) and body.get_meta("net_sim", true) and not body.has_meta("editing"):
			body.shape = p_shape
			body.set_meta(META_SEEN, p_shape)


## Another player changed a kind's animations (Update others there).
func take_kind_animations(p_path: String, p_animation: Dictionary) -> void:
	var library := library_from(p_animation)
	kind_libraries[p_path] = library
	for body in all():
		if body.scene_file_path == p_path and not is_unique(body) and not body.has_meta("editing"):
			set_library(body, library)


# --- Groups: Update others ------------------------------------------------------------

static func is_unique(p_body: Node) -> bool:
	return p_body.get_meta(META_UNIQUE, false)

static func unique_name_of(p_body: Node) -> String:
	return str(p_body.get_meta(META_UNIQUE_NAME, "")).strip_edges()

static func set_unique(p_body: Node, p_on: bool, p_name: String) -> void:
	if p_on:
		p_body.set_meta(META_UNIQUE, true)
	else:
		p_body.remove_meta(META_UNIQUE)
	if p_on and p_name.strip_edges() != "":
		p_body.set_meta(META_UNIQUE_NAME, p_name.strip_edges())
	else:
		p_body.remove_meta(META_UNIQUE_NAME)

## The others Update others on p_body gives it to: the rest of its kind
## that aren't unique, or (unique) those of its kind with its unique name
## (none, with no name). Only ones simulated here: another player's come
## as they send them.
func group_of(p_body: DensityBody3D) -> Array[DensityBody3D]:
	var list: Array[DensityBody3D] = []
	var unique := is_unique(p_body)
	var unique_name := unique_name_of(p_body)
	if p_body.scene_file_path == "" or (unique and unique_name == ""):
		return list
	for body in all():
		if body == p_body or body.scene_file_path != p_body.scene_file_path or not body.get_meta("net_sim", true):
			continue
		if is_unique(body) != unique or (unique and unique_name_of(body) != unique_name):
			continue
		list.append(body)
	return list

## The body's AnimationPlayer's library, or null.
static func library_of(p_body: Node) -> AnimationLibrary:
	var player := BodyAnimation.player_of(p_body)
	if player == null or not player.has_animation_library(""):
		return null
	return player.get_animation_library("")

## Gives the body's AnimationPlayer p_library in place of the one it has
## (what it plays by itself goes on, if p_library has it).
static func set_library(p_body: Node, p_library: AnimationLibrary) -> void:
	if p_library == null:
		return
	var player := BodyAnimation.player_of(p_body, true)
	if player.has_animation_library(""):
		if player.get_animation_library("") == p_library:
			return
		player.remove_animation_library("")
	player.add_animation_library("", p_library)
	var plays := BodyAnimation.get_plays(p_body)
	if plays != "" and not p_body.has_meta("editing"):
		BodyAnimation.set_plays(p_body, plays)

## A new library of copies of p_library's animations (changing them
## leaves p_library as it is).
static func copy_library(p_library: AnimationLibrary) -> AnimationLibrary:
	var copy := AnimationLibrary.new()
	if p_library != null:
		for anim_name in p_library.get_animation_list():
			copy.add_animation(anim_name, p_library.get_animation(anim_name).duplicate(true))
	return copy

## A library's animations as plain data ({} for null).
static func library_dict(p_library: AnimationLibrary) -> Dictionary:
	var animations := {}
	if p_library != null:
		for anim_name in p_library.get_animation_list():
			animations[str(anim_name)] = BodyAnimation.animation_to_dict(p_library.get_animation(anim_name))
	return animations

static func library_from(p_animation: Dictionary) -> AnimationLibrary:
	var library := AnimationLibrary.new()
	var animations: Dictionary = p_animation.get("animations", {})
	for anim_name in animations:
		library.add_animation(anim_name, BodyAnimation.animation_from_dict(animations[anim_name]))
	return library

## What Update others hands on: its shape, animations and facing.
func state_of(p_body: DensityBody3D) -> Dictionary:
	return {"shape": p_body.shape, "library": library_of(p_body),
			"facing": p_body.get("facing_offset"), "pitch": p_body.get("facing_pitch")}

func apply_state(p_body: DensityBody3D, p_state: Dictionary) -> void:
	if not is_instance_valid(p_body):
		return
	if p_state.shape != null and p_body.shape != p_state.shape:
		p_body.shape = p_state.shape
		p_body.sleeping = false
	p_body.set_meta(META_SEEN, p_body.shape)
	set_library(p_body, p_state.library)
	if p_state.facing != null and p_body.get("facing_offset") != null:
		p_body.set("facing_offset", p_state.facing)
	if p_state.pitch != null and p_body.get("facing_pitch") != null:
		p_body.set("facing_pitch", p_state.pitch)

func kind_state(p_path: String) -> Dictionary:
	return {"shape": kind_shapes.get(p_path), "library": kind_libraries.get(p_path),
			"facing": kind_facing.get(p_path, 0.0), "pitch": kind_pitch.get(p_path, 0.0)}

## Makes p_state the kind's, here and for the other players.
func set_kind_state(p_path: String, p_state: Dictionary) -> void:
	if p_state.shape != null:
		kind_shapes[p_path] = p_state.shape
		world.network.send_kind_shape(p_path, p_state.shape)
	if p_state.library != null:
		kind_libraries[p_path] = p_state.library
		world.network.send_kind_animations(p_path, {"animations": library_dict(p_state.library), "plays": ""})
		# (Sent as the kind's: not again as one creature's own.)
		world.network.library_hashes[p_state.library] = world.network._library_hash(p_state.library)
	kind_facing[p_path] = float(p_state.facing) if p_state.facing != null else 0.0
	kind_pitch[p_path] = float(p_state.pitch) if p_state.pitch != null else 0.0
	world.network.send_kind_facing(p_path, kind_facing[p_path], kind_pitch[p_path])
	loaded_kinds.erase(p_path)

## Update others: p_body's shape, animations (a copy shared by them all:
## p_body keeps its own, still being edited) and facing go to the rest of
## its group (group_of()), and, if it isn't unique, become its kind's.
## Returns the undo step for world.gd's history ({} if it has no group).
func update_others(p_body: DensityBody3D) -> Dictionary:
	var path := p_body.scene_file_path
	var unique := is_unique(p_body)
	if path == "" or (unique and unique_name_of(p_body) == ""):
		return {}
	var state := state_of(p_body)
	if state.library != null:
		state.library = copy_library(state.library)
	var entry := {"group_update": path, "members": [], "library": state.library}
	for body in group_of(p_body):
		entry.members.append({"body": body, "before": state_of(body), "after": state})
		apply_state(body, state)
	if not unique:
		entry["kind_before"] = kind_state(path)
		entry["kind_after"] = state
		set_kind_state(path, state)
	# (Its shape is shared now: its next edit makes it a copy of its own again.)
	p_body.shape = p_body.shape
	p_body.set_meta(META_SEEN, p_body.shape)
	return entry

## Undo (p_after false) or redo of an update_others() step.
func undo_update(p_entry: Dictionary, p_after: bool) -> void:
	var which := "after" if p_after else "before"
	for m in p_entry.members:
		apply_state(m.body, m[which])
	if p_entry.has("kind_" + which):
		set_kind_state(p_entry.group_update, p_entry["kind_" + which])


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
		entry.merge(own_entry(body))
		list.append(entry)
	list.append_array(loaded_creatures)  # (not made yet: kept)
	var kinds := loaded_kinds.duplicate()  # (kinds not met this time: kept as they were)
	for path in kind_shapes:
		var kind := {"shape": (kind_shapes[path] as DensityShape).to_dict(), "facing": kind_facing.get(path, 0.0),
				"facing_pitch": kind_pitch.get(path, 0.0), "default": kind_defaults.get(path, "")}
		var library: AnimationLibrary = kind_libraries.get(path)
		if library != null:
			var animations := {}
			for anim_name in library.get_animation_list():
				animations[str(anim_name)] = BodyAnimation.animation_to_dict(library.get_animation(anim_name))
			kind["animation"] = {"animations": animations, "plays": ""}
		kinds[path] = kind
	return {"list": list, "kinds": kinds}


func load_from(p_config: ConfigFile) -> void:
	loaded_kinds = {}
	var kinds = p_config.get_value("creatures", "kinds", {})
	if kinds is Dictionary:
		for path in kinds:
			loaded_kinds[MOVED_SCENES.get(path, path)] = kinds[path]
	loaded_creatures = p_config.get_value("creatures", "list", [])
	for entry in loaded_creatures:
		if entry is Dictionary and MOVED_SCENES.has(entry.get("scene", "")):
			entry.scene = MOVED_SCENES[entry.scene]


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
		var body := spawn(scene, p_player + world.objects._offset_from(p_player, at))
		body.global_basis = Basis(entry.get("rotation", Quaternion.IDENTITY))
		body.linear_velocity = entry.get("velocity", Vector3.ZERO)
		body.angular_velocity = entry.get("spin", Vector3.ZERO)
		body.freeze = entry.get("frozen", false)
		body.pose = entry.get("pose", 0.0)
		body.key_weights = entry.get("key_weights", PackedFloat32Array())
		restore_own(body, entry)


## What a creature has of its own (saved with it, or copied with Ctrl+C):
## its shape, animations and facing where they aren't its kind's, and
## whether it's unique.
func own_entry(p_body: DensityBody3D) -> Dictionary:
	var entry := {}
	var path := p_body.scene_file_path
	if p_body.shape != kind_shapes.get(path):
		entry["shape"] = p_body.shape.to_dict()  # (sculpted: its own)
	if is_unique(p_body):
		entry["unique"] = true
		entry["unique_name"] = unique_name_of(p_body)
	if p_body.get("facing_offset") != null and not is_equal_approx(p_body.facing_offset, kind_facing.get(path, 0.0)):
		entry["facing"] = p_body.facing_offset
	if p_body.get("facing_pitch") != null and not is_equal_approx(p_body.facing_pitch, kind_pitch.get(path, 0.0)):
		entry["facing_pitch"] = p_body.facing_pitch
	var library := library_of(p_body)
	if library != null and library != kind_libraries.get(path) and library_dict(library) != library_dict(kind_libraries.get(path)):
		entry["animation"] = BodyAnimation.to_dict(p_body)
	return entry

## Puts back what own_entry() kept (the shape too).
func restore_own(p_body: DensityBody3D, p_entry: Dictionary) -> void:
	if p_entry.get("shape") is Dictionary:
		p_body.shape = DensityShape.from_dict(p_entry.shape)
		p_body.set_meta(META_SEEN, p_body.shape)
	set_unique(p_body, p_entry.get("unique", false), p_entry.get("unique_name", ""))
	if p_entry.has("facing") and p_body.get("facing_offset") != null:
		p_body.set("facing_offset", float(p_entry.facing))
	if p_entry.has("facing_pitch") and p_body.get("facing_pitch") != null:
		p_body.set("facing_pitch", float(p_entry.facing_pitch))
	if p_entry.get("animation") is Dictionary and not (p_entry.animation as Dictionary).is_empty():
		set_library(p_body, library_from(p_entry.animation))
		var plays: String = p_entry.animation.get("plays", "")
		if plays != "":
			BodyAnimation.set_plays(p_body, plays)


# --- The game's own ------------------------------------------------------------------

## Makes p_body's kind (its scene) start out as it is now, for every world:
## its shape (and shape keys) into the scene's shape file, its animations
## into their files (the folder the scene's animation files are in, where
## it keeps them in files), or into the scene itself. Returns what it did, or
## what went wrong. (Only while running from the project: an exported
## game's files can't be written.)
## Every one of a kind faces p_degrees off and p_pitch up (creature.gd,
## facing_offset and facing_pitch): here, and for the other players unless
## p_from_network.
func set_kind_facing(p_path: String, p_degrees: float, p_pitch: float, p_from_network := false) -> void:
	kind_facing[p_path] = p_degrees
	kind_pitch[p_path] = p_pitch
	for body in all():
		if body.scene_file_path == p_path and body.get("facing_offset") != null and not is_unique(body) and not body.has_meta("editing"):
			body.set("facing_offset", p_degrees)
			body.set("facing_pitch", p_pitch)
	if not p_from_network:
		world.network.send_kind_facing(p_path, p_degrees, p_pitch)


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
	var shape: DensityShape = p_body.shape.duplicate_shape()  # (this one's, as the editor has it)
	if ResourceSaver.save(shape, shape_path) == OK:
		shape.take_over_path(shape_path)
		root.shape = shape
	else:
		failed.append(shape_path)
	# The animations: each into its file, where the scene keeps them in files.
	var library: AnimationLibrary = library_of(p_body)
	var player := BodyAnimation.player_of(root)
	var files := 0
	if library != null and player != null:
		# (New animations go beside the ones already in files, if any are.)
		var folder := ""
		for anim_name in library.get_animation_list():
			var file := library.get_animation(anim_name).resource_path
			if file != "" and not file.contains("::"):
				folder = file.get_base_dir()
				break
		var in_files := folder != ""
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
		root.set("facing_offset", p_body.get("facing_offset"))
	if root.get("facing_pitch") != null:
		root.set("facing_pitch", p_body.get("facing_pitch"))
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
	# (This world's copy is the game's own now: saved as made from it.)
	# (Unique, this world's kind isn't what was saved: it isn't marked so.)
	if not is_unique(p_body):
		kind_defaults[path] = _fingerprint(shape, library, p_body.get("facing_offset"), p_body.get("facing_pitch"))
	return "Saved as the game's own %s: shape (%d keys) in %s, %d animations%s" % [path.get_file().get_basename(),
			shape.get_key_count(), shape_path.get_file(), library.get_animation_list().size() if library != null else 0,
			" (%d in files)" % files if files > 0 else " (in the scene)"]


## A kind as the game makes it, told apart from any other: a hash of its
## shape, its animations and its facing.
static func _fingerprint(p_shape: DensityShape, p_library: AnimationLibrary, p_facing, p_pitch = 0.0) -> String:
	var hashing := HashingContext.new()
	hashing.start(HashingContext.HASH_MD5)
	if p_shape != null:
		hashing.update(var_to_bytes(p_shape.to_dict()))
	if p_library != null:
		var names := p_library.get_animation_list()
		names.sort()
		for anim_name in names:
			hashing.update(str(anim_name).to_utf8_buffer())
			hashing.update(var_to_bytes(BodyAnimation.animation_to_dict(p_library.get_animation(anim_name))))
	hashing.update(var_to_bytes(snappedf(float(p_facing) if p_facing != null else 0.0, 0.01)))
	var pitch := snappedf(float(p_pitch) if p_pitch != null else 0.0, 0.01)
	if pitch != 0.0:
		hashing.update(var_to_bytes(pitch))  # (only if tilted: older saves' fingerprints still match)
	return hashing.finish().hex_encode()
