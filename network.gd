extends Node
## Multiplayer, steps 1 and 2: a shared world, each other's players, and
## shared sculpting.
##
## Start the game with arguments after "--":
##   godot-4 --path . -- --host [--port 24565] [--name Sam]
##   godot-4 --path . -- --join 192.168.1.20[:24565] [--name Alex] [--backup-port 24566]
## Without them it is single-player, as before. --backup-port: where this
## computer hosts if it has to take over (two players on one computer need
## different ones).
##
## The host plays its own saved world. A player who joins gets the host's
## generator settings (so their computer generates the same ground) and the
## host's edited chunks around where they start, then plays unsaved: the
## world is the host's. Edited chunks farther off follow as they come near
## (see JOIN_RADIUS, STREAM_RADIUS). Each
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
##
## Physics objects (step 3): every ball, frog and object has an owner, the
## computer that simulates it (frogs: and runs their AI); the others hold a
## frozen copy that follows the owner's updates (its velocity set too, so
## their own objects bounce off it properly). Whoever makes an object owns
## it, so each player simulates what they throw or drop and the work is
## spread out; using one (physics gun, sculpting it, the object editor) takes
## it over. The host keeps the order of hand-overs, and takes the objects of
## a player who leaves. New and removed objects are found by looking (every
## SCAN_INTERVAL), so every way of making or removing one is covered. Other
## players are solid: frozen capsules that objects bounce off. Online, every
## object stays loaded (a frozen copy costs almost nothing).
##
## Spreading the load (step 4): the world's x / z plane is split into
## REGION-voxel squares, and the host (the directory) gives each region with
## objects in it to a player: at first the nearest one. Every REBALANCE_INTERVAL
## it compares each computer's load (moving objects, frogs counting extra)
## with how fast that computer is (a short benchmark of the native code each
## runs once; and a computer whose physics nears its frame budget counts as
## more loaded) and, past IMBALANCE, moves the overloaded computer's region
## nearest the least loaded one's player over to it. Objects go to the owner
## of the region they are in (so things near each other are simulated
## together), except while someone is using one (physics gun, sculpting,
## object editor: a short lease, renewed every frame by use()). Owners send
## fast updates only to players within INTEREST_RADIUS of an object; the rest
## get one a second.
##
## Step 5. Undo: SculptTerrain's undo puts back only points still as your
## stroke left them, so undoing never wipes out others' edits since. When
## the host leaves: everyone has every object and the world around where
## they've been (edited chunks they never came near aren't sent, so a new
## host doesn't have those), so the first player in the host's succession list (join order, with the address
## and --backup-port the host saw) starts hosting, and the rest reconnect to
## it (or the next, if it can't be reached in RECONNECT_TIMEOUT). The new host
## takes over every object and saves the world to user://shared/<world id>
## (never over its own single-player world); the others catch up from it as
## if joining. Edits made meanwhile are kept and sent once reconnected.
##
## Terrain edits stay in the host's one order: every computer applies every
## edit anyway (each keeps the whole world), so ordering them per region
## would spread no work, only add ways for copies to differ.
##
## Positions go over the network in wrapped world coordinates (x and z in
## [0, world_size)): world.gd moves its coordinates by whole world sizes to
## keep the player near 0 (_wrap_player()), differently on each computer.
## Each receiver puts a remote player at the repeat nearest its own.

const BodyAnimation := preload("res://body_animation.gd")

signal settings_received  ## joining: open the world with host_settings, then call world_opened()
signal joined  ## joining: every edited chunk has arrived and been applied
signal join_failed

const DEFAULT_PORT := 24565
const MAX_PLAYERS := 8
const SEND_INTERVAL := 0.05  # seconds between player updates
const CHUNKS_PER_FRAME := 8  # sent to a joining player each frame
const JOIN_OFFSET := Vector3(12, 0, 12)  # where a joining player starts, from the host's player
const EDIT_INTERVAL := 0.1  # seconds between batches of terrain changes
const RECONNECT_TIMEOUT := 10.0  # seconds to reach a new host before trying the next

enum Mode { OFFLINE, HOST, CLIENT }

var world: Node  # world.gd
var mode := Mode.OFFLINE
var address := ""
var port := DEFAULT_PORT
var player_name := ""
var status := ""
var backup_port := DEFAULT_PORT
## This computer, for the succession list and object ids (peer ids start over
## when a new host takes over; this doesn't).
var token := "%08x%04x" % [randi(), Time.get_ticks_usec() & 0xffff]
## The shared world's name (the host's save folder and seed): a new host saves to user://shared/<world_id>.
var world_id := ""
## Host: who takes over if it leaves, in order ({id, token, address, port,
## name}); everyone gets a copy.
var roster: Array = []
var candidates: Array = []  # still to try, after the host left
var resyncing := false  # reconnected to a new host: catching up with its world
var connect_deadline := 0.0

## Joining: the host's settings and how many chunks are still to come.
var host_settings: Dictionary = {}
var host_spawn := Vector3.ZERO
var chunks_expected := -1
var chunks_received := 0
var synced := false
var early: Array[Callable] = []  # world data that came before the world was open, in order

## Host: edited chunks not sent to each player yet. A joining player gets
## only the ones around where they start (JOIN_RADIUS) before they play;
## after that, the ones within STREAM_RADIUS of wherever they are go out
## nearest first, and the rest only once they come near. (Unedited ground
## needs nothing sent: every computer generates the same.)
const JOIN_RADIUS := 384.0  # voxels: more than the reach of the tools and the physics around the player
const STREAM_RADIUS := 960.0  # voxels: beyond the view distance (872), so edits are there before they're seen
const STREAM_CHECK := 0.5  # seconds between looking for chunks that came into range
var outgoing: Dictionary = {}  # peer id -> Array[Vector3i], still out of range
var due: Dictionary = {}  # peer id -> Array[Vector3i], in range, nearest last (sent first)
var join_spot: Dictionary = {}  # peer id -> where they start (wrapped), until they say where they are
var stream_timer := 0.0

## Sculpting: this computer's batches the host hasn't sent back yet
## ([id, changes]; joining players only), and counts for the HUD.
var unconfirmed: Array = []
var next_batch_id := 0
var edit_timer := 0.0
var batches_sent := 0
var batches_received := 0

## Physics objects: net id ("peer:n") -> DensityBody3D, ours and copies.
## Each body's metadata: net_id, net_owner (peer id), net_sim (simulated
## here), net_frozen (a copy: the owner's freeze), net_target / net_turn
## (a copy: where the owner last had it), net_shape (the shape and version
## last sent).
const BODY_INTERVAL := 0.05  # seconds between updates of moving objects
const REST_INTERVAL := 1.0  # ... and of ones at rest
const SCAN_INTERVAL := 0.1
const SHAPE_INTERVAL := 0.25
const BODY_PROPERTIES := ["mass", "bounce", "friction", "gravity_scale", "linear_damp", "angular_damp", "rolling_resistance",
		"auto_material_properties", "auto_center_of_mass", "center_of_mass", "collide_with_bodies", "drawn", "color", "material"]
var bodies: Dictionary = {}
var next_body := 0
var body_timer := 0.0
var scan_timer := 0.0
var shape_timer := 0.0
var ready_for_bodies := false
var early_bodies: Array[Callable] = []
## Animations: each AnimationLibrary (a body's own, or one a kind of
## creature shares) -> the hash of what it held when last sent or received.
## A change made here goes to everyone (with one of its bodies' ids), and
## a received one is put in the copy's library, so all of that kind get it.
var library_hashes: Dictionary = {}

## Regions (step 4). The host keeps region_owners (region -> peer id) and
## each peer's speed; everyone gets the region counts for the HUD.
const REGION := 256  # voxels a side
const REBALANCE_INTERVAL := 2.0
const RECONCILE_INTERVAL := 0.5
const REPORT_INTERVAL := 2.0
const IMBALANCE := 1.2  # move a region when one computer has 20% more load than another (for its speed)
const LEASE := 1.0  # seconds an object stays with whoever last used it
const INTEREST_RADIUS := 512.0  # players nearer an object than this get its fast updates
const FROG_WEIGHT := 2.0  # a frog (AI) counts as this many balls
var region_owners: Dictionary = {}  # Vector2i -> peer id (host)
var region_loads: Dictionary = {}  # Vector2i -> load (host, last rebalance)
var peer_speed: Dictionary = {}  # peer id -> benchmark score (host)
var peer_strain: Dictionary = {}  # peer id -> >= 1: how far its physics time is over BUSY of its budget (host)
var speed_score := 0.0  # this computer's benchmark (see _benchmark())
const BUSY := 0.6  # of the physics frame budget
const STATES_PER_PACKET := 12  # (~100 bytes each; unreliable packets over ~1400 bytes get lost more)
var regions_mine := 0  # (everyone, from the host) for the HUD
var regions_total := 0
var load_mine := 0.0
var rebalance_timer := 0.0
var reconcile_timer := 0.0
var report_timer := 0.0
var regions_moved := 0

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
			"--backup-port":
				backup_port = int(value)
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
	_connect_once(multiplayer.peer_connected, _on_peer_connected)
	_connect_once(multiplayer.peer_disconnected, _on_peer_disconnected)
	world.terrain.set_change_tracking(true)
	if world_id.is_empty():
		var folder: String = world.terrain.save_path.get_file() if not world.terrain.save_path.is_empty() else "world"
		world_id = "%s-%d" % [folder, world.terrain.get_world_settings().seed]
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
	_connect_signals_as_client()
	status = "Connecting to %s:%d..." % [address, port]
	print("Network: ", status)

func _connect_signals_as_client() -> void:
	_connect_once(multiplayer.connected_to_server, _on_connected_to_server)
	_connect_once(multiplayer.connection_failed, _on_connection_failed)
	_connect_once(multiplayer.server_disconnected, _on_server_disconnected)
	_connect_once(multiplayer.peer_disconnected, _on_peer_disconnected)

func _connect_once(p_signal: Signal, p_callable: Callable) -> void:
	if not p_signal.is_connected(p_callable):
		p_signal.connect(p_callable)

func _on_connected_to_server() -> void:
	status = "Connected, waiting for the world..."
	print("Network: connected to ", address)

func _on_connection_failed() -> void:
	if synced:
		_try_next_candidate()  # (a new host after the old one left)
	else:
		_fail("Couldn't reach %s:%d" % [address, port])

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
	var spawn := _wrapped(world.player.global_position)
	outgoing[p_id] = chunks
	due[p_id] = []
	join_spot[p_id] = _wrapped(spawn + JOIN_OFFSET)
	_refresh_due(p_id)
	# What they wait for: the chunks near where they start (the nearest, so
	# the first sent).
	var needed := 0
	for c in due[p_id]:
		if _chunk_distance(c, join_spot[p_id]) <= JOIN_RADIUS:
			needed += 1
	print("Network: player %d joined, sending %d of %d edited chunks now (the rest as they come near)" % [p_id, needed, chunks.size()])
	_welcome.rpc_id(p_id, world.terrain.get_world_settings(), spawn, needed, world_id)
	# Every object, with its owner (they appear once the joiner's world is ready).
	for id in bodies:
		var b = bodies[id]
		if is_instance_valid(b):
			_spawn.rpc_id(p_id, id, b.get_meta("net_owner", 1), _describe(b))
	# Where everyone already is, so the new player sees them at once.
	_player_state.rpc_id(p_id, _wrapped(world.player.global_position), world.yaw, world.player_height, player_name)

func _send_chunks(p_delta: float) -> void:
	stream_timer += p_delta
	var check := stream_timer >= STREAM_CHECK
	if check:
		stream_timer = 0.0
	var peers := multiplayer.get_peers()
	for id in outgoing.keys():
		if not peers.has(id):
			_forget_peer_chunks(id)
			continue
		if check:
			_refresh_due(id)
		var list: Array = due[id]
		var coords: Array = []
		var data: Array = []
		while not list.is_empty() and coords.size() < CHUNKS_PER_FRAME:
			var c: Vector3i = list.pop_back()
			coords.append(c)
			data.append(world.terrain.get_chunk_data(c))  # (as it is now: later edits follow in order)
		if not coords.is_empty():
			_chunks.rpc_id(id, coords, data)
		if list.is_empty() and (outgoing[id] as Array).is_empty():
			_forget_peer_chunks(id)

func _forget_peer_chunks(p_id: int) -> void:
	outgoing.erase(p_id)
	due.erase(p_id)
	join_spot.erase(p_id)

## Moves a player's chunks that are now within STREAM_RADIUS of them into
## their queue, nearest sent first.
func _refresh_due(p_id: int) -> void:
	var where := _peer_spot(p_id)
	var waiting: Array = outgoing[p_id]
	var list: Array = due[p_id]
	var still: Array = []
	for c in waiting:
		if _chunk_distance(c, where) <= STREAM_RADIUS:
			list.append(c)
		else:
			still.append(c)
	outgoing[p_id] = still
	if not list.is_empty():
		list.sort_custom(func(a: Vector3i, b: Vector3i) -> bool: return _chunk_distance(a, where) > _chunk_distance(b, where))

## Where a player is (wrapped), or will start.
func _peer_spot(p_id: int) -> Vector3:
	var r = remotes.get(p_id)
	if r != null and r.placed:
		return r.target
	return join_spot.get(p_id, _wrapped(world.player.global_position))

## How far a chunk's middle is from a wrapped position, the short way round.
func _chunk_distance(p_chunk: Vector3i, p_wrapped: Vector3) -> float:
	var size := float(world.terrain.get_world_size())
	var d := Vector3(p_chunk) * 32.0 + Vector3.ONE * 16.0 - p_wrapped
	d.x = fposmod(d.x + size * 0.5, size) - size * 0.5
	d.z = fposmod(d.z + size * 0.5, size) - size * 0.5
	return d.length()


# --- Joining: receiving the world ---------------------------------------------------

@rpc("authority", "call_remote", "reliable")
func _welcome(p_settings: Dictionary, p_host_position: Vector3, p_chunk_count: int, p_world_id: String) -> void:
	world_id = p_world_id
	chunks_expected = p_chunk_count
	_hello.rpc_id(1, player_name, backup_port, token)
	if resyncing:
		# A new host, after the old one left: its world replaces ours.
		status = "Catching up with the new host: %d chunks" % p_chunk_count
		print("Network: ", status)
		_check_resynced()
		return
	host_settings = p_settings
	host_spawn = p_host_position
	status = "Receiving the world..."
	print("Network: world settings received, %d edited chunks around the start to come" % p_chunk_count)
	settings_received.emit()

@rpc("authority", "call_remote", "reliable", 1)
func _chunks(p_coords: Array, p_data: Array) -> void:
	if not world.terrain.is_world_open():
		early.append(_apply_chunks.bind(p_coords, p_data))  # (the settings come on another channel)
		return
	_apply_chunks(p_coords, p_data)

func _apply_chunks(p_coords: Array, p_data: Array) -> void:
	# Chunks keep coming while we play (as we come near them): our own edits
	# there that the host hasn't ordered yet aren't in its copy, so they go
	# back on top (as under someone else's batch, see _apply_edits()).
	var ours: bool = synced and world.terrain.is_change_tracking()
	if ours:
		_flush_edits()
	var arrived := {}
	for i in p_coords.size():
		world.terrain.set_chunk_data(p_coords[i], p_data[i])
		arrived[p_coords[i]] = true
	if ours:
		for batch in unconfirmed:
			var changes: Array = batch[1]
			for j in range(0, changes.size() - 1, 2):
				if arrived.has(_wrap_chunk(changes[j])):
					world.terrain.apply_change(changes[j], changes[j + 1], false)
	chunks_received += p_coords.size()
	if resyncing:
		_check_resynced()
		return
	if not synced:
		status = "Receiving the world around you: %d / %d chunks" % [chunks_received, chunks_expected]
	_check_synced()

func _wrap_chunk(p_chunk: Vector3i) -> Vector3i:
	var n: int = world.terrain.get_world_size() / 32
	return Vector3i(posmod(p_chunk.x, n), p_chunk.y, posmod(p_chunk.z, n))

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


func _connected() -> bool:
	return multiplayer.multiplayer_peer != null and \
			multiplayer.multiplayer_peer.get_connection_status() == MultiplayerPeer.CONNECTION_CONNECTED


# --- When the host leaves (step 5) --------------------------------------------------

## Each player, on joining a host: where it would host if it had to take over.
@rpc("any_peer", "call_remote", "reliable")
func _hello(p_name: String, p_port: int, p_token: String) -> void:
	if not multiplayer.is_server():
		return
	var id := multiplayer.get_remote_sender_id()
	var enet := multiplayer.multiplayer_peer as ENetMultiplayerPeer
	var where := enet.get_peer(id).get_remote_address() if enet != null and enet.get_peer(id) != null else "127.0.0.1"
	roster = roster.filter(func(e: Dictionary) -> bool: return e.token != p_token)
	roster.append({"id": id, "token": p_token, "address": where, "port": p_port, "name": p_name})
	_roster.rpc(roster)

@rpc("authority", "call_remote", "reliable")
func _roster(p_roster: Array) -> void:
	roster = p_roster

func _on_server_disconnected() -> void:
	if not synced:
		_fail("The host left before the world arrived")
		return
	print("Network: the host left")
	for id in remotes.keys():
		_remove_remote(id)
	multiplayer.multiplayer_peer = null
	_flush_edits()  # (kept: there's no host to send them to)
	resyncing = true
	chunks_received = 0
	chunks_expected = -1
	candidates = roster.duplicate()
	_try_next_candidate()

## The next player in the succession list: us (we host), or someone to
## reconnect to; nobody left: play on alone.
func _try_next_candidate() -> void:
	multiplayer.multiplayer_peer = null
	while not candidates.is_empty():
		var c: Dictionary = candidates.pop_front()
		if c.token == token:
			_become_host()
			return
		address = c.address
		port = c.port
		var peer := ENetMultiplayerPeer.new()
		if peer.create_client(address, port) != OK:
			continue
		multiplayer.multiplayer_peer = peer
		_connect_signals_as_client()
		connect_deadline = _now() + RECONNECT_TIMEOUT
		status = "The host left: reconnecting to %s (%s:%d)..." % [c.name, address, port]
		print("Network: ", status)
		return
	_go_offline("The host left: playing on alone (unsaved)")

func _become_host() -> void:
	mode = Mode.HOST
	resyncing = false
	port = backup_port
	start_host()
	if mode != Mode.HOST:
		_go_offline("The host left, and hosting here failed (port %d): playing on alone" % port)
		return
	# Our edits are the world now; every object is simulated here until the
	# regions are given out again.
	unconfirmed.clear()
	for id in bodies:
		var b = bodies[id]
		if is_instance_valid(b):
			b.set_meta("net_owner", 1)
			_simulate_here(b, true)
	region_owners.clear()
	peer_speed.clear()
	peer_strain.clear()
	roster.clear()
	var saved := ""
	if world.terrain.save_path.is_empty() and not world_id.is_empty():
		var path := "user://shared".path_join(world_id)
		if world.terrain.save_world_as(path):
			saved = ", saving to " + ProjectSettings.globalize_path(path)
	status = "The host left: you are the host now (port %d%s)" % [port, saved]
	print("Network: ", status)

## Caught up with a new host: our kept edits go back on top, and to it.
func _check_resynced() -> void:
	if not resyncing or chunks_expected < 0 or chunks_received < chunks_expected:
		return
	resyncing = false
	_flush_edits()
	var kept := unconfirmed
	unconfirmed = []
	for batch in kept:
		_apply_changes(batch[1], false)
		next_batch_id += 1
		unconfirmed.append([next_batch_id, batch[1]])
		_submit_edits.rpc_id(1, next_batch_id, batch[1])
	status = "Rejoined: %s is hosting" % address
	print("Network: ", status, " (%d kept edit batches sent)" % kept.size())

func _go_offline(p_status: String) -> void:
	status = p_status
	print("Network: ", status)
	resyncing = false
	world.terrain.set_change_tracking(false)
	unconfirmed.clear()
	for id in remotes.keys():
		_remove_remote(id)
	for b in bodies.values():  # everything is ours now
		if is_instance_valid(b):
			_simulate_here(b, true)
			for key in ["net_id", "net_owner", "net_sim", "net_frozen", "net_target", "net_turn", "net_shape", "net_sent"]:
				b.remove_meta(key)
	bodies.clear()
	multiplayer.multiplayer_peer = null
	mode = Mode.OFFLINE


# --- Sculpting ---------------------------------------------------------------------------

## Sends what this computer's edits changed since last time (to the host, or
## from the host straight to everyone). Without a host (it left; catching up
## with a new one) they're kept, to send once back (_rejoined()).
func _flush_edits() -> void:
	if not world.terrain.is_change_tracking():
		return
	if mode == Mode.CLIENT and (resyncing or not _connected()):
		var kept: Array = world.terrain.take_changes()
		if not kept.is_empty():
			unconfirmed.append([0, kept])  # (0: not sent yet)
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
		_apply_changes(batch[1], false)
	batches_received += 1

## p_remote: another player's changes (false: our own, put back on top).
func _apply_changes(p_changes: Array, p_remote := true) -> void:
	for i in range(0, p_changes.size() - 1, 2):
		world.terrain.apply_change(p_changes[i], p_changes[i + 1], p_remote)


# --- Physics objects ---------------------------------------------------------------

## world.gd: the player, the balls and the rest exist now.
func world_ready() -> void:
	ready_for_bodies = true
	if mode == Mode.HOST:
		world.balls.load_all(world.player.global_position)  # online, everything stays loaded
	for call in early_bodies:
		call.call()
	early_bodies.clear()

## Using an object (the physics gun, sculpting it, the object editor): call
## every frame while in use. It's simulated here from now on, and isn't
## handed to its region's owner until LEASE seconds after the last call.
func use(p_body: DensityBody3D) -> void:
	if mode == Mode.OFFLINE or not is_instance_valid(p_body) or not p_body.has_meta("net_id"):
		return
	p_body.set_meta("net_lease", _now())
	take(p_body)

## Simulate an object here from now on: this computer takes it over at once,
## and the host tells everyone (if two players grab it at once, the host's
## order decides).
func take(p_body: DensityBody3D) -> void:
	if mode == Mode.OFFLINE or not p_body.has_meta("net_id"):
		return
	var me := multiplayer.get_unique_id()
	if p_body.get_meta("net_owner", 0) == me:
		return
	var id: String = p_body.get_meta("net_id")
	p_body.set_meta("net_owner", me)
	_simulate_here(p_body, true)
	if mode == Mode.HOST:
		_set_owner.rpc(id, me)
	else:
		_request_owner.rpc_id(1, id)

@rpc("any_peer", "call_remote", "reliable", 1)
func _request_owner(p_id: String) -> void:
	if not multiplayer.is_server() or not bodies.has(p_id):
		return
	var sender := multiplayer.get_remote_sender_id()
	_set_owner_now(p_id, sender)
	_set_owner.rpc(p_id, sender)

@rpc("authority", "call_remote", "reliable", 1)
func _set_owner(p_id: String, p_owner: int) -> void:
	if not ready_for_bodies:
		early_bodies.append(_set_owner_now.bind(p_id, p_owner))
		return
	_set_owner_now(p_id, p_owner)

func _set_owner_now(p_id: String, p_owner: int) -> void:
	var b = bodies.get(p_id)
	if b == null or not is_instance_valid(b):
		return
	b.set_meta("net_owner", p_owner)
	_simulate_here(b, p_owner == multiplayer.get_unique_id())

## Switches an object between simulated here and a frozen copy of another
## player's (its own freeze kept in net_frozen meanwhile; a frog's AI and
## animation only run where it's simulated).
func _simulate_here(p_body: DensityBody3D, p_on: bool) -> void:
	if p_body.has_meta("net_sim") and p_body.get_meta("net_sim") == p_on:
		return
	if p_on:
		p_body.freeze = p_body.get_meta("net_frozen", false)
		p_body.sleeping = false
	else:
		p_body.set_meta("net_frozen", p_body.freeze)
		p_body.freeze = true
		p_body.set_meta("net_target", _wrapped(p_body.global_position))
		p_body.set_meta("net_turn", p_body.global_basis.get_rotation_quaternion())
	p_body.set_meta("net_sim", p_on)
	if p_body.get_script() != null:
		p_body.set_physics_process(p_on)
	for child in p_body.get_children():
		if child is AnimationMixer:  # (trees and players: a copy's pose is the owner's)
			child.active = p_on

## Everything another computer needs to make a copy of an object.
func _describe(p_body: DensityBody3D) -> Dictionary:
	var props := {}
	for key in BODY_PROPERTIES:
		props[key] = p_body.get(key)
	var frozen: bool = p_body.freeze if p_body.get_meta("net_sim", true) else p_body.get_meta("net_frozen", false)
	return {"scene": p_body.scene_file_path, "shape": p_body.shape.to_dict(), "props": props,
			"position": _wrapped(p_body.global_position), "turn": p_body.global_basis.get_rotation_quaternion(),
			"velocity": p_body.linear_velocity, "spin": p_body.angular_velocity, "frozen": frozen, "pose": _pose_of(p_body), "animation": BodyAnimation.to_dict(p_body)}

## A body's pose: its `pose`, or [pose, key weights] while it weighs keys in.
func _pose_of(p_body: DensityBody3D) -> Variant:
	var weights: PackedFloat32Array = p_body.key_weights
	for w in weights:
		if w != 0.0:
			return [p_body.pose, weights]
	return p_body.pose

func _set_pose(p_body: DensityBody3D, p_pose: Variant) -> void:
	if p_pose is Array:
		p_body.pose = p_pose[0]
		p_body.key_weights = p_pose[1]
	else:
		p_body.pose = p_pose
		if not p_body.key_weights.is_empty():
			p_body.key_weights = PackedFloat32Array()

## A copy's pose a step towards the owner's (key weights eased, so an
## animation 20 updates a second still plays smoothly).
func _glide_pose(p_body: DensityBody3D, p_pose: Variant, p_k: float) -> void:
	var target_pose: float = p_pose[0] if p_pose is Array else p_pose
	var target: PackedFloat32Array = p_pose[1] if p_pose is Array else PackedFloat32Array()
	var k := minf(p_k * 1.6, 1.0)
	if absf(p_body.pose - target_pose) > 1e-4:
		p_body.pose = target_pose if absf(p_body.pose - target_pose) > 2.0 else lerpf(p_body.pose, target_pose, k)
	var weights: PackedFloat32Array = p_body.key_weights
	var n := maxi(weights.size(), target.size())
	var changed := false
	var glided := PackedFloat32Array()
	glided.resize(n)
	for i in n:
		var have := weights[i] if i < weights.size() else 0.0
		var want := target[i] if i < target.size() else 0.0
		var v := want if absf(want - have) < 1e-3 else lerpf(have, want, k)
		glided[i] = v
		changed = changed or v != have
	if changed:
		p_body.key_weights = glided

## Finds objects made here (thrown, dropped, P, imported, cut off) and gives
## them ids, and objects removed here (X, broken, fallen out), and tells
## everyone. Previews and the floating piece aren't pickable: left out.
func _scan_bodies() -> void:
	for id in bodies.keys():
		var b = bodies[id]
		if not is_instance_valid(b) or not b.is_inside_tree() or b.is_queued_for_deletion():
			bodies.erase(id)
			_despawn.rpc(id)
	var me := multiplayer.get_unique_id()
	for parent in [world.balls, world.camera.get_parent()]:
		for b in parent.get_children():
			if b is DensityBody3D and not b is DensityCharacter3D and b.pickable and b.shape != null \
					and not b.has_meta("net_id") and not b.has_meta("net_avatar") and not b.is_queued_for_deletion():
				next_body += 1
				var id := "%s:%d" % [token, next_body]
				b.set_meta("net_id", id)
				b.set_meta("net_owner", me)
				b.set_meta("net_sim", true)
				b.set_meta("net_shape", [b.shape, b.shape.get_version()])
				bodies[id] = b
				_spawn.rpc(id, me, _describe(b))

@rpc("any_peer", "call_remote", "reliable", 1)
func _spawn(p_id: String, p_owner: int, p_desc: Dictionary) -> void:
	if not ready_for_bodies:
		early_bodies.append(_spawn_now.bind(p_id, p_owner, p_desc))
		return
	_spawn_now(p_id, p_owner, p_desc)

func _spawn_now(p_id: String, p_owner: int, p_desc: Dictionary) -> void:
	if bodies.has(p_id):
		_set_owner_now(p_id, p_owner)  # (a new host telling us who has it now)
		return
	var shape := DensityShape.from_dict(p_desc.shape)
	var b: DensityBody3D
	if p_desc.scene != "":
		b = load(p_desc.scene).instantiate()
		world.camera.get_parent().add_child(b)
		b.shape = shape
	else:
		b = world.balls.add_shape_body(shape, Vector3.ZERO)
	for key in p_desc.props:
		b.set(key, p_desc.props[key])
	b.set_meta("net_id", p_id)
	b.set_meta("net_owner", p_owner)
	b.set_meta("net_sim", true)
	b.set_meta("net_shape", [b.shape, b.shape.get_version()])
	bodies[p_id] = b
	b.global_transform = Transform3D(Basis(p_desc.turn), _nearest_repeat(p_desc.position))
	b.linear_velocity = p_desc.velocity
	b.angular_velocity = p_desc.spin
	_set_pose(b, p_desc.pose)
	BodyAnimation.apply(b, p_desc.get("animation", {}))
	var library := _library_of(b)
	if library != null:
		library_hashes[library] = _library_hash(library)
	b.freeze = p_desc.frozen
	_simulate_here(b, p_owner == multiplayer.get_unique_id())

@rpc("any_peer", "call_remote", "reliable", 1)
func _despawn(p_id: String) -> void:
	if not ready_for_bodies:
		early_bodies.append(_despawn.bind(p_id))
		return
	var b = bodies.get(p_id)
	bodies.erase(p_id)
	if b != null and is_instance_valid(b):
		world.balls.remove(b)

## Where the objects simulated here are, and their shapes when sculpted.
## Moving ones: every BODY_INTERVAL to players within INTEREST_RADIUS, every
## REST_INTERVAL to the rest; ones at rest: every REST_INTERVAL to everyone.
## A state: id, wrapped position, turn, velocity, spin, pose (see _pose_of()), flags (1 frozen,
## 2 sleeping, 4 in use).
func _send_bodies() -> void:
	var now := _now()
	var send_shapes := shape_timer >= SHAPE_INTERVAL
	if send_shapes:
		shape_timer = 0.0
	var peers := multiplayer.get_peers()
	var checked := {}  # libraries looked at this time
	var batches := {}  # peer id -> Array
	for peer in peers:
		batches[peer] = []
	for id in bodies:
		var b = bodies[id]  # (untyped: it may have been freed since the last scan)
		if not is_instance_valid(b) or not b.get_meta("net_sim", false):
			continue
		if send_shapes:
			var seen: Array = b.get_meta("net_shape", [null, -1])
			if seen[0] != b.shape or seen[1] != b.shape.get_version():
				b.set_meta("net_shape", [b.shape, b.shape.get_version()])
				_shape.rpc(id, b.shape.to_dict())
			var library := _library_of(b)
			if library != null and not checked.has(library):
				checked[library] = true
				var hash := _library_hash(library)
				if library_hashes.get(library, -1) != hash:
					library_hashes[library] = hash
					_animations.rpc(id, BodyAnimation.to_dict(b))
		var pose = _pose_of(b)
		var sent = b.get_meta("net_pose_sent") if b.has_meta("net_pose_sent") else null
		var posing: bool = typeof(sent) != typeof(pose) or sent != pose  # (animating: sent as often as a moving one)
		var resting: bool = (b.sleeping or b.freeze) and not posing
		var everyone: bool = now - float(b.get_meta("net_sent", -100.0)) >= REST_INTERVAL
		if resting and not everyone:
			continue
		if everyone:
			b.set_meta("net_sent", now)
		b.set_meta("net_pose_sent", pose)
		var flags := (1 if b.freeze else 0) | (2 if b.sleeping else 0) | (4 if _in_use(b) else 0)
		var state := [id, _wrapped(b.global_position), b.global_basis.get_rotation_quaternion(),
				b.linear_velocity, b.angular_velocity, pose, flags]
		for peer in peers:
			if everyone or _peer_near(peer, b.global_position):
				batches[peer].append_array(state)
	for peer in batches:
		var batch: Array = batches[peer]
		var size := 7 * STATES_PER_PACKET
		for from in range(0, batch.size(), size):
			_states.rpc_id(peer, batch.slice(from, from + size))

func _peer_near(p_peer: int, p_position: Vector3) -> bool:
	var r = remotes.get(p_peer)
	if r == null or not r.placed:
		return true  # (where they are isn't known yet)
	return r.node.global_position.distance_to(p_position) < INTEREST_RADIUS

func _in_use(p_body: Node) -> bool:
	if p_body.get_meta("net_sim", false):
		return _now() - float(p_body.get_meta("net_lease", -100.0)) < LEASE
	return p_body.get_meta("net_in_use", false)

func _now() -> float:
	return Time.get_ticks_msec() / 1000.0

@rpc("any_peer", "call_remote", "unreliable_ordered", 2)
func _states(p_batch: Array) -> void:
	if not ready_for_bodies:
		return
	for i in range(0, p_batch.size() - 6, 7):
		var b = bodies.get(p_batch[i])
		if b == null or not is_instance_valid(b) or b.get_meta("net_sim", false):
			continue  # unknown yet, or simulated here (a hand-over on its way)
		b.set_meta("net_target", p_batch[i + 1])
		b.set_meta("net_turn", p_batch[i + 2])
		b.linear_velocity = p_batch[i + 3]  # what objects here bounce off
		b.angular_velocity = p_batch[i + 4]  # (kept, for a hand-over)
		b.set_meta("net_pose", p_batch[i + 5])  # (glided to in _update_bodies())
		var flags: int = p_batch[i + 6]
		b.set_meta("net_frozen", flags & 1 != 0)
		b.set_meta("net_sleeping", flags & 2 != 0)
		b.set_meta("net_in_use", flags & 4 != 0)

@rpc("any_peer", "call_remote", "reliable", 1)
func _shape(p_id: String, p_shape: Dictionary) -> void:
	if not ready_for_bodies:
		early_bodies.append(_shape.bind(p_id, p_shape))
		return
	var b = bodies.get(p_id)
	if b == null or not is_instance_valid(b):
		return
	b.shape = DensityShape.from_dict(p_shape)
	b.set_meta("net_shape", [b.shape, b.shape.get_version()])

## A body's animations (its AnimationPlayer's library), or null.
func _library_of(p_body: Node) -> AnimationLibrary:
	var player := BodyAnimation.player_of(p_body)
	if player == null or not player.has_animation_library(""):
		return null
	return player.get_animation_library("")

func _library_hash(p_library: AnimationLibrary) -> int:
	var all := {}
	for anim_name in p_library.get_animation_list():
		all[anim_name] = BodyAnimation.animation_to_dict(p_library.get_animation(anim_name))
	return all.hash()

## Someone changed a body's animations (made, keyframed, imported): this
## copy's (and, a creature's, all of its kind's) are made the same.
@rpc("any_peer", "call_remote", "reliable", 1)
func _animations(p_id: String, p_animation: Dictionary) -> void:
	if not ready_for_bodies:
		early_bodies.append(_animations.bind(p_id, p_animation))
		return
	var b = bodies.get(p_id)
	if b == null or not is_instance_valid(b):
		return
	BodyAnimation.apply(b, p_animation)
	var library := _library_of(b)
	if library != null:
		library_hashes[library] = _library_hash(library)  # (not sent back)
	if not b.get_meta("net_sim", false):
		for child in b.get_children():
			if child is AnimationMixer:
				child.active = false  # (apply() may have woken them: a copy's pose is the owner's)

## A kind of creature's shape changed here (creatures.gd): everyone's
## creatures of that kind take it on.
func send_kind_shape(p_scene: String, p_shape: DensityShape) -> void:
	if mode == Mode.OFFLINE or not ready_for_bodies or multiplayer.get_peers().is_empty():
		return
	_kind_shape.rpc(p_scene, p_shape.to_dict())

@rpc("any_peer", "call_remote", "reliable", 1)
func _kind_shape(p_scene: String, p_shape: Dictionary) -> void:
	if not ready_for_bodies:
		early_bodies.append(_kind_shape.bind(p_scene, p_shape))
		return
	world.creatures.take_kind_shape(p_scene, DensityShape.from_dict(p_shape))

func _update_bodies(p_delta: float, p_k: float) -> void:
	scan_timer += p_delta
	if scan_timer >= SCAN_INTERVAL:
		scan_timer = 0.0
		_scan_bodies()
	shape_timer += p_delta
	body_timer += p_delta
	if body_timer >= BODY_INTERVAL:
		body_timer = 0.0
		_send_bodies()
	report_timer += p_delta
	if report_timer >= REPORT_INTERVAL:
		report_timer = 0.0
		if speed_score <= 0.0:
			speed_score = _benchmark()
		var physics_ms := Performance.get_monitor(Performance.TIME_PHYSICS_PROCESS) * 1000.0
		if multiplayer.is_server():
			_take_report(1, speed_score, physics_ms)
		else:
			_report.rpc_id(1, speed_score, physics_ms)
	if multiplayer.is_server():
		reconcile_timer += p_delta
		if reconcile_timer >= RECONCILE_INTERVAL:
			reconcile_timer = 0.0
			_reconcile()
		rebalance_timer += p_delta
		if rebalance_timer >= REBALANCE_INTERVAL:
			rebalance_timer = 0.0
			_rebalance()
	# Copies glide towards where their owners have them, and how they pose them.
	for b in bodies.values():
		if not is_instance_valid(b) or b.get_meta("net_sim", false):
			continue
		if b.has_meta("net_pose"):
			_glide_pose(b, b.get_meta("net_pose"), p_k)
		if not b.has_meta("net_target"):
			continue
		var target := _nearest_repeat(b.get_meta("net_target"))
		var turn: Quaternion = b.get_meta("net_turn")
		var here: Quaternion = b.global_basis.get_rotation_quaternion()
		if b.global_position.distance_squared_to(target) < 1e-4 and absf(here.dot(turn)) > 0.999999:
			continue  # there already (most copies, at rest): setting it again would only notify the tree every frame
		if b.global_position.distance_to(target) > 50.0:
			b.global_transform = Transform3D(Basis(turn), target)
		else:
			var q: Quaternion = b.global_basis.get_rotation_quaternion().slerp(turn, p_k)
			b.global_transform = Transform3D(Basis(q), b.global_position.lerp(target, p_k))


# --- Regions and load (step 4) -------------------------------------------------------

func _region_of(p_position: Vector3) -> Vector2i:
	var w := _wrapped(p_position)
	return Vector2i(floori(w.x / REGION), floori(w.z / REGION))

func _region_centre(p_region: Vector2i) -> Vector3:
	return Vector3((p_region.x + 0.5) * REGION, 0.0, (p_region.y + 0.5) * REGION)

## How much simulating an object costs, roughly: moving ones 1 (frogs more,
## for their AI), ones at rest almost nothing.
func _body_load(p_body: Node) -> float:
	var at_rest: bool
	if p_body.get_meta("net_sim", false):
		at_rest = p_body.sleeping or p_body.freeze
	else:
		at_rest = p_body.get_meta("net_sleeping", false) or p_body.get_meta("net_frozen", false)
	var weight := FROG_WEIGHT if p_body.scene_file_path != "" else 1.0
	return weight * (0.05 if at_rest else 1.0)

## How fast this computer runs the native code the physics uses: shapes
## made per millisecond (the best of a few tries, so a hiccup doesn't count).
func _benchmark() -> float:
	var best := 0.0
	for attempt in 5:
		var t0 := Time.get_ticks_usec()
		for i in 6:
			DensityShape.make_sphere(9.0, 2).get_volume()
		var ms := (Time.get_ticks_usec() - t0) / 1000.0
		best = maxf(best, 6.0 / maxf(ms, 0.001))
	print("Network: speed score %.2f" % best)
	return best

## Each computer, every REPORT_INTERVAL: its speed and its physics time.
@rpc("any_peer", "call_remote", "reliable", 1)
func _report(p_speed: float, p_physics_ms: float) -> void:
	if multiplayer.is_server():
		_take_report(multiplayer.get_remote_sender_id(), p_speed, p_physics_ms)

func _take_report(p_peer: int, p_speed: float, p_physics_ms: float) -> void:
	peer_speed[p_peer] = p_speed
	var budget_ms := 1000.0 / Engine.physics_ticks_per_second
	peer_strain[p_peer] = maxf(1.0, p_physics_ms / (budget_ms * BUSY))

## A peer's speed relative to the others (1 = average; unknown = average).
func _relative_speed(p_peer: int, p_peers: Array) -> float:
	var known := []
	for peer in p_peers:
		if peer_speed.get(peer, 0.0) > 0.0:
			known.append(peer_speed[peer])
	var speed: float = peer_speed.get(p_peer, 0.0)
	if known.is_empty() or speed <= 0.0:
		return 1.0
	var mean := 0.0
	for k in known:
		mean += k
	mean /= known.size()
	return clampf(speed / mean, 0.25, 4.0)

## Where each player is (wrapped), for giving out regions.
func _player_positions() -> Dictionary:
	var where := {multiplayer.get_unique_id(): _wrapped(world.player.global_position)}
	for id in remotes:
		var r: Dictionary = remotes[id]
		if r.placed:
			where[id] = _wrapped(r.node.global_position)
	return where

## The player nearest a region (the short way round the wrapping world).
func _nearest_player(p_region: Vector2i, p_where: Dictionary) -> int:
	var size := float(world.terrain.get_world_size())
	var centre := _region_centre(p_region)
	var best := 1
	var best_d := INF
	for peer in p_where:
		var d: Vector3 = p_where[peer] - centre
		d.x = fposmod(d.x + size * 0.5, size) - size * 0.5
		d.z = fposmod(d.z + size * 0.5, size) - size * 0.5
		d.y = 0.0
		if d.length() < best_d:
			best_d = d.length()
			best = peer
	return best

## Host: every object goes to its region's owner (a region nobody has yet:
## the nearest player), unless someone is using it.
func _reconcile() -> void:
	var where := _player_positions()
	var peers := [1] + Array(multiplayer.get_peers())
	for region in region_owners.keys():
		if not peers.has(region_owners[region]):
			region_owners.erase(region)  # (they left)
	for id in bodies:
		var b = bodies[id]
		if not is_instance_valid(b) or _in_use(b):
			continue
		var region := _region_of(b.global_position)
		if not region_owners.has(region):
			region_owners[region] = _nearest_player(region, where)
		var owner: int = region_owners[region]
		if b.get_meta("net_owner", 1) != owner:
			_set_owner_now(id, owner)
			_set_owner.rpc(id, owner)

## Host: regions with objects, and their load; then, if one computer has
## IMBALANCE times the load of another (each divided by its speed), its
## region nearest the less loaded player moves over (one per rebalance, so
## it settles gently).
func _rebalance() -> void:
	var peers := [1] + Array(multiplayer.get_peers())
	region_loads.clear()
	for b in bodies.values():
		if is_instance_valid(b):
			var region := _region_of(b.global_position)
			region_loads[region] = region_loads.get(region, 0.0) + _body_load(b)
	for region in region_owners.keys():
		if not region_loads.has(region):
			region_owners.erase(region)  # empty: given out again when something arrives
	var load_of := {}
	for peer in peers:
		load_of[peer] = 0.0
	for region in region_owners:
		var owner: int = region_owners[region]
		if load_of.has(owner):
			load_of[owner] += region_loads.get(region, 0.0)
	if peers.size() > 1:
		var relative := func(peer: int) -> float: return load_of[peer] / _relative_speed(peer, peers) * peer_strain.get(peer, 1.0)
		var over: int = peers[0]
		var under: int = peers[0]
		for peer in peers:
			if relative.call(peer) > relative.call(over):
				over = peer
			if relative.call(peer) < relative.call(under):
				under = peer
		var worst: float = relative.call(over)
		if over != under and worst > 0.5 and worst > relative.call(under) * IMBALANCE:
			var where := _player_positions()
			var target: Vector3 = where.get(under, Vector3.ZERO)
			var best = null
			var best_d := INF
			for region in region_owners:
				if region_owners[region] != over:
					continue
				var moved: float = region_loads.get(region, 0.0)
				var after := maxf((load_of[over] - moved) / _relative_speed(over, peers) * peer_strain.get(over, 1.0),
						(load_of[under] + moved) / _relative_speed(under, peers) * peer_strain.get(under, 1.0))
				if after >= worst - 0.01:
					continue  # moving this one wouldn't help
				var d := Vector2(_region_centre(region).x - target.x, _region_centre(region).z - target.z).length()
				if d < best_d:
					best_d = d
					best = region
			if best != null:
				region_owners[best] = under
				regions_moved += 1
				_reconcile()
	# Everyone's share, for the HUD.
	for peer in peers:
		var count := 0
		for region in region_owners:
			if region_owners[region] == peer:
				count += 1
		if peer == 1:
			_region_summary(count, region_owners.size(), load_of[peer])
		else:
			_region_summary.rpc_id(peer, count, region_owners.size(), load_of[peer])

@rpc("authority", "call_remote", "unreliable")
func _region_summary(p_mine: int, p_total: int, p_load: float) -> void:
	regions_mine = p_mine
	regions_total = p_total
	load_mine = p_load


# --- Players ---------------------------------------------------------------------------

func _on_peer_disconnected(p_id: int) -> void:
	print("Network: player %d left" % p_id)
	_remove_remote(p_id)
	_forget_peer_chunks(p_id)
	if multiplayer.is_server():
		roster = roster.filter(func(e: Dictionary) -> bool: return e.id != p_id)
		_roster.rpc(roster)
		# Their objects: simulated here from now on.
		for id in bodies.keys():
			var b = bodies[id]
			if is_instance_valid(b) and b.get_meta("net_owner", 0) == p_id:
				b.set_meta("net_owner", 1)
				_simulate_here(b, true)
				_set_owner.rpc(id, 1)

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
	# A frozen body (spheres up a capsule) so objects bounce off other
	# players; not pickable, so brushes and the physics gun ignore it.
	var node := DensityBody3D.new()
	node.freeze = true
	node.pickable = false
	node.drawn = false
	node.set_meta("net_avatar", p_id)
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
	# The 3D view is only 320x180 (scaled up): the defaults (32 px font,
	# 0.004) made a name about 22 of its 180 lines tall. This is about 7.
	label.font_size = 16
	label.outline_size = 4
	label.pixel_size = 0.0035
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
	var spheres := PackedVector4Array()
	var count := maxi(2, ceili(h / radius))
	for i in count:
		spheres.append(Vector4(0.0, lerpf(radius, h - radius, float(i) / (count - 1)), 0.0, radius))
	p_r.node.spheres = spheres

func _remove_remote(p_id: int) -> void:
	if remotes.has(p_id):
		remotes[p_id].node.queue_free()
		remotes.erase(p_id)


# --- Every frame -------------------------------------------------------------------

func _process(delta: float) -> void:
	if mode == Mode.OFFLINE or multiplayer.multiplayer_peer == null:
		return
	if mode == Mode.HOST:
		_send_chunks(delta)
	if mode == Mode.CLIENT and not synced:
		world.info_label.text = status  # (world.gd isn't running yet)
		return
	if mode == Mode.CLIENT and resyncing and not _connected() and _now() > connect_deadline:
		_try_next_candidate()  # (couldn't reach that one in time)
		return
	if not _connected():
		return  # (reconnecting: nothing can be sent yet)
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
		var node: DensityBody3D = r.node
		var target: Vector3 = _nearest_repeat(r.target)
		if not r.placed or node.global_position.distance_to(target) > 50.0:
			node.global_position = target  # first update, or a jump
			node.linear_velocity = Vector3.ZERO
			r.placed = true
		else:
			var before := node.global_position
			node.global_position = before.lerp(target, k)
			node.linear_velocity = (node.global_position - before) / maxf(delta, 1e-4)  # what objects bounce off
		node.rotation.y = lerp_angle(node.rotation.y, r.yaw, k)
	if ready_for_bodies:
		_update_bodies(delta, k)

## A line for the HUD: who is here.
func hud_text() -> String:
	if mode == Mode.OFFLINE:
		return status
	var names := PackedStringArray([player_name + " (you)"])
	for r in remotes.values():
		names.append(r.name)
	var here := 0
	for b in bodies.values():
		if is_instance_valid(b) and b.get_meta("net_sim", false):
			here += 1
	return "%s   Players: %s   Edits sent %d, received %d   Objects %d (%d simulated here)   Regions %d of %d, load %.1f" % [
			status, ", ".join(names), batches_sent, batches_received, bodies.size(), here, regions_mine, regions_total, load_mine]


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
