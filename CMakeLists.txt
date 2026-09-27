cmake_minimum_required(VERSION 3.22)

project(ludifex
    VERSION 0.0.1
    DESCRIPTION "2D and 3D world library"
    HOMEPAGE_URL "https://cresmarmat-an.github.io/ludifex/"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Box2D and Box3D are C17.
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Installs ludifex, SDL3, Box2D, Box3D, and a package find_package(ludifex)
# reads. On by default only when ludifex is the project being built, not when it
# is a subdirectory of yours.
option(LUDIFEX_INSTALL "Generate ludifex's install rules and package" ${PROJECT_IS_TOP_LEVEL})

# Assimp reads every model format besides glTF: FBX, OBJ, Collada, 3DS,
# Blender, PLY, STL, X, and dozens more. Off, only .gltf and .glb load, and the
# build is a good deal quicker.
option(LUDIFEX_ASSIMP "Read FBX, OBJ, Collada, and the other formats Assimp knows, besides glTF" ON)

# ---------------------------------------------------------------------------
# Dependencies
#
# Box2D v3 and Box3D share an author and a design (data-oriented C, handle-based
# ids, sub-stepping solvers, and task-system hooks), so one wrapper serves both.
#
# SDL3 supplies the GPU device the renderer draws with. A world that never
# renders still simulates without one.
# ---------------------------------------------------------------------------

include(FetchContent)

# Every clone is made with long paths allowed. miniaudio's repository holds
# test files nested deep enough that, under a build directory of ordinary
# depth, Git for Windows would refuse to check them out without it.

set(BOX2D_SAMPLES OFF CACHE BOOL "" FORCE)
set(BOX2D_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(BOX2D_DOCS OFF CACHE BOOL "" FORCE)
set(BOX2D_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BOX2D_VALIDATE OFF CACHE BOOL "" FORCE)

set(BOX3D_SAMPLES OFF CACHE BOOL "" FORCE)
set(BOX3D_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(BOX3D_DOCS OFF CACHE BOOL "" FORCE)
set(BOX3D_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BOX3D_VALIDATE OFF CACHE BOOL "" FORCE)

set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL ${LUDIFEX_INSTALL} CACHE BOOL "" FORCE)

# SDL3 supplies the GPU device the world renders with. When opane is also in the
# build, FetchContent resolves this to the same copy, so there is one SDL.
FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.16
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
    GIT_CONFIG core.longpaths=true)

FetchContent_Declare(box2d
    GIT_REPOSITORY https://github.com/erincatto/box2d.git
    GIT_TAG v3.1.1
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
    GIT_CONFIG core.longpaths=true)

FetchContent_Declare(box3d
    GIT_REPOSITORY https://github.com/erincatto/box3d.git
    GIT_TAG v0.1.0
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
    GIT_CONFIG core.longpaths=true)

# cgltf is a single header with no CMake project of its own, so FetchContent
# only downloads it and the header is added to the include path.
FetchContent_Declare(cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG v1.15
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
    GIT_CONFIG core.longpaths=true)

# stb_image decodes textures. It is compiled with internal linkage, so a
# program that also links opane (which has its own copy) has no duplicate
# symbols.
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20
    GIT_SHALLOW FALSE
    GIT_PROGRESS TRUE
    GIT_CONFIG core.longpaths=true)

# miniaudio mixes world sound. Like stb_image it is compiled with internal
# linkage, and stb_vorbis's functions are renamed, so a program that also links
# opane (which has its own copies) links cleanly.
FetchContent_Declare(miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 0.11.25
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
    GIT_CONFIG core.longpaths=true
    # Only the header is used. miniaudio's repository has a CMake project of
    # its own, which would build and install libraries nothing links; naming a
    # folder with no CMakeLists.txt here downloads it without adding that.
    SOURCE_SUBDIR header-only)

FetchContent_MakeAvailable(SDL3 box2d box3d cgltf stb miniaudio)

# Assimp, for every other model format. Only its importers are built, and
# glTF's is left out, because cgltf reads glTF. It is a static library of its
# own, with its own copy of zlib for FBX's compressed arrays, and installs
# beside ludifex.
if(LUDIFEX_ASSIMP)
    set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
    set(ASSIMP_NO_EXPORT ON CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_GLTF_IMPORTER OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE)
    set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
    set(ASSIMP_INSTALL ${LUDIFEX_INSTALL} CACHE BOOL "" FORCE)
    set(ASSIMP_INSTALL_PDB OFF CACHE BOOL "" FORCE)
    set(ASSIMP_INJECT_DEBUG_POSTFIX ON CACHE BOOL "" FORCE)

    FetchContent_Declare(assimp
        GIT_REPOSITORY https://github.com/assimp/assimp.git
        GIT_TAG v6.0.5
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
        GIT_CONFIG core.longpaths=true)

    # Assimp builds a shared library unless told otherwise, and the setting
    # is not ludifex's to change for anything else in the build.
    set(LudifexBuildSharedLibs ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS OFF)
    FetchContent_MakeAvailable(assimp)
    set(BUILD_SHARED_LIBS ${LudifexBuildSharedLibs})
endif()

# Box3D selects the static C runtime for its own builds. Left alone that mixes
# /MT into a /MD program, which the linker only warns about but which means two
# heaps in one process. Forcing the dynamic runtime keeps one heap.
if(MSVC)
    set_property(TARGET box3d PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# Debug and release builds install side by side.
set_target_properties(box2d box3d PROPERTIES DEBUG_POSTFIX d)

# Box3D's cone limit, corrected so a cone joint does not gain energy; see the
# module.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchBox3D.cmake)
ludifex_patch_box3d(${box3d_SOURCE_DIR})

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

add_library(ludifex STATIC
    src/Animation.cpp
    src/Assets.cpp
    src/Audio.cpp
    src/Backend.cpp
    src/Character2D.cpp
    src/Character3D.cpp
    src/Clusters.cpp
    src/Common.cpp
    src/Hierarchy3D.cpp
    src/Host.cpp
    src/Images.cpp
    src/Joints2D.cpp
    src/Joints3D.cpp
    src/Jobs.cpp
    src/Loading.cpp
    src/Math.cpp
    src/Models.cpp
    src/ModelsAssimp.cpp
    src/Native.cpp
    src/Profile.cpp
    src/Quality.cpp
    src/RayScene.cpp
    src/Render3D.cpp
    src/RenderEffects.cpp
    src/RenderRays.cpp
    src/Serialize3D.cpp
    src/RenderMeshes.cpp
    src/RenderPasses.cpp
    src/World2D.cpp
    src/World3D.cpp
    src/WorldMaterials.cpp
    src/WorldTextures.cpp)

add_library(ludifex::ludifex ALIAS ludifex)

# Shaders are compiled to bytecode at build time and embedded, so a program
# built against ludifex has no runtime shader files to ship.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompileShaders.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchMiniaudio.cmake)

# A copy of miniaudio.h with its gain smoother corrected; see the module.
ludifex_patch_miniaudio(${miniaudio_SOURCE_DIR} LudifexMiniaudioDir)

set(LudifexShaderDir ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(LudifexWorldIncludes ${LudifexShaderDir}/world_material.hlsli)

# The world pass, the shadow map, the depth-and-motion prepass, and debug lines.
foreach(Stage
        "VertexMain;vs_6_0;LudifexWorldVertex"
        "FragmentMain;ps_6_0;LudifexWorldFragment"
        "ShadowVertexMain;vs_6_0;LudifexShadowVertex"
        "ShadowFragmentMain;ps_6_0;LudifexShadowFragment"
        "MotionVertexMain;vs_6_0;LudifexMotionVertex"
        "MotionFragmentMain;ps_6_0;LudifexMotionFragment"
        "LineVertexMain;vs_6_0;LudifexLineVertex"
        "LineFragmentMain;ps_6_0;LudifexLineFragment")
    list(GET Stage 0 Entry)
    list(GET Stage 1 Profile)
    list(GET Stage 2 Symbol)
    ludifex_add_shader(ludifex ${LudifexShaderDir}/world.hlsl ${Entry} ${Profile} ${Symbol}
        ${LudifexWorldIncludes})
endforeach()

# The full-screen passes.
foreach(Stage
        "FullscreenVertexMain;vs_6_0;LudifexFullscreenVertex"
        "BackdropFragmentMain;ps_6_0;LudifexBackdropFragment"
        "FxaaFragmentMain;ps_6_0;LudifexFxaaFragment"
        "TaaFragmentMain;ps_6_0;LudifexTaaFragment")
    list(GET Stage 0 Entry)
    list(GET Stage 1 Profile)
    list(GET Stage 2 Symbol)
    ludifex_add_shader(ludifex ${LudifexShaderDir}/post.hlsl ${Entry} ${Profile} ${Symbol})
endforeach()

# The sky, ambient occlusion, bloom, exposure, tone mapping, and scaling.
foreach(Stage
        "SkyFragmentMain;ps_6_0;LudifexSkyFragment"
        "AmbientOcclusionFragmentMain;ps_6_0;LudifexOcclusionFragment"
        "OcclusionBlurFragmentMain;ps_6_0;LudifexOcclusionBlurFragment"
        "BloomPrefilterFragmentMain;ps_6_0;LudifexBloomPrefilterFragment"
        "BloomDownFragmentMain;ps_6_0;LudifexBloomDownFragment"
        "BloomUpFragmentMain;ps_6_0;LudifexBloomUpFragment"
        "LuminanceFragmentMain;ps_6_0;LudifexLuminanceFragment"
        "AdaptFragmentMain;ps_6_0;LudifexAdaptFragment"
        "ToneMapFragmentMain;ps_6_0;LudifexToneMapFragment"
        "UpscaleFragmentMain;ps_6_0;LudifexUpscaleFragment")
    list(GET Stage 0 Entry)
    list(GET Stage 1 Profile)
    list(GET Stage 2 Symbol)
    ludifex_add_shader(ludifex ${LudifexShaderDir}/effects.hlsl ${Entry} ${Profile} ${Symbol}
        ${LudifexShaderDir}/sky_model.hlsli)
endforeach()

# Ray tracing and path tracing, on the compute units.
set(LudifexRayIncludes
    ${LudifexShaderDir}/ray_common.hlsli
    ${LudifexShaderDir}/ray_uniforms.hlsli
    ${LudifexShaderDir}/sky_model.hlsli)
foreach(Stage
        "rays.hlsl;RayHybridMain;LudifexRayHybrid"
        "ray_denoise.hlsl;RayTemporalMain;LudifexRayTemporal"
        "ray_denoise.hlsl;RayFilterMain;LudifexRayFilter"
        "pathtrace.hlsl;PathTraceMain;LudifexPathTrace")
    list(GET Stage 0 Source)
    list(GET Stage 1 Entry)
    list(GET Stage 2 Symbol)
    ludifex_add_shader(ludifex ${LudifexShaderDir}/${Source} ${Entry} cs_6_0 ${Symbol} ${LudifexRayIncludes})
endforeach()

# A material created from an .hlsl file is compiled at runtime, to whichever
# format the device runs. The includes it reads travel inside the library, so
# they always match the build that reads its output; the compilers are found
# when first needed, starting with the ones this build used. Materials built
# from bytecode need neither, which is the path a shipped program takes.
ludifex_embed_file(ludifex ${LudifexShaderDir}/world_material.hlsli LudifexWorldMaterialInclude)
ludifex_embed_file(ludifex ${LudifexShaderDir}/world_post.hlsli LudifexWorldPostInclude)
target_compile_definitions(ludifex PRIVATE
    LUDIFEX_DXC_PATH="${LUDIFEX_DXC}"
    LUDIFEX_DXC_SPIRV_PATH="${LUDIFEX_DXC_SPIRV}"
    LUDIFEX_SPIRV_CROSS_PATH="${LUDIFEX_SPIRV_CROSS}")

# Debug and release builds install side by side.
set_target_properties(ludifex PROPERTIES DEBUG_POSTFIX d)

# <ludifex/version.h>, from the version above, so the two cannot disagree.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/include/ludifex/version.h @ONLY)

target_include_directories(ludifex
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${cgltf_SOURCE_DIR}
        ${stb_SOURCE_DIR}
        ${LudifexMiniaudioDir}
        ${miniaudio_SOURCE_DIR})

# PUBLIC so that <ludifex/native.h> resolves box2d and box3d headers for
# consumers who take the escape hatch.
target_link_libraries(ludifex PUBLIC box2d::box2d box3d::box3d SDL3::SDL3)

if(LUDIFEX_ASSIMP)
    target_link_libraries(ludifex PRIVATE assimp::assimp)
    target_compile_definitions(ludifex PRIVATE LUDIFEX_HAS_ASSIMP=1)
endif()

target_compile_features(ludifex PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(ludifex PRIVATE /W4 /permissive-)
else()
    target_compile_options(ludifex PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Installation
#
# cmake --install puts ludifex, SDL3, Box2D, Box3D, and a package in the
# prefix, so a project elsewhere can use find_package(ludifex). Install the
# Debug and the Release configurations into the same prefix: the libraries
# carry a d suffix in Debug. Box2D and Box3D install themselves with their
# own packages, which ludifex's names as dependencies.
# ---------------------------------------------------------------------------

if(LUDIFEX_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    set(LudifexPackageDir "${CMAKE_INSTALL_LIBDIR}/cmake/ludifex")

    install(TARGETS ludifex
        EXPORT ludifexTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ludifex
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/generated/include/ludifex/version.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ludifex)

    install(EXPORT ludifexTargets
        NAMESPACE ludifex::
        DESTINATION ${LudifexPackageDir})

    configure_package_config_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/ludifexConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/ludifexConfig.cmake
        INSTALL_DESTINATION ${LudifexPackageDir})

    # Before 1.0, a minor version may change the interface, so only the same
    # major and minor version is taken as compatible.
    write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/ludifexConfigVersion.cmake
        COMPATIBILITY SameMinorVersion)

    install(FILES
            ${CMAKE_CURRENT_BINARY_DIR}/ludifexConfig.cmake
            ${CMAKE_CURRENT_BINARY_DIR}/ludifexConfigVersion.cmake
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompileShaders.cmake
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedBinary.cmake
        DESTINATION ${LudifexPackageDir})

    # What ludifex_add_material compiles against.
    install(FILES
            ${LudifexShaderDir}/world_material.hlsli
            ${LudifexShaderDir}/world_post.hlsli
        DESTINATION ${CMAKE_INSTALL_DATADIR}/ludifex/shaders)

    install(FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/README.md
            ${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md
            ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
            ${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR})

    # cpack -G ZIP -C "Debug;Release" makes the release archive.
    if(PROJECT_IS_TOP_LEVEL)
        set(CPACK_PACKAGE_NAME ludifex)
        set(CPACK_PACKAGE_VENDOR "Cresmar Mat-an")
        set(CPACK_PACKAGE_HOMEPAGE_URL ${PROJECT_HOMEPAGE_URL})
        set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
        set(CPACK_PACKAGE_FILE_NAME "ludifex-${PROJECT_VERSION}-windows-x64")
        set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE)
        set(CPACK_GENERATOR ZIP)
        include(CPack)
    endif()
endif()
