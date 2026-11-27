extends Control
## The on-screen interface over the 3D view, skinned from res://assets/ui/
## (ui_skin.gd; replace a PNG there to change how it looks):
##
## - crosshair (crosshair.png) in the middle;
## - a bar along the bottom: one slot (slot.png) each for the tool, the
##   material, brush radius and strength, symmetry, blocky mode, the
##   selection and undo / redo, with their icons (tool_*.png, stat_*.png);
## - top left, the status panel (panel.png): the current tool's hint, the
##   object editor's or the connection's status (world.info_label);
## - top right, frames per second and, online, the players and traffic;
## - H shows every key (the help panel), Tab held the tool wheel
##   (radial_menu.gd), C the color picker (world.gd's color_panel, added here).
##
## Laid out for a 720-pixel-high window and scaled to the real one.

const UiSkin := preload("res://ui_skin.gd")
const RadialMenu := preload("res://radial_menu.gd")

const ICON := 32.0  # icon size in the bar, before scaling
const DESIGN_HEIGHT := 720.0

var world: Node  # world.gd
## In a headset (vr_ui.gd) the HUD is a panel on the wrist: no crosshair.
var vr_mode := false

## The material slot was clicked (the color picker), and the help slot.
signal color_pressed
signal help_pressed
var radial: RadialMenu
var help_panel: PanelContainer

var crosshair: TextureRect
var status_panel: PanelContainer
var status_icon: TextureRect
var corner: VBoxContainer
var fps_label: Label
var network_panel: PanelContainer
var network_label: Label

## The bar's slots: name -> {icon: TextureRect, value: Label, caption: Label, panel}
var slots := {}
const STATUS_WIDTH := 560.0
var shown_status := ""
var white: ImageTexture  # the color swatch (tinted)

func _init(p_world: Node) -> void:
	name = "Hud"
	world = p_world
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	theme = UiSkin.make_theme()
	var image := Image.create_empty(4, 4, false, Image.FORMAT_RGBA8)
	image.fill(Color.WHITE)
	white = ImageTexture.create_from_image(image)

	crosshair = TextureRect.new()
	crosshair.texture = UiSkin.texture("crosshair")
	crosshair.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	crosshair.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(crosshair)

	_make_status()
	_make_corner()
	_make_bar()
	_make_help()

	radial = RadialMenu.new()
	add_child(radial)

## The window changed: scale to it, and keep things in their corners.
func fit(p_window: Vector2) -> void:
	var s := clampf(p_window.y / DESIGN_HEIGHT, 0.6, 2.0)
	scale = Vector2(s, s)
	position = Vector2.ZERO
	size = p_window / s
	var cross := 32.0
	crosshair.size = Vector2(cross, cross)
	crosshair.position = size * 0.5 - crosshair.size * 0.5

## Each frame (world.gd's _sculpt): shows the world's state.
func refresh() -> void:
	var tool_name: String = world.TOOL_NAMES[world.tool]
	_set_slot("tool", UiSkin.texture(UiSkin.tool_icon(tool_name)), tool_name)

	var material := slots.material as Dictionary
	if world.block < world.material_names.size():
		var block_name: String = world.material_names[world.block]
		(material.icon as TextureRect).texture = world.block_icon(world.block)
		(material.icon as TextureRect).self_modulate = Color.WHITE
		(material.value as Label).text = block_name.capitalize()
	else:
		(material.icon as TextureRect).texture = white
		(material.icon as TextureRect).self_modulate = world.paint_color
		(material.value as Label).text = "Color"

	(slots.radius.value as Label).text = "%.1f" % world.brush_radius
	(slots.strength.value as Label).text = "%.1f" % world.brush_strength

	if not world.symmetry_on:
		_set_slot("symmetry", UiSkin.texture("stat_symmetry_off"), "Off")
	elif world.symmetry == world.Symmetry.MIRROR:
		_set_slot("symmetry", UiSkin.texture("stat_symmetry_mirror"), "Mirror")
	else:
		_set_slot("symmetry", UiSkin.texture("stat_symmetry_radial"), "Radial x%d" % world.radial_count)
	_dim("symmetry", not world.symmetry_on)

	var blocky: bool = world.terrain.blocky_size >= 2
	(slots.blocky.value as Label).text = "On" if blocky else "Off"
	_dim("blocky", not blocky)

	var selected: int = world.terrain.get_selected_count()
	(slots.selection.value as Label).text = str(selected)
	_dim("selection", selected == 0)

	(slots.undo.value as Label).text = "%d / %d" % [world.terrain.get_undo_count(), world.terrain.get_redo_count()]

	fps_label.text = "%d FPS" % Engine.get_frames_per_second()
	var online: String = world.network.hud_text() if not world.network.status.is_empty() else ""
	network_label.text = online
	network_panel.visible = not online.is_empty()
	update_status()

## The status panel shows only while it has something to say.
func update_status() -> void:
	var info: Label = world.info_label
	status_panel.visible = not info.text.is_empty()
	if status_panel.visible and info.text != shown_status:
		# One line if it fits in STATUS_WIDTH, else wrapped to that.
		shown_status = info.text
		var width := info.get_theme_font("font").get_multiline_string_size(info.text, HORIZONTAL_ALIGNMENT_LEFT, -1,
				info.get_theme_font_size("font_size")).x
		info.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		info.custom_minimum_size.x = minf(ceilf(width) + 2.0, STATUS_WIDTH)
		status_panel.reset_size()

func toggle_help() -> void:
	help_panel.visible = not help_panel.visible

func _process(_delta: float) -> void:
	update_status()  # (the editor and the connection set it too)
	crosshair.visible = not radial.visible and not vr_mode


# --- Building it ----------------------------------------------------------------------

func _make_status() -> void:
	status_panel = PanelContainer.new()
	status_panel.position = Vector2(12, 10)
	status_panel.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(status_panel)
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)
	status_panel.add_child(row)
	status_icon = _icon(UiSkin.texture("stat_info"), 24.0)
	status_icon.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	row.add_child(status_icon)
	# The scene's Info label lives here now: the world, the object editor and
	# the connection write their messages into it.
	var info: Label = world.info_label
	info.get_parent().remove_child(info)
	info.remove_theme_color_override("font_outline_color")
	info.remove_theme_constant_override("outline_size")
	info.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	row.add_child(info)
	status_panel.visible = false

func _make_corner() -> void:
	corner = VBoxContainer.new()
	corner.set_anchors_and_offsets_preset(Control.PRESET_TOP_RIGHT)
	corner.grow_horizontal = Control.GROW_DIRECTION_BEGIN
	corner.offset_right = -12
	corner.offset_top = 10
	corner.alignment = BoxContainer.ALIGNMENT_BEGIN
	corner.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(corner)
	var fps_panel := PanelContainer.new()
	fps_panel.size_flags_horizontal = Control.SIZE_SHRINK_END
	corner.add_child(fps_panel)
	var row := HBoxContainer.new()
	fps_panel.add_child(row)
	row.add_child(_icon(UiSkin.texture("stat_fps"), 22.0))
	fps_label = Label.new()
	row.add_child(fps_label)
	network_panel = PanelContainer.new()
	network_panel.size_flags_horizontal = Control.SIZE_SHRINK_END
	network_panel.visible = false
	corner.add_child(network_panel)
	var net_row := HBoxContainer.new()
	network_panel.add_child(net_row)
	var players := _icon(UiSkin.texture("stat_players"), 22.0)
	players.size_flags_vertical = Control.SIZE_SHRINK_BEGIN
	net_row.add_child(players)
	network_label = Label.new()
	network_label.theme_type_variation = "SmallLabel"
	network_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	network_label.custom_minimum_size.x = 340
	net_row.add_child(network_label)

func _make_bar() -> void:
	var bar := HBoxContainer.new()
	bar.set_anchors_and_offsets_preset(Control.PRESET_CENTER_BOTTOM)
	bar.grow_horizontal = Control.GROW_DIRECTION_BOTH
	bar.grow_vertical = Control.GROW_DIRECTION_BEGIN
	bar.offset_bottom = -10
	bar.add_theme_constant_override("separation", 6)
	bar.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(bar)
	_add_slot(bar, "tool", "tool_draw", "Tab: wheel", 96.0)
	_add_slot(bar, "material", "", "[ ]  C: color", 80.0)
	(slots.material.icon as TextureRect).texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
	_add_slot(bar, "radius", "stat_radius", "Wheel")
	_add_slot(bar, "strength", "stat_strength", "Ctrl+wheel")
	_add_slot(bar, "symmetry", "stat_symmetry_off", "E  Q  Z", 72.0)
	_add_slot(bar, "blocky", "stat_blocky", "B")
	_add_slot(bar, "selection", "stat_selection", "Backspace")
	_add_slot(bar, "undo", "stat_undo", "Ctrl+Z / Y")
	_add_slot(bar, "help", "stat_help", "H")
	(slots.help.value as Label).text = "Keys"
	_clickable("material", color_pressed)
	_clickable("help", help_pressed)

## A slot that does something when clicked (with the mouse free, or a
## headset's laser).
func _clickable(p_name: String, p_signal: Signal) -> void:
	var panel := slots[p_name].panel as PanelContainer
	panel.mouse_filter = Control.MOUSE_FILTER_STOP
	panel.mouse_default_cursor_shape = Control.CURSOR_POINTING_HAND
	panel.gui_input.connect(func(event: InputEvent) -> void:
		if event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_LEFT:
			p_signal.emit()
			panel.accept_event())

## Whether a point (in the HUD's viewport) is on one of its panels.
func has_point(p_point: Vector2) -> bool:
	var parts: Array = [status_panel, help_panel, world.color_panel, world.brush_panel]
	parts.append_array(corner.get_children())
	for slot in slots.values():
		parts.append(slot.panel)
	for part in parts:
		var control := part as Control
		if control != null and control.is_visible_in_tree() and control.get_global_rect().has_point(p_point):
			return true
	return false

## p_width: the text's room, so the bar doesn't shift as values change.
func _add_slot(p_bar: HBoxContainer, p_name: String, p_icon: String, p_caption: String, p_width := 0.0) -> void:
	var panel := PanelContainer.new()
	panel.add_theme_stylebox_override("panel", UiSkin.box("slot", UiSkin.SLOT_MARGIN, 7.0))
	panel.mouse_filter = Control.MOUSE_FILTER_IGNORE
	p_bar.add_child(panel)
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 6)
	panel.add_child(row)
	var icon := _icon(UiSkin.texture(p_icon) if not p_icon.is_empty() else white, ICON)
	icon.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	row.add_child(icon)
	var column := VBoxContainer.new()
	column.custom_minimum_size.x = p_width
	column.add_theme_constant_override("separation", -2)
	row.add_child(column)
	var value := Label.new()
	column.add_child(value)
	var caption := Label.new()
	caption.theme_type_variation = "SmallLabel"
	caption.text = p_caption
	column.add_child(caption)
	slots[p_name] = {"panel": panel, "icon": icon, "value": value, "caption": caption}

func _set_slot(p_name: String, p_icon: Texture2D, p_value: String) -> void:
	var slot := slots[p_name] as Dictionary
	(slot.icon as TextureRect).texture = p_icon
	(slot.value as Label).text = p_value

func _dim(p_name: String, p_off: bool) -> void:
	(slots[p_name].panel as Control).modulate = Color(1, 1, 1, 0.55) if p_off else Color.WHITE

func _icon(p_texture: Texture2D, p_size: float) -> TextureRect:
	var icon := TextureRect.new()
	icon.texture = p_texture
	icon.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	icon.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	icon.custom_minimum_size = Vector2(p_size, p_size)
	icon.mouse_filter = Control.MOUSE_FILTER_IGNORE
	return icon

const HELP := [
	["Tools", ""],
	["Tab (hold)", "Tool wheel: point at a tool, let go to pick it (holding with the physics gun: turn it with the mouse)"],
	["1 - 0", "Draw / Inflate / Clay strips / Layer, Smooth / Pinch, Flatten, Grab / Rotate / Scale, Snake hook, Paint / Clone, Select (paint / fill / geometry / box), Extrude / Transform, Physics gun, Geometry / Box (Shift: cut)"],
	["LMB / RMB", "Sculpt / carve (Inflate, Pinch, Layer, Clay strips: the opposite)"],
	["Alt + LMB", "Any brush the other way (Draw carves, Smooth sharpens, Flatten pushes away...)"],
	["Shift + click", "A straight stroke from where the last ended (Grab: pulls that spot to here)"],
	["Ctrl + click", "Clone: where to copy from"],
	["N", "Brush settings: falloff, sphere / cube, blocky, Flatten's plane and mode, Layer height, lazy mouse; and Delete all objects"],
	["Wheel", "Brush radius (Ctrl: strength, Shift: your size)"],
	["Enter / Backspace", "Confirm (Shift+Enter: cut out) / clear the selection or points"],
	["T", "Transform: move / rotate / scale"],
	["Material", ""],
	["[  ]", "Previous / next material (the last is Color)"],
	["Middle click", "Pick the material or color under the crosshair"],
	["C", "Color picker, opacity and hardness"],
	["Symmetry", ""],
	["E / Q", "Axis along the view / remove it"],
	["Z,  + / -", "Mirror or radial, radial count"],
	["Selection", ""],
	["P", "Selection to a physics object (selected objects: back to ground)"],
	["Select tools on an object", "Select it (RMB: let it go); not with ground"],
	["Ctrl+C / Ctrl+V", "Copy / paste"],
	["Ctrl+P / Ctrl+O", "Export the selection or selected object / import a shape (Ctrl+Shift+O: import to paste)"],
	["World", ""],
	["F / G / X", "Throw a ball / drop a frog or chiggin / remove the one aimed at"],
	["Shift+G", "Frog or chiggin for G"],
	["M", "Edit the object aimed at"],
	["B", "Blocky mode (its size: N)"],
	["Ctrl+Z / Ctrl+Y", "Undo / redo (Ctrl+Shift+Z too)"],
	["K / L", "Grow / shrink"],
	["V", "Walk / freecam / orbit cam (WASD, Space / Shift: up / down; orbit: Alt+wheel zooms)"],
	["Shift+RMB (hold)", "Hoist: hold on to the ground and swing up by looking at it (wheel: reach)"],
	["\\", "Respawn"],
	["H", "This list"],
]

func _make_help() -> void:
	help_panel = PanelContainer.new()
	help_panel.set_anchors_and_offsets_preset(Control.PRESET_CENTER)
	help_panel.grow_horizontal = Control.GROW_DIRECTION_BOTH
	help_panel.grow_vertical = Control.GROW_DIRECTION_BOTH
	help_panel.mouse_filter = Control.MOUSE_FILTER_IGNORE
	help_panel.visible = false
	add_child(help_panel)
	var grid := GridContainer.new()
	grid.columns = 2
	grid.add_theme_constant_override("h_separation", 16)
	help_panel.add_child(grid)
	for row in HELP:
		var key := Label.new()
		key.text = row[0]
		var what := Label.new()
		what.text = row[1]
		if row[1].is_empty():
			key.add_theme_color_override("font_color", Color(1.0, 0.86, 0.47))  # a heading
		else:
			key.theme_type_variation = "SmallLabel"
			what.theme_type_variation = "SmallLabel"
			what.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
			what.custom_minimum_size.x = 460
		grid.add_child(key)
		grid.add_child(what)
