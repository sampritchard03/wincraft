HUD textures (loaded by ui_skin.gd). Replace any PNG here with your own,
same file name, and the HUD uses it. The current ones are placeholders.

Frames (nine-patch: the corners stay as drawn, the middle stretches)
  panel.png          32x32, 8 px corners   status, FPS, help, color picker panels
  slot.png           64x64, 14 px corners  a slot in the bottom bar / the wheel
  slot_hover.png     64x64, 14 px corners  the wheel slot being pointed at
  slot_active.png    64x64, 14 px corners  the wheel slot of the tool in use

Tool wheel (hold Tab)
  radial_back.png    512x512   the ring behind the icons (icons sit 190 px from the middle)
  radial_pointer.png 24x24     the pointer that follows the mouse

Crosshair
  crosshair.png      32x32

Tool icons (64x64, also in the bar): tool_ + the tool's name in lower case
  tool_draw, tool_inflate, tool_smooth, tool_pinch, tool_flatten, tool_grab,
  tool_snake_hook, tool_paint, tool_select_paint, tool_select_fill,
  tool_select_geometry, tool_extrude, tool_transform, tool_physics_gun,
  tool_geometry, tool_box, tool_box_cut

Bar and status icons (64x64)
  stat_radius, stat_strength, stat_symmetry_off, stat_symmetry_mirror,
  stat_symmetry_radial, stat_blocky, stat_selection, stat_undo, stat_redo
  (spare), stat_help, stat_fps, stat_players, stat_info

Font (optional): put font.ttf or font.otf here to replace the default font.
Sizes and text colors are in ui_skin.gd (FONT_SIZE, make_theme()).
