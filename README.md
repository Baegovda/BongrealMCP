# BongrealMCP — Niagara MCP Editor Tools (UE 5.8)

> Transactional Niagara graph authoring, inspection, and module stack notes through the engine Toolset Registry.

언리얼 엔진 5.8의 **Toolset Registry**에 나이아가라·머티리얼 제작 도구를 등록하는 플러그인입니다.
AI 클라이언트(Codex, Claude Code, Cursor 등)는 Epic의 **Unreal MCP**(ModelContextProtocol) 플러그인을 통해 이 도구로 이펙트를 만듭니다.
플러그인이 직접 서버를 띄우지는 않습니다. Unreal MCP가 등록된 툴셋을 `http://127.0.0.1:8000/mcp` 에서
`list_toolsets` / `describe_toolset` / `call_tool` 로 노출합니다.

```
AI 클라이언트 (Codex, Claude Code, ...)  ──HTTP──▶  Unreal MCP (ModelContextProtocol, :8000/mcp)
                                                       │  list_toolsets / describe_toolset / call_tool
                                                       │  + 이미지 도구 6개: niagara_capture_preview, niagara_compare_preview, niagara_cost_view,
                                                       │    material_capture_preview, texture_view, asset_thumbnails
                                                       │  + MCP 리소스: niagara-mcp://skill, log, changes, system specs, recipes
                                                       ▼
                                               Toolset Registry
                    ┌──────────────────────────────────┼──────────────────────────────────┐
   NiagaraMcpEditorTools.NiagaraMcpEditorTools   ...MaterialMcpEditorTools   ...AssetMcpEditorTools
        (나이아가라 96개)                           (머티리얼 24개)             (에셋 19개)
```

**미리보기 이미지는 최상위 MCP 도구 `niagara_capture_preview` / `material_capture_preview` 를 쓰세요.**
Unreal MCP는 툴셋 결과를 모두 텍스트로 감쌉니다. 그래서 `call_tool` 로 부른 `CapturePreview` 는 이미지가 아니라 긴 base64 문자열로 도착합니다.
최상위 도구 두 개는 같은 인자를 받아 진짜 MCP 이미지로 돌려줍니다.

모든 편집은 **Undo 트랜잭션** 안에서 실행되므로 Ctrl+Z로 되돌릴 수 있습니다.
스택 편집은 나이아가라 에디터 UI와 같은 스택 뷰모델을 거칩니다. 그래서 시스템을 나이아가라 에디터에 열어 둔 상태에서도 동작합니다.

## 0.5.6 charge_burst 레시피

- 새 레시피 `charge_burst`: 모았다가 터뜨리는 이펙트입니다. 1초 동안 파란 줄무늬가 가속하며 중심으로 빨려 들어가고(생성량도 점점 증가), 코어가 떨리며 커지다가 직전에 움츠러듭니다. 1.0초에 흰 섬광, 밀려 나갔다 멈추는 광채, 줄무늬 160개가 튀어 나가 드래그로 급정지합니다.
- 중심을 지나친 줄무늬는 보이지 않게 처리해 모이는 동작이 깔끔합니다.

## 0.5.5 스킬 문서: 이펙트를 보기 좋게 만드는 요령

- 에이전트 스킬(`niagara-mcp://skill/niagara-vfx-authoring`)에 "Making it look right" 절을 추가했습니다. 이펙트 5종을 실제로 만들고 다듬으며 겪은 시행착오를 정리한 것입니다: 가산 머티리얼 색 범위(하얗게 날아감 방지), 미리보기 카메라 거리, 연기 텍스처 머티리얼, Scale Sprite Size 모드, 줄무늬 불꽃 설정, 힘 모듈 순서, 무작위 회전, ParticleColor 알파 출력, 마켓플레이스 텍스처 채널, 다듬은 값을 레시피에 되돌려 넣기.

## 0.5.4 fireball 레시피

- `fireball` 레시피를 다듬은 `NS_Fireball` 과 같게 만들었습니다: 코어·불꽃의 크기 변화와 무작위 회전, 위로 피어오르는 연기(Smoke), 큰 주황 광채 + 점광원(Glow, 유저 파라미터 `LightRadius`), 0.5초 워밍업.
- 새 자리표시자 `SmokeMaterial`(반투명, 기본 `DefaultSpriteMaterial`).

## 0.5.3 레시피 조정

- 실제 에디터에서 다듬은 값을 레시피에 반영했습니다(`magic_orb`, `impact_flash`, `smoke_puff`, `spark_burst`). 레시피만으로 만들어도 다듬은 결과와 같은 모습이 나옵니다.
  - `magic_orb`: 파란 코어 이미터 추가, 입자 크기·개수 증가, 하얗게 날아가지 않는 색.
  - `impact_flash`: 0.12초 주황 섬광, 속도 방향으로 늘어난 불꽃 60개 + 중력.
  - `smoke_puff`: 약 2.3 m 높이로 피어오르며 0.6→2.4배로 커지는 연기(Scale Sprite Size), 더 진한 색.
  - `spark_burst`: 불꽃 140개, 길이 24~55 줄무늬, 흰빛→주황→빨강으로 식는 색.
- 발광 계열 레시피 색은 가산 + 발광 배율(약 6배) 머티리얼 기준입니다. `DefaultSpriteMaterial` 을 쓰면 색을 올려야 합니다. 연기는 연기 텍스처 반투명 머티리얼을 권장합니다.
- `fireball` 레시피는 0.5.4에서 반영했습니다.

## 0.5.2 수정 사항

- **크래시 수정**: 레거시 감마(Pow22) 텍스처(Paragon·마켓플레이스 에셋에 흔함)를 `texture_view` / `ViewTexture` 로 보거나 `CreateFlipbookTexture` 의 첫 프레임으로 쓰면, 엔진 ImageCore assert로 에디터가 크래시 리포트 없이 종료되던 문제. 이런 텍스처는 sRGB로 변환해 표시·합성합니다.
- 회귀 테스트: `Vision.ViewTexture`, `Assets.TexturesAndRenderTargets` 에 레거시 감마 케이스 추가. 32개 모두 Success.

## 0.5.1 수정 사항

- `Undo` / `Redo` / `RollbackToRestorePoint` 후 건드린 에셋을 바로 저장합니다(`bSave`, 기본 켜짐). 레벨·한 번도 저장 안 된 에셋·PIE 중에는 저장하지 않고 `UnsavedPackages` 로 알려줍니다.
- `BakeFlipbook`(Black): 톤매퍼 디더 노이즈가 배경에 옅은 알파 얼룩으로 남던 문제 수정.
- `niagara_cost_view` / `CaptureCostView`: 셰이더 캐시가 비어 있을 때 첫 호출이 빈 화면이던 문제 수정.
- 자동화 테스트 32개 모두 Success.

## 0.5.0 새 기능 요약

GitHub의 유사 MCP(나이아가라 특화 12개, 범용 언리얼 MCP, 블렌더·후디니·유니티 MCP)를 다시 조사해 고른 기능입니다.

### 바로잡은 것 (정확성)
- **미리보기·파티클 수·속성 읽기가 한 번의 연속 시뮬레이션**을 씁니다. 전에는 시점마다 새로 시작해서 무작위 이펙트의 칸마다 다른 실행이 찍혔고, 리셋도 기존 파티클을 남겨 개수가 두 배로 나오던 문제가 있었습니다.
- **커브·데이터 인터페이스 수정이 시뮬레이션에 반영**됩니다(진행 중이던 컴파일이 옛 값을 쓰던 문제 → 강제 재컴파일).
- 셰이더 대기는 시간 제한(90초)이 있고, 넘으면 "잠시 후 다시 호출" 오류를 돌려줍니다.

### 보기·측정 (Vision)
| 도구 | 내용 |
|---|---|
| `CapturePreview` / `niagara_capture_preview` | 이미지 + **칸별 수치 캡션**(파티클 수, 화면 점유율, 밝기, 과노출, 평균색, 화면 범위, 빈 칸 원인 힌트). `Background`(Black/Grey/White), `CameraYaws`("0, 90" 여러 각도), `QualityLevel`(Low~Cinematic), `ReferenceImage`(사용자 레퍼런스를 첫 칸에) |
| `ComparePreview` / `niagara_compare_preview` | 기준(Baseline) 저장 후 다시 렌더링해 바뀐 픽셀 %와 빨간 표시 이미지 |
| `CaptureCostView` / `niagara_cost_view` | 셰이더 복잡도(오버드로) 화면 + 빨강 이상 비율 |
| `BakeFlipbook` | 시뮬레이션을 SubUV 아틀라스 텍스처로 굽기 |
| `ViewTexture` / `texture_view` | 텍스처 색 + R/G/B/A 채널 + 플립북 격자 |
| `PreviewAssets` / `asset_thumbnails` | 에셋 썸네일 시트(후보 고르기) |

### 분석·제작 (Analysis)
| 도구 | 내용 |
|---|---|
| `GetParticleAttributes` | **GPU·경량 이미터도** 값 읽기(Sim Cache) |
| `DiagnoseVisibility` | "왜 안 보이지": 컴파일 → 켜짐/품질 → 스폰 → 수명 → 렌더러 → 머티리얼 사용 플래그 → 알파·크기 → 바운드 순서로 검사, 첫 실패와 다음 호출 제안 |
| `AnalyzeCost` / `GetCompiledHlsl` | 스크립트별 VM 연산·레지스터, 단계별 ms, GPU 단계 시간, 생성 HLSL |
| `ApplySystemSpec` | `ExportSystemSpec` 의 역방향(Merge/Replace, 드라이런) |
| `SaveEmitterAsAsset` | 이미터를 템플릿 에셋으로 |
| `AddLightweightEmitter` / `GetStatelessEmitterInfo` / `SetStatelessModuleProperty` | 경량(stateless) 이미터 |
| `GetUserParameterUsage` | 유저 파라미터가 쓰이는 곳 |
| `CreateNiagaraDataChannel` / `GetNiagaraDataChannel` | Niagara Data Channel |
| `RecolorEffect` | 색조 회전·틴트로 이펙트 전체 색 바꾸기 |

### 사람과 함께 쓰기·안전 (Infra)
| 기능 | 내용 |
|---|---|
| `GetChangesSince` | 변경 기록: 에디터에서 사람이 바꾼 것(bByAgent=false)과 AI가 바꾼 것을 구분 |
| `CreateRestorePoint` / `RollbackToRestorePoint` / `ListRestorePoints` | 실험 전체를 한 번에 되돌리기(중간에 사람이 수정했으면 거부) |
| `ExecuteBatch` `Expect` | 사후 조건 `Compiles:` `Valid:` `Particles:<sys>:<emitter>@0.5>=1` `MaterialCompiles:` — 실패하면 롤백 |
| 오류 코드 | 모든 주요 오류가 `[EMITTER_NOT_FOUND]` 같은 코드로 시작(배치 결과에는 `errorCode`) |
| 페이징 | `GetStack(ModuleFilter)`, `GetScriptGraph(NodeFilter, Offset, MaxNodes)`, `GetMaterialGraph(ClassFilter, Offset, MaxExpressions)` |
| `ListNiagaraTemplates` | 엔진 템플릿 목록과 설명 |
| MCP 리소스 | `niagara-mcp://skill/...`, `log/recent`, `changes/recent`, `system/<경로>/spec`, `recipes/<이름>` |
| 레시피 | `Tools/recipes`: fireball, spark_burst, smoke_puff, magic_orb, impact_flash, charge_burst (ExecuteBatch 스크립트, 테스트로 컴파일·검증·파티클 확인) |
| 안전장치 | PIE 중 저장 거부, 배치 중 모달 대화상자 방지, `Saved/NiagaraMcp/health.json` 상태 파일 |

## 설치

1. 이 폴더를 `<프로젝트>/Plugins/NiagaraMcpEditorTools` 로 복사합니다.
   `Binaries/` 에는 UE 5.8.4(Win64, BuildId 55116800)용 DLL이 들어 있어서 바로 로드됩니다.
   다른 엔진 빌드라면 프로젝트를 열 때 에디터가 플러그인 컴파일을 제안합니다(Visual Studio 2022 C++ 필요).
2. 설치 스크립트를 실행합니다. AI에게 "플러그인 폴더에 나이아가라 MCP가 추가 되었는데 확인해주고 설치 해줘"라고 시켜도 됩니다.
   ```
   python Plugins/NiagaraMcpEditorTools/Tools/setup_project.py <프로젝트>.uproject
   ```
   스크립트가 하는 일:
   - `.uproject` 에서 `NiagaraMcpEditorTools` 와 `ModelContextProtocol`(Unreal MCP)을 활성화합니다.
   - `Config/DefaultEditorPerProjectUserSettings.ini` 에 MCP 서버 자동 시작을 설정합니다.
     ```ini
     [/Script/NiagaraMcpEditorTools.NiagaraMcpEditorToolsSettings]
     bAutoStartMcpServer=True
     ```
   - 클라이언트 연결 설정을 서버 이름 `unreal-mcp` 로 생성합니다: `.codex/config.toml`, `.mcp.json`(Claude Code), `.cursor/mcp.json`
3. 에디터를 재시작합니다. `편집 > 플러그인` 에서 "mcp"로 검색했을 때 **Niagara MCP Editor Tools** 와 **Unreal MCP** 가 체크되어 있으면 됩니다.
   MCP Client Toolset은 필요 없습니다.
4. AI 클라이언트에서 "언리얼 MCP 연결 되었는지 확인해줘"라고 요청합니다.
   `list_toolsets` 결과에 `NiagaraMcpEditorTools.NiagaraMcpEditorTools` 가 보이면 연결된 것입니다.

설정 위치는 `에디터 환경설정 > 플러그인 > Niagara MCP Editor Tools` 입니다(MCP 자동 시작, 포트, 툴셋 on/off).
서버를 수동으로 시작하려면 콘솔에서 `ModelContextProtocol.StartServer` 를 실행합니다.

### 클라이언트 설정 (setup_project.py가 생성)

```toml
# .codex/config.toml  (Codex)
[mcp_servers.unreal-mcp]
url = "http://127.0.0.1:8000/mcp"
```
```json
// .mcp.json  (Claude Code)
{ "mcpServers": { "unreal-mcp": { "type": "http", "url": "http://127.0.0.1:8000/mcp" } } }
```

## 툴셋: `NiagaraMcpEditorTools.NiagaraMcpEditorTools`

| 분류 | 도구 | 설명 |
|---|---|---|
| 조회 | `GetSystemSummary` | 이미터, 렌더러, 시뮬레이션 스테이지, 유저 파라미터 |
| | **`CapturePreview`** | 이펙트를 여러 시점(Times)에서 오프스크린 렌더링해 **이미지 한 장(컨택트 시트)** 으로 반환. AI가 결과를 직접 보고 확인 |
| | **`GetParticleCounts`** | 시점별·이미터별 파티클 수(스폰·수명·타이밍 검증) |
| | **`ValidateSystem`** | 컴파일 오류, 스택 이슈(자동 수정 목록 포함), 흔한 실수 점검을 한 목록으로 |
| | **`ApplyStackIssueFix`** | 스택이 제안하는 자동 수정 적용 |
| | `GetStack` | 각 스택의 모듈을 순서대로 반환합니다. 버전, 노트, 모든 입력의 현재 값 포함 |
| | `FindModuleScripts` | 엔진·프로젝트의 모듈 에셋을 이름, 설명, 카테고리, 스택으로 검색 |
| | `CompileSystem` | 컴파일한 뒤 스크립트별 상태와 오류·경고 메시지를 반환 |
| 시스템·이미터 | `CreateSystem` | 새 나이아가라 시스템 에셋 |
| | `AddEmitter` / `RemoveEmitter` / `RenameEmitter` | 템플릿 기반 이미터(기본값: Minimal) |
| | `SetEmitterProperty` | `SimTarget`, `bLocalSpace`, `CalculateBoundsMode` 등 |
| | `AddRenderer` / `SetRendererProperty` / `RemoveRenderer` | 리본, 스프라이트, 메시, 라이트 등과 머티리얼 |
| | `SetUserParameter` | User 파라미터 추가·변경 |
| | **`GetSystemProperties`** / **`SetSystemProperty`** | 시스템 속성(WarmupTime, bFixedBounds·FixedBounds, EffectType, 결정성 등). 열거형 값 목록 포함 |
| | **`GetEmitterProperties`** / **`GetRendererProperties`** | 이미터·렌더러 속성 이름, 값, 열거형 값, 렌더러 어트리뷰트 바인딩 |
| | **`SetRendererBinding`** / **`MoveRenderer`** | 어트리뷰트 바인딩(예: SpriteSizeBinding → `User.Size`, 타입 검사), 렌더러 순서 |
| | **`SetEmitterEnabled`** / **`DuplicateEmitter`** | 이미터 켜기·끄기, 이미터 복제 |
| | **`RenameUserParameter`** / **`RemoveUserParameter`** / **`SetUserDataInterfaceProperty`** | 이름 변경 시 링크된 입력도 갱신, User 데이터 인터페이스 속성 |
| | **`ExportSystemSpec`** / **`DiffSystems`** | 시스템 전체를 간결한 JSON으로, 두 시스템의 차이 목록 |
| 스택 모듈 | `AddModule` | 스택에 모듈 추가(항상 최신 스크립트 버전) |
| | `RemoveModule` / `SetModuleEnabled` / `UpgradeModuleVersion` | |
| | `SetModuleInput` / `ResetModuleInput` | 스택 UI와 같은 경로로 처리: Rapid Iteration, 스태틱 스위치, 중첩 입력 |
| | `SetDataInterfaceInputProperty` | 입력에 들어 있는 데이터 인터페이스의 속성 |
| | **`MoveModule`** / **`DuplicateModule`** | 모듈 순서 변경·복제(입력값 유지, 스택 드래그 앤 드롭과 같은 경로) |
| | **`FindDynamicInputs`** | 다이내믹 인풋 검색(출력 타입 필터). 결과 경로를 `Dynamic:` 값에 그대로 사용 |
| 커브 | **`GetCurveKeys`** / **`SetCurveKeys`** | Float, Vector2/3/4, Color 커브 키 편집(Cubic, Linear, Constant). 다이내믹 인풋 안의 커브는 `Color/ColorCurve` 처럼 지정 |
| 스테이지·이벤트 | `GetEmitterStages` / `AddSimulationStage` / `AddEventHandler` / `AddModuleToStage` | 시뮬레이션 스테이지, 이벤트 핸들러와 그 스택 |
| | **`SetSimulationStageProperty`** / **`MoveSimulationStage`** / **`RemoveSimulationStage`** | NumIterations, IterationSource 등. 상수 또는 `@User.X` 바인딩 |
| | **`SetEventHandlerProperty`** / **`RemoveEventHandler`** | 이벤트 핸들러 속성, 삭제 |
| 로컬 모듈 | **`CreateLocalModule`** / **`ListLocalModules`** | Scratch Pad 모듈 생성. 그래프 도구로 편집 |
| 노트 | `SetModuleNote` | 모듈에 스택 노트를 답니다(헤더, 내용, 인라인) |
| 모듈 그래프 | `CreateModuleScript` | 기본 템플릿으로 모듈 스크립트 에셋 생성 |
| | `GetScriptGraph` | 노드, 핀, 타입, 기본값, 연결 |
| | **`ListNodeActions`** | 그래프의 "노드 추가" 메뉴 검색(함수, 연산자, 데이터 인터페이스 함수 등) |
| | `AddGraphNode` | `MapGet`, `MapSet`, `CustomHlsl`, `Reroute`, `Op:Numeric::Add`, `Function:<경로>`, **`Action:<메뉴 이름>`** |
| | **`LayoutScriptGraph`** | 데이터 흐름 기준으로 노드 자동 정렬 |
| | `AddParameterPin` | Map Get / Map Set 핀(`Module.Length`, `Particles.Position` 등) |
| | `SetCustomHlsl` | Custom Hlsl 본문과 타입이 지정된 입출력 핀 |
| | `ConnectPins` / `DisconnectPins` / `RemoveGraphNode` | |
| | `SetPinDefaultValue` | 핀 기본값. Map Get의 `Module.X` 출력이면 모듈 입력의 기본값 |
| | `SetModuleScriptInfo` | 설명과 허용 스택 |
| | **`SetFunctionSpecifier`** | 데이터 인터페이스 함수 노드의 지정자(예: Particle Attribute Reader의 `Attribute=Age`) |
| | `ApplyScriptChanges` | 이 스크립트를 쓰는 모듈 갱신, 관련 시스템 재컴파일, 저장 |
| 검증 | **`GetParticleAttributes`** | 시점별·이미터별 파티클 속성(Position, Velocity, Color, Age 등)의 최소·최대·평균과 샘플 |
| 배치 | **`ExecuteBatch`** | 여러 도구 호출을 한 번에 실행. 앞 단계 결과를 `$Id.field` 로 참조, 저장은 끝에 한 번, 실패 시 전체 되돌리기, 드라이런 |
| 에디터 | **`FindAssets`** | 클래스, 이름, 폴더로 에셋 검색(페이징). 텍스처나 메시를 찾을 때 |
| | **`PlaceInLevel`** | 시스템을 레벨에 배치하고 뷰포트 카메라를 맞춤 |
| | **`Undo`** / **`Redo`** | 에디터 트랜잭션 되돌리기·다시 실행(모든 쓰기 도구는 각각 트랜잭션 1개) |
| | **`ReadOutputLog`** | 최근 출력 로그(카테고리, 심각도, 커서) — 컴파일 실패 원인 확인 |

`SetModuleInput` 값 형식:

| 형식 | 의미 |
|---|---|
| `1.35`, `0, 0, 200`, `true`, `Direct Set` | 로컬 값(숫자, 벡터, 불리언, 열거형 이름 또는 표시 이름) |
| `@Particles.NormalizedAge` | 파라미터에 링크 |
| `=Particles.Age * 2` | 커스텀 HLSL 표현식 |
| `Dynamic:/Niagara/DynamicInputs/...` | 다이내믹 인풋 스크립트 |
| `Data:NiagaraDataInterfaceCurve` | 데이터 인터페이스 클래스 |
| `Asset:/Game/...` | 오브젝트 에셋 입력 |

`GetStack` 과 `ExportSystemSpec` 은 값을 **이 형식 그대로** 반환합니다. 그래서 읽은 값을 그대로 `SetModuleInput` 에 넣을 수 있습니다.

생략 가능한 텍스트 인자는 기본값이 `Any`(필터 없음), `None`, `Auto`, `Minimal` 같은 단어입니다.
엔진 스키마가 빈 문자열 기본값을 무시하기 때문입니다. 이름을 잘못 쓰면 오류 메시지에 "Did you mean ...?" 추천이 붙습니다.

### ExecuteBatch 예

```json
[{"Id": "sys",   "Tool": "CreateSystem", "Arguments": "{\"AssetPath\": \"/Game/FX/NS_Sparks\"}"},
 {"Tool": "AddEmitter", "Arguments": "{\"System\": \"$sys.path\", \"EmitterName\": \"Sparks\"}"},
 {"Id": "burst", "Tool": "AddModule", "Arguments": "{\"System\": \"$sys.path\", \"EmitterName\": \"Sparks\", \"StackName\": \"EmitterUpdate\", \"ModuleScript\": \"/Niagara/Modules/Emitter/SpawnBurst_Instantaneous\"}"},
 {"Tool": "SetModuleInput", "Arguments": "{\"System\": \"$sys.path\", \"EmitterName\": \"Sparks\", \"Module\": \"$burst.moduleId\", \"Input\": \"Spawn Count\", \"Value\": \"30\"}"}]
```
Python에서는 `McpClient().batch([...])` 로 Arguments를 딕셔너리로 넘길 수 있습니다.

이미터는 `EmitterName` 으로 지정합니다. `None` 이나 `System` 이면 시스템 스택입니다.
모듈은 `Module` 로 지정하며, 표시 이름("Spawn Burst Instantaneous"), 함수 이름, `ModuleId` 중 아무거나 쓸 수 있습니다.
이름이 겹치면 `StackName`(ParticleSpawn, ParticleUpdate, EmitterSpawn, EmitterUpdate, SystemSpawn, SystemUpdate)으로 좁힙니다.

## 툴셋: `NiagaraMcpEditorTools.MaterialMcpEditorTools`

그래프 도구는 **머티리얼과 머티리얼 함수 모두** 편집합니다(`Material` 인자에 둘 중 하나).

| 분류 | 도구 |
|---|---|
| 생성 | `CreateMaterial`, **`CreateMaterialFunction`**(Default, MaterialLayer, MaterialLayerBlend) |
| 그래프 | `GetMaterialGraph`, `SetMaterialProperty`(BlendMode, ShadingModel, TwoSided, bUsedWithNiagaraRibbons 등), `AddExpression`, `SetExpressionProperty`, `SetCustomExpression`, `ConnectExpressions`, **`DisconnectExpressions`**, `ConnectToMaterialProperty`, **`DisconnectMaterialProperty`**, `RemoveExpression`, **`PromoteToParameter`**(상수 → 파라미터, 연결 유지), `LayoutMaterial` |
| 함수 | FunctionInput / FunctionOutput 노드, `MaterialFunctionCall`(SetExpressionProperty `MaterialFunction=<경로>` 로 핀 자동 갱신, 순환 호출 거부) |
| 컴파일·검증 | `CompileMaterial`(함수면 사용하는 머티리얼까지 재컴파일), `ValidateMaterial`(`bCompile=false` 면 부작용 없이 마지막 결과만 읽음), `GetMaterialStats` |
| 미리보기 | `CapturePreview`(메시 기본값 Plane, **`FrameCount`·`FrameInterval`** 로 시간 흐름 컨택트 시트) |
| 인스턴스 | `GetMaterialParameters`, `CreateMaterialInstance`, `SetMaterialInstanceParameter`, **`SetMaterialInstanceParameters`**(여러 개를 한 번에, 실패 시 복원), **`SetMaterialInstanceParent`**, **`ClearMaterialInstanceParameters`** |

## 툴셋: `NiagaraMcpEditorTools.AssetMcpEditorTools` (0.4.0 신규)

VFX 작업에 필요한 보조 에셋을 다룹니다.

| 분류 | 도구 |
|---|---|
| 범용 | `GetAssetProperties` / `SetAssetProperty`(어떤 에셋이든 편집 가능한 속성, `A.B[0].C` 경로), `DuplicateAsset`, `GetAssetReferences`(이 에셋을 쓰는 곳 / 이 에셋이 쓰는 것) |
| 텍스처 | `ImportTexture`(PNG·TGA·EXR 등, 대화상자 없음, sRGB·압축 설정), `GetTextureInfo`, `CreateFlipbookTexture`(프레임 → SubUV 아틀라스) |
| 렌더 타깃 | `CreateRenderTarget2D`, `ResizeRenderTarget2D`, `DrawMaterialToRenderTarget`, `CreateTextureFromRenderTarget`(머티리얼로 노이즈·마스크 텍스처를 구워 만들기) |
| 파라미터 컬렉션 | `CreateMaterialParameterCollection`, `SetMaterialParameterCollectionParameter`, `GetMaterialParameterCollection`, `CreateNiagaraParameterCollection`, `AddNiagaraParameterCollectionParameter`, `GetNiagaraParameterCollection` |
| 이펙트 타입 | `CreateNiagaraEffectType`(컬링·예산 설정은 `SetAssetProperty`) |

## Agent Skill

엔진의 AgentSkillToolset(`ListSkills` / `GetSkills`)으로 **"Niagara VFX authoring" 스킬**이 함께 제공됩니다.
AI가 이 스킬을 읽으면 다음 내용을 알게 됩니다.
- 조회 → 편집 → 검증 루프
- 레시피: 빔·리본, 커스텀 모듈, 커브, 스테이지, 렌더러 바인딩, 머티리얼·머티리얼 함수, 텍스처·렌더 타깃, 배치
- 주의점: Minimal 템플릿에는 스폰 모듈이 없음, GPU 이미터는 고정 바운드 필요, 나이아가라용 머티리얼 사용 플래그 등

## Tools/ (Python, 표준 라이브러리만 사용)

| 파일 | 용도 |
|---|---|
| `mcp_client.py` | MCP Streamable HTTP 클라이언트. 예: `McpClient().niagara("GetStack", System=..., EmitterName="Chain")`, `.material(...)`, `.assets(...)`, `.batch([...])`, `.capture_image("niagara", System=..., Times=[...])`. 인자 이름은 스키마에 맞게 자동으로 맞춰 줍니다 |
| `mcppy.cmd` | 이 폴더를 import 경로에 넣고 Python을 실행합니다. 예: `Tools\mcppy.cmd Saved\my_build.py` |
| `material_rebuild.py` | JSON 스펙으로 머티리얼을 만들거나, 이미 있으면 비우고 다시 만듭니다 |
| `niagara_build.py` | JSON 스펙으로 머티리얼, 모듈 스크립트, 시스템을 만듭니다 |
| `setup_project.py` | 프로젝트에 설치합니다(uproject, ini, 클라이언트 설정). `--with-epic-toolsets` 를 붙이면 Epic의 EditorToolset·NiagaraToolsets(뷰포트 캡처, PIE 등)도 켭니다 |
| `examples/chain_strike.json` | 영상의 사슬 효과: M_ChainRibbon, NM_ChainStrikeMotion, NS_ChainStrike(리본 이미터 3개) |
| `examples/mimic_eye.json` | 영상의 미믹 눈 머티리얼 M_MimicEye. 텍스처 경로는 프로젝트에 맞게 수정하세요 |

```
python Tools/mcp_client.py toolsets
python Tools/niagara_build.py Tools/examples/chain_strike.json
python Tools/material_rebuild.py Tools/examples/mimic_eye.json
```

## 테스트

0.5.0 테스트 그룹: `NiagaraMcp.Assets`, `.Core`, `.EndToEnd`, `.Material`, `.NiagaraExt`, `.Vision`, `.Analysis`, `.Infra` (모두 숫자·픽셀로 결과를 확인).

- C++ 자동화 테스트(MCP 클라이언트 불필요): `세션 프론트엔드 > Automation > NiagaraMcp` 에서 실행하거나 아래 명령으로 실행합니다.
  `UnrealEditor-Cmd.exe <프로젝트>.uproject -ExecCmds="Automation RunTests NiagaraMcp;Quit" -unattended -nopause`
  - `NiagaraMcp.EndToEnd.ChainStrike`: 다음을 순서대로 만들고 컴파일합니다.
    - Custom HLSL 머티리얼
    - 모듈 스크립트 그래프(Map Get, Custom Hlsl, Map Set, 모듈 입력 기본값)
    - 리본 이미터 3개짜리 시스템(모듈 추가, 입력, 노트, 렌더러)
  - `NiagaraMcp.EndToEnd.MaterialParameters`: 스칼라·벡터·텍스처 오브젝트 파라미터와 Custom 샘플링을 만들고 컴파일합니다.
  - `NiagaraMcp.EndToEnd.Verification`: 0.3.0에서 추가한 도구를 모두 검증합니다.
    - 미리보기 이미지(`Saved/NiagaraMcpTests/*.png` 로 저장), 파티클 수, 검증·자동 수정
    - 다이내믹 인풋 안의 컬러 커브, 시뮬레이션 스테이지, 로컬 모듈, 노드 메뉴, 레이아웃, 이벤트 핸들러
    - Undo/Redo, 에셋 검색, 로그
    - 머티리얼 검증·통계·미리보기·인스턴스
  - `NiagaraMcp.Core.BatchAndRoundTrip`(0.4.0): ExecuteBatch(참조, 드라이런, 실패 시 되돌리기), GetStack 값 → SetModuleInput 왕복, 잘못된 값이 시스템을 더럽히지 않음, ValidateSystem(bCompile=false) 무부작용, Undo 후 시뮬레이션 반영, RemoveEmitter, 이름 추천, 커브 보간 읽기
  - `NiagaraMcp.NiagaraExt.Tools`(0.4.0): 시스템·렌더러 속성, 바인딩, 모듈 이동·복제(입력 유지), 이미터 복제, 스테이지·이벤트, 유저 파라미터 이름 변경(링크 갱신), 스펙 내보내기·비교, 파티클 속성
  - `NiagaraMcp.Material.Functions` / `Instances` / `PreviewOverTime`(0.4.0)
  - `NiagaraMcp.Assets.CollectionsAndReferences` / `TexturesAndRenderTargets`(0.4.0)
- 라이브 MCP 테스트(에디터와 MCP 서버가 실행 중이어야 함): `python Tests/smoke_test.py`

## 검증 결과 (UE 5.8.4)

- **빌드**: 실제 5.8 Toolset Registry / Unreal MCP API로 빌드했습니다. 오류와 경고 모두 0입니다.
- **C++ 자동화 테스트**: 0.5.0 출시 때 31개 모두 Success였고(Assets, Core, EndToEnd, Material, NiagaraExt, Vision, Analysis, Infra), 이후 `Infra.UndoAndRollbackSave` 를 추가한 32개도 모두 Success입니다.
- **라이브 MCP**:
  - 플러그인 자동 시작 설정만으로 MCP 서버가 떴고, `tools/list` 결과는 `list_toolsets / describe_toolset / call_tool` 이었습니다.
  - 이어서 `Tests/smoke_test.py` 가 HTTP MCP로 M_ChainRibbon, NM_ChainStrikeMotion, NS_ChainStrike를 만들었습니다.
  - 컴파일 오류 없이 모든 체크를 통과했습니다.
- **MCP 결과 형식**: `{"returnValue": {...}}` 형태이고 키는 camelCase입니다. `mcp_client.py` 가 자동으로 풀어 줍니다.
- **에셋 인자 경로**: System, Material, Script 인자는 `/Game/X/NS_A.NS_A` 처럼 오브젝트 경로 전체로 넘겨야 합니다. `mcp_client.py` 가 자동으로 보정합니다.

## 참고한 프로젝트

0.3.0과 0.4.0의 기능은 GitHub의 유사 MCP와 엔진 기본 툴셋을 조사해 골랐습니다.
- 0.4.0에서 추가로 조사: db-lyon/ue-mcp(파티클 속성 읽기, 배치), ChiR24/Unreal_mcp(배치·참조, 오류 추천), kevinpbuckley/VibeUE, CarlxsMG/UEAgent(텍스처·렌더 타깃·컬렉션), flopperam, runreal, Chang-Jin-Lee/unreal-niagara-mcp(드라이런·롤백)
- Joey0854/UnrealMCP-ExtraToolsets (MIT): 시뮬레이션 스테이지와 로컬 모듈 구현 방식 참고
- tumourlove/monolith: 미리보기 캡처, 검증
- aadeshrao123/Unreal-MCP, voodoofox/unreal-mcp: 커브, 노드 탐색, 이벤트
- ue-mcp, StraySpark: 이미지 결과, 통일된 이슈 목록, 페이징
- VibeUE, Epic 기본 툴셋: Agent Skill, 트랜잭션·Undo

## 0.4.0에서 고친 충돌

출처가 다른 기능을 합치면서 생긴 문제와 엔진 기본 툴셋과의 충돌을 점검해 고쳤습니다.
- **MCP 서버 자동 시작**: Unreal MCP 자체의 Auto Start가 켜져 있거나 서버가 이미 떠 있으면 다시 시작하지 않습니다(전에는 재시작으로 연결된 세션이 끊길 수 있었음).
- **이미지 결과**: 툴셋 결과가 텍스트로만 전달되는 문제 → 최상위 이미지 도구 2개 추가.
- **에디터 열림/닫힘에 따라 결과가 달라지던 문제**: 에디터가 닫혀 있을 때 쓰는 뷰모델을 에디터와 같은 방식(데이터 전용 아님)으로 바꿔, 모듈 이슈 누락·상속 모듈 삭제 문제를 없앴습니다. RemoveEmitter도 항상 뷰모델 경로를 씁니다.
- **실패해도 일부만 바뀌던 문제**: SetModuleInput, SetCustomExpression, 머티리얼 인스턴스 파라미터, CreateLocalModule이 모두 검증을 먼저 하고 바꿉니다.
- **에셋 생성과 Undo**: 생성을 Undo하면 빈 에셋이 남아 크래시하던 문제 → 에디터처럼 생성은 Undo 대상에서 뺐습니다.
- **Undo 후 미리보기가 옛 상태**: Undo/Redo 후 해당 시스템을 다시 컴파일합니다.
- **Undo·되돌리기 후 파일이 옛 상태**: `Undo` / `Redo` / `RollbackToRestorePoint` 가 메모리만 되돌리고 저장하지 않아, 디스크 파일에는 되돌리기 전 값이 남아 있었습니다. 이제 건드린 에셋을 바로 저장합니다(`bSave`, 기본 켜짐). 레벨, 한 번도 저장 안 된 에셋, PIE 중에는 저장하지 않고 `UnsavedPackages` 로 알려줍니다. 회귀 테스트 `NiagaraMcp.Infra.UndoAndRollbackSave`.
- **BakeFlipbook 배경 알파 얼룩**: 톤매퍼가 검은색을 1단계(sRGB 13/255) 디더링해서, 밝기→알파로 구울 때 빈 배경에 5% 알파 얼룩이 생겼습니다. 측정한 노이즈 이하 픽셀은 완전 투명으로 처리합니다(`AlphaMode` 에 기준값 표시).
- **비용 보기(ShaderComplexity) 첫 호출이 빈 화면**: 셰이더 캐시가 비어 있으면 디버그 뷰 셰이더가 첫 프레임 뒤에야 컴파일 요청돼 파티클이 안 그려졌습니다. 비용 보기에서는 워밍업을 3번 돌리며 셰이더를 기다립니다.
- **ApplyScriptChanges**: 엔진의 Apply와 같게(트랜잭션, 열린 에디터 사본 포함, ScriptApplied 알림, 이미터·스크립트 호출자도 저장).
- **읽기 도구의 부작용**: ValidateSystem/ValidateMaterial은 `bCompile=false` 면 아무것도 바꾸지 않습니다.
- **값 왕복**: GetStack 값과 커브 보간이 Set 도구에 그대로 들어갑니다. 스택 이름 별칭(ParticleSimulationStageScript, EventHandler 등)을 통일했습니다.
- **저장**: 소스 컨트롤(Perforce 등)이 켜져 있으면 저장 전에 조용히 체크아웃합니다.

## 제한 사항

- 그래프 도구로 스크립트를 편집할 때는 해당 에셋 에디터를 닫아 두세요. 열려 있으면 도구가 편집을 거부합니다.
  - 로컬(Scratch Pad) 모듈이면 그 시스템의 나이아가라 에디터를 닫아야 합니다.
- `CapturePreview` 는 렌더링이 가능한 에디터에서만 동작합니다(`-NullRHI` 불가).
- 임의의 Python·콘솔 명령 실행은 보안상 넣지 않았습니다. 필요하면 Epic의 EditorToolset을 켜세요.
- 5.8의 Minimal 이미터 템플릿에는 Spawn Rate 모듈이 없습니다. 파티클 수는 Spawn Burst / Spawn Rate 모듈을 추가해서 정하세요.
- `GetParticleAttributes` 는 CPU 이미터만 값을 읽습니다(GPU 이미터는 개수만).
- **커브·데이터 인터페이스 값 반영**: 0.4.0 초기 빌드에서는 `SetCurveKeys` 등으로 바꾼 값이 이미 컴파일된 시스템의 시뮬레이션에 반영되지 않는 버그가 있었습니다(값은 저장되지만 실행 시 이전 커브 사용). 이후 빌드에서 고쳤고 회귀 테스트 `NiagaraMcp.Core.EditsReachSimulation` 을 추가했습니다.
  그래프 내용이 같으면 나이아가라가 컴파일 캐시(DDC)를 재사용하기 때문에, 확실하게 하려면 색·크기 변화를 `=lerp(...)` 같은 HLSL 식으로 넣는 방법도 있습니다(NS_Fireball 예시 참고).
- 머티리얼 함수는 자체 셰이더가 없어서, 컴파일 오류는 그 함수를 쓰는 머티리얼을 통해서만 보입니다.
- Chaos 등 다른 툴셋은 엔진 기본 툴셋(AllToolsets, NiagaraToolsets 등)에서 제공합니다. 이 플러그인에는 포함되어 있지 않습니다.
