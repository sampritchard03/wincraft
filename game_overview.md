# Wincraft – how the game works

Each block below is a standalone Mermaid diagram. Paste one (without the
```` ```mermaid ```` fence) into https://mermaid.live to view or edit it, or open
this file in a Markdown preview that renders Mermaid (GitHub, VS Code with a
Mermaid extension).

1. Big picture: scenes, scripts and native classes
2. Startup
3. One frame
4. Controls: keys to tools
5. Sculpting decision (`_sculpt`)
6. Selection, transform and copy/paste
7. Undo / redo
8. Physics bodies, balls and the frog
9. Rendering pipeline
10. Object editor
11. Baking the frog

---

## 1. Big picture

`game.tscn` just instances `world.tscn`. Nearly all gameplay is in `world.gd`
(GDScript glue and tools) calling into the C++ extension (`gdextension/src`),
which does the heavy lifting: voxel storage, sculpting, physics and the data
the raymarching shader draws.

```mermaid
flowchart TB
    subgraph Scenes
        game["game.tscn<br/>(main scene)"] --> worldScene["world.tscn"]
        worldScene --> worldNode["World (Node)<br/>script: world.gd"]
        worldNode --> terrainNode["Terrain<br/>SculptTerrain (C++)"]
        worldNode --> screen["Screen: 320x180 SubViewport<br/>scaled up, chunky pixels"]
        screen --> cam["Camera3D"]
        screen --> view["TerrainView<br/>full-screen quad + terrain.gdshader"]
        screen --> prepass["Prepass SubViewport 160x90<br/>terrain_prepass.gdshader"]
        screen --> brush["BrushPreview sphere"]
        worldNode --> hud["HUD: crosshair + info text"]
    end

    subgraph "Created at runtime by world.gd"
        player["Player<br/>DensityCharacter3D (C++)"]
        balls["BouncyBalls<br/>bouncy_balls.gd"]
        editor["ObjectEditor<br/>object_editor.gd"]
        frogs["Frogs<br/>forg.tscn + forg.gd"]
        floating["Floating piece / previews<br/>DensityBody3D"]
    end
    worldNode --> player & balls & editor & frogs & floating

    subgraph "C++ extension (wincraft_native)"
        ST["SculptTerrain<br/>brushes, selection, undo,<br/>steps all bodies, update_view"]
        VW["VoxelWorld<br/>whole wrapping world in 32^3 chunks,<br/>generation, save/load"]
        CM["Clipmap<br/>nested LOD textures around camera<br/>for the shader"]
        NO["noise<br/>island / ground generator"]
        DB["DensityBody3D / DensityCharacter3D<br/>custom physics against density field"]
        DS["DensityShape<br/>small density grid + pose keys"]
        SEL["selection<br/>bitmask of selected voxels"]
    end
    terrainNode -.is.-> ST
    ST --> VW --> NO
    ST --> CM
    ST --> SEL
    ST -->|physics step| DB
    DB --> DS
    player -.is.-> DB
    balls --> DB
    frogs -.is.-> DB

    CM -->|textures| view
    ST -->|body atlas| view
    VW <-->|region files| disk[("user://torus<br/>world + player.cfg + objects.cfg")]
```

---

## 2. Startup (`world.gd _ready`)

```mermaid
flowchart TD
    A([Game starts]) --> B["terrain.open_world()<br/>load save or generate world<br/>(all chunks, in parallel)"]
    B --> C{"Saved player<br/>position?"}
    C -- yes --> D[Restore camera]
    C -- no --> E["find_island near origin<br/>spawn beside it, look at it"]
    D & E --> F["Set terrain.gravity"]
    F --> G["_make_player()<br/>DensityCharacter3D capsule"]
    G --> H["BouncyBalls.new()<br/>load objects.cfg"]
    H --> I["Read material names<br/>build colour picker panel"]
    I --> J["Create ObjectEditor"]
    J --> K["Load block textures (Texture2DArray)<br/>+ water texture into shader"]
    K --> L["terrain.update_view(camera)<br/>build + upload clipmap"]
    L --> M["Fit screen, make symmetry visuals"]
    M --> N([Running])
```

---

## 3. One frame

Godot calls these in order each frame. Physics runs at a fixed rate; the
terrain node steps every body after `world.gd` moves the player.

```mermaid
flowchart TD
    subgraph "_input (per event)"
        I0{"ObjectEditor.handle_input<br/>took it?"} -- yes --> Iend([done])
        I0 -- no --> I1["Mouse move: yaw / pitch camera"]
        I0 -- no --> I2["Mouse button: capture mouse, physics gun,<br/>box/geometry clicks, gizmo, middle-click pick,<br/>wheel = radius / strength / size"]
        I0 -- no --> I3["Keys: tools, undo/redo, copy/paste,<br/>spawn ball/frog, symmetry, ..."]
    end

    subgraph "_physics_process (fixed rate)"
        P1["_wrap_player()<br/>shift everything by world size<br/>when past half-way"] --> P2["balls.stream()<br/>load / unload far balls"]
        P2 --> P3["WASD + sprint + jump<br/>-> player.velocity, gravity"]
        P3 --> P4["player.move_and_slide()<br/>(against density field)"]
        P4 --> P5["_update_physics_gun()"]
        P5 --> P6["SculptTerrain._physics_process<br/>DensityBody3D.step_all():<br/>gravity, terrain contacts,<br/>body-body contacts, sleep"]
        P6 --> P7["forg.gd _physics_process<br/>(each frog decides hops)"]
    end

    subgraph "_process (every rendered frame)"
        R1["Camera follows player eye<br/>(eased on small steps)"] --> R2["_sculpt(delta)<br/>see diagram 5"]
        R2 --> R3["_update_gizmo / _update_geometry / _update_box"]
        R3 --> R4["_record_history()<br/>see diagram 7"]
        R4 --> R5["terrain.update_view(camera)<br/>move clipmap, upload changes,<br/>send drawn bodies, autosave"]
        R5 --> R6["Sync prepass camera"]
        R6 --> R7{"30 s passed?"}
        R7 -- yes --> R8["Save player + objects"]
    end
```

---

## 4. Controls: keys to tools

```mermaid
flowchart LR
    subgraph "Number keys (repeat to cycle)"
        k1["1"] --> Draw & Inflate
        k2["2"] --> Smooth & Pinch
        k3["3"] --> Flatten
        k4["4"] --> Grab
        k5["5"] --> SnakeHook["Snake hook"]
        k6["6"] --> Paint
        k7["7"] --> SelPaint["Select paint"] & SelFill["Select fill"] & SelGeo["Select geometry"]
        k8["8"] --> Extrude & Transform
        k9["9"] --> Gun["Physics gun"]
        k0["0"] --> Geometry & Box & BoxCut["Box cut"]
    end

    subgraph "Other keys"
        F["F: throw ball"]
        G["G: drop frog"]
        X["X: remove aimed body"]
        P["P: selection -> physics object"]
        B["B: blocky mode"]
        Br["[ ]: material"]
        Tab["Tab: colour picker"]
        EQZ["E / Q / Z / + -: symmetry"]
        M["M: object editor"]
        T["T: gizmo move/rotate/scale"]
        Ent["Enter: confirm (Shift = cut)"]
        Bs["Backspace: clear / cancel"]
        CZ["Ctrl+Z / Ctrl+Shift+Z: undo / redo"]
        CC["Ctrl+C / Ctrl+V: copy / paste"]
        CP["Ctrl+P / Ctrl+O: export / import shape"]
        KL["K / L / Shift+wheel: player size"]
    end
```

---

## 5. Sculpting decision (`_sculpt`, every frame)

```mermaid
flowchart TD
    S0{"Object editor owns<br/>the mouse?"} -- yes --> S0a["End stroke / drag,<br/>hide brush"] --> Send([return])
    S0 -- no --> S1["pressing = LMB or RMB held<br/>(not for gun / transform / geometry / box)"]
    S1 --> S2{"Stroke start<br/>or end?"}
    S2 -- start --> S2a["terrain.begin_stroke()"]
    S2 -- end --> S2b["terrain.end_stroke()<br/>release held body,<br/>clear flatten plane"]
    S2a & S2b & S2 --> S3{"Dragging<br/>(grab / snake hook)?"}
    S3 -- yes --> S3a["_drag(): terrain.warp()<br/>or body.warp()"]
    S3 -- no --> S4["Raycast terrain,<br/>then raycast bodies in front"]
    S4 --> S5{"Hit a body<br/>(ball / frog)?"}
    S5 -- yes --> S5a["target_body = it<br/>hold it still while sculpting"]
    S5 -- no --> S6
    S5a --> S6{"Grab / snake hook<br/>and LMB?"}
    S6 -- yes --> S6a["_begin_drag(hit)"]
    S6 -- no --> S7
    S6a --> S7["Move brush preview to hit"]
    S3a --> S7
    S7 --> S8["_select(): select paint / fill"]
    S8 --> S9["_extrude(): sweep selection<br/>along surface normal"]
    S9 --> S10{"Which brush mode?"}
    S10 -- "RMB" --> S10a["SUBTRACT<br/>(inflate/pinch: inverted)"]
    S10 -- "LMB" --> S10b["Tool's mode: ADD, SMOOTH,<br/>FLATTEN, PAINT, INFLATE, PINCH"]
    S10 -- "non-sculpt tool<br/>or no hit" --> S12
    S10a & S10b --> S11{"Target is a body?"}
    S11 -- yes --> S11a["body.apply_brush / paint_color<br/>(no symmetry)"]
    S11 -- no --> S11b["For each symmetry copy:<br/>terrain.apply_brush / paint_color<br/>(flatten locks a plane first)"]
    S11a & S11b --> S12["Update symmetry visuals<br/>+ HUD info text"]
```

---

## 6. Selection, transform and copy/paste

```mermaid
stateDiagram-v2
    [*] --> NoSelection
    NoSelection --> Selected: Select paint / fill / geometry (7)
    Selected --> NoSelection: Backspace
    Selected --> Selected: Extrude (8) drags it outwards
    Selected --> PhysicsObject: P (cut out, becomes a body)
    Selected --> File: Ctrl+P export DensityShape
    Selected --> Clipboard: Ctrl+C
    Selected --> Floating: Transform tool (8 twice) lifts it out
    Clipboard --> Floating: Ctrl+V paste
    File --> PhysicsObject: Ctrl+O import
    File --> Floating: Ctrl+Shift+O import to paste

    state Floating {
        [*] --> Move
        Move --> Rotate: T
        Rotate --> Scale: T
        Scale --> Move: T
    }
    Floating --> Selected: Enter (stamped back into terrain)
    Floating --> NoSelection: Shift+Enter (carves its shape out)
    Floating --> Selected: Ctrl+Z past first move (put back where lifted)

    PhysicsObject --> [*]
```

Geometry (0) and Box (0 twice / three times) work similarly: place points or
corners with clicks, preview a `DensityBody3D`, then Enter stamps it into the
terrain (Shift+Enter or Box cut carves it out). Select geometry selects the
ground inside it instead.

---

## 7. Undo / redo

The terrain keeps its own undo stack in C++; `world.gd` keeps a combined
history that interleaves terrain steps with body sculpts, selection changes
and moves of the floating piece.

```mermaid
flowchart TD
    subgraph Recording["_record_history() each frame"]
        A1["terrain.get_undo_serial()<br/>advanced?"] -- yes --> A2["push {terrain: true}<br/>per finished stroke"]
        A3["Selection serial changed<br/>and mouse released?"] -- yes --> A4{"Came with a<br/>terrain step?"}
        A4 -- yes --> A5["attach sel_before / sel_after<br/>to that entry"]
        A4 -- no --> A6["push {selection: true}"]
        A7["Body sculpt"] --> A8["push {body, before, after, parts}"]
        A9["Gizmo move"] --> A10["push {floating, before, after}"]
    end

    subgraph Undo["Ctrl+Z"]
        U0{"Piece floating?"} -- yes --> U1{"Last entry is<br/>its move?"}
        U1 -- yes --> U2["restore its transform"]
        U1 -- no --> U3["cancel lift: put it back"]
        U0 -- no --> U4["pop entry"]
        U4 --> U5{"Kind"}
        U5 -- terrain --> U6["terrain.undo()"]
        U5 -- body --> U7["shape = before,<br/>remove split-off parts"]
        U5 -- selection --> U8["restore sel_before"]
        U6 & U7 & U8 --> U9["move entry to future (redo)"]
    end
```

---

## 8. Physics bodies, balls and the frog

No Godot physics is used. Every body collides by sampling the terrain's
density field (roughly the signed distance to the surface) with spheres.

```mermaid
flowchart TD
    subgraph Kinds["Kinds of DensityBody3D"]
        K1["Player: DensityCharacter3D<br/>capsule of spheres, frozen,<br/>moved by move_and_slide"]
        K2["Ball: DensityShape.make_sphere<br/>(bouncy_balls.gd)"]
        K3["Physics object: shape from<br/>selection (P) or import"]
        K4["Frog: forg.tscn<br/>shape with pose keys"]
    end

    subgraph Step["DensityBody3D.step_all (once per physics frame)"]
        S1["Skip sleeping / frozen bodies"] --> S2["Split into substeps<br/>(no tunnelling)"]
        S2 --> S3["Integrate gravity + velocity"]
        S3 --> S4["Each probe sphere:<br/>depth = r + density(centre)"]
        S4 --> S5{"Touching ground<br/>or another body?"}
        S5 -- yes --> S6["Push out along gradient,<br/>bounce impulse, friction"]
        S5 -- no --> S9
        S6 --> S9{"Slow for a while<br/>while touching?"}
        S9 -- yes --> S10["Sleep until an edit<br/>or hit wakes it"]
    end

    subgraph Balls["bouncy_balls.gd"]
        B1["F: throw ball"] --> B2["stream(): unload > 512 away,<br/>reload < 448"]
        B2 --> B3["Fell out of world: break"]
        B2 --> B4["Saved to objects.cfg"]
    end

    subgraph Frog["forg.gd AI (each physics frame)"]
        F1["Every 4-10 s: pick a<br/>random direction"] --> F2{"On ground and<br/>not facing it?"}
        F2 -- yes --> F3["Every 0.6 s: small hop<br/>+ turn towards it"]
        F2 -- yes --> F4["Every 10-15 s: big leap<br/>forward + up"]
        F3 & F4 --> F5["AnimationTree: idle / jump_start / jump<br/>animates the shape's 'pose'"]
    end
```

---

## 9. Rendering pipeline

There are no terrain meshes. A full-screen quad runs a raymarching shader
over 3D textures that mirror the voxel data near the camera.

```mermaid
flowchart LR
    VW["VoxelWorld<br/>density byte + 16-bit material<br/>per point, 32^3 chunks"] --> CM["Clipmap<br/>levels 0..n, each 2x coarser,<br/>centred on camera,<br/>rebuilt on worker thread"]
    CM --> TEX["Texture arrays per level<br/>(R = density, GB = material)<br/>+ brick distance texture"]
    Bodies["Drawn DensityBody3Ds"] --> ATLAS["Body atlas + transforms"]
    SEL["Selection mask"] --> TEX

    TEX --> PRE["Prepass 160x90<br/>terrain_prepass.gdshader:<br/>where rays can start"]
    TEX --> MAIN["terrain.gdshader 320x180<br/>raymarch, skip empty bricks,<br/>textures / colours, water"]
    ATLAS --> MAIN
    PRE --> MAIN
    MAIN --> SHADOW["terrain_shadow.gdshaderinc<br/>shadow rays"]
    SHADOW --> OUT["Scaled up to window<br/>(nearest neighbour)"]
    OUT --> HUD["HUD at full resolution"]
```

---

## 10. Object editor (`object_editor.gd`, press M on a body)

The body is frozen and the world's normal tools sculpt and paint it. A
shape's pose keys are like a mesh's shape keys: `pose` blends between them,
and the timeline animates `pose`.

```mermaid
stateDiagram-v2
    [*] --> Closed
    Closed --> Editing: M aimed at a body (frozen, own copy of its shape, AnimationTree paused)
    Editing --> Closed: M (freeze and AnimationTree restored)

    state Editing {
        [*] --> Sculpting
        Sculpting: Sculpting - world tools edit the body (all keys, or one key only)
        Posing: Posing - pose slider blends keys, auto-keyed at current frame
        Animating: Animating - I key pose, U delete key, Y play, comma/period step
        Sculpting --> Posing: move pose slider
        Posing --> Animating: I / Y / timeline
        Animating --> Sculpting: brush
    }

    note right of Editing
        Side panel: pose slider, sculpt key, physics test,
        centre of mass, export model / scene.
        Timeline: animations (new, rename, delete,
        length, FPS, loop, import / export).
        Tab frees the mouse for the panels.
    end note
```

---

## 11. Baking the frog (offline tool, `bake_frog.gd`)

Run from a terminal with `--headless --script bake_frog.gd` after editing the
model in Blender.

```mermaid
flowchart LR
    A["forg.glb<br/>(Blender model)"] --> B["forg_model.tscn<br/>animated mesh"]
    B --> C["bake_frog.gd:<br/>sample poses idle, jump_start, jump"]
    C --> D["DensityShape.bake_meshes()<br/>one density key per pose"]
    D --> E[("forg_shape.res")]
    D --> F["Rebuild forg.tscn:<br/>DensityBody3D + AnimationPlayer<br/>animating 'pose' between keys"]
    E --> F
```
