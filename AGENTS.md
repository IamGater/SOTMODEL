# SOTModel for AI agents

SOTModel reads Sea of Thieves game archives directly (UE4.10 "Athena", pak v3 with AES + Oodle/LZ4/zlib).
Everything the UI can do is also exposed as **tools** that take JSON arguments and return JSON
(preview tools also return PNG images). Two ways to call them:

## 1. MCP server (recommended)

```
SOTModel.exe --mcp
```

Stdio MCP server (JSON-RPC 2.0, newline delimited). Example `.mcp.json` for Claude Code:

```json
{
  "mcpServers": {
    "sotmodel": { "command": "C:/path/to/SOTModel.exe", "args": ["--mcp"] }
  }
}
```

Claude Desktop / Cursor use the same `command` + `args`.

## 2. Command line

```
SOTModel.exe --cli tools                                  # every tool with its arguments
SOTModel.exe --cli <tool> [primary-argument] [key=value ...]
SOTModel.exe --cli <tool> --json "{\"path\": \"...\"}"
```

Output is JSON on stdout. Images are written to files (`out=<file.png>`, default `%TEMP%\sotmodel_<tool>.png`)
and reported as `{"image": "<path>"}`. From PowerShell pipe the output (`| Out-String`) so the shell waits for the
process (the exe is a GUI-subsystem program).

## 3. GUI automation

`SOTModel.exe <asset path> ...` opens the given assets in tabs. Adding `--screenshot <file.png> [--delay <s>]`
renders the window off-screen-safe (from the app's own back buffer), saves it once loading settles and exits —
handy for checking what the viewer shows without screen capture.

## Configuration

`%APPDATA%\SOTModel\Settings.ini` (written by the UI). Missing values are imported automatically from an
FModel installation (`%APPDATA%\FModel\AppSettings.json`): game directory, AES keys, Oodle DLL.

```
GameDir=E:\depot_2020
AesKey=0x...
OodlePath=C:\...\oodle-data-shared.dll
OutputDir=C:\...\SOTModel\Output
```

## Paths

Files are addressed by game path: `Athena/Content/Models/Props/Common/cmn_barrel_debris_02_a.uasset`.
Unreal package / object paths are accepted as well: `/Game/Models/Props/Common/cmn_barrel_debris_02_a`
or `/Game/Models/Props/Common/cmn_barrel_debris_02_a.cmn_barrel_debris_02_a`.

## Tools

| tool | purpose |
|---|---|
| `info` | mounted archives, file count, root folders, configuration |
| `search` | find files: substring terms (all must match) or `*`/`?` wildcards; `folder`, `extension`, paging |
| `list_folder` | browse the archive tree like FModel (subfolders with counts + files) |
| `find_assets_by_class` | packages whose main asset is a class (`StaticMesh`, `SkeletalMesh`, `AnimSequence`, `Texture2D`, `MaterialInstanceConstant`, `Blueprint`, `World`) |
| `get_package` | cheap overview: exports + classes, dependencies, decoded native summaries |
| `get_properties` | full FModel-style JSON (optionally one export) |
| `preview_texture` | decoded texture as a PNG image + format info |
| `get_mesh_info` | LODs, sections, material slots, bounds (and bones for skeletal meshes) |
| `preview_mesh` | renders a mesh with its textures to a PNG (`yaw`/`pitch` orbit, `wireframe`, `lod`) |
| `get_animation_info` | AnimSequence: frames, length, fps, compression, skeleton, animated bones (+ a mesh that can show it) |
| `preview_animation` | contact sheet PNG of an animation posed on a skeletal mesh (found automatically or `mesh=`) |
| `get_material` | material instance chain with every texture / scalar / vector parameter resolved |
| `export` | write files: textures (PNG/DDS), meshes (glTF/OBJ), animations (glTF with skeleton + skinned mesh), JSON |

Typical flow: `search` → `get_package` → `get_properties` / `preview_*` → `export`.

## Notes on the data

- Packages are legacy single-file `.uasset`/`.umap` (no `.uexp`/`.ubulk`); UE4 object version 483.
- 4.10 property tags do not name the element struct of arrays or the key/value types of maps. SOTModel guesses
  arrays of structs (tagged bags, or native layouts by element size) and shows maps as `UndecodedType` hex.
- Unreal units are centimetres, Z up, left handed. glTF exports are converted to metres, Y up.
