# Offline shader compilation for ludifex.
#
# This mirrors opane's module rather than sharing one, because the two libraries
# are independent: either can be used without the other being on disk.
#
# SDL's GPU API takes each backend's own shader format: DXIL for Direct3D 12,
# SPIR-V for Vulkan, MSL for Metal. ludifex's shaders are written once, in HLSL,
# and compiled here to every format the platform being built for can run:
#
#   Windows  DXIL, with the Windows SDK's dxc; and SPIR-V, with the Vulkan
#            SDK's dxc, when the Vulkan SDK is installed
#   macOS    MSL: SPIR-V from the Vulkan SDK's dxc, translated by its
#            spirv-cross
#   Linux    SPIR-V
#
# Every result is embedded in the binary, so no compiler travels with the
# program. A backend whose format a build lacks cannot be chosen at runtime.
#
# LUDIFEX_SHADER_FORMATS, a list drawn from DXIL, SPIRV, and MSL, overrides the
# choice; every format it names must then be buildable. The compilers are
# found on their own and can be named by hand: LUDIFEX_DXC (DXIL),
# LUDIFEX_DXC_SPIRV (SPIR-V and MSL), LUDIFEX_SPIRV_CROSS (MSL).

set(LUDIFEX_SHADER_EMBED_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/EmbedBinary.cmake")

set(LUDIFEX_SHADER_FORMATS "" CACHE STRING
    "Shader formats to build, from DXIL, SPIRV, and MSL. Empty builds every format this platform runs.")

# The Windows SDK's dxc, which signs its output with the dxil.dll beside it.
# Direct3D 12 refuses an unsigned shader, so a dxc without that file (such as
# the Vulkan SDK's) cannot make DXIL.
function(_ludifex_find_dxil_compiler)
    if(LUDIFEX_DXC)
        return()
    endif()

    set(SearchHints "")
    file(GLOB KitDirectories "C:/Program Files (x86)/Windows Kits/10/bin/10.*")
    if(KitDirectories)
        list(SORT KitDirectories)
        list(REVERSE KitDirectories)
        foreach(Kit IN LISTS KitDirectories)
            list(APPEND SearchHints "${Kit}/x64")
        endforeach()
    endif()

    find_program(LUDIFEX_DXC NAMES dxc HINTS ${SearchHints} NO_SYSTEM_ENVIRONMENT_PATH
        DOC "dxc for DXIL, the Direct3D 12 format; dxil.dll must be beside it")
    if(LUDIFEX_DXC)
        get_filename_component(Directory "${LUDIFEX_DXC}" DIRECTORY)
        if(NOT EXISTS "${Directory}/dxil.dll")
            message(STATUS "ludifex: ignoring ${LUDIFEX_DXC} for DXIL, which has no dxil.dll to sign it with")
            unset(LUDIFEX_DXC CACHE)
        endif()
    endif()
endfunction()

# A dxc built with the SPIR-V backend, which the Vulkan SDK's is and the
# Windows SDK's is not. Tried on a one-line shader before it is trusted.
function(_ludifex_find_spirv_compiler)
    if(LUDIFEX_DXC_SPIRV AND LUDIFEX_DXC_SPIRV STREQUAL LUDIFEX_DXC_SPIRV_CHECKED)
        return()
    endif()

    find_program(LUDIFEX_DXC_SPIRV NAMES dxc
        HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
        DOC "dxc with SPIR-V output, for Vulkan and Metal; the Vulkan SDK has one")
    if(NOT LUDIFEX_DXC_SPIRV)
        return()
    endif()

    set(Probe "${CMAKE_BINARY_DIR}/ludifex_spirv_probe.hlsl")
    file(WRITE "${Probe}" "float4 main() : SV_Target { return 0; }\n")
    execute_process(COMMAND "${LUDIFEX_DXC_SPIRV}" -spirv -T ps_6_0 -E main -Fo "${Probe}.spv" "${Probe}"
                    RESULT_VARIABLE Result OUTPUT_QUIET ERROR_QUIET)
    if(Result EQUAL 0)
        set(LUDIFEX_DXC_SPIRV_CHECKED "${LUDIFEX_DXC_SPIRV}" CACHE INTERNAL "")
    else()
        message(STATUS "ludifex: ignoring ${LUDIFEX_DXC_SPIRV} for SPIR-V, which was built without it")
        unset(LUDIFEX_DXC_SPIRV CACHE)
    endif()
endfunction()

function(_ludifex_find_spirv_cross)
    if(LUDIFEX_SPIRV_CROSS)
        return()
    endif()
    find_program(LUDIFEX_SPIRV_CROSS NAMES spirv-cross
        HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
        DOC "spirv-cross, which turns SPIR-V into MSL for Metal; the Vulkan SDK has one")
endfunction()

# Sets OutVar to the formats this configure builds, deciding them the first
# time it is asked.
function(ludifex_shader_formats OutVar)
    get_property(Formats GLOBAL PROPERTY LUDIFEX_SHADER_FORMATS_BUILT)
    if(Formats)
        set(${OutVar} "${Formats}" PARENT_SCOPE)
        return()
    endif()

    set(Strict FALSE)
    if(LUDIFEX_SHADER_FORMATS)
        set(Wanted ${LUDIFEX_SHADER_FORMATS})
        set(Strict TRUE)
    elseif(WIN32)
        set(Wanted DXIL SPIRV)
    elseif(APPLE)
        set(Wanted MSL)
    else()
        set(Wanted SPIRV)
    endif()

    set(Formats "")
    set(AllMissing "")
    foreach(Format IN LISTS Wanted)
        set(Missing "")
        if(Format STREQUAL "DXIL")
            _ludifex_find_dxil_compiler()
            if(NOT LUDIFEX_DXC)
                set(Missing "DXIL, for Direct3D 12, needs the Windows SDK's dxc.exe with dxil.dll beside it; "
                            "install the Windows SDK or set -DLUDIFEX_DXC=<path to dxc.exe>.")
            endif()
        elseif(Format STREQUAL "SPIRV")
            _ludifex_find_spirv_compiler()
            if(NOT LUDIFEX_DXC_SPIRV)
                set(Missing "SPIR-V, for Vulkan, needs a dxc with SPIR-V output; install the Vulkan SDK "
                            "(https://vulkan.lunarg.com) or set -DLUDIFEX_DXC_SPIRV=<path to dxc>.")
            endif()
        elseif(Format STREQUAL "MSL")
            _ludifex_find_spirv_compiler()
            _ludifex_find_spirv_cross()
            if(NOT LUDIFEX_DXC_SPIRV OR NOT LUDIFEX_SPIRV_CROSS)
                set(Missing "MSL, for Metal, needs a dxc with SPIR-V output and spirv-cross; install the Vulkan "
                            "SDK (https://vulkan.lunarg.com), or set -DLUDIFEX_DXC_SPIRV and -DLUDIFEX_SPIRV_CROSS.")
            endif()
        else()
            message(FATAL_ERROR
                "ludifex: LUDIFEX_SHADER_FORMATS names ${Format}; the formats are DXIL, SPIRV, and MSL.")
        endif()

        if(Missing)
            if(Strict)
                message(FATAL_ERROR "ludifex: ${Missing}")
            endif()
            message(STATUS "ludifex: not building ${Format} shaders, so that backend is unavailable. ${Missing}")
            string(APPEND AllMissing " ${Missing}")
        else()
            list(APPEND Formats ${Format})
        endif()
    endforeach()

    if(NOT Formats)
        message(FATAL_ERROR "ludifex: no shader compiler was found.${AllMissing}")
    endif()

    message(STATUS "ludifex shader formats: ${Formats}")
    set_property(GLOBAL PROPERTY LUDIFEX_SHADER_FORMATS_BUILT "${Formats}")
    set(${OutVar} "${Formats}" PARENT_SCOPE)
endfunction()

# Compiles one entry point to every format this configure builds and embeds
# each in the target, as <SYMBOL>Dxil, <SYMBOL>Spirv, and <SYMBOL>Msl, each with
# a Size beside it. A format that is not built is embedded empty, so all three
# symbols always exist and code tells them apart by size.
function(_ludifex_compile_embedded)
    cmake_parse_arguments(PARSE_ARGV 0 Arg "" "TARGET;SOURCE;ENTRY;PROFILE;SYMBOL;DIRECTORY;INCLUDE_DIR;DEFINE"
                          "DEPENDS")
    ludifex_shader_formats(Formats)

    get_filename_component(SourcePath "${Arg_SOURCE}" ABSOLUTE)
    set(Directory "${Arg_DIRECTORY}")
    set(Includes "")
    if(Arg_INCLUDE_DIR)
        set(Includes -I "${Arg_INCLUDE_DIR}")
    endif()
    set(Depends "${SourcePath}" ${Arg_DEPENDS})
    set(SpirvArguments -spirv -fspv-target-env=vulkan1.0 -T ${Arg_PROFILE} -E ${Arg_ENTRY} ${Includes} -O3)

    foreach(Suffix Dxil Spirv Msl)
        set(Symbol "${Arg_SYMBOL}${Suffix}")
        set(GeneratedPath "${Directory}/${Symbol}.cpp")

        if(Suffix STREQUAL "Dxil" AND "DXIL" IN_LIST Formats)
            set(BlobPath "${Directory}/${Arg_SYMBOL}.dxil")
            add_custom_command(
                OUTPUT "${BlobPath}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${Directory}"
                COMMAND "${LUDIFEX_DXC}" -T ${Arg_PROFILE} -E ${Arg_ENTRY} ${Includes} -O3
                        -Fo "${BlobPath}" "${SourcePath}"
                DEPENDS ${Depends}
                COMMENT "Compiling ${Arg_ENTRY} from ${Arg_SOURCE} to DXIL"
                VERBATIM)
        elseif(Suffix STREQUAL "Spirv" AND "SPIRV" IN_LIST Formats)
            set(BlobPath "${Directory}/${Arg_SYMBOL}.spv")
            add_custom_command(
                OUTPUT "${BlobPath}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${Directory}"
                COMMAND "${LUDIFEX_DXC_SPIRV}" ${SpirvArguments} -Fo "${BlobPath}" "${SourcePath}"
                DEPENDS ${Depends}
                COMMENT "Compiling ${Arg_ENTRY} from ${Arg_SOURCE} to SPIR-V"
                VERBATIM)
        elseif(Suffix STREQUAL "Msl" AND "MSL" IN_LIST Formats)
            # Metal numbers its buffer slots differently from Vulkan; the
            # define tells the shader includes to follow Metal's numbering.
            # See the resources section of world_material.hlsli.
            set(BlobPath "${Directory}/${Arg_SYMBOL}.metal")
            add_custom_command(
                OUTPUT "${BlobPath}"
                BYPRODUCTS "${Directory}/${Arg_SYMBOL}.metal.spv"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${Directory}"
                COMMAND "${LUDIFEX_DXC_SPIRV}" ${SpirvArguments} -D ${Arg_DEFINE}
                        -Fo "${Directory}/${Arg_SYMBOL}.metal.spv" "${SourcePath}"
                COMMAND "${LUDIFEX_SPIRV_CROSS}" "${Directory}/${Arg_SYMBOL}.metal.spv"
                        --msl --msl-version 20100 --msl-decoration-binding --output "${BlobPath}"
                DEPENDS ${Depends}
                COMMENT "Compiling ${Arg_ENTRY} from ${Arg_SOURCE} to MSL"
                VERBATIM)
        else()
            # Written only when it differs, so reconfiguring rebuilds nothing.
            file(WRITE "${GeneratedPath}.in"
                "// ${Arg_ENTRY} from ${Arg_SOURCE} was not compiled to this format. Do not edit.\n"
                "\n"
                "#include <cstddef>\n"
                "\n"
                "extern const unsigned char ${Symbol}[] = { 0 };\n"
                "extern const size_t ${Symbol}Size = 0;\n")
            configure_file("${GeneratedPath}.in" "${GeneratedPath}" COPYONLY)
            target_sources(${Arg_TARGET} PRIVATE "${GeneratedPath}")
            continue()
        endif()

        add_custom_command(
            OUTPUT "${GeneratedPath}"
            COMMAND ${CMAKE_COMMAND}
                    -DINPUT=${BlobPath}
                    -DOUTPUT=${GeneratedPath}
                    -DSYMBOL=${Symbol}
                    -P "${LUDIFEX_SHADER_EMBED_SCRIPT}"
            DEPENDS "${BlobPath}" "${LUDIFEX_SHADER_EMBED_SCRIPT}"
            COMMENT "Embedding ${Symbol}"
            VERBATIM)
        target_sources(${Arg_TARGET} PRIVATE "${GeneratedPath}")
    endforeach()
endfunction()

# ludifex_add_shader(<target> <source> <entry point> <profile> <symbol> [included files...])
#
# Compiles one entry point and adds the embedded results to the target's
# sources. Anything listed after the symbol is a file the shader includes, so
# an edit to it rebuilds the shader.
function(ludifex_add_shader Target Source Entry Profile Symbol)
    _ludifex_compile_embedded(
        TARGET ${Target}
        SOURCE "${Source}"
        ENTRY ${Entry}
        PROFILE ${Profile}
        SYMBOL ${Symbol}
        DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/shaders"
        DEFINE LUDIFEX_METAL
        DEPENDS ${ARGN})
endfunction()

# ludifex_embed_file(<target> <file> <symbol>)
#
# Embeds a file's bytes in the target, as <symbol> and <symbol>Size, the way the
# compiled shaders are.
function(ludifex_embed_file Target File Symbol)
    get_filename_component(FilePath "${File}" ABSOLUTE)
    set(GeneratedPath "${CMAKE_CURRENT_BINARY_DIR}/generated/${Symbol}.cpp")

    add_custom_command(
        OUTPUT "${GeneratedPath}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND ${CMAKE_COMMAND}
                -DINPUT=${FilePath}
                -DOUTPUT=${GeneratedPath}
                -DSYMBOL=${Symbol}
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/EmbedBinary.cmake"
        DEPENDS "${FilePath}" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/EmbedBinary.cmake"
        COMMENT "Embedding ${Symbol}"
        VERBATIM)

    target_sources(${Target} PRIVATE "${GeneratedPath}")
endfunction()

# ludifex_add_material(<target> <source.hlsl> SYMBOL <name> [ENTRY <entry point>])
#
# Compiles a material shader when <target> is built and embeds it, in every
# format this build makes, so a program that ships needs no shader compiler
# and no .hlsl files:
#
#   ludifex_add_material(my_game shaders/glow.hlsl SYMBOL GlowShader)
#
#   #include "GlowShader.h"
#   ... CreateMaterial({ .Bytecode = GlowShader(), ... });
#
# GlowShader() returns a ludifex::ShaderBytecode holding each format, and the
# material takes the one its device runs. The shader is compiled against
# world_material.hlsli and world_post.hlsli as this build of ludifex has them,
# and rebuilt when the source or they change. ENTRY defaults to FragmentMain, as
# MaterialDesc::EntryPoint does. The same function serves surface and
# post-process materials.
function(ludifex_add_material Target Source)
    cmake_parse_arguments(PARSE_ARGV 2 Material "" "SYMBOL;ENTRY" "")
    if(NOT Material_SYMBOL)
        message(FATAL_ERROR "ludifex_add_material(${Target} ${Source}) needs SYMBOL <name>.")
    endif()
    if(NOT Material_ENTRY)
        set(Material_ENTRY FragmentMain)
    endif()

    # Beside this file in a source tree, under share/ in an installed package.
    set(IncludeDir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../shaders")
    if(NOT EXISTS "${IncludeDir}/world_material.hlsli")
        get_filename_component(IncludeDir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../../share/ludifex/shaders" ABSOLUTE)
    endif()
    if(NOT EXISTS "${IncludeDir}/world_material.hlsli")
        message(FATAL_ERROR
            "ludifex_add_material could not find world_material.hlsli beside ${CMAKE_CURRENT_FUNCTION_LIST_DIR}.")
    endif()

    set(Directory "${CMAKE_CURRENT_BINARY_DIR}/ludifex_materials")
    set(HeaderPath "${Directory}/include/${Material_SYMBOL}.h")
    set(Name "${Material_SYMBOL}")

    file(WRITE "${HeaderPath}.in"
        "// Generated by ludifex_add_material from ${Source}. Do not edit.\n"
        "\n"
        "#pragma once\n"
        "\n"
        "#include <ludifex/ludifex.h>\n"
        "\n"
        "#include <cstddef>\n"
        "\n"
        "extern const unsigned char ${Name}Dxil[];\n"
        "extern const size_t ${Name}DxilSize;\n"
        "extern const unsigned char ${Name}Spirv[];\n"
        "extern const size_t ${Name}SpirvSize;\n"
        "extern const unsigned char ${Name}Msl[];\n"
        "extern const size_t ${Name}MslSize;\n"
        "\n"
        "// The material in every format this build compiled it to. A format it\n"
        "// was not compiled to is empty.\n"
        "inline ludifex::ShaderBytecode ${Name}()\n"
        "{\n"
        "    return { ${Name}Dxil, ${Name}DxilSize, ${Name}Spirv, ${Name}SpirvSize,\n"
        "             ${Name}Msl, ${Name}MslSize };\n"
        "}\n")
    configure_file("${HeaderPath}.in" "${HeaderPath}" COPYONLY)

    _ludifex_compile_embedded(
        TARGET ${Target}
        SOURCE "${Source}"
        ENTRY ${Material_ENTRY}
        PROFILE ps_6_0
        SYMBOL ${Material_SYMBOL}
        DIRECTORY "${Directory}"
        INCLUDE_DIR "${IncludeDir}"
        DEFINE LUDIFEX_METAL
        DEPENDS "${IncludeDir}/world_material.hlsli" "${IncludeDir}/world_post.hlsli")

    target_sources(${Target} PRIVATE "${HeaderPath}")
    target_include_directories(${Target} PRIVATE "${Directory}/include")
endfunction()
