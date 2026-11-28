extends RefCounted
## The world's features (feature.gd: trees, mushrooms...), given to the
## terrain's generator before the world opens (world.gd). The generator
## grows them as it makes the ground, the same way as the rest of the
## world: nothing to place each frame, nothing saved unless edited.
##
## To add a feature: a script extending procedural_feature.gd or
## handmade_feature.gd (see procedural/tree.gd, handmade/mushroom.gd), added
## to FEATURES. Earlier entries get first pick of a region's ground (later
## ones keep their spacing from them).

const FEATURES: Array[GDScript] = [
	preload("res://scripts/features/procedural/tree.gd"),
	preload("res://scripts/features/handmade/mushroom.gd"),
]
## Whether the generator grows them at all (off: bare ground, no trees or
## mushrooms; ones already in saved, edited ground stay).
const GENERATE := false


## Builds every feature's shapes and gives them to p_terrain (before
## open_world()).
static func register(p_terrain: SculptTerrain) -> void:
	var start := Time.get_ticks_msec()
	var list := []
	if not GENERATE:
		p_terrain.set_features(list)
		return
	for script in FEATURES:
		var feature = script.new()
		var entry: Dictionary = feature.to_dict()
		if entry.shapes.is_empty():
			push_warning("Feature %s has no shapes: left out" % feature.id)
			continue
		list.append(entry)
	p_terrain.set_features(list)
	print("Features ready in %d ms" % (Time.get_ticks_msec() - start))
