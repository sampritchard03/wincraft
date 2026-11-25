# Wincraft diagrams

How the game works, one Mermaid diagram per file. Open a `.mmd` file and use the
Mermaid extension's preview (Mermaid Chart: the preview icon at the top right of the editor; Mermaid
Preview: right-click → *Preview Mermaid*). Start with 01 and 03 for the big picture.

## [1. Big picture](01-big-picture.mmd)

`game.tscn` just instances `world.tscn`. Nearly all gameplay is in `world.gd`
(GDScript glue and tools) calling into the C++ extension (`gdextension/src`),
which does the heavy lifting: voxel storage, sculpting, physics and the data
the raymarching shader draws.

## [2. Startup (`world.gd _ready`)](02-startup.mmd)

With `--join`, the world comes from the host first (see 12).

## [3. One frame](03-one-frame.mmd)

Godot calls these in order each frame. Physics runs at a fixed rate; the
terrain node steps every body after `world.gd` moves the player.

## [4. Controls: keys to tools](04-controls-keys-to-tools.mmd)

## [5. Sculpting decision (`_sculpt`, every frame)](05-sculpting-decision.mmd)

## [6. Selection, transform and copy/paste](06-selection-transform-and-copy-paste.mmd)

Geometry (0) and Box (0 twice / three times) work similarly: place points or
corners with clicks, preview a `DensityBody3D`, then Enter stamps it into the
terrain (Shift+Enter or Box cut carves it out). Select geometry selects the
ground inside it instead.

## [7. Undo / redo](07-undo-redo.mmd)

The terrain keeps its own undo stack in C++; `world.gd` keeps a combined
history that interleaves terrain steps with body sculpts, selection changes
and moves of the floating piece.

## [8. Physics bodies, balls and the frog](08-physics-bodies-balls-and-the-frog.mmd)

No Godot physics is used. Every body collides by sampling the terrain's
density field (roughly the signed distance to the surface) with spheres.

## [9. Rendering pipeline](09-rendering-pipeline.mmd)

There are no terrain meshes. A full-screen quad runs a raymarching shader
over 3D textures that mirror the voxel data near the camera.

## [10. Object editor (`object_editor.gd`, press M on a body)](10-object-editor.mmd)

The body is frozen and the world's normal tools sculpt and paint it. A
shape's pose keys are like a mesh's shape keys: `pose` blends between them,
and the timeline animates `pose`.

## [11. Baking the frog (offline tool, `bake_frog.gd`)](11-baking-the-frog.mmd)

Run from a terminal with `--headless --script bake_frog.gd` after editing the
model in Blender.

## [12. Multiplayer (steps 1-4 built; step 5 planned)](12-multiplayer.mmd)

    godot-4 --path . -- --host [--port 24565] [--name Sam]
    godot-4 --path . -- --join 192.168.1.20[:24565] [--name Alex]

The joining player generates the host's world from its settings, receives
its edited chunks, and plays unsaved; everyone sees each other as coloured
capsules with names, which objects bounce off. Sculpting is shared: every
tool's changes (and undo / redo) go out as the voxel values they changed,
which the host puts in one order for everyone. Physics is shared and spread
out: the world is split into 256-voxel regions, the host gives each region
with objects in it to a player (the nearest at first, then rebalanced every
2 s by load and computer speed), and each object is simulated by its
region's owner, or by whoever is using it. The rest see copies that follow.
