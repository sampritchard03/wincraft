extends Node
## The object editor: press M looking at a ball, frog or other body to edit
## it (M again to leave). The body holds still and a panel and timeline
## appear. The usual tools sculpt and paint it (the panel's "Sculpt" picks
## whether brushes change every pose key or just one).
##
## Pose keys: a body's shape can hold several keys (like a mesh's shape
## keys); its `pose` blends between them (1.5 = halfway from key 1 to key 2).
## The pose slider sets it; with Auto key on, each change is keyed at the
## current frame.
##
## Animation: I keys the pose at the current frame, U removes the keys there,
## Y plays / pauses, , and . step a frame (Shift: to the previous / next
## key). Tab frees the mouse for the panels (settings, animations, export).
##
## The animations live on the body itself, in its AnimationPlayer (made if
## it has none), so the frog's AnimationTree, say, can play what you make here.

var world: Node  # world.gd
var active := false
var body: DensityBody3D = null

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
var pose_slider: HSlider
var edit_key_select: OptionButton
var physics_check: CheckBox
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
var updating_ui := false  # filling the panels in: their signals are ignored
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
	return active and (side_panel.get_global_rect().has_point(p_position) or timeline_panel.get_global_rect().has_point(p_position))


# --- Opening and closing ----------------------------------------------------

func open_aimed() -> void:
	var aimed := DensityBody3D.raycast_bodies(world.camera.global_position, -world.camera.global_basis.z, world.reach)
	if aimed.is_empty() or aimed.body.shape == null:
		return
	open(aimed.body)

func open(p_body: DensityBody3D) -> void:
	if active:
		close()
	body = p_body
	active = true
	was_frozen = body.freeze
	body.freeze = true
	body.linear_velocity = Vector3.ZERO
	body.angular_velocity = Vector3.ZERO
	# Its own copy to edit (frogs share one shape).
	body.shape = body.shape.duplicate_shape()
	anim_tree = null
	for child in body.get_children():
		if child is AnimationTree:
			anim_tree = child
	if anim_tree != null:
		tree_was_active = anim_tree.active
		anim_tree.active = false
	player = null
	for child in body.get_children():
		if child is AnimationPlayer:
			player = child
	if player == null:
		player = AnimationPlayer.new()
		player.name = "AnimationPlayer"
		body.add_child(player)
	if not player.has_animation_library(""):
		player.add_animation_library("", AnimationLibrary.new())
	var names := player.get_animation_list()
	anim_name = names[0] if not names.is_empty() else ""
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
		body.edit_key = -1
		body.freeze = was_frozen
		body.sleeping = false
		if anim_tree != null and is_instance_valid(anim_tree):
			if is_instance_valid(player):
				player.stop()
			anim_tree.active = tree_was_active
	body = null
	Input.mouse_mode = Input.MOUSE_MODE_CAPTURED


# --- Every frame --------------------------------------------------------------

func _process(_delta: float) -> void:
	if not active:
		return
	if not is_instance_valid(body):
		close()
		return
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
		updating_ui = true
		pose_slider.value = body.pose
		updating_ui = false
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

## Keys the body's pose at the current time (only with two or more keys:
## with one there is nothing to blend).
func _insert_keys() -> void:
	if not is_instance_valid(body) or body.shape.get_key_count() < 2:
		return
	if _anim() == null:
		_new_animation()
	_set_key(_anim(), NodePath(".:pose"), Animation.TYPE_VALUE, body.pose)
	timeline.queue_redraw()

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
	if pose_slider != null and is_instance_valid(body):
		updating_ui = true
		pose_slider.value = body.pose
		updating_ui = false


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
	line_mesh.surface_end()


# --- Panels -----------------------------------------------------------------------

func _make_panels() -> void:
	ui = Control.new()
	ui.set_anchors_preset(Control.PRESET_FULL_RECT)
	ui.mouse_filter = Control.MOUSE_FILTER_IGNORE
	ui.visible = false
	world.info_label.get_parent().add_child.call_deferred(ui)

	# Settings: on the right.
	side_panel = PanelContainer.new()
	side_panel.anchor_left = 1.0
	side_panel.anchor_right = 1.0
	side_panel.offset_left = -296
	side_panel.offset_right = -8
	side_panel.offset_top = 72
	side_panel.grow_horizontal = Control.GROW_DIRECTION_BEGIN
	ui.add_child(side_panel)
	var box := VBoxContainer.new()
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	side_panel.add_child(box)
	title_label = Label.new()
	box.add_child(title_label)
	pose_slider = _slider(box, "Pose (blends the shape's keys)", 0.0, 1.0, func(v: float) -> void:
		if is_instance_valid(body):
			body.pose = v
			if auto_key:
				_insert_keys())
	box.add_child(_label("Sculpt"))
	edit_key_select = OptionButton.new()
	edit_key_select.focus_mode = Control.FOCUS_NONE
	edit_key_select.item_selected.connect(func(i: int) -> void:
		if not updating_ui and is_instance_valid(body):
			body.edit_key = i - 1)  # item 0: all keys
	box.add_child(edit_key_select)
	physics_check = _check(box, "Physics on (test it; off puts it back)", _set_physics)
	auto_com_check = _check(box, "Auto centre of mass", func(on: bool) -> void:
		if is_instance_valid(body):
			body.auto_center_of_mass = on)
	_button(box, "Centre of mass at the crosshair", _set_com_here)
	autokey_check = _check(box, "Auto key the pose slider", func(on: bool) -> void: auto_key = on)
	autokey_check.set_pressed_no_signal(true)
	var files := HBoxContainer.new()
	box.add_child(files)
	_button(files, "Export model", _export_model)
	_button(files, "Export scene", _export_scene)

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
	help_label.text = "The usual tools sculpt and paint the body.   M: leave   I: key the pose   U: delete key   Y: play   , .: frame (Shift: key)   Tab: mouse for these panels"
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
	title_label.text = "Object editor: %s   (%d pose keys)" % [body.name, keys]
	pose_slider.max_value = maxf(keys - 1, 0.001)
	pose_slider.editable = keys > 1
	pose_slider.value = body.pose
	edit_key_select.clear()
	edit_key_select.add_item("All keys")
	for k in keys:
		edit_key_select.add_item("Key %d only" % k)
	edit_key_select.select(clampi(body.edit_key + 1, 0, keys))
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
	updating_ui = false

func _update_status() -> void:
	var anim := _anim()
	var frame := roundi(time * fps)
	status_label.text = "%s   frame %d / %d (%.2f s)   pose %.2f" % [anim_name if anim != null else "(no animation: I keys one)", frame,
			roundi(anim.length * fps) if anim != null else 0, time, body.pose]
	world.info_label.text = "Object editor - %d pose keys   %d FPS" % [body.shape.get_key_count(), Engine.get_frames_per_second()]


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

## The shape with its pose keys (load it with Ctrl+O, or set it as a body's shape).
func _export_model() -> void:
	var shape: DensityShape = body.shape.duplicate_shape()
	_open_dialog(FileDialog.FILE_MODE_SAVE_FILE, "*.res ; Density shape", func(path: String) -> void: _save(shape, path))

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
		var root := DensityBody3D.new()
		root.name = path.get_file().get_basename().validate_node_name()
		root.shape = body.shape.duplicate_shape()
		for property in ["mass", "bounce", "friction", "gravity_scale", "linear_damp", "angular_damp", "rolling_resistance",
				"auto_material_properties", "auto_center_of_mass", "center_of_mass", "collide_with_bodies", "drawn", "color", "material", "pose"]:
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
## line; click or drag to move through it.
class Timeline extends Control:
	var editor  # object_editor.gd

	func _draw() -> void:
		var w := size.x
		var h := size.y
		draw_rect(Rect2(Vector2.ZERO, size), Color(0.08, 0.08, 0.1, 0.9))
		var anim: Animation = editor._anim()
		if anim == null or anim.length <= 0.0:
			draw_string(get_theme_default_font(), Vector2(8, h * 0.6), "No animation yet: move the pose slider and press I (or New)")
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
			var r := 5.0
			draw_colored_polygon(PackedVector2Array([c + Vector2(0, -r), c + Vector2(r, 0), c + Vector2(0, r), c + Vector2(-r, 0)]), Color(1, 0.8, 0.2))
		var px: float = editor.time / anim.length * w
		draw_line(Vector2(px, 0), Vector2(px, h), Color(0.3, 0.8, 1.0), 2.0)

	func _gui_input(event: InputEvent) -> void:
		var anim: Animation = editor._anim()
		if anim == null:
			return
		if (event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_LEFT) or \
				(event is InputEventMouseMotion and event.button_mask & MOUSE_BUTTON_MASK_LEFT):
			editor.scrub(clampf(event.position.x / maxf(size.x, 1.0), 0.0, 1.0) * anim.length)
			accept_event()
