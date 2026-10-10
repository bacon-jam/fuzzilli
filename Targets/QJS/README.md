# Target: QuickJS

To build QuickJS for fuzzing:

1. Clone the QuickJS mirror from https://github.com/bellard/quickjs
2. Apply Patches/\*. The patches should apply cleanly to the git revision specified in [./REVISION](./REVISION)
3. On Linux or macOS, build QuickJS with `make qjs`
4. The `qjs` binary is the JavaScript shell for the fuzzer

## Native Windows build

Install [Swift 6.4 for Windows](https://www.swift.org/install/windows/), Visual
Studio 2022 with C++ tools, and [MSYS2](https://www.msys2.org/). In the MSYS2
UCRT64 shell, install Clang and GNU Make with
`pacman -S --needed mingw-w64-ucrt-x86_64-clang make`. The commands below use a
fresh Visual Studio Developer PowerShell, starting from the Fuzzilli repository
root. They assume per-user Swift 6.4 and MSYS2 installs in their default
locations; adjust the paths if you installed them elsewhere. They create a
sibling `quickjs` checkout:

```powershell
$swiftRoot = "$env:LOCALAPPDATA\Programs\Swift"
$env:SDKROOT = "$swiftRoot\Platforms\6.4.0\Windows.platform\Developer\SDKs\Windows.sdk"
$swiftBin = "$swiftRoot\Toolchains\6.4.0+Asserts\usr\bin"
$swiftRuntime = "$swiftRoot\Runtimes\6.4.0\usr\bin"
$env:Path = "$swiftBin;$swiftRuntime;$env:Path"

$patch = (Resolve-Path Targets\QJS\Patches\Fuzzilli-instrumentation-for-QJS.patch).Path
$revision = (Get-Content Targets\QJS\REVISION -Raw).Trim()
git clone https://github.com/bellard/quickjs.git ..\quickjs
git -C ..\quickjs checkout $revision
git -C ..\quickjs apply $patch

$env:Path = "C:\msys64\ucrt64\bin;C:\msys64\usr\bin;$env:Path"
$env:MSYSTEM = "UCRT64"
Push-Location ..\quickjs
& C:\msys64\usr\bin\make.exe CONFIG_WIN32=y CROSS_PREFIX= CONFIG_LTO= qjs.exe
Pop-Location

swift build
swift run --skip-build REPRLRun ..\quickjs\qjs.exe --reprl
swift run --skip-build FuzzilliCli --profile=qjs --maxIterations=1000 ..\quickjs\qjs.exe
```

The Windows build uses the native UCRT64 compiler (`CROSS_PREFIX=`) and disables
LTO for the COFF coverage guard layout. Keep `C:\msys64\ucrt64\bin` on `PATH`
when running Fuzzilli so Windows can load QuickJS's runtime DLLs. `REPRLRun`
should report `All tests passed!` before fuzzing.
