add_executable(d1x-gameserver gameserver.c spawn.c)
target_compile_definitions(d1x-gameserver PRIVATE
    DXX_VERSION_MAJORi=${PROJECT_VERSION_MAJOR}
    DXX_VERSION_MINORi=${PROJECT_VERSION_MINOR}
    DXX_VERSION_MICROi=$<IF:$<BOOL:${PROJECT_VERSION_MICRO}>,${PROJECT_VERSION_MICRO},0>
)
if(WIN32)
    target_link_libraries(d1x-gameserver ws2_32)
endif()
