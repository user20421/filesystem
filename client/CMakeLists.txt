cmake_minimum_required(VERSION 3.16)
project(secure_im_client VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# qt_add_protobuf 通过宿主机 protoc 驱动 qtprotobufgen；本地工具仅用于代码生成，不需要全局安装 Protobuf。
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
    src/core/ApiClientEndpoints.cpp
    src/core/ApiClientHistory.cpp
    src/core/ApiClientRefreshLogout.cpp
    src/core/ApiClientTransport.cpp
    src/core/ApiJsonCodec.cpp
    src/core/ApiJsonCodec.h
    src/core/ApiTypes.h
    src/core/LocalStore.cpp
    src/core/LocalStore.h
    src/core/LocalStoreMessageCodec.cpp
    src/core/LocalStoreMessageCodec.h
    src/core/MessageDelivery.cpp
    src/core/MessageDelivery.h
    src/core/MessageSyncState.h
    src/core/MessagePayloadCodec.cpp
    src/core/MessagePayloadCodec.h
    src/core/TransferManager.cpp
    src/core/TransferManager.h
    src/core/TransferDownload.cpp
    src/core/TransferLifecycle.cpp
    src/core/TransferScheduling.cpp
    src/core/TransferUpload.cpp
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

qt_add_protobuf(client_core
    PROTO_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/../proto/im/v1/envelope.proto"
        "${CMAKE_CURRENT_SOURCE_DIR}/../proto/im/v1/messages.proto"
    PROTO_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/../proto"
)
message(STATUS "Realtime Qt Protobuf codec enabled")

if(MSVC)
    target_compile_options(client_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(client_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# application 只保存会话、压测进程和报告等应用逻辑，不依赖 Qt Widgets。
add_library(client_application STATIC
    src/app/RootStressSession.cpp src/app/RootStressSession.h
    src/app/SessionContext.cpp src/app/SessionContext.h
    src/app/StressAssessment.cpp src/app/StressAssessment.h
    src/app/StressNdjsonCodec.cpp src/app/StressNdjsonCodec.h
    src/app/StressReportRepository.cpp src/app/StressReportRepository.h
    src/app/StressRunController.cpp src/app/StressRunController.h
)
target_include_directories(client_application PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/app")
target_link_libraries(client_application PUBLIC client_core)
target_compile_features(client_application PUBLIC cxx_std_17)
if(MSVC)
    target_compile_options(client_application PRIVATE /W4 /permissive-)
else()
    target_compile_options(client_application PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(client_ui STATIC
    src/ui/Format.cpp src/ui/Format.h
    src/ui/LoginPage.cpp src/ui/LoginPage.h
    src/ui/MainWindow.cpp src/ui/MainWindow.h
    src/ui/MainWindowChat.cpp
    src/ui/MainWindowContacts.cpp
    src/ui/MainWindowInternal.h
    src/ui/MainWindowTransfers.cpp
    src/ui/MainWindowUi.cpp
    src/ui/SessionPage.cpp src/ui/SessionPage.h
    src/ui/SingleUserWindow.cpp src/ui/SingleUserWindow.h
    src/ui/SimulatorWindow.cpp src/ui/SimulatorWindow.h
    src/ui/SystemTestPage.cpp src/ui/SystemTestPage.h
    src/ui/Theme.cpp src/ui/Theme.h
    src/ui/TransferTaskCard.cpp src/ui/TransferTaskCard.h
)
target_include_directories(client_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/ui")
target_link_libraries(client_ui PUBLIC client_application Qt6::Widgets)
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

# 集中保留 CTest 名称、超时、offscreen 字体环境和测试可执行文件依赖。
function(client_add_qt_test target test_name)
    cmake_parse_arguments(TEST "NO_CTEST;OFFSCREEN" "TIMEOUT" "LIBRARIES;DEPENDS" ${ARGN})
    qt_add_executable(${target} tests/${target}.cpp)
    target_link_libraries(${target} PRIVATE ${TEST_LIBRARIES} Qt6::Test)
    if(TEST_DEPENDS)
        add_dependencies(${target} ${TEST_DEPENDS})
    endif()
    if(NOT TEST_NO_CTEST)
        add_test(NAME ${test_name} COMMAND ${target})
        if(TEST_TIMEOUT)
            set_tests_properties(${test_name} PROPERTIES TIMEOUT ${TEST_TIMEOUT})
        endif()
        if(TEST_OFFSCREEN)
            set_tests_properties(${test_name} PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_FONTDIR=C:/Windows/Fonts")
        endif()
    endif()
endfunction()

if(BUILD_TESTING)
    client_add_qt_test(test_local_store local_store LIBRARIES client_core)
    client_add_qt_test(test_message_sync message_sync LIBRARIES client_core)
    client_add_qt_test(test_message_delivery message_delivery LIBRARIES client_core TIMEOUT 30)
    client_add_qt_test(test_transfer_manager transfer_manager LIBRARIES client_core TIMEOUT 120)
    client_add_qt_test(test_api_client api_client LIBRARIES client_core)
    client_add_qt_test(test_api_json_codec api_json_codec LIBRARIES client_core)
    # 不注册到默认 CTest：真实后端集成测试需显式设置 CLIENT_BACKEND_URL。
    client_add_qt_test(test_backend_integration backend_integration NO_CTEST LIBRARIES client_core)
    client_add_qt_test(test_wire_codec wire_codec LIBRARIES client_core)
    client_add_qt_test(test_application_logic application_logic LIBRARIES client_application)
    client_add_qt_test(test_session_tabs session_tabs OFFSCREEN LIBRARIES client_ui)

    qt_add_executable(fake_stress_runner tests/fake_stress_runner.cpp)
    target_link_libraries(fake_stress_runner PRIVATE Qt6::Core)

    # 先构建可控的假 runner，确保测试启动前该可执行文件存在。
    client_add_qt_test(test_stress_run stress_run OFFSCREEN LIBRARIES client_ui
                       DEPENDS fake_stress_runner)
endif()
