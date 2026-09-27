# Turns a compiled shader blob into a C++ translation unit.
#
# Shaders are embedded rather than loaded from disk so that a program built
# against ludifex has no runtime file dependency and cannot fall out of sync
# with the shaders it was compiled against.
#
# Invoked in script mode:
#   cmake -DINPUT=<blob> -DOUTPUT=<cpp> -DSYMBOL=<name> -P EmbedBinary.cmake

if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED SYMBOL)
    message(FATAL_ERROR "EmbedBinary.cmake requires INPUT, OUTPUT and SYMBOL")
endif()

file(READ "${INPUT}" HexContent HEX)

string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " Bytes "${HexContent}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f], ){12})" "\\1\n    " Bytes "${Bytes}")

get_filename_component(SourceName "${INPUT}" NAME)

file(WRITE "${OUTPUT}"
"// Generated from ${SourceName}. Do not edit.\n"
"\n"
"#include <cstddef>\n"
"\n"
"extern const unsigned char ${SYMBOL}[] = {\n"
"    ${Bytes}\n"
"};\n"
"\n"
"extern const size_t ${SYMBOL}Size = sizeof(${SYMBOL});\n")
