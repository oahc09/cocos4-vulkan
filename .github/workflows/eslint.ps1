#Requires -Version "6.1"

$ErrorActionPreference = "Stop"
$diffJson = & node "$PSScriptRoot/get-eslint-files.js"
if ($LASTEXITCODE -ne 0) {
    throw "Failed to determine the files to lint."
}
$diffFiles = $diffJson | ConvertFrom-Json
Write-Host "Diff files: $diffFiles"

$lintFailed = $false
foreach ($file in $diffFiles) {
    $extension = Split-Path -Extension -Path $file
    if ($extension -ne ".ts" -or -not ($file -match "^(cocos|exports|pal)/")) {
        Write-Host "Skip $file"
        continue
    }
    Write-Host "ESLint check $file"
    & npx eslint $file
    if ($LASTEXITCODE -ne 0) {
        $lintFailed = $true
    }
}
if ($lintFailed) {
    throw "ESLint failed for one or more changed files."
}
