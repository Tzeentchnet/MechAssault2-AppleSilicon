cmake_minimum_required(VERSION 3.20)
project(ma2_embed_override C)

add_library(xbox_d3d8 INTERFACE)
add_subdirectory(../../.. runtime)

get_target_property(type xbox_d3d8 TYPE)
get_target_property(links xboxrecomp INTERFACE_LINK_LIBRARIES)
if(NOT XBOXRECOMP_BUILD_D3D8 OR NOT type STREQUAL "INTERFACE_LIBRARY" OR
   NOT "xbox_d3d8" IN_LIST links OR EPOXY_FOUND)
    message(FATAL_ERROR "The embedding D3D8 replacement contract was not honored")
endif()
