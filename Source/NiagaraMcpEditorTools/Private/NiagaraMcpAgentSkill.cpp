#include "NiagaraMcpAgentSkill.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(NiagaraMcpAgentSkill)

UNiagaraMcpVfxAuthoringSkill::UNiagaraMcpVfxAuthoringSkill()
{
	Description = TEXT("Build, edit and verify Niagara VFX and materials with the NiagaraMcpEditorTools toolsets (observe -> edit -> verify).");

	Instructions = TEXT(R"SKILL(# Niagara VFX authoring with NiagaraMcpEditorTools

Toolsets (call through call_tool, or ExecuteBatch for many steps):
- NiagaraMcpEditorTools.NiagaraMcpEditorTools  (systems, emitters, stacks, module graphs, verification, batch)
- NiagaraMcpEditorTools.MaterialMcpEditorTools (materials, material functions, instances, preview, validation)
- NiagaraMcpEditorTools.AssetMcpEditorTools    (textures, render targets, parameter collections, effect types,
  any asset property, duplicate, references)
Top-level image tools (return a real image, prefer them for looking): niagara_capture_preview, material_capture_preview.

## Loop: observe -> edit -> verify
1. Observe: GetSystemSummary, GetStack (inputs with values in SetModuleInput syntax), ExportSystemSpec (whole system
   as compact JSON), GetSystemProperties / GetEmitterProperties / GetRendererProperties (valid names + enum values),
   FindModuleScripts, FindDynamicInputs, FindAssets, GetAssetReferences (ConsolidateAssets merges duplicates, RenameAssets renames and fixes references, FixRedirectors cleans leftover redirectors).
2. Edit. Each write is one undo transaction (Undo/Redo). For more than ~3 edits use ExecuteBatch:
   [{"Id":"e","Tool":"AddModule","Arguments":"{...}"}, {"Tool":"SetModuleInput","Arguments":"{\"Module\": \"$e.moduleId\", ...}"}]
   (steps reference earlier results as "$Id.field"; one save at the end; bRollbackOnError undoes everything on failure;
   bDryRun checks names first).
3. Verify after every meaningful change:
   - ValidateSystem: compile errors, stack issues (fixes -> ApplyStackIssueFix), common mistakes.
   - GetParticleCounts (does it spawn / die on time?) and GetParticleAttributes (Position, Velocity, Color, Age...
     min/max/mean over time: does it move up? fade out?).
   - niagara_capture_preview: LOOK at it at several Times (start, peak, end); adjust CameraYaw/Pitch/Distance.
   - Materials: ValidateMaterial (bCompile true after edits), GetMaterialStats, material_capture_preview
     (FrameCount > 1 for panners/flipbooks).
   - DiffSystems compares two systems (e.g. before/after or variant vs. original).

)SKILL") TEXT(R"SKILL(## Seeing and measuring (v0.5)
- niagara_capture_preview returns the image AND a caption with per-tile numbers: particles, coverage %, luminance,
  clipped %, mean color, screen bounds, and a hint for empty tiles ("0 particles: spawn/lifetime" vs "N particles but
  nothing drawn: material, alpha, size, facing or bounds"). Read the caption, not only the picture.
  Options: Background Grey for dark/translucent smoke; CameraYaws "0, 90" for side + front in one call (row per time);
  QualityLevel Low/Medium/High/Epic/Cinematic to check scalability; ReferenceImage <png path> puts the user's
  reference as the first tile. Pick Times around the key moments (e.g. impact -0.03, 0, +0.03, +0.07, +0.13 s).
- Nothing visible? DiagnoseVisibility(System) first: it walks compile -> enabled/scalability -> spawn -> lifetime ->
  renderer feedback -> material usage flag -> alpha/size -> bounds and returns the first failure with the next call.
- Did my edit change anything visible? niagara_compare_preview with a BaselineName before and after (deterministic
  emitters compare exactly). Cost: niagara_cost_view (shader complexity, % of red pixels = overdraw risk) and
  AnalyzeCost (VM ops, registers, ms per step, GPU stage timings); GetCompiledHlsl to read the generated code.
- GetParticleAttributes reads CPU, GPU and lightweight emitters (GPU via a sim cache): use it to confirm motion
  (Velocity, Position), fades (Color alpha) and sizes numerically.
- Textures: texture_view shows color + R/G/B/A channels (+ flipbook grid). BakeFlipbook renders a simulation into a
  SubUV atlas texture (cheap sprite version of an expensive effect).

## Reuse and restyle
- ExportSystemSpec -> ApplySystemSpec (Merge / Replace, bDryRun) copies or restores a whole setup; DiffSystems checks it.
- SaveEmitterAsAsset turns an emitter into a reusable template for AddEmitter.
- RecolorEffect(HueShiftDegrees / Tint) recolors local colors, color curves and user colors in one call.
- GetUserParameterUsage before renaming/removing a user parameter. Lightweight (stateless) emitters:
  AddLightweightEmitter, GetStatelessEmitterInfo, SetStatelessModuleProperty. Niagara Data Channels:
  CreateNiagaraDataChannel / GetNiagaraDataChannel (hook up through a DataChannelRead/Write user data interface).

)SKILL") TEXT(R"SKILL(## Working alongside a person
- Start every turn with GetChangesSince(SinceCursor = last NextCursor): UserChangeCount > 0 means the artist edited
  something in the editor; re-read those assets (GetStack / ExportSystemSpec) before changing them.
- Before experiments: CreateRestorePoint("before_x"); revert the whole experiment with RollbackToRestorePoint
  (refuses if the user edited in between, unless bAllowUserChanges). Asset creation is not undoable.
- Recipes: ready ExecuteBatch scripts (fireball, spark_burst, smoke_puff, magic_orb, impact_flash, charge_burst) as MCP resources
  niagara-mcp://recipes/<name>; fill {{SystemPath}} / {{SpriteMaterial}} and run them, then tune.
- ExecuteBatch Expect="Compiles:<sys>;Valid:<sys>;Particles:<sys>:<Emitter|Any>@0.5>=1" verifies the result and rolls
  back when it fails.

)SKILL") TEXT(R"SKILL(## Error codes (errors start with [CODE]) and what to do
- EMITTER_NOT_FOUND / MODULE_NOT_FOUND / INPUT_NOT_FOUND / PROPERTY_NOT_FOUND: use the suggested name ("Did you mean")
  or list names with GetSystemSummary / GetStack / Get*Properties.
- MODULE_AMBIGUOUS: pass StackName or the ModuleId from GetStack.
- ASSET_NOT_FOUND: FindAssets. ASSET_EXISTS: pick a new path or edit the existing asset. INVALID_PATH: /Game/Folder/Name.
- EDITOR_OPEN: close that asset's editor (or ask the user to), then retry.
- PIE_ACTIVE: stop Play In Editor first. SAVE_FAILED: file read-only / not checked out.
- TOOL_NOT_FOUND / ARGUMENT_NOT_FOUND / REFERENCE_NOT_FOUND (ExecuteBatch): fix the step; run with bDryRun first.
- USER_CHANGES / HISTORY_CHANGED (rollback): inspect GetChangesSince; don't discard the user's work silently.
- MISSING_ARGUMENT / INVALID_VALUE: read the tool description for the accepted values.
- RESTORE_POINT_NOT_FOUND: ListRestorePoints. BATCH_STEP_FAILED / EXPECTATION_UNMET: read the batch result JSON that
  follows in the error (which steps ran, whether it rolled back) before retrying - never blindly re-run a batch.
- Shader / compile waits that time out say "Retry the same call in a moment": just retry.
- Simulation times are clamped to 0-600 s.

## Big results
GetStack(ModuleFilter), GetScriptGraph(NodeFilter, Offset, MaxNodes), GetMaterialGraph(ClassFilter, Offset,
MaxExpressions) and FindAssets page or filter; follow NextOffset instead of asking for everything.

)SKILL") TEXT(R"SKILL(## Conventions
- Asset parameters are object paths: /Game/Folder/Asset.Asset (".Name" is appended when missing).
- EmitterName None (or "System") addresses the system stacks.
- Modules: display name ("Spawn Burst Instantaneous"), function name or ModuleId; StackName narrows (ParticleSpawn,
  ParticleUpdate, EmitterSpawn, EmitterUpdate, SystemSpawn, SystemUpdate, SimulationStage, EventHandler).
- SetModuleInput values: "1.35", "0, 0, 200", "true", enum display names ("Direct Set"), "@Particles.Age" (link),
  "=expr" (HLSL expression), "Dynamic:<script path>", "Data:<DataInterfaceClass>", "Asset:<path>".
  GetStack returns values in this same syntax, so they can be copied back.
- Inputs below a dynamic input: "Parent/Child" (e.g. "Color/ColorCurve"). Static switches (e.g. Lifetime Mode) are
  normal inputs: set the switch first, then the inputs it reveals.
- Optional text arguments use words: Any (no filter), None, Auto, Minimal (template). Errors suggest close names.

)SKILL") TEXT(R"SKILL(## Recipes
- New system: CreateSystem -> AddEmitter (Minimal template: Emitter State, Initialize Particle, Particle State, sprite
  renderer, NO spawn module) -> AddModule Spawn Rate / Spawn Burst Instantaneous -> SetModuleInput -> AddRenderer ->
  ValidateSystem -> preview. Variants: DuplicateAsset (whole system) or DuplicateEmitter; SetEmitterEnabled to A/B.
- Order matters (forces before Solve Forces and Velocity): MoveModule / DuplicateModule keep input overrides.
- Renderers: SetRendererProperty for plain values, SetRendererBinding for attribute bindings (e.g. SpriteSizeBinding ->
  Particles.SpriteSize or User.Size), MoveRenderer for draw order.
- System settings: SetSystemProperty (WarmupTime, bFixedBounds + FixedBounds for GPU systems, EffectType ->
  CreateNiagaraEffectType then SetAssetProperty for culling/budgets).
- Beams / ribbons: Beam Emitter Setup (EmitterUpdate) + Spawn Beam (ParticleSpawn) + Ribbon renderer.
- Custom behaviour: CreateModuleScript or CreateLocalModule -> GetScriptGraph -> AddGraphNode (MapGet, MapSet,
  CustomHlsl, Op:..., Action:<ListNodeActions name>) -> AddParameterPin -> SetCustomHlsl -> ConnectPins ->
  SetPinDefaultValue -> SetFunctionSpecifier (data interface functions such as Particle Attribute Reader need
  Attribute=...) -> LayoutScriptGraph -> ApplyScriptChanges -> AddModule.
- Curves: SetModuleInput "Data:NiagaraDataInterfaceColorCurve" (or Curve, VectorCurve) -> SetCurveKeys (pass the
  Interpolation GetCurveKeys reports to keep the shape).
- Stages / events: AddSimulationStage, SetSimulationStageProperty (NumIterations, IterationSource...), Move/Remove,
  AddEventHandler, SetEventHandlerProperty, AddModuleToStage.
- User parameters: SetUserParameter, RenameUserParameter (relinks inputs), RemoveUserParameter,
  SetUserDataInterfaceProperty. Shared globals: Niagara / Material Parameter Collections (Asset toolset).
- Materials: CreateMaterial -> SetMaterialProperty (BlendMode, ShadingModel, TwoSided, bUsedWithNiagaraSprites /
  Ribbons / MeshParticles) -> AddExpression -> SetExpressionProperty / SetCustomExpression -> ConnectExpressions ->
  ConnectToMaterialProperty -> LayoutMaterial -> CompileMaterial. PromoteToParameter turns constants into parameters.
  Reusable logic: CreateMaterialFunction (FunctionInput / FunctionOutput nodes) + MaterialFunctionCall
  (SetExpressionProperty MaterialFunction=<path>). Variants: CreateMaterialInstance + SetMaterialInstanceParameters.
- Textures: ImportTexture (masks: bSRGB false, CompressionSettings Masks), GetTextureInfo, SetAssetProperty.
  Procedural masks/noise: CreateRenderTarget2D -> DrawMaterialToRenderTarget -> CreateTextureFromRenderTarget.
  SubUV: CreateFlipbookTexture from frames, then SubImageSize on the sprite renderer.

)SKILL") TEXT(R"SKILL(## Making it look right (lessons from real builds)
- Start from a recipe, then tune with numbers and pictures. Recipes are already tuned; their description names the
  material they assume. Tune in one ExecuteBatch with Expect after CreateRestorePoint, so a bad pass rolls back.
- Additive materials with an emissive boost (about 6x) burn bright colors to white: keep particle colors around 0.3-0.8
  (e.g. float4(0.8, 0.35, 0.06, 1)) and let the boost make the glow. A near-grey mean color in the preview caption
  (R ~ G ~ B, e.g. 173, 172, 167) on an orange or blue effect means it is burnt out.
- Previews: set CameraDistance yourself. Auto framing follows the system bounds (falling sparks, smoke columns) and can
  make the effect tiny; coverage under ~2% means too small. Shoot the moments that matter (impacts: 0.02-0.4 s).
- Smoke: translucent material with a smoke texture (opacity = texture x particle alpha) instead of a plain radial
  mask; dark colors (0.03-0.12) with alpha up to ~0.9; grow over life; preview on Grey (dark smoke vanishes on black).
- Scale Sprite Size starts in "Uniform Curve" mode, whose default curve shrinks particles: set Scale Sprite Size Mode
  to "Uniform" (then Uniform Scale Factor) or "Non-Uniform" (then Scale Factor) before giving it an expression.
- Streaks: renderer Alignment VelocityAligned + Sprite Size Mode "Random Non-Uniform" with Sprite Size Min / Max
  "(X=3,Y=24)" / "(X=6,Y=55)" (X = width, Y = length along the velocity). Vector values use Unreal text syntax.
- Forces (Gravity, Curl Noise, Drag) must sit above Solve Forces and Velocity. AddModule appends at the end, so add
  them before the solver or pass TargetIndex; ValidateSystem reports the unmet dependency.
- Random spin: Sprite Rotation Mode Random + Sprite Rotation Rate "Rotation Rate" =
  Dynamic:/Niagara/DynamicInputs/UniformRange/UniformRangedFloat.UniformRangedFloat, then "Rotation Rate/Minimum" and
  "Rotation Rate/Maximum" (e.g. -120 / 120).
- Material ParticleColor: its default output is RGB only; connect output "A" for alpha (a Custom node reading
  Color.a from the default output does not compile).
- Marketplace / Paragon textures often keep the shape in RGB with an opaque alpha (and legacy gamma); look at them
  with texture_view Channels All and use the channel that holds the shape. Their thumbnails can render black.
- When a tuning pass clearly improves a recipe-built effect, write the values back into Tools/recipes so the next
  build starts there.

)SKILL") TEXT(R"SKILL(## Pitfalls
- Close the asset editor of a script, material or function before editing its graph (tools refuse otherwise).
- GPU emitters need fixed bounds; light renderers only work on CPU emitters; GetParticleAttributes reads CPU, GPU and lightweight emitters (GPU via a sim cache).
- Materials used by Niagara need the matching usage flag (bUsedWithNiagaraSprites / Ribbons / MeshParticles).
- Custom HLSL: assign every output; texture objects are sampled with Texture2DSample(Tex, TexSampler, UV).
- After graph edits always ApplyScriptChanges, then CompileSystem / ValidateSystem.
- Asset creation is not undoable (like the editor); Undo reverts edits only. Undo/Redo/RollbackToRestorePoint save the
  touched assets afterwards (bSave) so the files match; levels are never saved for you (see UnsavedPackages).
- ReadOutputLog (Categories LogNiagara, LogShaderCompilers, LogMaterial) shows details behind compile failures.
)SKILL");
}
