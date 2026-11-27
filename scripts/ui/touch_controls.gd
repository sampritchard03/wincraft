extends Control
## Touch screen controls: a phone or tablet with no headset (world.gd makes
## this when TouchControls.wanted(); try it on a computer with `-- --touch`,
## the mouse standing in for a finger). Drawn over the HUD, in its units
## (laid out for 720 high, scaled to the screen; see hud.gd).
##
## - Left half: a thumbstick wherever your thumb lands. Walks (or flies);
##   push past the ring to sprint.
## - Right half: drag to look; pinch with two fingers for the brush's size
##   (whatever the mouse wheel would do: the physics gun's reach, the box's
##   far corner...; with Ctrl lit, its strength; with Shift, your size).
## - Bottom right: the big Use button (the left mouse button: sculpt with
##   the current tool, shown on it) and the smaller second button (the
##   right one: carve, reverse, freeze...; it says which). Drag a finger
##   from either to aim while you hold it, for strokes. Jump (Up and Down
##   when flying) next to them.
## - Shift / Invert (Alt) / Ctrl: tap to light, tap again to let go; they
##   act as if held with everything you do meanwhile (world._shift_held()...).
## - Top right: the tool wheel (tap it then tap a tool, or hold it and slide
##   onto one), undo / redo (hold to keep going), the material and the menu
##   with everything else the keyboard does.
## - Confirm / Clear / the gizmo's mode, and Turn (holding a body with the
##   physics gun: drag on it to turn the body) show when they mean something.
##
## The buttons send world.gd the very events a mouse and keyboard would
## (`sending` marks them as these: world.gd ignores any other mouse event
## here, so a finger on the screen never sculpts by itself) and answer
## world._button() / _shift_held() / _alt_held() for the held state the
## tools poll. Godot's touch-to-mouse emulation stays on, so the HUD's
## panels (the color picker, brush settings, this menu) take taps as clicks.

const UiSkin := preload("res://scripts/ui/ui_skin.gd")

## How far the view turns for a drag, in mouse pixels per HUD pixel
## (world.mouse_sensitivity radians each): about 160 degrees across half a
## landscape screen.
@export var look_speed := 0.8
const STICK_RADIUS := 80.0
const STICK_DEADZONE := 0.15
const STICK_SPRINT := 1.35  # past the ring by this much (in radii): sprint
const STICK_AREA := 0.45  # of the width from the left: where a thumbstick can start
const TAP_MOVE := 18.0  # a finger that moves less than this (HUD pixels) tapped
const PINCH_STEP := 1.15  # one wheel notch per this much pinch (as the wheel's own step)
const REPEAT_DELAY := 0.4  # holding undo / redo: repeats after this, every REPEAT_EVERY
const REPEAT_EVERY := 0.08

const FILL := Color(0.07, 0.08, 0.11, 0.45)
const FILL_DOWN := Color(0.95, 0.95, 0.97, 0.35)
const EDGE := Color(0.95, 0.95, 0.97, 0.55)
const LIT := Color(1.0, 0.86, 0.47)

var world: Node  # world.gd
var hud: Control  # hud.gd
## True while handing world.gd an event of ours (it ignores other mouse events).
var sending := false
## The modifier toggles.
var shift := false
var alt := false
var ctrl := false
var turning := false  # Turn held (the physics gun)
var _held := {}  # MouseButton -> true, while Use / the second button are held

## The on-screen buttons: dictionaries of
##   id, at (Callable -> centre), r (radius), text (Callable -> String),
##   icon (Callable -> Texture2D or null), shown (Callable -> bool),
##   lit (Callable -> bool), kind ("hold", "tap", "toggle"), look (dragging it
##   aims), repeat (tap repeats while held), down / up (Callables).
var buttons: Array[Dictionary] = []
## Fingers on the screen: index -> {role ("button", "gui", "stick", "look",
## "wheel", "none"), start, at, button (its dictionary), repeat_in}.
var touches := {}
var stick_index := -1
var stick_origin := Vector2.ZERO
var stick := Vector2.ZERO  # in radii, not clamped
var pinch_from := 0.0  # the two look fingers' distance at the last notch (0: not pinching)
var wheel_sticky := false  # the tool wheel opened with a tap: the next touch picks
var menu: PanelContainer
var menu_editor: Array[Button] = []  # the menu's object editor buttons (only while it's open)
var _pressed_actions := {}  # the movement actions the thumbstick holds (not a keyboard's)

## Whether to use them: on phones and tablets, or asked for with --touch.
static func wanted() -> bool:
	if "--touch" in OS.get_cmdline_user_args():
		Input.emulate_touch_from_mouse = true
		return true
	return OS.has_feature("mobile")

func _init(p_world: Node) -> void:
	name = "TouchControls"
	world = p_world
	hud = p_world.hud
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	set_anchors_preset(Control.PRESET_FULL_RECT)

func _ready() -> void:
	Input.emulate_mouse_from_touch = true  # (taps on the HUD's panels: clicks)
	_make_buttons()
	_make_menu()
	hud.tool_pressed.connect(func() -> void: _open_wheel(true))


# --- The buttons -----------------------------------------------------------------------

func _make_buttons() -> void:
	var W := func() -> float: return size.x
	var H := func() -> float: return size.y
	var flying := func() -> bool: return world.move_mode != world.MoveMode.WALK

	# Bottom right: Use, the second button, jump / up and down.
	_add({id = "use", at = func() -> Vector2: return Vector2(W.call() - 150, H.call() - 150), r = 78.0,
			icon = func() -> Texture2D: return UiSkin.texture(UiSkin.tool_icon(world.TOOL_NAMES[world.tool])),
			text = func() -> String: return "", kind = "hold", look = true,
			down = func() -> void: _mouse(MOUSE_BUTTON_LEFT, true),
			up = func() -> void: _mouse(MOUSE_BUTTON_LEFT, false)})
	_add({id = "second", at = func() -> Vector2: return Vector2(W.call() - 310, H.call() - 92), r = 50.0,
			text = _second_label, shown = func() -> bool: return _second_label() != "", kind = "hold", look = true,
			down = func() -> void: _mouse(MOUSE_BUTTON_RIGHT, true),
			up = func() -> void: _mouse(MOUSE_BUTTON_RIGHT, false)})
	_add({id = "jump", at = func() -> Vector2: return Vector2(W.call() - 295, H.call() - 250), r = 46.0,
			text = func() -> String: return "Up" if flying.call() else "Jump", kind = "hold",
			down = func() -> void: Input.action_press("jump"),
			up = func() -> void: Input.action_release("jump")})
	_add({id = "down", at = func() -> Vector2: return Vector2(W.call() - 150, H.call() - 300), r = 42.0,
			text = func() -> String: return "Down", shown = flying, kind = "hold",
			down = func() -> void: Input.action_press("sneak"),
			up = func() -> void: Input.action_release("sneak")})
	_add({id = "turn", at = func() -> Vector2: return Vector2(W.call() - 430, H.call() - 110), r = 44.0,
			text = func() -> String: return "Turn", shown = func() -> bool: return world.gun_body != null or turning,
			kind = "hold", look = true,
			down = func() -> void: turning = true,
			up = func() -> void: turning = false})

	# The modifiers, a column on the right edge.
	for m in [["Ctrl", "ctrl", 480.0], ["Shift", "shift", 420.0], ["Invert", "alt", 360.0]]:
		var label: String = m[0]
		var flag: String = m[1]
		var up_from: float = m[2]
		_add({id = flag, at = func() -> Vector2: return Vector2(W.call() - 50, H.call() - up_from), r = 26.0, pill = true,
				text = func() -> String: return label, kind = "toggle",
				lit = func() -> bool: return get(flag),
				down = func() -> void: set(flag, not get(flag))})

	# What's there to confirm or clear, above Use.
	_add({id = "confirm", at = func() -> Vector2: return Vector2(W.call() - 150, H.call() - 420), r = 32.0,
			text = func() -> String: return "Cut out" if shift else "Confirm", kind = "tap",
			shown = func() -> bool: return world.floating != null or world.tool in [world.Tool.GEOMETRY, world.Tool.SELECT_GEOMETRY],
			down = func() -> void: key(KEY_ENTER, shift)})
	_add({id = "clear", at = func() -> Vector2: return Vector2(W.call() - 225, H.call() - 420), r = 32.0,
			text = func() -> String: return "Clear", kind = "tap", shown = _anything_to_clear,
			down = func() -> void: key(KEY_BACKSPACE, false)})
	_add({id = "gizmo", at = func() -> Vector2: return Vector2(W.call() - 300, H.call() - 420), r = 32.0,
			text = func() -> String: return world.GIZMO_NAMES[world.gizmo_mode],
			shown = func() -> bool: return world.tool == world.Tool.TRANSFORM and world.floating != null, kind = "tap",
			down = func() -> void: key(KEY_T, false)})

	# Top right: menu, tool wheel, redo, undo, material.
	var row := [
		["menu", "Menu", func() -> void: _toggle_menu(), false],
		["tools", "Tools", func() -> void: pass, false],  # (opened in _press: it takes the finger)
		["redo", "Redo", func() -> void: key(KEY_Y, false, true), true],
		["undo", "Undo", func() -> void: key(KEY_Z, false, true), true],
		["next", ">", func() -> void: key(KEY_BRACKETRIGHT, false), true],
		["prev", "<", func() -> void: key(KEY_BRACKETLEFT, false), true],
	]
	for i in row.size():
		var entry: Array = row[i]
		var label: String = entry[1]
		var from_right := 50.0 + i * 68.0
		_add({id = entry[0], at = func() -> Vector2: return Vector2(W.call() - from_right, 96), r = 28.0,
				text = func() -> String: return label, kind = "tap", repeat = entry[3], down = entry[2],
				icon = _row_icon(entry[0])})

## A row button's icon (undo / redo / the tool's own), or none.
func _row_icon(p_id: String) -> Callable:
	match p_id:
		"undo": return func() -> Texture2D: return UiSkin.texture("stat_undo")
		"redo": return func() -> Texture2D: return UiSkin.texture("stat_redo")
		"tools": return func() -> Texture2D: return UiSkin.texture(UiSkin.tool_icon(world.TOOL_NAMES[world.tool]))
	return func() -> Texture2D: return null

func _add(p_button: Dictionary) -> void:
	var defaults := {shown = func() -> bool: return true, lit = func() -> bool: return false,
			icon = func() -> Texture2D: return null, look = false, repeat = false, pill = false,
			up = func() -> void: pass, held_by = -1}
	for k in defaults:
		if not p_button.has(k):
			p_button[k] = defaults[k]
	buttons.append(p_button)

## What the second button (the right mouse button) does with this tool, or
## "" where it does nothing.
func _second_label() -> String:
	var Tool = world.Tool
	var t: int = world.tool
	if shift and world.move_mode == world.MoveMode.WALK:
		return "Hoist"  # (Shift + right button: hold on to the ground)
	if t in world.INVERTIBLE_TOOLS:
		return "Reverse"
	if t == Tool.PHYSICS_GUN:
		return "Freeze"
	if t == Tool.GEOMETRY or t == Tool.SELECT_GEOMETRY:
		return "Remove"
	if t == Tool.BOX:
		return "Cancel" if world.box_dragging else ""
	if t == Tool.SELECT_BOX:
		return "Cancel" if world.box_dragging else "Deselect"
	if t == Tool.SELECT_PAINT:
		return "Deselect"
	if t == Tool.SELECT_FILL:
		return "Similar"
	if t == Tool.EXTRUDE or t == Tool.TRANSFORM:
		return ""
	return "Carve"

func _anything_to_clear() -> bool:
	if world.box_dragging or world.floating != null or world.terrain.get_selected_count() > 0 or not world.selected_bodies.is_empty():
		return true
	return world.tool in [world.Tool.GEOMETRY, world.Tool.SELECT_GEOMETRY] and not world.geo_points.is_empty()

func _button_at(p_at: Vector2) -> Dictionary:
	for b in buttons:
		if b.shown.call() and _button_rect(b).grow(6.0).has_point(p_at):
			return b
	return {}

## A button's touch area: its circle's square, or its pill.
func _button_rect(b: Dictionary) -> Rect2:
	var c: Vector2 = b.at.call()
	var half := Vector2(b.r * 1.6, b.r * 0.85) if b.pill else Vector2(b.r, b.r)
	return Rect2(c - half, half * 2.0)


# --- What the world is told -------------------------------------------------------------

## Whether Use (left) or the second button (right) is held (world._button()).
func is_held(p_button: MouseButton) -> bool:
	return _held.get(p_button, false)

func _send(p_event: InputEvent) -> void:
	sending = true
	world._input(p_event)
	sending = false

func _mouse_event(p_button: MouseButton, p_down: bool) -> InputEventMouseButton:
	var e := InputEventMouseButton.new()
	e.button_index = p_button
	e.pressed = p_down
	e.position = get_viewport_rect().size * 0.5  # (the crosshair)
	e.global_position = e.position
	e.shift_pressed = shift
	e.alt_pressed = alt
	e.ctrl_pressed = ctrl
	e.factor = 1.0
	return e

func _mouse(p_button: MouseButton, p_down: bool) -> void:
	_held[p_button] = p_down
	_send(_mouse_event(p_button, p_down))

## A key pressed and let go, as the keyboard would send it.
func key(p_key: Key, p_shift := false, p_ctrl := false) -> void:
	for down in [true, false]:
		var e := InputEventKey.new()
		e.keycode = p_key
		e.physical_keycode = p_key
		e.pressed = down
		e.shift_pressed = p_shift
		e.ctrl_pressed = p_ctrl
		_send(e)

## One notch of the mouse wheel (up: bigger).
func _wheel(p_up: bool) -> void:
	_send(_mouse_event(MOUSE_BUTTON_WHEEL_UP if p_up else MOUSE_BUTTON_WHEEL_DOWN, true))
	_send(_mouse_event(MOUSE_BUTTON_WHEEL_UP if p_up else MOUSE_BUTTON_WHEEL_DOWN, false))

func _look(p_delta: Vector2) -> void:
	if not hud.radial.visible:
		world.look_input(p_delta * look_speed, shift, turning)


# --- Fingers -------------------------------------------------------------------------------

func _local(p_screen: Vector2) -> Vector2:
	return get_global_transform_with_canvas().affine_inverse() * p_screen

func _input(event: InputEvent) -> void:
	if event is InputEventScreenTouch:
		if event.pressed:
			_press(event.index, _local(event.position), event.position)
		else:
			_release(event.index)
	elif event is InputEventScreenDrag:
		_drag(event.index, _local(event.position))

func _press(p_index: int, p_at: Vector2, p_screen: Vector2) -> void:
	_release(p_index)  # (a lost release)
	var finger := {role = "none", start = p_at, at = p_at, button = {}, repeat_in = REPEAT_DELAY}
	touches[p_index] = finger
	if hud.radial.visible and wheel_sticky:
		finger.role = "wheel"
		_point_wheel(p_at)
		return
	if menu.visible:
		if menu.get_global_rect().has_point(p_screen):
			finger.role = "gui"
		else:
			menu.visible = false  # (a tap outside closes it)
		return
	var b := _button_at(p_at)
	if not b.is_empty():
		finger.role = "button"
		finger.button = b
		if b.id == "tools":
			if _open_wheel(false):
				finger.role = "wheel"
			return
		match b.kind:
			"hold":
				if b.held_by < 0:
					b.held_by = p_index
					b.down.call()
			_:
				b.down.call()
	elif _on_gui(p_screen):
		finger.role = "gui"  # (Godot's emulated mouse clicks it)
	elif p_at.x < size.x * STICK_AREA and p_at.y > 140.0 and stick_index < 0:
		finger.role = "stick"
		stick_index = p_index
		stick_origin = p_at
		stick = Vector2.ZERO
	else:
		finger.role = "look"
		var other := _other_look(p_index)
		if other >= 0:
			pinch_from = p_at.distance_to(touches[other].at)

func _drag(p_index: int, p_at: Vector2) -> void:
	if not touches.has(p_index):
		return
	var finger: Dictionary = touches[p_index]
	var delta: Vector2 = p_at - finger.at
	finger.at = p_at
	match finger.role:
		"stick":
			stick = (p_at - stick_origin) / STICK_RADIUS
		"look":
			var other := _other_look(p_index)
			if other >= 0 and pinch_from > 0.0:
				_pinch(p_at.distance_to(touches[other].at))
			else:
				_look(delta)
		"button":
			if finger.button.look and finger.button.held_by == p_index:
				_look(delta)
		"wheel":
			if wheel_sticky or finger.start.distance_to(p_at) > TAP_MOVE:
				_point_wheel(p_at)

func _release(p_index: int) -> void:
	if not touches.has(p_index):
		return
	var finger: Dictionary = touches[p_index]
	touches.erase(p_index)
	match finger.role:
		"stick":
			if stick_index == p_index:
				stick_index = -1
				stick = Vector2.ZERO
		"look":
			pinch_from = 0.0
		"button":
			var b: Dictionary = finger.button
			if b.kind == "hold" and b.held_by == p_index:
				b.held_by = -1
				b.up.call()
		"wheel":
			_wheel_release(finger)

## The other finger looking, if two are (a pinch), else -1.
func _other_look(p_index: int) -> int:
	for i in touches:
		if i != p_index and touches[i].role == "look":
			return i
	return -1

## Two fingers apart (or together): a wheel notch per PINCH_STEP.
func _pinch(p_distance: float) -> void:
	if pinch_from <= 0.0:
		pinch_from = p_distance
		return
	while p_distance > pinch_from * PINCH_STEP:
		pinch_from *= PINCH_STEP
		_wheel(true)
	while p_distance < pinch_from / PINCH_STEP:
		pinch_from /= PINCH_STEP
		_wheel(false)

func _on_gui(p_screen: Vector2) -> bool:
	if hud.has_point(p_screen):
		return true
	if world.editor != null and world.editor.active and world.editor.has_point(p_screen):
		return true
	return world.file_dialog != null and is_instance_valid(world.file_dialog) and world.file_dialog.visible

## Lets go of everything (the app went to the background: its fingers' releases won't come).
func release_all() -> void:
	for i in touches.keys():
		_release(i)
	stick_index = -1
	stick = Vector2.ZERO
	_apply_stick()


# --- The tool wheel ----------------------------------------------------------------------

## p_sticky: opened with a tap (the HUD's tool slot): stays open for the next touch.
func _open_wheel(p_sticky: bool) -> bool:
	if hud.radial.visible or world.dragging or world.stroking or world.gizmo_axis >= 0 or world.gun_body != null:
		return false
	hud.radial.open(world.tool)
	wheel_sticky = p_sticky
	return true

func _wheel_middle() -> Vector2:
	return hud.radial.size * 0.5

## Points at the tool towards a finger (from the wheel's middle).
func _point_wheel(p_at: Vector2) -> void:
	hud.radial.set_pointer((p_at - _wheel_middle()) / hud.radial._scale())

func _wheel_release(p_finger: Dictionary) -> void:
	if not hud.radial.visible:
		return
	var moved: bool = p_finger.start.distance_to(p_finger.at) > TAP_MOVE
	if not wheel_sticky and not moved:
		wheel_sticky = true  # tapped Tools: tap a tool next
		return
	var reach: float = (hud.radial.RING + hud.radial.SLOT) * hud.radial._scale()
	var on_ring: bool = p_finger.at.distance_to(_wheel_middle()) <= reach
	world._close_radial(on_ring and hud.radial.hover >= 0)
	wheel_sticky = false


# --- The menu ------------------------------------------------------------------------------

## Everything else the keyboard does: [label, key, shift, ctrl, stays open].
const MENU := [
	["Color picker", KEY_C], ["Brush settings", KEY_N], ["Blocky mode", KEY_B], ["Pick material", MOUSE_BUTTON_MIDDLE],
	["Symmetry here", KEY_E], ["No symmetry", KEY_Q], ["Mirror / radial", KEY_Z], ["Radial +", KEY_EQUAL, false, false, true],
	["Radial -", KEY_MINUS, false, false, true], ["Selection to object", KEY_P], ["Copy", KEY_C, false, true], ["Paste", KEY_V, false, true],
	["Throw ball", KEY_F, false, false, true], ["Drop creature", KEY_G], ["Frog / chiggin", KEY_G, true], ["Remove object", KEY_X],
	["Edit object", KEY_M], ["Walk / fly / orbit", KEY_V], ["Grow", KEY_K, false, false, true], ["Shrink", KEY_L, false, false, true],
	["Respawn", KEY_BACKSLASH],
]
## The object editor's, shown while it's open (object_editor.gd).
const MENU_EDITOR := [
	["Key the pose", KEY_I], ["Delete keys", KEY_U], ["Play / stop", KEY_Y],
	["Previous frame", KEY_COMMA, false, false, true], ["Next frame", KEY_PERIOD, false, false, true],
]

func _make_menu() -> void:
	menu = PanelContainer.new()
	menu.name = "TouchMenu"
	menu.visible = false
	add_child(menu)
	var grid := GridContainer.new()
	grid.columns = 4
	grid.add_theme_constant_override("h_separation", 8)
	grid.add_theme_constant_override("v_separation", 8)
	menu.add_child(grid)
	for entry in MENU:
		grid.add_child(_menu_button(entry))
	for entry in MENU_EDITOR:
		var button := _menu_button(entry)
		button.modulate = LIT
		menu_editor.append(button)
		grid.add_child(button)

func _menu_button(p_entry: Array) -> Button:
	var button := Button.new()
	button.text = p_entry[0]
	button.focus_mode = Control.FOCUS_NONE
	button.custom_minimum_size = Vector2(170, 52)
	var what = p_entry[1]
	var with_shift: bool = p_entry[2] if p_entry.size() > 2 else false
	var with_ctrl: bool = p_entry[3] if p_entry.size() > 3 else false
	var stays: bool = p_entry[4] if p_entry.size() > 4 else false
	button.pressed.connect(func() -> void:
		if not stays:
			menu.visible = false
		if p_entry[0] == "Pick material":  # (the middle mouse button)
			_send(_mouse_event(MOUSE_BUTTON_MIDDLE, true))
			_send(_mouse_event(MOUSE_BUTTON_MIDDLE, false))
		else:
			key(what, with_shift, with_ctrl))
	return button

func _toggle_menu() -> void:
	menu.visible = not menu.visible
	if menu.visible:
		var editing: bool = world.editor != null and world.editor.active
		for button in menu_editor:
			button.visible = editing
		menu.reset_size()
		menu.position = (size - menu.size) * 0.5

## Android's back: closes the wheel, the menu or a panel. False if nothing was open.
func go_back() -> bool:
	if hud.radial.visible:
		world._close_radial(false)
		wheel_sticky = false
	elif menu.visible:
		menu.visible = false
	elif world.color_panel.visible:
		world.toggle_color_panel()
	elif world.brush_panel.visible:
		world.toggle_brush_panel()
	elif world.editor != null and world.editor.active:
		world.editor.close()
	else:
		return false
	return true

## The tool hints in touch words (world._tool_hint()).
const REWORD := [
	["move the mouse", "drag"], ["Shift + click", "Shift + Use"], ["Ctrl + click", "Ctrl + Use"], ["Ctrl + clicked", "Ctrl + Used"],
	["Shift+Enter", "Shift + Confirm"], ["Enter", "Confirm"], ["Backspace", "Clear"], ["LMB", "Use"], ["RMB", "the second button"],
	["the other button", "the second button"], ["Alt", "Invert"], ["wheel", "pinch"], ["N: ", "Menu > Brush settings: "],
	["; N: brush settings", ""], ["T switches", "tap its button to switch"], ["(V switches)", "(Menu > Walk / fly / orbit)"],
	["Shift+G switches", "Menu > Frog / chiggin"],
]

func reword(p_hint: String) -> String:
	for pair in REWORD:
		p_hint = p_hint.replace(pair[0], pair[1])
	return p_hint


# --- Each frame ----------------------------------------------------------------------------

func _process(delta: float) -> void:
	_apply_stick()
	# Undo / redo held: keep going.
	for i in touches:
		var finger: Dictionary = touches[i]
		if finger.role == "button" and finger.button.repeat:
			finger.repeat_in -= delta
			if finger.repeat_in <= 0.0:
				finger.repeat_in = REPEAT_EVERY
				finger.button.down.call()
	# Let go of what isn't shown any more (Turn when the body's dropped).
	if turning and world.gun_body == null and not _turn_held():
		turning = false
	queue_redraw()

func _turn_held() -> bool:
	for b in buttons:
		if b.id == "turn":
			return b.held_by >= 0
	return false

## The thumbstick as the movement actions (world.gd's _walk / _fly read them).
func _apply_stick() -> void:
	var v := stick if stick.length() > STICK_DEADZONE else Vector2.ZERO
	_set_action("forward", maxf(-v.y, 0.0))
	_set_action("back", maxf(v.y, 0.0))
	_set_action("left", maxf(-v.x, 0.0))
	_set_action("right", maxf(v.x, 0.0))
	_set_action("sprint", 1.0 if stick.length() >= STICK_SPRINT else 0.0)

func _set_action(p_action: StringName, p_strength: float) -> void:
	p_strength = minf(p_strength, 1.0)
	if p_strength > 0.0:
		Input.action_press(p_action, p_strength)
		_pressed_actions[p_action] = true
	elif _pressed_actions.has(p_action):
		Input.action_release(p_action)
		_pressed_actions.erase(p_action)


# --- Drawing -------------------------------------------------------------------------------

func _draw() -> void:
	var font := get_theme_default_font()
	var dim := 0.4 if hud.radial.visible else 1.0
	# The thumbstick: where it is, or a faint hint of where to put a thumb.
	if stick_index >= 0:
		var sprinting := stick.length() >= STICK_SPRINT
		draw_circle(stick_origin, STICK_RADIUS, FILL)
		draw_arc(stick_origin, STICK_RADIUS, 0.0, TAU, 48, LIT if sprinting else EDGE, 3.0, true)
		draw_circle(stick_origin + stick.limit_length(1.0) * STICK_RADIUS, STICK_RADIUS * 0.42, FILL_DOWN)
	else:
		var hint := Vector2(170, size.y - 170)
		draw_arc(hint, STICK_RADIUS, 0.0, TAU, 48, Color(EDGE, 0.18 * dim), 2.0, true)
		draw_circle(hint, STICK_RADIUS * 0.42, Color(FILL, 0.2 * dim))
	for b in buttons:
		if b.shown.call():
			_draw_button(b, font, dim)

func _draw_button(b: Dictionary, p_font: Font, p_dim: float) -> void:
	var c: Vector2 = b.at.call()
	var down: bool = b.held_by >= 0 or _tapping(b)
	var lit: bool = b.lit.call()
	var fill := FILL_DOWN if down else (Color(LIT, 0.45) if lit else FILL)
	var edge := LIT if lit else EDGE
	fill.a *= p_dim
	edge.a *= p_dim
	if b.pill:
		var rect := _button_rect(b)
		var style := StyleBoxFlat.new()
		style.bg_color = fill
		style.border_color = edge
		style.set_border_width_all(2)
		style.set_corner_radius_all(int(rect.size.y * 0.5))
		style.anti_aliasing = true
		draw_style_box(style, rect)
	else:
		draw_circle(c, b.r, fill)
		draw_arc(c, b.r, 0.0, TAU, 48, edge, 3.0 if b.r > 40.0 else 2.0, true)
	var icon: Texture2D = b.icon.call()
	var text: String = b.text.call()
	if icon != null:
		var s: float = b.r * (1.0 if text.is_empty() else 0.8)
		var at := c - Vector2(s, s) * 0.5 - (Vector2(0, b.r * 0.18) if not text.is_empty() else Vector2.ZERO)
		draw_texture_rect(icon, Rect2(at, Vector2(s, s)), false, Color(1, 1, 1, p_dim))
	if not text.is_empty():
		var font_size := 15 if b.r >= 40.0 else 13
		var y: float = c.y + (b.r * 0.55 if icon != null else font_size * 0.35)
		var width := p_font.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x
		var at := Vector2(c.x - width * 0.5, y)
		draw_string_outline(p_font, at, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, 4, Color(0, 0, 0, p_dim))
		draw_string(p_font, at, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, Color(0.95, 0.95, 0.97, p_dim))

## A tap or toggle button with a finger on it (drawn pressed).
func _tapping(b: Dictionary) -> bool:
	for i in touches:
		if touches[i].role in ["button", "wheel"] and is_same(touches[i].button, b):
			return true
	return false
