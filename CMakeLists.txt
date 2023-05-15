cmake_minimum_required(VERSION 3.9)

project(
    broadcast_queue
    VERSION 1.0.0
    DESCRIPTION "A single producer multiple consumer broadcast queue for C++11"
    LANGUAGES CXX
)

add_library(${PROJECT_NAME} INTERFACE)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

target_include_directories(${PROJECT_NAME} 
    INTERFACE 
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/${PROJECT_NAME}>
)

write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-config-version.cmake
    VERSION 
        ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion
)

configure_package_config_file(
    ${PROJECT_NAME}-config.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}/
)

install(
    FILES ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-config.cmake
          ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)

install(
    FILES broadcast_queue.h futex_waiting_strategy.h semaphore_waiting_strategy.h
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/${PROJECT_NAME}"
)

install(
    TARGETS ${PROJECT_NAME}
    EXPORT ${PROJECT_NAME}-targets
)

install(
    EXPORT ${PROJECT_NAME}-targets
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}"
)

# testing
if(BUILD_TESTING)
    enable_testing()
    find_package(GTest)
    if(NOT GTest_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.13.0.zip
        )
        # For Windows: Prevent overriding the parent project's compiler/linker settings
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()
    add_executable(
    tests
        tests/single_threaded_tests.cpp
        tests/multi_threaded_tests.cpp
    )
    target_link_libraries(tests GTest::gtest_main broadcast_queue)
    gtest_discover_tests(tests)
endif()

if(BUILD_BENCHES)
    include(FetchContent)
    FetchContent_Declare(
        concurrentqueue
        GIT_REPOSITORY https://github.com/cameron314/concurrentqueue
        GIT_TAG master
    )
    FetchContent_MakeAvailable(concurrentqueue)

    add_executable(messages_per_second benches/messages_per_second.cpp)
    target_link_libraries(messages_per_second broadcast_queue concurrentqueue)
endif()
