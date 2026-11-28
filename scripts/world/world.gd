extends Node

## Native terrain (gdextension/src/sculpt_terrain.cpp): endless floating
## islands or Minecraft-like ground (its world_type), edited with
## apply_brush(), saved to its save_path, and drawn by the raymarching shader
## in terrain.gdshader. World settings are on that node.
@export var terrain : SculptTerrain
## Shows the 320x180 SubViewport the 3D view renders into, scaled up to fill
## the window with nearest-neighbour filtering (chunky pixels). The HUD is
## drawn separately at full resolution.
@export var screen : SubViewportContainer
@export var camera : Camera3D
@export var forg_scene : PackedScene
## What G drops (Shift+G switches): a frog or a chiggin.
const CHIGGIN_SCENE := preload("res://scenes/entities/chiggin.tscn")
const CREATURE_NAMES := ["Frog", "Chiggin"]
var creature := 0
## A message shown in the status panel for a moment (notice_until, msec).
var notice := ""
var notice_until := 0
## Half-resolution pass that tells the terrain shader where rays can start
## (see terrain_prepass.gdshader). Its camera follows `camera`. Off unless
## use_prepass: at 320x180 it cost more than it saved (about 0.6 ms a frame
## on an Intel HD 4400), as the full image re-marches from where it stops.
@export var use_prepass := false
@export var prepass_viewport : SubViewport
@export var prepass_camera : Camera3D
## Sphere showing where and how big the brush is.
@export var brush_preview : MeshInstance3D
@export var info_label : Label

@export_group("Controls")
@export var mouse_sensitivity := 0.005
## Flying speed in voxels per second (sprint is 4x).
@export var move_speed := 30.0
@export_range(1.0, 48.0) var brush_radius := 6.0
## Draw / carve: how fast the surface moves at the brush centre, in voxels per
## second. Smooth / flatten: how fast it blends towards the target.
@export_range(0.5, 64.0) var brush_strength := 8.0
## How far away you can sculpt, in voxels.
@export var reach := 300.0
## Player height in voxels (the camera is at the top).
@export var player_height := 16.0
## Player width: the radius of its capsule, in voxels.
@export var player_radius := 4.0
## Voxels per second squared.
@export var gravity := 120.0
## Upward speed a jump starts with, voxels per second.
@export var jump_speed := 69.0  # (about 20 voxels high, as before gravity was halved)


## The player: a capsule of spheres that slides through the terrain's
## density field (DensityCharacter3D, no collision meshes), its feet at its
## origin. Balls and frogs bounce off it; it can stand on them.
var player: DensityCharacter3D
## Physics objects (physics_objects.gd): balls thrown with F, and loose
## pieces (P, Ctrl+O, cut off), saved with the world.
const PhysicsObjects := preload("res://scripts/objects/physics_objects.gd")
var objects: PhysicsObjects
## Frogs and chiggins (creatures.gd): saved with the world, and every one
## of a kind shares its shape and animations.
const Creatures := preload("res://scripts/objects/entities/creatures.gd")
var creatures: Creatures
## World features (features.gd): trees, mushrooms... given to the terrain's
## generator before the world opens.
const Features := preload("res://scripts/features/features.gd")
## The object editor (M on a body: sculpt it, animate its pose keys; object_editor.gd).
const ObjectEditor := preload("res://scripts/editor/object_editor.gd")
var editor: ObjectEditor
## Multiplayer (network.gd): hosting or joining, from the command line.
const Network := preload("res://scripts/world/network.gd")
var network: Network
## VR (vr.gd): a headset, if one is running (Meta Quest through OpenXR).
const VR := preload("res://scripts/vr/vr.gd")
var vr: VR
## Touch screen controls (touch_controls.gd): a phone or tablet without a
## headset (or --touch on the command line, to try them with the mouse).
const TouchControls := preload("res://scripts/ui/touch_controls.gd")
var touch: TouchControls
## The HUD (hud.gd): the bar of tool / material / brush slots, the status
## panel, the help list (H) and the tool wheel (hold Tab), all skinned from
## res://assets/ui/.
const Hud := preload("res://scripts/ui/hud.gd")
const BrushPanel := preload("res://scripts/ui/brush_panel.gd")
var hud: Hud
## The tool wheel's ring, from the top clockwise, with each tool's key (the
## number of presses for a key with several).
const WHEEL_TOOLS := [Tool.DRAW, Tool.INFLATE, Tool.CLAY_STRIPS, Tool.LAYER, Tool.SMOOTH, Tool.PINCH, Tool.FLATTEN, Tool.GRAB, Tool.ROTATE,
		Tool.SCALE, Tool.SNAKE_HOOK, Tool.PAINT, Tool.CLONE, Tool.PAINT_FILL, Tool.SELECT_PAINT, Tool.SELECT_FILL, Tool.SELECT_GEOMETRY, Tool.SELECT_BOX,
		Tool.EXTRUDE, Tool.TRANSFORM, Tool.PHYSICS_GUN, Tool.GEOMETRY, Tool.BOX]
## The number keys: each picks its first tool, then the next on each press.
## (The wheel's key hints are worked out from these, so they can't fall out
## of step with it.)
const KEY_TOOLS := {
	KEY_1: [Tool.DRAW, Tool.INFLATE, Tool.CLAY_STRIPS, Tool.LAYER],
	KEY_2: [Tool.SMOOTH, Tool.PINCH],
	KEY_3: [Tool.FLATTEN],
	KEY_4: [Tool.GRAB, Tool.ROTATE, Tool.SCALE],
	KEY_5: [Tool.SNAKE_HOOK],
	KEY_6: [Tool.PAINT, Tool.CLONE, Tool.PAINT_FILL],
	KEY_7: [Tool.SELECT_PAINT, Tool.SELECT_FILL, Tool.SELECT_GEOMETRY, Tool.SELECT_BOX],
	KEY_8: [Tool.EXTRUDE, Tool.TRANSFORM],
	KEY_9: [Tool.PHYSICS_GUN],
	KEY_0: [Tool.GEOMETRY, Tool.BOX],
}


## The wheel's key hint for each of WHEEL_TOOLS ("1", "1 x2"; "" for none).
func _wheel_keys() -> Array:
	var hints: Array = []
	for t in WHEEL_TOOLS:
		var hint := ""
		for k in KEY_TOOLS:
			var at: int = KEY_TOOLS[k].find(t)
			if at >= 0:
				hint = OS.get_keycode_string(k) + ("" if at == 0 else " x%d" % (at + 1))
		hints.append(hint)
	return hints
var block_icons := {}  # material -> ImageTexture, for the HUD
## Sun shadows from shadow maps (sun_shadows.gd).
const SunShadows := preload("res://scripts/world/sun_shadows.gd")
var sun_shadows: SunShadows
## The ball or frog the brush is on this frame (null: the terrain).
var target_body: DensityBody3D = null
## A body being sculpted is held still for the stroke (it would roll away
## from the brush as its shape changed): the body and whether it was frozen.
var held_body: DensityBody3D = null
var held_was_frozen := false
## Grab / snake hook on a body rather than the terrain.
var drag_body: DensityBody3D = null
## Physics gun: the body held (null: none), how far in front of the camera,
## and the point of it that was grabbed (in its own space).
var gun_body: DensityBody3D = null
var gun_distance := 0.0
var gun_point := Vector3.ZERO
## Holding Tab while the physics gun holds a body turns it with the mouse
## (about the view's up and right axes); from then on it's held at that
## turn (gun_basis, world space) rather than left to spin.
var gun_turned := false
var gun_basis := Basis.IDENTITY

## How you get about (V cycles): walking (the player's capsule, gravity,
## collisions), a free camera flying through everything, or one orbiting a
## point (WASD / Space / Shift move the point, Alt + wheel the distance).
## Flying, the capsule rides along under the camera, not touching bodies.
enum MoveMode { WALK, FREECAM, ORBIT }
const MOVE_MODE_NAMES := ["Walking", "Freecam", "Orbit cam"]
var move_mode := MoveMode.WALK
var orbit_pivot := Vector3.ZERO
var orbit_distance := 60.0

## Hoist (Shift + RMB, any tool, walking): holds on to the ground where you
## clicked. While held you're drawn to the point hoist_distance back along
## your line of sight to it, so looking down at it lifts you up, looking
## around swings you round it, and the wheel lets out or takes in the reach.
var hoisting := false
var hoist_anchor := Vector3.ZERO
var hoist_distance := 0.0
## How quickly you're drawn there (per second), and at most how fast, in
## walking speeds.
const HOIST_PULL := 8.0
const HOIST_MAX_SPEED := 6.0
## Select fill acts once per click.
var fill_clicked := false
## Physics objects picked with the selection tools (clicking one; right
## click lets it go). Never at the same time as selected ground: selecting
## either lets the other go. P turns them into ground, Ctrl+P saves one.
var selected_bodies: Array[DensityBody3D] = []
var body_clicked := false
## The file dialog open for Ctrl+P / Ctrl+O, if any.
var file_dialog: FileDialog
var held_shape_before: DensityShape  # the held body's shape before the stroke (for undo)
## How far the camera trails behind the player's height (see _process).
var camera_lag := 0.0
## Flatten: the plane the current stroke flattens to (set when it starts).
var flatten_point := Vector3.ZERO
var flatten_normal := Vector3.ZERO
var flatten_locked := false
const FLATTEN_NEW_FACE := 50.0  # degrees: a face turned this far from the one flattened is another one
## Snake hook: where the last tendril ended (Shift + click pulls a straight
## one from there to where you click), or null.
var snake_last = null

## Undo history over the terrain and the bodies, newest last: {"terrain":
## true} (SculptTerrain.undo() / redo()), a body sculpt {"body", "before",
## "after", "parts" (bodies split off), "part_shapes", "part_transform"}, a
## selection change {"selection": true}, or a gizmo move of the floating
## piece {"floating", "before", "after"} (transforms). Any of them may carry
## "sel_before" / "sel_after" (SculptTerrain.get_selection_state()) when the
## selection changed with it.
const MAX_HISTORY := 400
var history: Array[Dictionary] = []
var future: Array[Dictionary] = []
var terrain_serial := 0  # SculptTerrain.get_undo_serial() already in history
var sel_state: Array = []  # the selection as history last saw it
var sel_serial := -1  # SculptTerrain.get_selection_serial() of sel_state
var lift_sel_state: Array = []  # the selection before the floating piece was lifted

## Transform (8, twice): the selection lifted out of the terrain as a frozen
## body (`floating`), moved, turned and scaled with a gizmo (T switches),
## put back with Enter (or another tool). Ctrl+C copies the selection,
## Ctrl+V pastes the copy as a new floating one.
enum Gizmo { MOVE, ROTATE, SCALE }
const GIZMO_NAMES := ["Move", "Rotate", "Scale"]
var gizmo_mode := Gizmo.MOVE
var floating: DensityBody3D = null
## Transform on an object or creature (clicked with Transform, no selection
## needed): the gizmo moves and turns it (held still while dragged). One undo
## step per drag: {"moved": body, "before", "after"} (transforms).
var gizmo_body: DensityBody3D = null
var gizmo_body_frozen := false  # its freeze before the drag
var gizmo_handles: Array[MeshInstance3D] = []  # x, y, z
var gizmo_axis := -1  # the handle being dragged (3: free), -1 none
var gizmo_hover := -1
var gizmo_start := Transform3D()  # floating's when the drag began
var gizmo_start_hit := Vector3.ZERO
var gizmo_start_param := 0.0
var gizmo_plane_normal := Vector3.ZERO
var clipboard: DensityShape = null
var clipboard_center := Vector3.ZERO

## Extrude (8): drag on the selection; it sweeps along the surface's normal.
var extruding := false
var extrude_origin := Vector3.ZERO
var extrude_dir := Vector3.UP
var extrude_last := -1.0
var extrude_whole := false  # Shift when it began: the whole selection, not just the brush's reach

## Geometry (0) and Select geometry (7, three times): points placed with the
## left button (the wheel sets how far in front), dragged by holding on one,
## removed with the right button; the solid round them is previewed. Enter
## builds it (Geometry, of the current material) or selects the ground in it.
var geo_points: Array[Vector3] = []
var geo_markers: Array[MeshInstance3D] = []
var geo_cursor: MeshInstance3D
var geo_preview: DensityBody3D = null
var geo_distance := 40.0
var geo_drag := -1
var geo_drag_distance := 0.0

## Box (0, twice): press on the ground where one corner goes and drag to
## the opposite one; letting go builds it (upright, square to the world, of
## the current material), or cuts it out with Shift held. The far corner
## follows the ground under the crosshair until the wheel moves it nearer or
## farther along it. Right click (or Backspace) while dragging gives up.
var box_dragging := false
var box_a := Vector3.ZERO
var box_distance := -1.0  # the far corner's distance along the crosshair (-1: on the ground)
var box_preview: DensityBody3D = null
var box_preview_cut := false
var box_shown := PackedVector3Array()  # the corners the preview was made from

enum Tool { DRAW, SMOOTH, FLATTEN, GRAB, SNAKE_HOOK, PAINT, SELECT_PAINT, SELECT_FILL, PHYSICS_GUN, SELECT_GEOMETRY, EXTRUDE, TRANSFORM, GEOMETRY,
		INFLATE, PINCH, BOX, ROTATE, SCALE, LAYER, CLAY_STRIPS, CLONE, SELECT_BOX, PAINT_FILL }
const TOOL_NAMES := ["Draw", "Smooth", "Flatten", "Grab", "Snake hook", "Paint", "Select paint", "Select fill", "Physics gun",
		"Select geometry", "Extrude", "Transform", "Geometry", "Inflate", "Pinch", "Box", "Rotate", "Scale", "Layer", "Clay strips",
		"Clone", "Select box", "Paint fill"]
## Tools that never sculpt with the mouse buttons.
const NON_SCULPT_TOOLS := [Tool.SELECT_PAINT, Tool.SELECT_FILL, Tool.PHYSICS_GUN, Tool.SELECT_GEOMETRY, Tool.EXTRUDE, Tool.TRANSFORM, Tool.GEOMETRY,
		Tool.BOX, Tool.SELECT_BOX, Tool.PAINT_FILL]
## Brushes whose right button does the opposite (deflate, spread, lower,
## scrape) rather than carve. (Alt does the opposite with any brush.)
const INVERTIBLE_TOOLS := [Tool.INFLATE, Tool.PINCH, Tool.LAYER, Tool.CLAY_STRIPS]
## The brush each tool applies while held (-1: grab and snake hook drag instead).
const TOOL_MODES := [SculptTerrain.BRUSH_ADD, SculptTerrain.BRUSH_SMOOTH, SculptTerrain.BRUSH_FLATTEN, -1, -1, SculptTerrain.BRUSH_PAINT, -1, -1, -1, -1, -1, -1, -1,
		SculptTerrain.BRUSH_INFLATE, SculptTerrain.BRUSH_PINCH, -1, -1, -1, SculptTerrain.BRUSH_LAYER, SculptTerrain.BRUSH_CLAY,
		SculptTerrain.BRUSH_CLONE, -1, -1]
## Flatten flattens to (brush settings, N): the face under the brush, one
## facing the view, or one square to an axis (floors, walls).
enum FlattenPlane { FACE, VIEW, AXIS_X, AXIS_Y, AXIS_Z }
var flatten_plane := FlattenPlane.FACE
## Lazy mouse (0: off .. 1): the brush trails the crosshair on a string this
## many brush radii (x 3) long, for steady strokes.
var lazy_mouse := 0.0
var lazy_point = null  # where the trailing brush is, while stroking
## The wheel eases the brush's radius and strength to where it's turned them.
var smooth_wheel := true
var radius_target := -1.0
var strength_target := -1.0
## Shift + click: a straight stroke from where the last one ended.
var stroke_end = null  # the last brush stroke's end (a brush tool's)
var grab_end = null  # the last grab's end
var stroke_last = null  # where the brush was last frame (Clay strips' direction)
## Clone: Ctrl + click sets where it copies from; the first stroke after
## fixes the offset (from there to where that stroke began), kept after.
var clone_source = null
var clone_offset = null
var clone_marker: MeshInstance3D
var brush_panel: Control
var blocky_size_on := 2  # the size B turns blocky mode on at
var box_select_add := true  # Select box: dragged with the left button (adds) or the right (takes away)
const SETTINGS_PATH := "user://brush_settings.cfg"
## The player's size the speeds above are for: shift + wheel scales the
## player, and its width, speed, jump and step with it.
const BASE_PLAYER_HEIGHT := 16.0
## How high a ledge the player steps up onto, at BASE_PLAYER_HEIGHT.
const STEP_HEIGHT := 3.0
## Select fill reaches this many brush radii from where you click (at most
## FILL_MAX voxels): far enough for a whole floating island.
const FILL_RADII := 16.0
const FILL_MAX := 128.0
var tool := Tool.DRAW

## The material Draw builds with and Paint paints: [ and ] cycle, middle
## click picks the one under the crosshair. Names (and textures, in
## res://assets/block/) come from SculptTerrain.get_material_names().
var material_names := PackedStringArray()
var block := 2  # stone; material_names.size() is "Color": paint_color, below

## Colors: after the blocks, [ / ] reach "Color", which Draw builds with and
## Paint paints, like a paint program's brush: paint_color laid over what was
## there with paint_opacity, solid within paint_hardness of the radius and
## fading out beyond (one stroke doesn't build up over itself). C opens
## the picker; middle click on a colored surface picks its color.
var paint_color := Color(0.85, 0.25, 0.2)
var paint_opacity := 1.0
var paint_hardness := 0.5
## Scatter (0..1): each voxel's paint varied a little at random, lighter or
## darker (a new pattern each stroke), for speckled stone, grass, skin.
var paint_scatter := 0.0
var paint_seed := 0  # the stroke's pattern
## Paint fill (6, three presses): a click paints the whole surface connected
## to it that's colored like it there, within brush_strength / 32 (an RGB
## distance, as Select fill's RMB: raise the strength to take in more).
var paint_fill_clicked := false
var color_panel: PanelContainer
var scatter_slider: HSlider
var color_picker: ColorPicker
var opacity_slider: HSlider
var hardness_slider: HSlider

## Grab / snake hook drag: the brush stays this far from the camera while the
## button is held, so it follows the crosshair.
var dragging := false
var drag_distance := 0.0
var drag_start := Vector3.ZERO  # grab: where it began; snake hook: where the brush is now
var drag_copies: Array[Transform3D] = []  # symmetry copies, fixed for the whole drag
var last_grab_delta := Vector3.INF
## Rotate / scale (4, twice / three times): held on the ground, the view
## holds still and the mouse turns (Shift: rolls about the view) or scales
## (right / up: bigger) what's within the brush radius of where it began.
var twisting := false
var twist_body: DensityBody3D = null  # rotating / scaling a body (held still meanwhile), not the ground
var twist_center := Vector3.ZERO
var twist_copies: Array[Transform3D] = []
var twist_turn := Basis()
var twist_scale := 1.0
var twist_sent := Basis()  # what the terrain has
var twist_view := Basis()  # the camera's when it began (the mouse turns about its axes)
## Snake hook moves in steps of at most this fraction of the radius (and 8
## voxels), at most this many per frame; the rest carries over.
const SNAKE_STEP := 0.5
const SNAKE_STEPS_PER_FRAME := 8

## Symmetry: E places an axis through the camera along the view direction, Q
## removes it, Z switches mirror / radial, + / - change the radial count.
## Mirror reflects across the plane holding the axis and the camera's up
## direction (left <-> right as seen when it was placed).
## E on an object instead fixes the axis to it (symmetry_body), through its
## centre of mass, and it moves and turns with it: along the way a frog or
## chiggin faces (creature.gd, _forward(); its left and right mirrored),
## or for anything else as the view was when E was pressed.
enum Symmetry { MIRROR, RADIAL }
var symmetry_on := false
var symmetry := Symmetry.MIRROR
var radial_count := 4
var axis_origin := Vector3.ZERO
var axis_dir := Vector3.FORWARD
var mirror_normal := Vector3.RIGHT
var symmetry_body: DensityBody3D = null
var symmetry_attached := false  # (a freed symmetry_body reads as null)
var symmetry_local := Transform3D()  # (on symmetry_body) origin: the axis's point; x: axis_dir; y: mirror_normal
var axis_line: MeshInstance3D
var plane_line: MeshInstance3D  # mirror only: shows which way the mirror plane runs
var ghost_previews: Array[MeshInstance3D] = []

## True while a mouse button is sculpting: everything from press to release
## is one undo step. Ctrl+Z undoes, Ctrl+Shift+Z or Ctrl+Y redoes.
var stroking := false

var yaw := 0.0    # radians, 0 = facing -Z, positive turns left
var pitch := 0.0  # radians, positive looks up; kept just short of straight up/down
## True from the click that captured the mouse until that button is released,
## so the capturing click doesn't also sculpt.
var capture_click := false

## Seconds between saves of the camera position (the terrain saves itself).
@export var player_save_interval := 30.0
var player_save_timer := 0.0

func respawn(spawn) -> void:
	camera.position = _find_open_air(spawn)
	if player != null:
		player.global_position = camera.global_position - Vector3.UP * player_height
		player.velocity = Vector3.ZERO

func _ready() -> void:
	var start := Time.get_ticks_msec()
	var layer := info_label.get_parent()  # the HUD CanvasLayer (Hud moves the label into itself)
	hud = Hud.new(self)
	layer.add_child(hud)
	brush_panel = BrushPanel.new(self)
	hud.add_child(brush_panel)
	load_brush_settings.call_deferred()
	var names: Array = []
	for t in WHEEL_TOOLS:
		names.append(TOOL_NAMES[t])
	hud.radial.setup(WHEEL_TOOLS, names, _wheel_keys())
	hud.color_pressed.connect(toggle_color_panel)
	hud.help_pressed.connect(hud.toggle_help)
	network = Network.new()
	network.name = "Network"
	network.world = self
	add_child(network)
	network.configure_from_args()
	Features.register(terrain)  # (part of the world's generation: before it opens)
	if network.is_joining() and await _join():
		pass  # the host's world is open, and we stand next to the host
	else:
		terrain.open_world()
		_place_at_start()
	yaw = camera.rotation.y
	pitch = camera.rotation.x
	terrain.gravity = gravity  # what the physics bodies fall with
	_make_player()
	objects = PhysicsObjects.new(terrain)
	camera.get_parent().add_child(objects)
	creatures = Creatures.new(self)
	add_child(creatures)
	if not terrain.save_path.is_empty():
		objects.load_from(_objects_path())
		var saved := ConfigFile.new()
		if saved.load(_objects_path()) == OK:
			creatures.load_from(saved)
			creatures.restore_loaded(player.global_position)
	_finish_ready(start)

## Joining a host: waits for its world settings, opens the same world
## (unsaved: it's the host's) and waits for its edited chunks. Returns false
## if that failed (the caller opens this computer's own world instead).
func _join() -> bool:
	set_process(false)
	set_physics_process(false)
	set_process_input(false)
	network.start_client()
	if network.is_joining():
		await network.settings_received
	if network.is_joining():
		terrain.save_path = ""  # the host's world: don't write it into ours
		terrain.set_world_settings(network.host_settings)
		terrain.open_world()
		network.world_opened()
		if not network.synced and network.is_joining():
			await network.joined
	set_process(true)
	set_physics_process(true)
	set_process_input(true)
	if not terrain.is_world_open():
		return false
	respawn(network.host_spawn + Network.JOIN_OFFSET)
	return true

## Where a new session starts: the saved camera, or off the edge of the
## nearest island, looking at it.
func _place_at_start() -> void:
	if not _load_player():
		# Start off the edge of the nearest island, looking at it.
		var island = terrain.find_island(Vector3.ZERO)
		var spawn := Vector3.ZERO
		if island != null:
			spawn = island + Vector3(0, 25, 90)
		respawn(spawn)
		if island != null:
			camera.look_at(island)

func _finish_ready(p_start: int) -> void:
	var material: ShaderMaterial = terrain.material
	material_names = SculptTerrain.get_material_names()
	_make_color_panel()
	editor = ObjectEditor.new()
	editor.name = "ObjectEditor"
	editor.world = self
	add_child(editor)
	material.set_shader_parameter("block_textures", _load_block_textures())
	material.set_shader_parameter("water_texture", _load_water_texture())
	material.set_shader_parameter("prepass_texture", prepass_viewport.get_texture())
	material.set_shader_parameter("use_prepass", use_prepass)
	prepass_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS if use_prepass else SubViewport.UPDATE_DISABLED
	terrain.update_view(camera.global_position)  # builds and uploads the clipmap
	sun_shadows = SunShadows.new(terrain)
	add_child(sun_shadows)
	_sync_prepass_camera()
	print("World opened in %d ms" % (Time.get_ticks_msec() - p_start))

	get_viewport().size_changed.connect(_fit_screen)
	_fit_screen()
	_make_symmetry_visuals()
	vr = VR.new()
	vr.name = "VR"
	vr.world = self
	add_child(vr)
	vr.start()
	if not vr.active and TouchControls.wanted():
		assume_captured = true  # no mouse to capture: the on-screen buttons sculpt
		touch = TouchControls.new(self)
		hud.add_child(touch)
		hud.set_touch_mode(true)
		_fit_screen()
	if network.mode == Network.Mode.HOST:
		network.start_host()
	network.world_ready()

## The block textures as one texture array (a layer per material, in
## material_names order), with mipmaps for distant ground. A missing
## texture becomes magenta.
func _load_block_textures() -> Texture2DArray:
	var images: Array[Image] = []
	var averages := PackedColorArray()
	for name in material_names:
		var path := "res://assets/block/%s.png" % name
		var image: Image = null
		if ResourceLoader.exists(path):
			var texture: Texture2D = load(path)
			image = texture.get_image()
		elif FileAccess.file_exists(path):
			# Not imported yet (the editor hasn't seen it): read the PNG itself.
			image = Image.load_from_file(path)
		if image == null or image.is_empty():
			push_warning("Missing block texture " + path)
			image = Image.create_empty(16, 16, false, Image.FORMAT_RGBA8)
			image.fill(Color.MAGENTA)
		if image.is_compressed():
			image.decompress()
		image.convert(Image.FORMAT_RGBA8)
		if image.get_size() != Vector2i(16, 16):
			image.resize(16, 16, Image.INTERPOLATE_NEAREST)
		averages.append(_average_color(image))
		image.generate_mipmaps()
		images.append(image)
	# Color painting over a block starts from its average color.
	terrain.set_material_colors(averages)
	var array := Texture2DArray.new()
	array.create_from_images(images)
	return array

func _average_color(p_image: Image) -> Color:
	var sum := Color(0, 0, 0, 0)
	for y in p_image.get_height():
		for x in p_image.get_width():
			var c := p_image.get_pixel(x, y)
			sum += Color(c.r * c.a, c.g * c.a, c.b * c.a, c.a)
	return Color(sum.r / sum.a, sum.g / sum.a, sum.b / sum.a) if sum.a > 0.0 else Color.GRAY

## The sea's ripple texture (grey; the shader tints it), with mipmaps.
func _load_water_texture() -> Texture2D:
	var image := _load_image("res://assets/block/water.png")
	image.generate_mipmaps()
	return ImageTexture.create_from_image(image)

## A material's block texture for the HUD.
func block_icon(p_material: int) -> Texture2D:
	if not block_icons.has(p_material):
		block_icons[p_material] = ImageTexture.create_from_image(_load_image("res://assets/block/%s.png" % material_names[p_material]))
	return block_icons[p_material]

## A 16x16 RGBA image from a texture file (imported or not); magenta if missing.
func _load_image(path: String) -> Image:
	var image: Image = null
	if ResourceLoader.exists(path):
		var texture: Texture2D = load(path)
		image = texture.get_image()
	elif FileAccess.file_exists(path):
		image = Image.load_from_file(path)
	if image == null or image.is_empty():
		push_warning("Missing texture " + path)
		image = Image.create_empty(16, 16, false, Image.FORMAT_RGBA8)
		image.fill(Color.MAGENTA)
	if image.is_compressed():
		image.decompress()
	image.convert(Image.FORMAT_RGBA8)
	return image

## Somewhere near p_near that isn't inside an island: steps upward until well clear.
func _find_open_air(p_near: Vector3) -> Vector3:
	var p := p_near
	for i in 1000:
		if terrain.get_density(p) < -6.0:
			return p
		p.y += 4.0
	return p

func _objects_path() -> String:
	return terrain.save_path.path_join("objects.cfg")

func _player_path() -> String:
	return terrain.save_path.path_join("player.cfg")

## Restores the camera from the last session. Returns false if there is none.
func _load_player() -> bool:
	if terrain.save_path.is_empty():
		return false
	var config := ConfigFile.new()
	if config.load(_player_path()) != OK:
		return false
	camera.position = config.get_value("camera", "position", camera.position)
	camera.rotation = config.get_value("camera", "rotation", camera.rotation)
	return true

## Saves the camera, writing a temporary file first so a crash can't leave a
## half-written one.
func _save_player() -> void:
	if terrain.save_path.is_empty():
		return
	var config := ConfigFile.new()
	config.set_value("camera", "position", camera.position)
	config.set_value("camera", "rotation", camera.rotation)
	var path := ProjectSettings.globalize_path(_player_path())
	if config.save(path + ".tmp") == OK:
		DirAccess.rename_absolute(path + ".tmp", path)

func _notification(what: int) -> void:
	if what == NOTIFICATION_APPLICATION_FOCUS_OUT and hud != null and hud.radial.visible:
		_close_radial(false)  # (Tab's release won't come)
	if what == NOTIFICATION_WM_CLOSE_REQUEST:
		_save_player()
		_save_objects()
	elif what == NOTIFICATION_APPLICATION_PAUSED and terrain != null and terrain.is_world_open():
		# A phone: the app may be killed in the background without a close request.
		_save_player()
		_save_objects()
		terrain.save_world()
		if touch != null:
			touch.release_all()
	elif what == NOTIFICATION_WM_GO_BACK_REQUEST:
		# Android's back: closes what's open (application/config/quit_on_go_back is off), else quits.
		if touch == null or not touch.go_back():
			_save_player()
			_save_objects()
			get_tree().quit()

func _save_objects() -> void:
	if not terrain.save_path.is_empty():
		objects.save(_objects_path(), creatures.to_save())

func _sync_prepass_camera() -> void:
	if not use_prepass:
		return
	prepass_camera.global_transform = camera.global_transform
	prepass_camera.fov = camera.fov

## Scales the low-resolution view up to fill the window, and fits the HUD
## to the window; the color panel (bottom left, above the HUD's bar) shrinks
## if it wouldn't fit. The view keeps its height in pixels (world.tscn's)
## and is as wide as the window's shape needs (a 16:10 tablet, a 20:9
## phone), so it fills the screen without bars or stretched pixels.
var _view_height := 0
func _fit_screen() -> void:
	var window := get_viewport().get_visible_rect().size
	var sub := camera.get_viewport() as SubViewport
	if _view_height == 0:
		_view_height = sub.size.y
	if window.y > 0.0 and (vr == null or not vr.active):
		var width := maxi(roundi(_view_height * window.x / window.y), 1)
		if sub.size.x != width:
			sub.size = Vector2i(width, _view_height)
			screen.size = Vector2(sub.size)
			prepass_viewport.size = sub.size / 2
	var view := Vector2(screen.size)
	var fit := minf(window.x / view.x, window.y / view.y)
	screen.scale = Vector2(fit, fit)
	screen.position = (window - view * fit) * 0.5

	hud.fit(vr.ui.hud_size() if vr != null and vr.active else window)
	if color_panel != null:
		var area := hud.size  # (the HUD's own units: it's scaled to the window)
		var need := color_panel.get_combined_minimum_size()
		color_panel.size = need
		var room := Vector2(area.x - 24.0, area.y - 160.0)
		var scale := minf(1.0, minf(room.x / need.x, room.y / need.y))
		color_panel.scale = Vector2(scale, scale)
		color_panel.position = Vector2(12.0, area.y - 80.0 - need.y * scale)

## Mouse look. The camera's rotation is rebuilt from these angles each time, so
## pitch always tilts around the camera's own right axis whichever way you face.
func _input(event: InputEvent) -> void:
	if touch != null and event is InputEventMouse and not touch.sending:
		return  # touch screen: fingers act through touch_controls.gd (a tap's emulated mouse is the GUI's)
	if editor != null and editor.handle_input(event):
		get_viewport().set_input_as_handled()
		return
	if hud.radial.visible and _radial_input(event):
		get_viewport().set_input_as_handled()
		return
	if event is InputEventMouseMotion and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
		look_input(event.relative, event.shift_pressed, Input.is_physical_key_pressed(KEY_TAB))
	elif event is InputEventMouseButton and event.pressed:
		if not _captured():
			if hud.has_point(event.position):
				return  # the HUD's (the picker, its clickable slots)
			if editor != null and editor.has_point(event.position):
				return  # the object editor's panels
			if event.button_index == MOUSE_BUTTON_LEFT:
				Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
				capture_click = true
				get_viewport().gui_release_focus()  # back to playing: keys are hotkeys again
		elif event.button_index == MOUSE_BUTTON_RIGHT and event.shift_pressed and _begin_hoist():
			pass  # (and not the tool's right button)
		elif tool == Tool.CLONE and event.button_index == MOUSE_BUTTON_LEFT and event.ctrl_pressed:
			_set_clone_source()
		elif event.button_index == MOUSE_BUTTON_LEFT and event.shift_pressed and TOOL_MODES[tool] >= 0 and stroke_end != null:
			_brush_line(stroke_end)
		elif tool == Tool.GRAB and event.button_index == MOUSE_BUTTON_LEFT and event.shift_pressed and grab_end != null:
			_grab_line()
		elif tool == Tool.PHYSICS_GUN and event.button_index in [MOUSE_BUTTON_LEFT, MOUSE_BUTTON_RIGHT]:
			var aimed := DensityBody3D.raycast_bodies(camera.global_position, -camera.global_basis.z, reach)
			if not aimed.is_empty():
				var body: DensityBody3D = aimed.body
				network.use(body)  # (multiplayer: it's simulated here while used)
				if event.button_index == MOUSE_BUTTON_RIGHT:
					# Freeze where it is, or let it go again.
					body.freeze = not body.freeze
					body.sleeping = false
				else:
					gun_body = body
					gun_turned = false
					gun_distance = aimed.distance
					gun_point = body.to_local(aimed.position)
		elif tool in [Tool.BOX, Tool.SELECT_BOX] and event.button_index in [MOUSE_BUTTON_LEFT, MOUSE_BUTTON_RIGHT]:
			_box_press(event.button_index == MOUSE_BUTTON_LEFT)
		elif tool in [Tool.GEOMETRY, Tool.SELECT_GEOMETRY] and event.button_index in [MOUSE_BUTTON_LEFT, MOUSE_BUTTON_RIGHT]:
			_geometry_click(event.button_index == MOUSE_BUTTON_LEFT)
		elif tool == Tool.SNAKE_HOOK and event.button_index == MOUSE_BUTTON_LEFT and event.shift_pressed and snake_last != null:
			_snake_line()
		elif tool == Tool.TRANSFORM and event.button_index == MOUSE_BUTTON_LEFT:
			_transform_press()
		elif event.button_index == MOUSE_BUTTON_MIDDLE:
			var picked := _material_under_crosshair()
			if picked != -1:
				if picked >= 32768:  # a color
					paint_color = terrain.get_material_color(picked)
					color_picker.color = paint_color
					block = material_names.size()
				elif picked >= 0:
					block = picked
		elif event.button_index == MOUSE_BUTTON_WHEEL_UP or event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			var step := 1.15 if event.button_index == MOUSE_BUTTON_WHEEL_UP else 1.0 / 1.15
			if hoisting:
				hoist_distance = clampf(hoist_distance * step, 2.0, reach)  # let out / take in
			elif move_mode == MoveMode.ORBIT and event.alt_pressed:
				orbit_distance = clampf(orbit_distance * step, 2.0, reach * 4.0)
			elif event.shift_pressed:
				_set_player_height(player_height * step)  # the player's size, and all that goes with it
			elif gun_body != null:
				gun_distance = clampf(gun_distance * step, 4.0, reach)  # pull in / push out
			elif box_dragging:
				if box_distance < 0.0:
					box_distance = camera.global_position.distance_to(_box_far_corner())
				box_distance = clampf(box_distance * step, 2.0, reach)
			elif tool in [Tool.GEOMETRY, Tool.SELECT_GEOMETRY] and not event.ctrl_pressed:
				if geo_drag >= 0:
					geo_drag_distance = clampf(geo_drag_distance * step, 2.0, reach)
				else:
					geo_distance = clampf(geo_distance * step, 2.0, reach)
			elif event.ctrl_pressed:
				strength_target = clampf((strength_target if strength_target > 0.0 else brush_strength) * step, 0.5, 64.0)
				if not smooth_wheel:
					brush_strength = strength_target
			else:
				radius_target = clampf((radius_target if radius_target > 0.0 else brush_radius) * step, 1.0, 48.0)
				if not smooth_wheel:
					brush_radius = radius_target
	elif event is InputEventKey and event.pressed and not event.echo and event.ctrl_pressed and event.physical_keycode in [KEY_P, KEY_O, KEY_C, KEY_V]:
		match event.physical_keycode:
			KEY_P: _export_selection()
			KEY_O:
				if event.shift_pressed:
					_import_to_paste()
				else:
					_import_shape()
			KEY_C: _copy_selection()
			KEY_V: _paste()
	elif event is InputEventKey and event.pressed and event.ctrl_pressed and event.physical_keycode in [KEY_Z, KEY_Y]:
		# Echo too, so holding the keys keeps stepping back.
		if not stroking and gizmo_axis < 0:
			if event.physical_keycode == KEY_Y or event.shift_pressed:
				_redo()
			else:
				_undo()
	elif event is InputEventKey and event.pressed and not event.echo:
		if event.physical_keycode == KEY_TAB:
			# Held: the tool wheel (_radial_input() picks on release).
			# (Holding a body with the physics gun, Tab turns it instead.)
			if not dragging and not stroking and gizmo_axis < 0 and gun_body == null:
				hud.radial.open(tool)
			get_viewport().set_input_as_handled()  # not the GUI's focus-next
			return
		if get_viewport().gui_get_focus_owner() is LineEdit:
			return  # typing into a field (the picker's, the editor's)
		match _key_of(event):
			KEY_C:
				toggle_color_panel()
			KEY_H:
				hud.toggle_help()
			KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0:
				var cycle: Array = KEY_TOOLS[_key_of(event)]
				if cycle.size() == 1:
					_set_tool(cycle[0])
				else:
					_cycle_tool(cycle)
			KEY_N:
				toggle_brush_panel()
			KEY_T:
				gizmo_mode = ((gizmo_mode + 1) % 3) as Gizmo
			KEY_ENTER, KEY_KP_ENTER:
				_confirm(event.shift_pressed)  # Shift: cut the shape out instead
			KEY_BACKSPACE:
				if box_dragging:
					box_dragging = false
				elif tool in [Tool.GEOMETRY, Tool.SELECT_GEOMETRY] and not geo_points.is_empty():
					geo_points.clear()
					_rebuild_geometry()
				else:
					_apply_floating()
					terrain.clear_selection()
					_clear_body_selection()
			KEY_P:
				_selection_to_body()
			KEY_B:
				# Blocky mode: every tool's result snaps to blocks (2 voxels, or
				# the size the brush settings, N, last had).
				terrain.blocky_size = 0 if terrain.blocky_size >= 2 else maxi(blocky_size_on, 2)
				save_brush_settings()
			KEY_F:
				# Made of the selected block, or a random colour when "Color" is.
				var ball_material := block if block < material_names.size() else -1
				objects.throw(camera.global_position, -camera.global_basis.z, player.velocity, ball_material)
			KEY_G:
				if event.shift_pressed:
					creature = (creature + 1) % CREATURE_NAMES.size()
					_notify("G drops: %s (Shift+G switches)" % CREATURE_NAMES[creature])
				else:
					_spawn_frog()
			KEY_X:
				# Removes the ball or frog under the crosshair.
				var aimed := DensityBody3D.raycast_bodies(camera.global_position, -camera.global_basis.z, reach)
				if not aimed.is_empty():
					_delete_bodies([aimed.body])  # (Ctrl+Z brings it back)
			KEY_BRACKETLEFT: block = posmod(block - 1, material_names.size() + 1)
			KEY_BRACKETRIGHT: block = posmod(block + 1, material_names.size() + 1)
			KEY_E when event.shift_pressed:
				if not dragging:
					mirror_half_from_view()
			KEY_E:
				if not dragging:
					symmetry_on = true
					symmetry_body = null
					symmetry_attached = false
					axis_origin = camera.global_position
					axis_dir = -camera.global_basis.z
					mirror_normal = camera.global_basis.x
					_attach_symmetry()
			KEY_Q:
				if not dragging:
					symmetry_on = false
					symmetry_body = null
					symmetry_attached = false
			KEY_Z:
				if not dragging:
					symmetry = Symmetry.RADIAL if symmetry == Symmetry.MIRROR else Symmetry.MIRROR
			KEY_EQUAL, KEY_KP_ADD:
				if not dragging:
					radial_count = mini(radial_count + 1, 16)
			KEY_MINUS, KEY_KP_SUBTRACT:
				if not dragging:
					radial_count = maxi(radial_count - 1, 2)
			KEY_BACKSLASH:
				respawn(camera.position)
			KEY_V:
				_cycle_move_mode()
			KEY_K:
				_set_player_height(player_height + 1)
			KEY_L:
				_set_player_height(player_height - 1)
			KEY_ALT, KEY_CTRL, KEY_META, KEY_SHIFT:
				pass  # (modifiers: held with the mouse)
			_:
				if not _is_action(event):
					print("Unbound key: physical %s, keycode %s, label %s, unicode %d" % [
						OS.get_keycode_string(event.physical_keycode), OS.get_keycode_string(event.keycode),
						OS.get_keycode_string(event.key_label), event.unicode])

## Mouse movement (or a touch screen's drag, in mouse pixels): turns what's
## being rotated / scaled, the physics gun's body while p_turn (Tab, or the
## touch screen's Turn button), or else the view. p_roll: Shift (rotating
## rolls about the view).
func look_input(p_relative: Vector2, p_roll: bool, p_turn: bool) -> void:
	if twisting:
		_twist_input(p_relative, p_roll)
	elif gun_body != null and p_turn:
		_turn_gun_body(p_relative)
	else:
		yaw -= p_relative.x * mouse_sensitivity
		pitch = clampf(pitch - p_relative.y * mouse_sensitivity, deg_to_rad(-89.0), deg_to_rad(89.0))
		camera.rotation = Vector3(pitch, yaw, 0.0)

## Mouse buttons and modifier keys as the tools see them: the mouse and
## keyboard's, or on a touch screen the on-screen buttons' (touch_controls.gd).
func _button(p_button: MouseButton) -> bool:
	if touch != null:
		return touch.is_held(p_button)
	return Input.is_mouse_button_pressed(p_button)

func _shift_held() -> bool:
	return Input.is_key_pressed(KEY_SHIFT) or (touch != null and touch.shift)

func _alt_held() -> bool:
	return Input.is_key_pressed(KEY_ALT) or (touch != null and touch.alt)

## Whether opening a panel frees the mouse (not in a headset or on a touch
## screen, where there's no captured mouse to free).
func _frees_mouse() -> bool:
	return (vr == null or not vr.active) and touch == null

## The color picker (C, or the HUD's material slot): on or off, freeing
## the mouse for it on a screen.
func toggle_color_panel() -> void:
	color_panel.visible = not color_panel.visible
	_fit_screen()
	if _frees_mouse():
		Input.mouse_mode = Input.MOUSE_MODE_VISIBLE if color_panel.visible else Input.MOUSE_MODE_CAPTURED
	get_viewport().gui_release_focus()

## While the tool wheel is open: the mouse moves its pointer (not the
## view), letting go of Tab or a click picks the tool, Escape or the right
## button closes it as it was. True if the event was the wheel's.
func _radial_input(event: InputEvent) -> bool:
	if event is InputEventMouseMotion:
		hud.radial.move(event.relative)
		return true
	if event is InputEventKey and event.physical_keycode == KEY_TAB:
		if not event.pressed:
			_close_radial(true)
		return true  # (and its repeats while held)
	if event is InputEventKey and event.pressed and event.physical_keycode == KEY_ESCAPE:
		_close_radial(false)
		return true
	if event is InputEventMouseButton and event.pressed:
		if event.button_index == MOUSE_BUTTON_LEFT:
			_close_radial(true)
			capture_click = true  # (that click doesn't also sculpt)
			return true
		if event.button_index == MOUSE_BUTTON_RIGHT:
			_close_radial(false)
			capture_click = true
			return true
	return false

func _close_radial(p_pick: bool) -> void:
	var picked := hud.radial.close()
	if p_pick and picked >= 0 and picked != tool:
		_set_tool(picked as Tool)

## Which key an event is. Physical keys (positions on a US keyboard) first, so
## hotkeys stay put on other layouts; but some setups (input methods, some
## Wayland compositors) don't report one for every key, so fall back to the
## key code, and for the brackets to the character typed.
func _key_of(event: InputEventKey) -> Key:
	match event.unicode:
		91, 123: return KEY_BRACKETLEFT  # [ {
		93, 125: return KEY_BRACKETRIGHT  # ] }
	if event.physical_keycode != KEY_NONE:
		return event.physical_keycode
	return event.keycode

func _is_action(event: InputEvent) -> bool:
	for action in InputMap.get_actions():
		if not String(action).begins_with("ui_") and event.is_action(action):
			return true
	return false

func _set_tool(p_tool: Tool) -> void:
	_end_drag()
	if tool == Tool.TRANSFORM and p_tool != Tool.TRANSFORM:
		_apply_floating()
		_drop_gizmo_body()
	tool = p_tool
	if tool == Tool.TRANSFORM:
		_lift_selection()
		if floating == null and not selected_bodies.is_empty() and is_instance_valid(selected_bodies[0]):
			_set_gizmo_body(selected_bodies[0])  # (a selected object: that one)

## A key with several tools: the first, or the next if one of them is on.
func _cycle_tool(p_tools: Array) -> void:
	var at := p_tools.find(tool)
	_set_tool(p_tools[(at + 1) % p_tools.size()] if at >= 0 else p_tools[0])

## Middle click: the material (or color, 32768 and up) of what the crosshair
## is on, the ground or an object, whichever is nearer. -1: nothing.
func _material_under_crosshair() -> int:
	var from := camera.global_position
	var dir := -camera.global_basis.z
	var hit = terrain.raycast(from, dir, reach)
	var aimed := DensityBody3D.raycast_bodies(from, dir, reach)
	if not aimed.is_empty() and (hit == null or from.distance_to(aimed.position) < from.distance_to(hit)):
		var body: DensityBody3D = aimed.body
		if body.shape == null:
			return body.material
		# A little inside, so the nearest grid point is solid.
		var inside: Vector3 = aimed.position + dir * 0.6 * _voxel_scale(body)
		var m: int = body.shape.get_material_at(body.to_local(inside), body.pose, body.key_weights)
		return m if m >= 0 else body.material
	if hit != null:
		return terrain.get_block_material(hit)
	return -1

## E while aiming at an object (nearer than the ground): the symmetry axis
## goes on it, see symmetry_body.
func _attach_symmetry() -> void:
	var from := camera.global_position
	var dir := -camera.global_basis.z
	var aimed := DensityBody3D.raycast_bodies(from, dir, reach)
	if aimed.is_empty():
		return
	var hit = terrain.raycast(from, dir, reach)
	if hit != null and from.distance_to(hit) < from.distance_to(aimed.position):
		return
	attach_symmetry_to(aimed.body)

## Fixes the symmetry axis to p_body (see symmetry_body): along where a
## creature faces, or the view.
func attach_symmetry_to(p_body: DensityBody3D) -> void:
	var body := p_body
	symmetry_on = true
	var forward := -camera.global_basis.z
	var up := camera.global_basis.y
	if body.has_method("_forward"):
		# A creature: along where it faces, mirrored left to right.
		up = body.global_basis.y.normalized()
		forward = body._forward()
		forward = (forward - up * forward.dot(up)).normalized()
	var side := forward.cross(up).normalized()
	symmetry_body = body
	symmetry_attached = true
	var inverse := body.global_transform.affine_inverse()
	symmetry_local = Transform3D(Basis(inverse.basis * forward, inverse.basis * side, Vector3.ZERO), body.to_local(body.get_global_center_of_mass()))
	_follow_symmetry_body()
	_notify("Symmetry on the %s (E elsewhere: by the view again, Q: off)" % (body.scene_file_path.get_file().get_basename() if body.scene_file_path != "" else "object"))

## Shift+E: the half of the symmetry's object the camera is on, mirrored
## onto the other half (once; Ctrl+Z undoes it).
func mirror_half_from_view() -> void:
	if not symmetry_on or not symmetry_attached or symmetry != Symmetry.MIRROR:
		_notify("Shift+E mirrors an object: put mirror symmetry on it first (E aiming at it)")
		return
	_follow_symmetry_body()
	var right := (camera.global_position - axis_origin).dot(mirror_normal) >= 0.0
	mirror_half(symmetry_body, right)

## Mirrors p_body across its attached mirror plane, keeping the side the
## plane's normal faces (p_keep_positive; for a creature, its right) or the
## other, as one undo step. Returns whether it did.
func mirror_half(p_body: DensityBody3D, p_keep_positive: bool) -> bool:
	if not is_instance_valid(p_body) or p_body.shape == null or p_body != symmetry_body or not symmetry_attached:
		return false
	network.use(p_body)
	var normal := symmetry_local.basis.y * (1.0 if p_keep_positive else -1.0)
	var before: DensityShape = p_body.shape
	var after: DensityShape = before.mirrored(symmetry_local.origin, normal)
	p_body.shape = after
	p_body.sleeping = false
	_push_history({"body": p_body, "before": before, "after": after,
			"part_shapes": [], "part_transform": p_body.global_transform, "parts": []})
	var kept := "right" if p_keep_positive else "left"
	if p_body.has_method("_forward"):
		_notify("Mirrored: its %s side onto its %s (Ctrl+Z undoes)" % [kept, "left" if p_keep_positive else "right"])
	else:
		_notify("Mirrored: the side you're on onto the other (Ctrl+Z undoes)")
	return true

## The axis of an attached symmetry where its object is now.
func _follow_symmetry_body() -> void:
	if not symmetry_attached:
		return
	if not is_instance_valid(symmetry_body) or not symmetry_body.is_inside_tree():
		symmetry_body = null
		symmetry_attached = false
		symmetry_on = false
		return
	var t := symmetry_body.global_transform
	axis_origin = t * symmetry_local.origin
	axis_dir = (t.basis * symmetry_local.basis.x).normalized()
	mirror_normal = (t.basis * symmetry_local.basis.y).normalized()

## The symmetry copies of a brush as transforms (identity first).
func _symmetry_copies() -> Array[Transform3D]:
	var copies: Array[Transform3D] = [Transform3D.IDENTITY]
	if not symmetry_on:
		return copies
	var to_axis := Transform3D(Basis.IDENTITY, -axis_origin)
	var from_axis := Transform3D(Basis.IDENTITY, axis_origin)
	if symmetry == Symmetry.MIRROR:
		var n := mirror_normal
		var reflect := Basis(Vector3.RIGHT - 2.0 * n.x * n, Vector3.UP - 2.0 * n.y * n, Vector3.BACK - 2.0 * n.z * n)
		copies.append(from_axis * Transform3D(reflect, Vector3.ZERO) * to_axis)
	else:
		for i in range(1, radial_count):
			copies.append(from_axis * Transform3D(Basis(axis_dir, TAU * i / radial_count), Vector3.ZERO) * to_axis)
	return copies

## Positions of p_point under each copy, leaving out ones that land on an
## earlier copy (the brush right on the axis or the mirror plane) so they
## don't apply twice. Copies merely near each other are all kept: their
## brushes (and markers) overlap, as they would in Blender. (This used to
## merge copies within a quarter radius, so near the line the mirror
## brush's marker vanished and only one of them applied.)
## Returns the indices of the copies kept.
func _distinct_copies(p_copies: Array[Transform3D], p_point: Vector3) -> PackedInt32Array:
	var kept := PackedInt32Array()
	for i in p_copies.size():
		var q := p_copies[i] * p_point
		var duplicate := false
		for j in kept:
			if (p_copies[j] * p_point).distance_to(q) < 0.05:
				duplicate = true
				break
		if not duplicate:
			kept.append(i)
	return kept

## How big a voxel of p_body is in the world (1: the terrain's). Brushes on
## a body are scaled by it, so the brush covers as many of its voxels as it
## would of the terrain's: a fine-grained object gets a finer brush.
func _voxel_scale(p_body: DensityBody3D) -> float:
	if p_body == null or not is_instance_valid(p_body) or p_body.shape == null:
		return 1.0
	return p_body.shape.get_voxel_size() * absf(p_body.global_basis.get_scale().x)

## The brush's radius in the world on p_body (null: the terrain).
func _radius_on(p_body: DensityBody3D) -> float:
	return brush_radius * _voxel_scale(p_body)

## _distinct_copies() for a body: only the copies whose brush reaches it
## (the axis needn't run through it; a copy off in the air would only grow
## its grid towards nothing). The first (the brush itself) is always kept.
func _body_copies(p_body: DensityBody3D, p_copies: Array[Transform3D], p_point: Vector3) -> PackedInt32Array:
	var kept := PackedInt32Array()
	for i in _distinct_copies(p_copies, p_point):
		if i == 0 or p_body.get_density_at(p_copies[i] * p_point) > -_radius_on(p_body):
			kept.append(i)
	return kept

func _make_symmetry_visuals() -> void:
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	material.albedo_color = Color(0.4, 0.85, 1.0, 0.7)
	var make_line := func(length: float) -> MeshInstance3D:
		var mesh := CylinderMesh.new()
		mesh.top_radius = 0.15
		mesh.bottom_radius = 0.15
		mesh.height = length
		mesh.radial_segments = 6
		mesh.rings = 1
		mesh.material = material
		var line := MeshInstance3D.new()
		line.mesh = mesh
		line.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		line.visible = false
		brush_preview.get_parent().add_child(line)
		return line
	axis_line = make_line.call(2000.0)
	plane_line = make_line.call(60.0)

## Places the axis line (a cylinder's length runs along its Y) and the
## brush's ghost copies.
func _update_symmetry_visuals(p_brush: Variant) -> void:
	axis_line.visible = symmetry_on
	plane_line.visible = symmetry_on and symmetry == Symmetry.MIRROR
	if symmetry_on:
		axis_line.global_transform = Transform3D(_basis_along(axis_dir), axis_origin)
		# In the mirror plane, across the axis, centred where the axis passes
		# nearest the brush (or the camera).
		var near: Vector3 = p_brush if p_brush != null else camera.global_position
		var along := axis_origin + axis_dir * (near - axis_origin).dot(axis_dir)
		plane_line.global_transform = Transform3D(_basis_along(axis_dir.cross(mirror_normal).normalized()), along)
	var copies := _symmetry_copies() if not dragging else drag_copies
	var kept := PackedInt32Array()
	if p_brush != null and brush_preview.visible:
		kept = _distinct_copies(copies, p_brush)
	var count := maxi(kept.size() - 1, 0)
	while ghost_previews.size() < count:
		var ghost := brush_preview.duplicate() as MeshInstance3D
		brush_preview.get_parent().add_child(ghost)
		ghost_previews.append(ghost)
	for i in ghost_previews.size():
		ghost_previews[i].visible = i < count
		if i < count:
			ghost_previews[i].global_position = copies[kept[i + 1]] * p_brush
			ghost_previews[i].scale = brush_preview.scale

## A basis whose Y axis points along p_dir.
func _basis_along(p_dir: Vector3) -> Basis:
	var y := p_dir.normalized()
	var x := y.cross(Vector3.UP if absf(y.y) < 0.99 else Vector3.RIGHT).normalized()
	return Basis(x, y, x.cross(y))

## The slowest time (ms) each part of _process took since the last
## take_step_times() (vr.gd logs them on the headset).
var step_times := {}

## Prints, once a second, the slowest frame and what took the time in it
## (to find hitches; turn off when done).
const PRINT_FRAME_TIMES := true
var _frame_report_time := 0.0
var _frame_worst := 0.0
var _frame_worst_physics := 0.0
var _frame_worst_gpu := 0.0
var _frame_count := 0
var _measuring_gpu := false

func _report_frame_times(delta: float) -> void:
	var vp_rid := camera.get_viewport().get_viewport_rid()
	if not _measuring_gpu:
		_measuring_gpu = true
		RenderingServer.viewport_set_measure_render_time(vp_rid, true)
	_frame_count += 1
	_frame_worst = maxf(_frame_worst, delta * 1000.0)
	_frame_worst_physics = maxf(_frame_worst_physics, Performance.get_monitor(Performance.TIME_PHYSICS_PROCESS) * 1000.0)
	_frame_worst_gpu = maxf(_frame_worst_gpu, RenderingServer.viewport_get_measured_render_time_gpu(vp_rid))
	_frame_report_time += delta
	if _frame_report_time < 1.0:
		return
	var native: Dictionary = terrain.take_view_times()
	var parts: PackedStringArray = []
	for k in native:
		if k != "layers" and native[k] >= 0.5:
			parts.append("%s %.1f" % [k, native[k]])
	print("frames %d, worst %.1f ms | physics %.1f | gpu %.1f | process: %s | update_view: %s | layers %d" % [
		_frame_count, _frame_worst, _frame_worst_physics, _frame_worst_gpu,
		take_step_times(), ", ".join(parts), native["layers"]])
	_frame_report_time = 0.0
	_frame_worst = 0.0
	_frame_worst_physics = 0.0
	_frame_worst_gpu = 0.0
	_frame_count = 0

func _time_step(p_name: String, p_since: int) -> int:
	var now := Time.get_ticks_usec()
	step_times[p_name] = maxf(step_times.get(p_name, 0.0), (now - p_since) / 1000.0)
	return now

func take_step_times() -> String:
	var parts: PackedStringArray = []
	for k in step_times:
		if step_times[k] >= 1.0:
			parts.append("%s %.0f" % [k, step_times[k]])
	step_times.clear()
	return ", ".join(parts)

func _process(delta: float) -> void:
	_follow_symmetry_body()
	if Input.is_action_pressed("ui_cancel") and touch == null:
		Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	if not _button(MOUSE_BUTTON_LEFT):
		capture_click = false

	# The camera rides on top of the player (moved in _physics_process).
	# Small ups and downs while walking (steps, rough ground) are eased, so
	# the view glides instead of jolting; anything bigger is followed at once.
	if move_mode == MoveMode.WALK:
		var eye := player.global_position + Vector3.UP * player_height
		var off := camera_lag
		if player.is_on_floor() and absf(off) < player.step_height * 2.0:
			off *= exp(-delta * 18.0)
		else:
			off = 0.0
		camera_lag = off
		camera.global_position = eye + Vector3.UP * camera_lag
	else:
		_fly(delta)
	var t := Time.get_ticks_usec()
	if vr != null and vr.active:
		vr.follow(player.global_position)  # the headset decides where the camera is
	t = _time_step("vr", t)

	_ease_brush(delta)
	_sculpt(delta)
	_update_body_selection()
	t = _time_step("sculpt", t)
	_update_gizmo()
	_update_geometry()
	_update_box()
	t = _time_step("gizmo/geometry/box", t)
	_record_history()
	t = _time_step("history", t)
	terrain.update_view(camera.global_position)  # also sends the drawn bodies to the shader
	t = _time_step("terrain.update_view", t)
	sun_shadows.update(camera.global_position)
	t = _time_step("sun_shadows", t)

	_sync_prepass_camera()
	t = _time_step("prepass", t)
	if PRINT_FRAME_TIMES:
		_report_frame_times(delta)

	player_save_timer += delta
	if player_save_timer >= player_save_interval:
		player_save_timer = 0.0
		_save_player()
		_save_objects()

func _physics_process(delta: float) -> void:
	# Bodies (balls, frogs) are stepped by the terrain itself, after this
	# (it is a child of this node); the player moves here.
	_wrap_player()
	# Online, every object stays loaded (each is simulated by one player's
	# computer; see network.gd); only ones that fell out of the world go.
	objects.stream(player.global_position, not network.is_online())

	if move_mode == MoveMode.WALK:
		_walk(delta)
	_update_physics_gun(delta)

## The player's move this physics frame, walking (or hoisting).
func _walk(delta: float) -> void:
	var size_scale := player_height / BASE_PLAYER_HEIGHT
	if hoisting and not _button(MOUSE_BUTTON_RIGHT):
		hoisting = false  # let go: falls from there, keeping the speed it had
	if hoisting:
		# Drawn to hoist_distance back along the line of sight to the hold
		# (camera z points back), no gravity meanwhile; still colliding.
		var eye := player.global_position + Vector3.UP * player_height
		var want := hoist_anchor + camera.global_basis.z * hoist_distance
		player.velocity = ((want - eye) * HOIST_PULL).limit_length(move_speed * size_scale * HOIST_MAX_SPEED)
		player.move_and_slide()
		camera_lag = 0.0
		return
	var facing := -camera.global_basis.z
	facing = Vector3(facing.x, 0.0, facing.z).normalized()
	var side := facing.cross(Vector3.UP)
	var forward: float = Input.get_action_strength("forward") - Input.get_action_strength("back")
	var right: float = Input.get_action_strength("right") - Input.get_action_strength("left")
	var speed: float = move_speed * size_scale * (4.0 if Input.is_action_pressed("sprint") else 1.0)
	var walk := (facing * forward + side * right).normalized() * speed
	player.velocity.x = walk.x
	player.velocity.z = walk.z
	if player.is_on_floor():
		player.velocity.y = jump_speed * size_scale if Input.is_action_pressed("jump") else 0.0
	else:
		player.velocity.y -= gravity * size_scale * delta  # a big player falls as a small one would, scaled up
	var feet_before := player.global_position.y
	player.move_and_slide()
	# The camera keeps its height for a moment when the feet jump (see _process).
	camera_lag -= player.global_position.y - feet_before
	if not player.is_on_floor():
		camera_lag = 0.0

## Freecam / orbit cam, each frame: moves the camera (WASD, Space / Shift
## for up / down, Ctrl 4x), and the player's capsule along under it, so
## loading, wrapping and saving go on as if it walked there.
func _fly(delta: float) -> void:
	var size_scale := player_height / BASE_PLAYER_HEIGHT
	var speed: float = move_speed * size_scale * 2.0 * (4.0 if Input.is_action_pressed("sprint") else 1.0)
	var forward: float = Input.get_action_strength("forward") - Input.get_action_strength("back")
	var right: float = Input.get_action_strength("right") - Input.get_action_strength("left")
	var up: float = Input.get_action_strength("jump") - Input.get_action_strength("sneak")
	var b := camera.global_basis
	if move_mode == MoveMode.FREECAM:
		# Where you look, and straight up / down.
		var move := -b.z * forward + b.x * right + Vector3.UP * up
		if move.length_squared() > 0.0:
			camera.global_position += move.normalized() * speed * delta
	else:
		# The point moves level with the view (and up / down), faster the
		# farther out it's looked at from.
		var facing := Vector3(-b.z.x, 0.0, -b.z.z).normalized()
		var move := facing * forward + b.x * right + Vector3.UP * up
		if move.length_squared() > 0.0:
			orbit_pivot += move.normalized() * speed * maxf(1.0, orbit_distance / 60.0) * delta
		camera.global_position = orbit_pivot + b.z * orbit_distance
	player.global_position = camera.global_position - Vector3.UP * player_height
	player.velocity = Vector3.ZERO
	camera_lag = 0.0

## V: walking -> freecam -> orbit cam -> walking.
func _cycle_move_mode() -> void:
	if vr != null and vr.active:
		return  # (the headset decides where the camera is)
	hoisting = false
	move_mode = ((move_mode + 1) % MOVE_MODE_NAMES.size()) as MoveMode
	player.collide_with_bodies = move_mode == MoveMode.WALK
	camera_lag = 0.0
	match move_mode:
		MoveMode.ORBIT:
			# Round what's under the crosshair (or a way in front).
			var look := -camera.global_basis.z
			var hit = terrain.raycast(camera.global_position, look, reach)
			orbit_distance = maxf(camera.global_position.distance_to(hit), 2.0) if hit != null else 60.0 * player_height / BASE_PLAYER_HEIGHT
			orbit_pivot = camera.global_position + look * orbit_distance
		MoveMode.WALK:
			player.velocity = Vector3.ZERO
			# Not inside the ground: up out of it if the camera was.
			if terrain.get_density(player.global_position + Vector3.UP * player_height * 0.5) > -1.0:
				respawn(camera.position)
	_notify("%s (V switches)" % MOVE_MODE_NAMES[move_mode])

## Shift + RMB: takes hold of the ground under the crosshair (walking).
## False if there's none, so the click goes to the tool.
func _begin_hoist() -> bool:
	if move_mode != MoveMode.WALK or (vr != null and vr.active):
		return false
	var hit = terrain.raycast(camera.global_position, -camera.global_basis.z, reach)
	if hit == null:
		return false
	hoisting = true
	hoist_anchor = hit
	hoist_distance = maxf(camera.global_position.distance_to(hit), 2.0)
	return true

## Tab + mouse with the physics gun: turns the held body about the view's
## up axis (left / right) and right axis (up / down), the near side
## following the mouse.
func _turn_gun_body(p_relative: Vector2) -> void:
	if not gun_turned:
		gun_basis = gun_body.global_basis.orthonormalized()
		gun_turned = true
	var b := camera.global_basis
	gun_basis = (Basis(b.y, p_relative.x * mouse_sensitivity) * Basis(b.x, p_relative.y * mouse_sensitivity) * gun_basis).orthonormalized()

## The world wraps around in x and z every world_size voxels. Coordinates
## are kept within about half that of 0 (where float precision is best) by
## moving the player back a whole world size once they go past the half way
## line (a little past it, so walking along the line doesn't flip back and
## forth). Everything around moves with them, to the same place on the
## wrapping world, so nothing visibly changes.
func _wrap_player() -> void:
	var size := float(terrain.get_world_size())
	var limit := size * 0.5 + 64.0
	var offset := Vector3.ZERO
	var p := player.global_position
	if absf(p.x) > limit:
		offset.x = -roundf(p.x / size) * size
	if absf(p.z) > limit:
		offset.z = -roundf(p.z / size) * size
	if offset == Vector3.ZERO:
		return
	_end_drag()  # a drag in progress is in the old coordinates
	camera.global_position += offset
	terrain.shift_origin(Vector3i(offset))  # moves every DensityBody3D too (the player, balls, frogs)
	axis_origin += offset
	for i in geo_points.size():
		geo_points[i] += offset  # (the preview body moves itself)
	hoist_anchor += offset
	orbit_pivot += offset

## The color picker (C): an RGB picker with opacity and hardness sliders.
func _make_color_panel() -> void:
	color_panel = PanelContainer.new()
	color_panel.visible = false
	var box := VBoxContainer.new()
	color_panel.add_child(box)
	color_picker = ColorPicker.new()
	color_picker.edit_alpha = false  # opacity is the brush's, below
	# Compact: no swatches, screen sampler or mode switcher.
	color_picker.presets_visible = false
	color_picker.sampler_visible = false
	color_picker.color_modes_visible = false
	color_picker.color = paint_color
	color_picker.color_changed.connect(func(c: Color) -> void:
		paint_color = c
		block = material_names.size())
	box.add_child(color_picker)
	opacity_slider = _add_slider(box, "Opacity", paint_opacity, func(v: float) -> void: paint_opacity = v)
	hardness_slider = _add_slider(box, "Hardness", paint_hardness, func(v: float) -> void: paint_hardness = v)
	scatter_slider = _add_slider(box, "Scatter", paint_scatter, func(v: float) -> void: paint_scatter = v)
	hud.add_child(color_panel)

func _add_slider(p_box: VBoxContainer, p_name: String, p_value: float, p_changed: Callable) -> HSlider:
	var label := Label.new()
	label.text = p_name
	p_box.add_child(label)
	var slider := HSlider.new()
	slider.min_value = 0.0
	slider.max_value = 1.0
	slider.step = 0.01
	slider.value = p_value
	slider.custom_minimum_size.x = 240
	slider.value_changed.connect(p_changed)
	p_box.add_child(slider)
	return slider

func _make_player() -> void:
	player = DensityCharacter3D.new()
	player.name = "Player"
	player.radius = player_radius
	player.height = player_height
	player.floor_max_angle = deg_to_rad(50.0)
	player.floor_snap_length = player_radius
	player.step_height = STEP_HEIGHT
	camera.get_parent().add_child(player)
	player.global_position = camera.global_position - Vector3.UP * player_height

func _notify(p_text: String) -> void:
	notice = p_text
	notice_until = Time.get_ticks_msec() + 2500

## A frog (forg.tscn) or a chiggin (chiggin.tscn), whichever Shift+G last
## picked, dropped where the crosshair points.
func _spawn_frog() -> void:
	var hit = terrain.raycast(camera.global_position, -camera.global_basis.z, reach)
	var spawn: Vector3 = hit + Vector3.UP * 20.0 if hit != null else camera.global_position - camera.global_basis.z * 30.0
	creatures.spawn(CHIGGIN_SCENE if creature == 1 else forg_scene, spawn, randf() * TAU)

## Whether the mouse is the game's (captured), so the buttons sculpt.
## assume_captured: as if it were (a headless test run can't capture it).
var assume_captured := false
func _captured() -> bool:
	return assume_captured or Input.mouse_mode == Input.MOUSE_MODE_CAPTURED

func _sculpt(delta: float) -> void:
	var look := -camera.global_basis.z
	var sculpting := _captured() and not capture_click and not hud.radial.visible and not hoisting
	var held := sculpting and _button(MOUSE_BUTTON_LEFT)
	var pressing := held or (sculpting and _button(MOUSE_BUTTON_RIGHT))
	if tool in [Tool.PHYSICS_GUN, Tool.TRANSFORM, Tool.GEOMETRY, Tool.SELECT_GEOMETRY, Tool.BOX]:
		pressing = false  # these don't sculpt
	if pressing and not stroking:
		terrain.begin_stroke()
		paint_seed += 1
	elif stroking and not pressing:
		terrain.end_stroke()
		_release_held_body()
		terrain.clear_flatten_plane()
		flatten_locked = false
	stroking = pressing
	var hit = null
	if dragging:
		if held:
			hit = _drag(camera.global_position + look * drag_distance)
		else:
			_end_drag()
	if twisting:
		if held:
			hit = _twist()
		else:
			_end_twist()
	target_body = null
	if not dragging and not twisting:
		hit = terrain.raycast(camera.global_position, look, reach)
		# A body (ball, frog) in front of the ground is sculpted instead.
		var nearest: float = reach if hit == null else camera.global_position.distance_to(hit)
		var aimed := DensityBody3D.raycast_bodies(camera.global_position, look, nearest)
		if not aimed.is_empty():
			target_body = aimed.body
			hit = aimed.position
		if target_body != null and pressing and held_body == null and (TOOL_MODES[tool] >= 0 or tool in [Tool.GRAB, Tool.SNAKE_HOOK, Tool.ROTATE, Tool.SCALE, Tool.PAINT_FILL]):
			_hold_body(target_body)
		if hit != null and held and (tool == Tool.GRAB or tool == Tool.SNAKE_HOOK):
			_begin_drag(hit)
		elif hit != null and held and tool in [Tool.ROTATE, Tool.SCALE]:
			_begin_twist(hit)
	brush_preview.visible = hit != null
	if hit != null:
		brush_preview.global_position = hit
		brush_preview.scale = Vector3.ONE * _radius_on(target_body)

	# Multiplayer: a body being sculpted or dragged stays with us meanwhile.
	if held_body != null:
		network.use(held_body)
	if drag_body != null:
		network.use(drag_body)
	_select(hit, sculpting)
	_paint_fill(hit, held)
	_extrude(hit, held)
	var mode := -1
	var inverted := false
	if tool in NON_SCULPT_TOOLS:
		pass  # not a sculpting tool
	elif hit != null and sculpting and not dragging and not twisting:
		if _button(MOUSE_BUTTON_RIGHT):
			mode = TOOL_MODES[tool] if tool in INVERTIBLE_TOOLS else SculptTerrain.BRUSH_SUBTRACT
			inverted = tool in INVERTIBLE_TOOLS
		elif held and TOOL_MODES[tool] >= 0:
			mode = TOOL_MODES[tool]
		# Alt: the opposite, with any brush (Draw carves, Carve draws, Smooth
		# sharpens, Flatten pushes away from its plane, Layer lowers...).
		if mode >= 0 and _alt_held():
			if mode == SculptTerrain.BRUSH_ADD:
				mode = SculptTerrain.BRUSH_SUBTRACT
			elif mode == SculptTerrain.BRUSH_SUBTRACT:
				mode = SculptTerrain.BRUSH_ADD
			else:
				inverted = not inverted
	if mode < 0 or not stroking:
		lazy_point = null
		stroke_last = null
	elif lazy_mouse > 0.0:
		hit = _lazy(hit)
		brush_preview.global_position = hit
	if mode >= 0:
		if mode == SculptTerrain.BRUSH_CLONE and clone_source != null and clone_offset == null:
			clone_offset = clone_source - hit  # (the first stroke after Ctrl + click: kept after)
		# Clay strips lay their strips along the stroke.
		if stroke_last != null and hit.distance_to(stroke_last) > 0.05:
			terrain.brush_stroke_direction = (hit - stroke_last).normalized()
		stroke_last = hit
		_dab(hit, target_body, brush_strength * delta * (-1.0 if inverted else 1.0), mode)
		stroke_end = hit
	_update_clone_marker(hit if TOOL_MODES[tool] == SculptTerrain.BRUSH_CLONE else null)
	_update_symmetry_visuals(hit)

	info_label.text = _tool_hint()
	hud.refresh()

## One dab of the brush at p_hit, with its symmetry copies: on p_body if
## given (the copies that reach it), else the terrain. p_strength < 0: the
## brush's opposite (deflate, sharpen, lower...).
func _dab(p_hit: Vector3, p_body: DensityBody3D, p_strength: float, p_mode: int) -> void:
	var coloring := block == material_names.size()
	var material: int = SculptTerrain.color_to_material(paint_color) if coloring else block
	var copies := _symmetry_copies()
	if p_mode == SculptTerrain.BRUSH_FLATTEN:
		_pick_flatten_plane(p_hit, p_body)
	# Each copy copies from (and lays strips along) its own mirror of it.
	var offset: Vector3 = clone_offset if clone_offset != null else Vector3.ZERO
	var direction := terrain.brush_stroke_direction
	var indices := _body_copies(p_body, copies, p_hit) if p_body != null else _distinct_copies(copies, p_hit)
	for i in indices:
		var at: Vector3 = copies[i] * p_hit
		terrain.brush_clone_offset = copies[i].basis * offset
		terrain.brush_stroke_direction = copies[i].basis * direction
		var planed := p_mode == SculptTerrain.BRUSH_FLATTEN and flatten_locked
		if p_body != null:
			# In its voxels: the radius, and the strength (a distance for
			# some brushes, which the body turns into its voxels).
			var k := _voxel_scale(p_body)
			# (Flattening a body to its own face: its own surface's, each dab.)
			if planed and flatten_plane != FlattenPlane.FACE:
				p_body.set_flatten_plane(copies[i] * flatten_point, copies[i].basis * flatten_normal)
			if p_mode == SculptTerrain.BRUSH_PAINT and coloring:
				p_body.paint_color(at, brush_radius * k, paint_color, paint_opacity, paint_hardness, paint_scatter, paint_seed)
			else:
				p_body.apply_brush(at, brush_radius * k, p_strength * k, p_mode, material)
			p_body.clear_flatten_plane()
		elif p_mode == SculptTerrain.BRUSH_PAINT and coloring:
			terrain.paint_color(at, brush_radius, paint_color, paint_opacity, paint_hardness, paint_scatter, paint_seed)
		else:
			if planed:
				terrain.set_flatten_plane(copies[i] * flatten_point, copies[i].basis * flatten_normal)
			terrain.apply_brush(at, brush_radius, p_strength, p_mode, material)
	terrain.brush_clone_offset = offset
	terrain.brush_stroke_direction = direction

## Flatten's plane, picked where the stroke begins (flatten_plane): the face
## under the brush (moving onto another face, turned well away past a
## corner, flattens that one; Shift keeps the first), the view, or square to
## an axis (facing the side the ground there faces).
func _pick_flatten_plane(p_hit: Vector3, p_body: DensityBody3D) -> void:
	var here := _surface_normal(p_hit, p_body)
	match flatten_plane:
		FlattenPlane.FACE:
			if not flatten_locked or (not _shift_held() and here.angle_to(flatten_normal) > deg_to_rad(FLATTEN_NEW_FACE)):
				flatten_locked = true
				flatten_point = p_hit
				flatten_normal = here
		_:
			if not flatten_locked:
				flatten_locked = true
				flatten_point = p_hit
				if flatten_plane == FlattenPlane.VIEW:
					flatten_normal = camera.global_basis.z
				else:
					var axis: Vector3 = [Vector3.RIGHT, Vector3.UP, Vector3.BACK][flatten_plane - FlattenPlane.AXIS_X]
					flatten_normal = axis if axis.dot(here) >= 0.0 else -axis

## The surface's outward normal at a point (the general lie of it, over half
## the brush): the terrain's, or a body's.
func _surface_normal(p_point: Vector3, p_body: DensityBody3D) -> Vector3:
	if p_body == null:
		return terrain.get_normal(p_point, brush_radius * 0.5)
	var h := maxf(_radius_on(p_body) * 0.5, 0.5 * _voxel_scale(p_body))
	var gradient := Vector3(
			p_body.get_density_at(p_point + Vector3(h, 0, 0)) - p_body.get_density_at(p_point - Vector3(h, 0, 0)),
			p_body.get_density_at(p_point + Vector3(0, h, 0)) - p_body.get_density_at(p_point - Vector3(0, h, 0)),
			p_body.get_density_at(p_point + Vector3(0, 0, h)) - p_body.get_density_at(p_point - Vector3(0, 0, h)))
	return -gradient.normalized() if gradient.length_squared() > 1e-10 else Vector3.UP

## Where a line from p_from (along p_dir, up to p_length) meets p_body, or
## the terrain if null. null if it doesn't.
func _surface_along(p_from: Vector3, p_dir: Vector3, p_length: float, p_body: DensityBody3D):
	if p_body != null:
		var aimed := DensityBody3D.raycast_bodies(p_from, p_dir, p_length)
		return aimed.position if not aimed.is_empty() and aimed.body == p_body else null
	return terrain.raycast(p_from, p_dir, p_length)

## Lazy mouse: the brush trails the crosshair's p_hit on a string (lazy_mouse
## x 3 brush radii), kept on the surface as seen from the camera.
func _lazy(p_hit: Vector3) -> Vector3:
	if lazy_point == null:
		lazy_point = p_hit
		return p_hit
	var length := lazy_mouse * _radius_on(target_body) * 3.0
	var d: Vector3 = p_hit - lazy_point
	if d.length() > length:
		lazy_point += d.normalized() * (d.length() - length)
	var from := camera.global_position
	var on = _surface_along(from, (lazy_point - from).normalized(), from.distance_to(lazy_point) + _radius_on(target_body) * 2.0, target_body)
	if on != null:
		lazy_point = on
	return lazy_point

## Shift + click with a brush: a straight stroke from p_from (where the last
## ended) to the crosshair, laid along the surface, as one undo step.
const LINE_DAB := 1.0 / 30.0  # each dab's strength, in seconds of holding the brush
func _brush_line(p_from: Vector3) -> void:
	var from := camera.global_position
	var look := -camera.global_basis.z
	var to = terrain.raycast(from, look, reach)
	var body: DensityBody3D = null
	var aimed := DensityBody3D.raycast_bodies(from, look, reach if to == null else from.distance_to(to))
	if not aimed.is_empty():
		body = aimed.body
		to = aimed.position
	if to == null:
		return
	capture_click = true  # (the click isn't a stroke of its own too)
	var mode: int = TOOL_MODES[tool]
	var inverted := _alt_held()
	if inverted and mode == SculptTerrain.BRUSH_ADD:
		mode = SculptTerrain.BRUSH_SUBTRACT
		inverted = false
	if mode == SculptTerrain.BRUSH_CLONE and clone_source != null and clone_offset == null:
		clone_offset = clone_source - p_from
	if to.distance_to(p_from) > 0.01:
		terrain.brush_stroke_direction = (to - p_from).normalized()
	terrain.begin_stroke()
	if body != null:
		_hold_body(body)
	flatten_locked = false
	var spacing := maxf(_radius_on(body) * 0.25, 0.5 * _voxel_scale(body))
	var steps := maxi(ceili(p_from.distance_to(to) / spacing), 1)
	for k in steps + 1:
		var q: Vector3 = p_from.lerp(to, float(k) / steps)
		var on = _surface_along(from, (q - from).normalized(), from.distance_to(q) + _radius_on(body) * 2.0, body)
		_dab(on if on != null else q, body, brush_strength * LINE_DAB * (-1.0 if inverted else 1.0), mode)
	_release_held_body()
	terrain.end_stroke()
	terrain.clear_flatten_plane()
	flatten_locked = false
	stroke_end = to

## Shift + click with Grab: the ground where the last grab ended pulled
## to the crosshair, in one go.
func _grab_line() -> void:
	var from := camera.global_position
	var look := -camera.global_basis.z
	var to = terrain.raycast(from, look, reach)
	if to == null:
		to = from + look * from.distance_to(grab_end)
	capture_click = true
	var copies := _symmetry_copies()
	var centers := PackedVector3Array()
	var deltas := PackedVector3Array()
	for i in _distinct_copies(copies, grab_end):
		centers.append(copies[i] * grab_end)
		deltas.append(copies[i].basis * (to - grab_end))
	terrain.begin_stroke()
	terrain.begin_grab()
	terrain.warp(centers, deltas, brush_radius, true)
	terrain.end_grab()
	terrain.end_stroke()
	grab_end = to

## Ctrl + click with Clone: copies from here (the next stroke fixes where to).
func _set_clone_source() -> void:
	var from := camera.global_position
	var look := -camera.global_basis.z
	var hit = terrain.raycast(from, look, reach)
	var aimed := DensityBody3D.raycast_bodies(from, look, reach if hit == null else from.distance_to(hit))
	if not aimed.is_empty():
		hit = aimed.position
	capture_click = true
	if hit == null:
		return
	clone_source = hit
	clone_offset = null
	_notify("Clone: copies from here; where your next stroke begins gets what's here")

## Clone's marker: where the brush copies from (or the source, before the
## first stroke).
func _update_clone_marker(p_hit) -> void:
	if clone_marker == null:
		clone_marker = MeshInstance3D.new()
		var mesh := SphereMesh.new()
		mesh.radius = 1.0
		mesh.height = 2.0
		var look := StandardMaterial3D.new()
		look.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		look.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
		look.albedo_color = Color(1.0, 0.55, 0.15, 0.35)
		look.no_depth_test = true
		mesh.material = look
		clone_marker.mesh = mesh
		brush_preview.get_parent().add_child(clone_marker)
	clone_marker.visible = p_hit != null and clone_source != null
	if clone_marker.visible:
		clone_marker.global_position = p_hit + clone_offset if clone_offset != null else clone_source
		clone_marker.scale = Vector3.ONE * maxf(_radius_on(target_body) * 0.3, 0.5 * _voxel_scale(target_body))

## The wheel eases the radius and strength to what it's set them to.
func _ease_brush(delta: float) -> void:
	var k := 1.0 - exp(-delta * 14.0)
	if radius_target > 0.0:
		brush_radius = lerpf(brush_radius, radius_target, k) if smooth_wheel else radius_target
		if absf(brush_radius - radius_target) < 0.01:
			brush_radius = radius_target
			radius_target = -1.0
	if strength_target > 0.0:
		brush_strength = lerpf(brush_strength, strength_target, k) if smooth_wheel else strength_target
		if absf(brush_strength - strength_target) < 0.01:
			brush_strength = strength_target
			strength_target = -1.0

## The brush settings panel (N): on or off, freeing the mouse for it.
## Removes every object in the world: balls, creatures, cut-off pieces,
## imports, near or put away far off (not the floating piece, which is still
## part of the ground). Other players see them go too (network.gd notices).
## Ctrl+Z brings them all back. Returns how many went.
func delete_all_objects() -> int:
	var count := objects.unloaded.size()
	var unloaded := objects.unloaded.duplicate()
	objects.unloaded.clear()
	var list: Array = []
	for parent in [objects, camera.get_parent()]:
		for b in parent.get_children():
			if b is DensityBody3D and not b is DensityCharacter3D and b.pickable and b != floating \
					and not b.has_meta("net_avatar") and not b.is_queued_for_deletion():
				list.append(b)
	count += list.size()
	_delete_bodies(list, unloaded)
	return count

## Takes bodies out of the world as one undo step (X, Delete all objects):
## kept out of the tree, not freed, so Ctrl+Z puts them back as they were
## (p_unloaded: put-away objects' entries that went with them). They're
## freed once the step falls out of the history.
func _delete_bodies(p_bodies: Array, p_unloaded: Array = []) -> void:
	var entry := {"deleted": [], "unloaded": p_unloaded}
	for b in p_bodies:
		if not is_instance_valid(b) or not b.is_inside_tree() or b.is_queued_for_deletion():
			continue
		_take_out(b)
		entry.deleted.append({"body": b, "parent": b.get_parent()})
	for r in entry.deleted:
		r.parent.remove_child(r.body)
	if not entry.deleted.is_empty() or not p_unloaded.is_empty():
		_push_history(entry)

## Lets go of a body about to leave the world (editor, physics gun, brush,
## selection, transform).
func _take_out(p_body: DensityBody3D) -> void:
	if editor != null and editor.active and editor.body == p_body:
		editor.close()
	if p_body == held_body:
		_release_held_body()
	if p_body == gun_body:
		gun_body = null
	if p_body == gizmo_body:
		_drop_gizmo_body()
	if p_body in selected_bodies:
		_select_body(p_body, false)
	p_body.highlighted = false
	objects.bodies.erase(p_body)

func _undelete(p_entry: Dictionary) -> void:
	for r in p_entry.deleted:
		if not is_instance_valid(r.body) or r.body.is_inside_tree():
			continue
		var b: DensityBody3D = r.body
		var parent: Node = r.parent if is_instance_valid(r.parent) else objects
		# (A new object to the other players: network.gd gives it a new id.)
		for meta in ["net_id", "net_owner", "net_sim", "net_shape"]:
			b.remove_meta(meta)
		parent.add_child(b)
		if parent == objects:
			objects.bodies.append(b)
		b.sleeping = false
	objects.unloaded.append_array(p_entry.unloaded)

## Frees what an undo step kept that nothing can bring back now (out of
## the world, and no step left in the history deleted it).
func _forget_entry(p_entry: Dictionary) -> void:
	for r in p_entry.get("deleted", []):
		if not is_instance_valid(r.body) or r.body.is_inside_tree():
			continue
		var held := false
		for e in history:
			for other in e.get("deleted", []):
				held = held or other.body == r.body
		if not held:
			r.body.free()

func toggle_brush_panel() -> void:
	brush_panel.visible = not brush_panel.visible
	if brush_panel.visible:
		brush_panel.refresh()
	if _frees_mouse():
		Input.mouse_mode = Input.MOUSE_MODE_VISIBLE if brush_panel.visible else Input.MOUSE_MODE_CAPTURED
	get_viewport().gui_release_focus()

## The brush preview's shape: a ball, or a cube for the cube brush.
func update_brush_preview_shape() -> void:
	if terrain.brush_shape == 1:
		if not brush_preview.mesh is BoxMesh:
			var box := BoxMesh.new()
			box.size = Vector3.ONE * 2.0
			brush_preview.set_meta("sphere_mesh", brush_preview.mesh)
			brush_preview.mesh = box
	elif brush_preview.has_meta("sphere_mesh"):
		brush_preview.mesh = brush_preview.get_meta("sphere_mesh")
		brush_preview.remove_meta("sphere_mesh")

## The brush settings (N), kept between sessions.
func save_brush_settings() -> void:
	var config := ConfigFile.new()
	config.set_value("brush", "falloff", terrain.brush_falloff)
	config.set_value("brush", "shape", terrain.brush_shape)
	config.set_value("brush", "blocky", terrain.blocky_size)
	config.set_value("brush", "blocky_on", blocky_size_on)
	config.set_value("brush", "flatten_mode", terrain.flatten_mode)
	config.set_value("brush", "flatten_plane", flatten_plane)
	config.set_value("brush", "layer_height", terrain.brush_layer_height)
	config.set_value("brush", "lazy_mouse", lazy_mouse)
	config.set_value("brush", "smooth_wheel", smooth_wheel)
	config.save(SETTINGS_PATH)

func load_brush_settings() -> void:
	var config := ConfigFile.new()
	if config.load(SETTINGS_PATH) != OK:
		return
	terrain.brush_falloff = config.get_value("brush", "falloff", 0)
	terrain.brush_shape = config.get_value("brush", "shape", 0)
	terrain.blocky_size = config.get_value("brush", "blocky", 0)
	blocky_size_on = config.get_value("brush", "blocky_on", 2)
	terrain.flatten_mode = config.get_value("brush", "flatten_mode", 0)
	flatten_plane = config.get_value("brush", "flatten_plane", FlattenPlane.FACE)
	terrain.brush_layer_height = config.get_value("brush", "layer_height", 2.0)
	lazy_mouse = config.get_value("brush", "lazy_mouse", 0.0)
	smooth_wheel = config.get_value("brush", "smooth_wheel", true)
	update_brush_preview_shape()

func _begin_drag(p_hit: Vector3) -> void:
	dragging = true
	drag_start = p_hit
	drag_distance = camera.global_position.distance_to(p_hit)
	drag_copies = _symmetry_copies()
	last_grab_delta = Vector3.INF
	drag_body = target_body
	if drag_body != null:
		# Symmetry on a body too: the copies that start on it.
		var on_body: Array[Transform3D] = []
		for i in _body_copies(drag_body, drag_copies, p_hit):
			on_body.append(drag_copies[i])
		drag_copies = on_body
		if tool == Tool.GRAB:
			drag_body.begin_grab()
	elif tool == Tool.GRAB:
		terrain.begin_grab()

func _end_drag() -> void:
	_end_twist()
	if dragging and tool == Tool.SNAKE_HOOK and drag_body == null:
		snake_last = drag_start
	if dragging and tool == Tool.GRAB:
		if drag_body == null:
			grab_end = camera.global_position - camera.global_basis.z * drag_distance  # (Shift + click pulls on from here)
		if drag_body != null:
			if is_instance_valid(drag_body):
				drag_body.end_grab()
		else:
			terrain.end_grab()
	dragging = false
	drag_body = null
	_release_held_body()

## Moves the dragged brush towards p_target. Returns where the brush is now.
func _drag(p_target: Vector3) -> Vector3:
	var kept := _distinct_copies(drag_copies, drag_start)
	if tool == Tool.GRAB:
		# Every frame re-applies the whole move since the grab began.
		var delta := p_target - drag_start
		if delta.distance_to(last_grab_delta) > 0.05:
			last_grab_delta = delta
			var centers := PackedVector3Array()
			var deltas := PackedVector3Array()
			for i in kept:
				centers.append(drag_copies[i] * drag_start)
				deltas.append(drag_copies[i].basis * delta)
			if drag_body != null:
				if is_instance_valid(drag_body):
					drag_body.warp_copies(centers, deltas, _radius_on(drag_body), true)
				return p_target
			terrain.warp(centers, deltas, brush_radius, true)
		return p_target
	# Snake hook: pulls material along in short steps.
	var max_step := minf(_radius_on(drag_body) * SNAKE_STEP, 8.0 * _voxel_scale(drag_body))
	for step_index in SNAKE_STEPS_PER_FRAME:
		var move := p_target - drag_start
		if move.length() < 0.05:
			break
		var step := move.limit_length(max_step)
		drag_start += step
		var centers := PackedVector3Array()
		var deltas := PackedVector3Array()
		for i in kept:
			centers.append(drag_copies[i] * drag_start)
			deltas.append(drag_copies[i].basis * step)
		if drag_body != null:
			if is_instance_valid(drag_body):
				drag_body.warp_copies(centers, deltas, _radius_on(drag_body), false)
			continue
		terrain.warp(centers, deltas, brush_radius, false)
	return drag_start

func _begin_twist(p_hit: Vector3) -> void:
	twisting = true
	twist_center = p_hit
	twist_copies = []
	twist_body = target_body
	var copies := _symmetry_copies()
	var kept := _distinct_copies(copies, p_hit) if twist_body == null else _body_copies(twist_body, copies, p_hit)
	for i in kept:
		twist_copies.append(copies[i])
	twist_turn = Basis()
	twist_scale = 1.0
	twist_sent = Basis()
	twist_view = camera.global_basis.orthonormalized()
	if twist_body != null:
		twist_body.begin_grab()
	else:
		terrain.begin_grab()

func _end_twist() -> void:
	if twisting:
		if twist_body != null:
			if is_instance_valid(twist_body):
				twist_body.end_grab()
			_release_held_body()  # (split off what it cut loose; undo)
		else:
			terrain.end_grab()
	twisting = false
	twist_body = null

## The mouse while rotating or scaling (the view holds still meanwhile).
func _twist_input(p_relative: Vector2, p_roll: bool) -> void:
	if tool == Tool.SCALE:
		twist_scale = clampf(twist_scale * exp((p_relative.x - p_relative.y) * 0.004), 0.1, 6.0)
		return
	var turn := 0.008
	if p_roll:
		twist_turn = Basis(-twist_view.z, -p_relative.x * turn) * twist_turn
	else:
		twist_turn = Basis(twist_view.y, p_relative.x * turn) * twist_turn
		twist_turn = Basis(twist_view.x, p_relative.y * turn) * twist_turn
	twist_turn = twist_turn.orthonormalized()

## Re-applies the whole turn / scale since the twist began (from the
## terrain as it was then). Returns where the brush is.
func _twist() -> Vector3:
	var change := twist_turn.scaled(Vector3.ONE * twist_scale)
	if not change.is_equal_approx(twist_sent):
		twist_sent = change
		var centers := PackedVector3Array()
		var bases := []
		for copy in twist_copies:
			centers.append(copy * twist_center)
			bases.append(copy.basis * change * copy.basis.inverse())
		if twist_body != null:
			if is_instance_valid(twist_body):
				twist_body.twist(centers, bases, _radius_on(twist_body))
		else:
			terrain.twist(centers, bases, brush_radius)
	return twist_center

## Holds a body still while it's sculpted (see held_body).
func _hold_body(p_body: DensityBody3D) -> void:
	network.use(p_body)  # (multiplayer: sculpted here, so simulated here)
	held_body = p_body
	held_was_frozen = p_body.freeze
	p_body.freeze = true
	# Shared again, so the stroke edits a copy and this one stays as it was.
	held_shape_before = p_body.shape
	p_body.shape = held_shape_before

## Ends a stroke on a body: lets it go, splits off parts the stroke cut
## loose (each its own body), and records it for undo.
func _release_held_body() -> void:
	if held_body != null and is_instance_valid(held_body):
		held_body.freeze = held_was_frozen
		held_body.sleeping = false  # its new shape may not rest as it is
		if held_body.shape != held_shape_before:
			var part_shapes := held_body.split_disconnected()
			var entry := {"body": held_body, "before": held_shape_before, "after": held_body.shape,
					"part_shapes": part_shapes, "part_transform": held_body.global_transform, "parts": []}
			_spawn_parts(entry)
			_push_history(entry)
	held_body = null

## The bodies of a split entry's parts, placed as the body was.
func _spawn_parts(p_entry: Dictionary) -> void:
	var parts: Array = []
	var from: DensityBody3D = p_entry.body
	for part_shape in p_entry.part_shapes:
		var part := objects.add_shape_body(part_shape, Vector3.ZERO)
		part.global_transform = p_entry.part_transform
		if is_instance_valid(from):
			part.linear_velocity = from.linear_velocity
		parts.append(part)
	p_entry.parts = parts

func _push_history(p_entry: Dictionary) -> void:
	history.append(p_entry)
	var dropped: Array = future.duplicate()
	future.clear()
	if history.size() > MAX_HISTORY:
		dropped.append(history.pop_front())
	for e in dropped:
		_forget_entry(e)

## Terrain strokes and selection changes since last time join the history.
## A selection change that came with a terrain stroke (placing a piece,
## extruding, P) goes with it as one step; one made with the mouse waits
## until the buttons are up (a select paint stroke is one step). While a
## piece floats, the selection waits for it to be placed (or the lift undone).
func _record_history(p_force := false) -> void:
	if sel_serial < 0:
		_sync_selection()
	var before_count := history.size()
	_record_terrain_history()
	var serial: int = terrain.get_selection_serial()
	if serial == sel_serial or floating != null:
		return
	if not p_force and (_button(MOUSE_BUTTON_LEFT) or _button(MOUSE_BUTTON_RIGHT)):
		return
	var state: Array = terrain.get_selection_state()
	if history.size() > before_count:
		history.back().sel_before = sel_state
		history.back().sel_after = state
	else:
		_push_history({"selection": true, "sel_before": sel_state, "sel_after": state})
	sel_state = state
	sel_serial = serial

## History's idea of the selection is what it is now.
func _sync_selection() -> void:
	sel_state = terrain.get_selection_state()
	sel_serial = terrain.get_selection_serial()

func _set_selection(p_state: Array) -> void:
	if not p_state.is_empty():
		terrain.set_selection_state(p_state)
	_sync_selection()

## Terrain strokes that ended since last time join the history.
func _record_terrain_history() -> void:
	var serial: int = terrain.get_undo_serial()
	while terrain_serial < serial:
		terrain_serial += 1
		_push_history({"terrain": true})

## Ctrl+Z / Ctrl+Y: the newest terrain, body, selection or transform edit,
## whichever it was. While a piece floats, they step through its moves; past
## the first, Ctrl+Z puts it back where it was lifted from.
func _undo() -> void:
	if floating != null:
		if not history.is_empty() and history.back().get("floating") == floating:
			var move: Dictionary = history.pop_back()
			floating.global_transform = move.before
			future.append(move)
		else:
			_cancel_lift()
		return
	_record_history(true)
	if history.is_empty():
		return
	var entry: Dictionary = history.pop_back()
	if entry.has("terrain"):
		terrain.undo()
	elif entry.has("body") and is_instance_valid(entry.body):
		entry.body.shape = entry.before
		entry.body.sleeping = false
		for part in entry.parts:
			if is_instance_valid(part):
				objects.remove(part)
		entry.parts = []
	elif entry.has("deleted"):
		_undelete(entry)
	elif entry.has("moved"):
		if is_instance_valid(entry.moved):
			entry.moved.global_transform = entry.before
			entry.moved.sleeping = false
	if entry.has("sel_before"):
		_set_selection(entry.sel_before)
	future.append(entry)

func _redo() -> void:
	if future.is_empty():
		return
	if floating != null:
		# Only the floating piece's own moves, while it floats.
		if future.back().get("floating") == floating:
			var move: Dictionary = future.pop_back()
			floating.global_transform = move.after
			history.append(move)
		return
	var entry: Dictionary = future.pop_back()
	if entry.has("terrain"):
		terrain.redo()
	elif entry.has("body") and is_instance_valid(entry.body):
		entry.body.shape = entry.after
		entry.body.sleeping = false
		_spawn_parts(entry)
	elif entry.has("deleted"):
		var gone: Array = []
		for r in entry.deleted:
			if is_instance_valid(r.body) and r.body.is_inside_tree():
				_take_out(r.body)
				gone.append(r.body)
		for b in gone:
			b.get_parent().remove_child(b)
		for e in entry.unloaded:
			objects.unloaded.erase(e)
	elif entry.has("moved"):
		if is_instance_valid(entry.moved):
			entry.moved.global_transform = entry.after
			entry.moved.sleeping = false
	if entry.has("sel_after"):
		_set_selection(entry.sel_after)
	history.append(entry)

## Drops the floating piece's moves from the history (it's been placed).
func _forget_floating_moves() -> void:
	history = history.filter(func(e: Dictionary) -> bool: return not e.has("floating"))
	future = future.filter(func(e: Dictionary) -> bool: return not e.has("floating"))

## Ctrl+Z on a floating piece with no moves left: it goes back into the
## ground where it was lifted from, selected as it was (a pasted copy just
## goes away).
func _cancel_lift() -> void:
	floating.queue_free()
	floating = null
	gizmo_axis = -1
	_forget_floating_moves()
	var serial: int = terrain.get_undo_serial()
	terrain.set_stroke_held(false)
	terrain.end_stroke()
	if terrain.get_undo_serial() > serial:
		terrain.undo()  # the lift's carve
	terrain_serial = terrain.get_undo_serial()
	future.clear()  # (the terrain's redo now holds the lift, not what these were)
	_set_selection(lift_sel_state)

## The selection tools: Select paint selects ground under the brush (right
## button deselects); Select fill, once per click, the solid connected to
## where you click, or (right button) just what's coloured like it, within
## brush_strength / 32 (an RGB distance: raise the strength to take more).
func _select(p_hit, p_sculpting: bool) -> void:
	var left := p_sculpting and _button(MOUSE_BUTTON_LEFT)
	var right := p_sculpting and _button(MOUSE_BUTTON_RIGHT)
	if not (left or right):
		fill_clicked = false
	if not (left or right):
		body_clicked = false
	if target_body != null and tool in [Tool.SELECT_PAINT, Tool.SELECT_FILL]:
		if (left or right) and not body_clicked:
			body_clicked = true
			_select_body(target_body, left)
		return
	if p_hit == null or body_clicked:
		return  # (body_clicked: a click that began on a body)
	if (left or right) and not selected_bodies.is_empty():
		_clear_body_selection()  # (ground and bodies aren't selected together)
	if tool == Tool.SELECT_PAINT and (left or right):
		terrain.select_sphere(p_hit, brush_radius, left)
	elif tool == Tool.SELECT_FILL and (left or right) and not fill_clicked:
		fill_clicked = true
		var radius := minf(brush_radius * FILL_RADII, FILL_MAX)
		if left:
			terrain.select_connected(p_hit, radius, true)
		else:
			terrain.select_similar(p_hit, radius, brush_strength / 32.0, true)

## Paint fill, once per click: at p_hit and its symmetry copies, on the
## aimed object (the copies that land on it) or the ground, with the block
## or color Draw builds with (a color at paint_opacity, scattered).
func _paint_fill(p_hit, p_held: bool) -> void:
	if tool != Tool.PAINT_FILL or not p_held:
		paint_fill_clicked = false
		return
	if p_hit == null or paint_fill_clicked:
		return
	paint_fill_clicked = true
	var coloring := block == material_names.size()
	var material: int = -1 if coloring else block
	var threshold := brush_strength / 32.0
	var copies := _symmetry_copies()
	var indices := _body_copies(target_body, copies, p_hit) if target_body != null else _distinct_copies(copies, p_hit)
	for i in indices:
		var at: Vector3 = copies[i] * p_hit
		if target_body != null:
			target_body.paint_fill(at, threshold, material, paint_color, paint_opacity, paint_scatter, paint_seed)
		else:
			terrain.paint_fill(at, minf(brush_radius * FILL_RADII, FILL_MAX), threshold, material, paint_color, paint_opacity, paint_scatter, paint_seed)

func _select_body(p_body: DensityBody3D, p_on: bool) -> void:
	if p_on:
		terrain.clear_selection()
		if not p_body in selected_bodies:
			selected_bodies.append(p_body)
		p_body.highlighted = true
	else:
		selected_bodies.erase(p_body)
		p_body.highlighted = false

func _clear_body_selection() -> void:
	for b in selected_bodies:
		if is_instance_valid(b):
			b.highlighted = false
	selected_bodies.clear()

## Drops selected bodies that are gone, and all of them once ground is
## selected (by any tool).
func _update_body_selection() -> void:
	for i in range(selected_bodies.size() - 1, -1, -1):
		if not is_instance_valid(selected_bodies[i]) or not selected_bodies[i].is_inside_tree():
			selected_bodies.remove_at(i)
	if not selected_bodies.is_empty() and terrain.has_selection():
		_clear_body_selection()

## P with bodies selected: each becomes ground where it is (selected, to
## move on with), and goes. Its grid is lined up with the terrain's (and
## turned square to it when it's nearly so), so no detail is lost to
## resampling. Ctrl+Z takes the ground away again, but doesn't bring the
## body back.
func _bodies_to_terrain() -> void:
	terrain.begin_stroke()
	for body in selected_bodies:
		if not is_instance_valid(body) or body.shape == null:
			continue
		network.use(body)
		terrain.stamp_shape(body.shape, _snapped_transform(body), -1, true, body.pose)
		body.highlighted = false
		objects.remove(body)
	terrain.end_stroke()
	selected_bodies.clear()

func _snapped_transform(p_body: DensityBody3D) -> Transform3D:
	var t := p_body.global_transform
	var sc := t.basis.get_scale()
	var rot := t.basis.orthonormalized()
	# Within 10 degrees of square to the world: square.
	var square := Basis()
	var ok := true
	for a in 3:
		var column := rot[a]
		var axis := Vector3.ZERO
		var biggest := column.abs().max_axis_index()
		axis[biggest] = signf(column[biggest])
		ok = ok and column.angle_to(axis) < deg_to_rad(10.0)
		square[a] = axis
	if ok and absf(square.determinant() - 1.0) < 0.01:
		rot = square
	var uniform := sc.x if is_equal_approx(sc.x, sc.y) and is_equal_approx(sc.y, sc.z) else 0.0
	if uniform > 0.0 and absf(uniform - roundf(uniform)) < 0.05:
		sc = Vector3.ONE * maxf(roundf(uniform), 1.0)  # (whole sizes: voxel for voxel)
	var b := Basis(rot[0] * sc.x, rot[1] * sc.y, rot[2] * sc.z)
	# The shape's first grid point on a whole voxel of the terrain.
	var first := b * p_body.shape.get_origin()
	return Transform3D(b, (t.origin + first).round() - first)

## P: the selection becomes a physics object (taken out of the terrain:
## Ctrl+Z puts the ground back, but not the object away); with bodies
## selected, they become ground instead.
func _selection_to_body() -> void:
	if not selected_bodies.is_empty():
		_bodies_to_terrain()
		return
	if not terrain.has_selection():
		return
	var shape: DensityShape = terrain.selection_to_shape(true)
	terrain.clear_selection()
	if shape != null:
		objects.add_shape_body(shape, terrain.get_selection_center())

## The physics gun (tool 9): the held body is pulled towards the point
## gun_distance in front of the camera (the wheel moves it), spinning down;
## letting go throws it with the speed it has. A frozen one just moves there.
func _update_physics_gun(p_delta: float) -> void:
	if gun_body == null:
		return
	if not is_instance_valid(gun_body) or tool != Tool.PHYSICS_GUN or not _button(MOUSE_BUTTON_LEFT):
		gun_body = null
		return
	network.use(gun_body)  # (multiplayer: stays with us while held)
	var target := camera.global_position - camera.global_basis.z * gun_distance
	if gun_body.freeze:
		if gun_turned:
			gun_body.global_basis = gun_basis * Basis.from_scale(gun_body.global_basis.get_scale())
		gun_body.global_position += target - gun_body.to_global(gun_point)
	else:
		var point := gun_body.to_global(gun_point)
		gun_body.linear_velocity = (target - point) * 12.0 + Vector3.UP * gravity * gun_body.gravity_scale * p_delta
		if gun_turned:
			# Spun towards the turn it's held at (the grabbed point is pulled
			# back to the crosshair meanwhile).
			var q := (gun_basis * gun_body.global_basis.orthonormalized().inverse()).get_rotation_quaternion()
			if q.w < 0.0:
				q = -q
			var angle := q.get_angle()
			gun_body.angular_velocity = q.get_axis() * angle * 12.0 if angle > 1e-4 else Vector3.ZERO
		else:
			gun_body.angular_velocity *= 0.85

## Where Ctrl+P and Ctrl+O start: handmade features' shapes
## (handmade_feature.gd) are kept there.
const FEATURE_SHAPES_DIR := "res://assets/features/"

## Ctrl+P: saves the selection (shape, colours and materials) as a
## DensityShape file, which a body's `shape` can use (a frog's, say), or a
## handmade feature (scripts/features/handmade/).
func _export_selection() -> void:
	var shape: DensityShape = null
	if not selected_bodies.is_empty() and is_instance_valid(selected_bodies[0]):
		shape = selected_bodies[0].shape.duplicate_shape()  # a physics object's model (its keys too)
	elif terrain.has_selection():
		shape = terrain.selection_to_shape(false)
	if shape == null:
		return
	_open_file_dialog(FileDialog.FILE_MODE_SAVE_FILE, func(path: String) -> void:
		var error := ResourceSaver.save(shape, path)
		if error != OK:
			push_error("Couldn't save %s: %s" % [path, error_string(error)]))

## Ctrl+O: drops a saved shape in front of you as a physics object.
func _import_shape() -> void:
	_open_file_dialog(FileDialog.FILE_MODE_OPEN_FILE, func(path: String) -> void:
		var shape := load(path) as DensityShape
		if shape == null:
			push_error("%s isn't a density shape" % path)
			return
		objects.add_shape_body(shape.duplicate_shape(), camera.global_position - camera.global_basis.z * 30.0))

func _open_file_dialog(p_mode: FileDialog.FileMode, p_chosen: Callable) -> void:
	if file_dialog != null:
		file_dialog.queue_free()
	file_dialog = FileDialog.new()
	file_dialog.file_mode = p_mode
	file_dialog.access = FileDialog.ACCESS_FILESYSTEM
	file_dialog.filters = PackedStringArray(["*.res, *.tres ; Density shapes"])
	file_dialog.current_dir = ProjectSettings.globalize_path(FEATURE_SHAPES_DIR)
	file_dialog.use_native_dialog = true
	file_dialog.file_selected.connect(p_chosen)
	hud.add_child(file_dialog)
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	file_dialog.popup_centered_ratio(0.6)

## The player's size (shift + wheel, K / L): its capsule, speed, jump and
## step all scale with its height.
func _set_player_height(p_height: float) -> void:
	player_height = clampf(p_height, 2.0, 400.0)
	var size_scale := player_height / BASE_PLAYER_HEIGHT
	player.radius = maxf(player_radius * size_scale, 0.5)
	player.height = player_height
	player.floor_snap_length = player_radius * size_scale
	player.step_height = STEP_HEIGHT * size_scale

func _tool_hint() -> String:
	var hint := _key_hint()
	return touch.reword(hint) if touch != null else hint

func _key_hint() -> String:
	if Time.get_ticks_msec() < notice_until:
		return notice
	match tool:
		Tool.EXTRUDE:
			return "Extrude: drag on the selection, out or back in (within the brush radius; Shift: all of it)"
		Tool.SNAKE_HOOK:
			return "Snake hook: Shift + click pulls a straight one from where the last ended"
		Tool.FLATTEN:
			var to: String = ["the face under the brush (onto another face: that one; Shift keeps the first)", "a plane facing you",
					"a wall across x", "a floor", "a wall across z"][flatten_plane]
			var how: String = ["", ", only filling up to it", ", only scraping down to it"][terrain.flatten_mode]
			return "Flatten: to %s%s (Alt: away from it; N: plane and mode)" % [to, how]
		Tool.ROTATE:
			return "Rotate: hold on the ground or an object and move the mouse (Shift: roll about the view)"
		Tool.SCALE:
			return "Scale: hold on the ground or an object and move the mouse (right / up: bigger, left / down: smaller)"
		Tool.INFLATE:
			return "Inflate: puffs the surface out (RMB: in)"
		Tool.PINCH:
			return "Pinch: pulls the surface into a crease (RMB: spreads it)"
		Tool.BOX:
			if box_dragging:
				var size := _box_size()
				return "Box %d x %d x %d: let go to %s (wheel: far corner nearer / farther, RMB: give up)" % [size.x, size.y, size.z,
					"cut it out" if _shift_held() else "build it (Shift: cut it out)"]
			return "Box: drag from one corner to the other (Shift when you let go: cut it out)"
		Tool.SELECT_BOX:
			if box_dragging:
				var size := _box_size()
				return "Select box %d x %d x %d: let go to %s (wheel: far corner nearer / farther, the other button: give up)" % [size.x, size.y, size.z,
					"select what's inside" if box_select_add else "deselect what's inside"]
			return "Select box: drag corner to corner, LMB selects what's inside (ground, or objects), RMB deselects"
		Tool.LAYER:
			return "Layer: raises the ground %.1f above where it was, however often you go over it (RMB / Alt: lowers; N: height)" % terrain.brush_layer_height
		Tool.CLAY_STRIPS:
			return "Clay strips: builds flat strips along the stroke, layer on layer (RMB / Alt: scrapes)"
		Tool.CLONE:
			if clone_source == null:
				return "Clone: Ctrl + click where to copy from, then paint it on somewhere else"
			return "Clone: copies from %s (Ctrl + click: from somewhere else)" % ("the orange ball" if clone_offset != null else "where you Ctrl + clicked, to where you begin")
		Tool.DRAW:
			return "Draw (RMB / Alt: carve; Shift + click: a straight stroke from the last; N: brush settings)"
		Tool.SMOOTH:
			return "Smooth (Alt: sharpen; Shift + click: a straight stroke from the last)"
		Tool.PAINT:
			return "Paint (Shift + click: a straight stroke from the last; C: color, scatter)"
		Tool.PAINT_FILL:
			return "Paint fill: click paints the surface colored like where you click (strength %.0f: how alike; C: color, scatter)" % brush_strength
		Tool.GRAB:
			return "Grab: drag the ground (Shift + click: pulls where the last grab ended to here)"
		Tool.TRANSFORM:
			if floating == null and _gizmo_target() != null:
				return "Transform (%s, T switches; objects move and turn): drag a handle, or the object freely; click another to switch" % GIZMO_NAMES[gizmo_mode]
			if floating == null:
				return "Transform: click an object to move it, or select ground first"
			return "Transform (%s, T switches): drag a handle (or anywhere, freely); Enter places it, Shift+Enter cuts its shape out" % GIZMO_NAMES[gizmo_mode]
		Tool.GEOMETRY, Tool.SELECT_GEOMETRY:
			return "%d points (LMB place on the ground / drag, RMB remove, wheel: drag distance %.0f, Backspace clears): Enter %s" % [
				geo_points.size(), geo_distance, "builds the solid, Shift+Enter cuts it out" if tool == Tool.GEOMETRY else "selects the ground inside"]
	return ""

## Enter: places a transformed selection, or builds / selects with the
## geometry tool's points.
func _confirm(p_cut := false) -> void:
	if floating != null:
		if p_cut:
			# The floating shape's form taken out of the terrain where it is
			# (and the floating piece itself let go).
			terrain.carve_shape(floating.shape, floating.global_transform)
			floating.queue_free()
			floating = null
			_forget_floating_moves()
			gizmo_axis = -1
			terrain.set_stroke_held(false)
			terrain.end_stroke()
		else:
			_apply_floating()
	elif tool in [Tool.GEOMETRY, Tool.SELECT_GEOMETRY] and geo_preview != null:
		if tool == Tool.GEOMETRY:
			terrain.begin_stroke()
			if p_cut:
				terrain.carve_shape(geo_preview.shape, geo_preview.global_transform)
			else:
				terrain.stamp_shape(geo_preview.shape, geo_preview.global_transform, _current_material(), false)
			terrain.end_stroke()
		else:
			terrain.select_inside_shape(geo_preview.shape, geo_preview.global_transform, true)
		geo_points.clear()
		_rebuild_geometry()

## What Draw builds with: the block, or the paint colour.
func _current_material() -> int:
	return block if block < material_names.size() else SculptTerrain.color_to_material(paint_color)

# --- Transform ----------------------------------------------------------------

## Lifts the selection out of the terrain as `floating` (one undo step with
## placing it again).
func _lift_selection() -> void:
	if floating != null or not terrain.has_selection():
		return
	terrain.set_stroke_held(false)
	terrain.end_stroke()
	_record_history(true)
	lift_sel_state = sel_state
	terrain.begin_stroke()
	terrain.set_stroke_held(true)
	var shape: DensityShape = terrain.selection_to_shape(true)
	terrain.clear_selection()
	_make_floating(shape, terrain.get_selection_center())

func _make_floating(p_shape: DensityShape, p_position: Vector3) -> void:
	floating = DensityBody3D.new()
	floating.shape = p_shape
	floating.drawn = true
	floating.freeze = true
	floating.collide_with_bodies = false
	floating.pickable = false
	camera.get_parent().add_child(floating)
	floating.global_position = p_position

## Puts the floating selection into the terrain where it is now (selected).
func _apply_floating() -> void:
	if floating == null:
		return
	terrain.stamp_shape(floating.shape, floating.global_transform, -1, true)
	floating.queue_free()
	floating = null
	_forget_floating_moves()
	gizmo_axis = -1
	terrain.set_stroke_held(false)
	terrain.end_stroke()

func _copy_selection() -> void:
	if terrain.has_selection():
		clipboard = terrain.selection_to_shape(false)
		clipboard_center = terrain.get_selection_center()

## A copy of the clipboard where it was copied from, floating, to transform.
func _paste() -> void:
	if clipboard == null:
		return
	_apply_floating()
	terrain.set_stroke_held(false)
	terrain.end_stroke()
	_record_history(true)
	lift_sel_state = sel_state
	terrain.clear_selection()
	terrain.begin_stroke()
	terrain.set_stroke_held(true)
	_make_floating(clipboard.duplicate_shape(), clipboard_center)
	_end_drag()
	tool = Tool.TRANSFORM

## The gizmo's handles: an axis each (rings when rotating), scaled to stay
## the same size on screen, along the world's axes (the body's own when
## scaling).
func _update_gizmo() -> void:
	if gizmo_handles.is_empty():
		for i in 3:
			var handle := MeshInstance3D.new()
			handle.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
			var material := StandardMaterial3D.new()
			material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
			material.no_depth_test = true  # always visible, through the piece
			material.render_priority = 1
			handle.material_override = material
			brush_preview.get_parent().add_child(handle)
			gizmo_handles.append(handle)
	var target := _gizmo_target()
	var show := target != null
	for i in 3:
		gizmo_handles[i].visible = show
	if not show:
		gizmo_axis = -1
		return
	var origin := target.global_position
	var length := _gizmo_length()
	var axes := _gizmo_axes()
	if gizmo_axis < 0:
		gizmo_hover = _gizmo_pick()
	for i in 3:
		var handle := gizmo_handles[i]
		var color := [Color(1, 0.25, 0.25), Color(0.3, 1, 0.3), Color(0.35, 0.5, 1)][i] as Color
		if i == gizmo_axis or (gizmo_axis < 0 and i == gizmo_hover):
			color = Color(1, 0.9, 0.2)
		(handle.material_override as StandardMaterial3D).albedo_color = color
		if gizmo_mode == Gizmo.ROTATE:
			if not handle.mesh is TorusMesh:
				var torus := TorusMesh.new()
				torus.rings = 32
				torus.ring_segments = 6
				handle.mesh = torus
			(handle.mesh as TorusMesh).inner_radius = length * 0.94
			(handle.mesh as TorusMesh).outer_radius = length
			handle.global_transform = Transform3D(_basis_along(axes[i]), origin)
		else:
			if not handle.mesh is CylinderMesh:
				var line := CylinderMesh.new()
				line.radial_segments = 6
				line.rings = 1
				handle.mesh = line
			var mesh := handle.mesh as CylinderMesh
			mesh.height = length
			mesh.top_radius = length * (0.08 if gizmo_mode == Gizmo.SCALE else 0.03)  # a block end for scale
			mesh.bottom_radius = length * 0.03
			handle.global_transform = Transform3D(_basis_along(axes[i]), origin + axes[i] * length * 0.5)
	# Dragging.
	if gizmo_axis >= 0:
		if not _button(MOUSE_BUTTON_LEFT):
			gizmo_axis = -1
			if target == floating and not floating.global_transform.is_equal_approx(gizmo_start):
				_push_history({"floating": floating, "before": gizmo_start, "after": floating.global_transform})
			elif target == gizmo_body:
				_end_body_drag()
		else:
			_gizmo_drag()

## What the gizmo moves: the transform tool's floating piece, or the
## object clicked with it.
func _gizmo_target() -> Node3D:
	if tool != Tool.TRANSFORM:
		return null
	if floating != null:
		return floating
	if gizmo_body != null and (not is_instance_valid(gizmo_body) or not gizmo_body.is_inside_tree()):
		gizmo_body = null
	return gizmo_body

## Transform, left click: a handle of what the gizmo has, or an object
## (which it takes, dragged freely from there), or nothing (lets it go).
func _transform_press() -> void:
	if floating != null:
		_gizmo_press()
		return
	if _gizmo_target() != null and _gizmo_pick() >= 0:
		_gizmo_press()
		return
	var aimed := DensityBody3D.raycast_bodies(camera.global_position, -camera.global_basis.z, reach)
	var body: DensityBody3D = aimed.get("body") as DensityBody3D if not aimed.is_empty() else null
	if body == null or not body.pickable or body is DensityCharacter3D or body.has_meta("net_avatar"):
		_drop_gizmo_body()
		return
	_set_gizmo_body(body)
	_gizmo_press()

func _set_gizmo_body(p_body: DensityBody3D) -> void:
	if p_body == gizmo_body:
		return
	_drop_gizmo_body()
	gizmo_body = p_body
	p_body.highlighted = true

func _drop_gizmo_body() -> void:
	if gizmo_body == null:
		return
	if is_instance_valid(gizmo_body):
		if gizmo_axis >= 0:
			_end_body_drag()
		gizmo_body.highlighted = gizmo_body in selected_bodies
	gizmo_body = null
	gizmo_axis = -1

## A drag of gizmo_body ended: let go where it is (one undo step).
func _end_body_drag() -> void:
	gizmo_axis = -1
	if not is_instance_valid(gizmo_body):
		return
	gizmo_body.freeze = gizmo_body_frozen
	gizmo_body.linear_velocity = Vector3.ZERO
	gizmo_body.angular_velocity = Vector3.ZERO
	gizmo_body.sleeping = false
	if not gizmo_body.global_transform.is_equal_approx(gizmo_start):
		_push_history({"moved": gizmo_body, "before": gizmo_start, "after": gizmo_body.global_transform})

func _gizmo_length() -> float:
	return maxf(camera.global_position.distance_to(_gizmo_target().global_position) * 0.18, 2.0)

func _gizmo_axes() -> Array[Vector3]:
	if gizmo_mode == Gizmo.SCALE:
		var b := _gizmo_target().global_basis
		return [b.x.normalized(), b.y.normalized(), b.z.normalized()]
	return [Vector3.RIGHT, Vector3.UP, Vector3.BACK]

## The handle under the crosshair, or -1.
func _gizmo_pick() -> int:
	var from := camera.global_position
	var dir := -camera.global_basis.z
	if _gizmo_target() == null:
		return -1
	var origin := _gizmo_target().global_position
	var length := _gizmo_length()
	var axes := _gizmo_axes()
	var best := -1
	var best_distance := length * 0.12
	for i in 3:
		var miss: float
		if gizmo_mode == Gizmo.ROTATE:
			# The ring: where the ray meets its plane, how far from the circle.
			var hit = Plane(axes[i], origin).intersects_ray(from, dir)
			if hit == null:
				continue
			miss = absf((hit as Vector3).distance_to(origin) - length * 0.97)
		else:
			var s := _line_param(origin, axes[i], from, dir)
			s = clampf(s, 0.0, length)
			var point := origin + axes[i] * s
			miss = (point - from).cross(dir).length()  # distance from the ray
		if miss < best_distance:
			best_distance = miss
			best = i
	return best

## Where along the line p_origin + p_axis * s the ray comes closest: s.
func _line_param(p_origin: Vector3, p_axis: Vector3, p_from: Vector3, p_dir: Vector3) -> float:
	var w := p_origin - p_from
	var b := p_axis.dot(p_dir)
	var denominator := 1.0 - b * b
	if denominator < 1e-6:
		return 0.0
	return (b * p_dir.dot(w) - p_axis.dot(w)) / denominator

func _gizmo_press() -> void:
	var target := _gizmo_target()
	if target == null:
		return
	if target == gizmo_body and gizmo_mode == Gizmo.SCALE:
		_notify("Objects move and turn (T switches); scaling is for a lifted selection")
		return
	gizmo_axis = _gizmo_pick()
	if gizmo_axis < 0:
		gizmo_axis = 3  # free: in the plane facing the camera
	gizmo_start = target.global_transform
	if target == gizmo_body:
		network.use(gizmo_body)  # (multiplayer: moved here, so simulated here)
		gizmo_body_frozen = gizmo_body.freeze
		gizmo_body.freeze = true
	var from := camera.global_position
	var dir := -camera.global_basis.z
	var origin := target.global_position
	var axes := _gizmo_axes()
	if gizmo_axis == 3 or gizmo_mode == Gizmo.ROTATE:
		gizmo_plane_normal = dir if gizmo_axis == 3 else axes[gizmo_axis]
		var hit = Plane(gizmo_plane_normal, origin).intersects_ray(from, dir)
		if hit == null:
			# Facing the plane edge on: use the plane facing the camera.
			gizmo_plane_normal = dir
			hit = Plane(dir, origin).intersects_ray(from, dir)
		gizmo_start_hit = hit if hit != null else origin
	else:
		gizmo_start_param = _line_param(origin, axes[gizmo_axis], from, dir)

func _gizmo_drag() -> void:
	var floating := _gizmo_target()
	var from := camera.global_position
	var dir := -camera.global_basis.z
	var origin := gizmo_start.origin
	var axes := _gizmo_axes()
	if gizmo_mode == Gizmo.SCALE:
		axes = [gizmo_start.basis.x.normalized(), gizmo_start.basis.y.normalized(), gizmo_start.basis.z.normalized()]
	if gizmo_axis == 3 or gizmo_mode == Gizmo.ROTATE:
		var hit = Plane(gizmo_plane_normal, origin).intersects_ray(from, dir)
		if hit == null:
			return
		var p: Vector3 = hit
		match gizmo_mode:
			Gizmo.MOVE:
				floating.global_position = origin + (p - gizmo_start_hit)
			Gizmo.ROTATE:
				var a := gizmo_start_hit - origin
				var b := p - origin
				if a.length() > 0.01 and b.length() > 0.01:
					var angle := a.signed_angle_to(b, gizmo_plane_normal)
					floating.global_transform = Transform3D(Basis(gizmo_plane_normal, angle) * gizmo_start.basis, origin)
			Gizmo.SCALE:
				# Free: all axes together, by how far from the middle.
				var ratio := p.distance_to(origin) / maxf(gizmo_start_hit.distance_to(origin), 0.01)
				floating.global_transform = Transform3D(gizmo_start.basis.scaled(Vector3.ONE * clampf(ratio, 0.05, 20.0)), origin)
		return
	var s := _line_param(origin, axes[gizmo_axis], from, dir)
	match gizmo_mode:
		Gizmo.MOVE:
			floating.global_position = origin + axes[gizmo_axis] * (s - gizmo_start_param)
		Gizmo.SCALE:
			var factor := clampf(s / gizmo_start_param, 0.05, 20.0) if absf(gizmo_start_param) > 0.01 else 1.0
			var v := Vector3.ONE
			v[gizmo_axis] = factor
			floating.global_transform = Transform3D(gizmo_start.basis * Basis.from_scale(v), origin)

# --- Extrude ------------------------------------------------------------------

## Extrude (8): press on the selection and drag; the selection sweeps out
## along the surface's normal where you pressed (or in, dragging back), as
## far as the crosshair's ray comes nearest that line. Only the selection
## within the brush's radius moves, fading out towards its edge, like a
## grab; hold Shift when pressing to move all of it.
func _extrude(p_hit, p_held: bool) -> void:
	if tool != Tool.EXTRUDE or not p_held:
		if extruding:
			terrain.end_extrude()
			extruding = false
		return
	var from := camera.global_position
	var dir := -camera.global_basis.z
	if not extruding:
		if p_hit == null or target_body != null or not terrain.has_selection():
			return
		if not terrain.begin_extrude():
			return
		extruding = true
		extrude_origin = p_hit
		extrude_dir = terrain.get_normal(p_hit, 2.0)
		extrude_whole = _shift_held()
		extrude_last = -1.0
	# Out along the normal, or (dragging back) in.
	var distance := _line_param(extrude_origin, extrude_dir, from, dir)
	if absf(distance - extrude_last) > 0.1:
		extrude_last = distance
		terrain.extrude(extrude_dir, distance, extrude_origin, 0.0 if extrude_whole else brush_radius)

# --- Geometry -----------------------------------------------------------------

func _geometry_click(p_left: bool) -> void:
	var aimed := _aimed_geo_point()
	if p_left:
		if aimed >= 0:
			geo_drag = aimed
			geo_drag_distance = camera.global_position.distance_to(geo_points[aimed])
		else:
			geo_points.append(_geo_place())
			_rebuild_geometry()
	elif aimed >= 0:
		geo_points.remove_at(aimed)
		_rebuild_geometry()

## The placed point under the crosshair (a little generous), or -1.
func _aimed_geo_point() -> int:
	var from := camera.global_position
	var dir := -camera.global_basis.z
	var best := -1
	var best_angle := 0.03  # radians
	for i in geo_points.size():
		var to := geo_points[i] - from
		var angle := dir.angle_to(to)
		var size := maxf(1.2 / maxf(to.length(), 0.1), 0.015)  # the marker's angular size
		if angle < maxf(best_angle, size) and (best < 0 or angle < best_angle):
			best_angle = angle
			best = i
	return best

func _update_geometry() -> void:
	var on := tool in [Tool.GEOMETRY, Tool.SELECT_GEOMETRY]
	if geo_cursor == null:
		geo_cursor = _geo_marker(Color(1, 1, 1, 0.6))
	geo_cursor.visible = on and geo_drag < 0
	geo_cursor.global_position = _geo_place()
	for m in geo_markers:
		m.visible = on
	if geo_preview != null:
		geo_preview.visible = on
	if geo_drag >= 0:
		if not on or not _button(MOUSE_BUTTON_LEFT) or geo_drag >= geo_points.size():
			geo_drag = -1
		else:
			var to := camera.global_position - camera.global_basis.z * geo_drag_distance
			if to.distance_to(geo_points[geo_drag]) > 0.05:
				geo_points[geo_drag] = to
				_rebuild_geometry()
	for i in geo_markers.size():
		if i < geo_points.size():
			geo_markers[i].global_position = geo_points[i]

func _geo_marker(p_color: Color) -> MeshInstance3D:
	var marker := MeshInstance3D.new()
	var sphere := SphereMesh.new()
	sphere.radius = 0.8
	sphere.height = 1.6
	sphere.radial_segments = 10
	sphere.rings = 5
	marker.mesh = sphere
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	material.no_depth_test = true
	material.albedo_color = p_color
	marker.material_override = material
	marker.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	brush_preview.get_parent().add_child(marker)
	return marker

## Markers for the points, and the solid round them as a preview body.
func _rebuild_geometry() -> void:
	while geo_markers.size() < geo_points.size():
		geo_markers.append(_geo_marker(Color(1, 0.85, 0.2)))
	while geo_markers.size() > geo_points.size():
		geo_markers.pop_back().queue_free()
	if geo_points.is_empty():
		if geo_preview != null:
			geo_preview.queue_free()
			geo_preview = null
		return
	if geo_preview == null:
		geo_preview = DensityBody3D.new()
		geo_preview.drawn = true
		geo_preview.freeze = true
		geo_preview.collide_with_bodies = false
		geo_preview.pickable = false
		camera.get_parent().add_child(geo_preview)
	# Selecting previews in a plain colour; building, in what it'll be made of.
	var material := _current_material() if tool == Tool.GEOMETRY else SculptTerrain.color_to_material(Color(0.3, 0.8, 1.0))
	geo_preview.global_transform = Transform3D.IDENTITY  # the hull is in world space
	geo_preview.shape = DensityShape.make_hull(PackedVector3Array(geo_points), material)

## Where a new geometry point goes: on the ground under the crosshair, or
## geo_distance ahead if there's none.
func _geo_place() -> Vector3:
	var look := -camera.global_basis.z
	var hit = terrain.raycast(camera.global_position, look, reach)
	return hit if hit != null else camera.global_position + look * geo_distance

## Shift + click with the snake hook: a straight tendril from where the last
## one ended to the ground under the crosshair (or as far ahead), and the
## next starts there.
func _snake_line() -> void:
	var look := -camera.global_basis.z
	var hit = terrain.raycast(camera.global_position, look, reach)
	var from: Vector3 = snake_last
	var to: Vector3 = hit if hit != null else camera.global_position + look * camera.global_position.distance_to(from)
	capture_click = true  # this click doesn't start a drag
	terrain.begin_stroke()
	dragging = true
	drag_body = null
	drag_start = from
	drag_copies = _symmetry_copies()
	for i in 64:
		if drag_start.distance_to(to) < 0.05:
			break
		_drag(to)
	dragging = false
	terrain.end_stroke()
	snake_last = to

## Ctrl+Shift+O: a saved shape (an export, a frog's...) pasted in front of
## you to place (Enter) or cut out (Shift+Enter) with the transform tool.
func _import_to_paste() -> void:
	_open_file_dialog(FileDialog.FILE_MODE_OPEN_FILE, func(path: String) -> void:
		var shape := load(path) as DensityShape
		if shape == null:
			push_error("%s isn't a density shape" % path)
			return
		clipboard = shape
		var look := -camera.global_basis.z
		var hit = terrain.raycast(camera.global_position, look, reach)
		clipboard_center = hit if hit != null else camera.global_position + look * 30.0
		_paste())

# --- Box ----------------------------------------------------------------------

## LMB: the first corner, where the crosshair meets the ground. (Select
## box: RMB too, taking away from the selection.)
func _box_press(p_left: bool) -> void:
	if tool == Tool.SELECT_BOX:
		if box_dragging:
			box_dragging = false  # (the other button: give up)
			return
		box_select_add = p_left
	elif not p_left:
		box_dragging = false  # (RMB: give up)
		return
	var hit = terrain.raycast(camera.global_position, -camera.global_basis.z, reach)
	box_a = hit if hit != null else camera.global_position - camera.global_basis.z * geo_distance
	box_distance = -1.0
	box_dragging = true

## Where the far corner is: on the ground under the crosshair, or (once the
## wheel has moved it) that far along it.
func _box_far_corner() -> Vector3:
	var from := camera.global_position
	var look := -camera.global_basis.z
	if box_distance < 0.0:
		var hit = terrain.raycast(from, look, reach)
		return hit if hit != null else from + look * from.distance_to(box_a)
	return from + look * box_distance

## The box's lowest and highest corners, on whole voxels, at least one thick.
func _box_bounds() -> AABB:
	var b := _box_far_corner()
	var lo := box_a.min(b).round()
	var hi := box_a.max(b).round()
	for a in 3:
		if hi[a] - lo[a] < 1.0:
			hi[a] = lo[a] + 1.0
	return AABB(lo, hi - lo)

func _box_size() -> Vector3i:
	return Vector3i(_box_bounds().size)

func _box_corners() -> PackedVector3Array:
	var box := _box_bounds()
	var corners := PackedVector3Array()
	for i in 8:
		corners.append(box.get_endpoint(i))
	return corners

func _apply_box(p_cut: bool) -> void:
	var shape := DensityShape.make_hull(_box_corners(), _current_material())
	terrain.begin_stroke()
	if p_cut:
		terrain.carve_shape(shape, Transform3D.IDENTITY)
	else:
		terrain.stamp_shape(shape, Transform3D.IDENTITY, _current_material(), false)
	terrain.end_stroke()

## Select box, let go: the ground inside the box selected (p_add) or not
## any more. Bodies whose middles are inside are selected instead when no
## ground is (the two aren't selected together).
func _apply_box_select(p_add: bool) -> void:
	var box := _box_bounds()
	var shape := DensityShape.make_hull(_box_corners(), 0)
	var before := terrain.get_selected_count()
	terrain.select_inside_shape(shape, Transform3D.IDENTITY, p_add)
	if not p_add:
		for b in selected_bodies.duplicate():
			if is_instance_valid(b) and box.has_point(b.global_position):
				_select_body(b, false)
		return
	if terrain.get_selected_count() > before:
		_clear_body_selection()
		return
	for b in camera.get_parent().get_children():
		if b is DensityBody3D and not b is DensityCharacter3D and b != box_preview and b.pickable and box.has_point(b.global_position):
			_select_body(b, true)

## While dragging a box: it's previewed as a body (rebuilt when it
## changes), and built (or cut out) when the button is let go.
func _update_box() -> void:
	if box_dragging and (not tool in [Tool.BOX, Tool.SELECT_BOX] or not _captured()):
		box_dragging = false
	var box_button := MOUSE_BUTTON_LEFT if tool != Tool.SELECT_BOX or box_select_add else MOUSE_BUTTON_RIGHT
	if box_dragging and not _button(box_button):
		box_dragging = false
		if tool == Tool.SELECT_BOX:
			_apply_box_select(box_select_add)
		else:
			_apply_box(_shift_held())
	if not box_dragging:
		if box_preview != null:
			box_preview.queue_free()
			box_preview = null
			box_shown = PackedVector3Array()
		return
	var corners := _box_corners()
	# (Select box: shown as a selection would be, or red taking away.)
	var cut := _shift_held() if tool == Tool.BOX else not box_select_add
	var changed := corners.size() != box_shown.size() or cut != box_preview_cut
	for i in mini(corners.size(), box_shown.size()):
		changed = changed or corners[i].distance_to(box_shown[i]) > 0.05
	if not changed:
		return
	box_shown = corners
	box_preview_cut = cut
	if box_preview == null:
		box_preview = DensityBody3D.new()
		box_preview.drawn = true
		box_preview.freeze = true
		box_preview.collide_with_bodies = false
		box_preview.pickable = false
		camera.get_parent().add_child(box_preview)
		box_preview.global_transform = Transform3D.IDENTITY  # the hull is in world space
	# A cut previews in red; a box in what it'll be made of.
	var material := SculptTerrain.color_to_material(Color(0.95, 0.25, 0.2)) if cut else _current_material()
	if tool == Tool.SELECT_BOX and not cut:
		material = SculptTerrain.color_to_material(Color(0.3, 0.7, 1.0))
	box_preview.shape = DensityShape.make_hull(corners, material)
