# One defect in Box3D 0.1.0 makes a cone joint gain energy:
#
# - The cone limit pushes along a swing axis found once per step, in prepare,
#   and reused by every sub-step. A body swinging round the rim of the cone
#   moves on while the axis stays behind, so each push has a component along
#   the direction the body is already going. The body circles faster every
#   step and creeps out of the cone. It shows most on a narrow cone and a long
#   body, and a ragdoll's arm resting against its shoulder limit can wind
#   itself up from rest.
#
# ludifex_patch_box3d writes a copy of spherical_joint.c whose cone limit takes
# the swing axis, and the mass along it, from the sub-step's own rotations, and
# builds the box3d target from that copy. The fetched source is left untouched.
# If a later Box3D changes the code the patch anchors on, configuration stops
# with a message instead of building the unpatched code.

set(LudifexPatchBox3DFile "${CMAKE_CURRENT_LIST_FILE}")

function(ludifex_patch_box3d SourceDir)
    set(Source "${SourceDir}/src/spherical_joint.c")
    set(PatchedDir "${CMAKE_CURRENT_BINARY_DIR}/box3d-patched")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${Source}" "${LudifexPatchBox3DFile}")

    file(READ "${Source}" Code)

    set(Axis "\t\tb3Vec3 swingAxis = joint->swingAxis;\n")
    set(Impulse "float deltaImpulse = -massScale * joint->swingMass * ( cdot + bias ) - impulseScale * oldImpulse;")

    string(FIND "${Code}" "${Axis}" AxisAt)
    string(FIND "${Code}" "${Impulse}" ImpulseAt)
    if(AxisAt EQUAL -1 OR ImpulseAt EQUAL -1)
        message(FATAL_ERROR
            "ludifex: spherical_joint.c in ${SourceDir} no longer has the code that "
            "cmake/PatchBox3D.cmake corrects. Check whether this Box3D version still "
            "solves the cone limit along the swing axis from prepare, then update or "
            "remove the patch.")
    endif()

    # Appended line by line: a CMake list would split the C at its semicolons.
    set(Corrected "")
    string(APPEND Corrected "\t\t// Corrected by ludifex (cmake/PatchBox3D.cmake): the swing axis comes from\n")
    string(APPEND Corrected "\t\t// this sub-step's rotations. The one from prepare goes stale as a body\n")
    string(APPEND Corrected "\t\t// circles the cone, and pushing along it drives the body round the rim.\n")
    string(APPEND Corrected "\t\tb3Vec3 swingAxis = joint->swingAxis;\n")
    string(APPEND Corrected "\t\tb3Vec3 currentSwing = b3Cross( b3RotateVector( quatA, b3Vec3_axisZ ), b3RotateVector( quatB, b3Vec3_axisZ ) );\n")
    string(APPEND Corrected "\t\tfloat currentLength = b3Length( currentSwing );\n")
    string(APPEND Corrected "\t\tif ( currentLength > 1.0e-4f )\n")
    string(APPEND Corrected "\t\t{\n")
    string(APPEND Corrected "\t\t\tswingAxis = b3MulSV( 1.0f / currentLength, currentSwing );\n")
    string(APPEND Corrected "\t\t\tjoint->swingAxis = swingAxis;\n")
    string(APPEND Corrected "\t\t}\n")
    string(APPEND Corrected "\t\tfloat swingK = b3Dot( swingAxis, b3Add( b3MulMV( iA, swingAxis ), b3MulMV( iB, swingAxis ) ) );\n")
    string(APPEND Corrected "\t\tfloat swingMass = swingK > 0.0f ? 1.0f / swingK : 0.0f;\n")

    string(REPLACE "${Axis}" "${Corrected}" Code "${Code}")
    string(REPLACE "${Impulse}"
        "float deltaImpulse = -massScale * swingMass * ( cdot + bias ) - impulseScale * oldImpulse;"
        Code "${Code}")

    # Written through configure_file so the copy's timestamp only changes when
    # its content does, and reconfiguring does not rebuild Box3D.
    file(WRITE "${PatchedDir}/spherical_joint.c.in" "${Code}")
    configure_file("${PatchedDir}/spherical_joint.c.in" "${PatchedDir}/spherical_joint.c" COPYONLY)

    get_target_property(Sources box3d SOURCES)
    list(TRANSFORM Sources REPLACE "^(.*[/\\\\])?spherical_joint\\.c$" "${PatchedDir}/spherical_joint.c")
    list(FIND Sources "${PatchedDir}/spherical_joint.c" PatchedAt)
    if(PatchedAt EQUAL -1)
        message(FATAL_ERROR "ludifex: the box3d target no longer lists spherical_joint.c; update cmake/PatchBox3D.cmake.")
    endif()
    set_property(TARGET box3d PROPERTY SOURCES ${Sources})

    # The copy includes its neighbours by relative path.
    target_include_directories(box3d PRIVATE "${SourceDir}/src")
endfunction()
