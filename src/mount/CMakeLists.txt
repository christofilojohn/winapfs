# Event log message table (mc.exe from the Windows SDK)
file(GLOB _mc_candidates "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/mc.exe")
list(SORT _mc_candidates)
list(REVERSE _mc_candidates)
find_program(MC_EXE NAMES mc.exe HINTS ${_mc_candidates} PATHS ${_mc_candidates})
if(NOT MC_EXE AND _mc_candidates)
	list(GET _mc_candidates 0 MC_EXE)
endif()
if(NOT MC_EXE)
	message(FATAL_ERROR "mc.exe (Windows SDK message compiler) not found")
endif()
set(MC_OUT ${CMAKE_CURRENT_BINARY_DIR}/mc)
add_custom_command(
	OUTPUT ${MC_OUT}/messages.h ${MC_OUT}/messages.rc
	COMMAND ${CMAKE_COMMAND} -E make_directory ${MC_OUT}
	COMMAND ${MC_EXE} -h ${MC_OUT} -r ${MC_OUT} ${CMAKE_CURRENT_SOURCE_DIR}/messages.mc
	DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/messages.mc
	COMMENT "mc messages.mc")

add_executable(winapfs
	main.cpp
	Mount.cpp
	Mount.h
	Service.cpp
	Control.cpp
	Profiler.cpp
	Profiler.h
	Control.h
	Service.h
	ApfsFs.cpp
	ApfsFs.h
	messages.mc
	${CMAKE_SOURCE_DIR}/assets/app_icon.rc
	${MC_OUT}/messages.h
	${MC_OUT}/messages.rc)
target_include_directories(winapfs PRIVATE ${MC_OUT})
target_include_directories(winapfs SYSTEM PRIVATE ${WINFSP_INCLUDE_DIR})
target_link_libraries(winapfs PRIVATE winapfs_core ${WINFSP_LIBRARY} delayimp dbghelp setupapi cfgmgr32 winmm)
target_link_options(winapfs PRIVATE /ENTRY:wmainCRTStartup /DELAYLOAD:winfsp-x64.dll)
target_compile_options(winapfs PRIVATE /W4 /wd4100)
# The service's "verify" command runs winapfs-fsck.exe from winapfs.exe's own
# folder only (as installed), so put a copy there in the build tree too.
add_dependencies(winapfs winapfs-fsck)
add_custom_command(TARGET winapfs POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:winapfs-fsck> $<TARGET_FILE_DIR:winapfs>
	COMMENT "copy winapfs-fsck.exe next to winapfs.exe")
install(TARGETS winapfs RUNTIME DESTINATION bin)
