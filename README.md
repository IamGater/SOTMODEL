# SOTModel

Asset explorer for Sea of Thieves. It reads the game's pak archives directly
(UE4.10 "Athena", pak v3, AES + Oodle / LZ4 / zlib) and shows what is inside.

- Browse archives, folders and packages; full-text search over packages, references, localization, content and decompiled code.
- 3D viewers for meshes, animations, particle systems, Blueprints (as an assembled actor) and maps with the sea's day cycle.
- Materials with a live graph shader and editable parameters, textures, Wwise audio, behavior trees, the game's HTML UI.
- Everything the UI does is also available as JSON tools for scripts and AI agents.

## Build

Windows 10/11 with the Visual Studio C++ toolset and the Windows SDK (no CMake needed):

```
.\build.ps1          # release -> bin\SOTModel.exe
.\build.ps1 -Debug
.\build.ps1 -Clean
```

## Run

Start `SOTModel.exe`, pick the game's archives on the Archives tab and press Load.

```
SOTModel.exe --mcp                 # MCP server for Claude, Cursor, ...
SOTModel.exe --cli tools           # list every tool
SOTModel.exe --cli <tool> [args]   # run a tool from the command line
```

See [AGENTS.md](AGENTS.md) for the full tool list and command-line options.