cmake_minimum_required(VERSION 3.20)
project(TrayApp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- MSVC flags ---
if(MSVC)
    add_compile_options(/utf-8 /EHsc /await)
    add_compile_definitions(
        UNICODE _UNICODE
        WIN32_LEAN_AND_MEAN
        WINRT_LEAN_AND_MEAN
        _WINDOWS
    )
endif()

# --- NuGet helper: download nuget.exe if missing ---
set(NUGET_EXE "${CMAKE_BINARY_DIR}/nuget.exe")
if(NOT EXISTS "${NUGET_EXE}")
    message(STATUS "Downloading nuget.exe ...")
    file(DOWNLOAD
        "https://dist.nuget.org/win-x86-commandline/v6.9.1/nuget.exe"
        "${NUGET_EXE}"
        SHOW_PROGRESS
    )
endif()

# --- Package versions ---
set(WASDK_VER   "1.6.240829007")
set(CPPWINRT_VER "2.0.240405.15")
set(PKG_DIR     "${CMAKE_BINARY_DIR}/packages")

# --- Install Microsoft.WindowsAppSDK ---
set(WASDK_DIR "${PKG_DIR}/Microsoft.WindowsAppSDK.${WASDK_VER}")
if(NOT EXISTS "${WASDK_DIR}")
    message(STATUS "Installing Microsoft.WindowsAppSDK ${WASDK_VER} ...")
    execute_process(
        COMMAND "${NUGET_EXE}" install Microsoft.WindowsAppSDK
                -Version ${WASDK_VER}
                -OutputDirectory "${PKG_DIR}"
        RESULT_VARIABLE _res
    )
    if(NOT _res EQUAL 0)
        message(FATAL_ERROR "Failed to install Microsoft.WindowsAppSDK")
    endif()
endif()

# --- Install Microsoft.Windows.CppWinRT ---
set(CPPWINRT_PKG "${PKG_DIR}/Microsoft.Windows.CppWinRT.${CPPWINRT_VER}")
if(NOT EXISTS "${CPPWINRT_PKG}")
    message(STATUS "Installing Microsoft.Windows.CppWinRT ${CPPWINRT_VER} ...")
    execute_process(
        COMMAND "${NUGET_EXE}" install Microsoft.Windows.CppWinRT
                -Version ${CPPWINRT_VER}
                -OutputDirectory "${PKG_DIR}"
        RESULT_VARIABLE _res
    )
    if(NOT _res EQUAL 0)
        message(FATAL_ERROR "Failed to install Microsoft.Windows.CppWinRT")
    endif()
endif()

# --- Generate C++/WinRT projection headers ---
set(CPPWINRT_EXE "${CPPWINRT_PKG}/bin/cppwinrt.exe")
set(GENERATED_DIR "${CMAKE_BINARY_DIR}/cppwinrt")

if(NOT EXISTS "${GENERATED_DIR}/winrt/Microsoft.UI.Xaml.h")
    message(STATUS "Generating C++/WinRT projection headers ...")
    set(SDK_VERSION "10.0.22621.0")

    # WindowsAppSDK metadata directories
    set(WASDK_WINMD_BASE "${WASDK_DIR}/lib/uap10.0.18362")
    set(WASDK_WINMD_UAP  "${WASDK_DIR}/lib/uap10.0")
    
    set(CPPWINRT_INPUTS "")
    list(APPEND CPPWINRT_INPUTS -input "${WASDK_WINMD_BASE}")
    list(APPEND CPPWINRT_INPUTS -input "${WASDK_WINMD_UAP}")

    # Find WebView2 package (it's a dependency)
    file(GLOB _WEBVIEW2_DIRS "${PKG_DIR}/Microsoft.Web.WebView2.*")
    if(_WEBVIEW2_DIRS)
        list(GET _WEBVIEW2_DIRS -1 WEBVIEW2_DIR)
        list(APPEND CPPWINRT_INPUTS -input "${WEBVIEW2_DIR}/lib")
    endif()

    # Use environment variables from CI if available, otherwise fallback to default
    if(DEFINED ENV{WindowsSdkDir} AND DEFINED ENV{WindowsSDKVersion})
        set(SDK_METADATA_PATH "$ENV{WindowsSdkDir}UnionMetadata/$ENV{WindowsSDKVersion}")
        # Strip trailing backslash if present
        string(REGEX REPLACE "\\\\$" "" SDK_METADATA_PATH "${SDK_METADATA_PATH}")
        message(STATUS "Using Windows SDK metadata from: ${SDK_METADATA_PATH}")
        list(APPEND CPPWINRT_INPUTS -input "${SDK_METADATA_PATH}")
    else()
        message(STATUS "Windows SDK environment variables not found, falling back to 'sdk' keyword")
        list(APPEND CPPWINRT_INPUTS -input "sdk")
    endif()

    execute_process(
        COMMAND "${CPPWINRT_EXE}"
                ${CPPWINRT_INPUTS}
                -output "${GENERATED_DIR}"
                -verbose
        RESULT_VARIABLE _res
    )

    if(NOT _res EQUAL 0)
        message(FATAL_ERROR "cppwinrt.exe failed (code ${_res})")
    endif()
endif()


# --- Resolve platform-specific paths ---
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(ARCH "x64")
else()
    set(ARCH "x86")
endif()

set(WASDK_LIB_DIR "${WASDK_DIR}/lib/win10-${ARCH}")
set(WASDK_INCLUDE  "${WASDK_DIR}/include")
# Find runtime directory robustly (could be win10-x64 or win-x64)
file(GLOB _WASDK_RUNTIME_DIRS "${WASDK_DIR}/runtimes/win*-${ARCH}/native")
if(_WASDK_RUNTIME_DIRS)
    list(GET _WASDK_RUNTIME_DIRS 0 WASDK_RUNTIME)
else()
    set(WASDK_RUNTIME "${WASDK_DIR}/runtimes/win10-${ARCH}/native")
endif()

# --- Executable ---
set(RPC_OUTPUT_DIR "${CMAKE_BINARY_DIR}/rpc")
file(MAKE_DIRECTORY "${RPC_OUTPUT_DIR}")

add_custom_command(
    OUTPUT "${RPC_OUTPUT_DIR}/ServiceRpc_h.h"
           "${RPC_OUTPUT_DIR}/ServiceRpc_c.c"
           "${RPC_OUTPUT_DIR}/ServiceRpc_s.c"
    COMMAND midl.exe /nologo /env x64 /Oicf /h "ServiceRpc_h.h" /cstub "ServiceRpc_c.c" /sstub "ServiceRpc_s.c" "${CMAKE_CURRENT_SOURCE_DIR}/src/ServiceRpc.idl"
    WORKING_DIRECTORY "${RPC_OUTPUT_DIR}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/ServiceRpc.idl"
)

set_source_files_properties(
    "${RPC_OUTPUT_DIR}/ServiceRpc_c.c"
    "${RPC_OUTPUT_DIR}/ServiceRpc_s.c"
    "${RPC_OUTPUT_DIR}/ServiceRpc_h.h"
    PROPERTIES GENERATED TRUE
)

add_custom_target(GenerateRpcStubs DEPENDS
    "${RPC_OUTPUT_DIR}/ServiceRpc_h.h"
    "${RPC_OUTPUT_DIR}/ServiceRpc_c.c"
    "${RPC_OUTPUT_DIR}/ServiceRpc_s.c"
)

add_library(RpcClientStub STATIC "${RPC_OUTPUT_DIR}/ServiceRpc_c.c")
target_include_directories(RpcClientStub PUBLIC "${RPC_OUTPUT_DIR}")
add_dependencies(RpcClientStub GenerateRpcStubs)

add_library(RpcServerStub STATIC "${RPC_OUTPUT_DIR}/ServiceRpc_s.c")
target_include_directories(RpcServerStub PUBLIC "${RPC_OUTPUT_DIR}")
add_dependencies(RpcServerStub GenerateRpcStubs)

add_executable(TrayApp WIN32
    src/main.cpp
    resources/app.rc
    src/app.manifest
)

target_include_directories(TrayApp PRIVATE
    "${GENERATED_DIR}"
    "${WASDK_INCLUDE}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${RPC_OUTPUT_DIR}"
)

# Link only Bootstrap lib + Win32 + WinRT base
target_link_libraries(TrayApp PRIVATE
    RpcClientStub
    "${WASDK_LIB_DIR}/Microsoft.WindowsAppRuntime.Bootstrap.lib"
    user32 shell32 gdi32 comctl32 ole32 advapi32 rpcrt4 wtsapi32
    windowsapp
)

set_target_properties(TrayApp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    VS_DPI_AWARE "PerMonitor"
)

# --- Copy Windows App SDK DLLs to output ---
add_custom_command(TARGET TrayApp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${WASDK_RUNTIME}"
        "$<TARGET_FILE_DIR:TrayApp>"
    COMMENT "Copying Windows App SDK runtime DLLs ..."
)

# --- Install ---
install(TARGETS TrayApp RUNTIME DESTINATION bin)
install(DIRECTORY "${WASDK_RUNTIME}/" DESTINATION bin)

target_compile_definitions(TrayApp PRIVATE DISABLE_XAML_GENERATED_MAIN)
target_include_directories(TrayApp PRIVATE src)

add_executable(TrayAppService WIN32
    src/service_main.cpp
)
target_include_directories(TrayAppService PRIVATE "${RPC_OUTPUT_DIR}")
target_link_libraries(TrayAppService PRIVATE RpcServerStub user32 advapi32 rpcrt4 wtsapi32 userenv)
set_target_properties(TrayAppService PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_executable(SecurePrompt WIN32
    src/prompt_main.cpp
)
target_link_libraries(SecurePrompt PRIVATE user32)
set_target_properties(SecurePrompt PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

