#requires -Version 5.1
<#
.SYNOPSIS
Creates a source-only snapshot for review before uploading to GitHub.
.DESCRIPTION
Copies the current working files, including untracked files, from an explicit
allowlist. No Git command runs and the source tree is never modified. Each run
creates a new timestamped directory containing ServerCore/, ServerCore.zip,
source-manifest.json, and ServerCore.zip.sha256. Failed runs are left for inspection.
.PARAMETER Destination
An absolute parent directory for the new export. Defaults to Builds/GitHub below
the repository. Inside the repository, only a destination below Builds is allowed.
.EXAMPLE
.\scripts\PrepareGitHubSource.ps1
.EXAMPLE
.\scripts\PrepareGitHubSource.ps1 -Destination C:\Exports
#>
[CmdletBinding()]
param(
    [string]$Destination
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Test-PathWithin {
    param([string]$Path, [string]$Directory)
    $baseDirectory = $Directory.TrimEnd([char[]]'\/')
    $prefix = $baseDirectory + [IO.Path]::DirectorySeparatorChar
    return $Path.TrimEnd([char[]]'\/').Equals($baseDirectory, [StringComparison]::OrdinalIgnoreCase) -or
        $Path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)
}

function Assert-NoReparseAncestors {
    param([string]$Path)
    $current = $Path
    while ($current) {
        $attributes = $null
        try { $attributes = [IO.File]::GetAttributes($current) }
        catch [IO.FileNotFoundException] { }
        catch [IO.DirectoryNotFoundException] { }
        if ($null -ne $attributes -and
            ($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Symbolic links and reparse points are not allowed: $current"
        }
        $parent = [IO.Path]::GetDirectoryName($current)
        if ($parent -eq $current) { break }
        $current = $parent
    }
}

function Test-ExcludedEntry {
    param([IO.FileSystemInfo]$Item, [string]$RelativePath)
    if ($Item.Name.StartsWith('.')) { return $true }
    if (($Item.Attributes -band [IO.FileAttributes]::Hidden) -ne 0) { return $true }
    if ($Item -is [IO.DirectoryInfo]) {
        return $Item.Name -match '^(docs|build|builds|out|stage|target|bin|obj|Debug|Release|RelWithDebInfo|MinSizeRel|CMakeFiles|Testing|x64.*|cmake-build-.*)$'
    }
    # README is included explicitly from RootFiles. Keep explanatory documents
    # out of the recursive source inventory; CMakeLists and test vectors remain.
    if ($Item.Extension -match '^\.(md|rst|adoc)$') { return $true }
    if ($Item.Name -in @('CMakeUserPresets.json', 'CMakeCache.txt', 'cmake_install.cmake',
            'CTestTestfile.cmake', 'Makefile', 'build.ninja', 'rules.ninja',
            'compile_commands.json', 'Thumbs.db', 'Desktop.ini')) { return $true }
    return $Item.Name -match '\.(exe|dll|lib|obj|o|a|so|dylib|pch|pdb|idb|ilk|iobj|ipdb|tlog|recipe|lastbuildstate|log|suo|user|db|opendb|sln|slnx|vcxproj|filters|zip|7z|rar|tar|gz)$'
}

function Get-SnapshotPaths {
    $paths = New-Object 'System.Collections.Generic.List[string]'
    foreach ($relativePath in $script:RootFiles) {
        $fullPath = Join-Path $script:RepositoryRoot $relativePath
        Assert-NoReparseAncestors $fullPath
        if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
            throw "Required public source file is missing: $relativePath"
        }
        $paths.Add($relativePath)
    }
    $pending = New-Object 'System.Collections.Generic.Stack[string]'
    foreach ($relativePath in $script:SourceDirectories) {
        $fullPath = Join-Path $script:RepositoryRoot $relativePath
        Assert-NoReparseAncestors $fullPath
        if (-not (Test-Path -LiteralPath $fullPath -PathType Container)) {
            throw "Required public source directory is missing: $relativePath"
        }
        $pending.Push($relativePath)
    }
    while ($pending.Count -gt 0) {
        $relativeDirectory = $pending.Pop()
        $fullDirectory = Join-Path $script:RepositoryRoot $relativeDirectory
        Assert-NoReparseAncestors $fullDirectory
        foreach ($item in Get-ChildItem -LiteralPath $fullDirectory -Force) {
            $relativePath = $relativeDirectory + '/' + $item.Name
            # Check links before exclusions so an excluded link cannot hide an unexpected boundary.
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Symbolic links and reparse points are not allowed: $relativePath"
            }
            if (Test-ExcludedEntry $item $relativePath) { continue }
            if ($item -is [IO.DirectoryInfo]) { $pending.Push($relativePath) }
            else { $paths.Add($relativePath) }
        }
    }
    $ordered = $paths.ToArray()
    [Array]::Sort($ordered, [StringComparer]::Ordinal)
    return $ordered
}

function Get-FileSha256 {
    param([string]$Path)
    $stream = $null
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $stream = [IO.File]::Open($Path, [IO.FileMode]::Open,
            [IO.FileAccess]::Read, [IO.FileShare]::Read)
        return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()
    }
    finally {
        if ($stream) { $stream.Dispose() }
        $algorithm.Dispose()
    }
}

function Write-NewUtf8File {
    param([string]$Path, [string]$Text)
    $stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew,
        [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $encoding = New-Object Text.UTF8Encoding($false)
        $bytes = $encoding.GetBytes($Text)
        $stream.Write($bytes, 0, $bytes.Length)
    }
    finally { $stream.Dispose() }
}

$script:RepositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$script:RootFiles = @('.clang-format', '.gitattributes', '.gitignore',
    '.github/workflows/build.yml', 'CMakeLists.txt', 'CMakePresets.json',
    'LICENSE', 'README.md', 'scripts/VerifyBuild.ps1', 'scripts/PrepareGitHubSource.ps1',
    'rust/.cargo/config.toml')
$script:SourceDirectories = @('include', 'src', 'tests', 'cmake', 'rust')

Assert-NoReparseAncestors $script:RepositoryRoot
if ($PSBoundParameters.ContainsKey('Destination')) {
    if ([string]::IsNullOrWhiteSpace($Destination) -or
        $Destination -match '^\\\\[?.][\\/]' -or
        $Destination -notmatch '^(?:[A-Za-z]:[\\/]|\\\\[^\\/]+[\\/][^\\/]+(?:[\\/]|$))') {
        throw '-Destination must be an absolute drive or UNC directory path.'
    }
    $destinationRoot = [IO.Path]::GetFullPath($Destination)
}
else { $destinationRoot = Join-Path $script:RepositoryRoot 'Builds/GitHub' }

$buildsRoot = Join-Path $script:RepositoryRoot 'Builds'
if ((Test-PathWithin $destinationRoot $script:RepositoryRoot) -and
    -not (Test-PathWithin $destinationRoot $buildsRoot)) {
    throw 'An export inside the repository must be below Builds, outside every source directory.'
}
Assert-NoReparseAncestors $destinationRoot
if ((Test-Path -LiteralPath $destinationRoot) -and
    -not (Test-Path -LiteralPath $destinationRoot -PathType Container)) {
    throw "Destination is an existing file: $destinationRoot"
}

# Complete the inventory before creating any output, including required files and link checks.
$paths = @(Get-SnapshotPaths)
if ('src/Runtime/ServerHost.cpp' -notin $paths) {
    throw 'The source inventory must include src/Runtime/ServerHost.cpp.'
}
Add-Type -AssemblyName System.IO.Compression
$createdUtc = [DateTimeOffset]::UtcNow
$runName = $createdUtc.ToString('yyyyMMdd-HHmmss-fffZ') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$runDirectory = Join-Path $destinationRoot $runName
if (Test-Path -LiteralPath $runDirectory) { throw "Export already exists: $runDirectory" }
[void][IO.Directory]::CreateDirectory($destinationRoot)
Assert-NoReparseAncestors $destinationRoot
# New-Item without Force refuses an existing run directory. Every output file also uses CreateNew.
[void](New-Item -ItemType Directory -Path $runDirectory)
$snapshotRoot = Join-Path $runDirectory 'ServerCore'
$archivePath = Join-Path $runDirectory 'ServerCore.zip'
$manifestPath = Join-Path $runDirectory 'source-manifest.json'
$hashPath = Join-Path $runDirectory 'ServerCore.zip.sha256'

try {
    [void][IO.Directory]::CreateDirectory($snapshotRoot)
    $entries = New-Object 'System.Collections.Generic.List[object]'
    [long]$totalBytes = 0
    foreach ($relativePath in $paths) {
        $sourcePath = Join-Path $script:RepositoryRoot $relativePath
        $targetPath = [IO.Path]::GetFullPath((Join-Path $snapshotRoot $relativePath))
        if (-not (Test-PathWithin $targetPath $snapshotRoot)) {
            throw "An export path escaped its source folder: $relativePath"
        }
        Assert-NoReparseAncestors $sourcePath
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($targetPath))
        $inputStream = $null
        $outputStream = $null
        try {
            $inputStream = [IO.File]::Open($sourcePath, [IO.FileMode]::Open,
                [IO.FileAccess]::Read, [IO.FileShare]::Read)
            $outputStream = [IO.File]::Open($targetPath, [IO.FileMode]::CreateNew,
                [IO.FileAccess]::Write, [IO.FileShare]::None)
            $inputStream.CopyTo($outputStream)
            $size = $inputStream.Length
        }
        finally {
            if ($outputStream) { $outputStream.Dispose() }
            if ($inputStream) { $inputStream.Dispose() }
        }
        $entries.Add([pscustomobject][ordered]@{
            path = $relativePath
            size = $size
            sha256 = Get-FileSha256 $targetPath
        })
        $totalBytes += $size
    }

    # Detect additions, removals, and edits during copying instead of publishing a mixed snapshot.
    $currentPaths = @(Get-SnapshotPaths)
    if (($paths -join "`n") -cne ($currentPaths -join "`n")) {
        throw 'The public source file list changed while exporting. Run again after edits finish.'
    }
    foreach ($entry in $entries) {
        $sourcePath = Join-Path $script:RepositoryRoot $entry.path
        Assert-NoReparseAncestors $sourcePath
        if ((Get-FileSha256 $sourcePath) -ne $entry.sha256) {
            throw "A source file changed while exporting: $($entry.path)"
        }
    }

    $archiveStream = $null
    $archive = $null
    try {
        $archiveStream = [IO.File]::Open($archivePath, [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write, [IO.FileShare]::None)
        $archive = New-Object IO.Compression.ZipArchive($archiveStream, [IO.Compression.ZipArchiveMode]::Create, $true)
        foreach ($entry in $entries) {
            # Explicit entries include the allowlisted dotfiles that Compress-Archive can omit.
            $zipEntry = $archive.CreateEntry('ServerCore/' + $entry.path,
                [IO.Compression.CompressionLevel]::Optimal)
            $zipEntry.LastWriteTime = $createdUtc
            $inputStream = $null
            $outputStream = $null
            try {
                $inputStream = [IO.File]::OpenRead((Join-Path $snapshotRoot $entry.path))
                $outputStream = $zipEntry.Open()
                $inputStream.CopyTo($outputStream)
            }
            finally {
                if ($outputStream) { $outputStream.Dispose() }
                if ($inputStream) { $inputStream.Dispose() }
            }
        }
    }
    finally {
        if ($archive) { $archive.Dispose() }
        if ($archiveStream) { $archiveStream.Dispose() }
    }
    $archiveHash = Get-FileSha256 $archivePath
    Write-NewUtf8File $hashPath ($archiveHash + '  ServerCore.zip' + "`n")
    $manifest = [ordered]@{
        schemaVersion = 1
        createdUtc = $createdUtc.ToString('o')
        sourceDirectory = 'ServerCore'
        fileCount = $entries.Count
        totalBytes = $totalBytes
        archive = [ordered]@{ path = 'ServerCore.zip'; sha256 = $archiveHash }
        files = $entries.ToArray()
    }
    Write-NewUtf8File $manifestPath (($manifest | ConvertTo-Json -Depth 6) + "`n")
    [pscustomobject]@{
        Directory = $runDirectory
        Source = $snapshotRoot
        Archive = $archivePath
        Manifest = $manifestPath
        ArchiveHash = $hashPath
        Files = $entries.Count
        Bytes = $totalBytes
    }
}
catch {
    Write-Warning "Export incomplete; partial files remain at $runDirectory. No existing files were overwritten."
    throw
}
