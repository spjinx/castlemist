# =============================================================================
# castlemist unit tests
#
# One executable per layer, so a failure names the layer immediately and a test
# binary only links what it is actually testing -- a `core` failure can never be
# caused by a Direct3D device that would not create.
#
#   ctest --preset debug
#   build/debug/bin/cm_test_native.exe method0   # run one group directly
#
# Tests that need a real Gw2.dat, a sound device or a GPU skip themselves rather
# than fail; see docs/testing.md.
# =============================================================================

add_library(castlemist_test_framework STATIC framework/test_main.cpp)
target_include_directories(castlemist_test_framework
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/framework")

# -----------------------------------------------------------------------------
# castlemist_add_test(<layer> SOURCES <files...> DEPS <targets...>
#                             [PRIVATE_HEADERS <dirs...>])
#
# PRIVATE_HEADERS grants access to a layer's internal headers (internal.h,
# detail/state.h, detail/app_state.h) for the tests that exercise the seams
# between a layer's own files.
# -----------------------------------------------------------------------------
function(castlemist_add_test layer)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPS;PRIVATE_HEADERS" ${ARGN})
    set(_target cm_test_${layer})

    add_executable(${_target} ${ARG_SOURCES})
    target_link_libraries(${_target} PRIVATE castlemist_test_framework ${ARG_DEPS})
    castlemist_link_runtime(${_target})
    if (ARG_PRIVATE_HEADERS)
        target_include_directories(${_target} PRIVATE ${ARG_PRIVATE_HEADERS})
    endif()
    set_target_properties(${_target} PROPERTIES FOLDER "castlemist/tests")

    add_test(NAME ${layer} COMMAND ${_target})
    set_tests_properties(${layer} PROPERTIES TIMEOUT 120)
endfunction()

castlemist_add_test(native
    SOURCES test_native_method0.cpp test_native_amat.cpp test_native_pose.cpp
            test_native_dat_lookup.cpp test_native_materials.cpp
    DEPS    castlemist::native)

castlemist_add_test(core
    SOURCES test_core_bytes.cpp
            test_core_packfile.cpp
            test_core_text.cpp
            test_core_env.cpp
    DEPS    castlemist::core)

castlemist_add_test(format
    SOURCES test_format_dds.cpp
            test_format_chat_link.cpp
            test_format_strs.cpp
            test_format_content_schema.cpp
            test_format_composite.cpp
    DEPS    castlemist::format)

castlemist_add_test(character
    SOURCES test_character_key_store.cpp
            test_character_http.cpp
            test_character_api.cpp
            test_character_resolver.cpp
            test_character_manifest_json.cpp
            test_character_look_store.cpp
    DEPS    castlemist::character ext::json)
target_compile_definitions(cm_test_character PRIVATE
    CM_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

castlemist_add_test(db
    SOURCES test_db_index.cpp test_db_type_names.cpp test_db_data_status.cpp
    DEPS    castlemist::db ext::sqlite)

castlemist_add_test(media
    SOURCES test_media_audio.cpp
    DEPS    castlemist::media)

castlemist_add_test(extract
    SOURCES test_extract_classify.cpp
            test_extract_summary.cpp
            test_extract_material_slots.cpp
            test_extract_model_load.cpp
    DEPS    castlemist::extract
    PRIVATE_HEADERS "${PROJECT_SOURCE_DIR}/src/extract")

# End-to-end extraction against a real Gw2.dat. Skips itself when the archive is
# absent, so it is safe to leave enabled; GW2_TEST_DAT overrides the location.
castlemist_add_test(dat
    SOURCES test_extract_dat.cpp test_exportgltf_world_dat.cpp
    DEPS    castlemist::extract castlemist::exportgltf castlemist::world ext::json ext::stb)
set_tests_properties(dat PROPERTIES TIMEOUT 600)
# world_export_dat.spirit_vale runs tools/world/validate_manifest.py from the source tree.
target_compile_definitions(cm_test_dat PRIVATE CM_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
# long_path.h's remove_tree: the Spirit Vale export now holds paths past MAX_PATH.
target_include_directories(cm_test_dat PRIVATE "${PROJECT_SOURCE_DIR}/src/exportgltf")

castlemist_add_test(world
    SOURCES test_world.cpp test_world_walk.cpp test_world_stream.cpp test_world_materials.cpp test_world_cells.cpp test_world_bake.cpp
            test_world_zones.cpp
    DEPS    castlemist::world)

# World-layer tests against a real Gw2.dat; skip when it is absent.
castlemist_add_test(world_dat
    SOURCES test_world_dat.cpp
    DEPS    castlemist::world castlemist::extract)
set_tests_properties(world_dat PROPERTIES TIMEOUT 900)
# T3D reference numbers (tools/world/README.md), read in place.
target_compile_definitions(cm_test_world_dat PRIVATE
    CM_WORLD_REF_DIR="${PROJECT_SOURCE_DIR}/tests/world_ref")

castlemist_add_test(sim
    SOURCES test_sim_cloth.cpp
    DEPS    castlemist::sim)

castlemist_add_test(exportgltf
    SOURCES test_exportgltf_writer.cpp test_exportgltf_vrchat.cpp test_exportgltf_sky.cpp
            test_exportgltf_world.cpp
    DEPS    castlemist::exportgltf ext::json
    PRIVATE_HEADERS "${PROJECT_SOURCE_DIR}/src/exportgltf")
target_compile_definitions(cm_test_exportgltf PRIVATE CM_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

# The world export's test fixture writer (sub-project 4): cm_world_export_fixture
# <dir> [--probe-only] writes probe.glb only, or (default) the whole synthetic
# export (tests/world_export_synthetic.h) into <dir> for the Blender ctests below.
add_executable(cm_world_export_fixture tools/world_export_fixture.cpp)
target_link_libraries(cm_world_export_fixture PRIVATE castlemist::exportgltf)
castlemist_link_runtime(cm_world_export_fixture)
set_target_properties(cm_world_export_fixture PROPERTIES FOLDER "castlemist/tests")

add_test(NAME world_fixture_probe
         COMMAND cm_world_export_fixture "${CMAKE_BINARY_DIR}/world_fixture" --probe-only)
set_tests_properties(world_fixture_probe PROPERTIES FIXTURES_SETUP world_probe)

# Headless Blender checks of the world export (spec D6 "Blender", "Frame probe";
# label `blender`). castlemist_world.py needs Blender 4.1 or newer (Mesh.corner_normals),
# so older versions are refused here. Blender is looked up in $ENV{BLENDER} (the
# blender.exe path), then PATH, then the newest "Blender Foundation/Blender <v>"
# under Program Files; with none of 4.1+ the tests are not added.
set(_cm_blender_hints "")
if (DEFINED ENV{BLENDER} AND EXISTS "$ENV{BLENDER}")
    get_filename_component(_cm_blender_env_dir "$ENV{BLENDER}" DIRECTORY)
    list(APPEND _cm_blender_hints "${_cm_blender_env_dir}")
endif()
file(GLOB _cm_blender_dirs "C:/Program Files/Blender Foundation/Blender *")
list(SORT _cm_blender_dirs COMPARE NATURAL ORDER DESCENDING)
find_program(CASTLEMIST_BLENDER NAMES blender blender.exe HINTS ${_cm_blender_hints} PATHS ${_cm_blender_dirs})
set(_cm_blender_ok OFF)
if (CASTLEMIST_BLENDER)
    execute_process(COMMAND "${CASTLEMIST_BLENDER}" --version OUTPUT_VARIABLE _cm_blender_ver
                    ERROR_QUIET TIMEOUT 120 RESULT_VARIABLE _cm_blender_rc)
    string(REGEX MATCH "Blender ([0-9]+)\\.([0-9]+)" _cm_blender_m "${_cm_blender_ver}")
    if (_cm_blender_m AND "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}" VERSION_GREATER_EQUAL 4.1)
        set(_cm_blender_ok ON)
        message(STATUS "Blender ${CMAKE_MATCH_1}.${CMAKE_MATCH_2} for the world-export tests: ${CASTLEMIST_BLENDER}")
    else()
        message(STATUS "Blender at ${CASTLEMIST_BLENDER} is not 4.1 or newer ('${_cm_blender_m}'); Blender tests skipped")
    endif()
else()
    message(STATUS "Blender not found (set BLENDER to blender.exe); Blender tests skipped")
endif()
if (_cm_blender_ok)
    set(_cm_world_py "${PROJECT_SOURCE_DIR}/tools/blender/castlemist_world.py")
    set(_cm_check_fbx "${PROJECT_SOURCE_DIR}/tests/blender/check_fbx.py")
    set(_cm_blender -b --factory-startup -P)
    add_test(NAME blender_probe_glb
             COMMAND "${CASTLEMIST_BLENDER}" ${_cm_blender} "${_cm_world_py}"
                     -- --check-probe "${CMAKE_BINARY_DIR}/world_fixture/probe.glb")
    # One Blender session: convert every .glb in the fixture, then check the .fbx.
    add_test(NAME blender_probe_fbx
             COMMAND "${CASTLEMIST_BLENDER}" ${_cm_blender} "${_cm_world_py}"
                     -- --to-fbx "${CMAKE_BINARY_DIR}/world_fixture"
                        --check-probe "${CMAKE_BINARY_DIR}/world_fixture/probe.fbx")
    set_tests_properties(blender_probe_glb blender_probe_fbx PROPERTIES
        FIXTURES_REQUIRED world_probe
        PASS_REGULAR_EXPRESSION "PROBE .* negScale=0 OK"
        FAIL_REGULAR_EXPRESSION "PROBE .* FAIL|Traceback"
        TIMEOUT 300
        LABELS blender
        RESOURCE_LOCK world_fixture)

    # The skinned probe (Task 11): an armature model, as the world export writes
    # one, through the same bake and FBX step as every model.
    add_test(NAME world_fixture_skinned_probe
             COMMAND cm_world_export_fixture "${CMAKE_BINARY_DIR}/skinned_probe" --skinned-probe)
    set_tests_properties(world_fixture_skinned_probe PROPERTIES
        FIXTURES_SETUP skinned_probe RESOURCE_LOCK skinned_probe)
    add_test(NAME blender_skinned_probe_glb
             COMMAND "${CASTLEMIST_BLENDER}" ${_cm_blender} "${_cm_world_py}"
                     -- --check-skinned-probe "${CMAKE_BINARY_DIR}/skinned_probe/skinned_probe.glb")
    add_test(NAME blender_skinned_probe_fbx
             COMMAND "${CASTLEMIST_BLENDER}" ${_cm_blender} "${_cm_world_py}"
                     -- --to-fbx "${CMAKE_BINARY_DIR}/skinned_probe"
                        --check-skinned-probe "${CMAKE_BINARY_DIR}/skinned_probe/skinned_probe.fbx")
    set_tests_properties(blender_skinned_probe_glb blender_skinned_probe_fbx PROPERTIES
        FIXTURES_REQUIRED skinned_probe
        PASS_REGULAR_EXPRESSION "SKINNED_PROBE .* negScale=0 OK"
        FAIL_REGULAR_EXPRESSION "SKINNED_PROBE .* FAIL|Traceback"
        TIMEOUT 300
        LABELS blender
        RESOURCE_LOCK skinned_probe)

    # The importer on the full synthetic export (spec "Testing", Blender).
    add_test(NAME blender_world_import
             COMMAND "${CASTLEMIST_BLENDER}" ${_cm_blender} "${_cm_world_py}"
                     -- --import "${CMAKE_BINARY_DIR}/world_fixture" --verify)
    add_test(NAME blender_world_import_scaled
             COMMAND "${CASTLEMIST_BLENDER}" ${_cm_blender} "${_cm_world_py}"
                     -- --import "${CMAKE_BINARY_DIR}/world_fixture" --scale 0.0254 --no-colliders --verify)
    # 4 props; foliage (world-foliage D8): one geometry-nodes object per (cell, model), 4 records.
    set_tests_properties(blender_world_import blender_world_import_scaled PROPERTIES
        PASS_REGULAR_EXPRESSION "INSTANCES 4.*FOLIAGE_OBJECTS 2.*FOLIAGE_INSTANCES 4.*VERIFY OK")
    add_test(NAME blender_world_import_extras
             COMMAND "${CASTLEMIST_BLENDER}" ${_cm_blender} "${PROJECT_SOURCE_DIR}/tests/blender/check_world_extras.py"
                     -- "${_cm_world_py}" "${CMAKE_BINARY_DIR}/world_fixture" "${CMAKE_BINARY_DIR}/world_extras")
    set_tests_properties(blender_world_import_extras PROPERTIES PASS_REGULAR_EXPRESSION "EXTRAS OK")
    # Review I3: --to-fbx alone records the .fbx files in the manifest (no gw2dat_cli needed).
    add_test(NAME blender_to_fbx_manifest
             COMMAND "${CASTLEMIST_BLENDER}" ${_cm_blender} "${_cm_world_py}"
                     -- --to-fbx "${CMAKE_BINARY_DIR}/world_fixture")
    set_tests_properties(blender_to_fbx_manifest PROPERTIES
        PASS_REGULAR_EXPRESSION "TO_FBX_MANIFEST fbxSet=5")   # probe, cell, models 101, 102, 103
    set_tests_properties(blender_world_import blender_world_import_scaled blender_world_import_extras
                         blender_to_fbx_manifest PROPERTIES
        FIXTURES_REQUIRED world_fixture
        FAIL_REGULAR_EXPRESSION "VERIFY FAIL|EXTRAS FAIL|Traceback"
        TIMEOUT 300
        LABELS blender
        RESOURCE_LOCK world_fixture)

    # --to-fbx through ripper::convert_world_to_fbx (gw2dat_cli world-fbx), then the
    # .fbx files: the animated model keeps its clip, and the textures resolve.
    if (CASTLEMIST_BUILD_TOOLS)
        add_test(NAME blender_world_to_fbx
                 COMMAND gw2dat_cli world-fbx --dir "${CMAKE_BINARY_DIR}/world_fixture")
        set_tests_properties(blender_world_to_fbx PROPERTIES
            FIXTURES_REQUIRED world_fixture FIXTURES_SETUP world_fbx
            PASS_REGULAR_EXPRESSION "\"fbxSet\":5,"
            TIMEOUT 600)
        add_test(NAME blender_world_fbx_clip
                 COMMAND "${CASTLEMIST_BLENDER}" ${_cm_blender} "${_cm_check_fbx}"
                         -- "${CMAKE_BINARY_DIR}/world_fixture/models/102/102.fbx")
        set_tests_properties(blender_world_fbx_clip PROPERTIES PASS_REGULAR_EXPRESSION "ACTIONS .*TestClip")
        add_test(NAME blender_world_fbx_textures
                 COMMAND "${CASTLEMIST_BLENDER}" ${_cm_blender} "${_cm_check_fbx}"
                         -- "${CMAKE_BINARY_DIR}/world_fixture/models/101/101.fbx")
        set_tests_properties(blender_world_fbx_textures PROPERTIES
            PASS_REGULAR_EXPRESSION "IMAGES 1 missing=0.*NEGSCALE 0 INWARD 0")
        set_tests_properties(blender_world_fbx_clip blender_world_fbx_textures PROPERTIES
            FIXTURES_REQUIRED "world_fixture;world_fbx")
        set_tests_properties(blender_world_to_fbx blender_world_fbx_clip blender_world_fbx_textures PROPERTIES
            FAIL_REGULAR_EXPRESSION "Traceback"
            TIMEOUT 600
            LABELS blender
            RESOURCE_LOCK world_fixture)
    endif()
endif()

# Dependency-free manifest validator and Unity dry run (tools/world/validate_manifest.py)
# on the full synthetic export. The setup test rewrites <build>/world_fixture, so it
# shares the world_fixture lock with the Blender checks above.
find_program(CASTLEMIST_PYTHON NAMES python python3)
if (CASTLEMIST_PYTHON)
    set(_cm_vm "${PROJECT_SOURCE_DIR}/tools/world/validate_manifest.py")
    add_test(NAME world_fixture_export
             COMMAND cm_world_export_fixture "${CMAKE_BINARY_DIR}/world_fixture")
    set_tests_properties(world_fixture_export PROPERTIES
        FIXTURES_SETUP world_fixture RESOURCE_LOCK world_fixture)
    add_test(NAME world_manifest_validate
             COMMAND "${CASTLEMIST_PYTHON}" -I "${_cm_vm}" "${CMAKE_BINARY_DIR}/world_fixture")
    add_test(NAME world_manifest_plan
             COMMAND "${CASTLEMIST_PYTHON}" -I "${_cm_vm}" "${CMAKE_BINARY_DIR}/world_fixture" --plan)
    add_test(NAME world_manifest_selftest
             COMMAND "${CASTLEMIST_PYTHON}" -I "${_cm_vm}" "${CMAKE_BINARY_DIR}/world_fixture" --self-test)
    add_test(NAME world_manifest_unit
             COMMAND "${CASTLEMIST_PYTHON}" -I "${PROJECT_SOURCE_DIR}/tools/world/test_validate_manifest.py"
                     "${CMAKE_BINARY_DIR}/world_fixture")
    set_tests_properties(world_manifest_validate world_manifest_plan world_manifest_selftest world_manifest_unit
        PROPERTIES FIXTURES_REQUIRED world_fixture RESOURCE_LOCK world_fixture TIMEOUT 120)
    set_tests_properties(world_manifest_validate PROPERTIES PASS_REGULAR_EXPRESSION "^OK")
    set_tests_properties(world_manifest_plan PROPERTIES PASS_REGULAR_EXPRESSION
        "PLAN materials=5 prefabs=3 controllers=1 cells=1 instances=4 colliders=1 terrainColliders=1 water=1 lights=1 skybox=0 foliageTrees=2")   # trees: the collider tree and the geometry-only collider
    set_tests_properties(world_manifest_selftest PROPERTIES PASS_REGULAR_EXPRESSION "SELFTEST OK")
endif()

castlemist_add_test(ripper
    SOURCES test_ripper_dye.cpp test_ripper_shader_dye.cpp test_ripper_atlas.cpp test_ripper_export.cpp test_ripper_skeleton.cpp test_ripper_assemble.cpp test_ripper_look.cpp
    DEPS    castlemist::ripper ext::json ext::stb)
target_compile_definitions(cm_test_ripper PRIVATE
    CM_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

castlemist_add_test(render
    SOURCES test_render_math.cpp test_render_preview_rig.cpp test_render_world_view.cpp
            test_render_world_materials.cpp test_render_world_game.cpp
            test_render_world_sky.cpp
    DEPS    castlemist::render
    PRIVATE_HEADERS "${PROJECT_SOURCE_DIR}/src/render")

castlemist_add_test(ui
    SOURCES test_ui_layout.cpp test_ui_content_map_service.cpp test_ui_character_state.cpp
            test_ui_content_links.cpp test_ui_data_status.cpp
            test_ui_map_window.cpp
    DEPS    castlemist::ui
    PRIVATE_HEADERS "${PROJECT_SOURCE_DIR}/src/ui")
