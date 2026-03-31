cmake_minimum_required(VERSION 3.20)
project(PotatoRec VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Compiler flags optimized for Celeron (generic x86-64, no AVX2) ──────────
if(MSVC)
    # /O2   = optimize for speed
    # /GL   = whole program optimization
    # /arch:SSE2 = Celeron supports SSE2 (not AVX/AVX2)
    # /GS-  = disable security cookies (tiny speed gain)
    # /W3   = warnings
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /GL /arch:SSE2 /W3 /MP /EHsc")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "/LTCG /OPT:REF /OPT:ICF")
    
    # Debug: fast iteration
    set(CMAKE_CXX_FLAGS_DEBUG "/Od /Zi /W3 /MP /EHsc /RTC1")
    
    # Unicode (required for modern Windows API)
    add_definitions(-DUNICODE -D_UNICODE)
    add_definitions(-DWIN32_LEAN_AND_MEAN -DNOMINMAX)
    
    # Target Windows 10 minimum (for DXGI 1.2 Desktop Duplication)
    add_definitions(-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00)
endif()

# ── Sources ──────────────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
)

set(HEADERS
    src/capture.h
    src/encoder.h
    src/audio.h
    src/cursor.h
    src/tray.h
    src/config.h
)

# ── Windows subsystem (no console window) ────────────────────────────────────
add_executable(PotatoRec WIN32 ${SOURCES} ${HEADERS})

# ── Link libraries ───────────────────────────────────────────────────────────
target_link_libraries(PotatoRec PRIVATE
    # DirectX 11 + DXGI
    d3d11
    dxgi
    
    # Media Foundation (hardware encoder)
    mf
    mfplat
    mfuuid
    mfreadwrite
    
    # Audio
    avrt        # AvSetMmThreadCharacteristics
    
    # Windows shell/UI
    shell32
    user32
    gdi32
    ole32
    oleaut32
    comctl32
    
    # Runtime
    advapi32
    kernel32
)

# ── Output name ──────────────────────────────────────────────────────────────
set_target_properties(PotatoRec PROPERTIES
    OUTPUT_NAME "PotatoRec"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ── Install ──────────────────────────────────────────────────────────────────
install(TARGETS PotatoRec DESTINATION .)
install(FILES 
    "${CMAKE_SOURCE_DIR}/PotatoRec.ini.example"
    DESTINATION .
    RENAME "PotatoRec.ini.example"
    OPTIONAL
)
