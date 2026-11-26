extends Node
## The HUD and the tool wheel in a headset (vr.gd makes this when OpenXR is
## running). A 2D overlay can't be seen in a headset, so both are drawn
## into their own SubViewports and shown on panels in the 3D view:
##
## - The HUD (hud.gd: the bar, status, help, color picker) is a panel on the
##   back of the left wrist. Point the right hand at it and pull the trigger
##   (or pinch) to click: the material slot opens the color picker, the help
##   slot the key list, and the picker's sliders drag.
## - The tool wheel (radial_menu.gd) opens where your hand is, facing you,
##   while you hold B or Y (that hand points), or pinch / pull the left
##   trigger while looking at the left wrist (the right hand points). Move the
##   hand towards a tool (the pointer follows it) or push the thumbstick, and
##   let go to pick it.
##
## Everything is sized in real metres and scaled by XRServer.world_scale, so
## it's the same size to you however big the player is.

const Hud := preload("res://hud.gd")

const HUD_PIXELS := Vector2i(1280, 720)  # the HUD's layout size (hud.gd's DESIGN_HEIGHT)
const WHEEL_PIXELS := 600  # the wheel's viewport (radial_menu.gd lays out for 600 high)
const WHEEL_SIZE := 0.3  # metres across
const LASER_REACH := 1.5  # metres
const PRESS := 0.6  # trigger / pinch value that clicks; let go below RELEASE
const RELEASE := 0.4

## Where the HUD panel sits on the left grip pose, in metres and degrees:
## on the back of the wrist, tilted to face you when you look at it.
## (Tuned on the headset.)
@export var wrist_offset := Vector3(0.0, 0.06, 0.14)
@export var wrist_rotation := Vector3(-55.0, 0.0, 0.0)
@export var wrist_width := 0.32  # metres
## How directly the wrist panel must face your eyes to count as looking at it.
@export var look_dot := 0.6

var vr: Node  # vr.gd
var world: Node  # world.gd
var hud: Hud

var hud_view: SubViewport
var wrist: MeshInstance3D
var wheel_view: SubViewport
var wheel: MeshInstance3D
var laser: MeshInstance3D
var dot: MeshInstance3D

var pointing := false  # the laser is on a HUD panel
var pointer_pixel := Vector2.ZERO
var clicking := false
var wheel_hand: XRController3D = null  # pointing at the wheel, while it's open
var wheel_button := ""  # the action that opened it (letting go picks)
var wheel_opener: XRController3D = null  # whose button that is
var wheel_start := Vector3.ZERO  # the pointing hand when it opened (origin space, metres)
var last_hover := -1

func _init(p_vr: Node) -> void:
	name = "VrUi"
	vr = p_vr
	world = p_vr.world
	hud = world.hud

func _ready() -> void:
	# The HUD into its own viewport, shown on the wrist.
	hud_view = _make_view(HUD_PIXELS)
	hud_view.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	hud.reparent(hud_view, false)
	hud.vr_mode = true
	world._fit_screen()  # (lays the HUD out for hud_size())
	var height := wrist_width * HUD_PIXELS.y / HUD_PIXELS.x
	wrist = _make_panel(hud_view, Vector2(wrist_width, height))
	wrist.name = "WristHud"
	vr.left_grip.add_child(wrist)

	# The wheel into its own, shown where it opens.
	wheel_view = _make_view(Vector2i(WHEEL_PIXELS, WHEEL_PIXELS))
	hud.radial.reparent(wheel_view, false)
	wheel = _make_panel(wheel_view, Vector2(WHEEL_SIZE, WHEEL_SIZE))
	wheel.name = "ToolWheel"
	wheel.visible = false
	vr.origin.add_child(wheel)

	# The laser from the right hand, and a dot where it meets the panel.
	laser = MeshInstance3D.new()
	var beam := BoxMesh.new()
	beam.size = Vector3(0.003, 0.003, 1.0)
	laser.mesh = beam
	laser.material_override = _flat_material(Color(1.0, 0.86, 0.47, 0.8))
	laser.visible = false
	vr.right_aim.add_child(laser)
	dot = MeshInstance3D.new()
	var ball := SphereMesh.new()
	ball.radius = 0.004
	ball.height = 0.008
	dot.mesh = ball
	dot.material_override = _flat_material(Color(1.0, 0.86, 0.47))
	dot.visible = false
	vr.origin.add_child(dot)

func _make_view(p_size: Vector2i) -> SubViewport:
	var view := SubViewport.new()
	view.size = p_size
	view.transparent_bg = true
	view.disable_3d = true
	view.gui_embed_subwindows = true
	view.render_target_update_mode = SubViewport.UPDATE_DISABLED
	add_child(view)
	return view

## A quad showing a viewport, over everything (it's the interface).
func _make_panel(p_view: SubViewport, p_size: Vector2) -> MeshInstance3D:
	var quad := QuadMesh.new()
	quad.size = p_size
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	material.no_depth_test = true
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	material.render_priority = 10
	material.albedo_texture = p_view.get_texture()
	material.texture_filter = BaseMaterial3D.TEXTURE_FILTER_LINEAR
	var panel := MeshInstance3D.new()
	panel.mesh = quad
	panel.material_override = material
	panel.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	return panel

func _flat_material(p_color: Color) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	material.no_depth_test = true
	material.render_priority = 11
	material.albedo_color = p_color
	return material

## The HUD's layout size (world.gd's _fit_screen()).
func hud_size() -> Vector2:
	return Vector2(HUD_PIXELS)

## Each frame, after vr.gd has placed the origin and scale.
func update() -> void:
	var scale: float = XRServer.world_scale
	wrist.position = wrist_offset * scale
	wrist.rotation_degrees = wrist_rotation
	wrist.scale = Vector3.ONE * scale
	if wheel_hand != null:
		_update_wheel(scale)
	else:
		_update_laser(scale)
		_maybe_open_wheel()


# --- Pointing at the HUD ------------------------------------------------------------

func _update_laser(p_scale: float) -> void:
	var hand: XRController3D = vr.right_aim
	var was_pointing := pointing
	pointing = false
	var hit := Vector3.ZERO
	if hand.get_has_tracking_data() and wrist.is_visible_in_tree():
		var from := hand.global_position
		var dir := -hand.global_basis.z.normalized()
		var normal := wrist.global_basis.z.normalized()
		var facing := dir.dot(normal)
		if absf(facing) > 0.0001:
			var t := (wrist.global_position - from).dot(normal) / facing
			if t > 0.0 and t < LASER_REACH * p_scale:
				hit = from + dir * t
				var local := wrist.global_transform.affine_inverse() * hit  # metres on the quad
				var quad := (wrist.mesh as QuadMesh).size
				var uv := Vector2(local.x / quad.x + 0.5, 0.5 - local.y / quad.y)
				var pixel := uv * Vector2(HUD_PIXELS)
				# Only on something there (the panel is mostly see-through).
				if uv.x >= 0.0 and uv.x <= 1.0 and uv.y >= 0.0 and uv.y <= 1.0 and (clicking or hud.has_point(pixel)):
					pointing = true
					if pixel != pointer_pixel or not was_pointing:
						pointer_pixel = pixel
						_send_motion()
					laser.scale = Vector3(p_scale, p_scale, t)  # (the beam is 1 long)
					laser.position = Vector3(0, 0, -t * 0.5)
					dot.global_position = hit
					dot.scale = Vector3.ONE * p_scale
	laser.visible = pointing
	dot.visible = pointing
	var value := hand.get_float("trigger")
	if pointing and not clicking and value >= PRESS:
		clicking = true
		_send_button(true)
	elif clicking and (value <= RELEASE or not pointing):
		clicking = false
		_send_button(false)

func _send_motion() -> void:
	var event := InputEventMouseMotion.new()
	event.position = pointer_pixel
	event.global_position = pointer_pixel
	event.button_mask = MOUSE_BUTTON_MASK_LEFT if clicking else 0
	hud_view.push_input(event, true)

func _send_button(p_down: bool) -> void:
	var event := InputEventMouseButton.new()
	event.position = pointer_pixel
	event.global_position = pointer_pixel
	event.button_index = MOUSE_BUTTON_LEFT
	event.pressed = p_down
	event.button_mask = MOUSE_BUTTON_MASK_LEFT if p_down else 0
	hud_view.push_input(event, true)
	if p_down:
		vr.right_aim.trigger_haptic_pulse("haptic", 0.0, 0.3, 0.03, 0.0)


# --- The tool wheel ----------------------------------------------------------------

## B / Y on either controller (that hand points), or a pinch / the trigger
## on the left hand while looking at the wrist (the right hand points).
func _maybe_open_wheel() -> void:
	if clicking:
		return
	if vr.right_aim.is_button_pressed("by_button"):
		_open_wheel(vr.right_aim, vr.right_aim, "by_button")
	elif vr.left_aim.is_button_pressed("by_button"):
		_open_wheel(vr.left_aim, vr.left_aim, "by_button")
	elif vr.left_aim.get_float("trigger") >= PRESS and _looking_at_wrist():
		_open_wheel(vr.right_aim, vr.left_aim, "trigger")

func _looking_at_wrist() -> bool:
	if not vr.left_grip.get_has_tracking_data():
		return false
	var to_eyes: Vector3 = (vr.head.global_position - wrist.global_position).normalized()
	return to_eyes.dot(wrist.global_basis.z.normalized()) >= look_dot

func _open_wheel(p_hand: XRController3D, p_opener: XRController3D, p_button: String) -> void:
	if world.dragging or world.stroking:
		return
	wheel_hand = p_hand
	wheel_opener = p_opener
	wheel_button = p_button
	var scale: float = XRServer.world_scale
	# At the hand, facing the eyes, upright.
	var at := p_hand.global_position
	var eyes: Vector3 = vr.head.global_position
	var away := (at - eyes).normalized()
	var up: Vector3 = Vector3.UP if absf(away.y) < 0.98 else vr.head.global_basis.y
	var basis := Basis.looking_at(away, up)  # -Z away: the quad's front faces the eyes
	wheel.global_transform = Transform3D(basis.scaled(Vector3.ONE * scale), at)
	wheel_start = vr.origin.to_local(at) / scale
	world.hud.radial.open(world.tool)
	last_hover = -1
	wheel_view.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	wheel.visible = true
	laser.visible = false
	dot.visible = false

func _update_wheel(p_scale: float) -> void:
	var radial = world.hud.radial
	if not radial.visible:
		close_wheel(false)  # (closed from elsewhere: the app lost focus)
		return
	# The pointer follows the hand across the wheel (in its pixels), or the stick.
	var moved: Vector3 = vr.origin.to_local(wheel_hand.global_position) / p_scale - wheel_start
	var right := wheel.basis.x.normalized()
	var up := wheel.basis.y.normalized()
	var pixels_per_metre := WHEEL_PIXELS / WHEEL_SIZE
	var pointer := Vector2(moved.dot(right), -moved.dot(up)) * pixels_per_metre
	var stick := wheel_hand.get_vector2("primary")
	if stick.length() > 0.3:
		pointer = Vector2(stick.x, -stick.y) * radial.POINTER_REACH
	radial.set_pointer(pointer)
	if radial.hover != last_hover:
		last_hover = radial.hover
		if last_hover >= 0:
			wheel_hand.trigger_haptic_pulse("haptic", 0.0, 0.2, 0.02, 0.0)
	var held: bool = wheel_opener.is_button_pressed(wheel_button) if wheel_button == "by_button" \
			else wheel_opener.get_float(wheel_button) > RELEASE
	if not held:
		close_wheel(true)

func close_wheel(p_pick: bool) -> void:
	if wheel_hand == null:
		return
	world._close_radial(p_pick)
	wheel_hand = null
	wheel.visible = false
	wheel_view.render_target_update_mode = SubViewport.UPDATE_DISABLED
