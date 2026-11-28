extends SceneTree
## Turns the frog's animated mesh (forg_model.tscn) into a density body:
## each pose below becomes a key of a DensityShape (forg_shape.res), and
## forg.tscn is rebuilt as a DensityBody3D with an AnimationPlayer whose
## animations move its `pose` between the keys, driven by the same
## AnimationTree state machine as before.
##
## Run it again after changing the model in Blender (with Godot closed, or
## from a terminal), from the project folder:
##   Godot_v4.7.2-stable_win64_console.exe --path . --headless --script bake_frog.gd
## It replaces forg_shape.res and forg.tscn (sculpted changes to them are lost).

## Pose keys: an animation and the time in it.
const POSES := [["idle", 0.0], ["jump_start", -1.0], ["jump", -1.0]]  # -1: its end
## The model's size in the frog scene (it was scaled 8x there).
const MODEL_SCALE := 8.0
const VOXEL_SIZE := 1.0
const MARGIN := 2.0

func _initialize() -> void:
	bake.call_deferred()

func bake() -> void:
	var model: Node3D = load("res://scenes/entities/forg_model.tscn").instantiate()
	model.scale = Vector3.ONE * MODEL_SCALE
	root.add_child(model)
	var tree := model.get_node("AnimationTree") as AnimationTree
	var tree_root: AnimationRootNode = tree.tree_root.duplicate(true)
	tree.active = false  # the player is posed by hand below
	var player := model.get_node("AnimationPlayer") as AnimationPlayer
	var mesh := model.find_children("*", "MeshInstance3D")[0] as MeshInstance3D

	var poses := []
	var colors := []
	var lengths := {}
	for pose in POSES:
		var animation: Animation = player.get_animation(pose[0])
		lengths[pose[0]] = animation.length
		var time: float = animation.length if pose[1] < 0.0 else pose[1]
		player.play(pose[0])
		player.seek(time, true)
		player.pause()
		await process_frame
		await process_frame
		var baked := mesh.bake_mesh_from_current_skeleton_pose()
		var to_frog := mesh.global_transform  # the model's root is the frog's origin
		var triangles := PackedVector3Array()
		var vertex_colors := PackedColorArray()
		for surface in baked.get_surface_count():
			var arrays := baked.surface_get_arrays(surface)
			var vertices: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
			var surface_colors = arrays[Mesh.ARRAY_COLOR]
			var indices = arrays[Mesh.ARRAY_INDEX]
			var order: PackedInt32Array = indices if indices != null else PackedInt32Array(range(vertices.size()))
			for i in order:
				triangles.append(to_frog * vertices[i])
				vertex_colors.append(surface_colors[i] if surface_colors != null else Color.WHITE)
		poses.append(triangles)
		colors.append(vertex_colors)
		print("pose %s at %.2f s: %d triangles" % [pose[0], time, triangles.size() / 3])

	var shape := DensityShape.new()
	shape.bake_meshes(poses, colors, VOXEL_SIZE, MARGIN)
	print("grid ", shape.get_size(), " voxel ", shape.voxel_size)
	ResourceSaver.save(shape, "res://assets/entities/frog/forg_shape.res")

	# The frog scene.
	var frog := DensityBody3D.new()
	frog.name = "Forg"
	frog.shape = load("res://assets/entities/frog/forg_shape.res")
	frog.drawn = true
	frog.bounce = 0.29
	frog.gravity_scale = 0.212  # ~51 voxels/s^2 with gravity 240
	frog.angular_damp = 1.316
	frog.auto_material_properties = false  # its own mass and bounce, set here
	frog.auto_center_of_mass = false
	frog.center_of_mass = Vector3(0, -5, 0)  # well below its middle: it rights itself
	frog.set_script(load("res://scripts/objects/entities/forg.gd"))
	var animations := AnimationPlayer.new()
	animations.name = "AnimationPlayer"
	var library := AnimationLibrary.new()
	library.add_animation("idle", _pose_animation(lengths["idle"], [0.0, 0.0], true))
	library.add_animation("jump_start", _pose_animation(lengths["jump_start"], [0.0, 1.0], false))
	library.add_animation("jump", _pose_animation(lengths["jump"], [1.0, 2.0], false))
	animations.add_animation_library("", library)
	frog.add_child(animations)
	animations.owner = frog
	var new_tree := AnimationTree.new()
	new_tree.name = "AnimationTree"
	new_tree.tree_root = tree_root
	frog.add_child(new_tree)
	new_tree.owner = frog
	new_tree.anim_player = NodePath("../AnimationPlayer")
	var scene := PackedScene.new()
	scene.pack(frog)
	# Keep forg.tscn's id, so world.tscn's reference to it still matches.
	var old_uid := ResourceLoader.get_resource_uid("res://scenes/entities/forg.tscn")
	ResourceSaver.save(scene, "res://scenes/entities/forg.tscn")
	if old_uid != ResourceUID.INVALID_ID:
		ResourceSaver.set_uid("res://scenes/entities/forg.tscn", old_uid)
	print("saved forg_shape.res and forg.tscn")
	quit()

## An animation of the body's pose from p_from_to[0] to [1] over p_length.
func _pose_animation(p_length: float, p_from_to: Array, p_loop: bool) -> Animation:
	var animation := Animation.new()
	animation.length = p_length
	animation.loop_mode = Animation.LOOP_LINEAR if p_loop else Animation.LOOP_NONE
	var track := animation.add_track(Animation.TYPE_VALUE)
	animation.track_set_path(track, NodePath(".:pose"))
	animation.track_insert_key(track, 0.0, p_from_to[0])
	animation.track_insert_key(track, p_length, p_from_to[1])
	return animation
