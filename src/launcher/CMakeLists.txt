add_executable(EndTask10Launcher WIN32 main.cpp)
target_link_libraries(EndTask10Launcher kernel32.lib user32.lib ntdll.lib advapi32.lib)
set_target_properties(EndTask10Launcher PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
