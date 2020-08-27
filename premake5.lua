function os.winSdkVersion()
	local reg_arch = iif( os.is64bit(), "\\Wow6432Node\\", "\\" )
	local sdk_version = os.getWindowsRegistry( "HKLM:SOFTWARE" .. reg_arch .."Microsoft\\Microsoft SDKs\\Windows\\v10.0\\ProductVersion" )
	if sdk_version ~= nil then return sdk_version end
	return ""
end

function symbols_on()
	if symbols ~= nil then
		symbols "On"
	else
		flags { "Symbols" }
	end
end

workspace "sp-tools"
	configurations { "debug", "develop", "release" }
	platforms { "x86", "x64" }

	location "proj"
	includedirs { "ext" }
	targetdir "build/%{cfg.buildcfg}_%{cfg.platform}"
    startproject "model"
	flags { "MultiProcessorCompile" }
	staticruntime "on"
	exceptionhandling "Off"
	rtti "Off"

	cppdialect "C++14"

	-- WTF: cppdialect seems broken on macOS
	if os.host() == "macosx" then
		flags { "C++14" }
	end

	filter { "system:linux" }
		linkoptions "-pthread"
		toolset "clang"

	filter "action:vs*"
		systemversion(os.winSdkVersion() .. ".0")

	filter "not action:vs*"
		buildoptions { "-Wno-invalid-offsetof" }
		buildoptions { "-Wno-switch" }

	filter "configurations:debug"
		defines { "DEBUG" }
		symbols_on()

	filter "configurations:develop"
		defines { "NDEBUG" }
		optimize "On"
		symbols_on()

	filter "configurations:release"
		defines { "NDEBUG" }
		optimize "On"
        flags { "LinkTimeOptimization" }

	filter "platforms:x86"
		architecture "x86"

	filter "platforms:x64"
		architecture "x86_64"

	filter "options:opengl"
		defines { "SP_USE_OPENGL=1" }

project "sp-common"
	kind "StaticLib"
	language "C++"
    files { "ext/**.h", "ext/**.c", "ext/**.cpp" }
    files { "misc/*.natvis" }

project "sp-texcomp"
	kind "ConsoleApp"
	language "C++"
	links { "sp-common" }
    files { "texcomp/**.h", "texcomp/**.c", "texcomp/**.cpp" }
	debugdir "."

project "sp-model"
	kind "ConsoleApp"
	language "C++"
	links { "sp-common" }
    files { "model/**.h", "model/**.c", "model/**.cpp" }
	debugdir "."

project "sp-sound"
	kind "ConsoleApp"
	language "C++"
	links { "sp-common" }
    files { "sound/**.h", "sound/**.c", "sound/**.cpp" }
	debugdir "."

