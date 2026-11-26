extends DensityBody3D
## The chiggin: wanders about on its feet, staying mostly upright, and looks
## around as it goes.
##
## Its animations are in its AnimationPlayer, one file each in res://chiggin/
## (empty until keyframed: edit one on a chiggin with the object editor, M,
## and every chiggin shows it; "Export animation" over its file keeps it):
##   chiggin_idle       standing about
##   chiggin_walk       walking
##   chiggin_flap       off the ground, or knocked over
##   chiggin_head_turn  (optional) played at the same time as the others,
##                      on a second player, if it has any tracks
## Without a head turn animation of its own, it turns its head with two
## shape keys, turn_l and turn_r (each the head turned 90 degrees that way:
## sculpt them in the object editor), weighed in by where it looks. They're
## added to its shape (empty) if it hasn't got them.
##
## It faces along its +x (where the beak is).

const IDLE := "chiggin_idle"
const WALK := "chiggin_walk"
const FLAP := "chiggin_flap"
const HEAD_TURN := "chiggin_head_turn"
const TURN_L := "turn_l"
const TURN_R := "turn_r"

@export var walk_speed := 7.0
## How hard it rights itself (per radian of tilt), and how much that's damped.
@export var upright_strength := 20.0
@export var upright_damping := 7.0
@export var turn_speed := 2.5  # radians a second, at most
## Off the ground (or tipped past upright_angle) this long: it flaps.
@export var flap_delay := 0.15
@export var upright_angle := 40.0  # degrees

@onready var animator: AnimationPlayer = $AnimationPlayer
var look_player: AnimationPlayer  # the head turn, at the same time
var wander_dir := Vector3.RIGHT
var wander_timer := 0.0
var walking := false
var air_time := 0.0
var look_angle := 0.0  # radians, + to its left
var look_target := 0.0
var look_timer := 0.0
var turn_keys_of: DensityShape = null
var key_l := -1
var key_r := -1


func _ready() -> void:
	_ensure_turn_keys()
	look_player = AnimationPlayer.new()
	look_player.name = "LookPlayer"
	add_child(look_player)
	var lib := AnimationLibrary.new()
	if animator.has_animation(HEAD_TURN):
		lib.add_animation(HEAD_TURN, animator.get_animation(HEAD_TURN))  # (the same one: edits show at once)
	look_player.add_animation_library("", lib)
	wander_timer = randf_range(1.0, 4.0)
	var a := randf() * TAU
	wander_dir = Vector3(cos(a), 0.0, sin(a))


## turn_l and turn_r, empty, if its shape hasn't got them.
func _ensure_turn_keys() -> void:
	if shape == null or shape.get_key_count() == 0:
		return
	for key_name in [TURN_L, TURN_R]:
		if _key_named(key_name) < 0:
			var k := shape.add_key(0.0)
			shape.set_key_name(k, key_name)


func _key_named(p_name: String) -> int:
	for k in shape.get_key_count():
		if shape.get_key_name(k) == p_name:
			return k
	return -1


func _physics_process(delta: float) -> void:
	if has_meta("editing") or freeze:
		return  # (the object editor has it, or it's pinned)
	var up := global_basis.y.normalized()
	var upright := up.dot(Vector3.UP) > cos(deg_to_rad(upright_angle))
	var grounded := get_contact_count() > 0
	air_time = 0.0 if grounded and upright else air_time + delta
	var flapping := air_time > flap_delay

	# Righting itself: spun towards upright about the horizontal, damped.
	var tilt := up.cross(Vector3.UP)
	var spin := Vector3(angular_velocity.x, 0.0, angular_velocity.z)
	var strength := upright_strength * (1.6 if flapping else 1.0)
	angular_velocity += (tilt * strength - spin * upright_damping) * delta

	# Wandering: a while walking somewhere, a while standing.
	wander_timer -= delta
	if wander_timer <= 0.0:
		walking = not walking
		wander_timer = randf_range(3.0, 7.0) if walking else randf_range(2.0, 5.0)
		if walking:
			var a := randf() * TAU
			wander_dir = Vector3(cos(a), 0.0, sin(a))
	var facing := global_basis.x
	facing = Vector3(facing.x, 0.0, facing.z)
	if facing.length_squared() > 1e-4:
		facing = facing.normalized()
		var angle := facing.signed_angle_to(wander_dir, Vector3.UP)
		if grounded and not flapping:
			var want := clampf(angle * 3.0, -turn_speed, turn_speed) if walking else 0.0
			angular_velocity.y = lerpf(angular_velocity.y, want, minf(delta * 8.0, 1.0))
			if walking:
				var flat := Vector3(linear_velocity.x, 0.0, linear_velocity.z)
				var go := facing * walk_speed * clampf(1.0 - absf(angle) / PI, 0.3, 1.0)
				linear_velocity += (go - flat) * minf(delta * 6.0, 1.0)

	var action := FLAP if flapping else (WALK if walking else IDLE)
	if animator.has_animation(action) and animator.current_animation != action:
		animator.play(action, 0.2)
	_look_around(delta)


## The head: its own animation, played over whatever else it does, or the
## turn keys weighed in by where it looks (now and then somewhere new).
func _look_around(delta: float) -> void:
	var anim := look_player.get_animation(HEAD_TURN) if look_player.has_animation(HEAD_TURN) else null
	if anim != null and anim.get_track_count() > 0:
		if not look_player.is_playing():
			look_player.play(HEAD_TURN)
		return
	if look_player.is_playing():
		look_player.stop()
	look_timer -= delta
	if look_timer <= 0.0:
		look_timer = randf_range(1.0, 4.0)
		look_target = 0.0 if randf() < 0.35 else randf_range(-PI * 0.5, PI * 0.5)
	look_angle = move_toward(look_angle, look_target, delta * 2.5)
	if shape != turn_keys_of:
		turn_keys_of = shape
		key_l = _key_named(TURN_L) if shape != null else -1
		key_r = _key_named(TURN_R) if shape != null else -1
	var turned := look_angle / (PI * 0.5)
	if key_l >= 0:
		set_key_weight(key_l, clampf(turned, 0.0, 1.0))
	if key_r >= 0:
		set_key_weight(key_r, clampf(-turned, 0.0, 1.0))
