add_executable(winapfs-verify verify.cpp)
target_link_libraries(winapfs-verify PRIVATE winapfs_core apfsrw bcrypt)
target_include_directories(winapfs-verify PRIVATE ${TP})
target_compile_options(winapfs-verify PRIVATE /W3)
target_link_options(winapfs-verify PRIVATE /ENTRY:wmainCRTStartup)

# APFS <-> Windows name mapping: exact round trip, no collisions (no images needed)
add_executable(winapfs-names-test names_test.cpp ${CMAKE_SOURCE_DIR}/src/core/winapfs/Names.cpp)
target_include_directories(winapfs-names-test PRIVATE ${CMAKE_SOURCE_DIR}/src/core)
target_compile_options(winapfs-names-test PRIVATE /W4)
add_test(NAME unit.names COMMAND winapfs-names-test)

# Control pipe helpers & updater security/version checks (no images needed)
add_executable(winapfs-control-test control_test.cpp
	${CMAKE_SOURCE_DIR}/src/mount/Control.cpp
	${CMAKE_SOURCE_DIR}/src/tray/Updater.cpp)
target_include_directories(winapfs-control-test PRIVATE ${CMAKE_SOURCE_DIR}/src/mount ${CMAKE_SOURCE_DIR}/src/tray)
target_include_directories(winapfs-control-test SYSTEM PRIVATE ${TP})
target_compile_definitions(winapfs-control-test PRIVATE WINAPFS_VERSION_W=L"${PROJECT_VERSION}")
target_link_libraries(winapfs-control-test PRIVATE winhttp)
target_compile_options(winapfs-control-test PRIVATE /W4)
add_test(NAME unit.control COMMAND winapfs-control-test)

set(TD ${CMAKE_SOURCE_DIR}/testdata)
set(TEST_PASSWORD winapfs-test)

# image manifest-prefix volume [password]
set(VOLUMES
	"basic.container.img|basic|Basic|"
	"basic.img|basic|Basic|"
	"compressed.img|compressed|Compressed|"
	"links.img|links|Links|"
	"casesensitive.img|casesensitive|CaseSens|"
	"multivol.img|multivol|VolA|"
	"multivol.img|multivol|VolB|"
	"multivol.img|multivol|VolC|"
	"encrypted.img|encrypted|Public|"
	"encrypted.img|encrypted|Secret|${TEST_PASSWORD}")

# Library-level checks: every manifest field (contents, metadata, xattrs) through the core library
foreach(v ${VOLUMES})
	string(REPLACE "|" ";" f "${v}")
	list(GET f 0 img)
	list(GET f 1 prefix)
	list(GET f 2 vol)
	list(GET f 3 pw)
	set(args ${TD}/${prefix}.${vol}.manifest.json --image ${TD}/${img} --volume ${vol} --fields all)
	if(pw)
		list(APPEND args --password ${pw})
	endif()
	add_test(NAME image.${img}.${vol} COMMAND winapfs-verify ${args})
	if(EXISTS ${TD}/${img})
	else()
		set_tests_properties(image.${img}.${vol} PROPERTIES DISABLED TRUE)
	endif()
endforeach()

# CLI smoke tests
add_test(NAME cli.info COMMAND winapfs-cli info ${TD}/multivol.img)
set_tests_properties(cli.info PROPERTIES PASS_REGULAR_EXPRESSION "VolA.*\n.*VolB.*\n.*VolC")
add_test(NAME cli.cat COMMAND winapfs-cli cat ${TD}/basic.container.img /hello.txt)
set_tests_properties(cli.cat PROPERTIES PASS_REGULAR_EXPRESSION "^Hello, APFS!\n$")
add_test(NAME cli.list COMMAND winapfs-cli list ${TD}/basic.img /names)
set_tests_properties(cli.list PROPERTIES PASS_REGULAR_EXPRESSION "colon:name\\.txt")
add_test(NAME cli.wrong_password COMMAND winapfs-cli list ${TD}/encrypted.img -v Secret -p nope)
set_tests_properties(cli.wrong_password PROPERTIES WILL_FAIL TRUE)

# Milestone 2: mount through WinFsp and check what Windows programs see
# (Win32 directory walk + CreateFile/ReadFile of every file against the manifest).
# MOUNT_FIELDS grows as fidelity features land.
set(MOUNT_FIELDS type,size,sha256,target,xattrs)
if(TARGET winapfs)
	foreach(v ${VOLUMES})
		string(REPLACE "|" ";" f "${v}")
		list(GET f 0 img)
		list(GET f 1 prefix)
		list(GET f 2 vol)
		list(GET f 3 pw)
		set(args mount ${TD}/${img} * --volume ${vol})
		if(pw)
			list(APPEND args --password ${pw})
		endif()
		list(APPEND args --run "\"$<TARGET_FILE:winapfs-verify>\" \"${TD}/${prefix}.${vol}.manifest.json\" --dir %MOUNT% --fields ${MOUNT_FIELDS}")
		add_test(NAME mount.${img}.${vol} COMMAND winapfs ${args})
		set_tests_properties(mount.${img}.${vol} PROPERTIES RESOURCE_LOCK winfsp_mount)
		if(NOT EXISTS ${TD}/${img})
			set_tests_properties(mount.${img}.${vol} PROPERTIES DISABLED TRUE)
		endif()
	endforeach()
endif()

# Milestone 3: mount-all mounts every volume of every container it finds.
# Images here; tests/physical_test.ps1 does the same against attached VHDs
# (real \.\PhysicalDriveN, needs an elevated prompt).
if(TARGET winapfs AND EXISTS ${TD}/multivol.img AND EXISTS ${TD}/encrypted.img)
	add_test(NAME mount_all.images
		COMMAND winapfs mount-all ${TD}/multivol.img ${TD}/encrypted.img --password ${TEST_PASSWORD}
			--run "powershell -NoProfile -ExecutionPolicy Bypass -File \"${CMAKE_CURRENT_SOURCE_DIR}/verify_mounts.ps1\" -Verify \"$<TARGET_FILE:winapfs-verify>\" -Manifests ${TD}/multivol.VolA.manifest.json,${TD}/multivol.VolB.manifest.json,${TD}/multivol.VolC.manifest.json,${TD}/encrypted.Public.manifest.json,${TD}/encrypted.Secret.manifest.json")
	set_tests_properties(mount_all.images PROPERTIES RESOURCE_LOCK winfsp_mount)
endif()

# The raw-device code path (DeviceWinPhys: aligned reads, chunk cache,
# overlapped I/O) without admin: \.\C:\...\file.img is a device-namespace
# path to a plain file.
file(TO_NATIVE_PATH "${TD}" TD_NATIVE)
set(BS "\\")
foreach(v "basic.img|basic|Basic|" "compressed.img|compressed|Compressed|" "encrypted.img|encrypted|Secret|${TEST_PASSWORD}")
	string(REPLACE "|" ";" f "${v}")
	list(GET f 0 img)
	list(GET f 1 prefix)
	list(GET f 2 vol)
	list(GET f 3 pw)
	set(args ${TD}/${prefix}.${vol}.manifest.json --image "${BS}${BS}.${BS}${TD_NATIVE}${BS}${img}" --volume ${vol} --fields all)
	if(pw)
		list(APPEND args --password ${pw})
	endif()
	add_test(NAME rawdev.${img}.${vol} COMMAND winapfs-verify ${args})
endforeach()
if(TARGET winapfs)
	add_test(NAME mount_rawdev.basic.img
		COMMAND winapfs mount "${BS}${BS}.${BS}${TD_NATIVE}${BS}basic.img" *
			--run "\"$<TARGET_FILE:winapfs-verify>\" \"${TD}/basic.Basic.manifest.json\" --dir %MOUNT% --fields ${MOUNT_FIELDS}")
	set_tests_properties(mount_rawdev.basic.img PROPERTIES RESOURCE_LOCK winfsp_mount)
endif()

# linux-apfs-rw engine (W1)
add_executable(apfsrw-smoke apfsrw_smoke.cpp)
target_link_libraries(apfsrw-smoke PRIVATE apfsrw)
target_link_options(apfsrw-smoke PRIVATE /ENTRY:wmainCRTStartup)

# Read-path parity of the linux-apfs-rw engine: every manifest field.
# Encrypted volumes are refused (the engine has no encryption yet, W6).
foreach(v ${VOLUMES})
	string(REPLACE "|" ";" f "${v}")
	list(GET f 0 img)
	list(GET f 1 prefix)
	list(GET f 2 vol)
	list(GET f 3 pw)
	add_test(NAME rwengine.${img}.${vol}
		COMMAND winapfs-verify ${TD}/${prefix}.${vol}.manifest.json --image ${TD}/${img} --volume ${vol} --engine rw --fields all)
	if(pw)
		set_tests_properties(rwengine.${img}.${vol} PROPERTIES WILL_FAIL TRUE)
	endif()
endforeach()

# Name matching: case/normalization-insensitive volumes find any spelling,
# case-sensitive ones only the exact one. Both engines, CLI and mount.
add_test(NAME cli.cat_case COMMAND winapfs-cli cat ${TD}/basic.container.img /HELLO.TXT)
set_tests_properties(cli.cat_case PROPERTIES PASS_REGULAR_EXPRESSION "^Hello, APFS!\n$")
if(TARGET winapfs)
	foreach(engine rw lib)
		add_test(NAME mount_case.${engine}.insensitive
			COMMAND winapfs mount ${TD}/basic.container.img * --engine ${engine} --run "cmd /c type %MOUNT%${BS}HELLO.TXT")
		set_tests_properties(mount_case.${engine}.insensitive PROPERTIES PASS_REGULAR_EXPRESSION "Hello, APFS!" RESOURCE_LOCK winfsp_mount)
		add_test(NAME mount_case.${engine}.sensitive
			COMMAND winapfs mount ${TD}/casesensitive.img * --engine ${engine} --run "cmd /c type %MOUNT%${BS}README.TXT")
		set_tests_properties(mount_case.${engine}.sensitive PROPERTIES WILL_FAIL TRUE RESOURCE_LOCK winfsp_mount)
	endforeach()
	# auto picks linux-apfs-rw for unencrypted volumes; keep the apfs-fuse mount path covered
	add_test(NAME mount_lib.basic.img
		COMMAND winapfs mount ${TD}/basic.img * --engine lib
			--run "\"$<TARGET_FILE:winapfs-verify>\" \"${TD}/basic.Basic.manifest.json\" --dir %MOUNT% --fields ${MOUNT_FIELDS}")
	set_tests_properties(mount_lib.basic.img PROPERTIES RESOURCE_LOCK winfsp_mount)
endif()

# W2: winapfs-fsck (apfsprogs' apfsck). Every unencrypted image must check
# clean; apfsck cannot decrypt, so encrypted containers wait for W6.
add_executable(apfs-corrupt apfs_corrupt.cpp)
foreach(img basic.container basic compressed links casesensitive multivol)
	add_test(NAME fsck.${img} COMMAND winapfs-fsck ${TD}/${img}.img)
	add_test(NAME fsck_crash.${img} COMMAND winapfs-fsck -c ${TD}/${img}.img)
endforeach()
# and it must notice damage: a flipped byte, and a misdirected omap record
# whose checksum has been fixed up
foreach(mode omap-flip omap-misdirect)
	add_test(NAME fsck_detects.${mode}.prepare
		COMMAND apfs-corrupt ${TD}/basic.container.img ${CMAKE_CURRENT_BINARY_DIR}/corrupt-${mode}.img ${mode})
	set_tests_properties(fsck_detects.${mode}.prepare PROPERTIES FIXTURES_SETUP corrupt-${mode})
	add_test(NAME fsck_detects.${mode} COMMAND winapfs-fsck ${CMAKE_CURRENT_BINARY_DIR}/corrupt-${mode}.img)
	set_tests_properties(fsck_detects.${mode} PROPERTIES WILL_FAIL TRUE FIXTURES_REQUIRED corrupt-${mode})
endforeach()

# W3: writes with the linux-apfs-rw engine on image copies, read back through
# both engines and checked with winapfs-fsck.
add_executable(apfsrw-bench apfsrw_bench.cpp)
target_link_libraries(apfsrw-bench PRIVATE apfsrw dbghelp psapi)
target_link_options(apfsrw-bench PRIVATE /ENTRY:wmainCRTStartup)

add_executable(apfsrw-writetest writetest.cpp)
target_link_libraries(apfsrw-writetest PRIVATE winapfs_core apfsrw bcrypt dbghelp)
target_include_directories(apfsrw-writetest PRIVATE ${TP})
target_link_options(apfsrw-writetest PRIVATE /ENTRY:wmainCRTStartup)
foreach(v "basic.container.img|basic|Basic" "basic.img|basic|Basic" "casesensitive.img|casesensitive|CaseSens" "multivol.img|multivol|VolB" "compressed.img|compressed|Compressed" "links.img|links|Links")
	string(REPLACE "|" ";" f "${v}")
	list(GET f 0 img)
	list(GET f 1 prefix)
	list(GET f 2 vol)
	set(work ${CMAKE_CURRENT_BINARY_DIR}/w3-${img})
	add_test(NAME write.${img}.${vol}
		COMMAND apfsrw-writetest ${TD}/${prefix}.${vol}.manifest.json ${TD}/${img} ${work} ${vol})
	set_tests_properties(write.${img}.${vol} PROPERTIES FIXTURES_SETUP w3-${img})
	add_test(NAME write_fsck.${img} COMMAND winapfs-fsck ${work})
	add_test(NAME write_fsck_crash.${img} COMMAND winapfs-fsck -c ${work})
	set_tests_properties(write_fsck.${img} write_fsck_crash.${img} PROPERTIES FIXTURES_REQUIRED w3-${img})
endforeach()

# W3b: the same writes through a read-write WinFsp mount with plain Win32
# file APIs, then the image is checked after unmount.
if(TARGET winapfs)
	foreach(v "basic.container.img|basic|Basic" "basic.img|basic|Basic" "casesensitive.img|casesensitive|CaseSens" "multivol.img|multivol|VolB" "compressed.img|compressed|Compressed" "links.img|links|Links")
		string(REPLACE "|" ";" f "${v}")
		list(GET f 0 img)
		list(GET f 1 prefix)
		list(GET f 2 vol)
		set(work ${CMAKE_CURRENT_BINARY_DIR}/w32-${img})
		set(model ${CMAKE_CURRENT_BINARY_DIR}/w32-${img}.model.json)
		add_test(NAME mountwrite_prepare.${img} COMMAND ${CMAKE_COMMAND} -E copy ${TD}/${img} ${work})
		set_tests_properties(mountwrite_prepare.${img} PROPERTIES FIXTURES_SETUP w32prep-${img})
		add_test(NAME mountwrite.${img}.${vol}
			COMMAND winapfs mount ${work} * --rw --volume ${vol}
				--run "\"$<TARGET_FILE:apfsrw-writetest>\" --win32 %MOUNT% \"${TD}/${prefix}.${vol}.manifest.json\" \"${work}\" \"${model}\" ${vol}")
		set_tests_properties(mountwrite.${img}.${vol} PROPERTIES RESOURCE_LOCK winfsp_mount
			FIXTURES_REQUIRED w32prep-${img} FIXTURES_SETUP w32-${img})
		add_test(NAME mountwrite_verify.${img}
			COMMAND apfsrw-writetest --verify ${TD}/${prefix}.${vol}.manifest.json ${work} ${model} ${vol})
		add_test(NAME mountwrite_fsck.${img} COMMAND winapfs-fsck ${work})
		add_test(NAME mountwrite_fsck_crash.${img} COMMAND winapfs-fsck -c ${work})
		set_tests_properties(mountwrite_verify.${img} mountwrite_fsck.${img} mountwrite_fsck_crash.${img}
			PROPERTIES FIXTURES_REQUIRED w32-${img})
	endforeach()
	# --rw is refused where it isn't safe yet
	# (a \\.\ path to an image file goes through the raw-disk device; no admin needed)
	add_test(NAME mountwrite_refuse.rawdisk COMMAND winapfs mount "${BS}${BS}.${BS}${TD_NATIVE}${BS}basic.img" * --rw --run "cmd /c exit 0")
	add_test(NAME mountwrite_refuse.encrypted COMMAND winapfs mount ${TD}/encrypted.img * --rw --volume Secret --password ${TEST_PASSWORD} --run "cmd /c exit 0")
	set_tests_properties(mountwrite_refuse.rawdisk mountwrite_refuse.encrypted PROPERTIES WILL_FAIL TRUE)
endif()

# W3c: crash safety. apfsrw-writetest crashes itself (process kill, power cut
# with a volatile disk cache) at spread-out device writes; after each crash
# fsck must be clean, the synced state intact, and the volume writable again.
foreach(v "basic.img|basic|Basic" "casesensitive.img|casesensitive|CaseSens" "multivol.img|multivol|VolB")
	string(REPLACE "|" ";" f "${v}")
	list(GET f 0 img)
	list(GET f 1 prefix)
	list(GET f 2 vol)
	add_test(NAME crash.${img}.${vol}
		COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_CURRENT_SOURCE_DIR}/crash_test.ps1
			-Writetest $<TARGET_FILE:apfsrw-writetest> -Fsck $<TARGET_FILE:winapfs-fsck>
			-Manifest ${TD}/${prefix}.${vol}.manifest.json -Image ${TD}/${img}
			-Work ${CMAKE_CURRENT_BINARY_DIR}/crash-${img} -Volume ${vol} -Points 6)
endforeach()
# the same crashes on a disk that ignores flushes must be caught
add_test(NAME crash_detects.basic.img
	COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_CURRENT_SOURCE_DIR}/crash_test.ps1
		-Writetest $<TARGET_FILE:apfsrw-writetest> -Fsck $<TARGET_FILE:winapfs-fsck>
		-Manifest ${TD}/basic.Basic.manifest.json -Image ${TD}/basic.img
		-Work ${CMAKE_CURRENT_BINARY_DIR}/crash-nobarrier-basic.img -Volume Basic -Points 3 -Modes nobarrier -ExpectBroken)

# disk full: fill the volume to ENOSPC, delete, check the space comes back,
# write again; then the image must be intact and fsck-clean
foreach(v "basic.img|basic|Basic" "casesensitive.img|casesensitive|CaseSens")
	string(REPLACE "|" ";" f "${v}")
	list(GET f 0 img)
	list(GET f 1 prefix)
	list(GET f 2 vol)
	set(work ${CMAKE_CURRENT_BINARY_DIR}/full-${img})
	add_test(NAME diskfull.${img}.${vol}
		COMMAND apfsrw-writetest --crash fill 0 0 ${TD}/${prefix}.${vol}.manifest.json ${TD}/${img} ${work} ${work}.model.json ${vol})
	set_tests_properties(diskfull.${img}.${vol} PROPERTIES FIXTURES_SETUP full-${img})
	add_test(NAME diskfull_verify.${img} COMMAND apfsrw-writetest --verify-crash ${TD}/${prefix}.${vol}.manifest.json ${work} ${work}.model.json ${vol})
	add_test(NAME diskfull_fsck.${img} COMMAND winapfs-fsck -c ${work})
	set_tests_properties(diskfull_verify.${img} diskfull_fsck.${img} PROPERTIES FIXTURES_REQUIRED full-${img})
endforeach()

# page cache under long sessions (user report 2026-11-17: big copies slowed to
# ~0 and hung after ~3 GB through one file): a 24 GiB sparse file read once
# (ever-new pages in one mapping), and a 600 MiB file re-read 8 times
set(pc_sparse ${CMAKE_CURRENT_BINARY_DIR}/pagecache-sparse.img)
set(pc_reread ${CMAKE_CURRENT_BINARY_DIR}/pagecache-reread.img)
add_test(NAME pagecache_prepare COMMAND ${CMAKE_COMMAND} -E copy ${TD}/basic.img ${pc_sparse})
add_test(NAME pagecache_prepare_reread COMMAND ${CMAKE_COMMAND} -E copy ${TD}/multivol.img ${pc_reread})
set_tests_properties(pagecache_prepare PROPERTIES FIXTURES_SETUP pagecache)
set_tests_properties(pagecache_prepare_reread PROPERTIES FIXTURES_SETUP pagecache_reread)
add_test(NAME pagecache.sparse COMMAND apfsrw-bench ${pc_sparse} 20480 0 0 --sparse 24)
add_test(NAME pagecache.reread COMMAND apfsrw-bench ${pc_reread} 20480 0 600 --reread 8)
# inode cache bound: 30,000 files in one session leave at most ~16K cached inodes
set(pc_small ${CMAKE_CURRENT_BINARY_DIR}/inodecache-small.img)
add_test(NAME inodecache_prepare COMMAND ${CMAKE_COMMAND} -E copy ${TD}/multivol.img ${pc_small})
set_tests_properties(inodecache_prepare PROPERTIES FIXTURES_SETUP inodecache)
add_test(NAME inodecache.small COMMAND apfsrw-bench ${pc_small} 20480 0 0 --small 30000)
set_tests_properties(inodecache.small PROPERTIES FIXTURES_REQUIRED inodecache FIXTURES_SETUP inodecache_done TIMEOUT 300)
add_test(NAME inodecache_fsck COMMAND winapfs-fsck ${pc_small})
set_tests_properties(inodecache_fsck PROPERTIES FIXTURES_REQUIRED inodecache_done)
add_test(NAME pagecache_fsck.sparse COMMAND winapfs-fsck ${pc_sparse})
add_test(NAME pagecache_fsck.reread COMMAND winapfs-fsck ${pc_reread})
set_tests_properties(pagecache.sparse PROPERTIES FIXTURES_REQUIRED pagecache FIXTURES_SETUP pagecache_done TIMEOUT 300)
set_tests_properties(pagecache.reread PROPERTIES FIXTURES_REQUIRED pagecache_reread FIXTURES_SETUP pagecache_reread_done TIMEOUT 300)
set_tests_properties(pagecache_fsck.sparse PROPERTIES FIXTURES_REQUIRED pagecache_done)
set_tests_properties(pagecache_fsck.reread PROPERTIES FIXTURES_REQUIRED pagecache_reread_done)

# the Windows write pattern: size first, then data (pending growth -> few extents)
if(TARGET winapfs)
	foreach(v "basic.img|Basic" "casesensitive.img|CaseSens")
		string(REPLACE "|" ";" f "${v}")
		list(GET f 0 img)
		list(GET f 1 vol)
		add_test(NAME prealloc.${img}
			COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_CURRENT_SOURCE_DIR}/prealloc_test.ps1
				-Winapfs $<TARGET_FILE:winapfs> -Fsck $<TARGET_FILE:winapfs-fsck>
				-Image ${TD}/${img} -Work ${CMAKE_CURRENT_BINARY_DIR}/prealloc-${img} -Volume ${vol})
		set_tests_properties(prealloc.${img} PROPERTIES RESOURCE_LOCK winfsp_mount)
	endforeach()

	# single-name directory queries (GetDirInfoByName): no full listing per create
	foreach(v "basic.img|Basic|" "casesensitive.img|CaseSens|-CaseSensitive")
		string(REPLACE "|" ";" f "${v}")
		list(GET f 0 img)
		list(GET f 1 vol)
		list(LENGTH f n)
		set(cs "")
		if(n GREATER 2)
			list(GET f 2 cs)
		endif()
		add_test(NAME dirquery.${img}
			COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_CURRENT_SOURCE_DIR}/dirquery_test.ps1
				-Winapfs $<TARGET_FILE:winapfs> -Fsck $<TARGET_FILE:winapfs-fsck>
				-Image ${TD}/${img} -Work ${CMAKE_CURRENT_BINARY_DIR}/dirquery-${img} -Volume ${vol} ${cs})
		set_tests_properties(dirquery.${img} PROPERTIES RESOURCE_LOCK winfsp_mount)
	endforeach()

	# multi-threaded Win32 concurrent write stress test (churn, compressed overwrite, ADS, queries)
	add_executable(winapfs-concurrent-test concurrent_write_test.cpp)
	target_compile_options(winapfs-concurrent-test PRIVATE /W4)
	target_link_options(winapfs-concurrent-test PRIVATE /ENTRY:wmainCRTStartup)
	set(conc_img ${CMAKE_CURRENT_BINARY_DIR}/conc-compressed.img)
	add_test(NAME concurrent_prepare.compressed.img COMMAND ${CMAKE_COMMAND} -E copy ${TD}/compressed.img ${conc_img})
	set_tests_properties(concurrent_prepare.compressed.img PROPERTIES FIXTURES_SETUP conc-prep)
	add_test(NAME concurrent.compressed.img
		COMMAND winapfs mount ${conc_img} * --rw
			--run "\"$<TARGET_FILE:winapfs-concurrent-test>\" %MOUNT%")
	set_tests_properties(concurrent.compressed.img PROPERTIES
		RESOURCE_LOCK winfsp_mount FIXTURES_REQUIRED conc-prep FIXTURES_SETUP conc-done TIMEOUT 180)
	add_test(NAME concurrent_fsck.compressed.img COMMAND winapfs-fsck -c ${conc_img})
	set_tests_properties(concurrent_fsck.compressed.img PROPERTIES FIXTURES_REQUIRED conc-done)
endif()

# W5: write safety and raw-device writes (always on copies)
if(TARGET winapfs)
	file(TO_NATIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}" BIN_NATIVE)

	# an image left unclean (crashed after a synced checkpoint) is refused for --rw,
	# mounts read-only, and is accepted with --force
	set(unclean ${CMAKE_CURRENT_BINARY_DIR}/unclean-basic.img)
	add_test(NAME w5_unclean_prepare
		COMMAND apfsrw-writetest --crash kill 1 1 ${TD}/basic.Basic.manifest.json ${TD}/basic.img ${unclean} ${unclean}.model.json Basic)
	# the crash exits 99 (non-zero: WILL_FAIL); a clean exit would leave a clean image
	set_tests_properties(w5_unclean_prepare PROPERTIES FIXTURES_SETUP w5unclean WILL_FAIL TRUE)
	add_test(NAME w5_unclean_refused COMMAND winapfs mount ${unclean} * --rw --run "cmd /c exit 0")
	set_tests_properties(w5_unclean_refused PROPERTIES WILL_FAIL TRUE)
	add_test(NAME w5_unclean_readonly COMMAND winapfs mount ${unclean} * --run "cmd /c exit 0")
	add_test(NAME w5_unclean_force COMMAND winapfs mount ${unclean} * --rw --force --run "cmd /c exit 0")
	set_tests_properties(w5_unclean_refused w5_unclean_readonly w5_unclean_force PROPERTIES
		FIXTURES_REQUIRED w5unclean RESOURCE_LOCK winfsp_mount)
	set_tests_properties(w5_unclean_force PROPERTIES DEPENDS "w5_unclean_refused;w5_unclean_readonly")

	# the full Win32 write test through the raw-device path (\.\C:\...\copy.img),
	# which needs --allow-disk-writes like a physical disk
	set(rawwork ${CMAKE_CURRENT_BINARY_DIR}/w5raw-basic.img)
	add_test(NAME w5_raw_prepare COMMAND ${CMAKE_COMMAND} -E copy ${TD}/basic.img ${rawwork})
	set_tests_properties(w5_raw_prepare PROPERTIES FIXTURES_SETUP w5rawprep)
	add_test(NAME w5_raw_write
		COMMAND winapfs mount "${BS}${BS}.${BS}${BIN_NATIVE}${BS}w5raw-basic.img" * --rw --allow-disk-writes --volume Basic
			--run "\"$<TARGET_FILE:apfsrw-writetest>\" --win32 %MOUNT% \"${TD}/basic.Basic.manifest.json\" \"${rawwork}\" \"${rawwork}.model.json\" Basic")
	set_tests_properties(w5_raw_write PROPERTIES RESOURCE_LOCK winfsp_mount FIXTURES_REQUIRED w5rawprep FIXTURES_SETUP w5raw)
	add_test(NAME w5_raw_verify COMMAND apfsrw-writetest --verify ${TD}/basic.Basic.manifest.json ${rawwork} ${rawwork}.model.json Basic)
	add_test(NAME w5_raw_fsck COMMAND winapfs-fsck -c ${rawwork})
	set_tests_properties(w5_raw_verify w5_raw_fsck PROPERTIES FIXTURES_REQUIRED w5raw)

	# mount-all --rw: the volumes of one container share one engine container
	# (separate ones would each have their own allocator and corrupt it)
	set(mvwork ${CMAKE_CURRENT_BINARY_DIR}/w5-multivol.img)
	add_test(NAME w5_multivol_prepare COMMAND ${CMAKE_COMMAND} -E copy ${TD}/multivol.img ${mvwork})
	set_tests_properties(w5_multivol_prepare PROPERTIES FIXTURES_SETUP w5mvprep)
	add_test(NAME w5_multivol_write
		COMMAND winapfs mount-all ${mvwork} --rw
			--run "powershell -NoProfile -Command \"foreach ($v in 'VolA','VolB','VolC') { $m = [Environment]::GetEnvironmentVariable('WINAPFS_MOUNT_' + $v); $d = New-Object byte[] 3000000; (New-Object Random 7).NextBytes($d); [IO.File]::WriteAllBytes($m + '${BS}' + $v + '.bin', $d) }\"")
	set_tests_properties(w5_multivol_write PROPERTIES RESOURCE_LOCK winfsp_mount FIXTURES_REQUIRED w5mvprep FIXTURES_SETUP w5mv)
	add_test(NAME w5_multivol_fsck COMMAND winapfs-fsck -c ${mvwork})
	add_test(NAME w5_multivol_read COMMAND winapfs-cli cat ${mvwork} /VolC.bin -v VolC)
	set_tests_properties(w5_multivol_fsck w5_multivol_read PROPERTIES FIXTURES_REQUIRED w5mv)
endif()

# W7: the auto-mount service's logic (service debug on images, settings in HKCU)
if(TARGET winapfs)
	add_test(NAME service.automount
		COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_CURRENT_SOURCE_DIR}/service_test.ps1
			-Winapfs $<TARGET_FILE:winapfs> -Fsck $<TARGET_FILE:winapfs-fsck> -Testdata ${TD} -WorkDir ${CMAKE_CURRENT_BINARY_DIR})
	set_tests_properties(service.automount PROPERTIES RESOURCE_LOCK winfsp_mount PASS_REGULAR_EXPRESSION "OK: auto-mount")
endif()

# dotfiles are hidden by default (as on macOS); --show-dotfiles turns it off
if(TARGET winapfs)
	add_test(NAME mount_dotfiles.hidden
		COMMAND winapfs mount ${TD}/basic.img * --run "powershell -NoProfile -Command \"if (((Get-Item -Force '%MOUNT%${BS}.hidden_dotfile').Attributes -band 'Hidden') -and -not ((Get-Item '%MOUNT%${BS}hello.txt').Attributes -band 'Hidden')) { exit 0 } else { exit 1 }\"")
	add_test(NAME mount_dotfiles.shown
		COMMAND winapfs mount ${TD}/basic.img * --show-dotfiles --run "powershell -NoProfile -Command \"if ((Get-Item -Force '%MOUNT%${BS}.hidden_dotfile').Attributes -band 'Hidden') { exit 1 } else { exit 0 }\"")
	set_tests_properties(mount_dotfiles.hidden mount_dotfiles.shown PROPERTIES RESOURCE_LOCK winfsp_mount)
endif()

# the service's control pipe through `winapfs ctl` (service debug on image copies)
if(TARGET winapfs)
	add_test(NAME service.control
		COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_CURRENT_SOURCE_DIR}/ctl_test.ps1
			-Winapfs $<TARGET_FILE:winapfs> -Fsck $<TARGET_FILE:winapfs-fsck> -Testdata ${TD} -WorkDir ${CMAKE_CURRENT_BINARY_DIR})
	set_tests_properties(service.control PROPERTIES RESOURCE_LOCK winfsp_mount PASS_REGULAR_EXPRESSION "OK: control pipe")
endif()
