extends RefCounted
## The HUD's look, from image files in res://assets/ui/ (see the README
## there): replace a PNG to reskin that part. A missing file becomes a
## magenta square (a panel: a flat grey one), so a typo shows.
##
## Panels and slots are nine-patch: their corners (PANEL_MARGIN / SLOT_MARGIN
## pixels) stay as drawn and the middle stretches. An optional
## res://assets/ui/font.ttf (or .otf) replaces the default font.

const DIR := "res://assets/ui/"
const PANEL_MARGIN := 8  # panel.png's corners, in pixels
const SLOT_MARGIN := 14  # slot*.png's corners
const FONT_SIZE := 15
const SMALL_FONT_SIZE := 12

static var _cache := {}

## A texture by name (no extension): imported, or read from the PNG itself
## if the editor hasn't imported it yet.
static func texture(p_name: String) -> Texture2D:
	if _cache.has(p_name):
		return _cache[p_name]
	var path := DIR + p_name + ".png"
	var result: Texture2D = null
	if ResourceLoader.exists(path):
		result = load(path)
	elif FileAccess.file_exists(path):
		var image := Image.load_from_file(path)
		if image != null and not image.is_empty():
			result = ImageTexture.create_from_image(image)
	if result == null:
		push_warning("Missing UI texture " + path)
		var image := Image.create_empty(16, 16, false, Image.FORMAT_RGBA8)
		image.fill(Color.MAGENTA)
		result = ImageTexture.create_from_image(image)
	_cache[p_name] = result
	return result

## A nine-patch style box from a texture.
static func box(p_name: String, p_margin: int, p_padding := 6.0) -> StyleBoxTexture:
	var style := StyleBoxTexture.new()
	style.texture = texture(p_name)
	style.texture_margin_left = p_margin
	style.texture_margin_top = p_margin
	style.texture_margin_right = p_margin
	style.texture_margin_bottom = p_margin
	style.content_margin_left = p_padding + 2.0
	style.content_margin_right = p_padding + 2.0
	style.content_margin_top = p_padding
	style.content_margin_bottom = p_padding
	return style

## The theme for everything under the HUD: panels from panel.png, labels
## with an outline so they read over the ground and the sky.
static func make_theme() -> Theme:
	var theme := Theme.new()
	for path in [DIR + "font.ttf", DIR + "font.otf"]:
		if ResourceLoader.exists(path) or FileAccess.file_exists(path):
			var font := FontFile.new()
			if font.load_dynamic_font(path) == OK:
				theme.default_font = font
			break
	theme.default_font_size = FONT_SIZE
	theme.set_stylebox("panel", "PanelContainer", box("panel", PANEL_MARGIN))
	theme.set_color("font_color", "Label", Color(0.95, 0.95, 0.97))
	theme.set_color("font_outline_color", "Label", Color(0, 0, 0))
	theme.set_constant("outline_size", "Label", 4)
	theme.set_type_variation("SmallLabel", "Label")
	theme.set_font_size("font_size", "SmallLabel", SMALL_FONT_SIZE)
	theme.set_color("font_color", "SmallLabel", Color(0.8, 0.82, 0.88))
	return theme

## The file name (no extension) of a tool's icon: "tool_" and its name in
## lower case, spaces as underscores ("Snake hook": tool_snake_hook).
static func tool_icon(p_tool_name: String) -> String:
	return "tool_" + p_tool_name.to_lower().replace(" ", "_")
