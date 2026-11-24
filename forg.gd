extends DensityBody3D

@onready var playback: AnimationNodeStateMachinePlayback = $AnimationTree.get("parameters/playback")
var target_dir := Vector3.FORWARD
var look_timer := 0.0
var hop_timer := 0.0
var leap_timer := 0.0

# Called when the node enters the scene tree for the first time.
func _ready() -> void:
	pass # Replace with function body.


# Called every frame. 'delta' is the elapsed time since the previous frame.
func _physics_process(delta: float) -> void:
	var facing := global_basis.z
	facing = Vector3(facing.x, 0.0, facing.z).normalized()
	
	look_timer -= delta
	if look_timer <= 0.0:
		look_timer = randf_range(4.0, 10.0)
		var a := randf() * TAU
		target_dir = Vector3(sin(a), 0.0, cos(a))
	var angle := facing.signed_angle_to(target_dir, Vector3.UP)
	
	hop_timer -= delta
	leap_timer -= delta
	if get_contact_count() > 0 and absf(angle) > 0.2:
		if hop_timer <= 0.0:
			hop_timer = 0.6
			linear_velocity.y += 10.6
			angular_velocity.y += clampf(angle * 3.0, -6.0, 6.0)
		if leap_timer <= 0.0:
			leap_timer = randf_range(10.0, 15.0)
			apply_central_impulse(mass * (facing * 21.0 + Vector3.UP * 21.0))  # (scaled with gravity: the same arc, slower)
