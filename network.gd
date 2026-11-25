extends Node
## Multiplayer, steps 1 and 2: a shared world, each other's players, and
## shared sculpting.
##
## Start the game with arguments after "--":
##   godot-4 --path . -- --host [--port 24565] [--name Sam]
##   godot-4 --path . -- --join 192.168.1.20[:24565] [--name Alex]
## Without them it is single-player, as before.
##
## The host plays its own saved world. A player who joins gets the host's
## generator settings (so their computer generates the same ground) and the
## host's edited chunks, then plays unsaved: the world is the host's. Each
## computer moves its own player and sends where it is to the others about
## 20 times a second; other players show as coloured capsules with names.
##
## Sculpting (step 2): every tool's changes to the terrain (and undo / redo)
## are shared as the voxel values they changed, not as brush strokes:
## SculptTerrain's change tracking records which points each edit changed,
## and every EDIT_INTERVAL they go out as a batch (a few KB per chunk). The
## host puts every batch in one order and sends it to everyone (its own
## straight away; a joining player's back to them too, as a receipt). Each
## computer applies them in that order, so all copies end up the same:
## exact values, so nothing drifts. Your own edits show at once; a batch from
## someone else that the host ordered before yours is applied under your
## unconfirmed batches (they're put back on top), as it will be everywhere.
## Physics objects and frogs aren't shared yet (step 3 of
## diagrams/12-multiplayer.mmd).
##
## Positions go over the network in wrapped world coordinates (x and z in
## [0, world_size)): world.gd moves its coordinates by whole world sizes to
## keep the player near 0 (_wrap_player()), differently on each computer.
## Each receiver puts a remote player at the repeat nearest its own.

signal settings_received  ## joining: open the world with host_settings, then call world_opened()
signal joined  ## joining: every edited chunk has arrived and been applied
signal join_failed

const DEFAULT_PORT := 24565
const MAX_PLAYERS := 8
const SEND_INTERVAL := 0.05  # seconds between player updates
const CHUNKS_PER_FRAME := 8  # sent to a joining player each frame
const EDIT_INTERVAL := 0.1  # seconds between batches of terrain changes

enum Mode { OFFLINE, HOST, CLIENT }

var world: Node  # world.gd
var mode := Mode.OFFLINE
var address := ""
var port := DEFAULT_PORT
var player_name := ""
var status := ""

## Joining: the host's settings and how many chunks are still to come.
var host_settings: Dictionary = {}
var host_spawn := Vector3.ZERO
var chunks_expected := -1
var chunks_received := 0
var synced := false
var early: Array[Callable] = []  # world data that came before the world was open, in order

## Host: chunks still to send to each joining peer.
var outgoing: Dictionary = {}  # peer id -> Array[Vector3i]

## Sculpting: this computer's batches the host hasn't sent back yet
## ([id, changes]; joining players only), and counts for the HUD.
var unconfirmed: Array = []
var next_batch_id := 0
var edit_timer := 0.0
var batches_sent := 0
var batches_received := 0

## Other players: peer id -> {"node", "label", "target", "yaw", "height", "name"}.
var remotes: Dictionary = {}
var send_timer := 0.0


## Reads the command line. Returns false if the game should be single-player.
func configure_from_args() -> bool:
	var args := OS.get_cmdline_user_args()
	var i := 0
	while i < args.size():
		var arg := args[i]
		var value := args[i + 1] if i + 1 < args.size() else ""
		match arg:
			"--host":
				mode = Mode.HOST
			"--join":
				mode = Mode.CLIENT
				address = value
				i += 1
			"--port":
				port = int(value)
				i += 1
			"--name":
				player_name = value
				i += 1
		i += 1
	if mode == Mode.CLIENT and ":" in address:
		port = int(address.get_slice(":", 1))
		address = address.get_slice(":", 0)
	if player_name.is_empty():
		player_name = "Host" if mode == Mode.HOST else "Player"
	return mode != Mode.OFFLINE

func is_joining() -> bool:
	return mode == Mode.CLIENT

func is_online() -> bool:
	return mode != Mode.OFFLINE


# --- Starting ---------------------------------------------------------------------

## The host: call once the world is open.
func start_host() -> void:
	var peer := ENetMultiplayerPeer.new()
	var error := peer.create_server(port, MAX_PLAYERS)
	if error != OK:
		push_error("Couldn't host on port %d: %s" % [port, error_string(error)])
		mode = Mode.OFFLINE
		status = "Hosting failed (port %d)" % port
		return
	multiplayer.multiplayer_peer = peer
	multiplayer.peer_connected.connect(_on_peer_connected)
	multiplayer.peer_disconnected.connect(_on_peer_disconnected)
	world.terrain.set_change_tracking(true)
	status = "Hosting on port %d" % port
	print("Network: ", status)

## A joining player: call before the world is opened; `joined` (or
## `join_failed`) follows.
func start_client() -> void:
	var peer := ENetMultiplayerPeer.new()
	var error := peer.create_client(address, port)
	if error != OK:
		_fail("Couldn't connect to %s:%d: %s" % [address, port, error_string(error)])
		return
	multiplayer.multiplayer_peer = peer
	multiplayer.connected_to_server.connect(func() -> void:
		status = "Connected, waiting for the world..."
		print("Network: connected to ", address))
	multiplayer.connection_failed.connect(func() -> void: _fail("Couldn't reach %s:%d" % [address, port]))
	multiplayer.server_disconnected.connect(_on_server_disconnected)
	multiplayer.peer_disconnected.connect(_on_peer_disconnected)
	status = "Connecting to %s:%d..." % [address, port]
	print("Network: ", status)

func _fail(p_message: String) -> void:
	push_error(p_message)
	status = p_message
	mode = Mode.OFFLINE
	multiplayer.multiplayer_peer = null
	join_failed.emit()
	# world.gd may be waiting for either.
	settings_received.emit()
	joined.emit()


# --- Host: sending the world -----------------------------------------------------

func _on_peer_connected(p_id: int) -> void:
	if not multiplayer.is_server():
		return
	var chunks: Array = world.terrain.get_edited_chunks()
	outgoing[p_id] = chunks
	print("Network: player %d joined, sending %d edited chunks" % [p_id, chunks.size()])
	_welcome.rpc_id(p_id, world.terrain.get_world_settings(), _wrapped(world.player.global_position), chunks.size())
	# Where everyone already is, so the new player sees them at once.
	_player_state.rpc_id(p_id, _wrapped(world.player.global_position), world.yaw, world.player_height, player_name)

func _send_chunks() -> void:
	for id in outgoing.keys():
		var list: Array = outgoing[id]
		if not multiplayer.get_peers().has(id):
			outgoing.erase(id)
			continue
		var coords: Array = []
		var data: Array = []
		while not list.is_empty() and coords.size() < CHUNKS_PER_FRAME:
			var c: Vector3i = list.pop_back()
			coords.append(c)
			data.append(world.terrain.get_chunk_data(c))
		if not coords.is_empty():
			_chunks.rpc_id(id, coords, data)
		if list.is_empty():
			outgoing.erase(id)


# --- Joining: receiving the world ---------------------------------------------------

@rpc("authority", "call_remote", "reliable")
func _welcome(p_settings: Dictionary, p_host_position: Vector3, p_chunk_count: int) -> void:
	host_settings = p_settings
	host_spawn = p_host_position
	chunks_expected = p_chunk_count
	status = "Receiving the world..."
	print("Network: world settings received, %d edited chunks to come" % p_chunk_count)
	settings_received.emit()

@rpc("authority", "call_remote", "reliable", 1)
func _chunks(p_coords: Array, p_data: Array) -> void:
	if not world.terrain.is_world_open():
		early.append(_apply_chunks.bind(p_coords, p_data))  # (the settings come on another channel)
		return
	_apply_chunks(p_coords, p_data)

func _apply_chunks(p_coords: Array, p_data: Array) -> void:
	for i in p_coords.size():
		world.terrain.set_chunk_data(p_coords[i], p_data[i])
	chunks_received += p_coords.size()
	status = "Receiving the world: %d / %d chunks" % [chunks_received, chunks_expected]
	_check_synced()

## world.gd: the world is open with host_settings.
func world_opened() -> void:
	for call in early:
		call.call()
	early.clear()
	_check_synced()

func _check_synced() -> void:
	if synced or chunks_expected < 0 or chunks_received < chunks_expected or not world.terrain.is_world_open():
		return
	synced = true
	world.terrain.set_change_tracking(true)  # from now on, our edits are shared
	status = "Joined %s:%d" % [address, port]
	print("Network: ", status)
	joined.emit()


# --- Sculpting ---------------------------------------------------------------------------

## Sends what this computer's edits changed since last time (to the host, or
## from the host straight to everyone).
func _flush_edits() -> void:
	if not world.terrain.is_change_tracking():
		return
	var changes: Array = world.terrain.take_changes()
	if changes.is_empty() or multiplayer.multiplayer_peer == null:
		return
	if mode == Mode.HOST:
		if not multiplayer.get_peers().is_empty():
			_edits.rpc(1, 0, changes)
			batches_sent += 1
	else:
		batches_sent += 1
		next_batch_id += 1
		unconfirmed.append([next_batch_id, changes])
		_submit_edits.rpc_id(1, next_batch_id, changes)

## A joining player's batch, to the host: it takes its place in the order.
@rpc("any_peer", "call_remote", "reliable", 1)
func _submit_edits(p_batch_id: int, p_changes: Array) -> void:
	if not multiplayer.is_server():
		return
	var sender := multiplayer.get_remote_sender_id()
	_flush_edits()  # the host's own edits so far come first
	_apply_changes(p_changes)
	batches_received += 1
	_edits.rpc(sender, p_batch_id, p_changes)  # to everyone (the sender: as a receipt)

## A batch in the host's order (p_origin: whose it was).
@rpc("authority", "call_remote", "reliable", 1)
func _edits(p_origin: int, p_batch_id: int, p_changes: Array) -> void:
	if not world.terrain.is_world_open():
		early.append(_apply_edits.bind(p_origin, p_batch_id, p_changes))
		return
	_apply_edits(p_origin, p_batch_id, p_changes)

func _apply_edits(p_origin: int, p_batch_id: int, p_changes: Array) -> void:
	if p_origin == multiplayer.get_unique_id():
		# Ours, now in the order: already applied here.
		for i in unconfirmed.size():
			if unconfirmed[i][0] == p_batch_id:
				unconfirmed.remove_at(i)
				break
		return
	# Our own edits so far become a batch first (so they're kept on top too),
	# then this goes under everything of ours the host hasn't ordered yet.
	_flush_edits()
	_apply_changes(p_changes)
	for batch in unconfirmed:
		_apply_changes(batch[1])
	batches_received += 1

func _apply_changes(p_changes: Array) -> void:
	for i in range(0, p_changes.size() - 1, 2):
		world.terrain.apply_change(p_changes[i], p_changes[i + 1])


# --- Players ---------------------------------------------------------------------------

func _on_peer_disconnected(p_id: int) -> void:
	print("Network: player %d left" % p_id)
	_remove_remote(p_id)
	outgoing.erase(p_id)

func _on_server_disconnected() -> void:
	if not synced:
		_fail("The host left before the world arrived")
		return
	status = "The host left: playing on alone (unsaved)"
	print("Network: ", status)
	world.terrain.set_change_tracking(false)
	unconfirmed.clear()
	for id in remotes.keys():
		_remove_remote(id)
	multiplayer.multiplayer_peer = null
	mode = Mode.OFFLINE

## Sends this player's state to everyone (the host relays to the others).
@rpc("any_peer", "call_remote", "unreliable_ordered")
func _player_state(p_position: Vector3, p_yaw: float, p_height: float, p_name: String) -> void:
	var id := multiplayer.get_remote_sender_id()
	if id == 0 or not is_instance_valid(world.player):
		return
	if not remotes.has(id):
		_add_remote(id)
	var r: Dictionary = remotes[id]
	r.target = p_position
	r.yaw = p_yaw
	if r.height != p_height or r.name != p_name:
		r.height = p_height
		r.name = p_name
		_shape_remote(r)

func _add_remote(p_id: int) -> void:
	var node := Node3D.new()
	node.name = "RemotePlayer%d" % p_id
	var mesh := MeshInstance3D.new()
	mesh.name = "Body"
	mesh.mesh = CapsuleMesh.new()
	var material := StandardMaterial3D.new()
	# Unshaded: the 3D view has no lights (the terrain shader lights itself).
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.albedo_color = Color.from_hsv(fposmod(p_id * 0.618, 1.0), 0.6, 0.95)
	mesh.material_override = material
	node.add_child(mesh)
	var eye := MeshInstance3D.new()
	eye.name = "Facing"  # a nose, so you can see which way they look
	eye.mesh = BoxMesh.new()
	eye.material_override = material
	node.add_child(eye)
	var label := Label3D.new()
	label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	label.no_depth_test = true
	label.fixed_size = true
	label.pixel_size = 0.004
	label.outline_size = 6
	node.add_child(label)
	world.camera.get_parent().add_child(node)
	var r := {"node": node, "mesh": mesh, "eye": eye, "label": label, "target": Vector3.ZERO, "yaw": 0.0, "height": -1.0, "name": "", "placed": false}
	remotes[p_id] = r

func _shape_remote(p_r: Dictionary) -> void:
	var h: float = p_r.height
	var radius: float = h * world.player_radius / world.player_height
	var capsule: CapsuleMesh = p_r.mesh.mesh
	capsule.height = h
	capsule.radius = radius
	p_r.mesh.position = Vector3.UP * h * 0.5
	var box: BoxMesh = p_r.eye.mesh
	box.size = Vector3.ONE * radius * 0.6
	p_r.eye.position = Vector3(0, h * 0.85, -radius)
	p_r.label.position = Vector3.UP * (h + radius)
	p_r.label.text = p_r.name

func _remove_remote(p_id: int) -> void:
	if remotes.has(p_id):
		remotes[p_id].node.queue_free()
		remotes.erase(p_id)


# --- Every frame -------------------------------------------------------------------

func _process(delta: float) -> void:
	if mode == Mode.OFFLINE or multiplayer.multiplayer_peer == null:
		return
	if mode == Mode.HOST:
		_send_chunks()
	if mode == Mode.CLIENT and not synced:
		world.info_label.text = status  # (world.gd isn't running yet)
		return
	edit_timer += delta
	if edit_timer >= EDIT_INTERVAL:
		edit_timer = 0.0
		_flush_edits()
	if not is_instance_valid(world.player):
		return
	send_timer += delta
	if send_timer >= SEND_INTERVAL and not multiplayer.get_peers().is_empty():
		send_timer = 0.0
		_player_state.rpc(_wrapped(world.player.global_position), world.yaw, world.player_height, player_name)
	# Glide towards the latest positions (updates come 20 times a second).
	var k := 1.0 - exp(-delta * 15.0)
	for r in remotes.values():
		var node: Node3D = r.node
		var target: Vector3 = _nearest_repeat(r.target)
		if not r.placed or node.global_position.distance_to(target) > 50.0:
			node.global_position = target  # first update, or a jump
			r.placed = true
		else:
			node.global_position = node.global_position.lerp(target, k)
		node.rotation.y = lerp_angle(node.rotation.y, r.yaw, k)

## A line for the HUD: who is here.
func hud_text() -> String:
	if mode == Mode.OFFLINE:
		return status
	var names := PackedStringArray([player_name + " (you)"])
	for r in remotes.values():
		names.append(r.name)
	return "%s   Players: %s   Edits sent %d, received %d" % [status, ", ".join(names), batches_sent, batches_received]


# --- Coordinates ------------------------------------------------------------------

## This computer's position as wrapped world coordinates.
func _wrapped(p_position: Vector3) -> Vector3:
	var size := float(world.terrain.get_world_size())
	return Vector3(fposmod(p_position.x, size), p_position.y, fposmod(p_position.z, size))

## The repeat of a wrapped position nearest this computer's player.
func _nearest_repeat(p_wrapped: Vector3) -> Vector3:
	var size := float(world.terrain.get_world_size())
	var here: Vector3 = world.player.global_position
	var p := p_wrapped
	p.x += roundf((here.x - p.x) / size) * size
	p.z += roundf((here.z - p.z) / size) * size
	return p
