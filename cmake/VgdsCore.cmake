find_package(nlohmann_json CONFIG REQUIRED)
function(veylaxis_add_vgds target)
    target_sources(${target} PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vgds/Grid.cpp
        ${PROJECT_SOURCE_DIR}/src/vgds/MockProvider.cpp
        ${PROJECT_SOURCE_DIR}/src/vgds/JsonCodec.cpp)
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(${target} PRIVATE nlohmann_json::nlohmann_json)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    endif()
endfunction()
