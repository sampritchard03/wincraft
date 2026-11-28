extends Node
## Sun shadows from two shadow maps instead of a ray to the sun from every
## pixel of every frame (which was about a quarter of the frame on a laptop
## GPU, and far more for a headset's two big views).
##
## Each map is a square of the plane facing the sun, centred near the
## camera: terrain_sun_map.gdshader marches one ray per texel from above it
## along the light, and stores how far it got before meeting the ground. The
## terrain shader then needs only four texel reads a pixel
## (terrain_shadow.gdshaderinc, sun_map_shadow()). Map 0 is small and sharp
## (NEAR_EXTENT voxels across), map 1 wide and coarse, reaching past where
## shadows fade out (terrain.gdshader: 300 to 700 voxels).
##
## A map is drawn again only when the camera has moved a snap step from its
## centre (the steps are whole texels, so shadows don't crawl as it moves) or
## the terrain changed (SculptTerrain.get_edit_serial(); at most every
## EDIT_DELAY). Each map has two buffers: the terrain shader reads one while
## the other is drawn, STRIPS frames for a whole map (a whole map in one
## frame was a ~14 ms hitch on a laptop GPU), and then they swap. One map is
## drawn at a time.

const SIZE := 512  # texels a side
const STRIPS := 4  # frames to draw one map
const NEAR_EXTENT := 256.0  # voxels across (0.5 a texel)
const FAR_EXTENT := 1536.0  # (3 a texel)
const NEAR_STEP := 32.0  # the camera moves this far from the centre: draw again
const FAR_STEP := 192.0
## How far above the camera (towards the sun) each map's rays start; they
## go twice that: occluders and receivers within that band count.
const NEAR_HEIGHT := 300.0
const FAR_HEIGHT := 800.0
const EDIT_DELAY := [0.1, 1.0]  # seconds between redraws for edits (near, far)

var terrain: SculptTerrain
var terrain_material: ShaderMaterial
var sun := Vector3.UP
var right := Vector3.RIGHT
var up := Vector3.FORWARD

class Cascade:
	var viewports: Array[SubViewport] = []  # two buffers
	var materials: Array[ShaderMaterial] = []
	var front := -1  # the buffer the terrain reads (-1: none yet)
	var extent: float
	var step: float
	var height: float
	var centre := Vector3.INF  # of the map being drawn, or last drawn
	var serial := -1  # the terrain's edit serial it was drawn from
	var started_at := -100.0
	var strip := -1  # the next strip of the back buffer to draw (-1: idle)

var cascades: Array[Cascade] = []

func _init(p_terrain: SculptTerrain) -> void:
	name = "SunShadows"
	terrain = p_terrain
	terrain_material = terrain.material

func _ready() -> void:
	sun = (terrain_material.get_shader_parameter("sun_direction") as Vector3).normalized()
	right = Vector3.UP.cross(sun).normalized() if absf(sun.y) < 0.99 else Vector3.RIGHT
	up = sun.cross(right).normalized()
	var shader: Shader = load("res://shaders/terrain_sun_map.gdshader")
	var extra: Array = []
	for i in 2:
		var c := Cascade.new()
		c.extent = NEAR_EXTENT if i == 0 else FAR_EXTENT
		c.step = NEAR_STEP if i == 0 else FAR_STEP
		c.height = NEAR_HEIGHT if i == 0 else FAR_HEIGHT
		for b in 2:
			var view := SubViewport.new()
			view.name = "SunMap%d%s" % [i, "ab"[b]]
			view.size = Vector2i(SIZE, SIZE)
			view.disable_3d = true
			view.use_hdr_2d = true  # 16-bit float: depth to about a voxel
			view.transparent_bg = false
			view.render_target_clear_mode = SubViewport.CLEAR_MODE_NEVER  # strips keep the rest
			view.render_target_update_mode = SubViewport.UPDATE_DISABLED
			var rect := ColorRect.new()
			rect.size = Vector2(SIZE, SIZE)
			var material := ShaderMaterial.new()
			material.shader = shader
			material.set_shader_parameter("map_right", right)
			material.set_shader_parameter("map_up", up)
			material.set_shader_parameter("sun_dir", sun)
			material.set_shader_parameter("map_extent", c.extent)
			material.set_shader_parameter("map_depth", c.height * 2.0)
			rect.material = material
			view.add_child(rect)
			add_child(view)
			c.viewports.append(view)
			c.materials.append(material)
			extra.append(material)
		cascades.append(c)
		terrain_material.set_shader_parameter("sun_map_extent%d" % i, c.extent)
		terrain_material.set_shader_parameter("sun_map_depth%d" % i, c.height * 2.0)
	terrain_material.set_shader_parameter("sun_map_right", right)
	terrain_material.set_shader_parameter("sun_map_up", up)
	terrain_material.set_shader_parameter("sun_map_size", float(SIZE))
	terrain_material.set_shader_parameter("sun_map_on", false)
	terrain.set_extra_materials(extra)  # they read the clipmap too

## Call each frame with the camera position (after the terrain's update_view).
func update(p_camera: Vector3) -> void:
	var now := Time.get_ticks_msec() / 1000.0
	var serial := terrain.get_edit_serial()
	for c in cascades:
		for view in c.viewports:
			view.render_target_update_mode = SubViewport.UPDATE_DISABLED
	# A map being drawn: its next strip, or (all drawn last frame) swap it in.
	for i in cascades.size():
		var c := cascades[i]
		if c.strip < 0:
			continue
		var back := 1 - maxi(c.front, 0) if c.front >= 0 else 0
		if c.strip >= STRIPS:
			c.front = back
			c.strip = -1
			terrain_material.set_shader_parameter("sun_map%d" % i, c.viewports[back].get_texture())
			terrain_material.set_shader_parameter("sun_map_origin%d" % i, c.centre + sun * c.height)
			terrain_material.set_shader_parameter("sun_map_on", cascades[0].front >= 0 and cascades[1].front >= 0)
			continue
		var material := c.materials[back]
		material.set_shader_parameter("band_lo", float(c.strip) / STRIPS)
		material.set_shader_parameter("band_hi", float(c.strip + 1) / STRIPS if c.strip + 1 < STRIPS else 2.0)
		c.viewports[back].render_target_update_mode = SubViewport.UPDATE_ONCE
		c.strip += 1
		return  # one strip a frame
	# Nothing being drawn: start the first map that needs it.
	for i in cascades.size():
		var c := cascades[i]
		var centre := _snapped_centre(p_camera, c.step)
		var moved := c.centre == Vector3.INF or (centre - c.centre).length() > 0.5
		var edited: bool = serial != c.serial and now - c.started_at >= EDIT_DELAY[i]
		if not (moved or edited):
			continue
		c.centre = centre
		c.serial = serial
		c.started_at = now
		c.strip = 0
		var back := 1 - c.front if c.front >= 0 else 0
		c.materials[back].set_shader_parameter("map_origin", centre + sun * c.height)
		return

## The camera's position on the sun's plane snapped to the step (a whole
## number of texels of either map), and along the sun to the same.
func _snapped_centre(p_camera: Vector3, p_step: float) -> Vector3:
	var a := snappedf(p_camera.dot(right), p_step)
	var b := snappedf(p_camera.dot(up), p_step)
	var c := snappedf(p_camera.dot(sun), p_step)
	return right * a + up * b + sun * c
