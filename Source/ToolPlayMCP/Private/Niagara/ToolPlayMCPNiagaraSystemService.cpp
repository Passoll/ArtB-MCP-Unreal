#include "Niagara/ToolPlayMCPNiagaraSystemService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraEditorSettings.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
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
}

bool FToolPlayMCPNiagaraSystemService::CreateSystem(const FString& PackagePath, const FString& AssetName, const FString& TemplateAssetPath, FString& OutJson, FString& OutError)
{
	FString LongPackageName;
	if (!ValidateAssetDestination(PackagePath, AssetName, LongPackageName, OutError))
	{
		return false;
	}

	UPackage* Package = CreatePackage(*LongPackageName);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("Failed to create package: %s"), *LongPackageName);
		return false;
	}

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
