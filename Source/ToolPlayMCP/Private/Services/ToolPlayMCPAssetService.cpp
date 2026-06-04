#include "Services/ToolPlayMCPAssetService.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"

namespace
{
	FString JsonObjectToString(const TSharedRef<FJsonObject>& Object)
	{
		FString Output;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
		FJsonSerializer::Serialize(Object, Writer);
		return Output;
	}
}

bool FToolPlayMCPAssetService::SaveAssetByPath(const FString& AssetPath, FString& OutJson, FString& OutError)
{
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("save_asset requires asset_path.");
		return false;
	}

	UObject* AssetObject = FSoftObjectPath(AssetPath).TryLoad();
	if (!AssetObject)
	{
		OutError = FString::Printf(TEXT("Failed to load asset '%s'."), *AssetPath);
		return false;
	}

	UPackage* Package = AssetObject->GetOutermost();
	if (!Package)
	{
		OutError = TEXT("Asset has no package.");
		return false;
	}

	const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_None;

	if (!UPackage::SavePackage(Package, AssetObject, *Filename, SaveArgs))
	{
		OutError = FString::Printf(TEXT("Failed to save asset package '%s'."), *Package->GetName());
		return false;
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("class"), AssetObject->GetClass()->GetName());
	Root->SetBoolField(TEXT("saved"), true);
	Root->SetStringField(TEXT("filename"), Filename);
	OutJson = JsonObjectToString(Root);
	return true;
}
