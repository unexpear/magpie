# build.ps1 - compile magpie.exe with gcc. No make required.
#
# Needs: gcc on PATH (MSYS2 UCRT64 is what this was developed against) and
# vendor/ populated by .\fetch-deps.ps1
#
# HTTP comes from WinHTTP, which ships with Windows - nothing to install and
# nothing to redistribute. See decisions.md D4.

param(
    [switch]$Debug,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

$out = Join-Path $root 'magpie.exe'
$obj = Join-Path $root 'build'

if ($Clean) {
    if (Test-Path $obj) { Remove-Item -Recurse -Force $obj }
    if (Test-Path $out) { Remove-Item -Force $out }
    Write-Host 'cleaned'
    return
}

if (-not (Get-Command gcc -ErrorAction SilentlyContinue)) {
    throw 'gcc not found on PATH. Install MSYS2 and use the UCRT64 shell, or add C:\msys64\ucrt64\bin to PATH.'
}
foreach ($f in 'sqlite3.c', 'yyjson.c') {
    if (-not (Test-Path (Join-Path $root "vendor\$f"))) {
        throw "vendor\$f missing. Run .\fetch-deps.ps1 first."
    }
}

New-Item -ItemType Directory -Force -Path $obj | Out-Null

$sources = @(
    'src\main.c'
    'src\util.c'
    'src\asset.c'
    'src\classify.c'
    'src\jsonutil.c'
    'src\http.c'
    'src\limiter.c'
    'src\store.c'
    'src\fetch.c'
    'src\robots.c'
    'src\adapter.c'
    'src\adapters\polyhaven.c'
    'src\adapters\ambientcg.c'
    'vendor\sqlite3.c'
    'vendor\yyjson.c'
)

$cflags = @('-std=c99', '-Isrc', '-Ivendor', '-Wall', '-Wextra', '-Wno-unused-parameter')
if ($Debug) { $cflags += @('-g', '-O0', '-DDEBUG') }
else        { $cflags += @('-O2') }

# FTS5 is the whole search feature; it is off by default in the amalgamation.
$cflags += @(
    '-DSQLITE_ENABLE_FTS5'
    '-DSQLITE_THREADSAFE=0'
    '-DSQLITE_DEFAULT_MEMSTATUS=0'
    '-DSQLITE_OMIT_DEPRECATED'
)

# Newest header timestamp. Without this the script only ever compared each .c
# against its own .o, so editing a header changed nothing: files that merely
# *include* it kept their stale objects. Adding a field to the asset struct
# that way produced two different struct layouts in one binary and a heap
# corruption crash that looked nothing like a build problem.
$newestHeader = Get-ChildItem -Path (Join-Path $root 'src') -Filter *.h -Recurse |
                Sort-Object LastWriteTime -Descending |
                Select-Object -First 1

# Compiler flags are dependencies too: switching Debug/Release must rebuild.
$flagStamp = Join-Path $obj '.cflags'
$flagText = $cflags -join ' '
$flagsChanged = -not (Test-Path -LiteralPath $flagStamp)
if (-not $flagsChanged) {
    $flagsChanged = ([IO.File]::ReadAllText($flagStamp) -ne $flagText)
}

$objs = @()
foreach ($src in $sources) {
    $name = [IO.Path]::GetFileNameWithoutExtension($src)
    $o    = Join-Path $obj "$name.o"
    $objs += $o

    $newestDep = (Get-Item $src).LastWriteTime
    # Vendored blobs do not include our headers; sqlite3.c alone is most of the
    # build time, so leave it out of the header dependency.
    if (($src -notlike 'vendor\*') -and $newestHeader -and
        ($newestHeader.LastWriteTime -gt $newestDep)) {
        $newestDep = $newestHeader.LastWriteTime
    }

    if (-not $flagsChanged -and (Test-Path $o) -and ((Get-Item $o).LastWriteTime -gt $newestDep)) {
        continue
    }

    Write-Host "  cc $src"
    $flags = $cflags
    if ($src -like 'vendor\*') {
        # Not our code, not our warnings.
        $flags = $cflags | Where-Object { $_ -notlike '-W*' }
    }
    & gcc @flags -c $src -o $o
    if ($LASTEXITCODE -ne 0) { throw "compile failed: $src" }
}

Write-Host '  link magpie.exe'
& gcc @objs -o $out -lwinhttp -lshell32
if ($LASTEXITCODE -ne 0) { throw 'link failed' }

[IO.File]::WriteAllText($flagStamp, $flagText)
Write-Host ''
Write-Host "built $out"
Write-Host 'try:  .\magpie.exe crawl'
