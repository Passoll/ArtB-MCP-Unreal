#include "Niagara/ToolPlayMCPNiagaraSystemService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraEditorSettings.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

namespace
{
	FString ToJsonString(const TSharedRef<FJsonObject>& Object)
	{
		FString Output;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
		FJsonSerializer::Serialize(Object, Writer);
		return Output;
	}

	bool ValidateAssetDestination(const FString& PackagePath, const FString& AssetName, FString& OutLongPackageName, FString& OutError)
	{
		if (PackagePath.IsEmpty() || AssetName.IsEmpty())
		{
			OutError = TEXT("package_path and asset_name are required.");
			return false;
		}

		OutLongPackageName = PackagePath / AssetName;
		if (!FPackageName::IsValidLongPackageName(OutLongPackageName))
		{
			OutError = FString::Printf(TEXT("Invalid long package name: %s"), *OutLongPackageName);
			return false;
		}

		if (FindPackage(nullptr, *OutLongPackageName))
		{
			OutError = FString::Printf(TEXT("Package already loaded: %s"), *OutLongPackageName);
			return false;
		}

		if (FSoftObjectPath(OutLongPackageName + TEXT(".") + AssetName).ResolveObject())
		{
			OutError = FString::Printf(TEXT("Asset already exists: %s.%s"), *OutLongPackageName, *AssetName);
			return false;
		}
		return true;
	}

	TSharedRef<FJsonObject> BuildSystemResult(UNiagaraSystem* System)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetBoolField(TEXT("ok"), System != nullptr);
		Root->SetStringField(TEXT("asset_path"), System ? System->GetPathName() : FString());
		Root->SetStringField(TEXT("name"), System ? System->GetName() : FString());
		Root->SetBoolField(TEXT("reexport_recommended"), true);
		return Root;
	}

	bool ResolveEmitterHandle(UNiagaraSystem* System, const FString& EmitterAlias, FNiagaraEmitterHandle*& OutHandle, FString& OutError)
	{
		if (!System)
		{
			OutError = TEXT("Invalid Niagara System.");
			return false;
		}

		if (!EmitterAlias.StartsWith(TEXT("e")))
		{
			OutError = FString::Printf(TEXT("Emitter alias must look like e0, e1, ... Got: %s"), *EmitterAlias);
			return false;
		}

		const FString IndexText = EmitterAlias.RightChop(1);
		bool bAllDigits = !IndexText.IsEmpty();
		for (const TCHAR Char : IndexText)
		{
			if (!FChar::IsDigit(Char))
			{
				bAllDigits = false;
				break;
			}
		}

		if (!bAllDigits)
		{
			OutError = FString::Printf(TEXT("Emitter alias must look like e0, e1, ... Got: %s"), *EmitterAlias);
			return false;
		}

		const int32 EmitterIndex = FCString::Atoi(*IndexText);
		if (!System->GetEmitterHandles().IsValidIndex(EmitterIndex))
		{
			OutError = FString::Printf(TEXT("Emitter alias is out of range for system %s: %s"), *System->GetPathName(), *EmitterAlias);
			return false;
		}

		OutHandle = &System->GetEmitterHandles()[EmitterIndex];
		return true;
	}

	bool ParseSimTarget(const FString& SimTarget, ENiagaraSimTarget& OutSimTarget)
	{
		if (SimTarget.Equals(TEXT("GPU"), ESearchCase::IgnoreCase) ||
			SimTarget.Equals(TEXT("GPUComputeSim"), ESearchCase::IgnoreCase) ||
			SimTarget.Equals(TEXT("ENiagaraSimTarget::GPUComputeSim"), ESearchCase::IgnoreCase))
		{
			OutSimTarget = ENiagaraSimTarget::GPUComputeSim;
			return true;
		}

		if (SimTarget.Equals(TEXT("CPU"), ESearchCase::IgnoreCase) ||
			SimTarget.Equals(TEXT("CPUSim"), ESearchCase::IgnoreCase) ||
			SimTarget.Equals(TEXT("ENiagaraSimTarget::CPUSim"), ESearchCase::IgnoreCase))
		{
			OutSimTarget = ENiagaraSimTarget::CPUSim;
			return true;
		}

		return false;
	}

	bool ParseSpriteFacingMode(const FString& FacingMode, ENiagaraSpriteFacingMode& OutFacingMode)
	{
		if (FacingMode.Equals(TEXT("FaceCamera"), ESearchCase::IgnoreCase) ||
			FacingMode.Equals(TEXT("FaceCameraPosition"), ESearchCase::IgnoreCase))
		{
			OutFacingMode = ENiagaraSpriteFacingMode::FaceCamera;
			return true;
		}

		if (FacingMode.Equals(TEXT("FaceCameraPlane"), ESearchCase::IgnoreCase))
		{
			OutFacingMode = ENiagaraSpriteFacingMode::FaceCameraPlane;
			return true;
		}

		if (FacingMode.Equals(TEXT("CustomFacingVector"), ESearchCase::IgnoreCase))
		{
			OutFacingMode = ENiagaraSpriteFacingMode::CustomFacingVector;
			return true;
		}

		if (FacingMode.Equals(TEXT("FaceCameraDistanceBlend"), ESearchCase::IgnoreCase))
		{
			OutFacingMode = ENiagaraSpriteFacingMode::FaceCameraDistanceBlend;
			return true;
		}

		if (FacingMode.Equals(TEXT("Automatic"), ESearchCase::IgnoreCase))
		{
			OutFacingMode = ENiagaraSpriteFacingMode::Automatic;
			return true;
		}

		return false;
	}

	bool ParseSpriteAlignment(const FString& Alignment, ENiagaraSpriteAlignment& OutAlignment)
	{
		if (Alignment.Equals(TEXT("Unaligned"), ESearchCase::IgnoreCase))
		{
			OutAlignment = ENiagaraSpriteAlignment::Unaligned;
			return true;
		}

		if (Alignment.Equals(TEXT("VelocityAligned"), ESearchCase::IgnoreCase))
		{
			OutAlignment = ENiagaraSpriteAlignment::VelocityAligned;
			return true;
		}

		if (Alignment.Equals(TEXT("CustomAlignment"), ESearchCase::IgnoreCase))
		{
			OutAlignment = ENiagaraSpriteAlignment::CustomAlignment;
			return true;
		}

		return false;
	}
}

bool FToolPlayMCPNiagaraSystemService::CreateSystem(const FString& PackagePath, const FString& AssetName, const FString& TemplateAssetPath, FString& OutJson, FString& OutError)
{
	FString LongPackageName;
	if (!ValidateAssetDestination(PackagePath, AssetName, LongPackageName, OutError))
	{
		return false;
	}

	const FScopedTransaction Transaction(FText::FromString(TEXT("ToolPlayMCP: Create Niagara System")));

	UPackage* Package = CreatePackage(*LongPackageName);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("Failed to create package: %s"), *LongPackageName);
		return false;
	}

	Package->Modify();

	UNiagaraSystem* NewSystem = nullptr;
	if (!TemplateAssetPath.IsEmpty())
	{
		UNiagaraSystem* TemplateSystem = Cast<UNiagaraSystem>(FSoftObjectPath(TemplateAssetPath).TryLoad());
		if (!TemplateSystem)
		{
			OutError = FString::Printf(TEXT("Template asset is not a Niagara System: %s"), *TemplateAssetPath);
			return false;
		}
		if (!TemplateSystem->IsReadyToRun())
		{
			TemplateSystem->WaitForCompilationComplete();
		}
		NewSystem = Cast<UNiagaraSystem>(StaticDuplicateObject(TemplateSystem, Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional, UNiagaraSystem::StaticClass()));
	}
	else
	{
		NewSystem = NewObject<UNiagaraSystem>(Package, UNiagaraSystem::StaticClass(), *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		UNiagaraSystemFactoryNew::InitializeSystem(NewSystem, true);
	}

	if (!NewSystem)
	{
		OutError = TEXT("Failed to create Niagara System.");
		return false;
	}

	NewSystem->SetFlags(RF_Transactional);
	NewSystem->Modify();
	NewSystem->MarkPackageDirty();
	NewSystem->RequestCompile(false);
	FAssetRegistryModule::AssetCreated(NewSystem);
	OutJson = ToJsonString(BuildSystemResult(NewSystem));
	return true;
}

bool FToolPlayMCPNiagaraSystemService::AddEmitter(const FString& SystemAssetPath, const FString& EmitterAssetPath, const FString& EmitterName, FString& OutJson, FString& OutError)
{
	UNiagaraSystem* System = Cast<UNiagaraSystem>(FSoftObjectPath(SystemAssetPath).TryLoad());
	if (!System)
	{
		OutError = FString::Printf(TEXT("Asset is not a Niagara System: %s"), *SystemAssetPath);
		return false;
	}

	UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(FSoftObjectPath(EmitterAssetPath).TryLoad());
	if (!Emitter)
	{
		OutError = FString::Printf(TEXT("Asset is not a Niagara Emitter: %s"), *EmitterAssetPath);
		return false;
	}

	const FScopedTransaction Transaction(FText::FromString(TEXT("ToolPlayMCP: Add Niagara Emitter")));
	System->Modify();

	const FGuid HandleId = FNiagaraEditorUtilities::AddEmitterToSystem(
		*System,
		*Emitter,
		Emitter->GetExposedVersion().VersionGuid,
		true);

	if (HandleId.IsValid() && !EmitterName.IsEmpty())
	{
		for (FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			if (Handle.GetId() == HandleId)
			{
				Handle.SetName(FName(*EmitterName), *System);
				break;
			}
		}
	}

	System->MarkPackageDirty();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> Root = BuildSystemResult(System);
	Root->SetStringField(TEXT("emitter_handle_id"), HandleId.ToString(EGuidFormats::DigitsWithHyphens));
	OutJson = ToJsonString(Root);
	return true;
}

bool FToolPlayMCPNiagaraSystemService::AddDefaultEmitter(const FString& SystemAssetPath, const FString& EmitterName, FString& OutJson, FString& OutError)
{
	const UNiagaraEditorSettings* Settings = GetDefault<UNiagaraEditorSettings>();
	if (!Settings)
	{
		OutError = TEXT("Niagara editor settings are unavailable.");
		return false;
	}

	const FSoftObjectPath DefaultEmitterPath = Settings->DefaultEmptyEmitter;
	if (!DefaultEmitterPath.IsValid())
	{
		OutError = TEXT("Niagara editor DefaultEmptyEmitter is not configured. Set Project Settings > Plugins > Niagara > Minimal Emitter, or use add_niagara_emitter with an explicit emitter asset.");
		return false;
	}

	return AddEmitter(SystemAssetPath, DefaultEmitterPath.ToString(), EmitterName, OutJson, OutError);
}

bool FToolPlayMCPNiagaraSystemService::SetEmitterSimTarget(const FString& SystemAssetPath, const FString& EmitterAlias, const FString& SimTarget, FString& OutJson, FString& OutError)
{
	UNiagaraSystem* System = Cast<UNiagaraSystem>(FSoftObjectPath(SystemAssetPath).TryLoad());
	if (!System)
	{
		OutError = FString::Printf(TEXT("Asset is not a Niagara System: %s"), *SystemAssetPath);
		return false;
	}

	FNiagaraEmitterHandle* Handle = nullptr;
	if (!ResolveEmitterHandle(System, EmitterAlias, Handle, OutError))
	{
		return false;
	}

	ENiagaraSimTarget Target = ENiagaraSimTarget::CPUSim;
	if (!ParseSimTarget(SimTarget, Target))
	{
		OutError = FString::Printf(TEXT("Unsupported sim target '%s'. Use CPU/CPUSim or GPU/GPUComputeSim."), *SimTarget);
		return false;
	}

	FVersionedNiagaraEmitterData* EmitterData = Handle ? Handle->GetEmitterData() : nullptr;
	if (!EmitterData)
	{
		OutError = FString::Printf(TEXT("Emitter alias does not resolve to valid emitter data: %s"), *EmitterAlias);
		return false;
	}

	const FScopedTransaction Transaction(FText::FromString(TEXT("ToolPlayMCP: Set Niagara Emitter Sim Target")));
	System->Modify();
	EmitterData->SimTarget = Target;
	System->MarkPackageDirty();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> Root = BuildSystemResult(System);
	Root->SetStringField(TEXT("emitter"), EmitterAlias);
	Root->SetStringField(TEXT("sim_target"), SimTarget);
	OutJson = ToJsonString(Root);
	return true;
}

bool FToolPlayMCPNiagaraSystemService::ConfigureSpriteRenderer(
	const FString& SystemAssetPath,
	const FString& EmitterAlias,
	int32 RendererIndex,
	const FString& FacingMode,
	const FString& Alignment,
	float PivotU,
	float PivotV,
	FString& OutJson,
	FString& OutError)
{
	UNiagaraSystem* System = Cast<UNiagaraSystem>(FSoftObjectPath(SystemAssetPath).TryLoad());
	if (!System)
	{
		OutError = FString::Printf(TEXT("Asset is not a Niagara System: %s"), *SystemAssetPath);
		return false;
	}

	FNiagaraEmitterHandle* Handle = nullptr;
	if (!ResolveEmitterHandle(System, EmitterAlias, Handle, OutError))
	{
		return false;
	}

	FVersionedNiagaraEmitterData* EmitterData = Handle ? Handle->GetEmitterData() : nullptr;
	if (!EmitterData)
	{
		OutError = FString::Printf(TEXT("Emitter alias does not resolve to valid emitter data: %s"), *EmitterAlias);
		return false;
	}

	const TArray<UNiagaraRendererProperties*>& Renderers = EmitterData->GetRenderers();
	if (!Renderers.IsValidIndex(RendererIndex))
	{
		OutError = FString::Printf(TEXT("Renderer index %d is out of range for emitter %s."), RendererIndex, *EmitterAlias);
		return false;
	}

	UNiagaraSpriteRendererProperties* SpriteRenderer = Cast<UNiagaraSpriteRendererProperties>(Renderers[RendererIndex]);
	if (!SpriteRenderer)
	{
		OutError = FString::Printf(TEXT("Renderer %d on emitter %s is not a Niagara Sprite Renderer."), RendererIndex, *EmitterAlias);
		return false;
	}

	ENiagaraSpriteFacingMode ParsedFacingMode = ENiagaraSpriteFacingMode::FaceCamera;
	if (!ParseSpriteFacingMode(FacingMode, ParsedFacingMode))
	{
		OutError = FString::Printf(TEXT("Unsupported sprite facing mode '%s'."), *FacingMode);
		return false;
	}

	ENiagaraSpriteAlignment ParsedAlignment = ENiagaraSpriteAlignment::Unaligned;
	if (!ParseSpriteAlignment(Alignment, ParsedAlignment))
	{
		OutError = FString::Printf(TEXT("Unsupported sprite alignment '%s'."), *Alignment);
		return false;
	}

	const FScopedTransaction Transaction(FText::FromString(TEXT("ToolPlayMCP: Configure Niagara Sprite Renderer")));
	System->Modify();
	SpriteRenderer->Modify();
	SpriteRenderer->FacingMode = ParsedFacingMode;
	SpriteRenderer->Alignment = ParsedAlignment;
	SpriteRenderer->PivotInUVSpace = FVector2D(PivotU, PivotV);
	System->MarkPackageDirty();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> Root = BuildSystemResult(System);
	Root->SetStringField(TEXT("emitter"), EmitterAlias);
	Root->SetNumberField(TEXT("renderer_index"), RendererIndex);
	Root->SetStringField(TEXT("facing_mode"), FacingMode);
	Root->SetStringField(TEXT("alignment"), Alignment);
	Root->SetArrayField(TEXT("pivot_uv"), {
		MakeShared<FJsonValueNumber>(PivotU),
		MakeShared<FJsonValueNumber>(PivotV)
	});
	OutJson = ToJsonString(Root);
	return true;
}
