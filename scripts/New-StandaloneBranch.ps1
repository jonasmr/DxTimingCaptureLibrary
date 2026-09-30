<#
.SYNOPSIS
    Creates a new branch with no history whose single commit holds the
    contents of an existing branch.

.DESCRIPTION
    Uses the committed tree of -SourceBranch (uncommitted changes are NOT
    included) to create one parentless commit, and points -NewBranch at it.
    The working tree and the current branch are not touched, so this is safe
    to run from any checkout.

    Internal-only paths listed in -Exclude (component-governance manifest,
    internal service metadata, Azure DevOps pipelines, and this script) are
    removed from the new commit.

    The result can be pushed to a fresh remote, e.g.:
        git push origin <NewBranch>:main

.EXAMPLE
    .\scripts\New-StandaloneBranch.ps1 -SourceBranch main -NewBranch public/main `
        -Message "Initial commit" -AuthorName "Microsoft Open Source" `
        -AuthorEmail "opensource@microsoft.com"
#>
[CmdletBinding()]
param(
    [string]$SourceBranch = 'HEAD',

    [Parameter(Mandatory = $true)]
    [string]$NewBranch,

    [string]$Message = 'Initial commit',

    # Override author/committer identity; defaults to your git config.
    [string]$AuthorName,
    [string]$AuthorEmail,

    # Overwrite -NewBranch if it already exists.
    [switch]$Force,

    # Repo-relative paths (files or directories) to leave out of the new commit.
    [string[]]$Exclude = @(
        'cgmanifest.json',
        'es-metadata.yml',
        'pipelines',
        'scripts/New-StandaloneBranch.ps1'
    )
)

$ErrorActionPreference = 'Stop'

function Invoke-Git {
    $out = & git @args
    if ($LASTEXITCODE -ne 0) { throw "git $($args -join ' ') failed (exit $LASTEXITCODE)" }
    $out
}

Push-Location (Invoke-Git rev-parse --show-toplevel)
try {
    $sourceTree = Invoke-Git rev-parse --verify "$SourceBranch^{tree}"

    & git show-ref --verify --quiet "refs/heads/$NewBranch"
    if ($LASTEXITCODE -eq 0 -and -not $Force) {
        throw "Branch '$NewBranch' already exists. Use -Force to overwrite it."
    }
    if ($NewBranch -eq (& git branch --show-current)) {
        throw "Cannot overwrite the currently checked-out branch '$NewBranch'."
    }

    if (Invoke-Git status --porcelain) {
        Write-Warning "Working tree has uncommitted changes; they will NOT be included (only '$SourceBranch' as committed)."
    }

    # Filter the tree through a throwaway index so the real index and working tree stay untouched.
    $tempIndex = [IO.Path]::GetTempFileName()
    Remove-Item $tempIndex  # git wants to create the index itself; an empty file is "corrupt"
    $savedIndex = $env:GIT_INDEX_FILE
    try {
        $env:GIT_INDEX_FILE = $tempIndex
        Invoke-Git read-tree $sourceTree | Out-Null
        foreach ($path in $Exclude) {
            Invoke-Git rm -r --cached --ignore-unmatch --quiet -- $path | Out-Null
            if (-not (Invoke-Git ls-tree -r --name-only $sourceTree -- $path)) {
                Write-Warning "Exclude path '$path' not found in '$SourceBranch'."
            }
            else {
                Write-Host "Excluding $path"
            }
        }
        $tree = Invoke-Git write-tree
    }
    finally {
        $env:GIT_INDEX_FILE = $savedIndex
        Remove-Item $tempIndex -ErrorAction SilentlyContinue
    }

    $saved = @{}
    foreach ($v in 'GIT_AUTHOR_NAME', 'GIT_AUTHOR_EMAIL', 'GIT_COMMITTER_NAME', 'GIT_COMMITTER_EMAIL') {
        $saved[$v] = [Environment]::GetEnvironmentVariable($v)
    }
    try {
        if ($AuthorName)  { $env:GIT_AUTHOR_NAME  = $AuthorName;  $env:GIT_COMMITTER_NAME  = $AuthorName }
        if ($AuthorEmail) { $env:GIT_AUTHOR_EMAIL = $AuthorEmail; $env:GIT_COMMITTER_EMAIL = $AuthorEmail }
        $commit = Invoke-Git commit-tree $tree -m $Message
    }
    finally {
        foreach ($v in $saved.Keys) { [Environment]::SetEnvironmentVariable($v, $saved[$v]) }
    }

    Invoke-Git branch --force $NewBranch $commit | Out-Null

    Write-Host "Created '$NewBranch' at $commit (1 commit, no parents; tree $tree from '$SourceBranch' minus excluded paths)."
    Write-Host "Push with: git push origin ${NewBranch}:main"
}
finally {
    Pop-Location
}
