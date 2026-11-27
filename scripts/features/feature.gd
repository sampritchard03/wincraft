extends RefCounted
## A world feature: something the world grows on its ground (a tree, a
## mushroom...). Features are part of the generated world, like its noise:
## the terrain's generator (SculptTerrain.set_features()) puts them where
## the seed says, as it makes the ground, so they cost nothing until seen
## and are only saved where their chunks are edited. Once there, they're
## ground like any other: sculpt them, paint them, select them, carve them.
##
## Two kinds, each a script extending this one:
##   procedural_feature.gd  its shapes are built in code (a few variants),
##                          e.g. procedural/tree.gd
##   handmade_feature.gd    its shapes are .res files sculpted in the game
##                          and exported (Ctrl+P on a selection), e.g.
##                          handmade/mushroom.gd
## features.gd lists them all and gives them to the terrain.
##
## Every player must have the same features (they're part of the world's
## generation): changing one changes where the world's unedited ground has
## them.

## Its name: seeds where it goes (keep it once worlds have it).
var id := ""
## How many to try for in each 64 x 64 voxels of ground, on average. Tries
## that land somewhere it can't grow come to nothing.
var per_region := 1.0
## Blocks it grows on (SculptTerrain.get_material_names()); empty: any ground.
var grows_on := PackedStringArray(["grass"])
## Steepest ground it grows on, in degrees.
var max_slope := 30.0
## How far into the ground it's set, in voxels (times its size), so it
## stands in the ground rather than on the tip of it.
var sink := 1.0
## Leans with the ground (0: stands straight up; 1: square to the slope).
var follow_slope := 0.0
## Each one turned any way about its up.
var random_turn := true
## Each one's size, times its shape's: from x to y.
var scale_range := Vector2(1.0, 1.0)
## The world types it appears in (SculptTerrain.WORLD_ISLANDS, WORLD_TERRAIN); empty: all.
var world_types: Array[int] = []
## Kept at least this far (voxels) from other features of its region.
var spacing := 0.0


## Its shapes (one is picked for each placement): their local origin is
## where they meet the ground, +y up.
func get_shapes() -> Array[DensityShape]:
	return []


## What the terrain takes (SculptTerrain.set_features()).
func to_dict() -> Dictionary:
	var names := SculptTerrain.get_material_names()
	var blocks := PackedInt32Array()
	for block in grows_on:
		var m := names.find(block)
		if m >= 0:
			blocks.append(m)
		else:
			push_warning("Feature %s: no block called %s" % [id, block])
	return {
		"id": id,
		"shapes": get_shapes(),
		"per_region": per_region,
		"grows_on": blocks,
		"max_slope": max_slope,
		"sink": sink,
		"follow_slope": follow_slope,
		"random_turn": random_turn,
		"scale_min": scale_range.x,
		"scale_max": scale_range.y,
		"spacing": spacing,
		"world_types": PackedInt32Array(world_types),
	}
