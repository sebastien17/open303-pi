cmake_minimum_required(VERSION 3.13)
project(open303_pi_host CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -ffast-math")

# Le code source d'Open303 utilise memcpy/memmove/INT_MAX sans inclure
# <cstring>/<climits> explicitement (tolere par d'anciens compilateurs, plus
# par GCC recent). On force ces includes pour tout le build plutot que de
# patcher le depot tiers a la main.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -include cstring -include climits")

# --- Optimisations specifiques Raspberry Pi 3B+ (CPU Cortex-A53) ---
# Active automatiquement sur ARM ; sans effet sur un cross-build x86 pour
# tester le code ailleurs.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|aarch64)")
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    # Raspberry Pi OS 64-bit
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mcpu=cortex-a53")
  else()
    # Raspberry Pi OS 32-bit (armhf)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mcpu=cortex-a53 -mfpu=neon-fp-armv8 -mfloat-abi=hard")
  endif()
endif()

# --- Dependances trouvees via pkg-config (installees par apt, voir README) ---
find_package(PkgConfig REQUIRED)
pkg_check_modules(RTAUDIO REQUIRED rtaudio)
pkg_check_modules(RTMIDI  REQUIRED rtmidi)

# --- Emplacement des sources d'Open303 (cf. README pour le telecharger) ---
# Le repo RobinSchmidt/Open303 contient le code DSP dans Source/DSPCode
set(OPEN303_DIR "${CMAKE_SOURCE_DIR}/third_party/Open303" CACHE PATH "Chemin vers le repo Open303 clone")
set(ROSIC_DIR "${OPEN303_DIR}/Source/DSPCode")

if(NOT EXISTS "${ROSIC_DIR}")
  message(FATAL_ERROR
    "Introuvable: ${ROSIC_DIR}\n"
    "Clonez d'abord Open303:\n"
    "  git clone https://github.com/RobinSchmidt/Open303 ${OPEN303_DIR}\n"
    "(le code DSP est dans Source/DSPCode dans ce depot)\n"
    "ou passez -DOPEN303_DIR=/chemin/vers/Open303 a cmake.")
endif()

file(GLOB_RECURSE ROSIC_SOURCES
  "${ROSIC_DIR}/*.cpp"
  "${ROSIC_DIR}/*.c"
)

add_executable(open303_pi_host
  src/main.cpp
  ${ROSIC_SOURCES}
)

target_include_directories(open303_pi_host PRIVATE
  ${ROSIC_DIR}
  ${RTAUDIO_INCLUDE_DIRS}
  ${RTMIDI_INCLUDE_DIRS}
)

target_link_libraries(open303_pi_host PRIVATE
  ${RTAUDIO_LIBRARIES}
  ${RTMIDI_LIBRARIES}
  pthread
)

target_compile_options(open303_pi_host PRIVATE
  ${RTAUDIO_CFLAGS_OTHER}
  ${RTMIDI_CFLAGS_OTHER}
)
