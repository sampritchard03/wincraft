extends PanelContainer
## Brush settings (N): how every brush is shaped, and Flatten's, Layer's and
## the stroke's own settings. Kept between sessions (world.gd,
## save_brush_settings()).
##
##   Falloff     how a brush fades towards its edge: smooth (Draw / Carve
##               build whole balls), linear, sharp, round, constant
##   Shape       sphere or cube (along the world's axes; an object's own)
##   Blocky      results snap to blocks this big (B turns it on and off)
##   Flatten     both ways, fill only (raise up to the plane) or scrape only
##               (cut down to it); to the face under the brush, a plane
##               facing the view, or one square to an axis
##   Layer       how high Layer raises the ground
##   Lazy mouse  the brush trails the crosshair on a string, for steady lines
##   Smooth wheel  the wheel eases the radius and strength
##   Delete all objects (asks again: no undo)

const FALLOFFS := ["Smooth", "Linear", "Sharp", "Round", "Constant"]
const SHAPES := ["Sphere", "Cube"]
const BLOCKY := [0, 2, 3, 4, 6, 8]
const FLATTEN_MODES := ["Both ways", "Fill only (raise up to it)", "Scrape only (cut down to it)"]
const FLATTEN_PLANES := ["The face under the brush", "Facing the view", "Axis: x (a wall)", "Axis: y (a floor)", "Axis: z (a wall)"]

var world: Node  # world.gd
var updating := false
var falloff: OptionButton
var shape: OptionButton
var blocky: OptionButton
var flatten_mode: OptionButton
var flatten_plane: OptionButton
var layer_height: HSlider
var layer_label: Label
var lazy: HSlider
var lazy_label: Label
var smooth_wheel: CheckBox
const DELETE_ALL_TEXT := "Delete all objects (balls, creatures, pieces)"
var delete_all: Button
var delete_armed := false


func _init(p_world: Node) -> void:
	world = p_world
	name = "BrushPanel"
	visible = false
	var box := VBoxContainer.new()
	add_child(box)
	var title := Label.new()
	title.text = "Brush settings (N closes)"
	box.add_child(title)
	falloff = _options(box, "Falloff", FALLOFFS, func(i: int) -> void: world.terrain.brush_falloff = i)
	shape = _options(box, "Shape", SHAPES, func(i: int) -> void:
		world.terrain.brush_shape = i
		world.update_brush_preview_shape())
	var blocky_names := []
	for size in BLOCKY:
		blocky_names.append("Off" if size == 0 else "%d voxels" % size)
	blocky = _options(box, "Blocky (B)", blocky_names, func(i: int) -> void:
		world.terrain.blocky_size = BLOCKY[i]
		if BLOCKY[i] >= 2:
			world.blocky_size_on = BLOCKY[i])
	flatten_mode = _options(box, "Flatten", FLATTEN_MODES, func(i: int) -> void: world.terrain.flatten_mode = i)
	flatten_plane = _options(box, "Flatten to", FLATTEN_PLANES, func(i: int) -> void: world.flatten_plane = i)
	layer_label = Label.new()
	box.add_child(layer_label)
	layer_height = _slider(box, 0.5, 7.5, 0.5, func(v: float) -> void:
		world.terrain.brush_layer_height = v
		layer_label.text = "Layer height: %.1f" % v)
	lazy_label = Label.new()
	box.add_child(lazy_label)
	lazy = _slider(box, 0.0, 1.0, 0.05, func(v: float) -> void:
		world.lazy_mouse = v
		lazy_label.text = "Lazy mouse: %s" % ("off" if v <= 0.0 else "%d%%" % roundi(v * 100.0)))
	smooth_wheel = CheckBox.new()
	smooth_wheel.text = "Smooth wheel (eases radius and strength)"
	smooth_wheel.focus_mode = Control.FOCUS_NONE
	smooth_wheel.toggled.connect(func(on: bool) -> void:
		if not updating:
			world.smooth_wheel = on
			world.save_brush_settings())
	box.add_child(smooth_wheel)
	var keys := Label.new()
	keys.text = "Alt: any brush the other way.  Shift + click: a straight\nstroke from where the last ended (grab: pulls it here)."
	keys.modulate = Color(1, 1, 1, 0.7)
	box.add_child(keys)
	box.add_child(HSeparator.new())
	# Asks again first: there's no undo for it.
	delete_all = Button.new()
	delete_all.focus_mode = Control.FOCUS_NONE
	delete_all.text = DELETE_ALL_TEXT
	delete_all.pressed.connect(func() -> void:
		if not delete_armed:
			delete_armed = true
			delete_all.text = "Click again: delete them all (no undo)"
			get_tree().create_timer(3.0).timeout.connect(func() -> void:
				delete_armed = false
				delete_all.text = DELETE_ALL_TEXT)
			return
		delete_armed = false
		var count: int = world.delete_all_objects()
		delete_all.text = "Deleted %d object%s" % [count, "" if count == 1 else "s"])
	box.add_child(delete_all)


func _options(p_box: VBoxContainer, p_label: String, p_items: Array, p_chosen: Callable) -> OptionButton:
	var row := HBoxContainer.new()
	p_box.add_child(row)
	var label := Label.new()
	label.text = p_label
	label.custom_minimum_size.x = 96
	row.add_child(label)
	var options := OptionButton.new()
	options.focus_mode = Control.FOCUS_NONE
	options.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	for item in p_items:
		options.add_item(item)
	options.item_selected.connect(func(i: int) -> void:
		if not updating:
			p_chosen.call(i)
			world.save_brush_settings())
	row.add_child(options)
	return options


func _slider(p_box: VBoxContainer, p_min: float, p_max: float, p_step: float, p_changed: Callable) -> HSlider:
	var slider := HSlider.new()
	slider.min_value = p_min
	slider.max_value = p_max
	slider.step = p_step
	slider.custom_minimum_size.x = 280
	slider.focus_mode = Control.FOCUS_NONE
	slider.value_changed.connect(func(v: float) -> void:
		p_changed.call(v)
		if not updating:
			world.save_brush_settings())
	p_box.add_child(slider)
	return slider


## Shows the settings as they are now (B, or a loaded file, may have changed them).
func refresh() -> void:
	updating = true
	var t: SculptTerrain = world.terrain
	falloff.select(t.brush_falloff)
	shape.select(t.brush_shape)
	var at := BLOCKY.find(t.blocky_size)
	blocky.select(at if at >= 0 else 1)
	flatten_mode.select(t.flatten_mode)
	flatten_plane.select(world.flatten_plane)
	layer_height.value = t.brush_layer_height
	layer_label.text = "Layer height: %.1f" % t.brush_layer_height
	lazy.value = world.lazy_mouse
	lazy_label.text = "Lazy mouse: %s" % ("off" if world.lazy_mouse <= 0.0 else "%d%%" % roundi(world.lazy_mouse * 100.0))
	smooth_wheel.button_pressed = world.smooth_wheel
	updating = false
	reset_size()
	var area: Vector2 = get_parent().size if get_parent() is Control else Vector2(1280, 720)
	position = Vector2(area.x - size.x - 12.0, 64.0)
