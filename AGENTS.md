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

The GUI starts on the Archives tab and mounts only when Load is pressed; asset paths, `--folder` or `--tab 1/2` on the
command line mount right away. `SOTModel.exe <asset path> ...` opens the given assets in tabs. Adding `--screenshot <file.png> [--delay <s>]`
renders the window off-screen-safe (from the app's own back buffer), saves it once loading settles and exits —
handy for checking what the viewer shows without screen capture. `--shaders <path>` opens the compiled shaders
of a material package or of `Engine/Content/GlobalShaderCache-PCD3D_SM5.bin` instead; `--material-hlsl <path>`
opens a material's graph translated to HLSL next to its node graph; `--references <path>` opens the References tab
on the package's dependency graph; 3D views open in a separate viewer window that follows the active tab
(`ViewerWindow=0` in Settings.ini / "Back to tab" keeps them inside the tabs; `--popout` forces the window for one run);
`--screenshot` saves that window's back buffer next to the main one as `<file>.viewer.png`.

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
| `get_mesh_info` | LODs, sections, material slots, bounds (and bones for skeletal meshes). Includes the simple collision: BodySetup (static) or PhysicsAsset (skeletal) box / sphere / capsule / convex counts, sockets (mesh + skeleton: bone, relative and mesh-space transform) and morph targets (vertices per LOD) |
| `preview_mesh` | renders a mesh with its textures to a PNG (`yaw`/`pitch` orbit, `wireframe`, `lod`). `collision=true` overlays the simple collision as green wireframe, `sockets=true` draws socket axes, `morphs={"Name":1}` applies morph target weights; `lod` picks the LOD |
| `get_animation_info` | AnimSequence: frames, length, fps, compression, skeleton, animated bones (+ a mesh that can show it) |
| `preview_animation` | contact sheet PNG of an animation posed on a skeletal mesh (found automatically or `mesh=`) |
| `get_anim_asset_info` | AnimMontage (sections, slot segments, notifies with times), BlendSpace / 1D / AimOffset (axes, samples; `x=`/`y=` adds the sample weights there), animation blueprint (baked state machines, transitions with crossfade, sequence / blend space players, notify events); plain sequences list their notifies |
| `preview_material` | material on a sphere (or `mesh=`); `graph=true` renders the translated node graph compiled as a pixel shader (stubbed engine inputs), `graph=false` the simple viewer shading; reports the compile result, `shader_source=true` returns the generated shader |
| `get_particle_system` | Cascade ParticleSystem: emitters (sprite / mesh / GPU / beam / ribbon, material + blend mode, mesh, alignment, duration / loops, subUV grid, spawn rate, bursts) and each module's baked distributions (`curves=true` adds sampled values) |
| `preview_particles` | contact sheet PNG of the CPU-simulated sprites (`time`, `frames`, `size`, `yaw`, `pitch`) |
| `preview_blend_space` | PNG of the blended pose of a blend space at `x`/`y` (`time` 0..1 of each sample, played in sync) |
| `get_world_info` | a map (.umap) as a scene: sublevels, placed meshes with Blueprint templates resolved, landscape components, instance counts, unresolved components (`filter=rock,-small` narrows the meshes). `path=Athena/Content/Maps/World_Layout/sot_world_layout_2.umap` is the whole sea: every island placed by the world layout; `islands=wsp_,-fort` / `region=[minX,minY,maxX,maxY]` select islands, `proxies=true` loads each island's merged LOD proxy instead (whole sea in seconds). Also lists lights by type, decal materials, height fog and the water level |
| `preview_world` | renders a whole map (meshes, Blueprint components, landscape terrain coloured by paint layers) to a PNG; orbit with `yaw`/`pitch`/`zoom` or place the camera with `eye`/`target`. Lit by the map's sun / sky light / nearest 32 point and spot lights with its exponential height fog and the sea plane at the water level (`lights`, `fog`, `water`); `gizmos=true` outlines light ranges, spot cones and decal boxes |
| `decompile_blueprint` | a Blueprint (`BlueprintGeneratedClass` package) as C++-like code: variables, functions with parameters and locals, Kismet bytecode as statements with labelled jumps |
| `export_world` | a whole map (.umap with sublevels) as one glTF scene for Blender: level nodes, every placed mesh with materials and textures, landscape. For `sot_world_layout_2` pick islands with `islands` / `region` or use `proxies`. Lights go out as KHR_lights_punctual, decals as empties scaled to their box (extras: material, half extents), fog and water level as scene extras |
| `get_world_layout` | the open world: every island / outpost / fort folder with its persistent map, sea, type, tile transform (location + yaw), bounds centre and radius (`filter`, `levels=true` for per-sublevel transforms) |
| `find_mesh_placements` | where a mesh is placed: levels using it (reference index, also through Blueprints), instance counts and world positions (open-world levels moved to their island's place) |
| `export_animations` | several AnimSequences of one skeleton (list or folder) into one .glb: the skinned mesh once, one animation per sequence |
| `export_datatable` | DataTable / CurveTable rows as JSON (paged) or a .csv file (struct fields flattened into columns) |
| `search_localization` | the game's translated strings (.locres) by text or key in any shipped language (`culture=de-DE`); FText values in JSON also carry `LocalizedString` |
| `get_localized_text` | one FText (namespace + key) in every language |
| `list_shaders` | compiled D3D11 (SM5) shaders of a material's shader maps or of `Engine/Content/GlobalShaderCache-PCD3D_SM5.bin`: index, stage, shader type, vertex factory, instructions, uniform buffers (`stage` / `type` / `vertex_factory` filters) |
| `decompile_shader` | one shader (`index`) as readable HLSL, `hlsl_exact` (raw uint4 lanes), `asm` (D3D disassembly), `reflection`, or raw `dxbc` with `save_to`; material shaders get their Material uniform buffer reads named (`cb1[19].x = ScalarParameter "Damage_Tiling"`) plus a register legend (`annotate=false` to skip) |
| `get_material_uniforms` | a material's uniform expression sets (one per shader map): each Material uniform buffer register as the parameter / folded expression it holds (vectors first, then scalars 4 per register), 2D / cube texture slots, which cb slot the shaders bind it to |
| `get_material_hlsl` | a material's node graph as readable HLSL (UE 4.10 translator style): parameters with values, Custom nodes, material functions inlined, static switches resolved, connected attributes; instances use the master graph with their values; material functions translate on their own |
| `get_material_graph` | the expression nodes of a material / function: type, caption, position (auto-laid out: cooked packages strip editor positions) and input links |
| `search_localization` | the game's translated strings (.locres) by text / key in any shipped language (en, de-DE, ru-RU, zh-CN, ...) |
| `get_localized_text` | one FText (namespace + key) in every language |
| `find_references` | packages that import the given one (meshes using a material, maps placing a Blueprint, ...); index built once, then cached |
| `get_dependency_graph` | nodes (path, class, side -1 referencer / 0 root / 1 reference, depth, how many the per-node `limit` left out) and [importer, imported] edges around a package, `depth` levels out; `direction` both / referencers / references |
| `search_content` | packages by main class, by what they use (`uses`), by property name / value (structs, arrays and DataTable rows searched) |
| `get_material` | material instance chain with every texture / scalar / vector parameter resolved |
| `export` | write files: textures (PNG/DDS), meshes (glTF/OBJ), animations (glTF with skeleton + skinned mesh), JSON; `raw=true` writes any file as stored. Meshes as glb carry their simple collision as a separate `Collision` node (`UCX_<mesh>`, `collision=false` to skip) |
| `get_audio_info` | Wwise audio: a `.wem` (codec, channels, rate, length) or a sound bank (`WwiseBank` package / `.bnk`) with its embedded media |
| `export_audio` | decode Wwise audio to `.wav` (or `.ogg` for Wwise Vorbis); banks export every embedded sound |

Typical flow: `search` → `get_package` → `get_properties` / `preview_*` → `export`.

## Notes on the data

- FText values in package JSON carry `LocalizedString` in the configured language (`Language=` in Settings.ini, default en).

- Packages are legacy single-file `.uasset`/`.umap` (no `.uexp`/`.ubulk`); UE4 object version 483.
- 4.10 property tags do not name the element struct of arrays or the key/value types of maps. SOTModel guesses
  arrays of structs (tagged bags, or native layouts by element size) and shows maps as `UndecodedType` hex.
- Shader code in shader maps is LZ4-compressed (Sea of Thieves addition, uncompressed size after the resource).
  Reflection names are stripped: constant buffers, textures and samplers keep their register numbers (`cb1_cb[42]`,
  `t3`, `s5`); inputs / outputs keep their semantics. The decompiled HLSL recompiles with fxc for almost every
  shader; geometry shaders, UAVs, groupshared memory and atomics are translated too.
- The open world (`sot_world_layout_2`): cooked tiles carry no world-composition offsets (`worldTile` in `get_package` is
  0,0). Island placement is recovered like Athena's tiles.py: the layout keeps `Copied<Name>` NavMeshBoundsVolumes of every
  island level (`OriginatingLevelName`); matching each copy to the level's own volume (by name, else by brush extent) gives
  the level's yaw + translation. 95 islands; the Legendary Hideout and the Maiden Voyage island are not part of it.
  CLI arrays: `key=[1,2,3]`.
- Collision: cooked 4.10 static meshes keep their BodySetup as an export under the mesh (not a tagged property).
  Box X/Y/Z are full sizes; capsules (Sphyl) are along Z with Length = the cylinder part. Convex elements only store
  their points (VertexData); SOTModel rebuilds the hull. Skeletal meshes: PhysicsAsset bodies at their bones' reference pose.
- Montages / blend spaces / anim blueprints: notify and section times are `LinkValue` (absolute seconds). Blend space
  `GridSamples` index = X*(GridNumY+1)+Y, each `{Indices[3], Weights[3]}` is a static array (one tagged property per
  element, read with FindValue(..., arrayIndex)); weights at a point are bilinear over the 4 surrounding grid points.
  AnimBlueprintGeneratedClass.BakedStateMachines has states / transitions; the class defaults hold the
  `AnimGraphNode_*` player structs (node indices can't be mapped to states in cooked data). The GUI shows montages as a
  timeline over the posed mesh, blend spaces as a draggable grid with the blended pose, anim blueprints as a state graph.
- Graph shaders (render/GraphShader.cpp): `get_material_hlsl` output + a stub UE environment (FMaterialPixelParameters,
  View / Primitive structs, Texture2DSample & co, Transform*_AToB, noise, scene textures) -> `GraphPSMain` (ps_5_0).
  Parameters become `static` constants, `Tex_*` get t-registers in declaration order with `Tex_*Sampler` -> one wrap
  sampler, everything after `CalcVertexMaterialInputs` is dropped. Blend: masked clips OpacityMask at 1/3, translucent /
  additive / modulate blend without depth writes. Scene depth / scene colour / clouds are constants, so depth fades and
  refraction look flat. GUI: "Graph shader" toggle in the model view (material tabs open a sphere with it on).
- Particles (Cascade): each module's `FRawDistribution*` property carries the cooked lookup table as tagged properties
  (`Table{Op, EntryCount, EntryStride, SubEntryStride, TimeScale, TimeBias, Values}`; Op 1 value, 2 random min..max,
  3 min or max); without a table the distribution export is read. SoT specifics: `Additional*` properties are per-group
  overrides (ignored), Lifetime 0 = lives until the emitter dies with relative time looping over Required `LoopTime`,
  SubImageIndex runs past the grid (wraps), VFX textures are often masks without alpha (brightness used as alpha).
  The preview simulation covers spawn / bursts / lifetime / size / color / velocity / cone / acceleration / drag /
  sphere + cylinder location / rotation / subUV; other modules are listed as not previewed.
- Material uniform buffer (ue/Shaders.cpp): each shader map's FUniformExpressionSet (UE 4.10 order: vector, scalar,
  2D texture, cube texture expressions, parameter collection guids, 4 per-frame arrays; each expression = type FName +
  fields) is found by scanning for count + known expression FName. Layout: one float4 per vector expression, then the
  scalars packed 4 per float4. The cb slot comes from FMaterialShader's MaterialUniformBuffer (u16 BaseIndex + u32
  bIsBound) 12 bytes after the shader resource on SoT (a u32, a zero u32, a u32 come first).
- Dependency graph (app/DepGraph.cpp): references come from the package's own import table (`/Game/...` Package
  imports; the class is that of the object imported from it), referencers from the ReferenceIndex (class = the
  referencer's main export). GUI: References tab > Graph (referencers left, references right, Depth 1-3); click a node
  to centre on it (Back returns), double-click opens it, right-click for the file menu, right / middle drag pans.
- Morph targets (UMorphTarget): after the tagged properties a 4-byte object-guid flag, the strip flags (2 bytes), then per
  LOD `{PositionDelta, TangentZDelta, SourceIdx}` (28 bytes, SourceIdx = render vertex). SoT's `MeshPatchAsset` exports
  in the same packages are a different (custom) thing.
- Lights / decals / fog: UE 4.10 decals are a unit box scaled by the component (no DecalSize yet). Point / spot lights with
  inverse-squared falloff are in lumens (glTF candela = lumens / 4pi). The open world's sun, sky light and the two
  ExponentialHeightFog components (above / under water) are in `wsp_common/wsp_lighting`, the FFT water in `wsp_water`;
  a whole-sea load adds both.
- Unreal units are centimetres, Z up, left handed. glTF exports are converted to metres, Y up.
