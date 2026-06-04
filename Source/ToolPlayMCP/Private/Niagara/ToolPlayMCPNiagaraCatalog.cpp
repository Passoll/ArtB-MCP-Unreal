#include "Niagara/ToolPlayMCPNiagaraCatalog.h"

#include "AssetRegistry/AssetData.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraScript.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	FString ToJsonString(const TSharedRef<FJsonObject>& Object)
	{
		FString Output;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
		FJsonSerializer::Serialize(Object, Writer);
		return Output;
	}

	FString EnumValueToString(const TCHAR* EnumPath, int64 Value)
	{
		if (const UEnum* Enum = FindObject<UEnum>(nullptr, EnumPath))
		{
			return Enum->GetNameStringByValue(Value);
		}
		return FString::FromInt(static_cast<int32>(Value));
	}

	FString ScriptUsageToString(ENiagaraScriptUsage Usage)
	{
		return EnumValueToString(TEXT("/Script/Niagara.ENiagaraScriptUsage"), static_cast<int64>(Usage));
	}

	bool ParseUsage(const FString& Usage, ENiagaraScriptUsage& OutUsage)
	{
		const FString Normalized = Usage.ToLower();
		if (Normalized.IsEmpty() || Normalized == TEXT("module"))
		{
			OutUsage = ENiagaraScriptUsage::Module;
			return true;
		}
		if (Normalized == TEXT("dynamic_input") || Normalized == TEXT("dynamicinput"))
		{
			OutUsage = ENiagaraScriptUsage::DynamicInput;
			return true;
		}
		if (Normalized == TEXT("function"))
		{
			OutUsage = ENiagaraScriptUsage::Function;
			return true;
		}
		return false;
	}

	FString AssetPathString(const FAssetData& Asset)
	{
		return Asset.GetSoftObjectPath().ToString();
	}

	FString SourceKind(const FString& AssetPath)
	{
		if (AssetPath.StartsWith(TEXT("/Game/")))
		{
			return TEXT("project");
		}
		if (AssetPath.StartsWith(TEXT("/Niagara/")) || AssetPath.StartsWith(TEXT("/Engine/")))
		{
			return TEXT("native");
		}
		return TEXT("plugin");
	}

	bool SourceMatches(const FString& AssetPath, const FString& Source)
	{
		const FString Normalized = Source.ToLower();
		if (Normalized.IsEmpty() || Normalized == TEXT("all"))
		{
			return true;
		}
		return SourceKind(AssetPath).Equals(Normalized, ESearchCase::IgnoreCase);
	}

	FString DeriveCategory(const FString& AssetPath)
	{
		FString Path = AssetPath;
		FString PackagePath;
		FString AssetName;
		Path.Split(TEXT("."), &PackagePath, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd);

		int32 ModulesIndex = INDEX_NONE;
		if (PackagePath.FindChar(TEXT('/'), ModulesIndex))
		{
		}

		const FString ModulesMarker = TEXT("/Modules/");
		const int32 MarkerIndex = PackagePath.Find(ModulesMarker);
		if (MarkerIndex != INDEX_NONE)
		{
			FString Category = PackagePath.Mid(MarkerIndex + ModulesMarker.Len());
			Category.RemoveFromEnd(TEXT("/") + AssetName);
			return Category;
		}

		const FString DynamicMarker = TEXT("/DynamicInputs/");
		const int32 DynamicIndex = PackagePath.Find(DynamicMarker);
		if (DynamicIndex != INDEX_NONE)
		{
			FString Category = PackagePath.Mid(DynamicIndex + DynamicMarker.Len());
			Category.RemoveFromEnd(TEXT("/") + AssetName);
			return Category;
		}

		return FPaths::GetPath(PackagePath);
	}

	bool QueryMatches(const FAssetData& Asset, const FString& Query, const FString& AssetPath, const FString& Category)
	{
		if (Query.IsEmpty())
		{
			return true;
		}

		const FString Haystack = FString::Printf(
			TEXT("%s %s %s %s"),
			*Asset.AssetName.ToString(),
			*Asset.AssetClassPath.ToString(),
			*AssetPath,
			*Category).ToLower();
		return Haystack.Contains(Query.ToLower());
	}

	TArray<TSharedPtr<FJsonValue>> ExportInputs(UNiagaraScript* Script)
	{
		TArray<TSharedPtr<FJsonValue>> Inputs;
		if (!Script)
		{
			return Inputs;
		}

		// Catalog input introspection for standalone scripts will be expanded once
		// module call context is available. Keep search results lightweight for now.
		return Inputs;
	}
}

bool FToolPlayMCPNiagaraCatalog::SearchScripts(
	const FString& Query,
	const FString& Usage,
	const FString& Source,
	int32 Limit,
	FString& OutJson,
	FString& OutError)
{
	ENiagaraScriptUsage ScriptUsage = ENiagaraScriptUsage::Module;
	if (!ParseUsage(Usage, ScriptUsage))
	{
		OutError = FString::Printf(TEXT("Unsupported Niagara script usage '%s'. Use module, dynamic_input, or function."), *Usage);
		return false;
	}

	TArray<FAssetData> Assets;
	FNiagaraEditorUtilities::FGetFilteredScriptAssetsOptions Options;
	Options.ScriptUsageToInclude = ScriptUsage;
	FNiagaraEditorUtilities::GetFilteredScriptAssets(Options, Assets);

	const int32 SafeLimit = FMath::Clamp(Limit, 1, 100);
	TArray<TSharedPtr<FJsonValue>> Entries;
	int32 MatchCount = 0;

	for (const FAssetData& Asset : Assets)
	{
		const FString AssetPath = AssetPathString(Asset);
		const FString Category = DeriveCategory(AssetPath);
		if (!SourceMatches(AssetPath, Source) || !QueryMatches(Asset, Query, AssetPath, Category))
		{
			continue;
		}

		++MatchCount;
		if (Entries.Num() >= SafeLimit)
		{
			continue;
		}

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		Entry->SetStringField(TEXT("asset"), AssetPath);
		Entry->SetStringField(TEXT("usage"), ScriptUsageToString(ScriptUsage));
		Entry->SetStringField(TEXT("source"), SourceKind(AssetPath));
		Entry->SetStringField(TEXT("category"), Category);

		if (UNiagaraScript* Script = Cast<UNiagaraScript>(Asset.GetAsset()))
		{
			Entry->SetStringField(TEXT("display_name"), Asset.AssetName.ToString());
			Entry->SetArrayField(TEXT("inputs"), ExportInputs(Script));
		}

		Entries.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetStringField(TEXT("query"), Query);
	Root->SetStringField(TEXT("usage"), ScriptUsageToString(ScriptUsage));
	Root->SetStringField(TEXT("source"), Source.IsEmpty() ? TEXT("all") : Source);
	Root->SetNumberField(TEXT("count"), MatchCount);
	Root->SetBoolField(TEXT("truncated"), MatchCount > SafeLimit);
	Root->SetArrayField(TEXT("entries"), Entries);
	OutJson = ToJsonString(Root);
	return true;
}
