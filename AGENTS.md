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
handy for checking what the viewer shows without screen capture. `--shaders <path>` opens the compiled shaders
of a material package or of `Engine/Content/GlobalShaderCache-PCD3D_SM5.bin` instead.

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
| `get_world_info` | a map (.umap) as a scene: sublevels, placed meshes with Blueprint templates resolved, landscape components, instance counts, unresolved components (`filter=rock,-small` narrows the meshes) |
| `preview_world` | renders a whole map (meshes, Blueprint components, landscape terrain coloured by paint layers) to a PNG; orbit with `yaw`/`pitch`/`zoom` or place the camera with `eye`/`target` |
| `decompile_blueprint` | a Blueprint (`BlueprintGeneratedClass` package) as C++-like code: variables, functions with parameters and locals, Kismet bytecode as statements with labelled jumps |
| `export_world` | a whole map (.umap with sublevels) as one glTF scene for Blender: level nodes, every placed mesh with materials and textures, landscape |
| `export_animations` | several AnimSequences of one skeleton (list or folder) into one .glb: the skinned mesh once, one animation per sequence |
| `export_datatable` | DataTable / CurveTable rows as JSON (paged) or a .csv file (struct fields flattened into columns) |
| `search_localization` | the game's translated strings (.locres) by text or key in any shipped language (`culture=de-DE`); FText values in JSON also carry `LocalizedString` |
| `get_localized_text` | one FText (namespace + key) in every language |
| `list_shaders` | compiled D3D11 (SM5) shaders of a material's shader maps or of `Engine/Content/GlobalShaderCache-PCD3D_SM5.bin`: index, stage, shader type, vertex factory, instructions, uniform buffers (`stage` / `type` / `vertex_factory` filters) |
| `decompile_shader` | one shader (`index`) as readable HLSL, `hlsl_exact` (raw uint4 lanes), `asm` (D3D disassembly), `reflection`, or raw `dxbc` with `save_to` |
| `get_material` | material instance chain with every texture / scalar / vector parameter resolved |
| `export` | write files: textures (PNG/DDS), meshes (glTF/OBJ), animations (glTF with skeleton + skinned mesh), JSON; `raw=true` writes any file as stored |
| `get_audio_info` | Wwise audio: a `.wem` (codec, channels, rate, length) or a sound bank (`WwiseBank` package / `.bnk`) with its embedded media |
| `export_audio` | decode Wwise audio to `.wav` (or `.ogg` for Wwise Vorbis); banks export every embedded sound |

Typical flow: `search` → `get_package` → `get_properties` / `preview_*` → `export`.

## Notes on the data

- Packages are legacy single-file `.uasset`/`.umap` (no `.uexp`/`.ubulk`); UE4 object version 483.
- 4.10 property tags do not name the element struct of arrays or the key/value types of maps. SOTModel guesses
  arrays of structs (tagged bags, or native layouts by element size) and shows maps as `UndecodedType` hex.
- Shader code in shader maps is LZ4-compressed (Sea of Thieves addition, uncompressed size after the resource).
  Reflection names are stripped: constant buffers, textures and samplers keep their register numbers (`cb1_cb[42]`,
  `t3`, `s5`); inputs / outputs keep their semantics. The decompiled HLSL recompiles with fxc for almost every
  shader; geometry shaders, UAVs, groupshared memory and atomics are translated too.
- Unreal units are centimetres, Z up, left handed. glTF exports are converted to metres, Y up.
