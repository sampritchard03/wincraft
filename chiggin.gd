extends "res://creature.gd"
## The chiggin: wanders about on its feet and looks around as it goes. A
## good shove knocks it over: it flaps a while, then rolls itself upright
## (creature.gd).
##
## Its animations are in its AnimationPlayer, one file each in res://chiggin/
## (edit one on a chiggin with the object editor, M; every chiggin shares
## them, and they're saved with the world):
##   chiggin_idle       standing about
##   chiggin_walk       walking
##   chiggin_flap       in the air, or knocked over
##   chiggin_head_turn  (optional) played at the same time as the others;
##                      without it, the turn_l / turn_r shape keys
##
## It faces along its +x (where the beak is).

const IDLE := "chiggin_idle"
const WALK := "chiggin_walk"
const FLAP := "chiggin_flap"
const HEAD_TURN := "chiggin_head_turn"

@export var walk_speed := 7.0
@export var turn_speed := 2.5  # radians a second, at most

var wander_dir := Vector3.RIGHT
var wander_timer := 0.0
var walking := false


func _base_animations() -> Array:
	return [IDLE, WALK, FLAP]


func _head_animation() -> String:
	return HEAD_TURN


func _action() -> String:
	if state != State.UP or air_time > air_delay:
		return FLAP
	return WALK if walking and not freeze else IDLE


func _forward() -> Vector3:
	return global_basis.x


func _ready() -> void:
	super()
	anim_blend[IDLE] = 1.0
	wander_timer = randf_range(1.0, 4.0)
	var a := randf() * TAU
	wander_dir = Vector3(cos(a), 0.0, sin(a))


## Wandering: a while walking somewhere, a while standing.
func _move(delta: float, p_on_feet: bool) -> void:
	wander_timer -= delta
	if wander_timer <= 0.0:
		walking = not walking
		wander_timer = randf_range(3.0, 7.0) if walking else randf_range(2.0, 5.0)
		if walking:
			var a := randf() * TAU
			wander_dir = Vector3(cos(a), 0.0, sin(a))
	var facing := _forward()
	facing = Vector3(facing.x, 0.0, facing.z)
	if facing.length_squared() > 1e-4 and p_on_feet:
		facing = facing.normalized()
		var angle := facing.signed_angle_to(wander_dir, Vector3.UP)
		var want := clampf(angle * 3.0, -turn_speed, turn_speed) if walking else 0.0
		angular_velocity.y = lerpf(angular_velocity.y, want, minf(delta * 8.0, 1.0))
		if walking and touching:
			var flat := Vector3(linear_velocity.x, 0.0, linear_velocity.z)
			var go := facing * walk_speed * clampf(1.0 - absf(angle) / PI, 0.3, 1.0)
			linear_velocity += (go - flat) * minf(delta * 6.0, 1.0)
