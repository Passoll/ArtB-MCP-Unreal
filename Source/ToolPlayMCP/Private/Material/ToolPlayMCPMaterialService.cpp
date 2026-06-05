#include "Material/ToolPlayMCPMaterialService.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "ContentBrowserModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "IContentBrowserSingleton.h"
#include "JsonObjectConverter.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/Package.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Materials/Material.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionClamp.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Engine/Texture.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"
#include "UObject/SavePackage.h"

namespace
{
	const FString MaterialRootNodeId(TEXT("material-root"));
	const FString CompactMaterialRootAlias(TEXT("root"));
	TMap<FString, FToolPlayMCPMaterialGraphSession> GMaterialGraphSessions;

	FString PinDirectionToString(const EEdGraphPinDirection Direction)
	{
		return Direction == EGPD_Input ? TEXT("input") : TEXT("output");
	}

	FString ExtractPinType(const UEdGraphPin* Pin)
	{
		if (!Pin)
		{
			return TEXT("unknown");
		}

		if (!Pin->PinType.PinCategory.IsNone())
		{
			return Pin->PinType.PinCategory.ToString();
		}

		return TEXT("unknown");
	}

	FString BuildMaterialExpressionId(const UMaterialExpression* Expression)
	{
		if (!Expression)
		{
			return TEXT("invalid-material-node");
		}

		if (Expression->MaterialExpressionGuid.IsValid())
		{
			return Expression->MaterialExpressionGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
		}

		return Expression->GetName();
	}

	FString NormalizeMaterialExpressionKind(const UMaterialExpression* Expression)
	{
		if (Expression->IsA<UMaterialExpressionMaterialFunctionCall>())
		{
			return TEXT("func");
		}
		if (Expression->IsA<UMaterialExpressionTextureSample>())
		{
			return TEXT("tex");
		}
		if (Expression->IsA<UMaterialExpressionMultiply>())
		{
			return TEXT("*");
		}
		if (Expression->IsA<UMaterialExpressionAdd>())
		{
			return TEXT("+");
		}
		if (Expression->IsA<UMaterialExpressionSubtract>())
		{
			return TEXT("-");
		}
		if (Expression->IsA<UMaterialExpressionDivide>())
		{
			return TEXT("/");
		}
		if (Expression->IsA<UMaterialExpressionLinearInterpolate>())
		{
			return TEXT("lerp");
		}
		if (Expression->IsA<UMaterialExpressionOneMinus>())
		{
			return TEXT("1-x");
		}
		if (Expression->IsA<UMaterialExpressionClamp>())
		{
			return TEXT("clamp");
		}
		if (Expression->IsA<UMaterialExpressionScalarParameter>())
		{
			return TEXT("param");
		}
		if (Expression->IsA<UMaterialExpressionVectorParameter>())
		{
			return TEXT("vparam");
		}
		if (Expression->IsA<UMaterialExpressionConstant>() || Expression->IsA<UMaterialExpressionConstant3Vector>())
		{
			return TEXT("const");
		}

		FString ClassName = Expression ? Expression->GetClass()->GetName() : TEXT("unknown");
		ClassName.RemoveFromStart(TEXT("MaterialExpression"));
		return ClassName;
	}

	FString NormalizeMaterialPinName(const FString& PinName)
	{
		if (PinName.IsEmpty() || PinName.StartsWith(TEXT("Output")))
		{
			return TEXT("out");
		}

		if (PinName.Equals(TEXT("RGB"), ESearchCase::IgnoreCase))
		{
			return TEXT("rgb");
		}
		if (PinName.Equals(TEXT("RGBA"), ESearchCase::IgnoreCase))
		{
			return TEXT("rgba");
		}
		if (PinName.Equals(TEXT("Alpha"), ESearchCase::IgnoreCase))
		{
			return TEXT("alpha");
		}
		if (PinName.Len() == 1)
		{
			return PinName.ToLower();
		}

		return PinName;
	}

	FString GetMaterialOutputName(const UMaterialExpression* Expression, const int32 OutputIndex)
	{
		if (!Expression)
		{
			return TEXT("out");
		}

		if (Expression->Outputs.IsValidIndex(OutputIndex) && !Expression->Outputs[OutputIndex].OutputName.IsNone())
		{
			return NormalizeMaterialPinName(Expression->Outputs[OutputIndex].OutputName.ToString());
		}

		return OutputIndex <= 0 ? TEXT("out") : FString::Printf(TEXT("out%d"), OutputIndex);
	}

	FString FormatCompactFloat(const float Value)
	{
		FString Text = FString::SanitizeFloat(Value);
		while (Text.Contains(TEXT(".")) && Text.EndsWith(TEXT("0")))
		{
			Text.LeftChopInline(1);
		}
		if (Text.EndsWith(TEXT(".")))
		{
			Text.LeftChopInline(1);
		}
		return Text;
	}

	FString ExtractMaterialNodeLabel(const UMaterialExpression* Expression)
	{
		if (const UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
		{
			return FunctionCall->MaterialFunction ? FunctionCall->MaterialFunction->GetName() : FString();
		}

		if (const UMaterialExpressionTextureSample* TextureSample = Cast<UMaterialExpressionTextureSample>(Expression))
		{
			return TextureSample->Texture ? TextureSample->Texture->GetName() : FString();
		}

		if (const UMaterialExpressionParameter* Parameter = Cast<UMaterialExpressionParameter>(Expression))
		{
			return Parameter->ParameterName.IsNone() ? FString() : Parameter->ParameterName.ToString();
		}

		return FString();
	}

	FString ExtractMaterialNodeValue(const UMaterialExpression* Expression)
	{
		if (const UMaterialExpressionScalarParameter* ScalarParameter = Cast<UMaterialExpressionScalarParameter>(Expression))
		{
			return FormatCompactFloat(ScalarParameter->DefaultValue);
		}

		if (const UMaterialExpressionConstant* Constant = Cast<UMaterialExpressionConstant>(Expression))
		{
			return FormatCompactFloat(Constant->R);
		}

		if (const UMaterialExpressionVectorParameter* VectorParameter = Cast<UMaterialExpressionVectorParameter>(Expression))
		{
			const FLinearColor& Value = VectorParameter->DefaultValue;
			return FString::Printf(
				TEXT("[%s,%s,%s,%s]"),
				*FormatCompactFloat(Value.R),
				*FormatCompactFloat(Value.G),
				*FormatCompactFloat(Value.B),
				*FormatCompactFloat(Value.A));
		}

		if (const UMaterialExpressionConstant3Vector* Constant3 = Cast<UMaterialExpressionConstant3Vector>(Expression))
		{
			const FLinearColor& Value = Constant3->Constant;
			return FString::Printf(
				TEXT("[%s,%s,%s]"),
				*FormatCompactFloat(Value.R),
				*FormatCompactFloat(Value.G),
				*FormatCompactFloat(Value.B));
		}

		return FString();
	}

	FString FormatCompactColor(const FLinearColor& Value)
	{
		return FString::Printf(
			TEXT("[%s,%s,%s,%s]"),
			*FormatCompactFloat(Value.R),
			*FormatCompactFloat(Value.G),
			*FormatCompactFloat(Value.B),
			*FormatCompactFloat(Value.A));
	}

	FString FormatCompactVector4f(const FVector4f& Value)
	{
		return FString::Printf(
			TEXT("[%s,%s,%s,%s]"),
			*FormatCompactFloat(Value.X),
			*FormatCompactFloat(Value.Y),
			*FormatCompactFloat(Value.Z),
			*FormatCompactFloat(Value.W));
	}

	FString MaterialFunctionUsageToString(const EMaterialFunctionUsage Usage)
	{
		switch (Usage)
		{
		case EMaterialFunctionUsage::MaterialLayer:
			return TEXT("MaterialLayer");
		case EMaterialFunctionUsage::MaterialLayerBlend:
			return TEXT("MaterialLayerBlend");
		case EMaterialFunctionUsage::Default:
		default:
			return TEXT("Default");
		}
	}

	void ApplyMaterialInstanceOverrides(UMaterialInstance* MaterialInstance, FToolPlayMCPCompactMaterialGraph& Graph)
	{
		if (!MaterialInstance)
		{
			return;
		}

		TMap<FString, FString> OverrideValuesByName;

		for (const FScalarParameterValue& Scalar : MaterialInstance->ScalarParameterValues)
		{
			const FString Name = Scalar.ParameterInfo.Name.ToString();
			const FString Value = FormatCompactFloat(Scalar.ParameterValue);
			OverrideValuesByName.Add(Name, Value);
			Graph.Overrides.Add(Name, Value);
		}

		for (const FVectorParameterValue& Vector : MaterialInstance->VectorParameterValues)
		{
			const FString Name = Vector.ParameterInfo.Name.ToString();
			const FString Value = FormatCompactColor(Vector.ParameterValue);
			OverrideValuesByName.Add(Name, Value);
			Graph.Overrides.Add(Name, Value);
		}

		for (const FTextureParameterValue& Texture : MaterialInstance->TextureParameterValues)
		{
			const FString Name = Texture.ParameterInfo.Name.ToString();
			const FString Value = Texture.ParameterValue ? Texture.ParameterValue->GetName() : TEXT("none");
			OverrideValuesByName.Add(Name, Value);
			Graph.Overrides.Add(Name, Value);
		}

		for (TPair<FString, FToolPlayMCPCompactMaterialNode>& NodePair : Graph.Nodes)
		{
			if (NodePair.Value.Label.IsEmpty())
			{
				continue;
			}

			if (const FString* OverrideValue = OverrideValuesByName.Find(NodePair.Value.Label))
			{
				NodePair.Value.V = *OverrideValue;
			}
		}
	}

	TSharedRef<FJsonObject> CompactGraphToJsonObject(const FToolPlayMCPCompactMaterialGraph& Document)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("asset"), Document.Asset);
		Root->SetStringField(TEXT("scope"), Document.Scope);
		if (!Document.Parent.IsEmpty())
		{
			Root->SetStringField(TEXT("parent"), Document.Parent);
		}

		if (!Document.Overrides.IsEmpty())
		{
			TSharedRef<FJsonObject> OverridesObject = MakeShared<FJsonObject>();
			for (const TPair<FString, FString>& Pair : Document.Overrides)
			{
				OverridesObject->SetStringField(Pair.Key, Pair.Value);
			}
			Root->SetObjectField(TEXT("overrides"), OverridesObject);
		}

		TSharedRef<FJsonObject> NodesObject = MakeShared<FJsonObject>();
		for (const TPair<FString, FToolPlayMCPCompactMaterialNode>& Pair : Document.Nodes)
		{
			TSharedRef<FJsonObject> NodeObject = MakeShared<FJsonObject>();
			NodeObject->SetStringField(TEXT("k"), Pair.Value.K);

			if (!Pair.Value.Label.IsEmpty())
			{
				NodeObject->SetStringField(TEXT("label"), Pair.Value.Label);
			}

			if (!Pair.Value.V.IsEmpty())
			{
				NodeObject->SetStringField(TEXT("v"), Pair.Value.V);
			}

			NodesObject->SetObjectField(Pair.Key, NodeObject);
		}
		Root->SetObjectField(TEXT("nodes"), NodesObject);

		TArray<TSharedPtr<FJsonValue>> EdgesArray;
		for (const TArray<FString>& Edge : Document.Edges)
		{
			TArray<TSharedPtr<FJsonValue>> EdgeItems;
			for (const FString& Item : Edge)
			{
				EdgeItems.Add(MakeShared<FJsonValueString>(Item));
			}
			EdgesArray.Add(MakeShared<FJsonValueArray>(MoveTemp(EdgeItems)));
		}
		Root->SetArrayField(TEXT("edges"), MoveTemp(EdgesArray));

		return Root;
	}

	FString JsonObjectToString(const TSharedRef<FJsonObject>& Object)
	{
		FString Output;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
		FJsonSerializer::Serialize(Object, Writer);
		return Output;
	}

	bool SaveAssetObject(UObject* AssetObject, FString& OutFilename, FString& OutError)
	{
		if (!AssetObject)
		{
			OutError = TEXT("save_asset requires a loaded asset object.");
			return false;
		}

		UPackage* Package = AssetObject->GetOutermost();
		if (!Package)
		{
			OutError = FString::Printf(TEXT("Asset '%s' has no outer package."), *AssetObject->GetName());
			return false;
		}

		OutFilename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;

		if (!UPackage::SavePackage(Package, AssetObject, *OutFilename, SaveArgs))
		{
			OutError = FString::Printf(TEXT("Failed to save asset '%s' to '%s'."), *AssetObject->GetPathName(), *OutFilename);
			return false;
		}

		Package->SetDirtyFlag(false);
		return true;
	}

	FString FormatEndpoint(const FString& Node, const FString& Pin)
	{
		return FString::Printf(TEXT("%s.%s"), *Node, *Pin);
	}

	FString BuildCompactEdgeKey(const TArray<FString>& Edge)
	{
		if (Edge.Num() != 4)
		{
			return TEXT("invalid-edge");
		}

		return FString::Printf(TEXT("%s|%s|%s|%s"), *Edge[0], *Edge[1], *Edge[2], *Edge[3]);
	}

	bool IsTransparentMaterialNodeKind(const FString& Kind)
	{
		return Kind == TEXT("Reroute") || Kind == TEXT("NamedRerouteUsage") || Kind == TEXT("NamedRerouteDeclaration");
	}

	void AddCompactNodeJson(const FToolPlayMCPCompactMaterialGraph& Graph, const FString& Alias, TSharedRef<FJsonObject>& NodesObject)
	{
		if (NodesObject->HasField(Alias))
		{
			return;
		}

		const FToolPlayMCPCompactMaterialNode* Node = Graph.Nodes.Find(Alias);
		if (!Node)
		{
			return;
		}

		TSharedRef<FJsonObject> NodeObject = MakeShared<FJsonObject>();
		NodeObject->SetStringField(TEXT("k"), Node->K);
		if (!Node->Label.IsEmpty())
		{
			NodeObject->SetStringField(TEXT("label"), Node->Label);
		}
		if (!Node->V.IsEmpty())
		{
			NodeObject->SetStringField(TEXT("v"), Node->V);
		}
		NodesObject->SetObjectField(Alias, NodeObject);
	}

	void AddEdgeJson(TArray<TSharedPtr<FJsonValue>>& EdgesArray, const TArray<FString>& Edge)
	{
		TArray<TSharedPtr<FJsonValue>> EdgeItems;
		for (const FString& Item : Edge)
		{
			EdgeItems.Add(MakeShared<FJsonValueString>(Item));
		}
		EdgesArray.Add(MakeShared<FJsonValueArray>(MoveTemp(EdgeItems)));
	}

	void AddChainJson(TArray<TSharedPtr<FJsonValue>>& ChainsArray, const TArray<FString>& Chain)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FString& Item : Chain)
		{
			Items.Add(MakeShared<FJsonValueString>(Item));
		}
		ChainsArray.Add(MakeShared<FJsonValueArray>(MoveTemp(Items)));
	}

	void AddUniqueGraphLink(
		FToolPlayMCPGraphExportGraph& Graph,
		TSet<FString>& SeenLinks,
		const FString& FromNode,
		const FString& FromPin,
		const FString& ToNode,
		const FString& ToPin,
		TMap<FString, FString> Metadata = {})
	{
		const FString Key = FString::Printf(TEXT("%s|%s|%s|%s"), *FromNode, *FromPin, *ToNode, *ToPin);
		if (SeenLinks.Contains(Key))
		{
			return;
		}

		SeenLinks.Add(Key);

		FToolPlayMCPGraphExportLink Link;
		Link.FromNode = FromNode;
		Link.FromPin = FromPin;
		Link.ToNode = ToNode;
		Link.ToPin = ToPin;
		Link.Metadata = MoveTemp(Metadata);
		Graph.Links.Add(MoveTemp(Link));
	}

	bool TryGetJsonString(const TSharedPtr<FJsonObject>& Object, const FString& Field, FString& OutValue)
	{
		return Object.IsValid() && Object->TryGetStringField(Field, OutValue) && !OutValue.IsEmpty();
	}

	void AddPatchMessage(TArray<TSharedPtr<FJsonValue>>& Messages, const FString& Level, const int32 OpIndex, const FString& Message)
	{
		TSharedRef<FJsonObject> MessageObject = MakeShared<FJsonObject>();
		MessageObject->SetStringField(TEXT("level"), Level);
		MessageObject->SetNumberField(TEXT("op_index"), OpIndex);
		MessageObject->SetStringField(TEXT("message"), Message);
		Messages.Add(MakeShared<FJsonValueObject>(MessageObject));
	}

	TSharedRef<FJsonObject> MakePatchOpResult(const int32 OpIndex, const FString& Op, const bool bOk, const FString& Message)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("op_index"), OpIndex);
		Result->SetStringField(TEXT("op"), Op);
		Result->SetBoolField(TEXT("ok"), bOk);
		if (!Message.IsEmpty())
		{
			Result->SetStringField(TEXT("message"), Message);
		}
		return Result;
	}

	bool ParsePatchJson(const FString& PatchJson, TSharedPtr<FJsonObject>& OutPatch, FString& OutError)
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PatchJson);
		if (!FJsonSerializer::Deserialize(Reader, OutPatch) || !OutPatch.IsValid())
		{
			OutError = TEXT("Invalid material patch JSON.");
			return false;
		}
		return true;
	}

	UObject* LoadPatchAsset(const FString& AssetPath, FString& OutError)
	{
		if (AssetPath.IsEmpty())
		{
			OutError = TEXT("asset_path is required.");
			return nullptr;
		}

		UObject* Asset = FSoftObjectPath(AssetPath).TryLoad();
		if (!Asset)
		{
			OutError = FString::Printf(TEXT("Unable to load asset '%s'."), *AssetPath);
			return nullptr;
		}

		return Asset;
	}

	bool TryGetPatchPosition(const TSharedPtr<FJsonObject>& NodeObject, int32& OutX, int32& OutY)
	{
		const TArray<TSharedPtr<FJsonValue>>* PositionArray = nullptr;
		if (NodeObject.IsValid() && NodeObject->TryGetArrayField(TEXT("position"), PositionArray) && PositionArray && PositionArray->Num() >= 2)
		{
			OutX = static_cast<int32>((*PositionArray)[0]->AsNumber());
			OutY = static_cast<int32>((*PositionArray)[1]->AsNumber());
			return true;
		}

		return false;
	}

	bool JsonValueToLinearColor(const TSharedPtr<FJsonValue>& Value, FLinearColor& OutColor)
	{
		if (!Value.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Value->TryGetArray(Array) && Array && Array->Num() >= 3)
		{
			OutColor.R = (*Array)[0]->AsNumber();
			OutColor.G = (*Array)[1]->AsNumber();
			OutColor.B = (*Array)[2]->AsNumber();
			OutColor.A = Array->Num() >= 4 ? (*Array)[3]->AsNumber() : 1.0f;
			return true;
		}

		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (Value->TryGetObject(Object) && Object && Object->IsValid())
		{
			OutColor.R = (*Object)->GetNumberField(TEXT("r"));
			OutColor.G = (*Object)->GetNumberField(TEXT("g"));
			OutColor.B = (*Object)->GetNumberField(TEXT("b"));
			OutColor.A = (*Object)->HasField(TEXT("a")) ? (*Object)->GetNumberField(TEXT("a")) : 1.0f;
			return true;
		}

		return false;
	}

	bool ApplyJsonPropertyValue(UObject* Target, FProperty* Property, const TSharedPtr<FJsonValue>& Value, FString& OutError)
	{
		if (!Target || !Property || !Value.IsValid())
		{
			OutError = TEXT("Invalid property assignment target.");
			return false;
		}

		void* PropertyAddress = Property->ContainerPtrToValuePtr<void>(Target);
		if (FFloatProperty* FloatProperty = CastField<FFloatProperty>(Property))
		{
			FloatProperty->SetPropertyValue(PropertyAddress, static_cast<float>(Value->AsNumber()));
			return true;
		}
		if (FDoubleProperty* DoubleProperty = CastField<FDoubleProperty>(Property))
		{
			DoubleProperty->SetPropertyValue(PropertyAddress, Value->AsNumber());
			return true;
		}
		if (FIntProperty* IntProperty = CastField<FIntProperty>(Property))
		{
			IntProperty->SetPropertyValue(PropertyAddress, static_cast<int32>(Value->AsNumber()));
			return true;
		}
		if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			BoolProperty->SetPropertyValue(PropertyAddress, Value->AsBool());
			return true;
		}
		if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			if (UEnum* Enum = ByteProperty->Enum)
			{
				int64 EnumValue = INDEX_NONE;
				if (Value->Type == EJson::String)
				{
					EnumValue = Enum->GetValueByNameString(Value->AsString());
				}
				else
				{
					EnumValue = static_cast<int64>(Value->AsNumber());
				}

				if (EnumValue == INDEX_NONE)
				{
					OutError = FString::Printf(TEXT("Enum value '%s' is invalid for property '%s'."), *Value->AsString(), *Property->GetName());
					return false;
				}
				ByteProperty->SetPropertyValue(PropertyAddress, static_cast<uint8>(EnumValue));
				return true;
			}

			ByteProperty->SetPropertyValue(PropertyAddress, static_cast<uint8>(Value->AsNumber()));
			return true;
		}
		if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			UEnum* Enum = EnumProperty->GetEnum();
			int64 EnumValue = INDEX_NONE;
			if (Value->Type == EJson::String)
			{
				EnumValue = Enum ? Enum->GetValueByNameString(Value->AsString()) : INDEX_NONE;
			}
			else
			{
				EnumValue = static_cast<int64>(Value->AsNumber());
			}

			if (EnumValue == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("Enum value '%s' is invalid for property '%s'."), *Value->AsString(), *Property->GetName());
				return false;
			}

			EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(PropertyAddress, EnumValue);
			return true;
		}
		if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			NameProperty->SetPropertyValue(PropertyAddress, FName(*Value->AsString()));
			return true;
		}
		if (FStrProperty* StringProperty = CastField<FStrProperty>(Property))
		{
			StringProperty->SetPropertyValue(PropertyAddress, Value->AsString());
			return true;
		}
		if (FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
		{
			UObject* LoadedObject = FSoftObjectPath(Value->AsString()).TryLoad();
			if (!LoadedObject || !LoadedObject->IsA(ObjectProperty->PropertyClass))
			{
				OutError = FString::Printf(TEXT("Unable to load object property '%s' as '%s'."), *Property->GetName(), *ObjectProperty->PropertyClass->GetName());
				return false;
			}
			ObjectProperty->SetObjectPropertyValue(PropertyAddress, LoadedObject);
			return true;
		}
		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct && StructProperty->Struct->GetFName() == FName(TEXT("LinearColor")))
			{
				FLinearColor Color;
				if (!JsonValueToLinearColor(Value, Color))
				{
					OutError = FString::Printf(TEXT("Property '%s' expects a color array/object."), *Property->GetName());
					return false;
				}
				*static_cast<FLinearColor*>(PropertyAddress) = Color;
				return true;
			}
		}

		OutError = FString::Printf(TEXT("Unsupported property type for '%s'."), *Property->GetName());
		return false;
	}

	bool ApplyJsonProperties(UObject* Target, const TSharedPtr<FJsonObject>& PropertiesObject, FString& OutError)
	{
		if (!PropertiesObject.IsValid())
		{
			return true;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : PropertiesObject->Values)
		{
			FProperty* Property = Target ? Target->GetClass()->FindPropertyByName(FName(*Pair.Key)) : nullptr;
			if (!Property)
			{
				OutError = FString::Printf(TEXT("Property '%s' does not exist on '%s'."), *Pair.Key, Target ? *Target->GetClass()->GetName() : TEXT("null"));
				return false;
			}
			if (!ApplyJsonPropertyValue(Target, Property, Pair.Value, OutError))
			{
				return false;
			}
		}

		return true;
	}

	bool IsPatchVisibleProperty(const FProperty* Property)
	{
		if (!Property)
		{
			return false;
		}

		if (Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient | CPF_DisableEditOnInstance))
		{
			return false;
		}

		return Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible);
	}

	FString PatchPropertyTypeName(const FProperty* Property)
	{
		if (CastField<FFloatProperty>(Property) || CastField<FDoubleProperty>(Property))
		{
			return TEXT("number");
		}
		if (CastField<FIntProperty>(Property))
		{
			return TEXT("integer");
		}
		if (CastField<FBoolProperty>(Property))
		{
			return TEXT("bool");
		}
		if (CastField<FNameProperty>(Property))
		{
			return TEXT("name");
		}
		if (CastField<FStrProperty>(Property))
		{
			return TEXT("string");
		}
		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			return ByteProperty->Enum ? TEXT("enum") : TEXT("byte");
		}
		if (CastField<FEnumProperty>(Property))
		{
			return TEXT("enum");
		}
		if (const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
		{
			return FString::Printf(TEXT("object:%s"), *ObjectProperty->PropertyClass->GetName());
		}
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			return StructProperty->Struct ? FString::Printf(TEXT("struct:%s"), *StructProperty->Struct->GetName()) : TEXT("struct");
		}
		return TEXT("unsupported");
	}

	TSharedPtr<FJsonValue> PropertyValueToJsonValue(UObject* Target, FProperty* Property)
	{
		if (!Target || !Property)
		{
			return nullptr;
		}

		void* PropertyAddress = Property->ContainerPtrToValuePtr<void>(Target);
		if (FFloatProperty* FloatProperty = CastField<FFloatProperty>(Property))
		{
			return MakeShared<FJsonValueNumber>(FloatProperty->GetPropertyValue(PropertyAddress));
		}
		if (FDoubleProperty* DoubleProperty = CastField<FDoubleProperty>(Property))
		{
			return MakeShared<FJsonValueNumber>(DoubleProperty->GetPropertyValue(PropertyAddress));
		}
		if (FIntProperty* IntProperty = CastField<FIntProperty>(Property))
		{
			return MakeShared<FJsonValueNumber>(IntProperty->GetPropertyValue(PropertyAddress));
		}
		if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			return MakeShared<FJsonValueBoolean>(BoolProperty->GetPropertyValue(PropertyAddress));
		}
		if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			return MakeShared<FJsonValueString>(NameProperty->GetPropertyValue(PropertyAddress).ToString());
		}
		if (FStrProperty* StringProperty = CastField<FStrProperty>(Property))
		{
			return MakeShared<FJsonValueString>(StringProperty->GetPropertyValue(PropertyAddress));
		}
		if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			const uint8 Value = ByteProperty->GetPropertyValue(PropertyAddress);
			if (ByteProperty->Enum)
			{
				return MakeShared<FJsonValueString>(ByteProperty->Enum->GetNameStringByValue(Value));
			}
			return MakeShared<FJsonValueNumber>(Value);
		}
		if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			const int64 Value = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(PropertyAddress);
			return MakeShared<FJsonValueString>(EnumProperty->GetEnum()->GetNameStringByValue(Value));
		}
		if (FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
		{
			UObject* ObjectValue = ObjectProperty->GetObjectPropertyValue(PropertyAddress);
			return MakeShared<FJsonValueString>(ObjectValue ? ObjectValue->GetPathName() : TEXT(""));
		}
		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct && StructProperty->Struct->GetFName() == FName(TEXT("LinearColor")))
			{
				const FLinearColor& Color = *static_cast<FLinearColor*>(PropertyAddress);
				TArray<TSharedPtr<FJsonValue>> Items;
				Items.Add(MakeShared<FJsonValueNumber>(Color.R));
				Items.Add(MakeShared<FJsonValueNumber>(Color.G));
				Items.Add(MakeShared<FJsonValueNumber>(Color.B));
				Items.Add(MakeShared<FJsonValueNumber>(Color.A));
				return MakeShared<FJsonValueArray>(MoveTemp(Items));
			}
		}

		return nullptr;
	}

	void AddConfigPropertiesJson(UObject* Target, TSharedRef<FJsonObject>& OutProperties)
	{
		if (!Target)
		{
			return;
		}

		for (TFieldIterator<FProperty> It(Target->GetClass()); It; ++It)
		{
			FProperty* Property = *It;
			if (!IsPatchVisibleProperty(Property))
			{
				continue;
			}

			TSharedPtr<FJsonValue> Value = PropertyValueToJsonValue(Target, Property);
			if (Value.IsValid())
			{
				OutProperties->SetField(Property->GetName(), Value);
			}
		}
	}

	void AddConfigSchemaJson(UClass* Class, TArray<TSharedPtr<FJsonValue>>& OutProperties)
	{
		if (!Class)
		{
			return;
		}

		for (TFieldIterator<FProperty> It(Class); It; ++It)
		{
			FProperty* Property = *It;
			if (!IsPatchVisibleProperty(Property))
			{
				continue;
			}

			TSharedRef<FJsonObject> PropertyObject = MakeShared<FJsonObject>();
			PropertyObject->SetStringField(TEXT("name"), Property->GetName());
			PropertyObject->SetStringField(TEXT("type"), PatchPropertyTypeName(Property));
			PropertyObject->SetBoolField(TEXT("editable"), true);
			PropertyObject->SetBoolField(TEXT("advanced"), Property->HasMetaData(TEXT("AdvancedDisplay")));
			if (Property->HasMetaData(TEXT("Category")))
			{
				PropertyObject->SetStringField(TEXT("category"), Property->GetMetaData(TEXT("Category")));
			}
			if (Property->HasMetaData(TEXT("DisplayName")))
			{
				PropertyObject->SetStringField(TEXT("display"), Property->GetMetaData(TEXT("DisplayName")));
			}
			if (Property->HasMetaData(TEXT("ToolTip")))
			{
				PropertyObject->SetStringField(TEXT("tooltip"), Property->GetMetaData(TEXT("ToolTip")));
			}

			if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
			{
				if (ByteProperty->Enum)
				{
					TArray<TSharedPtr<FJsonValue>> Values;
					for (int32 Index = 0; Index < ByteProperty->Enum->NumEnums() - 1; ++Index)
					{
						Values.Add(MakeShared<FJsonValueString>(ByteProperty->Enum->GetNameStringByIndex(Index)));
					}
					PropertyObject->SetArrayField(TEXT("values"), MoveTemp(Values));
				}
			}
			if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
			{
				TArray<TSharedPtr<FJsonValue>> Values;
				for (int32 Index = 0; Index < EnumProperty->GetEnum()->NumEnums() - 1; ++Index)
				{
					Values.Add(MakeShared<FJsonValueString>(EnumProperty->GetEnum()->GetNameStringByIndex(Index)));
				}
				PropertyObject->SetArrayField(TEXT("values"), MoveTemp(Values));
			}

			OutProperties.Add(MakeShared<FJsonValueObject>(PropertyObject));
		}
	}

	int32 ResolveMaterialOutputIndex(const UMaterialExpression* Expression, const FString& PinName)
	{
		if (!Expression)
		{
			return INDEX_NONE;
		}

		for (int32 OutputIndex = 0; OutputIndex < Expression->Outputs.Num(); ++OutputIndex)
		{
			if (GetMaterialOutputName(Expression, OutputIndex).Equals(PinName, ESearchCase::IgnoreCase))
			{
				return OutputIndex;
			}
		}

		if (PinName.Equals(TEXT("out"), ESearchCase::IgnoreCase) && Expression->Outputs.Num() > 0)
		{
			return 0;
		}

		return INDEX_NONE;
	}

	FExpressionInput* ResolveMaterialInput(UMaterialExpression* Expression, const FString& PinName)
	{
		if (!Expression)
		{
			return nullptr;
		}

		for (FExpressionInputIterator It(Expression); It; ++It)
		{
			FExpressionInput* Input = It.Input;
			if (!Input)
			{
				continue;
			}

			FString InputName = Expression->GetInputName(It.Index).ToString();
			if (InputName.IsEmpty())
			{
				InputName = FString::Printf(TEXT("in%d"), It.Index);
			}

			if (NormalizeMaterialPinName(InputName).Equals(PinName, ESearchCase::IgnoreCase) || InputName.Equals(PinName, ESearchCase::IgnoreCase))
			{
				return Input;
			}
		}

		return nullptr;
	}

	bool MaterialPropertyFromString(const FString& Name, EMaterialProperty& OutProperty)
	{
		for (int32 PropertyIndex = 0; PropertyIndex < MP_MAX; ++PropertyIndex)
		{
			const EMaterialProperty Property = static_cast<EMaterialProperty>(PropertyIndex);
			if (FMaterialAttributeDefinitionMap::GetAttributeName(Property).Equals(Name, ESearchCase::IgnoreCase))
			{
				OutProperty = Property;
				return true;
			}
		}

		return false;
	}

	UMaterialExpression* ResolvePatchNode(
		const FString& NodeId,
		const TMap<FString, UMaterialExpression*>& ExistingNodes,
		const TMap<FString, UMaterialExpression*>& CreatedNodes)
	{
		if (UMaterialExpression* const* Created = CreatedNodes.Find(NodeId))
		{
			return *Created;
		}
		if (UMaterialExpression* const* Existing = ExistingNodes.Find(NodeId))
		{
			return *Existing;
		}
		return nullptr;
	}

	bool BuildExistingNodeMapForPatch(const FString& AssetPath, TMap<FString, UMaterialExpression*>& OutNodes, FString& OutError)
	{
		FString SessionId;
		FToolPlayMCPCompactMaterialGraph Graph;
		if (!FToolPlayMCPMaterialService::BuildCompactMaterialGraphByPath(AssetPath, Graph, SessionId, OutError))
		{
			return false;
		}

		const FToolPlayMCPMaterialGraphSession* Session = GMaterialGraphSessions.Find(SessionId);
		if (!Session)
		{
			OutError = TEXT("Unable to resolve compact graph session bindings.");
			return false;
		}

		for (const TPair<FString, FToolPlayMCPMaterialNodeBinding>& Pair : Session->NodeBindings)
		{
			if (UMaterialExpression* Expression = Pair.Value.Expression.Get())
			{
				OutNodes.Add(Pair.Key, Expression);
			}
		}

		return true;
	}

	bool PatchSetParameter(UObject* AssetObject, const TSharedPtr<FJsonObject>& OpObject, const bool bApply, FString& OutMessage)
	{
		FString Name;
		if (!TryGetJsonString(OpObject, TEXT("parameter"), Name) && !TryGetJsonString(OpObject, TEXT("name"), Name))
		{
			OutMessage = TEXT("set_parameter requires 'parameter' or 'name'.");
			return false;
		}

		FString ValueType;
		if (!OpObject->TryGetStringField(TEXT("value_type"), ValueType))
		{
			OpObject->TryGetStringField(TEXT("type"), ValueType);
		}
		const TSharedPtr<FJsonValue> Value = OpObject->TryGetField(TEXT("value"));
		if (!Value.IsValid())
		{
			OutMessage = TEXT("set_parameter requires 'value'.");
			return false;
		}

		if (!bApply)
		{
			OutMessage = FString::Printf(TEXT("Will set parameter '%s'."), *Name);
			return true;
		}

		const FMaterialParameterInfo ParameterInfo{FName(*Name)};
		if (UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(AssetObject))
		{
			MIC->Modify();
			if (ValueType.Equals(TEXT("texture"), ESearchCase::IgnoreCase))
			{
				UTexture* Texture = Cast<UTexture>(FSoftObjectPath(Value->AsString()).TryLoad());
				if (!Texture)
				{
					OutMessage = FString::Printf(TEXT("Unable to resolve texture asset '%s'."), *Value->AsString());
					return false;
				}
				MIC->SetTextureParameterValueEditorOnly(ParameterInfo, Texture);
				return true;
			}

			if (ValueType.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
			{
				FLinearColor Color;
				if (!JsonValueToLinearColor(Value, Color))
				{
					OutMessage = TEXT("Vector parameter value must be a color array/object.");
					return false;
				}
				MIC->SetVectorParameterValueEditorOnly(ParameterInfo, Color);
				return true;
			}

			MIC->SetScalarParameterValueEditorOnly(ParameterInfo, static_cast<float>(Value->AsNumber()));
			return true;
		}

		if (UMaterial* Material = Cast<UMaterial>(AssetObject))
		{
			Material->Modify();
			if (ValueType.Equals(TEXT("texture"), ESearchCase::IgnoreCase))
			{
				UTexture* Texture = Cast<UTexture>(FSoftObjectPath(Value->AsString()).TryLoad());
				if (!Texture)
				{
					OutMessage = FString::Printf(TEXT("Unable to resolve texture asset '%s'."), *Value->AsString());
					return false;
				}
				return Material->SetTextureParameterValueEditorOnly(FName(*Name), Texture);
			}

			if (ValueType.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
			{
				FLinearColor Color;
				if (!JsonValueToLinearColor(Value, Color))
				{
					OutMessage = TEXT("Vector parameter value must be a color array/object.");
					return false;
				}
				return Material->SetVectorParameterValueEditorOnly(FName(*Name), Color);
			}

			return Material->SetScalarParameterValueEditorOnly(FName(*Name), static_cast<float>(Value->AsNumber()));
		}

		OutMessage = TEXT("set_parameter supports base Material and MaterialInstanceConstant assets.");
		return false;
	}

	bool PatchAddNode(
		UMaterial* Material,
		const TSharedPtr<FJsonObject>& OpObject,
		const bool bApply,
		TMap<FString, UMaterialExpression*>& CreatedNodes,
		FString& OutMessage)
	{
		if (!Material)
		{
			OutMessage = TEXT("add_node requires a base Material asset.");
			return false;
		}

		const TSharedPtr<FJsonObject>* NodeObjectPtr = nullptr;
		if (!OpObject->TryGetObjectField(TEXT("node"), NodeObjectPtr) || !NodeObjectPtr || !NodeObjectPtr->IsValid())
		{
			OutMessage = TEXT("add_node requires a node object.");
			return false;
		}

		const TSharedPtr<FJsonObject> NodeObject = *NodeObjectPtr;
		FString TempId;
		if (!TryGetJsonString(OpObject, TEXT("temp_id"), TempId))
		{
			TryGetJsonString(NodeObject, TEXT("temp_id"), TempId);
		}
		if (TempId.IsEmpty())
		{
			OutMessage = TEXT("add_node requires temp_id.");
			return false;
		}

		FString ClassPath;
		if (!TryGetJsonString(NodeObject, TEXT("create_class"), ClassPath) && !TryGetJsonString(NodeObject, TEXT("class"), ClassPath) && !TryGetJsonString(NodeObject, TEXT("kind"), ClassPath))
		{
			OutMessage = TEXT("add_node requires node.create_class, node.class, or node.kind.");
			return false;
		}

		UClass* ExpressionClass = LoadClass<UMaterialExpression>(nullptr, *ClassPath);
		if (!ExpressionClass)
		{
			ExpressionClass = FindObject<UClass>(nullptr, *ClassPath);
		}
		if (!ExpressionClass || !ExpressionClass->IsChildOf(UMaterialExpression::StaticClass()))
		{
			OutMessage = FString::Printf(TEXT("'%s' is not a MaterialExpression class."), *ClassPath);
			return false;
		}

		UObject* Outer = bApply ? static_cast<UObject*>(Material) : GetTransientPackage();
		UMaterialExpression* NewExpression = NewObject<UMaterialExpression>(Outer, ExpressionClass, NAME_None, RF_Transactional);
		NewExpression->Modify();
		NewExpression->Material = Material;
		int32 X = 0;
		int32 Y = 0;
		if (TryGetPatchPosition(NodeObject, X, Y))
		{
			NewExpression->MaterialExpressionEditorX = X;
			NewExpression->MaterialExpressionEditorY = Y;
		}

		const TSharedPtr<FJsonObject>* PropertiesObject = nullptr;
		if (NodeObject->TryGetObjectField(TEXT("properties"), PropertiesObject) && PropertiesObject && PropertiesObject->IsValid())
		{
			if (!ApplyJsonProperties(NewExpression, *PropertiesObject, OutMessage))
			{
				return false;
			}
		}

		if (bApply)
		{
			Material->Modify();
			Material->GetExpressionCollection().AddExpression(NewExpression);
		}
		CreatedNodes.Add(TempId, NewExpression);
		OutMessage = bApply
			? FString::Printf(TEXT("Added node '%s'."), *TempId)
			: FString::Printf(TEXT("Will add node '%s' as %s."), *TempId, *ExpressionClass->GetName());
		return true;
	}

	bool PatchSetMaterialProperty(UMaterial* Material, const TSharedPtr<FJsonObject>& OpObject, const bool bApply, FString& OutMessage)
	{
		if (!Material)
		{
			OutMessage = TEXT("set_material_property requires a base Material asset.");
			return false;
		}

		const TSharedPtr<FJsonObject>* PropertiesObject = nullptr;
		if (!OpObject->TryGetObjectField(TEXT("properties"), PropertiesObject) || !PropertiesObject || !PropertiesObject->IsValid())
		{
			OutMessage = TEXT("set_material_property requires properties object.");
			return false;
		}

		if (!bApply)
		{
			OutMessage = TEXT("Will set material properties.");
			return true;
		}

		Material->Modify();
		return ApplyJsonProperties(Material, *PropertiesObject, OutMessage);
	}

	bool PatchSetNodeProperty(
		UMaterial* Material,
		const TSharedPtr<FJsonObject>& OpObject,
		const bool bApply,
		const TMap<FString, UMaterialExpression*>& ExistingNodes,
		const TMap<FString, UMaterialExpression*>& CreatedNodes,
		FString& OutMessage)
	{
		if (!Material)
		{
			OutMessage = TEXT("set_node_property requires a base Material asset.");
			return false;
		}

		FString NodeId;
		if (!TryGetJsonString(OpObject, TEXT("node"), NodeId))
		{
			OutMessage = TEXT("set_node_property requires node.");
			return false;
		}

		const TSharedPtr<FJsonObject>* PropertiesObject = nullptr;
		if (!OpObject->TryGetObjectField(TEXT("properties"), PropertiesObject) || !PropertiesObject || !PropertiesObject->IsValid())
		{
			OutMessage = TEXT("set_node_property requires properties object.");
			return false;
		}

		UMaterialExpression* Expression = ResolvePatchNode(NodeId, ExistingNodes, CreatedNodes);
		if (!Expression)
		{
			OutMessage = FString::Printf(TEXT("Unknown node '%s'."), *NodeId);
			return false;
		}

		if (!bApply)
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PropertiesObject)->Values)
			{
				FProperty* Property = Expression->GetClass()->FindPropertyByName(FName(*Pair.Key));
				if (!Property || !IsPatchVisibleProperty(Property))
				{
					OutMessage = FString::Printf(TEXT("Property '%s' is not exposed for node '%s'."), *Pair.Key, *NodeId);
					return false;
				}
			}

			OutMessage = FString::Printf(TEXT("Will set properties on node '%s'."), *NodeId);
			return true;
		}

		Expression->Modify();
		if (!ApplyJsonProperties(Expression, *PropertiesObject, OutMessage))
		{
			return false;
		}

		Expression->PostEditChange();
		OutMessage = FString::Printf(TEXT("Set properties on node '%s'."), *NodeId);
		return true;
	}

	bool PatchConnect(
		UMaterial* Material,
		const TSharedPtr<FJsonObject>& OpObject,
		const bool bApply,
		const TMap<FString, UMaterialExpression*>& ExistingNodes,
		const TMap<FString, UMaterialExpression*>& CreatedNodes,
		FString& OutMessage)
	{
		if (!Material)
		{
			OutMessage = TEXT("connect requires a base Material asset.");
			return false;
		}

		const TSharedPtr<FJsonObject>* FromObjectPtr = nullptr;
		const TSharedPtr<FJsonObject>* ToObjectPtr = nullptr;
		if (!OpObject->TryGetObjectField(TEXT("from"), FromObjectPtr) || !FromObjectPtr || !FromObjectPtr->IsValid() ||
			!OpObject->TryGetObjectField(TEXT("to"), ToObjectPtr) || !ToObjectPtr || !ToObjectPtr->IsValid())
		{
			OutMessage = TEXT("connect requires from and to endpoint objects.");
			return false;
		}

		FString FromNodeId;
		FString FromPin;
		FString ToNodeId;
		FString ToPin;
		if (!TryGetJsonString(*FromObjectPtr, TEXT("node"), FromNodeId) ||
			!TryGetJsonString(*FromObjectPtr, TEXT("pin"), FromPin) ||
			!TryGetJsonString(*ToObjectPtr, TEXT("node"), ToNodeId) ||
			!TryGetJsonString(*ToObjectPtr, TEXT("pin"), ToPin))
		{
			OutMessage = TEXT("connect endpoints require node and pin.");
			return false;
		}

		UMaterialExpression* FromExpression = ResolvePatchNode(FromNodeId, ExistingNodes, CreatedNodes);
		if (!FromExpression)
		{
			OutMessage = FString::Printf(TEXT("Unknown from node '%s'."), *FromNodeId);
			return false;
		}

		const int32 OutputIndex = ResolveMaterialOutputIndex(FromExpression, FromPin);
		if (OutputIndex == INDEX_NONE)
		{
			OutMessage = FString::Printf(TEXT("Unable to resolve output pin '%s.%s'."), *FromNodeId, *FromPin);
			return false;
		}

		FExpressionInput* TargetInput = nullptr;
		if (ToNodeId == TEXT("root"))
		{
			EMaterialProperty Property = MP_MAX;
			if (!MaterialPropertyFromString(ToPin, Property))
			{
				OutMessage = FString::Printf(TEXT("Unknown material output '%s'."), *ToPin);
				return false;
			}
			TargetInput = Material->GetExpressionInputForProperty(Property);
		}
		else
		{
			UMaterialExpression* ToExpression = ResolvePatchNode(ToNodeId, ExistingNodes, CreatedNodes);
			if (!ToExpression)
			{
				OutMessage = FString::Printf(TEXT("Unknown to node '%s'."), *ToNodeId);
				return false;
			}
			TargetInput = ResolveMaterialInput(ToExpression, ToPin);
		}

		if (!TargetInput)
		{
			OutMessage = FString::Printf(TEXT("Unable to resolve input pin '%s.%s'."), *ToNodeId, *ToPin);
			return false;
		}

		if (!bApply)
		{
			OutMessage = FString::Printf(TEXT("Will connect %s.%s -> %s.%s."), *FromNodeId, *FromPin, *ToNodeId, *ToPin);
			return true;
		}

		Material->Modify();
		FromExpression->Modify();
		if (ToNodeId != TEXT("root"))
		{
			if (UMaterialExpression* ToExpression = ResolvePatchNode(ToNodeId, ExistingNodes, CreatedNodes))
			{
				ToExpression->Modify();
			}
		}
		TargetInput->Expression = FromExpression;
		TargetInput->OutputIndex = OutputIndex;
		TargetInput->InputName = FName(*ToPin);
		OutMessage = FString::Printf(TEXT("Connected %s.%s -> %s.%s."), *FromNodeId, *FromPin, *ToNodeId, *ToPin);
		return true;
	}

	bool PatchDisconnect(
		UMaterial* Material,
		const TSharedPtr<FJsonObject>& OpObject,
		const bool bApply,
		const TMap<FString, UMaterialExpression*>& ExistingNodes,
		const TMap<FString, UMaterialExpression*>& CreatedNodes,
		FString& OutMessage)
	{
		if (!Material)
		{
			OutMessage = TEXT("disconnect requires a base Material asset.");
			return false;
		}

		const TSharedPtr<FJsonObject>* ToObjectPtr = nullptr;
		if (!OpObject->TryGetObjectField(TEXT("to"), ToObjectPtr) || !ToObjectPtr || !ToObjectPtr->IsValid())
		{
			OutMessage = TEXT("disconnect requires a to endpoint object.");
			return false;
		}

		FString ToNodeId;
		FString ToPin;
		if (!TryGetJsonString(*ToObjectPtr, TEXT("node"), ToNodeId) || !TryGetJsonString(*ToObjectPtr, TEXT("pin"), ToPin))
		{
			OutMessage = TEXT("disconnect endpoint requires node and pin.");
			return false;
		}

		FExpressionInput* TargetInput = nullptr;
		if (ToNodeId == TEXT("root"))
		{
			EMaterialProperty Property = MP_MAX;
			if (!MaterialPropertyFromString(ToPin, Property))
			{
				OutMessage = FString::Printf(TEXT("Unknown material output '%s'."), *ToPin);
				return false;
			}
			TargetInput = Material->GetExpressionInputForProperty(Property);
		}
		else if (UMaterialExpression* ToExpression = ResolvePatchNode(ToNodeId, ExistingNodes, CreatedNodes))
		{
			TargetInput = ResolveMaterialInput(ToExpression, ToPin);
		}

		if (!TargetInput)
		{
			OutMessage = FString::Printf(TEXT("Unable to resolve input pin '%s.%s'."), *ToNodeId, *ToPin);
			return false;
		}

		if (!bApply)
		{
			OutMessage = FString::Printf(TEXT("Will disconnect %s.%s."), *ToNodeId, *ToPin);
			return true;
		}

		Material->Modify();
		if (ToNodeId != TEXT("root"))
		{
			if (UMaterialExpression* ToExpression = ResolvePatchNode(ToNodeId, ExistingNodes, CreatedNodes))
			{
				ToExpression->Modify();
			}
		}
		TargetInput->Expression = nullptr;
		TargetInput->OutputIndex = 0;
		OutMessage = FString::Printf(TEXT("Disconnected %s.%s."), *ToNodeId, *ToPin);
		return true;
	}

	bool PatchMoveNode(
		UMaterial* Material,
		const TSharedPtr<FJsonObject>& OpObject,
		const bool bApply,
		const TMap<FString, UMaterialExpression*>& ExistingNodes,
		const TMap<FString, UMaterialExpression*>& CreatedNodes,
		FString& OutMessage)
	{
		if (!Material)
		{
			OutMessage = TEXT("move_node requires a base Material asset.");
			return false;
		}

		FString NodeId;
		if (!TryGetJsonString(OpObject, TEXT("node"), NodeId))
		{
			OutMessage = TEXT("move_node requires node.");
			return false;
		}

		int32 X = 0;
		int32 Y = 0;
		if (!TryGetPatchPosition(OpObject, X, Y))
		{
			OutMessage = TEXT("move_node requires position [x, y].");
			return false;
		}

		if (NodeId == CompactMaterialRootAlias || NodeId == MaterialRootNodeId)
		{
			if (!bApply)
			{
				OutMessage = FString::Printf(TEXT("Will move %s to [%d, %d]."), *NodeId, X, Y);
				return true;
			}

			Material->Modify();
			Material->EditorX = X;
			Material->EditorY = Y;
			OutMessage = FString::Printf(TEXT("Moved %s to [%d, %d]."), *NodeId, X, Y);
			return true;
		}

		UMaterialExpression* Expression = ResolvePatchNode(NodeId, ExistingNodes, CreatedNodes);
		if (!Expression)
		{
			OutMessage = FString::Printf(TEXT("Unknown node '%s'."), *NodeId);
			return false;
		}

		if (!bApply)
		{
			OutMessage = FString::Printf(TEXT("Will move %s to [%d, %d]."), *NodeId, X, Y);
			return true;
		}

		Expression->Modify();
		Expression->MaterialExpressionEditorX = X;
		Expression->MaterialExpressionEditorY = Y;
		OutMessage = FString::Printf(TEXT("Moved %s to [%d, %d]."), *NodeId, X, Y);
		return true;
	}

	bool ProcessMaterialPatch(const FString& PatchJson, const bool bApply, FString& OutJson, FString& OutError)
	{
		TSharedPtr<FJsonObject> Patch;
		if (!ParsePatchJson(PatchJson, Patch, OutError))
		{
			return false;
		}

		FString AssetPath;
		Patch->TryGetStringField(TEXT("asset_path"), AssetPath);
		UObject* AssetObject = LoadPatchAsset(AssetPath, OutError);
		if (!AssetObject)
		{
			return false;
		}

		UMaterial* Material = Cast<UMaterial>(AssetObject);
		UMaterialInstanceConstant* MaterialInstance = Cast<UMaterialInstanceConstant>(AssetObject);
		if (!Material && !MaterialInstance)
		{
			OutError = TEXT("Material patches currently support base Material and MaterialInstanceConstant assets only.");
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
		if (!Patch->TryGetArrayField(TEXT("ops"), Ops) || !Ops)
		{
			OutError = TEXT("Material patch requires ops array.");
			return false;
		}

		TMap<FString, UMaterialExpression*> ExistingNodes;
		if (Material && !BuildExistingNodeMapForPatch(AssetPath, ExistingNodes, OutError))
		{
			return false;
		}

		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("asset_path"), AssetPath);
		Root->SetStringField(TEXT("mode"), bApply ? TEXT("apply") : TEXT("validate"));
		Root->SetStringField(TEXT("supported_ops"), TEXT("set_parameter, add_node, connect, disconnect, move_node, set_node_property, set_material_property"));

		TArray<TSharedPtr<FJsonValue>> ResultArray;
		TArray<TSharedPtr<FJsonValue>> Messages;
		TMap<FString, UMaterialExpression*> CreatedNodes;
		bool bAllOk = true;

		TUniquePtr<FScopedTransaction> Transaction;
		if (bApply)
		{
			Transaction = MakeUnique<FScopedTransaction>(FText::FromString(TEXT("ToolPlayMCP Apply Material Patch")));
			AssetObject->Modify();
		}

		for (int32 OpIndex = 0; OpIndex < Ops->Num(); ++OpIndex)
		{
			const TSharedPtr<FJsonObject> OpObject = (*Ops)[OpIndex]->AsObject();
			if (!OpObject.IsValid())
			{
				bAllOk = false;
				ResultArray.Add(MakeShared<FJsonValueObject>(MakePatchOpResult(OpIndex, TEXT("invalid"), false, TEXT("Operation must be an object."))));
				continue;
			}

			FString Op;
			if (!TryGetJsonString(OpObject, TEXT("op"), Op))
			{
				bAllOk = false;
				ResultArray.Add(MakeShared<FJsonValueObject>(MakePatchOpResult(OpIndex, TEXT("missing"), false, TEXT("Operation missing op field."))));
				continue;
			}

			FString Message;
			bool bOpOk = false;
			if (Op == TEXT("set_parameter"))
			{
				bOpOk = PatchSetParameter(AssetObject, OpObject, bApply, Message);
			}
			else if (Op == TEXT("add_node"))
			{
				bOpOk = PatchAddNode(Material, OpObject, bApply, CreatedNodes, Message);
			}
			else if (Op == TEXT("set_material_property"))
			{
				bOpOk = PatchSetMaterialProperty(Material, OpObject, bApply, Message);
			}
			else if (Op == TEXT("connect"))
			{
				bOpOk = PatchConnect(Material, OpObject, bApply, ExistingNodes, CreatedNodes, Message);
			}
			else if (Op == TEXT("disconnect"))
			{
				bOpOk = PatchDisconnect(Material, OpObject, bApply, ExistingNodes, CreatedNodes, Message);
			}
			else if (Op == TEXT("move_node"))
			{
				bOpOk = PatchMoveNode(Material, OpObject, bApply, ExistingNodes, CreatedNodes, Message);
			}
			else if (Op == TEXT("set_node_property"))
			{
				bOpOk = PatchSetNodeProperty(Material, OpObject, bApply, ExistingNodes, CreatedNodes, Message);
			}
			else
			{
				Message = FString::Printf(TEXT("Unsupported material patch op '%s'."), *Op);
			}

			bAllOk = bAllOk && bOpOk;
			ResultArray.Add(MakeShared<FJsonValueObject>(MakePatchOpResult(OpIndex, Op, bOpOk, Message)));
			if (!bOpOk)
			{
				AddPatchMessage(Messages, TEXT("error"), OpIndex, Message);
				if (bApply)
				{
					break;
				}
			}
		}

		if (bApply && bAllOk)
		{
			if (Material)
			{
				Material->PreEditChange(nullptr);
				Material->PostEditChange();
			}
			else if (MaterialInstance)
			{
				MaterialInstance->PreEditChange(nullptr);
				MaterialInstance->PostEditChange();
			}
			AssetObject->MarkPackageDirty();

			FString SavedFilename;
			FString SaveError;
			if (SaveAssetObject(AssetObject, SavedFilename, SaveError))
			{
				Root->SetBoolField(TEXT("saved"), true);
				Root->SetStringField(TEXT("filename"), SavedFilename);
			}
			else
			{
				bAllOk = false;
				Root->SetBoolField(TEXT("saved"), false);
				AddPatchMessage(Messages, TEXT("error"), INDEX_NONE, SaveError);
			}
		}
		else if (bApply && !bAllOk && Transaction.IsValid())
		{
			Transaction->Cancel();
		}

		Root->SetBoolField(TEXT("ok"), bAllOk);
		Root->SetArrayField(TEXT("ops"), MoveTemp(ResultArray));
		Root->SetArrayField(TEXT("messages"), MoveTemp(Messages));
		OutJson = JsonObjectToString(Root);
		return true;
	}
}

bool FToolPlayMCPMaterialService::ExportSelectedAsset(FString& OutJson, FString& OutSavedPath, FString& OutError)
{
	FAssetData AssetData;
	if (!GetSingleSelectedAsset(AssetData, OutError))
	{
		return false;
	}

	FToolPlayMCPGraphExportDocument Document;
	if (!ExportAsset(AssetData, Document, OutError))
	{
		return false;
	}

	OutJson = SerializeDocument(Document);
	OutSavedPath = BuildOutputPath(AssetData);

	const FString Directory = FPaths::GetPath(OutSavedPath);
	IFileManager::Get().MakeDirectory(*Directory, true);

	if (!FFileHelper::SaveStringToFile(OutJson, *OutSavedPath))
	{
		OutError = FString::Printf(TEXT("Failed to save export JSON to '%s'."), *OutSavedPath);
		return false;
	}

	return true;
}

bool FToolPlayMCPMaterialService::ExportSelectedMaterialCompact(FString& OutJson, FString& OutSessionId, FString& OutSavedPath, FString& OutError)
{
	FAssetData AssetData;
	if (!GetSingleSelectedAsset(AssetData, OutError))
	{
		return false;
	}

	UObject* AssetObject = AssetData.GetAsset();
	if (UMaterial* Material = Cast<UMaterial>(AssetObject))
	{
		FToolPlayMCPCompactMaterialGraph CompactGraph;
		if (!ExportCompactMaterial(Material, AssetData, CompactGraph, OutSessionId, OutError))
		{
			return false;
		}

		OutJson = SerializeCompactMaterialGraph(CompactGraph);
		OutSavedPath = BuildCompactOutputPath(AssetData);
	}
	else if (UMaterialInstance* MaterialInstance = Cast<UMaterialInstance>(AssetObject))
	{
		FToolPlayMCPCompactMaterialGraph CompactGraph;
		if (!ExportCompactMaterialInstance(MaterialInstance, AssetData, CompactGraph, OutSessionId, OutError))
		{
			return false;
		}

		OutJson = SerializeCompactMaterialGraph(CompactGraph);
		OutSavedPath = BuildCompactOutputPath(AssetData);
	}
	else
	{
		OutError = TEXT("Compact export currently supports Material and Material Instance assets only.");
		return false;
	}

	const FString Directory = FPaths::GetPath(OutSavedPath);
	IFileManager::Get().MakeDirectory(*Directory, true);

	if (!FFileHelper::SaveStringToFile(OutJson, *OutSavedPath))
	{
		OutError = FString::Printf(TEXT("Failed to save compact material JSON to '%s'."), *OutSavedPath);
		return false;
	}

	return true;
}

bool FToolPlayMCPMaterialService::ExportMaterialCompactByPath(const FString& AssetPath, FString& OutJson, FString& OutSessionId, FString& OutSavedPath, FString& OutError)
{
	FToolPlayMCPCompactMaterialGraph CompactGraph;
	if (!BuildCompactMaterialGraphByPath(AssetPath, CompactGraph, OutSessionId, OutError))
	{
		return false;
	}

	OutJson = SerializeCompactMaterialGraph(CompactGraph);
	FAssetData AssetData(FSoftObjectPath(AssetPath).TryLoad());
	OutSavedPath = BuildCompactOutputPath(AssetData);

	const FString Directory = FPaths::GetPath(OutSavedPath);
	IFileManager::Get().MakeDirectory(*Directory, true);

	if (!FFileHelper::SaveStringToFile(OutJson, *OutSavedPath))
	{
		OutError = FString::Printf(TEXT("Failed to save compact material JSON to '%s'."), *OutSavedPath);
		return false;
	}

	return true;
}

bool FToolPlayMCPMaterialService::CreateMaterialAsset(const FString& PackagePath, const FString& AssetName, FString& OutJson, FString& OutError)
{
	if (PackagePath.IsEmpty() || AssetName.IsEmpty())
	{
		OutError = TEXT("create_material_asset requires package_path and asset_name.");
		return false;
	}

	FString CleanPackagePath = PackagePath;
	CleanPackagePath.RemoveFromEnd(TEXT("/"));
	if (!CleanPackagePath.StartsWith(TEXT("/Game")))
	{
		OutError = TEXT("create_material_asset package_path must start with /Game.");
		return false;
	}

	const FString PackageName = CleanPackagePath / AssetName;
	const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
	if (LoadObject<UMaterial>(nullptr, *ObjectPath))
	{
		OutError = FString::Printf(TEXT("Material asset '%s' already exists or is already loaded."), *ObjectPath);
		return false;
	}

	const FScopedTransaction Transaction(FText::FromString(TEXT("ToolPlayMCP: Create Material Asset")));

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("Failed to create package '%s'."), *PackageName);
		return false;
	}
	Package->Modify();

	UMaterial* Material = NewObject<UMaterial>(Package, UMaterial::StaticClass(), FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	if (!Material)
	{
		OutError = FString::Printf(TEXT("Failed to create material '%s'."), *AssetName);
		return false;
	}

	Material->Modify();
	Material->MaterialDomain = MD_Surface;
	Material->BlendMode = BLEND_Translucent;
	Material->TwoSided = true;
	Material->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(Material);

	FString SavedFilename;
	if (!SaveAssetObject(Material, SavedFilename, OutError))
	{
		return false;
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset"), Material->GetName());
	Root->SetStringField(TEXT("asset_path"), ObjectPath);
	Root->SetStringField(TEXT("package"), PackageName);
	Root->SetStringField(TEXT("class"), Material->GetClass()->GetName());
	Root->SetBoolField(TEXT("created"), true);
	Root->SetBoolField(TEXT("saved"), true);
	Root->SetStringField(TEXT("filename"), SavedFilename);
	OutJson = JsonObjectToString(Root);
	return true;
}

bool FToolPlayMCPMaterialService::SaveAssetByPath(const FString& AssetPath, FString& OutJson, FString& OutError)
{
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("save_asset requires asset_path.");
		return false;
	}

	UObject* AssetObject = FSoftObjectPath(AssetPath).TryLoad();
	if (!AssetObject)
	{
		OutError = FString::Printf(TEXT("Unable to load asset '%s'."), *AssetPath);
		return false;
	}

	FString SavedFilename;
	if (!SaveAssetObject(AssetObject, SavedFilename, OutError))
	{
		return false;
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetObject->GetPathName());
	Root->SetStringField(TEXT("class"), AssetObject->GetClass()->GetName());
	Root->SetBoolField(TEXT("saved"), true);
	Root->SetStringField(TEXT("filename"), SavedFilename);
	OutJson = JsonObjectToString(Root);
	return true;
}

bool FToolPlayMCPMaterialService::ListMaterialFunctionsByPath(const FString& AssetPath, FString& OutJson, FString& OutError)
{
	FString SessionId;
	FToolPlayMCPCompactMaterialGraph Graph;
	if (!BuildCompactMaterialGraphByPath(AssetPath, Graph, SessionId, OutError))
	{
		return false;
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset"), Graph.Asset);

	TArray<TSharedPtr<FJsonValue>> FunctionsArray;
	for (const TPair<FString, FToolPlayMCPCompactMaterialNode>& Pair : Graph.Nodes)
	{
		if (Pair.Value.K != TEXT("func"))
		{
			continue;
		}

		TSet<FString> Inputs;
		TSet<FString> Outputs;
		for (const TArray<FString>& Edge : Graph.Edges)
		{
			if (Edge.Num() != 4)
			{
				continue;
			}
			if (Edge[2] == Pair.Key)
			{
				Inputs.Add(Edge[3]);
			}
			if (Edge[0] == Pair.Key)
			{
				Outputs.Add(Edge[1]);
			}
		}

		TSharedRef<FJsonObject> FunctionObject = MakeShared<FJsonObject>();
		FunctionObject->SetStringField(TEXT("node"), Pair.Key);
		FunctionObject->SetStringField(TEXT("name"), Pair.Value.Label);

		TArray<TSharedPtr<FJsonValue>> InputArray;
		for (const FString& Input : Inputs)
		{
			InputArray.Add(MakeShared<FJsonValueString>(Input));
		}
		FunctionObject->SetArrayField(TEXT("inputs"), MoveTemp(InputArray));

		TArray<TSharedPtr<FJsonValue>> OutputArray;
		for (const FString& Output : Outputs)
		{
			OutputArray.Add(MakeShared<FJsonValueString>(Output));
		}
		FunctionObject->SetArrayField(TEXT("outputs"), MoveTemp(OutputArray));

		FunctionsArray.Add(MakeShared<FJsonValueObject>(FunctionObject));
	}

	Root->SetArrayField(TEXT("functions"), MoveTemp(FunctionsArray));
	OutJson = JsonObjectToString(Root);
	return true;
}

bool FToolPlayMCPMaterialService::DescribeMaterialFunctionInterfaceByPath(const FString& FunctionPath, FString& OutJson, FString& OutError)
{
	if (FunctionPath.IsEmpty())
	{
		OutError = TEXT("Function path is empty.");
		return false;
	}

	UObject* AssetObject = FSoftObjectPath(FunctionPath).TryLoad();
	UMaterialFunctionInterface* Function = Cast<UMaterialFunctionInterface>(AssetObject);
	if (!Function)
	{
		OutError = FString::Printf(TEXT("Asset '%s' is not a Material Function asset."), *FunctionPath);
		return false;
	}

#if WITH_EDITOR
	Function->UpdateInputOutputTypes();
#endif

	TArray<FFunctionExpressionInput> Inputs;
	TArray<FFunctionExpressionOutput> Outputs;
	Function->GetInputsAndOutputs(Inputs, Outputs);

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset"), Function->GetName());
	Root->SetStringField(TEXT("path"), Function->GetPathName());
	Root->SetStringField(TEXT("class"), Function->GetClass()->GetName());
	Root->SetStringField(TEXT("usage"), MaterialFunctionUsageToString(Function->GetMaterialFunctionUsage()));

	TArray<TSharedPtr<FJsonValue>> InputArray;
	for (const FFunctionExpressionInput& Input : Inputs)
	{
		TSharedRef<FJsonObject> InputObject = MakeShared<FJsonObject>();
		if (const UMaterialExpressionFunctionInput* ExpressionInput = Input.ExpressionInput)
		{
			InputObject->SetStringField(TEXT("name"), ExpressionInput->InputName.ToString());
			InputObject->SetStringField(TEXT("type"), UMaterialExpressionFunctionInput::GetInputTypeDisplayName(ExpressionInput->InputType));
			InputObject->SetStringField(TEXT("compact"), NormalizeMaterialPinName(ExpressionInput->InputName.ToString()));
			InputObject->SetNumberField(TEXT("sort_priority"), ExpressionInput->SortPriority);
			InputObject->SetBoolField(TEXT("uses_preview_as_default"), ExpressionInput->bUsePreviewValueAsDefault != 0);
			InputObject->SetStringField(TEXT("preview_value"), FormatCompactVector4f(ExpressionInput->PreviewValue));
			if (!ExpressionInput->Description.IsEmpty())
			{
				InputObject->SetStringField(TEXT("description"), ExpressionInput->Description);
			}
		}
		else
		{
			InputObject->SetStringField(TEXT("name"), TEXT("unknown"));
			InputObject->SetStringField(TEXT("type"), TEXT("unknown"));
		}

		InputArray.Add(MakeShared<FJsonValueObject>(InputObject));
	}
	Root->SetArrayField(TEXT("inputs"), MoveTemp(InputArray));

	TArray<TSharedPtr<FJsonValue>> OutputArray;
	for (const FFunctionExpressionOutput& Output : Outputs)
	{
		TSharedRef<FJsonObject> OutputObject = MakeShared<FJsonObject>();
		if (const UMaterialExpressionFunctionOutput* ExpressionOutput = Output.ExpressionOutput)
		{
			OutputObject->SetStringField(TEXT("name"), ExpressionOutput->OutputName.ToString());
			OutputObject->SetStringField(TEXT("compact"), NormalizeMaterialPinName(ExpressionOutput->OutputName.ToString()));
			OutputObject->SetNumberField(TEXT("sort_priority"), ExpressionOutput->SortPriority);
			if (!ExpressionOutput->Description.IsEmpty())
			{
				OutputObject->SetStringField(TEXT("description"), ExpressionOutput->Description);
			}
		}
		else
		{
			OutputObject->SetStringField(TEXT("name"), TEXT("unknown"));
		}

		OutputArray.Add(MakeShared<FJsonValueObject>(OutputObject));
	}
	Root->SetArrayField(TEXT("outputs"), MoveTemp(OutputArray));

	TArray<UMaterialFunctionInterface*> DependentFunctions;
	Function->GetDependentFunctions(DependentFunctions);
	TArray<TSharedPtr<FJsonValue>> DependencyArray;
	for (const UMaterialFunctionInterface* Dependency : DependentFunctions)
	{
		if (Dependency)
		{
			DependencyArray.Add(MakeShared<FJsonValueString>(Dependency->GetPathName()));
		}
	}
	Root->SetArrayField(TEXT("dependencies"), MoveTemp(DependencyArray));

	OutJson = JsonObjectToString(Root);
	return true;
}

bool FToolPlayMCPMaterialService::GetMaterialNodeConfigByAlias(const FString& AssetPath, const FString& NodeAlias, FString& OutJson, FString& OutError)
{
	if (NodeAlias.IsEmpty() || NodeAlias == TEXT("root"))
	{
		OutError = TEXT("A non-root compact node alias is required.");
		return false;
	}

	TMap<FString, UMaterialExpression*> ExistingNodes;
	if (!BuildExistingNodeMapForPatch(AssetPath, ExistingNodes, OutError))
	{
		return false;
	}

	UMaterialExpression** ExpressionPtr = ExistingNodes.Find(NodeAlias);
	if (!ExpressionPtr || !*ExpressionPtr)
	{
		OutError = FString::Printf(TEXT("Node alias '%s' was not found for '%s'."), *NodeAlias, *AssetPath);
		return false;
	}

	UMaterialExpression* Expression = *ExpressionPtr;
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("node"), NodeAlias);
	Root->SetStringField(TEXT("class"), Expression->GetClass()->GetName());
	Root->SetStringField(TEXT("kind"), NormalizeMaterialExpressionKind(Expression));
	Root->SetStringField(TEXT("label"), ExtractMaterialNodeLabel(Expression));
	Root->SetNumberField(TEXT("x"), Expression->MaterialExpressionEditorX);
	Root->SetNumberField(TEXT("y"), Expression->MaterialExpressionEditorY);

	TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
	AddConfigPropertiesJson(Expression, Properties);
	Root->SetObjectField(TEXT("properties"), Properties);

	OutJson = JsonObjectToString(Root);
	return true;
}

bool FToolPlayMCPMaterialService::GetMaterialNodeConfigSchema(const FString& NodeKindOrClassPath, FString& OutJson, FString& OutError)
{
	if (NodeKindOrClassPath.IsEmpty())
	{
		OutError = TEXT("Node kind or class path is required.");
		return false;
	}

	UClass* NodeClass = LoadClass<UMaterialExpression>(nullptr, *NodeKindOrClassPath);
	if (!NodeClass)
	{
		NodeClass = FindObject<UClass>(nullptr, *NodeKindOrClassPath);
	}
	if (!NodeClass && !NodeKindOrClassPath.StartsWith(TEXT("/Script/")))
	{
		NodeClass = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *NodeKindOrClassPath));
	}
	if (!NodeClass || !NodeClass->IsChildOf(UMaterialExpression::StaticClass()))
	{
		OutError = FString::Printf(TEXT("'%s' is not a MaterialExpression class."), *NodeKindOrClassPath);
		return false;
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("class"), NodeClass->GetName());
	Root->SetStringField(TEXT("class_path"), NodeClass->GetPathName());

	TArray<TSharedPtr<FJsonValue>> Properties;
	AddConfigSchemaJson(NodeClass, Properties);
	Root->SetArrayField(TEXT("properties"), MoveTemp(Properties));

	OutJson = JsonObjectToString(Root);
	return true;
}

bool FToolPlayMCPMaterialService::TraceMaterialParameterByPath(const FString& AssetPath, const FString& ParameterName, FString& OutJson, FString& OutError)
{
	FString SessionId;
	FToolPlayMCPCompactMaterialGraph Graph;
	if (!BuildCompactMaterialGraphByPath(AssetPath, Graph, SessionId, OutError))
	{
		return false;
	}

	TArray<FString> StartNodes;
	for (const TPair<FString, FToolPlayMCPCompactMaterialNode>& Pair : Graph.Nodes)
	{
		if (Pair.Value.Label == ParameterName)
		{
			StartNodes.Add(Pair.Key);
		}
	}

	if (StartNodes.IsEmpty())
	{
		OutError = FString::Printf(TEXT("Parameter '%s' was not found in material '%s'."), *ParameterName, *AssetPath);
		return false;
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset"), Graph.Asset);
	Root->SetStringField(TEXT("parameter"), ParameterName);
	Root->SetStringField(TEXT("direction"), TEXT("downstream"));

	TSharedRef<FJsonObject> NodesObject = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> EdgesArray;
	TArray<TSharedPtr<FJsonValue>> ChainsArray;
	TSet<FString> SeenEdgeKeys;

	for (const FString& StartNode : StartNodes)
	{
		AddCompactNodeJson(Graph, StartNode, NodesObject);

		struct FTraceState
		{
			FString Node;
			TArray<FString> Chain;
		};

		TArray<FTraceState> Stack;
		FTraceState InitialState;
		InitialState.Node = StartNode;
		InitialState.Chain.Add(StartNode);
		Stack.Add(MoveTemp(InitialState));
		TSet<FString> VisitedNodes;

		while (!Stack.IsEmpty())
		{
			FTraceState State = Stack.Pop(EAllowShrinking::No);
			if (State.Chain.Num() > 48)
			{
				continue;
			}

			for (const TArray<FString>& Edge : Graph.Edges)
			{
				if (Edge.Num() != 4 || Edge[0] != State.Node)
				{
					continue;
				}

				const FString EdgeKey = BuildCompactEdgeKey(Edge);
				if (!SeenEdgeKeys.Contains(EdgeKey))
				{
					SeenEdgeKeys.Add(EdgeKey);
					AddEdgeJson(EdgesArray, Edge);
				}

				AddCompactNodeJson(Graph, Edge[0], NodesObject);
				AddCompactNodeJson(Graph, Edge[2], NodesObject);

				TArray<FString> NextChain = State.Chain;
				NextChain.Add(FormatEndpoint(Edge[0], Edge[1]));
				const FToolPlayMCPCompactMaterialNode* TargetNode = Graph.Nodes.Find(Edge[2]);
				if (!TargetNode || !IsTransparentMaterialNodeKind(TargetNode->K))
				{
					NextChain.Add(FormatEndpoint(Edge[2], Edge[3]));
				}

				if (Edge[2] == CompactMaterialRootAlias)
				{
					AddChainJson(ChainsArray, NextChain);
					continue;
				}

				if (!VisitedNodes.Contains(Edge[2]))
				{
					VisitedNodes.Add(Edge[2]);
					FTraceState NextState;
					NextState.Node = Edge[2];
					NextState.Chain = MoveTemp(NextChain);
					Stack.Add(MoveTemp(NextState));
				}
			}
		}
	}

	Root->SetObjectField(TEXT("nodes"), NodesObject);
	Root->SetArrayField(TEXT("edges"), MoveTemp(EdgesArray));
	Root->SetArrayField(TEXT("chains"), MoveTemp(ChainsArray));
	OutJson = JsonObjectToString(Root);
	return true;
}

bool FToolPlayMCPMaterialService::TraceMaterialOutputByPath(const FString& AssetPath, const FString& OutputName, FString& OutJson, FString& OutError)
{
	FString SessionId;
	FToolPlayMCPCompactMaterialGraph Graph;
	if (!BuildCompactMaterialGraphByPath(AssetPath, Graph, SessionId, OutError))
	{
		return false;
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset"), Graph.Asset);
	Root->SetStringField(TEXT("output"), OutputName);
	Root->SetStringField(TEXT("direction"), TEXT("upstream"));

	TSharedRef<FJsonObject> NodesObject = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> EdgesArray;
	TArray<TSharedPtr<FJsonValue>> ChainsArray;
	TSet<FString> SeenEdgeKeys;
	bool bFoundOutput = false;

	struct FTraceState
	{
		FString Node;
		TArray<FString> Chain;
	};

	TArray<FTraceState> Stack;
	for (const TArray<FString>& Edge : Graph.Edges)
	{
		if (Edge.Num() == 4 && Edge[2] == CompactMaterialRootAlias && Edge[3] == OutputName)
		{
			bFoundOutput = true;
			FTraceState InitialState;
			InitialState.Node = Edge[0];
			InitialState.Chain.Add(FormatEndpoint(CompactMaterialRootAlias, OutputName));
			InitialState.Chain.Add(FormatEndpoint(Edge[0], Edge[1]));
			Stack.Add(MoveTemp(InitialState));
			AddEdgeJson(EdgesArray, Edge);
			AddCompactNodeJson(Graph, Edge[0], NodesObject);
			AddCompactNodeJson(Graph, CompactMaterialRootAlias, NodesObject);
		}
	}

	if (!bFoundOutput)
	{
		OutError = FString::Printf(TEXT("Material output '%s' is not connected in '%s'."), *OutputName, *AssetPath);
		return false;
	}

	TSet<FString> VisitedNodes;
	while (!Stack.IsEmpty())
	{
		FTraceState State = Stack.Pop(EAllowShrinking::No);
		if (State.Chain.Num() > 48)
		{
			continue;
		}

		bool bHasUpstream = false;
		for (const TArray<FString>& Edge : Graph.Edges)
		{
			if (Edge.Num() != 4 || Edge[2] != State.Node)
			{
				continue;
			}

			bHasUpstream = true;
			const FString EdgeKey = BuildCompactEdgeKey(Edge);
			if (!SeenEdgeKeys.Contains(EdgeKey))
			{
				SeenEdgeKeys.Add(EdgeKey);
				AddEdgeJson(EdgesArray, Edge);
			}

			AddCompactNodeJson(Graph, Edge[0], NodesObject);
			AddCompactNodeJson(Graph, Edge[2], NodesObject);

			TArray<FString> NextChain = State.Chain;
			const FToolPlayMCPCompactMaterialNode* SourceNode = Graph.Nodes.Find(Edge[0]);
			if (!SourceNode || !IsTransparentMaterialNodeKind(SourceNode->K))
			{
				NextChain.Add(FormatEndpoint(Edge[0], Edge[1]));
			}

			if (!VisitedNodes.Contains(Edge[0]))
			{
				VisitedNodes.Add(Edge[0]);
				FTraceState NextState;
				NextState.Node = Edge[0];
				NextState.Chain = MoveTemp(NextChain);
				Stack.Add(MoveTemp(NextState));
			}
		}

		if (!bHasUpstream)
		{
			AddChainJson(ChainsArray, State.Chain);
		}
	}

	Root->SetObjectField(TEXT("nodes"), NodesObject);
	Root->SetArrayField(TEXT("edges"), MoveTemp(EdgesArray));
	Root->SetArrayField(TEXT("chains"), MoveTemp(ChainsArray));
	OutJson = JsonObjectToString(Root);
	return true;
}

bool FToolPlayMCPMaterialService::ValidateMaterialPatch(const FString& PatchJson, FString& OutJson, FString& OutError)
{
	return ProcessMaterialPatch(PatchJson, false, OutJson, OutError);
}

bool FToolPlayMCPMaterialService::ApplyMaterialPatch(const FString& PatchJson, FString& OutJson, FString& OutError)
{
	return ProcessMaterialPatch(PatchJson, true, OutJson, OutError);
}

bool FToolPlayMCPMaterialService::BuildCompactMaterialGraphByPath(const FString& AssetPath, FToolPlayMCPCompactMaterialGraph& OutGraph, FString& OutSessionId, FString& OutError)
{
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Asset path is empty.");
		return false;
	}

	const FSoftObjectPath SoftObjectPath(AssetPath);
	UObject* AssetObject = SoftObjectPath.TryLoad();
	if (!AssetObject)
	{
		OutError = FString::Printf(TEXT("Unable to load material asset '%s'."), *AssetPath);
		return false;
	}

	if (UMaterial* Material = Cast<UMaterial>(AssetObject))
	{
		return ExportCompactMaterial(Material, FAssetData(Material), OutGraph, OutSessionId, OutError);
	}
	else if (UMaterialInstance* MaterialInstance = Cast<UMaterialInstance>(AssetObject))
	{
		return ExportCompactMaterialInstance(MaterialInstance, FAssetData(MaterialInstance), OutGraph, OutSessionId, OutError);
	}

	OutError = FString::Printf(TEXT("Asset '%s' is not a Material or Material Instance."), *AssetPath);
	return false;
}

bool FToolPlayMCPMaterialService::GetSingleSelectedAsset(FAssetData& OutAssetData, FString& OutError)
{
	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	TArray<FAssetData> SelectedAssets;
	ContentBrowserModule.Get().GetSelectedAssets(SelectedAssets);

	if (SelectedAssets.Num() == 0)
	{
		OutError = TEXT("No asset selected. Select one Blueprint or Material in the Content Browser.");
		return false;
	}

	if (SelectedAssets.Num() > 1)
	{
		OutError = TEXT("Multiple assets selected. Please select exactly one Blueprint or Material asset.");
		return false;
	}

	OutAssetData = SelectedAssets[0];
	return true;
}

bool FToolPlayMCPMaterialService::ExportAsset(const FAssetData& AssetData, FToolPlayMCPGraphExportDocument& OutDocument, FString& OutError)
{
	UObject* AssetObject = AssetData.GetAsset();
	if (!AssetObject)
	{
		OutError = FString::Printf(TEXT("Unable to load selected asset '%s'."), *AssetData.AssetName.ToString());
		return false;
	}

	if (UBlueprint* Blueprint = Cast<UBlueprint>(AssetObject))
	{
		return ExportBlueprint(Blueprint, AssetData, OutDocument, OutError);
	}

	if (UMaterial* Material = Cast<UMaterial>(AssetObject))
	{
		return ExportMaterial(Material, AssetData, OutDocument, OutError);
	}

	OutError = FString::Printf(
		TEXT("Unsupported asset class '%s'. Select a Blueprint or a base Material asset."),
		*AssetObject->GetClass()->GetName());
	return false;
}

bool FToolPlayMCPMaterialService::ExportBlueprint(UBlueprint* Blueprint, const FAssetData& AssetData, FToolPlayMCPGraphExportDocument& OutDocument, FString& OutError)
{
	if (!Blueprint)
	{
		OutError = TEXT("Blueprint asset was null.");
		return false;
	}

	OutDocument.AssetName = AssetData.AssetName.ToString();
	OutDocument.AssetPath = AssetData.GetSoftObjectPath().ToString();
	OutDocument.AssetClass = Blueprint->GetClass()->GetName();

	TArray<UEdGraph*> AllGraphs;
	AllGraphs.Append(Blueprint->UbergraphPages);
	AllGraphs.Append(Blueprint->FunctionGraphs);
	AllGraphs.Append(Blueprint->MacroGraphs);
	AllGraphs.Append(Blueprint->DelegateSignatureGraphs);

	for (const UEdGraph* Graph : AllGraphs)
	{
		if (!Graph)
		{
			continue;
		}

		FToolPlayMCPGraphExportGraph ExportGraph;
		ExportGraph.Name = Graph->GetName();
		ExportGraph.GraphClass = Graph->GetClass()->GetName();
		TSet<FString> SeenLinks;

		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			FToolPlayMCPGraphExportNode ExportNode;
			ExportNode.Id = BuildNodeId(Node);
			ExportNode.Name = Node->GetName();
			ExportNode.Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
			ExportNode.NodeClass = Node->GetClass()->GetName();
			ExportNode.PositionX = Node->NodePosX;
			ExportNode.PositionY = Node->NodePosY;
			ExportNode.Metadata.Add(TEXT("comment"), Node->NodeComment);

			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin)
				{
					continue;
				}

				FToolPlayMCPGraphExportPin ExportPin;
				ExportPin.Name = Pin->PinName.ToString();
				ExportPin.Direction = PinDirectionToString(Pin->Direction);
				ExportPin.Type = ExtractPinType(Pin);
				ExportPin.DefaultValue = Pin->DefaultValue;

				for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (!LinkedPin || !LinkedPin->GetOwningNodeUnchecked())
					{
						continue;
					}

					ExportPin.LinkedTo.Add(FString::Printf(
						TEXT("%s:%s"),
						*BuildNodeId(LinkedPin->GetOwningNodeUnchecked()),
						*LinkedPin->PinName.ToString()));
				}

				ExportNode.Pins.Add(MoveTemp(ExportPin));

				if (Pin->Direction != EGPD_Output)
				{
					continue;
				}

				for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (!LinkedPin || !LinkedPin->GetOwningNodeUnchecked())
					{
						continue;
					}

					AddUniqueGraphLink(
						ExportGraph,
						SeenLinks,
						BuildNodeId(Node),
						Pin->PinName.ToString(),
						BuildNodeId(LinkedPin->GetOwningNodeUnchecked()),
						LinkedPin->PinName.ToString());
				}
			}

			ExportGraph.Nodes.Add(MoveTemp(ExportNode));
		}

		OutDocument.Graphs.Add(MoveTemp(ExportGraph));
	}

	OutDocument.Messages.Add(TEXT("Blueprint export currently focuses on graph/node/pin topology for MCP-friendly inspection."));
	return true;
}

bool FToolPlayMCPMaterialService::ExportMaterial(UMaterial* Material, const FAssetData& AssetData, FToolPlayMCPGraphExportDocument& OutDocument, FString& OutError)
{
	if (!Material)
	{
		OutError = TEXT("Material asset was null.");
		return false;
	}

	OutDocument.AssetName = AssetData.AssetName.ToString();
	OutDocument.AssetPath = AssetData.GetSoftObjectPath().ToString();
	OutDocument.AssetClass = Material->GetClass()->GetName();

	FToolPlayMCPGraphExportGraph Graph;
	Graph.Name = TEXT("MaterialGraph");
	Graph.GraphClass = TEXT("Material");
	TSet<FString> SeenLinks;

	for (const UMaterialExpression* Expression : Material->GetExpressions())
	{
		if (!Expression)
		{
			continue;
		}

		FToolPlayMCPGraphExportNode Node;
		Node.Id = BuildMaterialExpressionId(Expression);
		Node.Name = Expression->GetName();
		Node.Title = Expression->GetClass()->GetName();
		Node.NodeClass = Expression->GetClass()->GetName();
		Node.PositionX = Expression->MaterialExpressionEditorX;
		Node.PositionY = Expression->MaterialExpressionEditorY;
		Node.Metadata.Add(TEXT("desc"), Expression->Desc);
		Node.Metadata.Add(TEXT("material_expression_class"), Expression->GetClass()->GetName());

		for (int32 OutputIndex = 0; OutputIndex < Expression->Outputs.Num(); ++OutputIndex)
		{
			FToolPlayMCPGraphExportPin OutputPin;
			const FExpressionOutput& Output = Expression->Outputs[OutputIndex];
			OutputPin.Name = Output.OutputName.IsNone()
				? FString::Printf(TEXT("Output%d"), OutputIndex)
				: Output.OutputName.ToString();
			OutputPin.Direction = TEXT("output");
			OutputPin.Type = TEXT("material_output");
			Node.Pins.Add(MoveTemp(OutputPin));
		}

		for (FExpressionInputIterator It(const_cast<UMaterialExpression*>(Expression)); It; ++It)
		{
			const FExpressionInput* Input = It.Input;
			if (!Input)
			{
				continue;
			}

			FString InputName = Expression->GetInputName(It.Index).ToString();
			if (InputName.IsEmpty())
			{
				InputName = FString::Printf(TEXT("Input%d"), It.Index);
			}

			FToolPlayMCPGraphExportPin InputPin;
			InputPin.Name = InputName;
			InputPin.Direction = TEXT("input");
			InputPin.Type = TEXT("material_input");

			if (Input->Expression)
			{
				const FString FromNodeId = BuildMaterialExpressionId(Input->Expression);
				const int32 OutputIndex = Input->OutputIndex;
				FString OutputName = FString::Printf(TEXT("Output%d"), OutputIndex);
				if (Input->Expression->Outputs.IsValidIndex(OutputIndex) && !Input->Expression->Outputs[OutputIndex].OutputName.IsNone())
				{
					OutputName = Input->Expression->Outputs[OutputIndex].OutputName.ToString();
				}

				InputPin.LinkedTo.Add(FString::Printf(TEXT("%s:%s"), *FromNodeId, *OutputName));

				TMap<FString, FString> LinkMetadata;
				LinkMetadata.Add(TEXT("output_index"), LexToString(OutputIndex));
				LinkMetadata.Add(TEXT("kind"), TEXT("expression_input"));

				AddUniqueGraphLink(
					Graph,
					SeenLinks,
					FromNodeId,
					OutputName,
					Node.Id,
					InputName,
					MoveTemp(LinkMetadata));
			}

			Node.Pins.Add(MoveTemp(InputPin));
		}

		Graph.Nodes.Add(MoveTemp(Node));
	}

	FToolPlayMCPGraphExportNode RootNode;
	RootNode.Id = MaterialRootNodeId;
	RootNode.Name = TEXT("MaterialRoot");
	RootNode.Title = TEXT("Material Output");
	RootNode.NodeClass = TEXT("MaterialRoot");
	RootNode.PositionX = 0;
	RootNode.PositionY = 0;

	for (int32 PropertyIndex = 0; PropertyIndex < MP_MAX; ++PropertyIndex)
	{
		const EMaterialProperty Property = static_cast<EMaterialProperty>(PropertyIndex);
		FExpressionInput* PropertyInput = Material->GetExpressionInputForProperty(Property);
		if (!PropertyInput || !PropertyInput->Expression)
		{
			continue;
		}

		const FString PropertyName = FMaterialAttributeDefinitionMap::GetAttributeName(Property);
		FToolPlayMCPGraphExportPin RootPin;
		RootPin.Name = PropertyName;
		RootPin.Direction = TEXT("input");
		RootPin.Type = TEXT("material_property");

		const FString FromNodeId = BuildMaterialExpressionId(PropertyInput->Expression);
		const int32 OutputIndex = PropertyInput->OutputIndex;
		FString OutputName = FString::Printf(TEXT("Output%d"), OutputIndex);
		if (PropertyInput->Expression->Outputs.IsValidIndex(OutputIndex) && !PropertyInput->Expression->Outputs[OutputIndex].OutputName.IsNone())
		{
			OutputName = PropertyInput->Expression->Outputs[OutputIndex].OutputName.ToString();
		}

		RootPin.LinkedTo.Add(FString::Printf(TEXT("%s:%s"), *FromNodeId, *OutputName));

		TMap<FString, FString> LinkMetadata;
		LinkMetadata.Add(TEXT("output_index"), LexToString(OutputIndex));
		LinkMetadata.Add(TEXT("kind"), TEXT("material_property"));
		LinkMetadata.Add(TEXT("material_property"), PropertyName);

		AddUniqueGraphLink(
			Graph,
			SeenLinks,
			FromNodeId,
			OutputName,
			MaterialRootNodeId,
			PropertyName,
			MoveTemp(LinkMetadata));

		RootNode.Pins.Add(MoveTemp(RootPin));
	}

	Graph.Nodes.Add(MoveTemp(RootNode));
	OutDocument.Messages.Add(TEXT("Material export now includes expression-to-expression links and connections into material output properties."));
	OutDocument.Graphs.Add(MoveTemp(Graph));
	return true;
}

bool FToolPlayMCPMaterialService::ExportCompactMaterial(UMaterial* Material, const FAssetData& AssetData, FToolPlayMCPCompactMaterialGraph& OutGraph, FString& OutSessionId, FString& OutError)
{
	if (!Material)
	{
		OutError = TEXT("Material asset was null.");
		return false;
	}

	TMap<const UMaterialExpression*, FString> ExpressionAliases;
	FToolPlayMCPMaterialGraphSession Session;
	Session.SessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	Session.AssetPath = AssetData.GetSoftObjectPath().ToString();

	OutGraph.Asset = AssetData.AssetName.ToString();
	OutGraph.Scope = TEXT("asset");

	int32 AliasIndex = 0;
	for (UMaterialExpression* Expression : Material->GetExpressions())
	{
		if (!Expression)
		{
			continue;
		}

		const FString Alias = FString::Printf(TEXT("n%d"), AliasIndex++);
		ExpressionAliases.Add(Expression, Alias);

		FToolPlayMCPCompactMaterialNode CompactNode;
		CompactNode.K = NormalizeMaterialExpressionKind(Expression);
		CompactNode.Label = ExtractMaterialNodeLabel(Expression);
		CompactNode.V = ExtractMaterialNodeValue(Expression);
		OutGraph.Nodes.Add(Alias, MoveTemp(CompactNode));

		FToolPlayMCPMaterialNodeBinding Binding;
		Binding.Alias = Alias;
		Binding.ExpressionGuid = BuildMaterialExpressionId(Expression);
		Binding.Expression = Expression;
		Session.NodeBindings.Add(Alias, MoveTemp(Binding));
	}

	FToolPlayMCPCompactMaterialNode RootNode;
	RootNode.K = TEXT("root");
	OutGraph.Nodes.Add(CompactMaterialRootAlias, MoveTemp(RootNode));

	TSet<FString> SeenEdges;
	auto AddCompactEdge = [&OutGraph, &SeenEdges](const FString& FromNode, const FString& FromPin, const FString& ToNode, const FString& ToPin)
	{
		const FString Key = FString::Printf(TEXT("%s|%s|%s|%s"), *FromNode, *FromPin, *ToNode, *ToPin);
		if (SeenEdges.Contains(Key))
		{
			return;
		}

		SeenEdges.Add(Key);

		TArray<FString> Edge;
		Edge.Add(FromNode);
		Edge.Add(FromPin);
		Edge.Add(ToNode);
		Edge.Add(ToPin);
		OutGraph.Edges.Add(MoveTemp(Edge));
	};

	for (UMaterialExpression* Expression : Material->GetExpressions())
	{
		if (!Expression)
		{
			continue;
		}

		const FString* ToAlias = ExpressionAliases.Find(Expression);
		if (!ToAlias)
		{
			continue;
		}

		for (FExpressionInputIterator It(Expression); It; ++It)
		{
			const FExpressionInput* Input = It.Input;
			if (!Input || !Input->Expression)
			{
				continue;
			}

			const FString* FromAlias = ExpressionAliases.Find(Input->Expression);
			if (!FromAlias)
			{
				continue;
			}

			FString InputName = Expression->GetInputName(It.Index).ToString();
			if (InputName.IsEmpty())
			{
				InputName = FString::Printf(TEXT("in%d"), It.Index);
			}

			AddCompactEdge(
				*FromAlias,
				GetMaterialOutputName(Input->Expression, Input->OutputIndex),
				*ToAlias,
				NormalizeMaterialPinName(InputName));
		}
	}

	for (int32 PropertyIndex = 0; PropertyIndex < MP_MAX; ++PropertyIndex)
	{
		const EMaterialProperty Property = static_cast<EMaterialProperty>(PropertyIndex);
		FExpressionInput* PropertyInput = Material->GetExpressionInputForProperty(Property);
		if (!PropertyInput || !PropertyInput->Expression)
		{
			continue;
		}

		const FString* FromAlias = ExpressionAliases.Find(PropertyInput->Expression);
		if (!FromAlias)
		{
			continue;
		}

		AddCompactEdge(
			*FromAlias,
			GetMaterialOutputName(PropertyInput->Expression, PropertyInput->OutputIndex),
			CompactMaterialRootAlias,
			FMaterialAttributeDefinitionMap::GetAttributeName(Property));
	}

	OutSessionId = Session.SessionId;
	GMaterialGraphSessions.Add(Session.SessionId, MoveTemp(Session));
	return true;
}

bool FToolPlayMCPMaterialService::ExportCompactMaterialInstance(UMaterialInstance* MaterialInstance, const FAssetData& AssetData, FToolPlayMCPCompactMaterialGraph& OutGraph, FString& OutSessionId, FString& OutError)
{
	if (!MaterialInstance)
	{
		OutError = TEXT("Material instance asset was null.");
		return false;
	}

	UMaterial* ParentMaterial = MaterialInstance->GetMaterial();
	if (!ParentMaterial)
	{
		OutError = FString::Printf(TEXT("Material instance '%s' has no resolvable parent material."), *AssetData.AssetName.ToString());
		return false;
	}

	FAssetData ParentAssetData(ParentMaterial);
	if (!ExportCompactMaterial(ParentMaterial, ParentAssetData, OutGraph, OutSessionId, OutError))
	{
		return false;
	}

	OutGraph.Asset = AssetData.AssetName.ToString();
	OutGraph.Scope = TEXT("material_instance");
	OutGraph.Parent = MaterialInstance->Parent ? MaterialInstance->Parent->GetPathName() : ParentMaterial->GetPathName();
	ApplyMaterialInstanceOverrides(MaterialInstance, OutGraph);

	return true;
}

FString FToolPlayMCPMaterialService::SerializeDocument(const FToolPlayMCPGraphExportDocument& Document)
{
	FString Output;
	FJsonObjectConverter::UStructToJsonObjectString(Document, Output, 0, 0, 0, nullptr, true);
	return Output;
}

FString FToolPlayMCPMaterialService::SerializeCompactMaterialGraph(const FToolPlayMCPCompactMaterialGraph& Document)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset"), Document.Asset);
	Root->SetStringField(TEXT("scope"), Document.Scope);
	if (!Document.Parent.IsEmpty())
	{
		Root->SetStringField(TEXT("parent"), Document.Parent);
	}

	if (!Document.Overrides.IsEmpty())
	{
		TSharedRef<FJsonObject> OverridesObject = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& Pair : Document.Overrides)
		{
			OverridesObject->SetStringField(Pair.Key, Pair.Value);
		}
		Root->SetObjectField(TEXT("overrides"), OverridesObject);
	}

	TSharedRef<FJsonObject> NodesObject = MakeShared<FJsonObject>();
	for (const TPair<FString, FToolPlayMCPCompactMaterialNode>& Pair : Document.Nodes)
	{
		TSharedRef<FJsonObject> NodeObject = MakeShared<FJsonObject>();
		NodeObject->SetStringField(TEXT("k"), Pair.Value.K);

		if (!Pair.Value.Label.IsEmpty())
		{
			NodeObject->SetStringField(TEXT("label"), Pair.Value.Label);
		}

		if (!Pair.Value.V.IsEmpty())
		{
			NodeObject->SetStringField(TEXT("v"), Pair.Value.V);
		}

		NodesObject->SetObjectField(Pair.Key, NodeObject);
	}
	Root->SetObjectField(TEXT("nodes"), NodesObject);

	TArray<TSharedPtr<FJsonValue>> EdgesArray;
	for (const TArray<FString>& Edge : Document.Edges)
	{
		TArray<TSharedPtr<FJsonValue>> EdgeItems;
		for (const FString& Item : Edge)
		{
			EdgeItems.Add(MakeShared<FJsonValueString>(Item));
		}
		EdgesArray.Add(MakeShared<FJsonValueArray>(MoveTemp(EdgeItems)));
	}
	Root->SetArrayField(TEXT("edges"), MoveTemp(EdgesArray));

	FString Output;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Root, Writer);
	return Output;
}

FString FToolPlayMCPMaterialService::BuildOutputPath(const FAssetData& AssetData)
{
	const FString ExportDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("ToolPlayMCP"), TEXT("Exports"));
	const FString FileName = FString::Printf(TEXT("%s.graph.json"), *AssetData.AssetName.ToString());
	return FPaths::Combine(ExportDirectory, FileName);
}

FString FToolPlayMCPMaterialService::BuildCompactOutputPath(const FAssetData& AssetData)
{
	const FString ExportDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("ToolPlayMCP"), TEXT("Exports"));
	const FString FileName = FString::Printf(TEXT("%s.compact-material.json"), *AssetData.AssetName.ToString());
	return FPaths::Combine(ExportDirectory, FileName);
}

FString FToolPlayMCPMaterialService::BuildNodeId(const UEdGraphNode* Node)
{
	if (!Node)
	{
		return TEXT("invalid-node");
	}

	if (Node->NodeGuid.IsValid())
	{
		return Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
	}

	return Node->GetName();
}
