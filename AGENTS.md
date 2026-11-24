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
on the package's dependency graph; `--content "uses=<path>;class=<class>;property=..;value=..;name=..;folder=..;textkey=.."`
runs the search window's Content tab (the GUI face of `search_content`, same filters, results open on double-click; the Localization tab's "Find Assets Using This Text" / double-click fills `textkey`); 3D views open in a separate viewer window that follows the active tab
(`ViewerWindow=0` in Settings.ini / "Back to tab" keeps them inside the tabs; `--popout` forces the window for one run);
`--screenshot` saves that window's back buffer next to the main one as `<file>.viewer.png`.
More automation for the 3D views: `--pick u,v` clicks a map's viewport once at that fraction of its size (the pick is
logged), `--attach <mesh or particle system>@<socket>` (repeatable; without `@<socket>` the default hand socket) attaches
assets to the opened mesh / animation's sockets, `--tab` takes `archives` / `folders` / `packages` or 0-2,
`--variant <name>` switches a model tab's first matching material slot to that variant (the model view lists sibling material instances per slot: `name_01_a` -> `_01_b`, `_01_a_v02`, ...), `--time <hour>` opens maps at that hour of the day cycle, `--graph` opens model tabs with Graph shader on and the material parameters expanded.

The viewer also opens `BlueprintGeneratedClass` packages as one assembled actor (component templates attached by the
construction script tree, child actors included), draws particle systems with the 3D renderer (also on sockets and the
`ParticleSystemComponent`s of maps near the camera), edits material scalar / vector parameters live (Graph shader), and
opens `WwiseEvent` assets as the list of sounds the event plays (a bank's Events tab opens its events by name); events with switch containers get a value picker per switch / state group (FootwearType, surface, ...) that narrows the list, and "Play random" plays one of the listed sounds. Events with RTPC curves get a slider per game parameter and the resulting totals; sounds then play with the curves' Volume (gain) and Pitch (resampled copy; the waveform position and seeking account for the rate).
Maps get the sea's day cycle ("Day cycle" in the map toolbar, `time_of_day` in `preview_world`): island levels have no
sun or sky light, so the lighting curves of the region (`Curves/Lighting/<WSP|Wilds|Volcanic|Standard|...>`: sun colour
and intensity, sky light, sky dome horizon / zenith, fog) are evaluated for the hour (`ue/TimeOfDay`).
Animations and montages play their AnimNotifies ("Notifies" toggle, `ui/AnimNotifies.cpp`): `AnimNotify_WwiseSound`
events are decoded up front and played (a random one of the event's sounds, several at once), particle notifies spawn the
system at their socket (a bone missing on the preview mesh falls back to its nearest Skeleton ancestor; `Attached=false`
stays where it spawned); cosmetic item notifies (`SpawnCosmeticItem` / `Destroy` / `SwitchCosmeticItemAttachment` /
`SetCosmeticItemVisibility`) put the item Blueprint's meshes in the hand prop sockets, reset when the clip loops and
replayed silently when scrubbing. A marker strip under the frame slider shows every notify (blue sound, orange effect, green item, grey event; click to jump). Exporting an animation or montage (GUI or `export`, `animation_sounds`, Settings "AnimationSounds") also writes its notify sounds as `<name>_sounds/*.wav` (every variant), `<name>.mix.wav` (all of them at their times, one track) and `<name>.notifies.json` (the timeline). `get_anim_asset_info` lists each notify's `sound` / `particle` / `socket` / `itemAction`.
Blueprints add what native classes build at runtime: ship masts (`MastIntactMesh`, and per `Sail Parameters` entry the yard and the skeletal sail at their heights). Ship Blueprints (paths with `/Ships/`) open on the sea with the day cycle ("Sea & sky" toggle; water line at the actor origin).
"Export glTF" in the map / Blueprint toolbar writes what the view shows as one .glb: the assembled Blueprint, the whole
loaded map or only the part within a radius of the view target (optionally only the outliner's mesh filter), moved to
the origin (`WorldExportOptions::clip` / `recenter` / `meshFilter`; the landscape is reloaded, the viewer frees its CPU copy).
DataTable / CurveTable tabs show a sortable table with a filter per column (`>x`, `<=x`, `=x`, `!x` or a substring);
"Export CSV" writes the rows shown, in the shown order, and double-clicking a `/Game/` path opens that asset.

## Configuration

`%APPDATA%\SOTModel\Settings.ini` (written by the UI). Missing values are imported automatically from an
FModel installation (`%APPDATA%\FModel\AppSettings.json`): game directory, AES keys, Oodle DLL.

```
GameDir=E:\depot_2020
AesKey=0x...
OodlePath=C:\...\oodle-data-shared.dll
OutputDir=C:\...\SOTModel\Output
```

`UiAnimations=0` turns the interface transitions off (eased hovers, popups / menus / tooltips / windows fading in,
content fades, the sliding log, spinners); a `--screenshot` may otherwise catch one mid-way. The status bar and the
folder tree's expanding are deliberately not animated. `PreviewTextures=1` (default) shows thumbnails on the explorer
cards: textures, models (a small headless render of LOD 1 through `RenderMeshPreview`) and materials (their main
texture). The main window opens at ~90% of the work area, centred (not maximized).
Tooltips (`Tip`) show after the pointer rests 0.6 s on an item. Standard ImGui widgets ease their colours through
`GImGuiItemColorHook`, a small SOTModel patch in `third_party/imgui/imgui_widgets.cpp`.

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
| `preview_mesh` | renders a mesh with its textures to a PNG (`yaw`/`pitch` orbit, `wireframe`, `lod`). `collision=true` overlays the simple collision as green wireframe, `sockets=true` draws socket axes, `morphs={"Name":1}` applies morph target / body patch weights (the reply lists `morphsApplied`); `lod` picks the LOD |
| `get_animation_info` | AnimSequence: frames, length, fps, compression, skeleton, animated bones (+ a mesh that can show it) |
| `preview_animation` | contact sheet PNG of an animation posed on a skeletal mesh (found automatically or `mesh=`) |
| `get_anim_asset_info` | AnimMontage (sections, slot segments, notifies with times), BlendSpace / 1D / AimOffset (axes, samples; `x=`/`y=` adds the sample weights there), animation blueprint (baked state machines, transitions with crossfade, sequence / blend space players, notify events); plain sequences list their notifies |
| `preview_material` | material on a sphere (or `mesh=`); `graph=true` renders the translated node graph compiled as a pixel shader (stubbed engine inputs), `graph=false` the simple viewer shading; reports the compile result, `shader_source=true` returns the generated shader; `backdrop=true` (default) draws the checker floor / gradient the SceneColor / SceneDepth / DepthFade nodes read |
| `get_particle_system` | Cascade ParticleSystem: emitters (sprite / mesh / GPU / beam / ribbon, material + blend mode, mesh, alignment, duration / loops, subUV grid, spawn rate, bursts) and each module's baked distributions (`curves=true` adds sampled values) |
| `preview_particles` | contact sheet PNG of the CPU-simulated sprites (`time`, `frames`, `size`, `yaw`, `pitch`) |
| `preview_blend_space` | PNG of the blended pose of a blend space at `x`/`y` (`time` 0..1 of each sample, played in sync) |
| `get_world_info` | a map (.umap) as a scene: sublevels, placed meshes with Blueprint templates resolved, landscape components, instance counts, unresolved components (`filter=rock,-small` narrows the meshes). `path=Athena/Content/Maps/World_Layout/sot_world_layout_2.umap` is the whole sea: every island placed by the world layout; `islands=wsp_,-fort` / `region=[minX,minY,maxX,maxY]` select islands, `proxies=true` loads each island's merged LOD proxy instead (whole sea in seconds). Also lists lights by type, decal materials, particle emitters (systems with counts), height fog and the water level |
| `preview_world` | renders a whole map (meshes, Blueprint components, landscape terrain coloured by paint layers) to a PNG; orbit with `yaw`/`pitch`/`zoom` or place the camera with `eye`/`target`. Lit by the map's sun / sky light / nearest 32 point and spot lights with its exponential height fog and the sea plane at the water level (`lights`, `fog`, `water`); `gizmos=true` outlines light ranges, spot cones and decal boxes |
| `decompile_blueprint` | a Blueprint (`BlueprintGeneratedClass` package) as C++: class with C++ types, functions with structured control flow (if / else, loops, sequences), native calls with the game's signatures, events split out of the event graph; `raw=true` keeps the bytecode jumps as labels |
| `export_world` | a whole map (.umap with sublevels) or an assembled Blueprint class as one glTF scene for Blender: level nodes, every placed mesh with materials and textures, landscape. `center` [x,y,z] + `radius` (cm) export only that part of a map, moved to the origin (`recenter=false` keeps world coordinates). For `sot_world_layout_2` pick islands with `islands` / `region` or use `proxies`. Lights go out as KHR_lights_punctual, decals as empties scaled to their box (extras: material, half extents), fog and water level as scene extras |
| `get_world_layout` | the open world: every island / outpost / fort folder with its persistent map, sea, type, tile transform (location + yaw), bounds centre and radius (`filter`, `levels=true` for per-sublevel transforms) |
| `find_mesh_placements` | where a mesh is placed: levels using it (reference index, also through Blueprints), instance counts and world positions (open-world levels moved to their island's place) |
| `export_animations` | several AnimSequences of one skeleton (list or folder) into one .glb: the skinned mesh once, one animation per sequence |
| `export_datatable` | DataTable / CurveTable rows as JSON (paged) or a .csv file (struct fields flattened into columns) |
| `search_localization` | the game's translated strings (.locres) by text or key in any shipped language (`culture=de-DE`); FText values in JSON also carry `LocalizedString` |
| `get_localized_text` | one FText (namespace + key) in every language |
| `list_shaders` | compiled D3D11 (SM5) shaders of a material's shader maps or of `Engine/Content/GlobalShaderCache-PCD3D_SM5.bin`: index, stage, shader type, vertex factory, instructions, uniform buffers (`stage` / `type` / `vertex_factory` filters) |
| `decompile_shader` | one shader (`index`) as readable HLSL, `hlsl_exact` (raw uint4 lanes), `asm` (D3D disassembly), `reflection`, or raw `dxbc` with `save_to`; material shaders get their Material uniform buffer reads named (`cb1[19].x = ScalarParameter "Damage_Tiling"`) plus a register legend (`annotate=false` to skip) |
| `get_material_uniforms` | a material's uniform expression sets (one per shader map): each Material uniform buffer register as the parameter / folded expression it holds (vectors first, then scalars 4 per register), 2D / cube texture slots, which cb slot the shaders bind it to |
| `get_material_hlsl` | a material's node graph as readable HLSL (UE 4.10 translator style): parameters with values, Custom nodes, material functions inlined, static switches resolved, connected attributes; instances use the master graph with their values; material functions translate on their own (unbound texture inputs become texture parameters). Every node type used by the game's 569 materials is translated; SoT's own LumaAvg / CausticTexture / CausticTextureCoordinate become calls `AthenaLumaAvg()` etc. (engine-side values) |
| `get_material_graph` | the expression nodes of a material / function: type, caption, position (auto-laid out: cooked packages strip editor positions) and input links |
| `search_localization` | the game's translated strings (.locres) by text / key in any shipped language (en, de-DE, ru-RU, zh-CN, ...) |
| `get_localized_text` | one FText (namespace + key) in every language |
| `find_references` | packages that import the given one (meshes using a material, maps placing a Blueprint, ...); index built once, then cached |
| `get_dependency_graph` | nodes (path, class, side -1 referencer / 0 root / 1 reference, depth, how many the per-node `limit` left out) and [importer, imported] edges around a package, `depth` levels out; `direction` both / referencers / references |
| `search_content` | packages by main class, by what they use (`uses`), by property name / value (structs, arrays and DataTable rows searched; object references show and match as full paths), by localisation key (`text_key` from `search_localization`: packages holding an FText with it; the reference index records every locres key it finds in package bytes, an index cached by an older version is rebuilt once) |
| `get_behavior_tree` | an AI behavior tree (`Blueprints/AI/BehaviorTree`, 81 trees): composites / tasks from the root with their settings, each node's decorators (with the editor's cooked `CachedDescription`) and services, RunBehavior subtrees, the blackboard's keys and types (parents included); `format=text` indented tree or `json`. The GUI shows it as a collapsible tree beside the JSON (click a subtree / the blackboard to open it) |
| `get_physical_materials` | the physical surfaces named in `Athena/Config/DefaultEngine.ini` (PhysicsSettings PhysicalSurfaces: `SurfaceType4` = `WoodDeck`, the names footstep / impact sounds switch on) with the physical materials using each (friction, restitution, density); `users=true` adds who references each physical material (reference index). PhysicalMaterial JSON carries `Surface` with the name and the effective values |
| `export_web_ui` | the game's HTML interface (Coherent UI): writes the whole web root (`WebRoot` = front end menus / HUD / tooltips, `uiresources` = in-game HUD pages; or any path inside one) to `<outputDir>\Web\<game path>` keeping folders so pages open in a browser (no live data bindings); files already there with the same size are skipped; lists the `.html` pages |
| `serve_web_ui` | runs the web UI pages in a normal browser: starts a local server (127.0.0.1, the process's lifetime; `--cli serve_web_ui` keeps running until killed) serving the web roots from the archives and returns a page URL (default the front end). app/WebUi rewrites `coui://` links (web root files, fonts, `Texture2D'/Game/...'` decoded to PNG), injects a script standing in for the Coherent engine (engine.call / dispatcher requests answered with empty data, `featureConfig`, locres text for `engine.translate` and `data-l10n-id`). URL options: `?state=Lobby` / `PirateSelection` / ... picks the front end screen (`BootFlowStart<state>StateEvent`, default `Engage` = title), `?event=HUD:EventPlayerOpenEscapeMenu,...` sends game events once the page listens (menus.html opens the Esc menu by default) |
| `get_material` | material instance chain with every texture / scalar / vector parameter resolved |
| `export` | write files: textures (PNG/DDS), meshes (glTF/OBJ), animations (glTF with skeleton + skinned mesh), JSON; `raw=true` writes any file as stored. Meshes as glb carry their simple collision as a separate `Collision` node (`UCX_<mesh>`, `collision=false` to skip) |
| `get_audio_info` | Wwise audio: a `.wem` (codec, channels, rate, length) or a sound bank (`WwiseBank` package / `.bnk`) with its embedded media and its events (ids, names from the WwiseEvent assets) |
| `get_wwise_event` | a Wwise event (WwiseEvent asset path or name `Play_sfx_...`) resolved through its bank's HIRC: Play actions → containers → sounds / music tracks; each media embedded in the bank or streamed (`WwiseAudio/Streams/<id>.wem`); `output_dir` decodes them to .wav; `switches` lists the switch / state groups and each media's `when`, `switch="surface=WoodDeck,FootwearType=Pegleg"` keeps only what plays with those values (names are guessed by hashing words of event / audio asset names, plus physical surface names and every native identifier of the game's reflection; unknown ones are hex ids); `rtpcs` lists the game parameter curves on the event's nodes and their parent actor-mixers (parameter, property Volume dB / Pitch cents / LPF / HPF / ..., accumulation, points; `audio/WwiseRtpc.cpp` finds the InitialRTPC block by a strictly validated scan of the v134 layout), `rtpc="param=value,..."` evaluates them into `properties` (curve shapes approximated; the game parameters' defaults live in the Init bank, which the game files don't ship, so unset parameters sit at their curve's first point) |
| `export_audio` | decode Wwise audio to `.wav` (or `.ogg` for Wwise Vorbis); banks export every embedded sound |

Typical flow: `search` → `get_package` → `get_properties` / `preview_*` → `export`.

## Notes on the data

- FText values in package JSON carry `LocalizedString` in the configured language (`Language=` in Settings.ini, default en).
  Every 4.10 text history is read: generated texts (NamedFormat / OrderedFormat / ArgumentFormat, AsNumber / Percent /
  Currency, AsDate / Time / DateTime, Transform) carry `History` and their parts, and `SourceString` is the text
  rebuilt from them (format arguments substituted, numbers / dates printed).

- Packages are legacy single-file `.uasset`/`.umap` (no `.uexp`/`.ubulk`); UE4 object version 483.
- 4.10 property tags do not name the element struct of arrays or the key/value types of maps. SOTModel guesses
  arrays of structs (tagged bags whose tags are all `*Property`, StringAssetReference FStrings, or native layouts by
  element size). Maps (NumKeysToRemove 0, count, pairs) try key x value layouts (FName, FString, tagged struct, 4-byte
  int / object / float, Vector, Guid, byte) and keep the first that ends exactly at the property's end; they come out
  as `[{"Key": ..., "Value": ...}]`. Lazy object pointers are their GUID string.
- Loose (non-package) files open in ui/LooseFilePanels.cpp: text shows in the editor; .png/.jpg/.gif/.bmp/.tif/.ico are decoded with WIC (core/ImageDecode) and shown in the image panel; .ttf/.otf get a font panel (name table info, sample text at several sizes, Cyrillic, a glyph grid; the atlas font is made once per file and kept, since fonts can't be dropped mid-frame); files under Athena/Content/WebRoot/ or uiresources/ (pp/WebUi) get a panel that opens the page in the browser through the `serve_web_ui` server (a `file://` page stays white: the scripts wait for the game) or exports the root and shows it in Explorer, with the root's page list.
- Dev reports: `--cli scan <substring>` ends with `undecoded <type> <count> e.g. <package> <class>.<property>` for
  values still shown as hex (a full scan of all 105k packages leaves none) and prints the first BC6H textures;
  `--cli materials [substring]` lists material node types the HLSL translator leaves at 0 and compiles each material's
  preview shader with fxc, grouping the errors (all 569 compile; mt_unq_simple_flare's Custom code returns the
  float2 ScreenAlignedPosition from a float3 function, so inside the custom expressions the preview `#define`s it to a
  float4 variant that narrows to any declared type). `SOTMODEL_DUMP_SHADER=<file>` makes the viewer write a preview shader fxc rejected.
- Blueprint decompiler (ue/Blueprint.cpp): Kismet 4.10 bytecode -> statements, then structured flow from the
  execution-flow stack (function = Push(return label); Sequence pushes its later pins; ForEach / ForLoop bodies run
  inside Push(increment) and jump back to the head; WhileLoop pushes the head). Loops, if / else joins and latent resume
  points (FLatentActionInfo.Linkage) are rebuilt. Code a pushed chain jumps to (shared code after an outer join) belongs
  to the loop it pops back into and is repeated there; a jump to the function's return label is a `return` (never an
  if / else join inside loops or pushed chains). A function whose flow can't be structured keeps labelled jumps with
  `// control flow kept as jumps (reason)`. Event stubs (`UberGraphFrame->X = param; ExecuteUbergraph_X(N)`) give the
  event entries; the ubergraph is split into those events (frame variables renamed to the event params, variables shared
  between events listed as "event graph state"). Latent calls ending a chain continue inline; when a loop runs through
  latent actions the event is redone with the resumes as `Label_N:` sections (`// event graph kept whole (reason)` if
  even that fails). One-use `CallFunc_*` temporaries are inlined. Function parameters / locals follow the function's
  Children -> Next property chain (the order calls pass arguments in; export order differs). A non-virtual call of a
  parent's function this class overrides is `Super::X()`. Integer constants written to bools / enums use the target's
  type (`false`, `EFoo::Bar`); FGuid constants are hex. Native classes, calls, members and enums come from
  third_party/sot_reflection (embedded resource 103, ue/SotReflection; generated from old-sot's depot_2020 SDK index +
  sot-dump's SDK, see its README). The class of a call's context comes from the types of the current function's
  locals, class variables, parent Blueprints' variables, Blueprint struct members and native class / struct members;
  interface functions and calls on an unknown type fall back to the only class declaring the name. For the 198
  signatures whose out-parameter positions the dumps don't agree on, each call places them by its out temporaries
  (`CallFunc_<Function>_<Parameter>`). `--cli blueprints [substring]`
  decompiles every Blueprint and prints failures, the "kept as jumps" / "kept whole" reasons and the share of calls with
  a known signature with the unresolved ones (all 15,597: none kept as jumps, every one of the 17,808 calls with a signature).
- Texture formats in the game: BC7, DXT1/5, BC5, BC4, B8G8R8A8, G8, FloatRGBA, and BC6H (the skydome stars, unsigned)
  / PF_BC6H_SF16 (signed, the ships' deck-water height maps). BC6H decodes with third_party/bcdec (MIT / Unlicense)
  and is tone mapped like the float formats; DDS export writes DXGI 95 / 96.
- Material parameter collection parameters are `float` when the collection lists them under ScalarParameters.
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
  additive / modulate blend without depth writes. Scene depth / scene colour trace a virtual scene per screen uv
  (`GRAPH_BACKDROP_HLSL` in render/GraphShader.h: a checker floor at `DrawOptions::backdropFloor` fading into a dim gradient,
  far distance from the camera distance) through the inverse view-projection in the GraphFrame buffer; material previews
  (sphere tab, `preview_material` `backdrop=true`) draw that same backdrop, so refraction, SceneColor and depth fades match what
  is behind. Elsewhere SceneColor is the clear colour and SceneDepth the far distance. Clouds stay constants. GUI: "Graph shader" toggle in the model view (material tabs open a sphere with it on).
- Particles (Cascade): each module's `FRawDistribution*` property carries the cooked lookup table as tagged properties
  (`Table{Op, EntryCount, EntryStride, SubEntryStride, TimeScale, TimeBias, Values}`; Op 1 value, 2 random min..max,
  3 min or max); without a table the distribution export is read. SoT specifics: `Additional*` properties are per-group
  overrides (ignored), Lifetime 0 = lives until the emitter dies with relative time looping over Required `LoopTime`,
  SubImageIndex runs past the grid (wraps), VFX textures are often masks without alpha (brightness used as alpha).
  The preview simulation covers spawn / bursts / lifetime / size / color / velocity / cone / acceleration / drag /
  sphere + cylinder location / rotation / mesh rotation (+ rate) / subUV; other modules are listed as not previewed.
  Drawing: `PivotOffset` moves the quad ((0, -0.5) = bottom edge on the particle), velocity-aligned sprites put the
  texture top along the velocity, SubImageIndex tables spanning more than two grids (SoT keys 0..1279 over the life)
  play at 30 fps instead of over the life, `*_Blend` SubUV cross-fades frames, "Desaturate" materials grey the texture
  and tint it with `EmissiveColourMultiply`, mesh emitters (TypeDataMesh) draw their mesh per particle (size = scale).
  SoT has no Beam / Ribbon emitters (a scan of all ~920 systems): only sprites, GPU sprites, meshes and 2 AnimTrails
  (weapon swing trails, not drawn). No soft-particle depth fade.
  Simulated besides the basics: `*_Seeded` variants (as their base module), RotationRateMultiplyLife and
  AccelerationDragScaleOverLife (scale the running rate / drag every frame, as UE does), Orbit (offset turned by
  rotation + rate), SizeScaleBySpeed, AttractorPointGravity (approximation: Strength fading to 0 at Radius),
  SphereSlice as a sphere; VelocityInheritParent / SpawnPerUnit do nothing for the still preview emitter. Module
  properties that reference a Distribution object (not an inline raw distribution) are read as curves too.
  Vector fields: VectorFieldLocal loads its `VectorFieldStatic` (native data after the properties: bulk data of
  SizeX*SizeY*SizeZ x 4 halfs, xyz + unused, over `Bounds`; SoT has one, `vfx_vf_noise`); the force is the trilinear
  sample in the module's local space (RelativeTranslation / Rotation in turns / Scale3D, tiling per axis) times
  Intensity, VectorFieldScale (+ OverLife), and Tightness pulls the velocity toward it; Rotation / RotationRate turn
  the field. Orientation: OrientationAxisLock (EPAL_X/Y/Z lie flat in that plane, NEGATIVE mirrored, ROTATE_* spin
  about the axis facing the camera) and AlignRotationToVelocity (texture top along the velocity) go through
  `ParticleQuadAxes`, shared by the GPU draw and the software renderer (which rasterises any projected parallelogram).
  SoT spawn modules: LocationWorldOffset as Location, VelocityConeEmitterPoint as VelocityCone with VelocityScale +
  AdditiveVelocity, LocationEmitter at a random live particle of `EmitterName`, ParametersOverVelocity / OverCustom
  scale spawn rate, lifetime, size, start velocity and opacity at input 0 (the still emitter) / 1 (gameplay value
  assumed full), SizeOverVelocity reads `SizeMultiplier` at speed / spawn speed (0..1; the input range is a guess).
  Modules a standalone preview cannot show carry a reason (`ParticleModuleInfo::previewNote`, `preview_note` in
  `get_particle_system`, the module tooltip, and `--cli particles` prints them as "not previewable"): Collision(GPU),
  Light, LocationBoneSocket, WindInfluence, ParameterDynamic (material input only), MurkyColor, SpawnRepeatPerPoint,
  LocationVelocityComputeShaderOutput and TypeDataAnimTrail. Every other module class in the game is simulated.
- Explorer thumbnails (`App::Thumbnail`, async, 160 px): textures, meshes (headless render, LOD 1), materials (main
  texture), particle systems (software sprite render ~2 s in), .wem (waveform), Blueprint classes (assembled actor,
  no water / fog).
- Settings: `AccentColor=RRGGBB` (Settings window presets + picker; `ApplyAccent` in Shell.cpp updates `kAccent` and
  the ImGui style), `WindowPlacement=x,y,w,h,maximized` (saved on exit; first start uses ~90% of the work area).
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
  LOD `{PositionDelta, TangentZDelta, SourceIdx}` (28 bytes, SourceIdx = render vertex).
- Body-shape patches (SoT `MeshPatchAsset`, listed in a wardrobe mesh's `MeshPatchAssets`: Female_C ... Male_W): after
  the properties the guid flag, strip flags, a LOD count, then per LOD a bulk array (u32 count, u32 element size 24,
  u32 count) of `{half3 position delta, half3 normal delta, u16 x2, u32 0, u32 index}` plus a u32. The index is the
  imported vertex: the skeletal LOD's `MeshToImportVertexMap` (kept in cooked SoT meshes, now in
  `MeshLod::importVertex`) maps render vertices to it, so one entry moves every split vertex at that point. They are
  returned as morph targets (`bodyPatch: true` in get_mesh_info) and drive the same sliders / `preview_mesh` `morphs`.
- Lights / decals / fog: UE 4.10 decals are a unit box scaled by the component (no DecalSize yet). Point / spot lights with
  inverse-squared falloff are in lumens (glTF candela = lumens / 4pi). The open world's sun, sky light and the two
  ExponentialHeightFog components (above / under water) are in `wsp_common/wsp_lighting`, the FFT water in `wsp_water`;
  a whole-sea load adds both.
- Unreal units are centimetres, Z up, left handed. glTF exports are converted to metres, Y up.
