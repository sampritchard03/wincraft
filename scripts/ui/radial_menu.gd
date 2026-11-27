extends Control
## The tool wheel (hold Tab): every tool on a ring round the middle of the
## screen. Moving the mouse while it's open moves a pointer from the middle
## (the view doesn't turn); the tool it points towards is highlighted, and
## letting go of Tab (or a click) picks it. Let go with the pointer still in
## the middle to keep the tool you had.
##
## Drawn from res://assets/ui/ (ui_skin.gd): radial_back (the ring),
## slot / slot_hover / slot_active (behind each icon: normal, pointed at,
## the tool in use), radial_pointer, and the tools' icons.

const UiSkin := preload("res://ui_skin.gd")

const RING := 190.0  # the icons' distance from the middle, at full size
const SLOT := 64.0  # an icon's slot, at full size
const BACK := 512.0  # radial_back's size, at full size
const DEADZONE := 40.0  # the pointer must be this far out to point at a tool
const POINTER_REACH := 210.0  # how far out the pointer can go

var tools: Array = []  # the tools in ring order, from the top clockwise
var names: Array = []  # their names
var hints: Array = []  # their keys ("1", "1 x2")
var current := -1  # the tool in use (its index in `tools`)
var pointer := Vector2.ZERO  # from the middle, in full-size pixels
var hover := -1

func _init() -> void:
	name = "RadialMenu"
	set_anchors_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	visible = false

## p_tools in ring order, with their display names and key hints.
func setup(p_tools: Array, p_names: Array, p_hints: Array) -> void:
	tools = p_tools
	names = p_names
	hints = p_hints

func open(p_current_tool: int) -> void:
	current = tools.find(p_current_tool)
	pointer = Vector2.ZERO
	hover = -1
	visible = true
	queue_redraw()

## Mouse movement while open (an InputEventMouseMotion's relative).
func move(p_relative: Vector2) -> void:
	set_pointer(pointer + p_relative)

## Where the pointer is, from the middle (full-size pixels; a headset's
## hand or thumbstick sets it directly, vr_ui.gd).
func set_pointer(p_pointer: Vector2) -> void:
	pointer = p_pointer
	if pointer.length() > POINTER_REACH:
		pointer = pointer.normalized() * POINTER_REACH
	hover = -1
	if pointer.length() >= DEADZONE:
		# The slot whose direction is nearest the pointer's.
		var turn := fposmod(atan2(pointer.x, -pointer.y), TAU)  # 0 at the top, clockwise
		hover = roundi(turn / TAU * tools.size()) % tools.size()
	queue_redraw()

## Closes it: the tool pointed at, or -1 for none.
func close() -> int:
	visible = false
	return tools[hover] if hover >= 0 else -1

func _scale() -> float:
	return clampf(size.y / 600.0, 0.5, 1.5)

func _slot_centre(p_index: int, p_middle: Vector2, p_scale: float) -> Vector2:
	var turn := TAU * p_index / tools.size()
	return p_middle + Vector2(sin(turn), -cos(turn)) * RING * p_scale

func _draw() -> void:
	var s := _scale()
	var middle := size * 0.5
	var back := BACK * s
	draw_texture_rect(UiSkin.texture("radial_back"), Rect2(middle - Vector2(back, back) * 0.5, Vector2(back, back)), false)
	var slot := Vector2(SLOT, SLOT) * s
	var normal := UiSkin.box("slot", UiSkin.SLOT_MARGIN, 0.0)
	var pointed := UiSkin.box("slot_hover", UiSkin.SLOT_MARGIN, 0.0)
	var in_use := UiSkin.box("slot_active", UiSkin.SLOT_MARGIN, 0.0)
	for i in tools.size():
		var grow := 1.18 if i == hover else 1.0
		var rect := Rect2(_slot_centre(i, middle, s) - slot * grow * 0.5, slot * grow)
		draw_style_box(pointed if i == hover else in_use if i == current else normal, rect)
		draw_texture_rect(UiSkin.texture(UiSkin.tool_icon(names[i])), rect.grow(-6.0 * s * grow), false)
	# The middle: what's pointed at (or the tool in use), and its key.
	var shown := hover if hover >= 0 else current
	var font := get_theme_default_font()
	if shown >= 0:
		var icon := 72.0 * s
		draw_texture_rect(UiSkin.texture(UiSkin.tool_icon(names[shown])), Rect2(middle - Vector2(icon, icon * 1.25) * 0.5, Vector2(icon, icon)), false)
		var font_size := roundi(18 * s)
		_centred_text(font, names[shown], middle + Vector2(0, 38 * s), font_size, Color.WHITE)
		var hint: String = str(hints[shown]) if shown < hints.size() else ""
		if hint != "":
			_centred_text(font, "key " + hint, middle + Vector2(0, 58 * s), roundi(13 * s), Color(0.8, 0.82, 0.88))
	var dot := 24.0 * s
	draw_texture_rect(UiSkin.texture("radial_pointer"), Rect2(middle + pointer * s - Vector2(dot, dot) * 0.5, Vector2(dot, dot)), false)

func _centred_text(p_font: Font, p_text: String, p_at: Vector2, p_size: int, p_color: Color) -> void:
	var width := p_font.get_string_size(p_text, HORIZONTAL_ALIGNMENT_LEFT, -1, p_size).x
	var at := p_at - Vector2(width * 0.5, 0)
	draw_string_outline(p_font, at, p_text, HORIZONTAL_ALIGNMENT_LEFT, -1, p_size, maxi(2, p_size / 4), Color.BLACK)
	draw_string(p_font, at, p_text, HORIZONTAL_ALIGNMENT_LEFT, -1, p_size, p_color)
