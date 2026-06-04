// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class ToolPlayMCP : ModuleRules
{
	public ToolPlayMCP(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"Json",
				"JsonUtilities"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Slate",
				"SlateCore",
				"ApplicationCore",
				"AssetRegistry",
				"BlueprintGraph",
				"ContentBrowser",
				"EditorStyle",
				"GraphEditor",
				"InputCore",
				"MaterialEditor",
				"Networking",
				"Niagara",
				"NiagaraCore",
				"NiagaraEditor",
				"Projects",
				"Sockets",
				"ToolMenus",
				"UnrealEd",
				"WorkspaceMenuStructure"
			}
		);
	}
}
