#include "NiagaraMcpPreview.h"

#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/Paths.h"
#include "NiagaraPlatformSet.h"
#include "NiagaraSettings.h"
#include "ShowFlags.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraMcpEditorTools.h"
#include "NiagaraSystemInstance.h"
#include "NiagaraSystemInstanceController.h"
#include "NiagaraMcpCommon.h"
#include "NiagaraSystem.h"
#include "PreviewScene.h"
#include "RenderingThread.h"
#include "ShaderCompiler.h"
#include "TextureResource.h"
#include "UObject/StrongObjectPtr.h"

namespace NiagaraMcp::Preview
{
	namespace
	{
		/** Off-screen capture: only the subject is drawn, on black, with fixed exposure and no temporal effects. */
		struct FCaptureRig
		{
			TStrongObjectPtr<USceneCaptureComponent2D> Capture;
			TStrongObjectPtr<UTextureRenderTarget2D> Target;
			/** Size of the returned frames; the render target is larger in cost view mode (see Init). */
			int32 FrameSize = 0;
			int32 TargetSize = 0;

			bool Init(FPreviewScene& Scene, UPrimitiveComponent& Subject, int32 Size, float FieldOfView, ECostView CostView = ECostView::None)
			{
				// The ShaderComplexity visualization always draws a crosshair at the view center and a legend near the
				// bottom. In cost view mode the target is (2 * Size + 64) wide and an off-center projection puts the
				// requested Size x Size view into its top-left corner, so the overlays stay outside the cropped frame.
				FrameSize = Size;
				TargetSize = CostView != ECostView::None ? Size * 2 + 64 : Size;
				Target.Reset(NewObject<UTextureRenderTarget2D>(GetTransientPackage()));
				Target->ClearColor = FLinearColor::Black;
				Target->InitCustomFormat(TargetSize, TargetSize, PF_B8G8R8A8, false);
				Target->UpdateResourceImmediate(true);

				Capture.Reset(NewObject<USceneCaptureComponent2D>(GetTransientPackage()));
				Capture->TextureTarget = Target.Get();
				Capture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
				Capture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
				Capture->ShowOnlyComponent(&Subject);
				Capture->bCaptureEveryFrame = false;
				Capture->bCaptureOnMovement = false;
				Capture->bAlwaysPersistRenderingState = true;
				Capture->FOVAngle = FieldOfView;
				if (TargetSize != FrameSize)
				{
					const float Scale = static_cast<float>(FrameSize) / static_cast<float>(TargetSize);
					const float InvTan = 1.0f / FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(FieldOfView, 10.0f, 120.0f) * 0.5f));
					FMatrix Projection = FReversedZPerspectiveMatrix(FMath::DegreesToRadians(FieldOfView * 0.5f), 1.0f, 1.0f, GNearClippingPlane);
					Projection.M[0][0] = InvTan * Scale;
					Projection.M[1][1] = InvTan * Scale;
					Projection.M[2][0] = Scale - 1.0f; // x: normal NDC [-1, 1] -> [-1, -1 + 2 * Scale] (left part)
					Projection.M[2][1] = 1.0f - Scale; // y: normal NDC [-1, 1] -> [1 - 2 * Scale, 1] (top part)
					Capture->bUseCustomProjectionMatrix = true;
					Capture->CustomProjectionMatrix = Projection;
				}
				FPostProcessSettings& Post = Capture->PostProcessSettings;
				Post.bOverride_AutoExposureMethod = true;
				Post.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
				Post.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
				Post.AutoExposureApplyPhysicalCameraExposure = false;
				Post.bOverride_AutoExposureBias = true;
				Post.AutoExposureBias = 0.0f;
				Capture->PostProcessBlendWeight = 1.0f;
				Scene.AddComponent(Capture.Get(), FTransform::Identity);

				// Show flags are set after registration: USceneCaptureComponent::OnRegister resets ShowFlags to the
				// archetype's (UpdateShowFlags), which would silently drop anything set before AddComponent.
				if (CostView != ECostView::None)
				{
					// The debug view shader mode is chosen from these show flags (FSceneViewFamily::ChooseDebugViewShaderMode);
					// the complexity is colorized in post processing, so the LDR final color shows the engine's color ramp.
					ApplyViewMode(VMI_ShaderComplexity, true, Capture->ShowFlags);
				}
				Capture->ShowFlags.SetTemporalAA(false);
				Capture->ShowFlags.SetMotionBlur(false);
				Capture->ShowFlags.SetEyeAdaptation(false);
				Capture->ShowFlags.SetFog(false);
				Capture->ShowFlags.SetAtmosphere(false);
				return true;
			}

			bool Shoot(UWorld* World, const FVector& Location, const FRotator& Rotation, TArray<FColor>& OutPixels)
			{
				Capture->SetWorldLocationAndRotation(Location, Rotation);
				World->SendAllEndOfFrameUpdates();
				Capture->CaptureScene();
				FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
				OutPixels.Reset();
				if (!Resource || !Resource->ReadPixels(OutPixels) || OutPixels.Num() != TargetSize * TargetSize)
				{
					return false;
				}
				if (TargetSize != FrameSize)
				{
					TArray<FColor> Cropped;
					Cropped.SetNumUninitialized(FrameSize * FrameSize);
					for (int32 Y = 0; Y < FrameSize; ++Y)
					{
						FMemory::Memcpy(&Cropped[Y * FrameSize], &OutPixels[Y * TargetSize], FrameSize * sizeof(FColor));
					}
					OutPixels = MoveTemp(Cropped);
				}
				return true;
			}
		};

		void ComputeCamera(const FBoxSphereBounds& Bounds, const FCameraSettings& Camera, FVector& OutLocation, FRotator& OutRotation)
		{
			OutRotation = FRotator(Camera.Pitch, Camera.Yaw, 0.0f);
			const float HalfFov = FMath::DegreesToRadians(FMath::Clamp(Camera.FieldOfView, 10.0f, 120.0f) * 0.5f);
			const float Radius = FMath::Max(Bounds.SphereRadius, 10.0f);
			const float Distance = Camera.Distance > 0.0f ? Camera.Distance : Radius / FMath::Tan(HalfFov) * 1.15f;
			OutLocation = Bounds.Origin - OutRotation.Vector() * Distance;
		}

		void PasteFrame(const TArray<FColor>& Frame, int32 FrameSize, int32 Index, int32 Columns, TArray<FColor>& Sheet, int32 SheetWidth)
		{
			const int32 OffsetX = (Index % Columns) * FrameSize;
			const int32 OffsetY = (Index / Columns) * FrameSize;
			for (int32 Y = 0; Y < FrameSize; ++Y)
			{
				FMemory::Memcpy(&Sheet[(OffsetY + Y) * SheetWidth + OffsetX], &Frame[Y * FrameSize], FrameSize * sizeof(FColor));
			}
		}

		void ForceOpaque(TArray<FColor>& Pixels)
		{
			for (FColor& Pixel : Pixels)
			{
				Pixel.A = 255;
			}
		}

		FPreviewScene::ConstructionValues PreviewValues()
		{
			return FPreviewScene::ConstructionValues()
				.SetEditor(true)
				.SetTransactional(false)
				.SetCreatePhysicsScene(false)
				.AllowAudioPlayback(false)
				.SetCreateDefaultLighting(true);
		}
	}

	bool WaitForSystemCompile(UNiagaraSystem& System, double /*MaxSeconds*/, FString& /*OutError*/)
	{
		// Niagara script compiles only advance inside the engine's own wait (QueryCompileComplete in wait mode, which
		// also issues a deferred on-demand RequestCompile); polling from outside never finishes them. Script compiles
		// take seconds; the long waits are shader jobs, which WaitForShaderJobs bounds.
		System.WaitForCompilationComplete(false, false);
		return true;
	}

	bool WaitForShaderJobs(double MaxSeconds, FString& OutError)
	{
		if (GShaderCompilingManager == nullptr)
		{
			return true;
		}
		const double Start = FPlatformTime::Seconds();
		while (GShaderCompilingManager->GetNumRemainingJobs() > 0)
		{
			GShaderCompilingManager->ProcessAsyncResults(true, false);
			if (GShaderCompilingManager->GetNumRemainingJobs() == 0)
			{
				break;
			}
			if (FPlatformTime::Seconds() - Start > MaxSeconds)
			{
				OutError = FString::Printf(TEXT("%d shader jobs are still compiling after %.0f s (first use of a material compiles its shaders). Retry the same call in a moment."),
					GShaderCompilingManager->GetNumRemainingJobs(), MaxSeconds);
				return false;
			}
			FPlatformProcess::Sleep(0.05f);
		}
		// Hand finished jobs to their materials.
		GShaderCompilingManager->ProcessAsyncResults(false, false);
		return true;
	}

	void SimulateInOrder(UNiagaraComponent& Component, const TArray<float>& Times, float TickDelta, TFunctionRef<void(int32 Index, float Seconds)> Visit)
	{
		TArray<int32> Order;
		for (int32 Index = 0; Index < Times.Num(); ++Index)
		{
			Order.Add(Index);
		}
		Order.StableSort([&Times](int32 A, int32 B) { return Times[A] < Times[B]; });

		// ResetSystem only restarts the system's age and keeps living particles (FNiagaraSystemInstance::Reset with
		// EResetMode::ResetSystem does not reset the emitter simulations), so a second run on the same component would
		// start with the previous run's particles. Reinitializing gives every run a clean start.
		Component.ReinitializeSystem();
		float Current = 0.0f;
		for (int32 Index : Order)
		{
			// Bounded: AdvanceSimulationByTime ticks Seconds / TickDelta times on the game thread. Non-finite or
			// huge values would hang the editor, so times are clamped to 0-600 s (documented on the tools).
			const float Raw = Times[Index];
			const float Seconds = FMath::IsFinite(Raw) ? FMath::Clamp(Raw, 0.0f, MaxSimulationSeconds) : 0.0f;
			if (Seconds > Current + KINDA_SMALL_NUMBER)
			{
				Component.AdvanceSimulationByTime(Seconds - Current, TickDelta);
				Current = Seconds;
			}
			Visit(Index, Seconds);
		}
	}

	namespace
	{
		/** Backdrop material: the engine's basic shape material (Color parameter). Building a material at runtime is not safe here. */
		UMaterialInterface* GetBackdropMaterial()
		{
			return LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
		}

		/** Background colors (linear, before the fixed-exposure tonemap). False for an unknown name. */
		bool ResolveBackground(const FString& Name, bool& bOutBackdrop, FLinearColor& OutA, FLinearColor& OutB)
		{
			const FString N = Name.TrimStartAndEnd();
			bOutBackdrop = true;
			if (N.IsEmpty() || N.Equals(TEXT("Black"), ESearchCase::IgnoreCase) || N.Equals(TEXT("Default"), ESearchCase::IgnoreCase))
			{
				bOutBackdrop = false;
				return true;
			}
			if (N.Equals(TEXT("Grey"), ESearchCase::IgnoreCase) || N.Equals(TEXT("Gray"), ESearchCase::IgnoreCase))
			{
				OutA = OutB = FLinearColor(0.18f, 0.18f, 0.18f);
				return true;
			}
			if (N.Equals(TEXT("White"), ESearchCase::IgnoreCase))
			{
				OutA = OutB = FLinearColor(1.0f, 1.0f, 1.0f);
				return true;
			}
			return false;
		}
	}

	namespace
	{
		/** Particle counts of every emitter of the component's current instance. */
		void ReadCounts(UNiagaraComponent& Component, float Seconds, FNiagaraMcpParticleSample& Sample)
		{
			static const UEnum* StateEnum = StaticEnum<ENiagaraExecutionState>();
			Sample.Time = Seconds;
			Sample.TotalParticles = 0;
			Sample.Emitters.Reset();
			FNiagaraSystemInstanceControllerPtr Controller = Component.GetSystemInstanceController();
			FNiagaraSystemInstance* Instance = Controller.IsValid() ? Controller->GetSystemInstance_Unsafe() : nullptr;
			if (Instance == nullptr)
			{
				return;
			}
			for (const FNiagaraEmitterInstanceRef& Emitter : Instance->GetEmitters())
			{
				FNiagaraMcpEmitterCount& Count = Sample.Emitters.AddDefaulted_GetRef();
				Count.Emitter = Emitter->GetEmitterHandle().GetName();
				Count.Particles = Emitter->GetNumParticles();
				Count.State = StateEnum ? StateEnum->GetNameStringByValue(static_cast<int64>(Emitter->GetExecutionState())) : FString();
				Sample.TotalParticles += Count.Particles;
			}
		}

		TStrongObjectPtr<UNiagaraComponent> MakePreviewComponent(FPreviewScene& Scene, UNiagaraSystem& System)
		{
			TStrongObjectPtr<UNiagaraComponent> Component(NewObject<UNiagaraComponent>(GetTransientPackage()));
			Component->SetAutoActivate(false);
			Component->SetAsset(&System);
			Component->SetForceSolo(true);
			Scene.AddComponent(Component.Get(), FTransform::Identity);
			return Component;
		}
	}

	bool CaptureNiagaraShots(UNiagaraSystem& System, const FShotRequest& Request, FShotResult& OutResult, FString& OutError)
	{
		const TArray<float>& Times = Request.Times;
		if (Times.Num() == 0 || Request.Yaws.Num() == 0)
		{
			OutError = TEXT("Internal error: no times or camera yaws to capture.");
			return false;
		}
		const int32 FrameSize = FMath::Clamp(Request.FrameSize, 32, 1024);
		constexpr float TickDelta = 1.0f / 60.0f;
		const FCameraSettings& Camera = Request.Camera;

		bool bBackdrop = false;
		FLinearColor BackA, BackB;
		if (!ResolveBackground(Camera.Background, bBackdrop, BackA, BackB))
		{
			OutError = FString::Printf(TEXT("Unknown Background '%s'. Use Black, Grey or White."), *Camera.Background);
			return false;
		}

		// The quality level must be in place before the component initializes its emitters.
		FScopedQualityLevel Quality(Request.QualityLevel);
		OutResult = FShotResult();
		OutResult.QualityLevelName = QualityLevelName(INDEX_NONE);
		OutResult.FrameSize = FrameSize;

		if (!WaitForSystemCompile(System, DefaultMaxWaitSeconds, OutError))
		{
			return false;
		}
		if (!System.IsValid())
		{
			OutError = FString::Printf(TEXT("'%s' is not valid (compile errors?). Run CompileSystem for details."), *System.GetPathName());
			return false;
		}

		FPreviewScene Scene(PreviewValues());
		UWorld* World = Scene.GetWorld();
		TStrongObjectPtr<UNiagaraComponent> Component = MakePreviewComponent(Scene, System);

		// Frame the union of the effect's bounds over every requested time (one continuous run), unless fixed.
		FBoxSphereBounds Framing = Request.Framing;
		if (!Request.bFixedFraming)
		{
			FBox Union(ForceInit);
			SimulateInOrder(*Component, Times, TickDelta, [&](int32, float)
			{
				Component->UpdateBounds();
				const FBox Box = Component->Bounds.GetBox();
				if (Box.IsValid && Box.GetExtent().GetMax() > KINDA_SMALL_NUMBER && Box.GetExtent().GetMax() < 100000.0f)
				{
					Union += Box;
				}
			});
			Framing = Union.IsValid ? FBoxSphereBounds(Union) : FBoxSphereBounds(FVector::ZeroVector, FVector(200.0f), 200.0f);
		}
		OutResult.Framing = Framing;

		FCaptureRig Rig;
		Rig.Init(Scene, *Component, FrameSize, Camera.FieldOfView, Request.CostView);

		const int32 NumYaws = Request.Yaws.Num();
		TArray<FVector> CameraLocations;
		TArray<FRotator> CameraRotations;
		for (float Yaw : Request.Yaws)
		{
			FCameraSettings YawCamera = Camera;
			YawCamera.Yaw = Yaw;
			ComputeCamera(Framing, YawCamera, CameraLocations.AddDefaulted_GetRef(), CameraRotations.AddDefaulted_GetRef());
		}

		// Optional backdrop: an unlit plane behind the effect, facing the camera, filling the view (moved per yaw).
		TStrongObjectPtr<UStaticMeshComponent> Backdrop;
		if (bBackdrop)
		{
			UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
			UMaterialInterface* BaseBackdrop = GetBackdropMaterial();
			UMaterialInstanceDynamic* BackdropMaterial = Plane && BaseBackdrop ? UMaterialInstanceDynamic::Create(BaseBackdrop, GetTransientPackage()) : nullptr;
			if (BackdropMaterial)
			{
				BackdropMaterial->SetVectorParameterValue(TEXT("Color"), BackA);
				Backdrop.Reset(NewObject<UStaticMeshComponent>(GetTransientPackage()));
				Backdrop->SetStaticMesh(Plane);
				Backdrop->SetMaterial(0, BackdropMaterial);
				Backdrop->SetCastShadow(false);
				Scene.AddComponent(Backdrop.Get(), FTransform::Identity);
				Rig.Capture->ShowOnlyComponent(Backdrop.Get());
			}
		}
		auto PlaceBackdrop = [&](int32 YawIndex)
		{
			if (!Backdrop.IsValid())
			{
				return;
			}
			const FVector Forward = CameraRotations[YawIndex].Vector();
			const FVector BackLocation = Framing.Origin + Forward * (Framing.SphereRadius * 2.0f + 50.0f);
			const float DistanceToBack = FVector::Dist(BackLocation, CameraLocations[YawIndex]);
			const float Width = 2.0f * FMath::Tan(FMath::DegreesToRadians(Camera.FieldOfView * 0.5f)) * DistanceToBack * 1.3f;
			Backdrop->SetWorldTransform(FTransform(FRotationMatrix::MakeFromZ(-Forward).Rotator(), BackLocation, FVector(Width / 100.0f)));
			// The backdrop material is lit: aim the preview light along the view (tilted down a little) so every yaw sees
			// the same backdrop brightness instead of a black plane when the camera faces away from the light.
			Scene.SetLightDirection(CameraRotations[YawIndex] + FRotator(-20.0f, 0.0f, 0.0f));
		};
		auto Shoot = [&](int32 YawIndex, TArray<FColor>& OutPixels)
		{
			PlaceBackdrop(YawIndex);
			return Rig.Shoot(World, CameraLocations[YawIndex], CameraRotations[YawIndex], OutPixels) && OutPixels.Num() == FrameSize * FrameSize;
		};
		UE_LOG(LogNiagaraMcp, Verbose, TEXT("Preview of %s: framing origin %s radius %.1f, %d yaws"), *System.GetName(), *Framing.Origin.ToString(), Framing.SphereRadius, NumYaws);

		// Renderer materials (e.g. the Niagara sprite vertex factory variants, debug view shaders) compile on first
		// use; wait for them and render one warm-up frame so the first captured frame is not empty.
		{
			TArray<float> First = { FMath::Max(0.0f, Times[0]) };
			SimulateInOrder(*Component, First, TickDelta, [](int32, float) {});
		}
		Component->MarkRenderDynamicDataDirty();
		{
			TArray<FColor> WarmUp;
			Shoot(0, WarmUp);
		}
		if (!WaitForShaderJobs(DefaultMaxWaitSeconds, OutError))
		{
			return false;
		}
		FlushRenderingCommands();

		// The empty background per yaw: coverage is measured against it. It is shot with the component visible but
		// without particles (freshly reinitialized, not ticked), so anything the renderer adds just because a primitive
		// is present cancels out; if the system already has particles at that point (pre-warm), the component is hidden.
		Component->ReinitializeSystem();
		FNiagaraMcpParticleSample Empty;
		ReadCounts(*Component, 0.0f, Empty);
		const bool bHideForBackground = Empty.TotalParticles > 0;
		Component->SetVisibility(!bHideForBackground);
		Component->MarkRenderDynamicDataDirty();
		OutResult.Backgrounds.SetNum(NumYaws);
		OutResult.Thresholds.Init(CoverageThreshold, NumYaws);
		for (int32 YawIndex = 0; YawIndex < NumYaws; ++YawIndex)
		{
			// Two shots: their average is the background, their largest difference the noise level.
			TArray<FColor> First, Second;
			if (!Shoot(YawIndex, First) || !Shoot(YawIndex, Second))
			{
				OutError = TEXT("Reading the preview render target failed (is rendering available? -NullRHI cannot capture).");
				return false;
			}
			int32 Noise = 0;
			TArray<FColor>& Background = OutResult.Backgrounds[YawIndex];
			Background.SetNumUninitialized(First.Num());
			for (int32 Pixel = 0; Pixel < First.Num(); ++Pixel)
			{
				const FColor& A = First[Pixel];
				const FColor& B = Second[Pixel];
				Noise = FMath::Max(Noise, FMath::Max3(FMath::Abs(A.R - B.R), FMath::Abs(A.G - B.G), FMath::Abs(A.B - B.B)));
				Background[Pixel] = FColor((A.R + B.R + 1) / 2, (A.G + B.G + 1) / 2, (A.B + B.B + 1) / 2, 255);
			}
			OutResult.Thresholds[YawIndex] = FMath::Clamp(Noise + 2, CoverageThreshold, 40);
		}
		Component->SetVisibility(true);

		// One continuous run; every time is shot from every yaw.
		OutResult.Shots.SetNum(Times.Num() * NumYaws);
		bool bShotFailed = false;
		SimulateInOrder(*Component, Times, TickDelta, [&](int32 TimeIndex, float Seconds)
		{
			if (bShotFailed)
			{
				return;
			}
			FNiagaraMcpParticleSample Counts;
			ReadCounts(*Component, Seconds, Counts);
			Component->MarkRenderDynamicDataDirty();
			for (int32 YawIndex = 0; YawIndex < NumYaws && !bShotFailed; ++YawIndex)
			{
				FShot& Shot = OutResult.Shots[TimeIndex * NumYaws + YawIndex];
				Shot.TimeIndex = TimeIndex;
				Shot.YawIndex = YawIndex;
				Shot.Time = Seconds;
				Shot.Yaw = Request.Yaws[YawIndex];
				Shot.Counts = Counts;
				if (!Shoot(YawIndex, Shot.Pixels))
				{
					bShotFailed = true;
					return;
				}
				ForceOpaque(Shot.Pixels);
			}
		});
		Component->DeactivateImmediate();
		if (bShotFailed)
		{
			OutError = TEXT("Reading the preview render target failed (is rendering available? -NullRHI cannot capture).");
			return false;
		}
		return true;
	}

	bool CaptureNiagaraFrames(UNiagaraSystem& System, const TArray<float>& InTimes, int32 InFrameSize, const FCameraSettings& Camera, FToolsetImage& OutImage, FString& OutError)
	{
		FShotRequest Request;
		Request.Times = InTimes.Num() ? InTimes : TArray<float>({ 0.25f, 0.5f, 1.0f, 2.0f });
		if (Request.Times.Num() > 12)
		{
			OutError = TEXT("At most 12 frames per preview.");
			return false;
		}
		Request.Yaws = { Camera.Yaw };
		Request.FrameSize = FMath::Clamp(InFrameSize, 64, 1024);
		Request.Camera = Camera;
		FShotResult Result;
		if (!CaptureNiagaraShots(System, Request, Result, OutError))
		{
			return false;
		}
		FSheet Sheet;
		const int32 Columns = FMath::Min(Result.Shots.Num(), 4);
		Sheet.Init(Columns, FMath::DivideAndRoundUp(Result.Shots.Num(), Columns), Result.FrameSize);
		for (int32 Index = 0; Index < Result.Shots.Num(); ++Index)
		{
			Sheet.Paste(Index % Columns, Index / Columns, Result.Shots[Index].Pixels);
		}
		if (!Sheet.Encode(OutImage))
		{
			OutError = TEXT("Encoding the preview image failed.");
			return false;
		}
		return true;
	}

	bool SimulateParticleCounts(UNiagaraSystem& System, const TArray<float>& InTimes, TArray<FNiagaraMcpParticleSample>& OutSamples, FString& OutError, int32 QualityLevel)
	{
		TArray<float> Times = InTimes;
		if (Times.Num() == 0)
		{
			Times = { 0.25f, 0.5f, 1.0f, 2.0f };
		}
		FScopedQualityLevel Quality(QualityLevel);
		if (!WaitForSystemCompile(System, DefaultMaxWaitSeconds, OutError))
		{
			return false;
		}
		if (!System.IsValid())
		{
			OutError = FString::Printf(TEXT("'%s' is not valid (compile errors?). Run CompileSystem for details."), *System.GetPathName());
			return false;
		}

		FPreviewScene Scene(PreviewValues());
		TStrongObjectPtr<UNiagaraComponent> Component = MakePreviewComponent(Scene, System);

		const int32 FirstSample = OutSamples.Num();
		OutSamples.AddDefaulted(Times.Num());
		SimulateInOrder(*Component, Times, 1.0f / 60.0f, [&](int32 Index, float Seconds)
		{
			ReadCounts(*Component, Seconds, OutSamples[FirstSample + Index]);
		});
		Component->DeactivateImmediate();
		return true;
	}

	bool CaptureMaterial(UMaterialInterface& Material, const FString& Mesh, int32 InSize, const FCameraSettings& InCamera, int32 InFrameCount, float InFrameInterval, FToolsetImage& OutImage, FString& OutError)
	{
		if (InFrameCount < 1 || InFrameCount > 16)
		{
			OutError = FString::Printf(TEXT("FrameCount must be 1-16 (got %d)."), InFrameCount);
			return false;
		}
		if (InFrameCount > 1 && !(InFrameInterval > 0.0f && InFrameInterval <= 3600.0f))
		{
			OutError = FString::Printf(TEXT("FrameInterval must be > 0 seconds when FrameCount > 1 (got %s)."), *FString::SanitizeFloat(InFrameInterval));
			return false;
		}
		const int32 Size = FMath::Clamp(InSize, 64, 1024);
		const FString MeshName = Mesh.TrimStartAndEnd().IsEmpty() ? FString(TEXT("Plane")) : Mesh.TrimStartAndEnd();
		FString MeshPath = MeshName;
		FCameraSettings Camera = InCamera;
		if (MeshName.Equals(TEXT("Plane"), ESearchCase::IgnoreCase))
		{
			MeshPath = TEXT("/Engine/BasicShapes/Plane.Plane");
			Camera.Pitch = -90.0f; // the plane faces +Z: look straight down so UV0 fills the frame
			Camera.Yaw = 0.0f;
		}
		else if (MeshName.Equals(TEXT("Sphere"), ESearchCase::IgnoreCase))
		{
			MeshPath = TEXT("/Engine/BasicShapes/Sphere.Sphere");
		}
		else if (MeshName.Equals(TEXT("Cube"), ESearchCase::IgnoreCase))
		{
			MeshPath = TEXT("/Engine/BasicShapes/Cube.Cube");
		}
		UStaticMesh* StaticMesh = Cast<UStaticMesh>(LoadObjectByPath(MeshPath, UStaticMesh::StaticClass(), TEXT("Mesh")));
		if (StaticMesh == nullptr)
		{
			OutError = FString::Printf(TEXT("Mesh '%s' could not be loaded. Use Plane, Sphere, Cube or a static mesh path."), *Mesh);
			return false;
		}

		FPreviewScene Scene(PreviewValues());
		UWorld* World = Scene.GetWorld();
		TStrongObjectPtr<UStaticMeshComponent> Component(NewObject<UStaticMeshComponent>(GetTransientPackage()));
		Component->SetStaticMesh(StaticMesh);
		Component->SetMaterial(0, &Material);
		Scene.AddComponent(Component.Get(), FTransform::Identity);
		Component->UpdateBounds();

		// The scene capture's view family takes its time from the world (UWorld::GetTime), which is what the Time,
		// Panner and flipbook expressions read (View.GameTime / View.RealTime). The preview world is never ticked,
		// so its clock is set explicitly before every frame.
		auto SetWorldTime = [World](double Seconds, float Delta)
		{
			World->TimeSeconds = Seconds;
			World->UnpausedTimeSeconds = Seconds;
			World->RealTimeSeconds = Seconds;
			World->AudioTimeSeconds = Seconds;
			World->DeltaTimeSeconds = Delta;
			World->DeltaRealTimeSeconds = Delta;
		};
		const float FrameInterval = InFrameCount > 1 ? InFrameInterval : 0.0f;
		const float Delta = FrameInterval > 0.0f ? FrameInterval : 1.0f / 60.0f;
		SetWorldTime(0.0, Delta);

		// Wait (bounded) for shaders so the first capture is not the default material; on timeout ask for a retry
		// instead of blocking the editor for minutes on a first compile.
		if (!WaitForShaderJobs(DefaultMaxWaitSeconds, OutError))
		{
			return false;
		}
		FlushRenderingCommands();

		FCaptureRig Rig;
		Rig.Init(Scene, *Component, Size, Camera.FieldOfView);
		FVector CameraLocation;
		FRotator CameraRotation;
		const FBoxSphereBounds Bounds = Component->Bounds;
		ComputeCamera(FBoxSphereBounds(Bounds.Origin, Bounds.BoxExtent, Bounds.BoxExtent.Size2D() * 0.75f), Camera, CameraLocation, CameraRotation);

		// Warm-up frame: shaders for this vertex factory / static permutation can still be requested on first draw.
		{
			TArray<FColor> WarmUp;
			Rig.Shoot(World, CameraLocation, CameraRotation, WarmUp);
		}
		if (!WaitForShaderJobs(DefaultMaxWaitSeconds, OutError))
		{
			return false;
		}
		FlushRenderingCommands();

		const int32 Columns = FMath::Min(InFrameCount, 4);
		const int32 Rows = FMath::DivideAndRoundUp(InFrameCount, Columns);
		const int32 SheetWidth = Columns * Size;
		TArray<FColor> Sheet;
		Sheet.Init(FColor::Black, SheetWidth * Rows * Size);

		for (int32 Index = 0; Index < InFrameCount; ++Index)
		{
			SetWorldTime(static_cast<double>(Index) * FrameInterval, Delta);
			TArray<FColor> Frame;
			if (!Rig.Shoot(World, CameraLocation, CameraRotation, Frame) || Frame.Num() != Size * Size)
			{
				OutError = TEXT("Reading the preview render target failed (is rendering available? -NullRHI cannot capture).");
				return false;
			}
			PasteFrame(Frame, Size, Index, Columns, Sheet, SheetWidth);
		}

		ForceOpaque(Sheet);
		if (!OutImage.SetFromBitmap(Sheet, FIntPoint(SheetWidth, Rows * Size)))
		{
			OutError = TEXT("Encoding the preview image failed.");
			return false;
		}
		return true;
	}

	// ------------------------------------------------------------------------------------------ Quality level

	namespace
	{
		TArray<FString> QualityLevelNames()
		{
			TArray<FString> Names;
			if (const UNiagaraSettings* Settings = GetDefault<UNiagaraSettings>())
			{
				for (const FText& Name : Settings->QualityLevels)
				{
					Names.Add(Name.ToString());
				}
			}
			if (Names.Num() == 0)
			{
				Names = { TEXT("Low"), TEXT("Medium"), TEXT("High"), TEXT("Epic"), TEXT("Cinematic") };
			}
			return Names;
		}

		/** The quality level without any override (the fx.Niagara.QualityLevel console variable). */
		int32 BaseQualityLevel()
		{
			const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("fx.Niagara.QualityLevel"));
			return Variable ? Variable->GetInt() : FNiagaraPlatformSet::GetQualityLevel();
		}
	}

	bool ParseQualityLevel(const FString& Text, int32& OutLevel, FString& OutError)
	{
		const FString Value = Text.TrimStartAndEnd();
		OutLevel = INDEX_NONE;
		if (Value.IsEmpty() || IsUnset(Value) || Value.Equals(TEXT("Current"), ESearchCase::IgnoreCase))
		{
			return true;
		}
		const TArray<FString> Names = QualityLevelNames();
		if (Value.IsNumeric())
		{
			const int32 Number = FCString::Atoi(*Value);
			if (Number >= 0 && Number < Names.Num())
			{
				OutLevel = Number;
				return true;
			}
		}
		for (int32 Index = 0; Index < Names.Num(); ++Index)
		{
			if (LooseNameEquals(Names[Index], Value))
			{
				OutLevel = Index;
				return true;
			}
		}
		OutError = FString::Printf(TEXT("Unknown QualityLevel '%s'. Use Current, %s or 0-%d.%s"), *Text, *FString::Join(Names, TEXT(", ")), Names.Num() - 1, *DidYouMean(Value, Names));
		return false;
	}

	FString QualityLevelName(int32 Level)
	{
		const int32 Resolved = Level == INDEX_NONE ? FNiagaraPlatformSet::GetQualityLevel() : Level;
		const TArray<FString> Names = QualityLevelNames();
		return Names.IsValidIndex(Resolved) ? Names[Resolved] : FString::FromInt(Resolved);
	}

	FScopedQualityLevel::FScopedQualityLevel(int32 Level)
	{
		const int32 Current = FNiagaraPlatformSet::GetQualityLevel();
		if (Level == INDEX_NONE || Level == Current)
		{
			return;
		}
		// The override itself is private; an active one shows as a current level that differs from the CVar.
		PreviousOverride = Current != BaseQualityLevel() ? Current : INDEX_NONE;
		FNiagaraPlatformSet::SetNiagaraQualityLevelOverride(Level);
		bApplied = true;
	}

	FScopedQualityLevel::~FScopedQualityLevel()
	{
		if (!bApplied)
		{
			return;
		}
		if (PreviousOverride != INDEX_NONE)
		{
			FNiagaraPlatformSet::SetNiagaraQualityLevelOverride(PreviousOverride);
		}
		else
		{
			FNiagaraPlatformSet::ClearNiagaraQualityLevelOverride();
		}
	}

	// ------------------------------------------------------------------------------------------ CPU image helpers

	void ComputeFrameStats(const TArray<FColor>& Pixels, const TArray<FColor>& Background, int32 Size, FNiagaraMcpFrameStats& Out, int32 Threshold)
	{
		int64 Covered = 0;
		int64 Clipped = 0;
		double SumLuminance = 0.0;
		float PeakLuminance = 0.0f;
		double SumR = 0.0, SumG = 0.0, SumB = 0.0;
		int32 MinX = MAX_int32, MinY = MAX_int32, MaxX = -1, MaxY = -1;
		const bool bHasBackground = Background.Num() == Pixels.Num();
		for (int32 Y = 0; Y < Size; ++Y)
		{
			for (int32 X = 0; X < Size; ++X)
			{
				const int32 Index = Y * Size + X;
				if (!Pixels.IsValidIndex(Index))
				{
					continue;
				}
				const FColor& Pixel = Pixels[Index];
				if (!IsCovered(Pixel, bHasBackground ? Background[Index] : FColor::Black, Threshold))
				{
					continue;
				}
				++Covered;
				const float Lum = Luminance(Pixel);
				SumLuminance += Lum;
				PeakLuminance = FMath::Max(PeakLuminance, Lum);
				SumR += Pixel.R;
				SumG += Pixel.G;
				SumB += Pixel.B;
				if (Pixel.R == 255 || Pixel.G == 255 || Pixel.B == 255)
				{
					++Clipped;
				}
				MinX = FMath::Min(MinX, X);
				MinY = FMath::Min(MinY, Y);
				MaxX = FMath::Max(MaxX, X);
				MaxY = FMath::Max(MaxY, Y);
			}
		}
		const double Total = FMath::Max<double>(1.0, static_cast<double>(Size) * Size);
		Out.Coverage = static_cast<float>(100.0 * Covered / Total);
		Out.bEmpty = Covered == 0;
		if (Covered == 0)
		{
			Out.MeanLuminance = Out.PeakLuminance = Out.ClippedShare = 0.0f;
			Out.MeanColor.Reset();
			Out.ScreenBounds.Reset();
			return;
		}
		const double N = static_cast<double>(Covered);
		Out.MeanLuminance = static_cast<float>(SumLuminance / N);
		Out.PeakLuminance = PeakLuminance;
		Out.ClippedShare = static_cast<float>(100.0 * Clipped / N);
		Out.MeanColor = FString::Printf(TEXT("%d, %d, %d"), FMath::RoundToInt(SumR / N), FMath::RoundToInt(SumG / N), FMath::RoundToInt(SumB / N));
		const float InvSize = 1.0f / static_cast<float>(Size);
		Out.ScreenBounds = FString::Printf(TEXT("%.2f, %.2f, %.2f, %.2f"), MinX * InvSize, MinY * InvSize, (MaxX + 1) * InvSize, (MaxY + 1) * InvSize);
	}

	bool LoadImageTile(const FString& InPath, int32 Size, TArray<FColor>& OutPixels, FIntPoint& OutSourceSize, FString& OutError)
	{
		FString Path = InPath.TrimStartAndEnd().TrimQuotes();
		FPaths::NormalizeFilename(Path);
		if (FPaths::IsRelative(Path))
		{
			OutError = FString::Printf(TEXT("ReferenceImage '%s' must be an absolute file path, e.g. D:/Refs/fire.png."), *InPath);
			return false;
		}
		if (!IFileManager::Get().FileExists(*Path))
		{
			OutError = FString::Printf(TEXT("ReferenceImage '%s' does not exist."), *Path);
			return false;
		}
		FImage Source;
		if (!FImageUtils::LoadImage(*Path, Source) || Source.GetWidth() <= 0 || Source.GetHeight() <= 0)
		{
			OutError = FString::Printf(TEXT("ReferenceImage '%s' is not a readable image (use PNG, JPG, BMP, TGA, EXR or HDR)."), *Path);
			return false;
		}
		OutSourceSize = FIntPoint(Source.GetWidth(), Source.GetHeight());
		const float Scale = static_cast<float>(Size) / static_cast<float>(FMath::Max(Source.GetWidth(), Source.GetHeight()));
		const int32 Width = FMath::Clamp(FMath::RoundToInt(Source.GetWidth() * Scale), 1, Size);
		const int32 Height = FMath::Clamp(FMath::RoundToInt(Source.GetHeight() * Scale), 1, Size);
		FImage Resized;
		FImageCore::ResizeImageAllocDest(Source, Resized, Width, Height, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		const TArrayView64<FColor> Colors = Resized.AsBGRA8();
		OutPixels.Init(FColor::Black, Size * Size);
		const int32 OffsetX = (Size - Width) / 2;
		const int32 OffsetY = (Size - Height) / 2;
		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				FColor Pixel = Colors[static_cast<int64>(Y) * Width + X];
				Pixel.A = 255;
				OutPixels[(OffsetY + Y) * Size + OffsetX + X] = Pixel;
			}
		}
		return true;
	}

	bool ParseYawList(const FString& Text, float Fallback, TArray<float>& OutYaws, FString& OutError)
	{
		OutYaws.Reset();
		FString Value = Text.TrimStartAndEnd();
		Value.RemoveFromStart(TEXT("["));
		Value.RemoveFromEnd(TEXT("]"));
		if (Value.IsEmpty() || IsUnset(Value))
		{
			OutYaws.Add(Fallback);
			return true;
		}
		TArray<FString> Parts;
		Value.ParseIntoArray(Parts, TEXT(","), true);
		if (Parts.Num() == 1 && Value.Contains(TEXT(" ")))
		{
			Value.ParseIntoArrayWS(Parts);
		}
		for (FString Part : Parts)
		{
			Part.TrimStartAndEndInline();
			if (Part.IsEmpty())
			{
				continue;
			}
			if (!Part.IsNumeric())
			{
				OutError = FString::Printf(TEXT("CameraYaws '%s': '%s' is not a number. Use degrees separated by commas, e.g. \"0, 90\"."), *Text, *Part);
				return false;
			}
			OutYaws.Add(FCString::Atof(*Part));
		}
		if (OutYaws.Num() == 0 || OutYaws.Num() > 8)
		{
			OutError = FString::Printf(TEXT("CameraYaws '%s' must list 1-8 yaws, e.g. \"0, 90\"."), *Text);
			return false;
		}
		return true;
	}

	void FSheet::Init(int32 InColumns, int32 InRows, int32 InTileSize, FColor Fill)
	{
		Columns = FMath::Max(1, InColumns);
		Rows = FMath::Max(1, InRows);
		TileSize = FMath::Max(1, InTileSize);
		Pixels.Init(Fill, Width() * Height());
	}

	void FSheet::Paste(int32 Column, int32 Row, const TArray<FColor>& Tile)
	{
		if (Column < 0 || Column >= Columns || Row < 0 || Row >= Rows || Tile.Num() != TileSize * TileSize)
		{
			return;
		}
		const int32 SheetWidth = Width();
		for (int32 Y = 0; Y < TileSize; ++Y)
		{
			FMemory::Memcpy(&Pixels[(Row * TileSize + Y) * SheetWidth + Column * TileSize], &Tile[Y * TileSize], TileSize * sizeof(FColor));
		}
	}

	bool FSheet::Encode(FToolsetImage& OutImage) const
	{
		TArray<FColor> Opaque = Pixels;
		ForceOpaque(Opaque);
		return OutImage.SetFromBitmap(Opaque, FIntPoint(Width(), Height()));
	}
}
