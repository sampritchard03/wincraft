extends Node
## The object editor: press M looking at a ball, frog or other body to edit
## it (M again to leave). The body holds still and a panel and timeline
## appear. The usual tools sculpt and paint it (the panel's "Sculpt" picks
## whether brushes change the shape itself or one key).
##
## Shape keys: a body's shape can hold several keys (like a mesh's shape
## keys), each with a name and a weight slider: a key at 1 shows all of it,
## and several at once add up (a mouth key and a leg key both at 1: mouth
## open and leg out). Key 0 is the plain shape the others are added to.
## Clicking a key's name makes it the current key (the key buttons act on
## it, and "Sculpt: Current key" sculpts it). "Add key" makes a new key of
## how the body looks now and makes it current. Grab or snake hook on one key
## moves its parts (weighing it in then moves them there, keeping their
## shape), other brushes add to that key only. Sculpting "All keys" changes
## the shape itself where the brush is on it as shown, so what you add
## follows the part it's on in every key.
##
## Animation: each weight is keyed on a track of its own (".:key_weight/2"
## ...), with Auto key as you move its slider; I keys every weight in use at
## the current frame, U removes the keys there, Y plays / pauses, , and .
## step a frame (Shift: to the previous / next key). Tab frees the mouse for
## the panels (settings, animations, export). "Plays by itself" picks the
## animation the body plays on its own when not being edited (a ball's
## "ball1", say): it's saved with the world's objects.
##
## The animations live on the body itself, in its AnimationPlayer (made if
## it has none), so the frog's AnimationTree, say, can play what you make here.
##
## Shape keys and animation are for entities only (entity.gd: balls, frogs,
## chiggins): a plain physics object (P, Ctrl+O, a piece cut off) is loose
## ground, one shape, so for one the editor shows only its sculpting and
## physics settings.

const BodyAnimation := preload("res://scripts/editor/body_animation.gd")
const PhysicsObject := preload("res://scripts/objects/physics_object.gd")
const Entity := preload("res://scripts/objects/entities/entity.gd")

var world: Node  # world.gd
var active := false
var body: DensityBody3D = null
var make_default_button: Button
var notice := ""  # said in the status line for a while
var notice_until := 0

# What the body was like before editing, put back on leaving.
var was_frozen := false
var anim_tree: AnimationTree = null
var tree_was_active := false
var physics_start := Transform3D()  # where "Physics" was turned on

# Animation.
var player: AnimationPlayer = null
var anim_name := ""
var time := 0.0
var playing := false
var auto_key := true
var fps := 12.0

# Drawing the centre of mass.
var lines: MeshInstance3D
var line_mesh: ImmediateMesh

# Panels.
var ui: Control
var side_panel: PanelContainer
var timeline_panel: PanelContainer
var title_label: Label
var help_label: Label
var status_label: Label
var weights_box: VBoxContainer
var weight_sliders: Array[HSlider] = []
var key_buttons: Array[Button] = []
var edit_key_select: OptionButton
var key_name_edit: LineEdit
var plays_select: OptionButton
var current_key := 0  # the key the key buttons act on
var sculpt_target := -1  # what brushes edit: -1 all keys, -2 the current key, else that key
var kept_targets := {}  # body instance id -> [current key, sculpt target] when it was last closed
var physics_check: CheckBox
var voxel_select: OptionButton
## The voxel sizes offered (finer: more detail, but a smaller object fits:
## MAX_SIZE points a side, 64).
const VOXEL_SIZES := [2.0, 1.0, 0.5, 0.25, 0.125]
var auto_com_check: CheckBox
var autokey_check: CheckBox
var anim_select: OptionButton
var anim_name_edit: LineEdit
var length_spin: SpinBox
var fps_spin: SpinBox
var loop_check: CheckBox
var play_button: Button
var timeline: Timeline
var file_dialog: FileDialog
var facing_row: HBoxContainer
var facing_spin: SpinBox
var updating_ui := false  # filling the panels in: their signals are ignored
var entity_controls: Array[Control] = []  # shown only for an entity (shape keys, animation)
var plain_label: Label  # shown instead, for a plain physics object
var seen_shape: DensityShape = null
var seen_keys := -1


func _ready() -> void:
	line_mesh = ImmediateMesh.new()
	lines = MeshInstance3D.new()
	lines.mesh = line_mesh
	lines.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.vertex_color_use_as_albedo = true
	material.no_depth_test = true  # shows through the body
	material.render_priority = 2
	lines.material_override = material
	world.brush_preview.get_parent().add_child.call_deferred(lines)
	_make_panels()


# --- Hooks for world.gd -----------------------------------------------------

## Handles an input event first; true if it was the editor's.
func handle_input(event: InputEvent) -> bool:
	if event is InputEventKey and event.pressed and not event.echo:
		if get_viewport().gui_get_focus_owner() is LineEdit:
			return false
		var key: Key = world._key_of(event)
		if key == KEY_M and not event.ctrl_pressed:
			if active:
				close()
			else:
				open_aimed()
			return true
		if not active or event.ctrl_pressed or event.alt_pressed:
			return false  # (Ctrl+Z, Ctrl+Y... are the world's)
		match key:
			KEY_TAB:
				Input.mouse_mode = Input.MOUSE_MODE_VISIBLE if Input.mouse_mode == Input.MOUSE_MODE_CAPTURED else Input.MOUSE_MODE_CAPTURED
				get_viewport().gui_release_focus()
			KEY_I, KEY_U, KEY_Y, KEY_COMMA, KEY_PERIOD:
				if not _is_entity():
					return false  # (no animation for a plain object)
				match key:
					KEY_I: _insert_keys()
					KEY_U: _delete_keys()
					KEY_Y: _toggle_play()
					KEY_COMMA: _step(-1, event.shift_pressed)
					KEY_PERIOD: _step(1, event.shift_pressed)
			_:
				return false
		return true
	return false

## Whether a screen point is on one of the panels.
func has_point(p_position: Vector2) -> bool:
	return active and (side_panel.get_global_rect().has_point(p_position)
			or (timeline_panel.visible and timeline_panel.get_global_rect().has_point(p_position)))


## Whether the body being edited may have shape keys and animations.
func _is_entity() -> bool:
	return is_instance_valid(body) and PhysicsObject.body_is_entity(body)


# --- Opening and closing ----------------------------------------------------

func open_aimed() -> void:
	var aimed := DensityBody3D.raycast_bodies(world.camera.global_position, -world.camera.global_basis.z, world.reach)
	if aimed.is_empty() or aimed.body.shape == null:
		return
	open(aimed.body)

func open(p_body: DensityBody3D) -> void:
	if active:
		close()
	world.network.use(p_body)  # (multiplayer: edited here, so simulated here)
	body = p_body
	active = true
	body.set_meta("editing", true)  # (a creature's own script holds off meanwhile)
	was_frozen = body.freeze
	body.freeze = true
	body.linear_velocity = Vector3.ZERO
	body.angular_velocity = Vector3.ZERO
	# Shared again, so the first edit makes it a copy of its own (frogs share
	# one shape; creatures.gd then gives that to every one of the kind).
	body.shape = body.shape
	anim_tree = BodyAnimation.tree_of(body)
	if anim_tree != null:
		tree_was_active = anim_tree.active
		anim_tree.active = false
	player = BodyAnimation.player_of(body, true)
	player.stop()  # (what it plays by itself: on again when the editor closes)
	if not player.has_animation_library(""):
		player.add_animation_library("", AnimationLibrary.new())
	var names := player.get_animation_list()
	var plays: String = BodyAnimation.get_plays(body)
	anim_name = plays if plays != "" else (names[0] if not names.is_empty() else "")
	if not _has_pose_tracks():
		body.pose = 0.0  # (keys are weighed in on top of key 0)
	# The keys it was left on, if it's been open before.
	var kept: Array = kept_targets.get(body.get_instance_id(), [current_key, -1])
	var key_count := body.shape.get_key_count()
	current_key = clampi(kept[0], 0, maxi(key_count - 1, 0))
	sculpt_target = kept[1] if kept[1] < key_count else -1
	body.edit_key = -1
	time = 0.0
	playing = false
	physics_check.set_pressed_no_signal(false)
	_show_time()
	ui.visible = true
	_refresh_ui()

func close() -> void:
	if not active:
		return
	active = false
	ui.visible = false
	line_mesh.clear_surfaces()
	if is_instance_valid(body):
		kept_targets[body.get_instance_id()] = [current_key, sculpt_target]
		body.remove_meta("editing")
		body.edit_key = -1
		body.freeze = was_frozen
		body.sleeping = false
		if is_instance_valid(player):
			player.stop()
		if anim_tree != null and is_instance_valid(anim_tree):
			anim_tree.active = tree_was_active
		# Back to what it plays by itself (if anything).
		var plays: String = BodyAnimation.get_plays(body)
		if plays != "":
			BodyAnimation.set_plays(body, plays)
	body = null
	Input.mouse_mode = Input.MOUSE_MODE_CAPTURED


# --- Every frame --------------------------------------------------------------

func _process(_delta: float) -> void:
	if not active:
		return
	if not is_instance_valid(body):
		close()
		return
	world.network.use(body)  # (multiplayer: stays with us while edited)
	if playing and is_instance_valid(player):
		if player.is_playing():
			time = player.current_animation_position
		else:
			playing = false
			play_button.text = "Play (Y)"
	# Undo / redo may have swapped the shape (and its keys) under us.
	if body.shape != seen_shape or body.shape.get_key_count() != seen_keys:
		seen_shape = body.shape
		seen_keys = body.shape.get_key_count()
		_refresh_ui()
	if playing:
		_show_weights()
	_draw_center_of_mass()
	_update_status()
	timeline.queue_redraw()


# --- Aiming ---------------------------------------------------------------------

## The middle of the body where the crosshair goes through it, or null if
## it misses.
func _aimed_inside():
	var from: Vector3 = world.camera.global_position
	var dir: Vector3 = -world.camera.global_basis.z
	var hit = body.raycast(from, dir, world.reach)
	if hit == null:
		return null
	var t_in := from.distance_to(hit)
	var t_out := t_in
	var step := maxf(body.shape.voxel_size * body.global_basis.x.length() * 0.5, 0.1)
	for i in 400:
		t_out += step
		if body.get_density_at(from + dir * t_out) <= 0.0:
			break
	return from + dir * ((t_in + t_out) * 0.5)


# --- Animation ------------------------------------------------------------------

func _library() -> AnimationLibrary:
	return player.get_animation_library("")

func _anim() -> Animation:
	if anim_name == "" or not player.has_animation(anim_name):
		return null
	return player.get_animation(anim_name)

func _new_animation(p_name := "anim") -> void:
	var anim := Animation.new()
	anim.length = 1.0
	anim.loop_mode = Animation.LOOP_LINEAR
	var unique := p_name
	var n := 2
	while player.has_animation(unique):
		unique = "%s%d" % [p_name, n]
		n += 1
	_library().add_animation(unique, anim)
	anim_name = unique
	time = 0.0
	_show_time()
	_refresh_ui()

## Keys the body's key weights that are in use (not 0, or already on a
## track) at the current time.
func _insert_keys() -> void:
	if not _is_entity() or body.shape.get_key_count() < 2:
		return
	if _anim() == null:
		_new_animation()
	for k in body.shape.get_key_count():
		var path := _weight_path(k)
		if body.get_key_weight(k) != 0.0 or _anim().find_track(path, Animation.TYPE_VALUE) >= 0:
			_set_key(_anim(), path, Animation.TYPE_VALUE, body.get_key_weight(k))
	timeline.queue_redraw()

## Whether any of the body's animations moves its `pose` (the frog's do:
## left alone then).
func _has_pose_tracks() -> bool:
	for anim_key in player.get_animation_list():
		if player.get_animation(anim_key).find_track(NodePath(".:pose"), Animation.TYPE_VALUE) >= 0:
			return true
	return false

func _weight_path(p_key: int) -> NodePath:
	return NodePath(".:key_weight/%d" % p_key)

## Keys one key weight at the current time.
func _insert_weight_key(p_key: int) -> void:
	if not is_instance_valid(body):
		return
	if _anim() == null:
		_new_animation()
	_set_key(_anim(), _weight_path(p_key), Animation.TYPE_VALUE, body.get_key_weight(p_key))
	timeline.queue_redraw()

## The weight sliders as the body has them.
func _show_weights() -> void:
	if not is_instance_valid(body):
		return
	updating_ui = true
	for k in weight_sliders.size():
		weight_sliders[k].value = body.get_key_weight(k)
	updating_ui = false

func _set_key(p_anim: Animation, p_path: NodePath, p_type: Animation.TrackType, p_value) -> void:
	var track := p_anim.find_track(p_path, p_type)
	if track < 0:
		track = p_anim.add_track(p_type)
		p_anim.track_set_path(track, p_path)
	var old := p_anim.track_find_key(track, time, Animation.FIND_MODE_APPROX)
	if old >= 0:
		p_anim.track_remove_key(track, old)
	p_anim.track_insert_key(track, time, p_value)

func _delete_keys() -> void:
	var anim := _anim()
	if anim == null:
		return
	for t in anim.get_track_count():
		var k := anim.track_find_key(t, time, Animation.FIND_MODE_APPROX)
		if k >= 0:
			anim.track_remove_key(t, k)
	_show_time()

func _key_times() -> PackedFloat32Array:
	var times := PackedFloat32Array()
	var anim := _anim()
	if anim == null:
		return times
	for t in anim.get_track_count():
		for k in anim.track_get_key_count(t):
			var at := anim.track_get_key_time(t, k)
			var seen := false
			for s in times:
				if absf(s - at) < 0.001:
					seen = true
					break
			if not seen:
				times.append(at)
	times.sort()
	return times

func _toggle_play() -> void:
	if _anim() == null:
		return
	playing = not playing
	if playing:
		player.play(anim_name)
		player.seek(time, true)
	else:
		player.pause()
	play_button.text = "Pause (Y)" if playing else "Play (Y)"

func _step(p_direction: int, p_to_key: bool) -> void:
	var anim := _anim()
	if anim == null:
		return
	if playing:
		_toggle_play()
	if p_to_key:
		var times := _key_times()
		var next := time
		if p_direction > 0:
			for s in times:
				if s > time + 0.001:
					next = s
					break
		else:
			for i in range(times.size() - 1, -1, -1):
				if times[i] < time - 0.001:
					next = times[i]
					break
		time = next
	else:
		time = clampf((roundf(time * fps) + p_direction) / fps, 0.0, anim.length)
	_show_time()

func scrub(p_time: float) -> void:
	var anim := _anim()
	if anim == null:
		return
	if playing:
		_toggle_play()
	time = clampf(roundf(p_time * fps) / fps, 0.0, anim.length)
	_show_time()

## Poses the body as the animation has it now.
func _show_time() -> void:
	if not is_instance_valid(player) or _anim() == null:
		return
	if player.current_animation != anim_name:
		player.play(anim_name)
	player.seek(time, true)
	if not playing:
		player.pause()
	_show_weights()


# --- Drawing --------------------------------------------------------------------

## The centre of mass: a magenta star (dimmer while it's automatic).
func _draw_center_of_mass() -> void:
	line_mesh.clear_surfaces()
	line_mesh.surface_begin(Mesh.PRIMITIVE_LINES)
	var com := body.get_global_center_of_mass()
	var color := Color(1.0, 0.3, 0.9) if not body.auto_center_of_mass else Color(0.8, 0.45, 0.75)
	var size := maxf(world.camera.global_position.distance_to(com) * 0.012, 0.6) * 1.4
	for axis in [Vector3(1, 1, 0), Vector3(1, -1, 0), Vector3(0, 1, 1), Vector3(0, 1, -1), Vector3(1, 0, 1), Vector3(1, 0, -1)]:
		var a: Vector3 = com - axis.normalized() * size
		var b: Vector3 = com + axis.normalized() * size
		line_mesh.surface_set_color(color)
		line_mesh.surface_add_vertex(a)
		line_mesh.surface_set_color(color)
		line_mesh.surface_add_vertex(b)
	# Which way it faces (a frog or chiggin): an arrow from the centre of mass.
	if body.has_method("_forward"):
		var forward: Vector3 = body._forward().normalized()
		var length := size * 6.0
		var tip := com + forward * length
		var side := forward.cross(body.global_basis.y.normalized()).normalized() * size
		for to in [com, tip - forward * size * 1.5 + side, tip - forward * size * 1.5 - side]:
			line_mesh.surface_set_color(Color(1.0, 0.85, 0.1))
			line_mesh.surface_add_vertex(tip if to != com else to)
			line_mesh.surface_set_color(Color(1.0, 0.85, 0.1))
			line_mesh.surface_add_vertex(to if to != com else tip)
	line_mesh.surface_end()


# --- Panels -----------------------------------------------------------------------

func _make_panels() -> void:
	ui = Control.new()
	ui.set_anchors_preset(Control.PRESET_FULL_RECT)
	ui.mouse_filter = Control.MOUSE_FILTER_IGNORE
	ui.visible = false
	world.hud.add_child.call_deferred(ui)

	# Settings: on the right.
	side_panel = PanelContainer.new()
	side_panel.anchor_left = 1.0
	side_panel.anchor_right = 1.0
	side_panel.offset_left = -296
	side_panel.offset_right = -8
	side_panel.offset_top = 72
	side_panel.grow_horizontal = Control.GROW_DIRECTION_BEGIN
	# Down to just above the timeline (it'd go under it, unclickable, with
	# many keys), scrolling when there's more.
	side_panel.anchor_bottom = 1.0
	side_panel.offset_bottom = -166
	ui.add_child(side_panel)
	var scroll := ScrollContainer.new()
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	side_panel.add_child(scroll)
	var box := VBoxContainer.new()
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(box)
	title_label = Label.new()
	box.add_child(title_label)
	plain_label = _label("A plain physics object: one shape, no shape keys or animation (those are for entities: balls, frogs, chiggins)")
	plain_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	box.add_child(plain_label)
	var keys_label := _label("Shape keys (click a name: current key; weights add up)")
	box.add_child(keys_label)
	weights_box = VBoxContainer.new()
	box.add_child(weights_box)
	var key_row := HBoxContainer.new()
	box.add_child(key_row)
	_button(key_row, "Add key", _add_key)
	_button(key_row, "<", func() -> void: _move_key(-1))
	_button(key_row, ">", func() -> void: _move_key(1))
	_button(key_row, "Reset", _reset_key)
	_button(key_row, "Remove", _remove_key)
	key_name_edit = LineEdit.new()
	key_name_edit.placeholder_text = "Name the current key (Enter)"
	key_name_edit.text_submitted.connect(func(t: String) -> void:
		_rename_key(t)
		key_name_edit.text = ""
		key_name_edit.release_focus())
	box.add_child(key_name_edit)
	var sculpt_label := _label("Sculpt")
	box.add_child(sculpt_label)
	edit_key_select = OptionButton.new()
	edit_key_select.focus_mode = Control.FOCUS_NONE
	edit_key_select.item_selected.connect(func(i: int) -> void:
		if not updating_ui and is_instance_valid(body):
			sculpt_target = edit_key_select.get_item_id(i)
			_apply_sculpt_target())
	box.add_child(edit_key_select)
	physics_check = _check(box, "Physics on (test it; off puts it back)", _set_physics)
	auto_com_check = _check(box, "Auto centre of mass", func(on: bool) -> void:
		if is_instance_valid(body):
			body.auto_center_of_mass = on)
	_button(box, "Centre of mass at the crosshair", _set_com_here)
	facing_row = HBoxContainer.new()
	box.add_child(facing_row)
	facing_row.add_child(_label("Faces (degrees, the yellow arrow)"))
	facing_spin = _spin(facing_row, -180.0, 180.0, 1.0, _set_facing)
	facing_spin.tooltip_text = "Turns which way a frog or chiggin thinks it's facing (every one of its kind): for a model made facing another way"
	for turn in [-90, 90]:
		_button(facing_row, "%+d" % turn, func() -> void:
			if is_instance_valid(body) and body.get("facing_offset") != null:
				facing_spin.value = wrapf(body.facing_offset + turn, -180.0, 180.0))
	autokey_check = _check(box, "Auto key the weight sliders", func(on: bool) -> void: auto_key = on)
	autokey_check.set_pressed_no_signal(true)
	# (Here, not along the bottom: that row is full.)
	var plays_label := _label("Plays by itself (when not editing)")
	box.add_child(plays_label)
	plays_select = OptionButton.new()
	plays_select.focus_mode = Control.FOCUS_NONE
	plays_select.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	plays_select.clip_text = true
	plays_select.item_selected.connect(func(i: int) -> void:
		if updating_ui or not is_instance_valid(body):
			return
		# Kept as the body's own: it starts when the editor closes (and
		# whenever the body is loaded again).
		var chosen := "" if i == 0 else plays_select.get_item_text(i)
		if chosen == "":
			body.remove_meta(BodyAnimation.META)
		else:
			body.set_meta(BodyAnimation.META, chosen))
	box.add_child(plays_select)
	entity_controls = [keys_label, weights_box, key_row, key_name_edit, sculpt_label, edit_key_select,
			autokey_check, plays_label, plays_select]
	var voxel_row := HBoxContainer.new()
	box.add_child(voxel_row)
	var voxel_label := Label.new()
	voxel_label.text = "Voxel size"
	voxel_row.add_child(voxel_label)
	voxel_select = OptionButton.new()
	voxel_select.focus_mode = Control.FOCUS_NONE
	voxel_select.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	voxel_select.tooltip_text = "How fine its voxels are (the brush is measured in them). Finer: more detail, but at most 64 a side, so a big object stays coarser"
	voxel_select.item_selected.connect(func(i: int) -> void:
		if not updating_ui:
			set_voxel_size(voxel_select.get_item_metadata(i)))
	voxel_row.add_child(voxel_select)
	var files := HBoxContainer.new()
	box.add_child(files)
	_button(files, "Import model", _import_model).tooltip_text = "Replaces this one's model with a saved shape (.res); on a frog or chiggin, every one of its kind. Shape keys it hasn't got are kept, empty, so the animations still find them: sculpt them again"
	_button(files, "Export model", _export_model)
	_button(files, "Export scene", _export_scene)
	make_default_button = _button(files, "Make game default", _make_default)
	make_default_button.tooltip_text = "A frog or chiggin: every one, in every world, starts out with this shape, these shape keys and these animations (writes the game's files)"

	# Animation: along the bottom.
	timeline_panel = PanelContainer.new()
	timeline_panel.anchor_top = 1.0
	timeline_panel.anchor_bottom = 1.0
	timeline_panel.anchor_right = 1.0
	timeline_panel.grow_vertical = Control.GROW_DIRECTION_BEGIN
	timeline_panel.offset_top = -150
	timeline_panel.offset_left = 8
	timeline_panel.offset_right = -8
	timeline_panel.offset_bottom = -8
	ui.add_child(timeline_panel)
	var rows := VBoxContainer.new()
	timeline_panel.add_child(rows)
	var top := HBoxContainer.new()
	rows.add_child(top)
	anim_select = OptionButton.new()
	anim_select.custom_minimum_size.x = 140
	anim_select.item_selected.connect(func(i: int) -> void:
		if updating_ui:
			return
		anim_name = anim_select.get_item_text(i)
		time = 0.0
		_show_time()
		_refresh_ui())
	top.add_child(anim_select)
	_button(top, "New", func() -> void: _new_animation())
	anim_name_edit = LineEdit.new()
	anim_name_edit.placeholder_text = "Rename (Enter)"
	anim_name_edit.custom_minimum_size.x = 120
	anim_name_edit.text_submitted.connect(func(t: String) -> void:
		var wanted := t.strip_edges()
		if _anim() != null and wanted != "" and not player.has_animation(wanted):
			_library().rename_animation(anim_name, wanted)
			if BodyAnimation.get_plays(body) == anim_name:
				body.set_meta(BodyAnimation.META, wanted)
			anim_name = wanted
		anim_name_edit.text = ""
		anim_name_edit.release_focus()
		_refresh_ui())
	top.add_child(anim_name_edit)
	_button(top, "Delete", func() -> void:
		if _anim() != null:
			if playing:
				_toggle_play()
			player.stop()
			if BodyAnimation.get_plays(body) == anim_name:
				body.remove_meta(BodyAnimation.META)
			_library().remove_animation(anim_name)
			var names := player.get_animation_list()
			anim_name = names[0] if not names.is_empty() else ""
			_refresh_ui())
	top.add_child(_label("  Length"))
	length_spin = _spin(top, 0.1, 30.0, 0.05, func(v: float) -> void:
		if _anim() != null:
			_anim().length = v)
	top.add_child(_label("FPS"))
	fps_spin = _spin(top, 1.0, 60.0, 1.0, func(v: float) -> void: fps = v)
	fps_spin.value = fps
	loop_check = _check(top, "Loop", func(on: bool) -> void:
		if _anim() != null:
			_anim().loop_mode = Animation.LOOP_LINEAR if on else Animation.LOOP_NONE)
	play_button = _button(top, "Play (Y)", _toggle_play)
	_button(top, "Key (I)", _insert_keys)
	_button(top, "Delete key (U)", _delete_keys)
	_button(top, "Export anim", _export_animation)
	_button(top, "Import anim", _import_animation)
	timeline = Timeline.new()
	timeline.editor = self
	timeline.custom_minimum_size = Vector2(0, 44)
	rows.add_child(timeline)
	status_label = Label.new()
	rows.add_child(status_label)
	help_label = Label.new()
	help_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	help_label.text = "The usual tools sculpt and paint the body.   M: leave   I: key the weights   U: delete key   Y: play   , .: frame (Shift: key)   Tab: mouse for these panels"
	rows.add_child(help_label)

func _label(p_text: String) -> Label:
	var label := Label.new()
	label.text = p_text
	return label

func _button(p_box: Container, p_text: String, p_pressed: Callable) -> Button:
	var button := Button.new()
	button.text = p_text
	button.focus_mode = Control.FOCUS_NONE  # keys stay hotkeys
	button.pressed.connect(p_pressed)
	p_box.add_child(button)
	return button

func _check(p_box: Container, p_text: String, p_toggled: Callable) -> CheckBox:
	var check := CheckBox.new()
	check.text = p_text
	check.focus_mode = Control.FOCUS_NONE
	check.toggled.connect(func(on: bool) -> void:
		if not updating_ui:
			p_toggled.call(on))
	p_box.add_child(check)
	return check

func _slider(p_box: Container, p_text: String, p_min: float, p_max: float, p_changed: Callable) -> HSlider:
	p_box.add_child(_label(p_text))
	var slider := HSlider.new()
	slider.min_value = p_min
	slider.max_value = p_max
	slider.step = (p_max - p_min) / 100.0
	slider.focus_mode = Control.FOCUS_NONE
	slider.value_changed.connect(func(v: float) -> void:
		if not updating_ui:
			p_changed.call(v))
	p_box.add_child(slider)
	return slider

func _spin(p_box: Container, p_min: float, p_max: float, p_step: float, p_changed: Callable) -> SpinBox:
	var spin := SpinBox.new()
	spin.min_value = p_min
	spin.max_value = p_max
	spin.step = p_step
	spin.value_changed.connect(func(v: float) -> void:
		if not updating_ui:
			p_changed.call(v))
	p_box.add_child(spin)
	return spin

## The centre of mass where the crosshair goes through the body (it stops
## being automatic).
func _set_facing(p_degrees: float) -> void:
	if not is_instance_valid(body) or body.get("facing_offset") == null:
		return
	if body.scene_file_path != "":
		world.creatures.set_kind_facing(body.scene_file_path, p_degrees)
	else:
		body.facing_offset = p_degrees

func _set_com_here() -> void:
	if not is_instance_valid(body):
		return
	var inside = _aimed_inside()
	if inside == null:
		return
	body.center_of_mass = body.to_local(inside)
	body.auto_center_of_mass = false
	_refresh_ui()

func _set_physics(p_on: bool) -> void:
	if not is_instance_valid(body):
		return
	if p_on:
		physics_start = body.global_transform
		body.freeze = false
		body.sleeping = false
	else:
		body.freeze = true
		body.linear_velocity = Vector3.ZERO
		body.angular_velocity = Vector3.ZERO
		body.global_transform = physics_start

func _refresh_ui() -> void:
	if not active or not is_instance_valid(body):
		return
	updating_ui = true
	var shape: DensityShape = body.shape
	var keys := shape.get_key_count()
	current_key = clampi(current_key, 0, maxi(keys - 1, 0))
	if sculpt_target >= keys:
		sculpt_target = -1
	var entity := _is_entity()
	title_label.text = "Object editor: %s   (%s)" % [body.name, "%d shape keys" % keys if entity else "plain object"]
	plain_label.visible = not entity
	for control in entity_controls:
		control.visible = entity
	timeline_panel.visible = entity
	if not entity:
		sculpt_target = -1
	facing_row.visible = body.get("facing_offset") != null
	if facing_row.visible:
		facing_spin.set_value_no_signal(body.facing_offset)
	# Its voxel size (one not offered is listed too, as it is).
	voxel_select.clear()
	var sizes: Array = VOXEL_SIZES.duplicate()
	var own: float = shape.get_voxel_size()
	if not sizes.any(func(v: float) -> bool: return is_equal_approx(v, own)):
		sizes.append(own)
		sizes.sort()
		sizes.reverse()
	for v in sizes:
		voxel_select.add_item(_voxel_text(v))
		voxel_select.set_item_metadata(voxel_select.item_count - 1, v)
		if is_equal_approx(v, own):
			voxel_select.select(voxel_select.item_count - 1)
	edit_key_select.clear()
	edit_key_select.add_item("All keys (the shape itself)", -1)
	edit_key_select.add_item("Current key (%s)" % shape.get_key_label(current_key), -2)
	for k in keys:
		edit_key_select.add_item("%s only" % shape.get_key_label(k), k)
	edit_key_select.select(edit_key_select.get_item_index(sculpt_target))
	_apply_sculpt_target()
	# A row per key: its name (click: the current key) and weight (key 0 is
	# usually the plain shape the others are added to: its slider only
	# works once it has a weight).
	for child in weights_box.get_children():
		child.queue_free()
	weight_sliders.clear()
	key_buttons.clear()
	for k in keys:
		var row := HBoxContainer.new()
		weights_box.add_child(row)
		var name_button := Button.new()
		name_button.text = shape.get_key_label(k)
		name_button.toggle_mode = true
		name_button.button_pressed = k == current_key
		name_button.focus_mode = Control.FOCUS_NONE
		name_button.custom_minimum_size.x = 96
		name_button.clip_text = true
		name_button.tooltip_text = "Make %s the current key" % shape.get_key_label(k)
		var picked := k
		name_button.pressed.connect(func() -> void: _set_current_key(picked))
		row.add_child(name_button)
		key_buttons.append(name_button)
		var slider := HSlider.new()
		slider.min_value = 0.0
		slider.max_value = 1.0
		slider.step = 0.01
		slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		slider.focus_mode = Control.FOCUS_NONE
		slider.value = body.get_key_weight(k)
		slider.editable = k > 0 or body.get_key_weight(0) != 0.0
		var key := k
		slider.value_changed.connect(func(v: float) -> void:
			if updating_ui or not is_instance_valid(body):
				return
			body.set_key_weight(key, v)
			if auto_key:
				_insert_weight_key(key)
			if current_key != key:
				_set_current_key(key))
		row.add_child(slider)
		weight_sliders.append(slider)
	auto_com_check.set_pressed_no_signal(body.auto_center_of_mass)
	anim_select.clear()
	var names := player.get_animation_list()
	for i in names.size():
		anim_select.add_item(names[i])
		if names[i] == anim_name:
			anim_select.select(i)
	var anim := _anim()
	if anim != null:
		length_spin.value = anim.length
		loop_check.set_pressed_no_signal(anim.loop_mode != Animation.LOOP_NONE)
	plays_select.clear()
	plays_select.add_item("(nothing)")
	var plays: String = BodyAnimation.get_plays(body)
	for i in names.size():
		plays_select.add_item(names[i])
		if names[i] == plays:
			plays_select.select(i + 1)
	updating_ui = false

## The key the key buttons act on.
func _current_key() -> int:
	return clampi(current_key, 0, maxi(body.shape.get_key_count() - 1, 0))

## (In place: a slider being dragged stays.)
func _set_current_key(p_key: int) -> void:
	current_key = p_key
	for k in key_buttons.size():
		key_buttons[k].set_pressed_no_signal(k == current_key)
	var at := edit_key_select.get_item_index(-2)
	if at >= 0:
		edit_key_select.set_item_text(at, "Current key (%s)" % body.shape.get_key_label(current_key))
	_apply_sculpt_target()

## Brushes edit what "Sculpt" says (the current key follows it).
func _apply_sculpt_target() -> void:
	if not is_instance_valid(body):
		return
	body.edit_key = _current_key() if sculpt_target == -2 else sculpt_target

func _rename_key(p_name: String) -> void:
	if not is_instance_valid(body) or body.shape.get_key_count() == 0:
		return
	var k := _current_key()
	_change_shape(func(s: DensityShape) -> void: s.set_key_name(k, p_name))

## After keys moved: animation tracks and the body's weights follow them
## (p_map[old] = new, -1 removed).
func _remap_keys(p_map: PackedInt32Array) -> void:
	BodyAnimation.remap_weight_tracks(body, p_map)
	var old: PackedFloat32Array = body.key_weights
	var weights := PackedFloat32Array()
	weights.resize(body.shape.get_key_count())
	for i in mini(old.size(), p_map.size()):
		if p_map[i] >= 0 and p_map[i] < weights.size():
			weights[p_map[i]] = old[i]
	body.key_weights = weights

## Changes a copy of the body's shape with p_change (given the copy), as one undo step.
func _change_shape(p_change: Callable) -> void:
	if not is_instance_valid(body):
		return
	var before: DensityShape = body.shape
	var after: DensityShape = before.duplicate_shape()
	p_change.call(after)
	body.shape = after
	world._push_history({"body": body, "before": before, "after": after,
			"part_shapes": [], "part_transform": body.global_transform, "parts": []})
	_refresh_ui()

## A new last key, as the body looks now (pose and key weights), picked for
## sculpting and shown.
func _add_key() -> void:
	if not _is_entity():
		return
	var added := body.shape.get_key_count()
	if added == 0:
		return
	var weights: PackedFloat32Array = body.key_weights
	_change_shape(func(s: DensityShape) -> void: s.add_key(body.pose, weights))
	# Shown as it was: the new key alone, at 1 (it has the others in it).
	var shown := PackedFloat32Array()
	shown.resize(added + 1)
	shown[added] = 1.0
	body.key_weights = shown
	body.pose = 0.0
	current_key = added
	if auto_key:
		_insert_keys()
	_refresh_ui()

## Moves the current key one place earlier (-1) or later (1).
func _move_key(p_by: int) -> void:
	if not is_instance_valid(body):
		return
	var k := _current_key()
	var to := k + p_by
	if to < 0 or to >= body.shape.get_key_count():
		return
	_change_shape(func(s: DensityShape) -> void: s.move_key(k, to))
	var map := PackedInt32Array(range(body.shape.get_key_count()))
	map[k] = to
	map[to] = k
	_remap_keys(map)
	current_key = to
	if sculpt_target == k:
		sculpt_target = to
	elif sculpt_target == to:
		sculpt_target = k
	_refresh_ui()

## The current key back to the plain shape (no motion, nothing of its own).
func _reset_key() -> void:
	if not is_instance_valid(body):
		return
	var k := _current_key()
	_change_shape(func(s: DensityShape) -> void: s.reset_key(k))

func _remove_key() -> void:
	if not is_instance_valid(body) or body.shape.get_key_count() <= 1:
		return
	var k := _current_key()
	var count := body.shape.get_key_count()
	_change_shape(func(s: DensityShape) -> void: s.remove_key(k))
	var map := PackedInt32Array()
	for i in count:
		map.append(-1 if i == k else (i if i < k else i - 1))
	_remap_keys(map)
	if sculpt_target >= 0:
		sculpt_target = -1
	current_key = maxi(k - 1, 0)
	body.pose = minf(body.pose, body.shape.get_key_count() - 1)
	_refresh_ui()

func _update_status() -> void:
	var anim := _anim()
	var frame := roundi(time * fps)
	status_label.text = "%s   frame %d / %d (%.2f s)   current key: %s" % [anim_name if anim != null else "(no animation: I keys one)", frame,
			roundi(anim.length * fps) if anim != null else 0, time, body.shape.get_key_label(_current_key())]
	if Time.get_ticks_msec() < notice_until:
		status_label.text += "      " + notice
	world.info_label.text = "Object editor - %d shape keys   %d FPS" % [body.shape.get_key_count(), Engine.get_frames_per_second()]


# --- Files --------------------------------------------------------------------------

func _open_dialog(p_mode: FileDialog.FileMode, p_filter: String, p_chosen: Callable) -> void:
	if file_dialog != null:
		file_dialog.queue_free()
	file_dialog = FileDialog.new()
	file_dialog.file_mode = p_mode
	file_dialog.access = FileDialog.ACCESS_FILESYSTEM
	file_dialog.filters = PackedStringArray([p_filter])
	file_dialog.current_dir = ProjectSettings.globalize_path("res://")
	file_dialog.use_native_dialog = true
	file_dialog.file_selected.connect(p_chosen)
	ui.add_child(file_dialog)
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	file_dialog.popup_centered_ratio(0.6)

func _save(p_resource: Resource, p_path: String) -> void:
	var error := ResourceSaver.save(p_resource, p_path)
	if error != OK:
		push_error("Couldn't save %s: %s" % [p_path, error_string(error)])

## A creature's kind (creatures.gd): its scene's files take this shape and
## these animations, so every one starts out so, in every world.
func _make_default() -> void:
	if body.scene_file_path == "":
		notice = "Only for a frog or chiggin (something with a scene of its own): Export scene makes one"
	else:
		notice = world.creatures.save_as_default(body)
	notice_until = Time.get_ticks_msec() + 8000

## The shape with its pose keys (load it with Ctrl+O, or set it as a body's shape).
func _export_model() -> void:
	var shape: DensityShape = body.shape.duplicate_shape()
	_open_dialog(FileDialog.FILE_MODE_SAVE_FILE, "*.res ; Density shape", func(path: String) -> void: _save(shape, path))

## Replaces the body's model with a saved shape (an export, a Ctrl+P), as one
## undo step. Its keys stay where the animations expect them: the shape's own
## first, then any of the old one's past those, empty (named as before), to
## sculpt again.
func _import_model() -> void:
	_open_dialog(FileDialog.FILE_MODE_OPEN_FILE, "*.res, *.tres ; Density shape", import_model_from)

func import_model_from(path: String) -> void:
	var loaded := load(path) as DensityShape
	if loaded == null or not is_instance_valid(body):
		notice = "%s isn't a density shape" % path.get_file()
		notice_until = Time.get_ticks_msec() + 8000
		return
	var before: DensityShape = body.shape
	var after: DensityShape = loaded.duplicate_shape()
	if not _is_entity():
		after = PhysicsObject.conform_shape(after)  # (one shape: no shape keys; its voxel size kept)
	var own := after.get_key_count()
	var old := before.get_key_count() if before != null and _is_entity() else 0
	if own > 0:
		for k in mini(own, old):
			if after.get_key_name(k) == "" and before.get_key_name(k) != "":
				after.set_key_name(k, before.get_key_name(k))
		for k in range(own, old):
			after.set_key_name(after.add_key(0.0), before.get_key_name(k))
	body.shape = after
	var weights: PackedFloat32Array = body.key_weights
	weights.resize(after.get_key_count())
	body.key_weights = weights
	body.pose = minf(body.pose, maxf(after.get_key_count() - 1, 0))
	world._push_history({"body": body, "before": before, "after": after,
			"part_shapes": [], "part_transform": body.global_transform, "parts": []})
	current_key = mini(current_key, maxi(after.get_key_count() - 1, 0))
	notice = "Imported %s" % path.get_file() + (" (%d of its keys empty: sculpt them again)" % (old - own) if old > own and own > 0 else "")
	notice_until = Time.get_ticks_msec() + 8000
	_refresh_ui()

static func _voxel_text(p_size: float) -> String:
	return ("%.3f" % p_size).rstrip("0").rstrip(".")

## Samples the body's shape again at p_size (its keys too), as one undo
## step. A shape too big for that many voxels gets the finest that fits.
func set_voxel_size(p_size: float) -> void:
	if not is_instance_valid(body) or body.shape == null:
		return
	var before: DensityShape = body.shape
	if is_equal_approx(before.get_voxel_size(), p_size):
		return
	var after: DensityShape = before.resampled(p_size)
	body.shape = after
	world._push_history({"body": body, "before": before, "after": after,
			"part_shapes": [], "part_transform": body.global_transform, "parts": []})
	var got := after.get_voxel_size()
	notice = "Voxel size %s" % _voxel_text(got) + ("" if is_equal_approx(got, p_size) else
			" (too big for %s: at most 64 voxels a side)" % _voxel_text(p_size))
	notice_until = Time.get_ticks_msec() + 8000
	_refresh_ui()

func _export_animation() -> void:
	var anim := _anim()
	if anim == null:
		return
	var copy := anim.duplicate(true)
	_open_dialog(FileDialog.FILE_MODE_SAVE_FILE, "*.tres, *.res ; Animation", func(path: String) -> void: _save(copy, path))

func _import_animation() -> void:
	_open_dialog(FileDialog.FILE_MODE_OPEN_FILE, "*.tres, *.res, *.anim ; Animation", func(path: String) -> void:
		var anim := load(path) as Animation
		if anim == null:
			push_error("%s isn't an animation" % path)
			return
		var base := path.get_file().get_basename().validate_node_name()
		var unique := base
		var n := 2
		while player.has_animation(unique):
			unique = "%s%d" % [base, n]
			n += 1
		_library().add_animation(unique, anim.duplicate(true))
		anim_name = unique
		time = 0.0
		_show_time()
		_refresh_ui())

## A scene of its own: the body (shape, physics settings) and an
## AnimationPlayer with every animation, ready to instance.
func _export_scene() -> void:
	_open_dialog(FileDialog.FILE_MODE_SAVE_FILE, "*.tscn ; Scene", export_scene_to)

func export_scene_to(path: String) -> void:
		var root: DensityBody3D = Entity.new()  # (dropped from its scene, it's a creature: an entity)
		root.name = path.get_file().get_basename().validate_node_name()
		root.shape = body.shape.duplicate_shape()
		for property in ["mass", "bounce", "friction", "gravity_scale", "linear_damp", "angular_damp", "rolling_resistance",
				"auto_material_properties", "auto_center_of_mass", "center_of_mass", "collide_with_bodies", "drawn", "color", "material", "pose", "key_weights"]:
			root.set(property, body.get(property))
		var animator := AnimationPlayer.new()
		animator.name = "AnimationPlayer"
		var lib := AnimationLibrary.new()
		for anim_key in player.get_animation_list():
			lib.add_animation(anim_key, player.get_animation(anim_key).duplicate(true))
		animator.add_animation_library("", lib)
		root.add_child(animator)
		animator.owner = root
		var packed := PackedScene.new()
		var error := packed.pack(root)
		if error == OK:
			_save(packed, path)
		else:
			push_error("Couldn't pack the scene: %s" % error_string(error))
		root.free()


## The timeline: a tick per frame, a diamond per key, the current frame's
## line; click or drag to move through it. Dragging a diamond moves its keys
## (every track's at that time) to another frame; keys it passes over come
## back as it moves on, and the one it's let go on is replaced.
class Timeline extends Control:
	var editor  # object_editor.gd
	var moving_from := -1.0  # the dragged keys' time when the drag began (-1: scrubbing)
	var moving_to := -1.0
	var before: Array = []  # every track's keys when the drag began: [[time, value, transition], ...] per track

	func _draw() -> void:
		var w := size.x
		var h := size.y
		draw_rect(Rect2(Vector2.ZERO, size), Color(0.08, 0.08, 0.1, 0.9))
		var anim: Animation = editor._anim()
		if anim == null or anim.length <= 0.0:
			draw_string(get_theme_default_font(), Vector2(8, h * 0.6), "No animation yet: move a key's weight slider (or press I, or New)")
			return
		var fps: float = editor.fps
		var frames := roundi(anim.length * fps)
		for f in frames + 1:
			var x := f / fps / anim.length * w
			var major := f % int(maxf(fps, 1.0)) == 0
			draw_line(Vector2(x, h * (0.45 if major else 0.7)), Vector2(x, h), Color(1, 1, 1, 0.5 if major else 0.2))
		for t in editor._key_times():
			var x: float = t / anim.length * w
			var c := Vector2(x, h * 0.3)
			var r := 6.0 if moving_from >= 0.0 and absf(t - moving_to) < 0.001 else 5.0
			var color := Color(1, 1, 1) if r > 5.0 else Color(1, 0.8, 0.2)
			draw_colored_polygon(PackedVector2Array([c + Vector2(0, -r), c + Vector2(r, 0), c + Vector2(0, r), c + Vector2(-r, 0)]), color)
		var px: float = editor.time / anim.length * w
		draw_line(Vector2(px, 0), Vector2(px, h), Color(0.3, 0.8, 1.0), 2.0)

	func _gui_input(event: InputEvent) -> void:
		var anim: Animation = editor._anim()
		if anim == null:
			return
		var w := maxf(size.x, 1.0)
		if event is InputEventMouseButton and event.button_index == MOUSE_BUTTON_LEFT:
			if event.pressed:
				moving_from = _key_at(anim, event.position)
				if moving_from >= 0.0:
					moving_to = moving_from
					before = _keys_of(anim)
					editor.scrub(moving_from)
				else:
					editor.scrub(clampf(event.position.x / w, 0.0, 1.0) * anim.length)
			else:
				moving_from = -1.0
				before = []
			accept_event()
			queue_redraw()
		elif event is InputEventMouseMotion and event.button_mask & MOUSE_BUTTON_MASK_LEFT:
			var at := clampf(event.position.x / w, 0.0, 1.0) * anim.length
			if moving_from >= 0.0:
				var fps: float = editor.fps
				var to := clampf(roundf(at * fps) / fps, 0.0, anim.length)
				if absf(to - moving_to) > 0.0005:
					moving_to = to
					_move_keys(anim, moving_from, to)
					editor.scrub(to)
			else:
				editor.scrub(at)
			accept_event()
			queue_redraw()

	## The time of the key diamond under p_point, or -1.
	func _key_at(p_anim: Animation, p_point: Vector2) -> float:
		if p_point.y > size.y * 0.65:
			return -1.0
		var best := -1.0
		var best_d := 8.0
		for t in editor._key_times():
			var d := absf(t / p_anim.length * size.x - p_point.x)
			if d < best_d:
				best_d = d
				best = t
		return best

	func _keys_of(p_anim: Animation) -> Array:
		var all := []
		for track in p_anim.get_track_count():
			var keys := []
			for k in p_anim.track_get_key_count(track):
				keys.append([p_anim.track_get_key_time(track, k), p_anim.track_get_key_value(track, k), p_anim.track_get_key_transition(track, k)])
			all.append(keys)
		return all

	## Every track as it was when the drag began, but with its keys at
	## p_from at p_to instead (over any there).
	func _move_keys(p_anim: Animation, p_from: float, p_to: float) -> void:
		for track in mini(p_anim.get_track_count(), before.size()):
			for k in range(p_anim.track_get_key_count(track) - 1, -1, -1):
				p_anim.track_remove_key(track, k)
			var moved = null
			for key in before[track]:
				if absf(key[0] - p_from) < 0.001:
					moved = key
			for key in before[track]:
				if moved != null and (key == moved or absf(key[0] - p_to) < 0.001):
					continue  # (the moved key, and whatever it's on now)
				p_anim.track_insert_key(track, key[0], key[1], key[2])
			if moved != null:
				p_anim.track_insert_key(track, p_to, moved[1], moved[2])
