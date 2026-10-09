# OptiCraft Heritage (WIP DSi PORT)

## English:

In this branch is being vibecoded a port of OptiCraft Heritage to the Nintendo DSi. Its VERY experimental, and it WILL run laggy and slow (around 5-17fps iirc), so dont expect anything crazy. I still havent writed a building guide, so you will have to download the builds from GitHub Actions (which you need a github account to do so).

Multiplayer is being worked on but it still wont work properly. Getting the game to run "smootly" and stable is currently the main goal

You can try it on real hardware by creating a folder called "opticraft" on the root of your SD card, and excracting the "data" folder from the Wii port (which you can find on Opti's website) into it, then copying the "OptiCraftDsi.nds" file into any folder on the SD card. You can launch it using Unlaunch/Astronaut, or nds-bootstrap via a front-end like TWiLight Menu++ or AKMenu-Next. Currently the game doesnt boot if you install it to the system menu if using HiyaCFW

Also it doesnt work on melonDS afaik

## Español:

En esta rama se esta vibecodeando un port del OptiCraft Heritage a la Nintendo DSi. Es MUY experimental y VA a correr lento y lageado, asi que no se esperen nada loco. Todavia no he escrito una guia para compilarlo, asi que tienen que descargar las builds de GitHub Actions (lo cual requiere tener una cuenta de github para descargarlos).

Se esta trabajando en el multijugador pero de momento no funciona bien. Actualmente, la meta principal es hacer que el juego corra "fluido" y estable.

Pueden probarlo en hardware real, creando una carpeta llamada "opticraft" en la raiz de la tarjeta SD, y extrayendo la carpeta "data" del port de la Wii (el cual se encuentra en la pagina web de Opti) adentro de ella, despues se copia el "OptiCraftDsi.nds" en cualquier directorio de la SD. Se puede abrir el juego a traves de Unlaunch/Astronaut, o de nds-bootstrap usando un front-end como TWiLight Menu++ o AKMenu-Next. De momento no se puede instalar el juego al menu del sistema si se usa HiyaCFW (osea, si se instalara, pero no cargara)

Ah y de momento no funciona en melonDS que yo me acuerde

# README ORIGINAL:

OptiCraft Heritage is a heavily modified, clean-room C++ implementation of classic Minecraft-era gameplay designed around portability, low-end hardware, and console-specific optimization.

This repository is not intended to be a line-for-line source translation. The runtime, platform layers, rendering paths, input backends, storage systems, user interface, asset loading, memory policies, and console support have been extensively reworked for the needs of this project.

## Project goals

- Keep the implementation portable across desktop PC, PlayStation 2, and Nintendo Wii.
- Preserve the intended classic gameplay and visual behavior where practical while allowing platform-specific adaptations.
- Run on constrained hardware through aggressive memory, rendering, chunk, and asset-loading optimizations.
- Keep platform code isolated behind explicit backends instead of scattering host-specific logic through the game code.
- Maintain a debuggable and production-oriented C++17 codebase.

## Clean-room implementation

OptiCraft Heritage is developed as a clean-room implementation. The project code is independently implemented in C/C++ and is heavily modified around its own runtime and platform architecture.

The project does not rely on original proprietary game source code as part of its implementation. Compatibility-oriented behavior may be reproduced from observable behavior, documented formats, protocol behavior, and independently developed interfaces.

This project is not affiliated with, endorsed by, or sponsored by Mojang Studios or Microsoft.

## Supported targets

### PC

The desktop build uses SDL2, OpenGL, and the shared platform abstraction layer. A dedicated 32-bit legacy profile is available for older SSE2-class CPUs and legacy OpenGL hardware.

### PlayStation 2

The PS2 build uses a native platform backend with PS2SDK support, GS-specific rendering, console-aware memory policies, asynchronous asset loading, platform storage, controller input, and optional VU-assisted terrain paths.

The expected USB application directory is:

```text
mass:/OptiCraftHeritage/
```

### Nintendo Wii

The Wii build uses devkitPPC/libogc and a native GX rendering path. The Homebrew Channel layout remains:

```text
apps/OptiCraft/
```

## Source layout

```text
src/
  client/       Client-side shared code
  java/         Java compatibility/runtime helpers
  net/          Game implementation
  platform/     Shared platform interfaces and backend selection
  pc/           Desktop-specific implementation
  ps2/          PlayStation 2 implementation
  wii/          Nintendo Wii implementation
  util/         Shared utility code

cmake/          Toolchains, source selection, and platform build logic
external/       Third-party dependencies
```

Platform targets deliberately select one implementation for each public backend. This keeps PC, PS2, and Wii implementations from accidentally entering the same link target.

## Building

CMake 3.21 or newer is required. Presets are defined in `CMakePresets.json`.

### Desktop

```text
cmake --preset gcc-debug
cmake --build --preset gcc-debug
```

For a normal optimized build:

```text
cmake --preset gcc-release
cmake --build --preset gcc-release
```

### 32-bit / legacy PC

The CMake presets do not hardcode an MSYS2 installation path. On Windows, use:

```text
build_gcc32.bat legacy
```

The batch file owns the local MSYS2 installation path instead of exposing it through CMake. To use another installation without editing the project:

```bat
set OPTICRAFT_MSYS2_ROOT=D:\Tools\msys64
build_gcc32.bat legacy
```

The accepted modes are `debug`, `release`, and `legacy`.

### PlayStation 2

```text
cmake --preset ps2-release
cmake --build --preset ps2-release
```

Use `ps2-debug` for a debug build. Asset staging remains a separate step so large runtime data is not recopied after every link.

### Nintendo Wii

```text
cmake --preset wii-release
cmake --build --preset wii-release
```

Use `wii-debug` for a debug build and `wii-bringup` for the minimal hardware/toolchain bring-up target.

## Development notes

OptiCraft Heritage contains substantial platform-specific changes compared with the behavior it reproduces. Examples include custom render backends, legacy UI work, low-memory chunk policies, console input layers, asset streaming, platform storage, audio backends, profiling, and console-specific performance tuning.

When changing shared systems, keep the platform abstraction boundary intact and avoid introducing PC-only assumptions into common code. Likewise, console-specific optimizations should remain behind platform policies or dedicated backends whenever possible.

## Third-party software

Third-party libraries are kept under `external/` and retain their respective licenses and notices. Review those licenses independently before redistributing binaries.
