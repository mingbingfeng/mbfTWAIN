#define MyAppName "mbfTwain"
#ifndef MyAppVersion
#define MyAppVersion "0.0.0"
#endif
#ifndef MyAppVersionFourPart
#define MyAppVersionFourPart "0.0.0.0"
#endif
#ifndef RepoRoot
#define RepoRoot ".."
#endif

[Setup]
AppId={{5BAE1230-87E5-49C8-B3EB-3845B7C3B314}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=mingbingfeng
AppPublisherURL=https://github.com/mingbingfeng/mbfTWAIN
AppSupportURL=https://github.com/mingbingfeng/mbfTWAIN/issues
AppUpdatesURL=https://github.com/mingbingfeng/mbfTWAIN/releases/latest
DefaultDirName={autopf}\mbfTwain
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#RepoRoot}\build\release
OutputBaseFilename=mbfTwain-Setup-v{#MyAppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
CloseApplicationsFilter=mbfTwain.VirtualScannerConfig.exe
SetupLogging=yes
UninstallDisplayName=mbfTwain Virtual TWAIN Scanner
VersionInfoVersion={#MyAppVersionFourPart}
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}
VersionInfoCompany=mingbingfeng

[Dirs]
Name: "{win}\twain_32"
Name: "{win}\twain_64"; Check: IsWin64

[Files]
Source: "{#RepoRoot}\build\manual\Win32\Release\mbfVirtualTwainDS.ds"; DestDir: "{win}\twain_32"; Flags: ignoreversion
Source: "{#RepoRoot}\build\manual\Win32\Release\mbfTwain.VirtualScannerConfig.exe"; DestDir: "{win}\twain_32"; Flags: ignoreversion
Source: "{#RepoRoot}\build\manual\Win32\Release\mbfTwain.VirtualScannerConfig.dll"; DestDir: "{win}\twain_32"; Flags: ignoreversion
Source: "{#RepoRoot}\build\manual\Win32\Release\mbfTwain.VirtualScannerConfig.deps.json"; DestDir: "{win}\twain_32"; Flags: ignoreversion
Source: "{#RepoRoot}\build\manual\Win32\Release\mbfTwain.VirtualScannerConfig.runtimeconfig.json"; DestDir: "{win}\twain_32"; Flags: ignoreversion
Source: "{#RepoRoot}\build\manual\x64\Release\mbfVirtualTwainDS.ds"; DestDir: "{win}\twain_64"; Flags: ignoreversion; Check: IsWin64
Source: "{#RepoRoot}\build\manual\x64\Release\mbfTwain.VirtualScannerConfig.exe"; DestDir: "{win}\twain_64"; Flags: ignoreversion; Check: IsWin64
Source: "{#RepoRoot}\build\manual\x64\Release\mbfTwain.VirtualScannerConfig.dll"; DestDir: "{win}\twain_64"; Flags: ignoreversion; Check: IsWin64
Source: "{#RepoRoot}\build\manual\x64\Release\mbfTwain.VirtualScannerConfig.deps.json"; DestDir: "{win}\twain_64"; Flags: ignoreversion; Check: IsWin64
Source: "{#RepoRoot}\build\manual\x64\Release\mbfTwain.VirtualScannerConfig.runtimeconfig.json"; DestDir: "{win}\twain_64"; Flags: ignoreversion; Check: IsWin64

[Registry]
Root: HKLM; Subkey: "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"; ValueType: string; ValueName: "MBF_TWAIN_FORCE_UI"; ValueData: "1"; Flags: preservestringtype uninsdeletevalue

[InstallDelete]
Type: files; Name: "{win}\twain_32\mbfVirtualTwainDS.ds"
Type: files; Name: "{win}\twain_32\mbfTwain.VirtualScannerConfig.exe"
Type: files; Name: "{win}\twain_32\mbfTwain.VirtualScannerConfig.dll"
Type: files; Name: "{win}\twain_32\mbfTwain.VirtualScannerConfig.deps.json"
Type: files; Name: "{win}\twain_32\mbfTwain.VirtualScannerConfig.runtimeconfig.json"
Type: files; Name: "{win}\twain_64\mbfVirtualTwainDS.ds"; Check: IsWin64
Type: files; Name: "{win}\twain_64\mbfTwain.VirtualScannerConfig.exe"; Check: IsWin64
Type: files; Name: "{win}\twain_64\mbfTwain.VirtualScannerConfig.dll"; Check: IsWin64
Type: files; Name: "{win}\twain_64\mbfTwain.VirtualScannerConfig.deps.json"; Check: IsWin64
Type: files; Name: "{win}\twain_64\mbfTwain.VirtualScannerConfig.runtimeconfig.json"; Check: IsWin64

[UninstallDelete]
Type: files; Name: "{win}\twain_32\mbfVirtualTwainDS.ds"
Type: files; Name: "{win}\twain_32\mbfTwain.VirtualScannerConfig.exe"
Type: files; Name: "{win}\twain_32\mbfTwain.VirtualScannerConfig.dll"
Type: files; Name: "{win}\twain_32\mbfTwain.VirtualScannerConfig.deps.json"
Type: files; Name: "{win}\twain_32\mbfTwain.VirtualScannerConfig.runtimeconfig.json"
Type: files; Name: "{win}\twain_64\mbfVirtualTwainDS.ds"; Check: IsWin64
Type: files; Name: "{win}\twain_64\mbfTwain.VirtualScannerConfig.exe"; Check: IsWin64
Type: files; Name: "{win}\twain_64\mbfTwain.VirtualScannerConfig.dll"; Check: IsWin64
Type: files; Name: "{win}\twain_64\mbfTwain.VirtualScannerConfig.deps.json"; Check: IsWin64
Type: files; Name: "{win}\twain_64\mbfTwain.VirtualScannerConfig.runtimeconfig.json"; Check: IsWin64
