extends "res://scripts/features/feature.gd"
## A feature whose shapes are built in code: a subclass's build() makes one
## (with ShapeBuilder, say), different for each random number generator.
## `variants` of them are built when the world opens (each from its own
## seed, so every computer builds the same ones); each placement picks one
## and turns and sizes it, so no two look alike.

const ShapeBuilder := preload("res://scripts/objects/shape_builder.gd")

## How many different shapes to build.
var variants := 6


## Builds one shape. p_rng: this variant's own (seeded from the feature's id).
func build(_rng: RandomNumberGenerator) -> DensityShape:
	return null


func get_shapes() -> Array[DensityShape]:
	var shapes: Array[DensityShape] = []
	for v in maxi(variants, 1):
		var rng := RandomNumberGenerator.new()
		rng.seed = hash("%s/%d" % [id, v])
		var shape := build(rng)
		if shape != null:
			shapes.append(shape)
	return shapes


## The material for a block name (SculptTerrain.get_material_names()), or
## for a colour.
static func block(p_name: String) -> int:
	return maxi(SculptTerrain.get_material_names().find(p_name), 0)


static func color(p_color: Color) -> int:
	return SculptTerrain.color_to_material(p_color)
