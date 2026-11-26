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
##
## The hands: an XRController3D per hand for its aim (pointing, buttons) and
## one for its grip (the wrist, where things are held). Controllers and
## tracked hands both work: the action map (res://openxr_action_map.tres)
## maps a pinch to "trigger" and a fist to "grip". The HUD and the tool
## wheel are vr_ui.gd's.

const REAL_HEIGHT := 1.7  # metres, the player's height in the world
const VrUi := preload("res://vr_ui.gd")

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
var left_aim: XRController3D
var right_aim: XRController3D
var left_grip: XRController3D
var right_grip: XRController3D
var ui: VrUi
var focussed := false
var _stats_time := 0.0

## Optional overrides for tuning on the headset without rebuilding: a
## ConfigFile at user://vr_tuning.cfg, section [vr], keys render_scale,
## maximum_refresh_rate, log_stats (push it with adb run-as).
const TUNING := "user://vr_tuning.cfg"
var log_stats := true

## Call once the world's camera and player exist. Returns false (and does
## nothing) without a running OpenXR headset.
func start() -> bool:
	xr = XRServer.find_interface("OpenXR") as OpenXRInterface
	if xr == null or not xr.is_initialized():
		return false
	var tuning := ConfigFile.new()
	if tuning.load(TUNING) == OK:
		render_scale = tuning.get_value("vr", "render_scale", render_scale)
		maximum_refresh_rate = tuning.get_value("vr", "maximum_refresh_rate", maximum_refresh_rate)
		log_stats = tuning.get_value("vr", "log_stats", log_stats)
		print("VR: tuning from %s" % TUNING)
	var view: Viewport = world.camera.get_viewport()
	origin = XROrigin3D.new()
	origin.name = "XROrigin"
	head = XRCamera3D.new()
	head.name = "Head"
	head.near = 0.05 * world.player_height / REAL_HEIGHT
	head.far = world.camera.far
	origin.add_child(head)
	left_aim = _hand("LeftAim", &"left_hand", &"aim")
	right_aim = _hand("RightAim", &"right_hand", &"aim")
	left_grip = _hand("LeftGrip", &"left_hand", &"grip")
	right_grip = _hand("RightGrip", &"right_hand", &"grip")
	world.camera.get_parent().add_child(origin)
	head.current = true
	view.use_xr = true
	view.scaling_3d_scale = render_scale
	RenderingServer.viewport_set_measure_render_time(view.get_viewport_rid(), true)
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)  # the headset paces frames
	xr.session_begun.connect(_on_session_begun)
	xr.session_focussed.connect(func() -> void: focussed = true)
	xr.session_visible.connect(func() -> void: focussed = false)
	xr.session_stopping.connect(func() -> void: focussed = false)
	active = true
	ui = VrUi.new(self)
	add_child(ui)
	print("VR: OpenXR running, %s" % xr.get_name())
	return true

## Each frame (world.gd's _process): the origin stands on the player, scaled
## to its size, and the world's camera takes the headset's pose.
func follow(p_feet: Vector3) -> void:
	XRServer.world_scale = world.player_height / REAL_HEIGHT
	origin.global_position = p_feet
	world.camera.global_transform = head.global_transform
	for hand in [left_aim, right_aim, left_grip, right_grip]:
		_find_pose(hand)
	ui.update()
	if log_stats:
		_stats_time += world.get_process_delta_time()
		if _stats_time >= 2.0:
			_stats_time = 0.0
			_log_stats()

## Where a frame's time goes, to the log (adb logcat -s godot).
func _log_stats() -> void:
	var rid: RID = world.camera.get_viewport().get_viewport_rid()
	print("VR stats: %d fps, process %.1f ms, physics %.1f ms, render cpu %.1f ms gpu %.1f ms, draws %d, prims %d" % [
		Engine.get_frames_per_second(),
		Performance.get_monitor(Performance.TIME_PROCESS) * 1000.0,
		Performance.get_monitor(Performance.TIME_PHYSICS_PROCESS) * 1000.0,
		RenderingServer.viewport_get_measured_render_time_cpu(rid),
		RenderingServer.viewport_get_measured_render_time_gpu(rid),
		Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME),
		Performance.get_monitor(Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME)])
	print("VR slowest steps (ms): " + world.take_step_times())

func _hand(p_name: String, p_tracker: StringName, p_pose: StringName) -> XRController3D:
	var hand := XRController3D.new()
	hand.name = p_name
	hand.tracker = p_tracker
	hand.pose = p_pose
	origin.add_child(hand)
	return hand

## A hand's pose by its standard name ("aim"), or by its action's name
## ("aim_pose") if the runtime reports it that way. Poses only appear once
## the session runs, so this checks until one is found.
func _find_pose(p_hand: XRController3D) -> void:
	if p_hand.has_meta("pose_found"):
		return
	var tracker := XRServer.get_tracker(p_hand.tracker) as XRPositionalTracker
	if tracker == null:
		return
	var base := String(p_hand.pose).trim_suffix("_pose")
	for name in [base, base + "_pose"]:
		if tracker.get_pose(name) != null:
			p_hand.pose = name
			p_hand.set_meta("pose_found", true)
			return

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
