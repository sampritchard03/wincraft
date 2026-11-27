extends "res://scripts/features/handmade_feature.gd"
## A mushroom, sculpted by hand (res://assets/features/mushroom.res). Sculpt
## a new one in the game, select it, Ctrl+P and save over that file (or add
## more files to shape_paths: each placement picks one).


func _init() -> void:
	id = "mushroom"
	shape_paths = PackedStringArray(["res://assets/features/mushroom.res"])
	per_region = 1.5
	grows_on = PackedStringArray(["grass", "dirt"])
	max_slope = 25.0
	sink = 1.0
	follow_slope = 0.5
	scale_range = Vector2(1.0, 1.6)
	spacing = 6.0
