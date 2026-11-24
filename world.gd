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
## Half-resolution pass that tells the terrain shader where rays can start
## (see terrain_prepass.gdshader). Its camera follows `camera`.
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
## Balls thrown with F (bouncy_balls.gd), saved with the world.
const BouncyBalls := preload("res://bouncy_balls.gd")
var balls: BouncyBalls
## The object editor (M on a body: sculpt it, animate its pose keys; object_editor.gd).
const ObjectEditor := preload("res://object_editor.gd")
var editor: ObjectEditor
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
## Select fill acts once per click.
var fill_clicked := false
## The file dialog open for Ctrl+P / Ctrl+O, if any.
var file_dialog: FileDialog
var held_shape_before: DensityShape  # the held body's shape before the stroke (for undo)
## How far the camera trails behind the player's height (see _process).
var camera_lag := 0.0
## Flatten: the plane the current stroke flattens to (set when it starts).
var flatten_point := Vector3.ZERO
var flatten_normal := Vector3.ZERO
var flatten_locked := false
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

## Box (0, twice) and Box cut (0, three times): click where an edge starts,
## where it ends, then how wide, then how high (or deep: below the ground);
## the box is built of the current material, or cut out. Right click steps
## back. box_stage: how many of those clicks so far.
var box_stage := 0
var box_a := Vector3.ZERO
var box_b := Vector3.ZERO
var box_up := Vector3.UP
var box_width := 0.0
var box_height := 0.0
var box_preview: DensityBody3D = null
var box_shown := PackedVector3Array()  # the corners the preview was made from

enum Tool { DRAW, SMOOTH, FLATTEN, GRAB, SNAKE_HOOK, PAINT, SELECT_PAINT, SELECT_FILL, PHYSICS_GUN, SELECT_GEOMETRY, EXTRUDE, TRANSFORM, GEOMETRY,
		INFLATE, PINCH, BOX, BOX_CUT }
const TOOL_NAMES := ["Draw", "Smooth", "Flatten", "Grab", "Snake hook", "Paint", "Select paint", "Select fill", "Physics gun",
		"Select geometry", "Extrude", "Transform", "Geometry", "Inflate", "Pinch", "Box", "Box cut"]
## Tools that never sculpt with the mouse buttons.
const NON_SCULPT_TOOLS := [Tool.SELECT_PAINT, Tool.SELECT_FILL, Tool.PHYSICS_GUN, Tool.SELECT_GEOMETRY, Tool.EXTRUDE, Tool.TRANSFORM, Tool.GEOMETRY,
		Tool.BOX, Tool.BOX_CUT]
## Brushes whose right button does the opposite (deflate, spread) rather than carve.
const INVERTIBLE_TOOLS := [Tool.INFLATE, Tool.PINCH]
## The brush each tool applies while held (-1: grab and snake hook drag instead).
const TOOL_MODES := [SculptTerrain.BRUSH_ADD, SculptTerrain.BRUSH_SMOOTH, SculptTerrain.BRUSH_FLATTEN, -1, -1, SculptTerrain.BRUSH_PAINT, -1, -1, -1, -1, -1, -1, -1,
		SculptTerrain.BRUSH_INFLATE, SculptTerrain.BRUSH_PINCH, -1, -1]
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
## fading out beyond (one stroke doesn't build up over itself). Tab opens
## the picker; middle click on a colored surface picks its color.
var paint_color := Color(0.85, 0.25, 0.2)
var paint_opacity := 1.0
var paint_hardness := 0.5
var color_panel: PanelContainer
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
## Snake hook moves in steps of at most this fraction of the radius (and 8
## voxels), at most this many per frame; the rest carries over.
const SNAKE_STEP := 0.5
const SNAKE_STEPS_PER_FRAME := 8

## Symmetry: E places an axis through the camera along the view direction, Q
## removes it, Z switches mirror / radial, + / - change the radial count.
## Mirror reflects across the plane holding the axis and the camera's up
## direction (left <-> right as seen when it was placed).
enum Symmetry { MIRROR, RADIAL }
var symmetry_on := false
var symmetry := Symmetry.MIRROR
var radial_count := 4
var axis_origin := Vector3.ZERO
var axis_dir := Vector3.FORWARD
var mirror_normal := Vector3.RIGHT
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
	terrain.open_world()
	if not _load_player():
		# Start off the edge of the nearest island, looking at it.
		var island = terrain.find_island(Vector3.ZERO)
		var spawn := Vector3.ZERO
		if island != null:
			spawn = island + Vector3(0, 25, 90)
		respawn(spawn)
		if island != null:
			camera.look_at(island)
	yaw = camera.rotation.y
	pitch = camera.rotation.x
	terrain.gravity = gravity  # what the physics bodies fall with
	_make_player()
	balls = BouncyBalls.new(terrain)
	camera.get_parent().add_child(balls)
	if not terrain.save_path.is_empty():
		balls.load_from(_objects_path())
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
	material.set_shader_parameter("use_prepass", true)
	terrain.update_view(camera.global_position)  # builds and uploads the clipmap
	_sync_prepass_camera()
	print("World opened in %d ms" % (Time.get_ticks_msec() - start))

	get_viewport().size_changed.connect(_fit_screen)
	_fit_screen()
	_make_symmetry_visuals()

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
	if what == NOTIFICATION_WM_CLOSE_REQUEST:
		_save_player()
		_save_objects()

func _save_objects() -> void:
	if not terrain.save_path.is_empty():
		balls.save(_objects_path())

func _sync_prepass_camera() -> void:
	prepass_camera.global_transform = camera.global_transform
	prepass_camera.fov = camera.fov

## Scales the low-resolution view up as far as it fits, centred, and fits
## the HUD to the window: the info text wraps to its width, and the color
## panel (bottom left) shrinks if it wouldn't fit.
func _fit_screen() -> void:
	var window := get_viewport().get_visible_rect().size
	var view := Vector2(screen.size)
	var fit := minf(window.x / view.x, window.y / view.y)
	screen.scale = Vector2(fit, fit)
	screen.position = (window - view * fit) * 0.5

	info_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	info_label.size = Vector2(maxf(window.x - 24.0, 100.0), 0.0)
	if color_panel != null:
		var need := color_panel.get_combined_minimum_size()
		color_panel.size = need
		var room := Vector2(window.x - 24.0, window.y * 0.7)
		var scale := minf(1.0, minf(room.x / need.x, room.y / need.y))
		color_panel.scale = Vector2(scale, scale)
		color_panel.position = Vector2(12.0, window.y - 12.0 - need.y * scale)

## Mouse look. The camera's rotation is rebuilt from these angles each time, so
## pitch always tilts around the camera's own right axis whichever way you face.
func _input(event: InputEvent) -> void:
	if editor != null and editor.handle_input(event):
		get_viewport().set_input_as_handled()
		return
	if event is InputEventMouseMotion and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
		yaw -= event.relative.x * mouse_sensitivity
		pitch = clampf(pitch - event.relative.y * mouse_sensitivity, deg_to_rad(-89.0), deg_to_rad(89.0))
		camera.rotation = Vector3(pitch, yaw, 0.0)
	elif event is InputEventMouseButton and event.pressed:
		if Input.mouse_mode != Input.MOUSE_MODE_CAPTURED:
			if color_panel.visible and color_panel.get_global_rect().has_point(event.position):
				return  # the picker's
			if editor != null and editor.has_point(event.position):
				return  # the object editor's panels
			if event.button_index == MOUSE_BUTTON_LEFT:
				Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
				capture_click = true
				get_viewport().gui_release_focus()  # back to playing: keys are hotkeys again
		elif tool == Tool.PHYSICS_GUN and event.button_index in [MOUSE_BUTTON_LEFT, MOUSE_BUTTON_RIGHT]:
			var aimed := DensityBody3D.raycast_bodies(camera.global_position, -camera.global_basis.z, reach)
			if not aimed.is_empty():
				var body: DensityBody3D = aimed.body
				if event.button_index == MOUSE_BUTTON_RIGHT:
					# Freeze where it is, or let it go again.
					body.freeze = not body.freeze
					body.sleeping = false
				else:
					gun_body = body
					gun_distance = aimed.distance
					gun_point = body.to_local(aimed.position)
		elif tool in [Tool.BOX, Tool.BOX_CUT] and event.button_index in [MOUSE_BUTTON_LEFT, MOUSE_BUTTON_RIGHT]:
			_box_click(event.button_index == MOUSE_BUTTON_LEFT)
		elif tool in [Tool.GEOMETRY, Tool.SELECT_GEOMETRY] and event.button_index in [MOUSE_BUTTON_LEFT, MOUSE_BUTTON_RIGHT]:
			_geometry_click(event.button_index == MOUSE_BUTTON_LEFT)
		elif tool == Tool.SNAKE_HOOK and event.button_index == MOUSE_BUTTON_LEFT and event.shift_pressed and snake_last != null:
			_snake_line()
		elif tool == Tool.TRANSFORM and event.button_index == MOUSE_BUTTON_LEFT and floating != null:
			_gizmo_press()
		elif event.button_index == MOUSE_BUTTON_MIDDLE:
			var hit = terrain.raycast(camera.global_position, -camera.global_basis.z, reach)
			if hit != null:
				var picked: int = terrain.get_block_material(hit)
				if picked >= 32768:  # a color
					paint_color = terrain.get_material_color(picked)
					color_picker.color = paint_color
					block = material_names.size()
				elif picked >= 0:
					block = picked
		elif event.button_index == MOUSE_BUTTON_WHEEL_UP or event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			var step := 1.15 if event.button_index == MOUSE_BUTTON_WHEEL_UP else 1.0 / 1.15
			if event.shift_pressed:
				_set_player_height(player_height * step)  # the player's size, and all that goes with it
			elif gun_body != null:
				gun_distance = clampf(gun_distance * step, 4.0, reach)  # pull in / push out
			elif tool in [Tool.GEOMETRY, Tool.SELECT_GEOMETRY] and not event.ctrl_pressed:
				if geo_drag >= 0:
					geo_drag_distance = clampf(geo_drag_distance * step, 2.0, reach)
				else:
					geo_distance = clampf(geo_distance * step, 2.0, reach)
			elif event.ctrl_pressed:
				brush_strength = clampf(brush_strength * step, 0.5, 64.0)
			else:
				brush_radius = clampf(brush_radius * step, 1.0, 48.0)
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
			color_panel.visible = not color_panel.visible
			_fit_screen()
			Input.mouse_mode = Input.MOUSE_MODE_VISIBLE if color_panel.visible else Input.MOUSE_MODE_CAPTURED
			get_viewport().gui_release_focus()
			get_viewport().set_input_as_handled()  # not the GUI's focus-next
			return
		if get_viewport().gui_get_focus_owner() is LineEdit:
			return  # typing into a field (the picker's, the editor's)
		match _key_of(event):
			KEY_1: _cycle_tool([Tool.DRAW, Tool.INFLATE])
			KEY_2: _cycle_tool([Tool.SMOOTH, Tool.PINCH])
			KEY_3: _set_tool(Tool.FLATTEN)
			KEY_4: _set_tool(Tool.GRAB)
			KEY_5: _set_tool(Tool.SNAKE_HOOK)
			KEY_6: _set_tool(Tool.PAINT)
			KEY_7: _cycle_tool([Tool.SELECT_PAINT, Tool.SELECT_FILL, Tool.SELECT_GEOMETRY])
			KEY_8: _cycle_tool([Tool.EXTRUDE, Tool.TRANSFORM])
			KEY_9: _set_tool(Tool.PHYSICS_GUN)
			KEY_0: _cycle_tool([Tool.GEOMETRY, Tool.BOX, Tool.BOX_CUT])
			KEY_T:
				gizmo_mode = ((gizmo_mode + 1) % 3) as Gizmo
			KEY_ENTER, KEY_KP_ENTER:
				_confirm(event.shift_pressed)  # Shift: cut the shape out instead
			KEY_BACKSPACE:
				if tool in [Tool.BOX, Tool.BOX_CUT] and box_stage > 0:
					box_stage = 0
				elif tool in [Tool.GEOMETRY, Tool.SELECT_GEOMETRY] and not geo_points.is_empty():
					geo_points.clear()
					_rebuild_geometry()
				else:
					_apply_floating()
					terrain.clear_selection()
			KEY_P:
				_selection_to_body()
			KEY_B:
				# Blocky mode: every tool's result snaps to 2-voxel blocks.
				terrain.blocky_size = 0 if terrain.blocky_size >= 2 else 2
			KEY_F:
				# Made of the selected block, or a random colour when "Color" is.
				var ball_material := block if block < material_names.size() else -1
				balls.throw(camera.global_position, -camera.global_basis.z, player.velocity, ball_material)
			KEY_G:
				_spawn_frog()
			KEY_X:
				# Removes the ball or frog under the crosshair.
				var aimed := DensityBody3D.raycast_bodies(camera.global_position, -camera.global_basis.z, reach)
				if not aimed.is_empty():
					balls.remove(aimed.body)
			KEY_BRACKETLEFT: block = posmod(block - 1, material_names.size() + 1)
			KEY_BRACKETRIGHT: block = posmod(block + 1, material_names.size() + 1)
			KEY_E:
				if not dragging:
					symmetry_on = true
					axis_origin = camera.global_position
					axis_dir = -camera.global_basis.z
					mirror_normal = camera.global_basis.x
			KEY_Q:
				if not dragging:
					symmetry_on = false
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
			KEY_K:
				_set_player_height(player_height + 1)
			KEY_L:
				_set_player_height(player_height - 1)
			_:
				if not _is_action(event):
					print("Unbound key: physical %s, keycode %s, label %s, unicode %d" % [
						OS.get_keycode_string(event.physical_keycode), OS.get_keycode_string(event.keycode),
						OS.get_keycode_string(event.key_label), event.unicode])

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
	tool = p_tool
	if tool == Tool.TRANSFORM:
		_lift_selection()

## A key with several tools: the first, or the next if one of them is on.
func _cycle_tool(p_tools: Array) -> void:
	var at := p_tools.find(tool)
	_set_tool(p_tools[(at + 1) % p_tools.size()] if at >= 0 else p_tools[0])

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
## earlier copy (on the axis or the mirror plane) so they don't apply twice.
## Returns the indices of the copies kept.
func _distinct_copies(p_copies: Array[Transform3D], p_point: Vector3) -> PackedInt32Array:
	var kept := PackedInt32Array()
	for i in p_copies.size():
		var q := p_copies[i] * p_point
		var duplicate := false
		for j in kept:
			if (p_copies[j] * p_point).distance_to(q) < maxf(1.0, brush_radius * 0.25):
				duplicate = true
				break
		if not duplicate:
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

func _process(delta: float) -> void:
	if Input.is_action_pressed("ui_cancel"):
		Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	if not Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT):
		capture_click = false

	# The camera rides on top of the player (moved in _physics_process).
	# Small ups and downs while walking (steps, rough ground) are eased, so
	# the view glides instead of jolting; anything bigger is followed at once.
	var eye := player.global_position + Vector3.UP * player_height
	var off := camera_lag
	if player.is_on_floor() and absf(off) < player.step_height * 2.0:
		off *= exp(-delta * 18.0)
	else:
		off = 0.0
	camera_lag = off
	camera.global_position = eye + Vector3.UP * camera_lag

	_sculpt(delta)
	_update_gizmo()
	_update_geometry()
	_update_box()
	_record_history()
	terrain.update_view(camera.global_position)  # also sends the drawn bodies to the shader

	_sync_prepass_camera()

	player_save_timer += delta
	if player_save_timer >= player_save_interval:
		player_save_timer = 0.0
		_save_player()
		_save_objects()

func _physics_process(delta: float) -> void:
	# Bodies (balls, frogs) are stepped by the terrain itself, after this
	# (it is a child of this node); the player moves here.
	_wrap_player()
	balls.stream(player.global_position)

	var facing := -camera.global_basis.z
	facing = Vector3(facing.x, 0.0, facing.z).normalized()
	var side := facing.cross(Vector3.UP)
	var forward: float = Input.get_action_strength("forward") - Input.get_action_strength("back")
	var right: float = Input.get_action_strength("right") - Input.get_action_strength("left")
	var size_scale := player_height / BASE_PLAYER_HEIGHT
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
	_update_physics_gun(delta)

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

## The color picker (Tab): an RGB picker with opacity and hardness sliders.
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
	info_label.get_parent().add_child(color_panel)

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

## A frog (forg.tscn, a DensityBody3D) dropped where the crosshair points.
func _spawn_frog() -> void:
	var frog := forg_scene.instantiate() as Node3D
	var hit = terrain.raycast(camera.global_position, -camera.global_basis.z, reach)
	var spawn: Vector3 = hit + Vector3.UP * 20.0 if hit != null else camera.global_position - camera.global_basis.z * 30.0
	camera.get_parent().add_child(frog)
	frog.global_position = spawn
	frog.rotation.y = randf() * TAU

func _sculpt(delta: float) -> void:
	var look := -camera.global_basis.z
	var sculpting := Input.mouse_mode == Input.MOUSE_MODE_CAPTURED and not capture_click
	var held := sculpting and Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT)
	var pressing := held or (sculpting and Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT))
	if tool in [Tool.PHYSICS_GUN, Tool.TRANSFORM, Tool.GEOMETRY, Tool.SELECT_GEOMETRY, Tool.BOX, Tool.BOX_CUT]:
		pressing = false  # these don't sculpt
	if pressing and not stroking:
		terrain.begin_stroke()
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
	target_body = null
	if not dragging:
		hit = terrain.raycast(camera.global_position, look, reach)
		# A body (ball, frog) in front of the ground is sculpted instead.
		var nearest: float = reach if hit == null else camera.global_position.distance_to(hit)
		var aimed := DensityBody3D.raycast_bodies(camera.global_position, look, nearest)
		if not aimed.is_empty():
			target_body = aimed.body
			hit = aimed.position
		if target_body != null and pressing and held_body == null and (TOOL_MODES[tool] >= 0 or tool in [Tool.GRAB, Tool.SNAKE_HOOK]):
			_hold_body(target_body)
		if hit != null and held and (tool == Tool.GRAB or tool == Tool.SNAKE_HOOK):
			_begin_drag(hit)
	brush_preview.visible = hit != null
	if hit != null:
		brush_preview.global_position = hit
		brush_preview.scale = Vector3.ONE * brush_radius

	_select(hit, sculpting)
	_extrude(hit, held)
	var mode := -1
	if tool in NON_SCULPT_TOOLS:
		pass  # not a sculpting tool
	elif hit != null and sculpting and not dragging:
		if Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT):
			mode = TOOL_MODES[tool] if tool in INVERTIBLE_TOOLS else SculptTerrain.BRUSH_SUBTRACT
		elif held and TOOL_MODES[tool] >= 0:
			mode = TOOL_MODES[tool]
	var coloring := block == material_names.size()
	# Inflate / pinch with the right button: deflate / spread.
	var strength := brush_strength * delta * (-1.0 if tool in INVERTIBLE_TOOLS and Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT) else 1.0)
	if mode >= 0 and target_body != null:
		# Bodies are sculpted on their own (no symmetry, no undo).
		if mode == SculptTerrain.BRUSH_PAINT and coloring:
			target_body.paint_color(hit, brush_radius, paint_color, paint_opacity, paint_hardness)
		else:
			var material: int = SculptTerrain.color_to_material(paint_color) if coloring else block
			target_body.apply_brush(hit, brush_radius, strength, mode, material)
	elif mode >= 0:
		var copies := _symmetry_copies()
		for i in _distinct_copies(copies, hit):
			if mode == SculptTerrain.BRUSH_PAINT and coloring:
				terrain.paint_color(copies[i] * hit, brush_radius, paint_color, paint_opacity, paint_hardness)
			else:
				var material: int = SculptTerrain.color_to_material(paint_color) if coloring else block
				if mode == SculptTerrain.BRUSH_FLATTEN:
					# The stroke flattens to one plane: through where it began,
					# facing the general lie of the ground there.
					if not flatten_locked:
						flatten_locked = true
						flatten_point = hit
						flatten_normal = terrain.get_normal(hit, brush_radius * 0.5)
					terrain.set_flatten_plane(copies[i] * flatten_point, copies[i].basis * flatten_normal)
				terrain.apply_brush(copies[i] * hit, brush_radius, strength, mode, material)
	_update_symmetry_visuals(hit)

	var sym := "Blocky %s (B)   F throws a ball, G drops a frog, X removes one   Selected %d (Backspace clears, P: to physics object, Ctrl+C / Ctrl+V copy, Ctrl+P export, Ctrl+O import)   %s   " % [
		"on" if terrain.blocky_size >= 2 else "off", terrain.get_selected_count(), _tool_hint()]
	var prefix := sym
	sym += "Symmetry off (E: axis along view)"
	if symmetry_on:
		sym = prefix
		sym += "Mirror symmetry" if symmetry == Symmetry.MIRROR else "Radial symmetry x%d (+/-)" % radial_count
		sym += " (Z: mirror/radial, E: move axis, Q: remove)"
	var block_name := material_names[block].capitalize() if block < material_names.size() else \
			"Color #%s, opacity %d%%, hardness %d%% (Tab: picker)" % [paint_color.to_html(false), roundi(paint_opacity * 100.0), roundi(paint_hardness * 100.0)]
	info_label.text = "%s (1 Draw / Inflate, 2 Smooth / Pinch, 3 Flatten, 4 Grab, 5 Snake hook, 6 Paint, 7 Select: paint / fill / geometry, 8 Extrude / Transform, 9 Physics gun, 0 Geometry / Box / Box cut)   LMB sculpt, RMB carve\nMaterial: %s ([ / ], middle click picks)   Radius %.1f (wheel)   Strength %.1f (Ctrl+wheel)   Undo %d / redo %d (Ctrl+Z / Ctrl+Shift+Z)   %d FPS\n%s" % [
		TOOL_NAMES[tool], block_name, brush_radius, brush_strength, terrain.get_undo_count(), terrain.get_redo_count(), Engine.get_frames_per_second(), sym]

func _begin_drag(p_hit: Vector3) -> void:
	dragging = true
	drag_start = p_hit
	drag_distance = camera.global_position.distance_to(p_hit)
	drag_copies = _symmetry_copies()
	last_grab_delta = Vector3.INF
	drag_body = target_body
	if drag_body != null:
		drag_copies = [Transform3D.IDENTITY]  # no symmetry on bodies
		if tool == Tool.GRAB:
			drag_body.begin_grab()
	elif tool == Tool.GRAB:
		terrain.begin_grab()

func _end_drag() -> void:
	if dragging and tool == Tool.SNAKE_HOOK and drag_body == null:
		snake_last = drag_start
	if dragging and tool == Tool.GRAB:
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
			if drag_body != null:
				if is_instance_valid(drag_body):
					drag_body.warp(drag_start, delta, brush_radius, true)
				return p_target
			for i in kept:
				centers.append(drag_copies[i] * drag_start)
				deltas.append(drag_copies[i].basis * delta)
			terrain.warp(centers, deltas, brush_radius, true)
		return p_target
	# Snake hook: pulls material along in short steps.
	var max_step := minf(brush_radius * SNAKE_STEP, 8.0)
	for step_index in SNAKE_STEPS_PER_FRAME:
		var move := p_target - drag_start
		if move.length() < 0.05:
			break
		var step := move.limit_length(max_step)
		drag_start += step
		if drag_body != null:
			if is_instance_valid(drag_body):
				drag_body.warp(drag_start, step, brush_radius, false)
			continue
		var centers := PackedVector3Array()
		var deltas := PackedVector3Array()
		for i in kept:
			centers.append(drag_copies[i] * drag_start)
			deltas.append(drag_copies[i].basis * step)
		terrain.warp(centers, deltas, brush_radius, false)
	return drag_start

## Holds a body still while it's sculpted (see held_body).
func _hold_body(p_body: DensityBody3D) -> void:
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
		var part := balls.add_shape_body(part_shape, Vector3.ZERO)
		part.global_transform = p_entry.part_transform
		if is_instance_valid(from):
			part.linear_velocity = from.linear_velocity
		parts.append(part)
	p_entry.parts = parts

func _push_history(p_entry: Dictionary) -> void:
	history.append(p_entry)
	future.clear()
	if history.size() > MAX_HISTORY:
		history.pop_front()

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
	if not p_force and (Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT) or Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)):
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
				balls.remove(part)
		entry.parts = []
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
	var left := p_sculpting and Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT)
	var right := p_sculpting and Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)
	if not (left or right):
		fill_clicked = false
	if p_hit == null or target_body != null:
		return
	if tool == Tool.SELECT_PAINT and (left or right):
		terrain.select_sphere(p_hit, brush_radius, left)
	elif tool == Tool.SELECT_FILL and (left or right) and not fill_clicked:
		fill_clicked = true
		var radius := minf(brush_radius * FILL_RADII, FILL_MAX)
		if left:
			terrain.select_connected(p_hit, radius, true)
		else:
			terrain.select_similar(p_hit, radius, brush_strength / 32.0, true)

## P: the selection becomes a physics object (taken out of the terrain:
## Ctrl+Z puts the ground back, but not the object away).
func _selection_to_body() -> void:
	if not terrain.has_selection():
		return
	var shape: DensityShape = terrain.selection_to_shape(true)
	terrain.clear_selection()
	if shape != null:
		balls.add_shape_body(shape, terrain.get_selection_center())

## The physics gun (tool 9): the held body is pulled towards the point
## gun_distance in front of the camera (the wheel moves it), spinning down;
## letting go throws it with the speed it has. A frozen one just moves there.
func _update_physics_gun(p_delta: float) -> void:
	if gun_body == null:
		return
	if not is_instance_valid(gun_body) or tool != Tool.PHYSICS_GUN or not Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT):
		gun_body = null
		return
	var target := camera.global_position - camera.global_basis.z * gun_distance
	var point := gun_body.to_global(gun_point)
	if gun_body.freeze:
		gun_body.global_position += target - point
	else:
		gun_body.linear_velocity = (target - point) * 12.0 + Vector3.UP * gravity * gun_body.gravity_scale * p_delta
		gun_body.angular_velocity *= 0.85

## Ctrl+P: saves the selection (shape, colours and materials) as a
## DensityShape file, which a body's `shape` can use (a frog's, say).
func _export_selection() -> void:
	if not terrain.has_selection():
		return
	var shape: DensityShape = terrain.selection_to_shape(false)
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
		balls.add_shape_body(shape.duplicate_shape(), camera.global_position - camera.global_basis.z * 30.0))

func _open_file_dialog(p_mode: FileDialog.FileMode, p_chosen: Callable) -> void:
	if file_dialog != null:
		file_dialog.queue_free()
	file_dialog = FileDialog.new()
	file_dialog.file_mode = p_mode
	file_dialog.access = FileDialog.ACCESS_FILESYSTEM
	file_dialog.filters = PackedStringArray(["*.res, *.tres ; Density shapes"])
	file_dialog.current_dir = ProjectSettings.globalize_path("res://")
	file_dialog.use_native_dialog = true
	file_dialog.file_selected.connect(p_chosen)
	info_label.get_parent().add_child(file_dialog)
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
	match tool:
		Tool.EXTRUDE:
			return "Extrude: drag on the selection, out or back in (within the brush radius; Shift: all of it)"
		Tool.SNAKE_HOOK:
			return "Snake hook: Shift + click pulls a straight one from where the last ended"
		Tool.FLATTEN:
			return "Flatten: to the plane where the stroke began"
		Tool.INFLATE:
			return "Inflate: puffs the surface out (RMB: in)"
		Tool.PINCH:
			return "Pinch: pulls the surface into a crease (RMB: spreads it)"
		Tool.BOX, Tool.BOX_CUT:
			return "%s: click %s (RMB: back, Backspace: start over)" % [TOOL_NAMES[tool],
				["where an edge starts", "where it ends", "how wide", "how high (below the ground: deep)"][box_stage]]
		Tool.TRANSFORM:
			if floating == null:
				return "Transform: select something first"
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
		if not Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT):
			gizmo_axis = -1
			if target == floating and not floating.global_transform.is_equal_approx(gizmo_start):
				_push_history({"floating": floating, "before": gizmo_start, "after": floating.global_transform})
		else:
			_gizmo_drag()

## What the gizmo moves: the transform tool's floating piece.
func _gizmo_target() -> Node3D:
	return floating if floating != null and tool == Tool.TRANSFORM else null

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
	gizmo_axis = _gizmo_pick()
	if gizmo_axis < 0:
		gizmo_axis = 3  # free: in the plane facing the camera
	var target := _gizmo_target()
	gizmo_start = target.global_transform
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
		extrude_whole = Input.is_key_pressed(KEY_SHIFT)
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
		if not on or not Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT) or geo_drag >= geo_points.size():
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

## Where the crosshair meets the box's base plane (through its first
## corner, square to box_up), or a point ahead if it's looking along it.
func _box_plane_point() -> Vector3:
	var from := camera.global_position
	var look := -camera.global_basis.z
	var hit = Plane(box_up, box_a).intersects_ray(from, look)
	if hit == null:
		return from + look * from.distance_to(box_a)
	return hit

func _box_click(p_left: bool) -> void:
	if not p_left:
		box_stage = maxi(box_stage - 1, 0)
		return
	match box_stage:
		0:
			var hit = terrain.raycast(camera.global_position, -camera.global_basis.z, reach)
			if hit == null:
				return
			box_a = hit
			# Square to the ground; on near-level ground, straight up.
			var normal := terrain.get_normal(hit, 2.0)
			box_up = Vector3.UP if normal.dot(Vector3.UP) > 0.82 else normal
			box_stage = 1
		1:
			box_b = _box_plane_point()
			if box_b.distance_to(box_a) > 0.5:
				box_stage = 2
		2:
			box_width = _box_live_width()
			box_stage = 3
		3:
			box_height = _box_live_height()
			_apply_box()
			box_stage = 0

func _box_live_width() -> float:
	var edge := (box_b - box_a).normalized()
	var across := box_up.cross(edge).normalized()
	return (_box_plane_point() - box_a).dot(across)

func _box_live_height() -> float:
	var edge_middle := box_a.lerp(box_b, 0.5) + box_up.cross((box_b - box_a).normalized()).normalized() * box_width * 0.5
	return _line_param(edge_middle, box_up, camera.global_position, -camera.global_basis.z)

## The box's corners as the clicks (and the crosshair, for what's still to
## click) have it: just the edge until the width is known.
func _box_corners() -> PackedVector3Array:
	var a := box_a
	var b := box_b if box_stage >= 2 else _box_plane_point()
	var corners := PackedVector3Array([a, b])
	if box_stage >= 2:
		var across := box_up.cross((b - a).normalized()).normalized()
		var width := box_width if box_stage >= 3 else _box_live_width()
		corners.append(a + across * width)
		corners.append(b + across * width)
		if box_stage >= 3:
			var up := box_up * _box_live_height()
			for i in 4:
				corners.append(corners[i] + up)
	return corners

func _apply_box() -> void:
	var corners := _box_corners()
	var shape := DensityShape.make_hull(corners, _current_material())
	terrain.begin_stroke()
	if tool == Tool.BOX_CUT:
		terrain.carve_shape(shape, Transform3D.IDENTITY)
	else:
		terrain.stamp_shape(shape, Transform3D.IDENTITY, _current_material(), false)
	terrain.end_stroke()

## The box so far, previewed as a body (rebuilt when it changes).
func _update_box() -> void:
	var on := tool in [Tool.BOX, Tool.BOX_CUT] and box_stage > 0
	if not on:
		if box_preview != null:
			box_preview.queue_free()
			box_preview = null
			box_shown = PackedVector3Array()
		if tool not in [Tool.BOX, Tool.BOX_CUT]:
			box_stage = 0
		return
	var corners := _box_corners()
	var changed := corners.size() != box_shown.size()
	for i in mini(corners.size(), box_shown.size()):
		changed = changed or corners[i].distance_to(box_shown[i]) > 0.05
	if not changed:
		return
	box_shown = corners
	if box_preview == null:
		box_preview = DensityBody3D.new()
		box_preview.drawn = true
		box_preview.freeze = true
		box_preview.collide_with_bodies = false
		box_preview.pickable = false
		camera.get_parent().add_child(box_preview)
		box_preview.global_transform = Transform3D.IDENTITY  # the hull is in world space
	# A cut previews in red; a box in what it'll be made of.
	var material := SculptTerrain.color_to_material(Color(0.95, 0.25, 0.2)) if tool == Tool.BOX_CUT else _current_material()
	box_preview.shape = DensityShape.make_hull(corners, material)
