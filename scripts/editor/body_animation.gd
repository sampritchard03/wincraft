extends RefCounted
## A body's own animations (in its AnimationPlayer, as the object editor
## makes them) and the one it plays by itself, as plain data: for saving
## the world's objects (physics_objects.gd) and sending them to other players
## (network.gd).
##
## The animation a body plays by itself is its "plays" (meta "plays_animation"):
## set with set_plays(), it starts whenever the body is made again (loaded,
## streamed back in, joined). A body with an AnimationTree (the frog) has
## the tree switched off while it plays one of its own, back on with "".

const META := "plays_animation"

## The body's AnimationPlayer (made if p_make and it has none).
static func player_of(p_body: Node, p_make := false) -> AnimationPlayer:
	for child in p_body.get_children():
		if child is AnimationPlayer:
			return child
	if not p_make:
		return null
	var player := AnimationPlayer.new()
	player.name = "AnimationPlayer"
	p_body.add_child(player)
	return player

static func tree_of(p_body: Node) -> AnimationTree:
	for child in p_body.get_children():
		if child is AnimationTree:
			return child
	return null

## The animation the body plays by itself ("" none).
static func get_plays(p_body: Node) -> String:
	return p_body.get_meta(META, "")

## Makes the body play p_name by itself, looping as the animation says
## ("" stops it: an AnimationTree, if it has one, takes over again).
static func set_plays(p_body: Node, p_name: String) -> void:
	var player := player_of(p_body)
	if p_name == "" or player == null or not player.has_animation(p_name):
		p_body.remove_meta(META)
		if player != null:
			player.stop()
		var tree := tree_of(p_body)
		if tree != null:
			tree.active = true
		return
	p_body.set_meta(META, p_name)
	var tree := tree_of(p_body)
	if tree != null:
		tree.active = false
	player.play(p_name)

## The body's animations and what it plays ({} if it has none of its own).
static func to_dict(p_body: Node) -> Dictionary:
	var player := player_of(p_body)
	if player == null or not player.has_animation_library(""):
		return {}
	var library := player.get_animation_library("")
	var animations := {}
	for anim_name in library.get_animation_list():
		animations[str(anim_name)] = animation_to_dict(library.get_animation(anim_name))
	if animations.is_empty():
		return {}
	return {"animations": animations, "plays": get_plays(p_body)}

## Puts saved animations (to_dict()) on the body, replacing ones of the
## same names, and starts what it plays.
static func apply(p_body: Node, p_dict: Dictionary) -> void:
	var animations: Dictionary = p_dict.get("animations", {})
	if animations.is_empty():
		return
	var player := player_of(p_body, true)
	if not player.has_animation_library(""):
		player.add_animation_library("", AnimationLibrary.new())
	var library := player.get_animation_library("")
	for anim_name in animations:
		if library.has_animation(anim_name):
			library.remove_animation(anim_name)
		library.add_animation(anim_name, animation_from_dict(animations[anim_name]))
	set_plays(p_body, p_dict.get("plays", ""))

## An animation's value tracks (what the object editor keys) as plain data.
static func animation_to_dict(p_anim: Animation) -> Dictionary:
	var tracks := []
	for t in p_anim.get_track_count():
		if p_anim.track_get_type(t) != Animation.TYPE_VALUE:
			continue
		var times := PackedFloat32Array()
		var values := []
		var transitions := PackedFloat32Array()
		for k in p_anim.track_get_key_count(t):
			times.append(p_anim.track_get_key_time(t, k))
			values.append(p_anim.track_get_key_value(t, k))
			transitions.append(p_anim.track_get_key_transition(t, k))
		tracks.append({"path": str(p_anim.track_get_path(t)), "interpolation": p_anim.track_get_interpolation_type(t),
				"update": p_anim.value_track_get_update_mode(t), "times": times, "values": values, "transitions": transitions})
	return {"length": p_anim.length, "loop": p_anim.loop_mode, "tracks": tracks}

static func animation_from_dict(p_dict: Dictionary) -> Animation:
	var anim := Animation.new()
	anim.length = p_dict.get("length", 1.0)
	anim.loop_mode = p_dict.get("loop", Animation.LOOP_NONE)
	for track in p_dict.get("tracks", []):
		var t := anim.add_track(Animation.TYPE_VALUE)
		anim.track_set_path(t, NodePath(track.get("path", "")))
		anim.track_set_interpolation_type(t, track.get("interpolation", Animation.INTERPOLATION_LINEAR))
		anim.value_track_set_update_mode(t, track.get("update", Animation.UPDATE_CONTINUOUS))
		var times: PackedFloat32Array = track.get("times", PackedFloat32Array())
		var values: Array = track.get("values", [])
		var transitions: PackedFloat32Array = track.get("transitions", PackedFloat32Array())
		for k in mini(times.size(), values.size()):
			anim.track_insert_key(t, times[k], values[k], transitions[k] if k < transitions.size() else 1.0)
	return anim

## After keys were moved or removed: each animation's key_weight/N tracks
## follow their keys (p_map[old index] = new index, -1: removed).
static func remap_weight_tracks(p_body: Node, p_map: PackedInt32Array) -> void:
	var player := player_of(p_body)
	if player == null or not player.has_animation_library(""):
		return
	var library := player.get_animation_library("")
	for anim_name in library.get_animation_list():
		var anim := library.get_animation(anim_name)
		var removed := []
		for t in anim.get_track_count():
			var path := str(anim.track_get_path(t))
			var at := path.find(":key_weight/")
			if at < 0:
				continue
			var old := path.substr(at + 12).to_int()
			if old >= p_map.size():
				continue
			if p_map[old] < 0:
				removed.append(t)
			else:
				anim.track_set_path(t, NodePath(path.substr(0, at) + ":key_weight/%d" % p_map[old]))
		removed.reverse()
		for t in removed:
			anim.remove_track(t)

## After a bone was renamed (p_new: its new name) or removed (p_new ""):
## each animation's bones/<name>/... tracks follow it, or go.
static func rename_bone_tracks(p_body: Node, p_old: String, p_new: String) -> void:
	var player := player_of(p_body)
	if player == null or not player.has_animation_library(""):
		return
	var library := player.get_animation_library("")
	var prefix := ":bones/%s/" % p_old
	for anim_name in library.get_animation_list():
		var anim := library.get_animation(anim_name)
		var removed := []
		for t in anim.get_track_count():
			var path := str(anim.track_get_path(t))
			var at := path.find(prefix)
			if at < 0:
				continue
			if p_new == "":
				removed.append(t)
			else:
				anim.track_set_path(t, NodePath(path.substr(0, at) + ":bones/%s/" % p_new + path.substr(at + prefix.length())))
		removed.reverse()
		for t in removed:
			anim.remove_track(t)
