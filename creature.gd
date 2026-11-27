extends DensityBody3D
## What frogs and chiggins have in common (chiggin.gd, forg.gd extend it):
##
## Falling over: it only holds itself up gently while nearly upright, so a
## good shove knocks it over. Tipped past fall_angle it's down: it lies
## there a while (flailing: its air animation), then rolls itself upright.
##
## Animation: the script plays its AnimationPlayer's animations itself, in
## layers, rather than through the AnimationPlayer (which plays one at a
## time): a base layer, blended from one animation into the next (each
## carrying on from where it was when it's a looping one: a walk interrupted
## by a moment's flap picks up mid-stride; one that doesn't loop starts
## over), and a head layer on top. On keys both layers move, the head's wins.
## Edit them on one with the object editor (M); every one of a kind shares
## them, and they're saved with the world ("Make game default" in the
## editor makes them the game's own).
##
## The head layer: head_animation if it has keyframes, otherwise two shape
## keys, turn_l and turn_r (each the head turned 90 degrees that way: sculpt
## them in the object editor), weighed in by where it looks. They're added
## to its shape (empty) if it hasn't got them.

const TURN_L := "turn_l"
const TURN_R := "turn_r"

## Keeping upright: how hard it leans back (per radian of tilt) and how much
## that's damped. Gentle, so a shove can still knock it over.
@export var balance_strength := 8.0
@export var balance_damping := 4.0
@export var balance_angle := 25.0  # degrees: leaning further, it holds on less and less (none 20 past it)
## Righting itself once it's down: harder.
@export var righting_strength := 26.0
@export var righting_damping := 7.0
## Tipped past this, it's knocked over: down for down_time seconds, then it
## rolls upright again (it's back on its feet within upright_angle).
@export var fall_angle := 55.0  # degrees
@export var upright_angle := 20.0
@export var down_time := 1.2
## Off the ground this long, it's in the air (shorter gaps, between steps, don't count).
@export var air_delay := 0.3
## Seconds to blend from one animation into the next.
@export var blend_time := 0.25
## Which way its model faces: degrees about its up from the way the script
## expects (_model_forward()), for a model made facing elsewhere. Every one of
## a kind shares it (the object editor's "Faces" sets it; saved with the world).
@export var facing_offset := 0.0

enum State { UP, DOWN, RIGHTING }

@onready var animator: AnimationPlayer = get_node_or_null("AnimationPlayer")
var state := State.UP
var down_timer := 0.0
var air_time := 0.0
var touching := false
var look_angle := 0.0  # radians, + to its left
var look_target := 0.0
var look_timer := 0.0
var turn_keys_of: DensityShape = null
var key_l := -1
var key_r := -1
## Animation layers: each base animation's own time, speed and blend weight, and the head's time.
var anim_times := {}
var anim_speeds := {}
var anim_blend := {}
var head_time := 0.0
var animated_keys := {}  # key index (-1: pose) -> true: what the layers set last frame
var last_action := ""


## What the base layer can play (subclasses: their animations' names).
func _base_animations() -> Array:
	return []


## The head layer's own animation's name ("" for none: the turn keys).
func _head_animation() -> String:
	return ""


## Which base animation plays now.
func _action() -> String:
	return ""


## Which way the script expects a model to face (its own space), before facing_offset.
func _model_forward() -> Vector3:
	return Vector3.RIGHT


## Which way it faces (world space).
func _forward() -> Vector3:
	return global_basis * Basis(Vector3.UP, deg_to_rad(facing_offset)) * _model_forward()


## What it does on its own (walking, hopping), each physics frame. p_on_feet:
## upright and on the ground (or only just off it).
func _move(_delta: float, _on_feet: bool) -> void:
	pass


func _ready() -> void:
	_ensure_turn_keys()
	for anim_name in _base_animations():
		anim_times[anim_name] = randf() * 0.5  # (not all in step)
	var tree := get_node_or_null("AnimationTree") as AnimationTree
	if tree != null:
		tree.active = false  # (the script plays them)


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


## Whether it's on its feet (not knocked over, and not long off the ground).
func on_feet() -> bool:
	return state == State.UP and air_time < air_delay


func _physics_process(delta: float) -> void:
	if has_meta("editing") or freeze:
		return  # (the object editor has it, or it's pinned)
	var up := global_basis.y.normalized()
	var tilt_angle := rad_to_deg(up.angle_to(Vector3.UP))
	touching = get_contact_count() > 0
	air_time = 0.0 if touching else air_time + delta

	match state:
		State.UP:
			if tilt_angle > fall_angle:
				state = State.DOWN
				down_timer = down_time
		State.DOWN:
			down_timer -= delta
			if down_timer <= 0.0:
				state = State.RIGHTING
		State.RIGHTING:
			if tilt_angle < upright_angle:
				state = State.UP

	# Leaning back towards upright (about the horizontal, damped): gently
	# while up, not at all while down, hard while righting itself.
	var tilt := up.cross(Vector3.UP)
	var spin := Vector3(angular_velocity.x, 0.0, angular_velocity.z)
	if state == State.UP:
		# (fading out as it leans past balance_angle: a shove that far topples it)
		var hold := clampf(inverse_lerp(balance_angle + 20.0, balance_angle, tilt_angle), 0.0, 1.0)
		angular_velocity += (tilt * balance_strength - spin * balance_damping) * hold * delta
	elif state == State.RIGHTING:
		if tilt.length_squared() < 1e-4:
			tilt = _forward().cross(Vector3.UP)  # (upside down: any way over)
		angular_velocity += (tilt.normalized() * deg_to_rad(tilt_angle) * righting_strength - spin * righting_damping) * delta
	_move(delta, on_feet())


func _process(delta: float) -> void:
	if has_meta("editing") or not get_meta("net_sim", true):
		return  # (the editor plays its animations; a copy is posed by its owner)
	if animator != null and animator.is_playing():
		animator.stop()  # (the script plays them: both at once fight over the keys)
	_animate(_action(), delta)


## Starts a base animation over (its time back to 0), at p_speed.
func restart(p_name: String, p_speed := 1.0) -> void:
	anim_times[p_name] = 0.0
	anim_speeds[p_name] = p_speed


## Plays the layers: the base animation crossfaded to p_action, the head on
## top, and sets the key weights (and pose) they move.
func _animate(p_action: String, delta: float) -> void:
	if shape != turn_keys_of:
		turn_keys_of = shape
		_ensure_turn_keys()
		key_l = _key_named(TURN_L) if shape != null else -1
		key_r = _key_named(TURN_R) if shape != null else -1
	if shape == null:
		return
	# A non-looping animation starts over each time it's started (one that
	# loops carries on).
	if p_action != last_action:
		var started := _animation(p_action)
		if started != null and started.loop_mode == Animation.LOOP_NONE and anim_blend.get(p_action, 0.0) < 0.5:
			anim_times[p_action] = 0.0
		last_action = p_action
	var values := {}  # key index (-1: pose) -> value
	# Base layer: weights move towards the current animation (summing to 1
	# all the way: what one gives up the other takes), each one's own time
	# running while it's weighed in at all. A key one animation moves and
	# another doesn't is at 0 in the other (as the AnimationPlayer would have it).
	var step := delta / maxf(blend_time, 0.01)
	var names := _base_animations()
	var target: float = anim_blend.get(p_action, 0.0)
	var gain := minf(step, 1.0 - target) if names.has(p_action) else 0.0
	var others := 0.0
	for anim_name in names:
		if anim_name != p_action:
			others += anim_blend.get(anim_name, 0.0)
	for anim_name in names:
		var w: float = anim_blend.get(anim_name, 0.0)
		if anim_name == p_action:
			w = minf(w + gain, 1.0)
		elif others > 0.0:
			w = maxf(w - (gain if names.has(p_action) else step) * w / others, 0.0)
		anim_blend[anim_name] = w
		var anim := _animation(anim_name)
		if w <= 0.0 or anim == null or anim.get_track_count() == 0:
			continue
		anim_times[anim_name] = anim_times.get(anim_name, 0.0) + delta * anim_speeds.get(anim_name, 1.0)
		var sampled := _sample(anim, anim_times[anim_name])
		for k in sampled:
			values[k] = values.get(k, 0.0) + sampled[k] * w
	# Head layer: its own animation, or the turn keys.
	var head_name := _head_animation()
	var head := _animation(head_name) if head_name != "" else null
	if head != null and head.get_track_count() > 0:
		head_time += delta
		var sampled := _sample(head, head_time)
		for k in sampled:
			values[k] = sampled[k]
	else:
		_look_around(delta)
		var turned := look_angle / (PI * 0.5)
		if key_l >= 0:
			values[key_l] = clampf(turned, 0.0, 1.0)
		if key_r >= 0:
			values[key_r] = clampf(-turned, 0.0, 1.0)
	# Keys a layer let go of since last frame go back to 0.
	for k in animated_keys:
		if not values.has(k):
			values[k] = 0.0
	animated_keys = {}
	for k in values:
		if values[k] != 0.0:
			animated_keys[k] = true
		if k == -1:
			if absf(pose - values[k]) > 1e-5:
				pose = values[k]
		elif k < shape.get_key_count() and absf(get_key_weight(k) - values[k]) > 1e-5:
			set_key_weight(k, values[k])


func _animation(p_name: String) -> Animation:
	if animator == null or p_name == "" or not animator.has_animation(p_name):
		return null
	return animator.get_animation(p_name)


## An animation's key weights (and pose, as -1) at p_time, looping as it says.
func _sample(p_anim: Animation, p_time: float) -> Dictionary:
	var length := maxf(p_anim.length, 0.001)
	var t := p_time
	match p_anim.loop_mode:
		Animation.LOOP_LINEAR:
			t = fposmod(p_time, length)
		Animation.LOOP_PINGPONG:
			t = pingpong(p_time, length)
		_:
			t = minf(p_time, length)
	var out := {}
	for track in p_anim.get_track_count():
		if p_anim.track_get_type(track) != Animation.TYPE_VALUE or p_anim.track_get_key_count(track) == 0:
			continue
		var path := str(p_anim.track_get_path(track))
		var at := path.find(":key_weight/")
		var key := -2
		if at >= 0:
			key = path.substr(at + 12).to_int()
		elif path.ends_with(":pose"):
			key = -1
		if key == -2:
			continue
		var value = p_anim.value_track_interpolate(track, t)
		if value is float or value is int:
			out[key] = float(value)
	return out


## Where it looks: now and then somewhere new, the head easing round.
func _look_around(delta: float) -> void:
	look_timer -= delta
	if look_timer <= 0.0:
		look_timer = randf_range(1.0, 4.0)
		look_target = 0.0 if randf() < 0.35 else randf_range(-PI * 0.5, PI * 0.5)
	look_angle = move_toward(look_angle, look_target, delta * 2.5)
