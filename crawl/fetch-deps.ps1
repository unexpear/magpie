# fetch-deps.ps1 - download the two vendored single-file libraries.
#
#   sqlite3  public domain  (amalgamation: sqlite3.c + sqlite3.h)
#   yyjson   MIT            (yyjson.c + yyjson.h)
#
# Both are permissive, both are one .c file, neither needs a package manager.
# Run once. vendor/ is gitignored - this script is the record of what goes in it.

$ErrorActionPreference = 'Stop'
$root   = Split-Path -Parent $MyInvocation.MyCommand.Path
$vendor = Join-Path $root 'vendor'

New-Item -ItemType Directory -Force -Path $vendor | Out-Null

function Need($name) { Test-Path (Join-Path $vendor $name) }

# --- yyjson ---------------------------------------------------------------
if ((Need 'yyjson.c') -and (Need 'yyjson.h')) {
    Write-Host 'yyjson    already present'
} else {
    Write-Host 'yyjson    downloading...'
    $base = 'https://raw.githubusercontent.com/ibireme/yyjson/master/src'
    Invoke-WebRequest "$base/yyjson.c" -OutFile (Join-Path $vendor 'yyjson.c')
    Invoke-WebRequest "$base/yyjson.h" -OutFile (Join-Path $vendor 'yyjson.h')
    Write-Host 'yyjson    ok'
}

# --- sqlite ---------------------------------------------------------------
# The amalgamation URL embeds the version, so read it off the download page's
# machine-readable PRODUCT lines rather than hardcoding a version that rots.
if ((Need 'sqlite3.c') -and (Need 'sqlite3.h')) {
    Write-Host 'sqlite3   already present'
} else {
    Write-Host 'sqlite3   resolving current amalgamation...'
    $page = (Invoke-WebRequest 'https://sqlite.org/download.html' -UseBasicParsing).Content
    $m = [regex]::Match($page, 'PRODUCT,[\d.]+,(\d{4}/sqlite-amalgamation-\d+\.zip)')
    if (-not $m.Success) {
        throw "Could not find the amalgamation link on sqlite.org/download.html. Download it manually into $vendor"
    }

    $url = "https://sqlite.org/$($m.Groups[1].Value)"
    Write-Host "sqlite3   downloading $url"

    $zip = Join-Path $env:TEMP 'sqlite-amalgamation.zip'
    $tmp = Join-Path $env:TEMP 'sqlite-amalgamation'
    Invoke-WebRequest $url -OutFile $zip
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    Expand-Archive $zip -DestinationPath $tmp

    foreach ($f in 'sqlite3.c', 'sqlite3.h') {
        $src = Get-ChildItem -Path $tmp -Filter $f -Recurse | Select-Object -First 1
        if (-not $src) { throw "$f missing from the archive" }
        Copy-Item $src.FullName (Join-Path $vendor $f) -Force
    }
    Remove-Item -Recurse -Force $tmp
    Remove-Item -Force $zip
    Write-Host 'sqlite3   ok'
}

Write-Host ''
Write-Host 'deps ready. next:  .\build.ps1'
