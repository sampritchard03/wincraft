extends Node
## VR on a standalone headset (Meta Quest 3, through OpenXR). The same
## project runs on desktop: OpenXR is only enabled for Android exports
## (project.godot, xr/openxr/enabled.android), and without a headset start()
## returns false and nothing here runs.
##
## The 3D view (world.tscn's Screen/Viewport, its own world) renders to the
## headset instead of the 320x180 screen: an XROrigin3D with a tracked
## XRCamera3D is added in it, standing on the player (world.player's feet).
## world.gd's `camera` then follows the headset every frame, so everything
## that aims or measures from it keeps working.
##
## Scale: the player's height in voxels is a real person's height
## (REAL_HEIGHT), so world.player_height (shift + wheel on desktop) sets how
## big the world feels: 16 voxels tall, 1 voxel is about 10 cm.

const REAL_HEIGHT := 1.7  # metres, the player's height in the world

## Headset refresh rate to ask for (the highest available up to this).
@export var maximum_refresh_rate := 90
## The 3D view's resolution per eye, of what the headset asks for: the
## terrain is raymarched per pixel, which at full resolution would be far
## too slow. Tuned on the headset.
@export var render_scale := 0.5

var world: Node  # world.gd
var xr: OpenXRInterface
var active := false
var origin: XROrigin3D
var head: XRCamera3D
var focussed := false

## Call once the world's camera and player exist. Returns false (and does
## nothing) without a running OpenXR headset.
func start() -> bool:
	xr = XRServer.find_interface("OpenXR") as OpenXRInterface
	if xr == null or not xr.is_initialized():
		return false
	var view: Viewport = world.camera.get_viewport()
	origin = XROrigin3D.new()
	origin.name = "XROrigin"
	head = XRCamera3D.new()
	head.name = "Head"
	head.near = 0.05 * world.player_height / REAL_HEIGHT
	head.far = world.camera.far
	origin.add_child(head)
	world.camera.get_parent().add_child(origin)
	head.current = true
	view.use_xr = true
	view.scaling_3d_scale = render_scale
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)  # the headset paces frames
	xr.session_begun.connect(_on_session_begun)
	xr.session_focussed.connect(func() -> void: focussed = true)
	xr.session_visible.connect(func() -> void: focussed = false)
	xr.session_stopping.connect(func() -> void: focussed = false)
	active = true
	print("VR: OpenXR running, %s" % xr.get_name())
	return true

## Each frame (world.gd's _process): the origin stands on the player, scaled
## to its size, and the world's camera takes the headset's pose.
func follow(p_feet: Vector3) -> void:
	XRServer.world_scale = world.player_height / REAL_HEIGHT
	origin.global_position = p_feet
	world.camera.global_transform = head.global_transform

## The highest refresh rate up to maximum_refresh_rate, and physics at the
## same rate (so bodies don't judder against the display).
func _on_session_begun() -> void:
	var rate := xr.get_display_refresh_rate()
	for r in xr.get_available_display_refresh_rates():
		if r > rate and r <= maximum_refresh_rate:
			rate = r
	if rate > 0.0:
		if rate != xr.get_display_refresh_rate():
			xr.set_display_refresh_rate(rate)
		Engine.physics_ticks_per_second = roundi(rate)
	print("VR: session begun, %.0f Hz, render scale %.2f" % [rate, render_scale])
