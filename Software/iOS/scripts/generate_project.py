"""Regenerate the dependency-free Xcode project after adding/removing Swift files."""
from pathlib import Path
import hashlib
import json

ROOT = Path(__file__).resolve().parents[1]
objects = {}


def identifier(name):
    return hashlib.sha256(name.encode()).hexdigest()[:24].upper()


def quote(value):
    return json.dumps(str(value))


def array(values):
    return "(" + ", ".join(values) + ")"


def obj(key_name, isa, **values):
    key = identifier(key_name)
    objects[key] = "{ isa = " + isa + "; " + " ".join(f"{k} = {v};" for k, v in values.items()) + " };"
    return key


def configurations(name, common):
    configurations = []
    for mode in ("Debug", "Release"):
        settings = dict(common)
        settings.update({"SWIFT_OPTIMIZATION_LEVEL": quote("-Onone" if mode == "Debug" else "-O"),
                         "DEBUG_INFORMATION_FORMAT": quote("dwarf" if mode == "Debug" else "dwarf-with-dsym")})
        # Match SwiftPM's Debug architecture for the selected device/simulator.
        # Otherwise the app can request an Intel module from an arm64-only package.
        settings["ONLY_ACTIVE_ARCH"] = "YES" if mode == "Debug" else "NO"
        if mode == "Debug":
            settings.update({"ENABLE_TESTABILITY": "YES", "SWIFT_ACTIVE_COMPILATION_CONDITIONS": quote("DEBUG $(inherited)")})
        body = "{ " + " ".join(f"{k} = {v};" for k, v in settings.items()) + " }"
        configurations.append(obj(f"{name}-{mode}", "XCBuildConfiguration", name=quote(mode), buildSettings=body))
    return obj(name + "-configuration-list", "XCConfigurationList", buildConfigurations=array(configurations),
               defaultConfigurationIsVisible="0", defaultConfigurationName="Release")


def source_group(folder):
    refs, builds = [], []
    for path in sorted((ROOT / folder).rglob("*.swift")):
        relative = path.relative_to(ROOT).as_posix()
        ref = obj(relative, "PBXFileReference", lastKnownFileType="sourcecode.swift", path=quote(relative), sourceTree=quote("<group>"))
        refs.append(ref)
        builds.append(obj(relative + "-build", "PBXBuildFile", fileRef=ref))
    group = obj(folder, "PBXGroup", children=array(refs), name=quote(folder), sourceTree=quote("<group>"))
    phase = obj(folder + "-sources", "PBXSourcesBuildPhase", buildActionMask="2147483647", files=array(builds), runOnlyForDeploymentPostprocessing="0")
    return group, phase


app_group, app_sources = source_group("App")
test_group, test_sources = source_group("AppTests")
package = obj("core-package", "XCLocalSwiftPackageReference", relativePath=quote("."))
product = obj("core-product", "XCSwiftPackageProductDependency", package=package, productName="WearableCore")


def frameworks(name):
    build = obj(name + "-core-build", "PBXBuildFile", productRef=product)
    return obj(name + "-frameworks", "PBXFrameworksBuildPhase", buildActionMask="2147483647", files=array([build]), runOnlyForDeploymentPostprocessing="0")


app_product = obj("app-product", "PBXFileReference", explicitFileType="wrapper.application", includeInIndex="0", path=quote("KappaBeta.app"), sourceTree="BUILT_PRODUCTS_DIR")
test_product = obj("test-product", "PBXFileReference", explicitFileType="wrapper.cfbundle", includeInIndex="0", path=quote("KappaBetaTests.xctest"), sourceTree="BUILT_PRODUCTS_DIR")
products = obj("products", "PBXGroup", children=array([app_product, test_product]), name="Products", sourceTree=quote("<group>"))
info = obj("info", "PBXFileReference", lastKnownFileType="text.plist.xml", path=quote("App/Info.plist"), sourceTree=quote("<group>"))
privacy = obj("privacy", "PBXFileReference", lastKnownFileType="text.xml", path=quote("App/PrivacyInfo.xcprivacy"), sourceTree=quote("<group>"))
privacy_build = obj("privacy-build", "PBXBuildFile", fileRef=privacy)
resources = obj("app-resources", "PBXResourcesBuildPhase", buildActionMask="2147483647", files=array([privacy_build]), runOnlyForDeploymentPostprocessing="0")
root_group = obj("root-group", "PBXGroup", children=array([app_group, test_group, info, privacy, products]), sourceTree=quote("<group>"))
common = {"IPHONEOS_DEPLOYMENT_TARGET": "17.0", "SWIFT_VERSION": "5.0", "SWIFT_STRICT_CONCURRENCY": "complete",
          "CLANG_ENABLE_MODULES": "YES", "SDKROOT": "iphoneos", "TARGETED_DEVICE_FAMILY": quote("1,2"),
          "SUPPORTED_PLATFORMS": quote("iphoneos iphonesimulator"), "CODE_SIGN_STYLE": "Automatic"}
app_settings = dict(common, PRODUCT_BUNDLE_IDENTIFIER=quote("org.kappabeta.wearable"), PRODUCT_NAME=quote("$(TARGET_NAME)"),
                    INFOPLIST_FILE=quote("App/Info.plist"), MARKETING_VERSION="1.0", CURRENT_PROJECT_VERSION="1",
                    GENERATE_INFOPLIST_FILE="NO", LD_RUNPATH_SEARCH_PATHS=quote("$(inherited) @executable_path/Frameworks"),
                    SUPPORTS_MACCATALYST="NO", ENABLE_USER_SCRIPT_SANDBOXING="YES")
app_target = obj("app-target", "PBXNativeTarget", buildConfigurationList=configurations("app", app_settings),
                 buildPhases=array([app_sources, frameworks("app"), resources]), buildRules="()", dependencies="()", name="KappaBeta",
                 packageProductDependencies=array([product]), productName="KappaBeta", productReference=app_product,
                 productType=quote("com.apple.product-type.application"))
proxy = obj("test-proxy", "PBXContainerItemProxy", containerPortal=identifier("project"), proxyType="1", remoteGlobalIDString=app_target, remoteInfo="KappaBeta")
dependency = obj("test-dependency", "PBXTargetDependency", target=app_target, targetProxy=proxy)
test_settings = dict(common, PRODUCT_BUNDLE_IDENTIFIER=quote("org.kappabeta.wearable.tests"), PRODUCT_NAME=quote("$(TARGET_NAME)"),
                     GENERATE_INFOPLIST_FILE="YES", BUNDLE_LOADER=quote("$(TEST_HOST)"),
                     TEST_HOST=quote("$(BUILT_PRODUCTS_DIR)/KappaBeta.app/KappaBeta"),
                     LD_RUNPATH_SEARCH_PATHS=quote("$(inherited) @executable_path/Frameworks @loader_path/Frameworks"))
test_target = obj("test-target", "PBXNativeTarget", buildConfigurationList=configurations("tests", test_settings),
                  buildPhases=array([test_sources, frameworks("tests")]), buildRules="()", dependencies=array([dependency]),
                  name="KappaBetaTests", packageProductDependencies=array([product]), productName="KappaBetaTests",
                  productReference=test_product, productType=quote("com.apple.product-type.bundle.unit-test"))
project = obj("project", "PBXProject", attributes="{ BuildIndependentTargetsInParallel = YES; LastUpgradeCheck = 1600; }",
              buildConfigurationList=configurations("project", common), compatibilityVersion=quote("Xcode 14.0"),
              developmentRegion="en", hasScannedForEncodings="0", knownRegions="(en, Base)", mainGroup=root_group,
              packageReferences=array([package]), productRefGroup=products, projectDirPath=quote(""), projectRoot=quote(""),
              targets=array([app_target, test_target]))
project_dir = ROOT / "KappaBeta.xcodeproj"
project_dir.mkdir(exist_ok=True)
body = "// !$*UTF8*$!\n{\n archiveVersion = 1;\n classes = {};\n objectVersion = 56;\n objects = {\n"
body += "\n".join(f"  {key} = {value}" for key, value in objects.items())
body += f"\n }};\n rootObject = {project};\n}}\n"
(project_dir / "project.pbxproj").write_text(body, encoding="utf-8")
scheme_dir = project_dir / "xcshareddata" / "xcschemes"
scheme_dir.mkdir(parents=True, exist_ok=True)


def buildable(target, name, product):
    return f'<BuildableReference BuildableIdentifier="primary" BlueprintIdentifier="{target}" BuildableName="{product}" BlueprintName="{name}" ReferencedContainer="container:KappaBeta.xcodeproj"/>'


app_ref = buildable(app_target, "KappaBeta", "KappaBeta.app")
test_ref = buildable(test_target, "KappaBetaTests", "KappaBetaTests.xctest")
scheme = f'''<?xml version="1.0" encoding="UTF-8"?>
<Scheme LastUpgradeVersion="1600" version="1.3">
 <BuildAction parallelizeBuildables="YES" buildImplicitDependencies="YES"><BuildActionEntries>
  <BuildActionEntry buildForTesting="YES" buildForRunning="YES" buildForProfiling="YES" buildForArchiving="YES" buildForAnalyzing="YES">{app_ref}</BuildActionEntry>
 </BuildActionEntries></BuildAction>
 <TestAction buildConfiguration="Debug" selectedDebuggerIdentifier="Xcode.DebuggerFoundation.Debugger.LLDB" selectedLauncherIdentifier="Xcode.IDEFoundation.Launcher.LLDB" shouldUseLaunchSchemeArgsEnv="YES">
  <Testables><TestableReference skipped="NO" parallelizable="NO">{test_ref}</TestableReference></Testables>
 </TestAction>
 <LaunchAction buildConfiguration="Debug" selectedDebuggerIdentifier="Xcode.DebuggerFoundation.Debugger.LLDB" selectedLauncherIdentifier="Xcode.IDEFoundation.Launcher.LLDB" launchStyle="0" useCustomWorkingDirectory="NO" ignoresPersistentStateOnLaunch="NO" debugServiceExtension="internal" allowLocationSimulation="YES"><BuildableProductRunnable runnableDebuggingMode="0">{app_ref}</BuildableProductRunnable></LaunchAction>
 <ProfileAction buildConfiguration="Release" shouldUseLaunchSchemeArgsEnv="YES" savedToolIdentifier="" useCustomWorkingDirectory="NO"><BuildableProductRunnable runnableDebuggingMode="0">{app_ref}</BuildableProductRunnable></ProfileAction>
 <AnalyzeAction buildConfiguration="Debug"/>
 <ArchiveAction buildConfiguration="Release" revealArchiveInOrganizer="YES"/>
</Scheme>
'''
(scheme_dir / "KappaBeta.xcscheme").write_text(scheme, encoding="utf-8")
print(f"Generated {project_dir.name}: {len(objects)} objects")
