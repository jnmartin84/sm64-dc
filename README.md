# Super Mario 64 for the Sega Dreamcast

- This repo contains a full decompilation of Super Mario 64 (J), (U), and (E) with minor exceptions in the audio subsystem.
- Naming and documentation of the source code and data structures are in progress.
- Efforts to decompile the Shindou ROM steadily advance toward a matching build.

This repo does not include all assets necessary for compiling the game.
A prior copy of the game is required to extract the assets.

## Building for Sega Dreamcast

Just use the builder found here: https://colab.research.google.com/drive/1JsN-2JOu1tzzHKowCJJRgtL01XgejZTE

Nobody follows directions anyway. If you want to build it the real way, I'll leave the reset of the docs, but I know they'll go unread.

**Fixed textures live in the psp/textures/ folder. copy these into textures/, overwrite the extracted ones, and rebuild**

1. Install the Dreamcast toolchain https://github.com/KallistiOS/KallistiOS/tree/master/utils/dc-chain.
2. Install python3
3. Place a Super Mario 64 ROM called `baserom.<VERSION>.z64` into the repository's root directory for asset extraction, where `VERSION` can be `us`, `jp`, or `eu`. **Note: Only US supported**
4. Run `make TARGET_DC=1`
5. This will produce an ELF ready to be loaded with dc-tool / dc-load.
6. To make a CDI, run `make TARGET_DC=1 cdi` . You need `mkdcdisc` installed and on your path.


## Project Structure

```
sm64
├── actors: object behaviors, geo layout, and display lists
├── asm: handwritten assembly code, rom header
│   └── non_matchings: asm for non-matching sections
├── assets: animation and demo data
│   ├── anims: animation data
│   └── demos: demo data
├── bin: C files for ordering display lists and textures
├── build: output directory
├── data: behavior scripts, misc. data
├── doxygen: documentation infrastructure
├── enhancements: example source modifications
├── include: header files
├── levels: level scripts, geo layout, and display lists
├── lib: SDK library code
├── rsp: audio and Fast3D RSP assembly code
├── sound: sequences, sound samples, and sound banks
├── src: C source code for game
│   ├── audio: audio code
│   ├── buffers: stacks, heaps, and task buffers
│   ├── engine: script processing engines and utils
│   ├── game: behaviors and rest of game source
│   ├── goddard: Mario intro screen
│   ├── menu: title screen and file, act, and debug level selection menus
│   └── pc: port code, audio and video renderer
├── text: dialog, level names, act names
├── textures: skybox and generic texture data
└── tools: build tools
```

## Contributing

Pull requests are welcome. For major changes, please open an issue first to
discuss what you would like to change.

Run `clang-format` on your code to ensure it meets the project's coding standards.
