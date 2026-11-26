#include "density_body.h"
#include "parallel.h"
#include "sculpt_terrain.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

static void initialize_wincraft(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(SculptTerrain);
	GDREGISTER_CLASS(DensityShape);
	GDREGISTER_CLASS(DensityBody3D);
	GDREGISTER_CLASS(DensityCharacter3D);
}

static void uninitialize_wincraft(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		WorkerPool::get().shutdown(); // before the library (their code) goes
		DensityShape::shutdown_worker();
	}
}

extern "C" {
GDExtensionBool GDE_EXPORT wincraft_native_init(GDExtensionInterfaceGetProcAddress p_get_proc_address,
		GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
	GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);
	init_obj.register_initializer(initialize_wincraft);
	init_obj.register_terminator(uninitialize_wincraft);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
	return init_obj.init();
}
}
