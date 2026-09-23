// Copyright Jad Deeb. All Rights Reserved.

using UnrealBuildTool;

public class PanoCapture : ModuleRules
{
	public PanoCapture(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });

		PrivateDependencyModuleNames.AddRange(new string[] { "RenderCore", "ImageCore", "ImageWrapper", "Json", "Slate", "SlateCore", "Projects", "HTTPServer" });
	}
}
