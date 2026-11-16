#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"
#include "NiagaraCommon.h"

class UNiagaraEmitterBase;
class UNiagaraRendererProperties;
class UNiagaraStackEntry;
class UNiagaraStackFunctionInput;
class UNiagaraStackModuleItem;
struct FNiagaraEmitterHandle;

/** Helpers shared by the v0.5 analysis tools (NiagaraMcpEditorTools_Analysis*.cpp). */
namespace NiagaraMcpAnalysis
{
	/**
	 * Captures the plugin's own error reports (LogNiagaraMcp warnings, see NiagaraMcp::ReportError) while in scope, so a tool
	 * that calls other tools can record why a nested call failed.
	 */
	class FErrorCapture : public FOutputDevice
	{
	public:
		FErrorCapture();
		virtual ~FErrorCapture() override;
		virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override;
		/** Called synchronously on the logging thread, so a nested report is visible right after the call returns. */
		virtual bool CanBeUsedOnAnyThread() const override { return true; }

		int32 Num() const;
		/** Messages captured since index Start, joined. */
		FString TakeSince(int32 Start) const;

	private:
		mutable FCriticalSection Lock;
		TArray<FString> Messages;
	};

	/** "1, 2, 3" -> {1, 2, 3}. */
	TArray<double> ParseNumbers(const FString& Text);

	/** ParticleSpawnScript -> ParticleSpawn. */
	FString ShortUsage(ENiagaraScriptUsage Usage);

	FString CompileStatusText(ENiagaraScriptCompileStatus Status);

	/** Lightweight (stateless) emitter handle. */
	bool IsLightweight(const FNiagaraEmitterHandle& Handle);

	/** Renderers of a standard or lightweight emitter. */
	TArray<UNiagaraRendererProperties*> GetRenderers(const FNiagaraEmitterHandle& Handle);

	/** Module objects of a lightweight emitter (empty for standard emitters). */
	TArray<UObject*> GetStatelessModules(const FNiagaraEmitterHandle& Handle);

	/** Every input of a module item including the inputs of its dynamic inputs. */
	void CollectInputsRecursive(UNiagaraStackEntry& Entry, TArray<UNiagaraStackFunctionInput*>& Out, int32 Depth);

	/** Input path in SetModuleInput syntax ("Color" or "Color/Curve" below a dynamic input). */
	FString InputPath(UNiagaraStackFunctionInput& Input, UNiagaraStackModuleItem& Module);
}
