cmake_minimum_required(VERSION 3.25)
project(MmoniNative VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Force static CRT (/MT) so Mmoni.exe has zero MSVC runtime DLL dependencies.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(Mmoni WIN32
    src/main.cpp
    src/platform/settings.cpp
    src/platform/single_instance.cpp
    src/platform/startup_task.cpp
    src/platform/overlay_window.cpp
    src/app/tray_icon.cpp
    src/app/app_controller.cpp
    src/sensors/pawnio_client.cpp
    src/sensors/pdh_cpu.cpp
    src/sensors/cpu_provider.cpp
    src/sensors/gpu_sleep_guard.cpp
    src/sensors/gpu_nvidia.cpp
    src/sensors/gpu_amd.cpp
    src/sensors/gpu_intel.cpp
    src/sensors/sensor_monitor.cpp
    src/ui/d2d_context.cpp
    src/ui/compositor_host.cpp
    src/ui/history_chart.cpp
    src/ui/overlay_renderer.cpp
    src/ui/overlay_view.cpp
    resources/Mmoni.rc
)

target_include_directories(Mmoni PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/resources
)

target_compile_definitions(Mmoni PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A00000C
    MMONI_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    MMONI_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    MMONI_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

target_compile_options(Mmoni PRIVATE
    /W4
    /permissive-
    /utf-8
    /Zc:__cplusplus
    /EHsc
    $<$<CONFIG:Release>:/Os /Ob2 /GL /Gw /Gy /GS>
)

target_link_options(Mmoni PRIVATE
    /MANIFEST:NO
    /SUBSYSTEM:WINDOWS
    $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO>
)

target_link_libraries(Mmoni PRIVATE
    user32
    gdi32
    shell32
    advapi32
    ole32
    oleaut32
    comctl32
    shcore
    dwmapi
    dcomp
    d2d1
    d3d11
    dxgi
    dwrite
    pdh
    cfgmgr32
    wtsapi32
    powrprof
    taskschd
    windowsapp
)

# Dev/benchmark console diagnostic target (asInvoker, includes embedded PawnIO modules for elevated validation).
add_executable(MmoniDiag
    src/diag_main.cpp
    src/platform/settings.cpp
    src/platform/startup_task.cpp
    src/sensors/pawnio_client.cpp
    src/sensors/pdh_cpu.cpp
    src/sensors/cpu_provider.cpp
    src/sensors/gpu_sleep_guard.cpp
    src/sensors/gpu_nvidia.cpp
    src/sensors/gpu_amd.cpp
    src/sensors/gpu_intel.cpp
    src/sensors/sensor_monitor.cpp
    resources/MmoniDiag.rc
)
target_include_directories(MmoniDiag PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/resources
)
target_compile_definitions(MmoniDiag PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A00000C
)
target_compile_options(MmoniDiag PRIVATE
    /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc $<$<CONFIG:Release>:/Os /Ob2 /GL /Gw /Gy /GS>
)
target_link_options(MmoniDiag PRIVATE
    $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO>
)
target_link_libraries(MmoniDiag PRIVATE
    user32 shell32 advapi32 ole32 oleaut32 dxgi pdh cfgmgr32 powrprof taskschd psapi
)

# Dev/benchmark GUI target (identical to Mmoni.exe, asInvoker manifest for non-elevated automated tests).
add_executable(MmoniBench WIN32
    src/main.cpp
    src/platform/settings.cpp
    src/platform/single_instance.cpp
    src/platform/startup_task.cpp
    src/platform/overlay_window.cpp
    src/app/tray_icon.cpp
    src/app/app_controller.cpp
    src/sensors/pawnio_client.cpp
    src/sensors/pdh_cpu.cpp
    src/sensors/cpu_provider.cpp
    src/sensors/gpu_sleep_guard.cpp
    src/sensors/gpu_nvidia.cpp
    src/sensors/gpu_amd.cpp
    src/sensors/gpu_intel.cpp
    src/sensors/sensor_monitor.cpp
    src/ui/d2d_context.cpp
    src/ui/compositor_host.cpp
    src/ui/history_chart.cpp
    src/ui/overlay_renderer.cpp
    src/ui/overlay_view.cpp
    resources/MmoniBench.rc
)
target_include_directories(MmoniBench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/resources
)
target_compile_definitions(MmoniBench PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A00000C
    MMONI_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    MMONI_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    MMONI_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)
target_compile_options(MmoniBench PRIVATE
    /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc $<$<CONFIG:Release>:/Os /Ob2 /GL /Gw /Gy /GS>
)
target_link_options(MmoniBench PRIVATE
    /MANIFEST:NO /SUBSYSTEM:WINDOWS $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO>
)
target_link_libraries(MmoniBench PRIVATE
    user32 gdi32 shell32 advapi32 ole32 oleaut32 comctl32 shcore dwmapi dcomp d2d1 d3d11 dxgi dwrite pdh cfgmgr32 wtsapi32 powrprof taskschd windowsapp
)
