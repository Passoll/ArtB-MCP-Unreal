#include "Niagara/ToolPlayMCPNiagaraSystemService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "NiagaraComponentRendererProperties.h"
#include "NiagaraGraph.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraEditorSettings.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraLightRendererProperties.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraRibbonRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "NiagaraUserRedirectionParameterStore.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "EdGraph/EdGraphNodeUtils.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/StaticMesh.h"
#include "UObject/UnrealType.h"
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

	FString NormalizeUserParameterName(const FString& UserParameter)
	{
		FString Name = UserParameter.TrimStartAndEnd();
		if (Name.IsEmpty())
		{
			return Name;
		}
		if (!Name.StartsWith(TEXT("User."), ESearchCase::IgnoreCase))
		{
			Name = FString::Printf(TEXT("User.%s"), *Name);
		}
		return Name;
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

	FString CleanRendererClass(const UClass* Class)
	{
		if (!Class)
		{
			return TEXT("unknown");
		}
		FString Name = Class->GetName();
		Name.RemoveFromStart(TEXT("Niagara"));
		Name.RemoveFromEnd(TEXT("RendererProperties"));
		return Name.ToLower();
	}

	UClass* ResolveRendererClass(const FString& RendererType)
	{
		if (RendererType.Equals(TEXT("sprite"), ESearchCase::IgnoreCase) || RendererType.Equals(TEXT("sprites"), ESearchCase::IgnoreCase))
		{
			return UNiagaraSpriteRendererProperties::StaticClass();
		}
		if (RendererType.Equals(TEXT("mesh"), ESearchCase::IgnoreCase) || RendererType.Equals(TEXT("meshes"), ESearchCase::IgnoreCase))
		{
			return UNiagaraMeshRendererProperties::StaticClass();
		}
		if (RendererType.Equals(TEXT("ribbon"), ESearchCase::IgnoreCase) || RendererType.Equals(TEXT("ribbons"), ESearchCase::IgnoreCase))
		{
			return UNiagaraRibbonRendererProperties::StaticClass();
		}
		if (RendererType.Equals(TEXT("light"), ESearchCase::IgnoreCase) || RendererType.Equals(TEXT("lights"), ESearchCase::IgnoreCase))
		{
			return UNiagaraLightRendererProperties::StaticClass();
		}
		if (RendererType.Equals(TEXT("component"), ESearchCase::IgnoreCase) || RendererType.Equals(TEXT("components"), ESearchCase::IgnoreCase))
		{
			return UNiagaraComponentRendererProperties::StaticClass();
		}
		return nullptr;
	}

	TArray<FString> GetAllowedRendererProperties(const UClass* RendererClass)
	{
		TArray<FString> Properties;
		if (!RendererClass)
		{
			return Properties;
		}

		if (RendererClass->IsChildOf<UNiagaraSpriteRendererProperties>())
		{
			Properties = { TEXT("SourceMode"), TEXT("Alignment"), TEXT("FacingMode"), TEXT("PivotInUVSpace"), TEXT("SortMode"), TEXT("bCastShadows"), TEXT("bRemoveHMDRollInVR") };
		}
		else if (RendererClass->IsChildOf<UNiagaraMeshRendererProperties>())
		{
			Properties = { TEXT("SourceMode"), TEXT("SortMode"), TEXT("FacingMode"), TEXT("bCastShadows"), TEXT("bOverrideMaterials"), TEXT("bEnableFrustumCulling"), TEXT("bEnableCameraDistanceCulling"), TEXT("mesh_asset") };
		}
		else if (RendererClass->IsChildOf<UNiagaraRibbonRendererProperties>())
		{
			Properties = { TEXT("FacingMode"), TEXT("DrawDirection"), TEXT("Shape"), TEXT("bScreenSpaceTessellation"), TEXT("bUseMaterialBackfaceCulling"), TEXT("bCastShadows") };
		}
		else if (RendererClass->IsChildOf<UNiagaraLightRendererProperties>())
		{
			Properties = { TEXT("SourceMode"), TEXT("bUseInverseSquaredFalloff"), TEXT("bAffectsTranslucency"), TEXT("RadiusScale"), TEXT("DefaultExponent"), TEXT("ColorAdd") };
		}
		else if (RendererClass->IsChildOf<UNiagaraComponentRendererProperties>())
		{
			Properties = { TEXT("ComponentType"), TEXT("ComponentCountLimit"), TEXT("bAssignComponentsOnParticleID"), TEXT("bCreateComponentFirstParticleFrame"), TEXT("bOnlyCreateComponentsOnParticleSpawn") };
		}
		return Properties;
	}

	TArray<FString> GetAllowedStageProperties()
	{
		return {
			TEXT("SimulationStageName"),
			TEXT("bEnabled"),
			TEXT("IterationSource"),
			TEXT("ExecuteBehavior"),
			TEXT("bDisablePartialParticleUpdate"),
			TEXT("bParticleIterationStateEnabled"),
			TEXT("ParticleIterationStateRange"),
			TEXT("bGpuDispatchForceLinear"),
			TEXT("bOverrideGpuDispatchNumThreads"),
			TEXT("DirectDispatchType"),
			TEXT("DirectDispatchElementType")
		};
	}

	bool IsAllowedProperty(const TArray<FString>& AllowedProperties, const FString& Property)
	{
		for (const FString& Allowed : AllowedProperties)
		{
			if (Allowed.Equals(Property, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	FString ExportPropertyValue(UObject* Object, const FString& PropertyName)
	{
		if (!Object)
		{
			return FString();
		}
		if (FProperty* Property = Object->GetClass()->FindPropertyByName(*PropertyName))
		{
			FString Value;
			Property->ExportText_InContainer(0, Value, Object, Object, Object, PPF_None);
			return Value;
		}
		return FString();
	}

	bool ImportObjectPropertyValue(UObject* Object, const FString& PropertyName, const FString& Value, FString& OutError)
	{
		if (!Object)
		{
			OutError = TEXT("Invalid object.");
			return false;
		}

		FProperty* Property = Object->GetClass()->FindPropertyByName(*PropertyName);
		if (!Property)
		{
			OutError = FString::Printf(TEXT("Property '%s' was not found on %s."), *PropertyName, *Object->GetClass()->GetName());
			return false;
		}

		const TCHAR* ImportResult = Property->ImportText_Direct(*Value, Property->ContainerPtrToValuePtr<void>(Object), Object, PPF_None);
		if (!ImportResult)
		{
			OutError = FString::Printf(TEXT("Failed to import value '%s' for property '%s'."), *Value, *PropertyName);
			return false;
		}
		return true;
	}

	TSharedRef<FJsonObject> BuildMutationResult(UNiagaraSystem* System, const FString& Operation)
	{
		TSharedRef<FJsonObject> Root = BuildSystemResult(System);
		Root->SetStringField(TEXT("operation"), Operation);
		Root->SetBoolField(TEXT("compile_requested"), true);
		Root->SetBoolField(TEXT("compile_result_included"), false);
		Root->SetStringField(TEXT("validation_next_step"), TEXT("Call niagara(action='diagnostics', params={'asset_path': asset, 'force': true, 'wait': true}) after this mutation."));
		return Root;
	}

	bool ResolveEmitterForEdit(UNiagaraSystem* System, const FString& EmitterAlias, FNiagaraEmitterHandle*& OutHandle, FVersionedNiagaraEmitterData*& OutData, FVersionedNiagaraEmitter& OutEmitter, FString& OutError)
	{
		if (!ResolveEmitterHandle(System, EmitterAlias, OutHandle, OutError))
		{
			return false;
		}

		OutData = OutHandle ? OutHandle->GetEmitterData() : nullptr;
		OutEmitter = OutHandle ? OutHandle->GetInstance() : FVersionedNiagaraEmitter();
		if (!OutData || !OutEmitter.Emitter)
		{
			OutError = FString::Printf(TEXT("Emitter alias does not resolve to editable emitter data: %s"), *EmitterAlias);
			return false;
		}
		return true;
	}

	TSharedRef<FJsonObject> ExportRendererObject(const UNiagaraRendererProperties* Renderer, int32 Index)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("index"), Index);
		Object->SetStringField(TEXT("class"), Renderer ? Renderer->GetClass()->GetName() : TEXT("null"));
		Object->SetStringField(TEXT("type"), Renderer ? CleanRendererClass(Renderer->GetClass()) : TEXT("null"));
		Object->SetStringField(TEXT("name"), Renderer ? Renderer->GetName() : FString());
		if (Renderer)
		{
			TArray<TSharedPtr<FJsonValue>> Editable;
			for (const FString& PropertyName : GetAllowedRendererProperties(Renderer->GetClass()))
			{
				TSharedRef<FJsonObject> PropertyObject = MakeShared<FJsonObject>();
				PropertyObject->SetStringField(TEXT("name"), PropertyName);
				PropertyObject->SetStringField(TEXT("value"), ExportPropertyValue(const_cast<UNiagaraRendererProperties*>(Renderer), PropertyName));
				Editable.Add(MakeShared<FJsonValueObject>(PropertyObject));
			}
			Object->SetArrayField(TEXT("editable_properties"), Editable);
		}
		return Object;
	}

	TSharedRef<FJsonObject> ExportStageObject(const UNiagaraSimulationStageBase* Stage, int32 Index)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("index"), Index);
		Object->SetStringField(TEXT("class"), Stage ? Stage->GetClass()->GetName() : TEXT("null"));
		Object->SetStringField(TEXT("name"), Stage ? Stage->SimulationStageName.ToString() : FString());
		Object->SetBoolField(TEXT("enabled"), Stage ? Stage->bEnabled != 0 : false);
		if (Stage)
		{
			Object->SetStringField(TEXT("script_usage_id"), Stage->Script ? Stage->Script->GetUsageId().ToString(EGuidFormats::DigitsWithHyphens) : FString());
			TArray<TSharedPtr<FJsonValue>> Editable;
			for (const FString& PropertyName : GetAllowedStageProperties())
			{
				TSharedRef<FJsonObject> PropertyObject = MakeShared<FJsonObject>();
				PropertyObject->SetStringField(TEXT("name"), PropertyName);
				PropertyObject->SetStringField(TEXT("value"), ExportPropertyValue(const_cast<UNiagaraSimulationStageBase*>(Stage), PropertyName));
				Editable.Add(MakeShared<FJsonValueObject>(PropertyObject));
			}
			Object->SetArrayField(TEXT("editable_properties"), Editable);
		}
		return Object;
	}

	bool SetMeshRendererAsset(UNiagaraMeshRendererProperties* MeshRenderer, const FString& MeshAssetPath, FString& OutError)
	{
		if (!MeshRenderer)
		{
			OutError = TEXT("Renderer is not a Mesh Renderer.");
			return false;
		}

		UStaticMesh* Mesh = Cast<UStaticMesh>(FSoftObjectPath(MeshAssetPath).TryLoad());
		if (!Mesh)
		{
			OutError = FString::Printf(TEXT("Asset is not a StaticMesh: %s"), *MeshAssetPath);
			return false;
		}

		if (MeshRenderer->Meshes.Num() == 0)
		{
			MeshRenderer->Meshes.AddDefaulted();
		}
		MeshRenderer->Meshes[0].Mesh = Mesh;
		return true;
	}

	UNiagaraNodeOutput* ResetGraphForOutputLite(UNiagaraGraph& Graph, ENiagaraScriptUsage ScriptUsage, FGuid UsageId)
	{
		Graph.Modify();

		TArray<UNiagaraNodeOutput*> OutputNodes;
		Graph.GetNodesOfClass<UNiagaraNodeOutput>(OutputNodes);
		for (UNiagaraNodeOutput* ExistingOutput : OutputNodes)
		{
			if (ExistingOutput && ExistingOutput->GetUsage() == ScriptUsage && ExistingOutput->GetUsageId() == UsageId)
			{
				ExistingOutput->Modify();
				return ExistingOutput;
			}
		}

		FGraphNodeCreator<UNiagaraNodeOutput> OutputCreator(Graph);
		UNiagaraNodeOutput* OutputNode = OutputCreator.CreateNode();
		OutputNode->SetUsage(ScriptUsage);
		OutputNode->SetUsageId(UsageId);
		OutputNode->Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Out")));
		OutputCreator.Finalize();

		FGraphNodeCreator<UNiagaraNodeInput> InputCreator(Graph);
		UNiagaraNodeInput* InputNode = InputCreator.CreateNode();
		InputNode->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("InputMap"));
		InputNode->Usage = ENiagaraInputNodeUsage::Parameter;
		InputCreator.Finalize();

		UEdGraphPin* InputOutputPin = InputNode->GetOutputPin(0);
		UEdGraphPin* OutputInputPin = OutputNode->GetInputPin(0);
		if (InputOutputPin && OutputInputPin)
		{
			OutputInputPin->BreakAllPinLinks();
			InputOutputPin->MakeLinkTo(OutputInputPin);
		}

		Graph.NotifyGraphChanged();
		return OutputNode;
	}

	void RemoveGraphOutputLite(UNiagaraGraph& Graph, ENiagaraScriptUsage ScriptUsage, FGuid UsageId)
	{
		Graph.Modify();
		TArray<UNiagaraNodeOutput*> OutputNodes;
		Graph.GetNodesOfClass<UNiagaraNodeOutput>(OutputNodes);
		for (UNiagaraNodeOutput* OutputNode : OutputNodes)
		{
			if (OutputNode && OutputNode->GetUsage() == ScriptUsage && OutputNode->GetUsageId() == UsageId)
			{
				OutputNode->Modify();
				Graph.RemoveNode(OutputNode);
			}
		}
		Graph.NotifyGraphChanged();
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

bool FToolPlayMCPNiagaraSystemService::RemoveUserParameter(const FString& SystemAssetPath, const FString& UserParameter, FString& OutJson, FString& OutError)
{
	UNiagaraSystem* System = Cast<UNiagaraSystem>(FSoftObjectPath(SystemAssetPath).TryLoad());
	if (!System)
	{
		OutError = FString::Printf(TEXT("Asset is not a Niagara System: %s"), *SystemAssetPath);
		return false;
	}

	const FString UserParameterName = NormalizeUserParameterName(UserParameter);
	if (UserParameterName.IsEmpty())
	{
		OutError = TEXT("user_parameter is required.");
		return false;
	}

	FNiagaraUserRedirectionParameterStore& UserParameters = System->GetExposedParameters();
	TArray<FNiagaraVariable> Parameters;
	UserParameters.GetParameters(Parameters);

	TOptional<FNiagaraVariable> MatchedParameter;
	for (const FNiagaraVariable& Parameter : Parameters)
	{
		if (Parameter.GetName().ToString().Equals(UserParameterName, ESearchCase::IgnoreCase))
		{
			MatchedParameter = Parameter;
			break;
		}
	}

	if (!MatchedParameter.IsSet())
	{
		OutError = FString::Printf(TEXT("User parameter not found: %s"), *UserParameterName);
		return false;
	}

	const FScopedTransaction Transaction(FText::FromString(TEXT("ToolPlayMCP: Remove Niagara User Parameter")));
	System->Modify();
	const bool bRemoved = UserParameters.RemoveParameter(MatchedParameter.GetValue());
	if (!bRemoved)
	{
		OutError = FString::Printf(TEXT("Failed to remove user parameter: %s"), *UserParameterName);
		return false;
	}
	UserParameters.OnInterfaceChange();
	System->MarkPackageDirty();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> Root = BuildMutationResult(System, TEXT("remove_user_parameter"));
	Root->SetStringField(TEXT("user_parameter"), UserParameterName);
	Root->SetStringField(TEXT("removed_type"), MatchedParameter->GetType().GetName());
	Root->SetStringField(TEXT("reference_warning"), TEXT("This removes the exposed User parameter value. Existing module links to the same User.* name are not automatically disconnected; export or inspect module input overrides before assuming references are gone."));
	OutJson = ToJsonString(Root);
	return true;
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

bool FToolPlayMCPNiagaraSystemService::ListRenderers(const FString& SystemAssetPath, const FString& EmitterAlias, FString& OutJson, FString& OutError)
{
	UNiagaraSystem* System = Cast<UNiagaraSystem>(FSoftObjectPath(SystemAssetPath).TryLoad());
	if (!System)
	{
		OutError = FString::Printf(TEXT("Asset is not a Niagara System: %s"), *SystemAssetPath);
		return false;
	}

	FNiagaraEmitterHandle* Handle = nullptr;
	FVersionedNiagaraEmitterData* EmitterData = nullptr;
	FVersionedNiagaraEmitter VersionedEmitter;
	if (!ResolveEmitterForEdit(System, EmitterAlias, Handle, EmitterData, VersionedEmitter, OutError))
	{
		return false;
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetStringField(TEXT("asset_path"), System->GetPathName());
	Root->SetStringField(TEXT("emitter"), EmitterAlias);

	TArray<TSharedPtr<FJsonValue>> Renderers;
	const TArray<UNiagaraRendererProperties*>& RendererList = EmitterData->GetRenderers();
	for (int32 Index = 0; Index < RendererList.Num(); ++Index)
	{
		Renderers.Add(MakeShared<FJsonValueObject>(ExportRendererObject(RendererList[Index], Index)));
	}
	Root->SetArrayField(TEXT("renderers"), Renderers);
	Root->SetArrayField(TEXT("supported_types"), {
		MakeShared<FJsonValueString>(TEXT("sprite")),
		MakeShared<FJsonValueString>(TEXT("mesh")),
		MakeShared<FJsonValueString>(TEXT("ribbon")),
		MakeShared<FJsonValueString>(TEXT("light")),
		MakeShared<FJsonValueString>(TEXT("component"))
	});
	OutJson = ToJsonString(Root);
	return true;
}

bool FToolPlayMCPNiagaraSystemService::GetRendererSchema(const FString& RendererTypeOrClass, FString& OutJson, FString& OutError)
{
	UClass* RendererClass = ResolveRendererClass(RendererTypeOrClass);
	if (!RendererClass)
	{
		OutError = FString::Printf(TEXT("Unsupported renderer type '%s'. Use sprite, mesh, ribbon, light, or component."), *RendererTypeOrClass);
		return false;
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetStringField(TEXT("renderer_type"), CleanRendererClass(RendererClass));
	Root->SetStringField(TEXT("class"), RendererClass->GetName());
	Root->SetStringField(TEXT("set_property_rule"), TEXT("Only fields listed in editable_properties can be changed through set_renderer_property. Use value strings in Unreal import-text format for complex structs/enums."));
	TArray<TSharedPtr<FJsonValue>> Properties;
	for (const FString& PropertyName : GetAllowedRendererProperties(RendererClass))
	{
		TSharedRef<FJsonObject> PropertyObject = MakeShared<FJsonObject>();
		PropertyObject->SetStringField(TEXT("name"), PropertyName);
		if (FProperty* Property = RendererClass->FindPropertyByName(*PropertyName))
		{
			PropertyObject->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
			PropertyObject->SetStringField(TEXT("kind"), Property->GetClass()->GetName());
		}
		else if (PropertyName == TEXT("mesh_asset"))
		{
			PropertyObject->SetStringField(TEXT("cpp_type"), TEXT("UStaticMesh asset path"));
			PropertyObject->SetStringField(TEXT("kind"), TEXT("special_writer"));
		}
		Properties.Add(MakeShared<FJsonValueObject>(PropertyObject));
	}
	Root->SetArrayField(TEXT("editable_properties"), Properties);
	OutJson = ToJsonString(Root);
	return true;
}

bool FToolPlayMCPNiagaraSystemService::AddRenderer(const FString& SystemAssetPath, const FString& EmitterAlias, const FString& RendererType, int32 TargetIndex, const FString& MeshAssetPath, FString& OutJson, FString& OutError)
{
	UNiagaraSystem* System = Cast<UNiagaraSystem>(FSoftObjectPath(SystemAssetPath).TryLoad());
	if (!System)
	{
		OutError = FString::Printf(TEXT("Asset is not a Niagara System: %s"), *SystemAssetPath);
		return false;
	}

	UClass* RendererClass = ResolveRendererClass(RendererType);
	if (!RendererClass)
	{
		OutError = FString::Printf(TEXT("Unsupported renderer type '%s'. Use sprite, mesh, ribbon, light, or component."), *RendererType);
		return false;
	}

	FNiagaraEmitterHandle* Handle = nullptr;
	FVersionedNiagaraEmitterData* EmitterData = nullptr;
	FVersionedNiagaraEmitter VersionedEmitter;
	if (!ResolveEmitterForEdit(System, EmitterAlias, Handle, EmitterData, VersionedEmitter, OutError))
	{
		return false;
	}
	if (TargetIndex >= 0 && TargetIndex > EmitterData->GetRenderers().Num())
	{
		OutError = FString::Printf(TEXT("target_index %d is out of range for adding renderer to emitter %s."), TargetIndex, *EmitterAlias);
		return false;
	}

	const FScopedTransaction Transaction(FText::FromString(TEXT("ToolPlayMCP: Add Niagara Renderer")));
	System->Modify();
	VersionedEmitter.Emitter->Modify();
	UNiagaraRendererProperties* Renderer = NewObject<UNiagaraRendererProperties>(VersionedEmitter.Emitter, RendererClass, NAME_None, RF_Transactional);
	if (!Renderer)
	{
		OutError = FString::Printf(TEXT("Failed to create renderer type '%s'."), *RendererType);
		return false;
	}
	Renderer->Modify();

	if (!MeshAssetPath.IsEmpty())
	{
		if (!SetMeshRendererAsset(Cast<UNiagaraMeshRendererProperties>(Renderer), MeshAssetPath, OutError))
		{
			return false;
		}
	}

	VersionedEmitter.Emitter->AddRenderer(Renderer, VersionedEmitter.Version);
	const int32 LastIndex = EmitterData->GetRenderers().Num() - 1;
	if (TargetIndex >= 0 && TargetIndex < LastIndex)
	{
		VersionedEmitter.Emitter->MoveRenderer(Renderer, TargetIndex, VersionedEmitter.Version);
	}
	System->MarkPackageDirty();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> Root = BuildMutationResult(System, TEXT("add_renderer"));
	Root->SetStringField(TEXT("emitter"), EmitterAlias);
	Root->SetStringField(TEXT("renderer_type"), CleanRendererClass(RendererClass));
	Root->SetObjectField(TEXT("renderer"), ExportRendererObject(Renderer, TargetIndex >= 0 && TargetIndex < EmitterData->GetRenderers().Num() ? TargetIndex : LastIndex));
	OutJson = ToJsonString(Root);
	return true;
}

bool FToolPlayMCPNiagaraSystemService::RemoveRenderer(const FString& SystemAssetPath, const FString& EmitterAlias, int32 RendererIndex, FString& OutJson, FString& OutError)
{
	UNiagaraSystem* System = Cast<UNiagaraSystem>(FSoftObjectPath(SystemAssetPath).TryLoad());
	if (!System)
	{
		OutError = FString::Printf(TEXT("Asset is not a Niagara System: %s"), *SystemAssetPath);
		return false;
	}

	FNiagaraEmitterHandle* Handle = nullptr;
	FVersionedNiagaraEmitterData* EmitterData = nullptr;
	FVersionedNiagaraEmitter VersionedEmitter;
	if (!ResolveEmitterForEdit(System, EmitterAlias, Handle, EmitterData, VersionedEmitter, OutError))
	{
		return false;
	}

	const TArray<UNiagaraRendererProperties*>& Renderers = EmitterData->GetRenderers();
	if (!Renderers.IsValidIndex(RendererIndex))
	{
		OutError = FString::Printf(TEXT("Renderer index %d is out of range for emitter %s."), RendererIndex, *EmitterAlias);
		return false;
	}

	UNiagaraRendererProperties* Renderer = Renderers[RendererIndex];
	const FScopedTransaction Transaction(FText::FromString(TEXT("ToolPlayMCP: Remove Niagara Renderer")));
	System->Modify();
	VersionedEmitter.Emitter->Modify();
	if (Renderer)
	{
		Renderer->Modify();
	}
	VersionedEmitter.Emitter->RemoveRenderer(Renderer, VersionedEmitter.Version);
	System->MarkPackageDirty();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> Root = BuildMutationResult(System, TEXT("remove_renderer"));
	Root->SetStringField(TEXT("emitter"), EmitterAlias);
	Root->SetNumberField(TEXT("renderer_index"), RendererIndex);
	OutJson = ToJsonString(Root);
	return true;
}

bool FToolPlayMCPNiagaraSystemService::SetRendererProperty(const FString& SystemAssetPath, const FString& EmitterAlias, int32 RendererIndex, const FString& Property, const FString& Value, FString& OutJson, FString& OutError)
{
	UNiagaraSystem* System = Cast<UNiagaraSystem>(FSoftObjectPath(SystemAssetPath).TryLoad());
	if (!System)
	{
		OutError = FString::Printf(TEXT("Asset is not a Niagara System: %s"), *SystemAssetPath);
		return false;
	}

	FNiagaraEmitterHandle* Handle = nullptr;
	FVersionedNiagaraEmitterData* EmitterData = nullptr;
	FVersionedNiagaraEmitter VersionedEmitter;
	if (!ResolveEmitterForEdit(System, EmitterAlias, Handle, EmitterData, VersionedEmitter, OutError))
	{
		return false;
	}

	const TArray<UNiagaraRendererProperties*>& Renderers = EmitterData->GetRenderers();
	if (!Renderers.IsValidIndex(RendererIndex) || !Renderers[RendererIndex])
	{
		OutError = FString::Printf(TEXT("Renderer index %d is out of range for emitter %s."), RendererIndex, *EmitterAlias);
		return false;
	}

	UNiagaraRendererProperties* Renderer = Renderers[RendererIndex];
	if (!IsAllowedProperty(GetAllowedRendererProperties(Renderer->GetClass()), Property))
	{
		OutError = FString::Printf(TEXT("Property '%s' is not exposed for renderer type %s. Call get_renderer_schema first."), *Property, *CleanRendererClass(Renderer->GetClass()));
		return false;
	}

	const FScopedTransaction Transaction(FText::FromString(TEXT("ToolPlayMCP: Set Niagara Renderer Property")));
	System->Modify();
	Renderer->Modify();
	if (Property.Equals(TEXT("mesh_asset"), ESearchCase::IgnoreCase))
	{
		if (!SetMeshRendererAsset(Cast<UNiagaraMeshRendererProperties>(Renderer), Value, OutError))
		{
			return false;
		}
	}
	else if (!ImportObjectPropertyValue(Renderer, Property, Value, OutError))
	{
		return false;
	}

	System->MarkPackageDirty();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> Root = BuildMutationResult(System, TEXT("set_renderer_property"));
	Root->SetStringField(TEXT("emitter"), EmitterAlias);
	Root->SetNumberField(TEXT("renderer_index"), RendererIndex);
	Root->SetStringField(TEXT("property"), Property);
	Root->SetStringField(TEXT("stored_value"), Property.Equals(TEXT("mesh_asset"), ESearchCase::IgnoreCase) ? Value : ExportPropertyValue(Renderer, Property));
	OutJson = ToJsonString(Root);
	return true;
}

bool FToolPlayMCPNiagaraSystemService::ListSimulationStages(const FString& SystemAssetPath, const FString& EmitterAlias, FString& OutJson, FString& OutError)
{
	UNiagaraSystem* System = Cast<UNiagaraSystem>(FSoftObjectPath(SystemAssetPath).TryLoad());
	if (!System)
	{
		OutError = FString::Printf(TEXT("Asset is not a Niagara System: %s"), *SystemAssetPath);
		return false;
	}

	FNiagaraEmitterHandle* Handle = nullptr;
	FVersionedNiagaraEmitterData* EmitterData = nullptr;
	FVersionedNiagaraEmitter VersionedEmitter;
	if (!ResolveEmitterForEdit(System, EmitterAlias, Handle, EmitterData, VersionedEmitter, OutError))
	{
		return false;
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetStringField(TEXT("asset_path"), System->GetPathName());
	Root->SetStringField(TEXT("emitter"), EmitterAlias);
	Root->SetArrayField(TEXT("editable_properties"), {});
	TArray<TSharedPtr<FJsonValue>> Stages;
	const TArray<UNiagaraSimulationStageBase*>& StageList = EmitterData->GetSimulationStages();
	for (int32 Index = 0; Index < StageList.Num(); ++Index)
	{
		Stages.Add(MakeShared<FJsonValueObject>(ExportStageObject(StageList[Index], Index)));
	}
	Root->SetArrayField(TEXT("simulation_stages"), Stages);
	OutJson = ToJsonString(Root);
	return true;
}

bool FToolPlayMCPNiagaraSystemService::AddSimulationStage(const FString& SystemAssetPath, const FString& EmitterAlias, const FString& StageName, int32 TargetIndex, FString& OutJson, FString& OutError)
{
	UNiagaraSystem* System = Cast<UNiagaraSystem>(FSoftObjectPath(SystemAssetPath).TryLoad());
	if (!System)
	{
		OutError = FString::Printf(TEXT("Asset is not a Niagara System: %s"), *SystemAssetPath);
		return false;
	}

	FNiagaraEmitterHandle* Handle = nullptr;
	FVersionedNiagaraEmitterData* EmitterData = nullptr;
	FVersionedNiagaraEmitter VersionedEmitter;
	if (!ResolveEmitterForEdit(System, EmitterAlias, Handle, EmitterData, VersionedEmitter, OutError))
	{
		return false;
	}
	if (TargetIndex >= 0 && TargetIndex > EmitterData->GetSimulationStages().Num())
	{
		OutError = FString::Printf(TEXT("target_index %d is out of range for adding simulation stage to emitter %s."), TargetIndex, *EmitterAlias);
		return false;
	}

	UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
	if (!Source || !Source->NodeGraph)
	{
		OutError = TEXT("Emitter does not have an editable Niagara graph source for simulation stage output.");
		return false;
	}

	const FScopedTransaction Transaction(FText::FromString(TEXT("ToolPlayMCP: Add Niagara Simulation Stage")));
	System->Modify();
	VersionedEmitter.Emitter->Modify();
	UNiagaraSimulationStageGeneric* Stage = NewObject<UNiagaraSimulationStageGeneric>(VersionedEmitter.Emitter, UNiagaraSimulationStageGeneric::StaticClass(), NAME_None, RF_Transactional);
	Stage->Modify();
	Stage->SimulationStageName = StageName.IsEmpty() ? FName(TEXT("SimulationStage")) : FName(*StageName);
	Stage->Script = NewObject<UNiagaraScript>(Stage, MakeUniqueObjectName(Stage, UNiagaraScript::StaticClass(), TEXT("SimulationStage")), RF_Transactional);
	Stage->Script->SetUsage(ENiagaraScriptUsage::ParticleSimulationStageScript);
	Stage->Script->SetUsageId(Stage->GetMergeId());
	Stage->Script->SetLatestSource(Source);

	VersionedEmitter.Emitter->AddSimulationStage(Stage, VersionedEmitter.Version);
	if (TargetIndex >= 0)
	{
		VersionedEmitter.Emitter->MoveSimulationStageToIndex(Stage, TargetIndex, VersionedEmitter.Version);
	}
	ResetGraphForOutputLite(*Source->NodeGraph, ENiagaraScriptUsage::ParticleSimulationStageScript, Stage->Script->GetUsageId());
	System->MarkPackageDirty();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> Root = BuildMutationResult(System, TEXT("add_simulation_stage"));
	Root->SetStringField(TEXT("emitter"), EmitterAlias);
	Root->SetObjectField(TEXT("simulation_stage"), ExportStageObject(Stage, TargetIndex >= 0 ? TargetIndex : EmitterData->GetSimulationStages().Num() - 1));
	OutJson = ToJsonString(Root);
	return true;
}

bool FToolPlayMCPNiagaraSystemService::RemoveSimulationStage(const FString& SystemAssetPath, const FString& EmitterAlias, int32 StageIndex, FString& OutJson, FString& OutError)
{
	UNiagaraSystem* System = Cast<UNiagaraSystem>(FSoftObjectPath(SystemAssetPath).TryLoad());
	if (!System)
	{
		OutError = FString::Printf(TEXT("Asset is not a Niagara System: %s"), *SystemAssetPath);
		return false;
	}

	FNiagaraEmitterHandle* Handle = nullptr;
	FVersionedNiagaraEmitterData* EmitterData = nullptr;
	FVersionedNiagaraEmitter VersionedEmitter;
	if (!ResolveEmitterForEdit(System, EmitterAlias, Handle, EmitterData, VersionedEmitter, OutError))
	{
		return false;
	}

	const TArray<UNiagaraSimulationStageBase*>& Stages = EmitterData->GetSimulationStages();
	if (!Stages.IsValidIndex(StageIndex) || !Stages[StageIndex])
	{
		OutError = FString::Printf(TEXT("Simulation stage index %d is out of range for emitter %s."), StageIndex, *EmitterAlias);
		return false;
	}

	UNiagaraSimulationStageBase* Stage = Stages[StageIndex];
	const FScopedTransaction Transaction(FText::FromString(TEXT("ToolPlayMCP: Remove Niagara Simulation Stage")));
	System->Modify();
	VersionedEmitter.Emitter->Modify();
	Stage->Modify();
	if (UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(EmitterData->GraphSource))
	{
		if (Source->NodeGraph && Stage->Script)
		{
			RemoveGraphOutputLite(*Source->NodeGraph, ENiagaraScriptUsage::ParticleSimulationStageScript, Stage->Script->GetUsageId());
		}
	}
	VersionedEmitter.Emitter->RemoveSimulationStage(Stage, VersionedEmitter.Version);
	System->MarkPackageDirty();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> Root = BuildMutationResult(System, TEXT("remove_simulation_stage"));
	Root->SetStringField(TEXT("emitter"), EmitterAlias);
	Root->SetNumberField(TEXT("stage_index"), StageIndex);
	OutJson = ToJsonString(Root);
	return true;
}

bool FToolPlayMCPNiagaraSystemService::MoveSimulationStage(const FString& SystemAssetPath, const FString& EmitterAlias, int32 StageIndex, int32 TargetIndex, FString& OutJson, FString& OutError)
{
	UNiagaraSystem* System = Cast<UNiagaraSystem>(FSoftObjectPath(SystemAssetPath).TryLoad());
	if (!System)
	{
		OutError = FString::Printf(TEXT("Asset is not a Niagara System: %s"), *SystemAssetPath);
		return false;
	}

	FNiagaraEmitterHandle* Handle = nullptr;
	FVersionedNiagaraEmitterData* EmitterData = nullptr;
	FVersionedNiagaraEmitter VersionedEmitter;
	if (!ResolveEmitterForEdit(System, EmitterAlias, Handle, EmitterData, VersionedEmitter, OutError))
	{
		return false;
	}

	const TArray<UNiagaraSimulationStageBase*>& Stages = EmitterData->GetSimulationStages();
	if (!Stages.IsValidIndex(StageIndex) || !Stages[StageIndex] || !Stages.IsValidIndex(TargetIndex))
	{
		OutError = FString::Printf(TEXT("Simulation stage move indices are out of range. stage_index=%d target_index=%d."), StageIndex, TargetIndex);
		return false;
	}

	const FScopedTransaction Transaction(FText::FromString(TEXT("ToolPlayMCP: Move Niagara Simulation Stage")));
	System->Modify();
	VersionedEmitter.Emitter->Modify();
	VersionedEmitter.Emitter->MoveSimulationStageToIndex(Stages[StageIndex], TargetIndex, VersionedEmitter.Version);
	System->MarkPackageDirty();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> Root = BuildMutationResult(System, TEXT("move_simulation_stage"));
	Root->SetStringField(TEXT("emitter"), EmitterAlias);
	Root->SetNumberField(TEXT("stage_index"), StageIndex);
	Root->SetNumberField(TEXT("target_index"), TargetIndex);
	OutJson = ToJsonString(Root);
	return true;
}

bool FToolPlayMCPNiagaraSystemService::SetSimulationStageProperty(const FString& SystemAssetPath, const FString& EmitterAlias, int32 StageIndex, const FString& Property, const FString& Value, FString& OutJson, FString& OutError)
{
	UNiagaraSystem* System = Cast<UNiagaraSystem>(FSoftObjectPath(SystemAssetPath).TryLoad());
	if (!System)
	{
		OutError = FString::Printf(TEXT("Asset is not a Niagara System: %s"), *SystemAssetPath);
		return false;
	}

	FNiagaraEmitterHandle* Handle = nullptr;
	FVersionedNiagaraEmitterData* EmitterData = nullptr;
	FVersionedNiagaraEmitter VersionedEmitter;
	if (!ResolveEmitterForEdit(System, EmitterAlias, Handle, EmitterData, VersionedEmitter, OutError))
	{
		return false;
	}

	const TArray<UNiagaraSimulationStageBase*>& Stages = EmitterData->GetSimulationStages();
	if (!Stages.IsValidIndex(StageIndex) || !Stages[StageIndex])
	{
		OutError = FString::Printf(TEXT("Simulation stage index %d is out of range for emitter %s."), StageIndex, *EmitterAlias);
		return false;
	}

	UNiagaraSimulationStageBase* Stage = Stages[StageIndex];
	if (!IsAllowedProperty(GetAllowedStageProperties(), Property))
	{
		OutError = FString::Printf(TEXT("Property '%s' is not exposed for simulation stages."), *Property);
		return false;
	}

	const FScopedTransaction Transaction(FText::FromString(TEXT("ToolPlayMCP: Set Niagara Simulation Stage Property")));
	System->Modify();
	Stage->Modify();
	if (!ImportObjectPropertyValue(Stage, Property, Value, OutError))
	{
		return false;
	}
	Stage->RequestRecompile();
	System->MarkPackageDirty();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> Root = BuildMutationResult(System, TEXT("set_simulation_stage_property"));
	Root->SetStringField(TEXT("emitter"), EmitterAlias);
	Root->SetNumberField(TEXT("stage_index"), StageIndex);
	Root->SetStringField(TEXT("property"), Property);
	Root->SetStringField(TEXT("stored_value"), ExportPropertyValue(Stage, Property));
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
