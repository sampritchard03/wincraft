extends "res://scripts/objects/entities/creature.gd"
## The frog: hops round to face somewhere new now and then, and every so
## often takes a big leap. Each jump crouches first (jump_start), then plays
## jump while it's in the air. A good shove knocks it over: it flails a
## while, then rolls itself upright (creature.gd, as the chiggin does).
##
## Its animations (the AnimationPlayer's: edit them with the object editor,
## M) move its pose:
##   idle        sitting
##   jump_start  crouching for a jump (sped up for a little hop)
##   jump        in the air (and knocked over)
##   head_turn   (optional, made in the editor) played on top; without it,
##               the turn_l / turn_r shape keys
##
## It faces along its +z.

const IDLE := "idle"
const JUMP_START := "jump_start"
const JUMP := "jump"
const HEAD_TURN := "head_turn"

## How long it crouches before a little turning hop (a big leap: the whole
## of jump_start).
@export var hop_crouch := 0.25
@export var hop_up := 10.6
@export var leap_speed := 21.0  # forwards and up, each (scaled with gravity: the same arc, slower)

var target_dir := Vector3.FORWARD
var aim_timer := 0.0
var hop_timer := 0.0
var leap_timer := 0.0
var crouch := -1.0  # >= 0: crouching, it jumps when this runs out
var crouch_leap := false
var jumping := false  # launched and not yet landed


func _base_animations() -> Array:
	return [IDLE, JUMP_START, JUMP]


func _head_animation() -> String:
	return HEAD_TURN


func _action() -> String:
	if state != State.UP:
		return JUMP
	if crouch >= 0.0:
		return JUMP_START
	if jumping or air_time > air_delay:
		return JUMP
	return IDLE


func _model_forward() -> Vector3:
	return Vector3.BACK  # (+z)


func _ready() -> void:
	super()
	anim_blend[IDLE] = 1.0
	leap_timer = randf_range(4.0, 15.0)


func _move(delta: float, p_on_feet: bool) -> void:
	var facing := _forward()
	facing = Vector3(facing.x, 0.0, facing.z)
	if facing.length_squared() < 1e-4:
		return
	facing = facing.normalized()
	aim_timer -= delta
	if aim_timer <= 0.0:
		aim_timer = randf_range(4.0, 10.0)
		var a := randf() * TAU
		target_dir = Vector3(sin(a), 0.0, cos(a))
	var angle := facing.signed_angle_to(target_dir, Vector3.UP)
	hop_timer -= delta
	leap_timer -= delta
	if jumping and touching and air_time == 0.0 and linear_velocity.y <= 0.5:
		jumping = false  # (landed)
	if crouch >= 0.0:
		if not p_on_feet:
			crouch = -1.0  # (knocked off its feet meanwhile)
			return
		crouch -= delta
		if crouch < 0.0:
			_launch(facing, angle)
		return
	if not p_on_feet or not touching or jumping:
		return
	if leap_timer <= 0.0:
		leap_timer = randf_range(10.0, 15.0)
		_crouch(true)
	elif hop_timer <= 0.0 and absf(angle) > 0.2:
		hop_timer = 0.6
		_crouch(false)


## Crouches for a jump: a big leap the whole of jump_start, a hop briefly
## (jump_start sped up to fit).
func _crouch(p_leap: bool) -> void:
	crouch_leap = p_leap
	var start := _animation(JUMP_START)
	var length := start.length if start != null else 0.5
	crouch = length if p_leap else minf(hop_crouch, length)
	restart(JUMP_START, length / maxf(crouch, 0.01))


func _launch(p_facing: Vector3, p_angle: float) -> void:
	if crouch_leap:
		apply_central_impulse(mass * (p_facing * leap_speed + Vector3.UP * leap_speed))
	else:
		linear_velocity.y += hop_up
		angular_velocity.y += clampf(p_angle * 3.0, -6.0, 6.0)
	jumping = true
	air_time = air_delay  # (in the air from now: it plays jump at once)
	restart(JUMP)
