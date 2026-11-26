# winapfs-tray: the notification-area app (per user). Talks to the service's
# control pipe; built next to winapfs.exe so it can find it.
add_executable(winapfs-tray WIN32
	main.cpp
	Dashboard.h
	Dashboard.cpp
	Updater.h
	Updater.cpp
	resource.h
	tray.rc
	tray.manifest
	${CMAKE_SOURCE_DIR}/src/mount/Control.cpp
	${CMAKE_SOURCE_DIR}/src/mount/Control.h)
target_include_directories(winapfs-tray PRIVATE ${CMAKE_SOURCE_DIR}/src/mount)
target_include_directories(winapfs-tray SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
target_compile_definitions(winapfs-tray PRIVATE
	WINAPFS_VERSION_STR="${PROJECT_VERSION}"
	WINAPFS_VERSION_W=L"${PROJECT_VERSION}"
	WINAPFS_VERSION_COMMA=${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0)
set_source_files_properties(tray.rc PROPERTIES COMPILE_DEFINITIONS
	"WINAPFS_VERSION_STR=\"${PROJECT_VERSION}\";WINAPFS_VERSION_COMMA=${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")
target_link_libraries(winapfs-tray PRIVATE shell32 comctl32 user32 advapi32 gdi32 gdiplus dwmapi winhttp)
target_link_options(winapfs-tray PRIVATE /MANIFEST:NO)
target_compile_options(winapfs-tray PRIVATE /W4)
set_target_properties(winapfs-tray PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/src/mount)
install(TARGETS winapfs-tray RUNTIME DESTINATION bin)
