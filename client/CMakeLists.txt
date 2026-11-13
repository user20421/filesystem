cmake_minimum_required(VERSION 3.16)
project(secure_im_client VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Qt's qt_add_protobuf drives qtprotobufgen through a host protoc.  A locally
# downloaded tool can be supplied without installing Google Protobuf globally.
set(CLIENT_PROTOC_EXECUTABLE "" CACHE FILEPATH "Host protoc used only for code generation")
if(CLIENT_PROTOC_EXECUTABLE AND NOT TARGET WrapProtoc::WrapProtoc)
    if(NOT EXISTS "${CLIENT_PROTOC_EXECUTABLE}")
        message(FATAL_ERROR "CLIENT_PROTOC_EXECUTABLE does not exist: ${CLIENT_PROTOC_EXECUTABLE}")
    endif()
    add_executable(WrapProtoc::WrapProtoc IMPORTED)
    set_target_properties(WrapProtoc::WrapProtoc PROPERTIES
        IMPORTED_LOCATION "${CLIENT_PROTOC_EXECUTABLE}")
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Network WebSockets Test Protobuf ProtobufTools)

set(CLIENT_CORE_SOURCES
    src/core/ApiClient.cpp
    src/core/ApiClient.h
    src/core/ApiTypes.h
    src/core/LocalStore.cpp
    src/core/LocalStore.h
    src/core/MessageSyncState.h
    src/core/TransferManager.cpp
    src/core/TransferManager.h
    src/core/WebSocketClient.cpp
    src/core/WebSocketClient.h
    src/core/WireCodec.cpp
    src/core/WireCodec.h
    src/core/ProtobufWireCodec.cpp
    src/core/ProtobufWireCodec.h
    src/core/Workers.cpp
    src/core/Workers.h
)

add_library(client_core STATIC ${CLIENT_CORE_SOURCES})
target_include_directories(client_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/core")
target_link_libraries(client_core
    PUBLIC Qt6::Network Qt6::WebSockets
    PRIVATE Qt6::Protobuf)
target_compile_features(client_core PUBLIC cxx_std_17)
target_compile_definitions(client_core PUBLIC CLIENT_HAS_PROTOBUF=1)

qt_add_protobuf(client_core
    PROTO_FILES "${CMAKE_CURRENT_SOURCE_DIR}/../proto/im/v1/envelope.proto"
    PROTO_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/../proto"
)
message(STATUS "Realtime Qt Protobuf codec enabled")

if(MSVC)
    target_compile_options(client_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(client_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(client_ui STATIC
    src/ui/LoginPage.cpp src/ui/LoginPage.h
    src/ui/MainWindow.cpp src/ui/MainWindow.h
    src/ui/SessionContext.cpp src/ui/SessionContext.h
    src/ui/SessionPage.cpp src/ui/SessionPage.h
    src/ui/SimulatorWindow.cpp src/ui/SimulatorWindow.h
    src/ui/SystemTestPage.cpp src/ui/SystemTestPage.h
    src/ui/Theme.cpp src/ui/Theme.h
)
target_include_directories(client_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/ui")
target_link_libraries(client_ui PUBLIC client_core Qt6::Widgets)
target_compile_features(client_ui PUBLIC cxx_std_17)
if(MSVC)
    target_compile_options(client_ui PRIVATE /W4 /permissive-)
else()
    target_compile_options(client_ui PRIVATE -Wall -Wextra -Wpedantic)
endif()

qt_add_executable(secure-im-client WIN32 src/apps/client_main.cpp)
target_link_libraries(secure-im-client PRIVATE client_ui)

qt_add_executable(secure-im-simulator WIN32 src/apps/simulator_main.cpp)
target_link_libraries(secure-im-simulator PRIVATE client_ui)

set_target_properties(secure-im-client secure-im-simulator PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

include(CTest)
if(BUILD_TESTING)
    qt_add_executable(test_local_store tests/test_local_store.cpp)
    target_link_libraries(test_local_store PRIVATE client_core Qt6::Test)
    add_test(NAME local_store COMMAND test_local_store)

    qt_add_executable(test_message_sync tests/test_message_sync.cpp)
    target_link_libraries(test_message_sync PRIVATE client_core Qt6::Test)
    add_test(NAME message_sync COMMAND test_message_sync)

    qt_add_executable(test_transfer_manager tests/test_transfer_manager.cpp)
    target_link_libraries(test_transfer_manager PRIVATE client_core Qt6::Test)
    add_test(NAME transfer_manager COMMAND test_transfer_manager)

    qt_add_executable(test_api_client tests/test_api_client.cpp)
    target_link_libraries(test_api_client PRIVATE client_core Qt6::Test)
    add_test(NAME api_client COMMAND test_api_client)

    qt_add_executable(test_backend_integration tests/test_backend_integration.cpp)
    target_link_libraries(test_backend_integration PRIVATE client_core Qt6::Test)
    # Not registered with CTest: run explicitly with CLIENT_BACKEND_URL set.

    qt_add_executable(test_wire_codec tests/test_wire_codec.cpp)
    target_link_libraries(test_wire_codec PRIVATE client_core Qt6::Test)
    add_test(NAME wire_codec COMMAND test_wire_codec)

    qt_add_executable(test_session_tabs tests/test_session_tabs.cpp)
    target_link_libraries(test_session_tabs PRIVATE client_ui Qt6::Test)
    add_test(NAME session_tabs COMMAND test_session_tabs)
    set_tests_properties(session_tabs PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_FONTDIR=C:/Windows/Fonts;SECURE_IM_PREVIEW_PATH=${CMAKE_BINARY_DIR}/session-tabs-preview.png")
endif()
