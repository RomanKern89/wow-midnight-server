<#
.SYNOPSIS
    Install a World of Warcraft retail client and point it at your private
    server.

.DESCRIPTION
    Does the three things that otherwise go wrong by hand:
      1. installs a retail client,
      2. compares the build with the one your server expects BEFORE the
         download, so you are not told after ~100 GB that it is the wrong one,
      3. writes the one Config.wtf line that redirects login to your server.

    IT CANNOT INSTALL AN OLD BUILD. Battle.Net-Installer drives Blizzard's
    Agent, and the Agent only ever fetches whatever is live right now - there is
    no argument for a specific build, and Blizzard does not serve arbitrary past
    builds. If your server expects an older build than Blizzard is shipping
    today, your real options are to move the server to the current build, or to
    keep an existing install of the old one and re-run this with -SkipInstall.

    The client is downloaded from Blizzard by Blizzard's own Battle.net Agent.
    This script does not, and cannot, give you a client you do not already have
    the right to download - you need a Battle.net account, and the app must be
    installed and logged in at least once.

    Nothing is downloaded or executed behind your back: the two third-party
    tools are located, not fetched, and the script tells you exactly where to
    get them if they are missing.

.PARAMETER InstallDir
    Where the client goes, e.g. C:\Games\WoW_Midnight

.PARAMETER ServerIP
    Your server's address, e.g. 192.168.1.50 or wow.example.com

.PARAMETER Locale
    Asset language: enUS, ruRU, deDE, frFR, esES, ...

.PARAMETER Build
    The build your server expects. Default 12.0.7.68275. This is used to CHECK
    what you get, not to select it - see the warning above.

.PARAMETER Product
    TACT product code. Default 'wow' (retail).

.PARAMETER SkipInstall
    Client already installed - only verify the build and set the portal.

.EXAMPLE
    .\setup-client.ps1
    Asks for everything it needs.

.EXAMPLE
    .\setup-client.ps1 -InstallDir C:\Games\WoW -ServerIP 192.168.1.50 -Locale enUS

.EXAMPLE
    .\setup-client.ps1 -InstallDir C:\Games\WoW -ServerIP 192.168.1.50 -SkipInstall
    Re-point an existing client at a different server.
#>
[CmdletBinding()]
param(
    [string] $InstallDir,
    [string] $ServerIP,
    [string] $Locale  = 'enUS',
    [string] $Build   = '12.0.7.68275',
    [string] $Product = 'wow',
    [switch] $SkipInstall
)

$ErrorActionPreference = 'Stop'

$BNET_INSTALLER_UPSTREAM = 'https://github.com/barncastle/Battle.Net-Installer'
$BNET_INSTALLER_FORK     = 'https://github.com/xCortlandx/Battle.Net-Installer'
$ARCTIUM                 = 'https://github.com/Arctium/WoW-Launcher'

function Step  ($n, $m) { Write-Host "`n[$n] $m" -ForegroundColor Cyan }
function Ok    ($m)     { Write-Host "    OK  $m" -ForegroundColor Green }
function Warn  ($m)     { Write-Host "    !   $m" -ForegroundColor Yellow }
function Fail  ($m)     { Write-Host "`nSTOP: $m" -ForegroundColor Red; exit 1 }

# Prompts, but survives a non-interactive host: falls back to the default, and
# only gives up when there is no default to fall back to.
function Ask ($prompt, $default) {
    try {
        if ($default) {
            $v = Read-Host "$prompt [$default]"
            if ([string]::IsNullOrWhiteSpace($v)) { return $default }
            return $v
        }
        return (Read-Host $prompt)
    } catch {
        if ($default) {
            Write-Host "    (no console - using '$default')" -ForegroundColor DarkGray
            return $default
        }
        Fail "'$prompt' is required, and this session cannot prompt. Pass it as a parameter instead."
    }
}

# A pause that simply does not pause when nobody is watching.
function Pause-For ($message) {
    try { Read-Host $message | Out-Null }
    catch { Write-Host '    (no console - continuing)' -ForegroundColor DarkGray }
}

# What build is Blizzard shipping for this product right now? This is the build
# an install will actually produce - the Agent has no way to fetch an older one.
# Format: Region|BuildConfig|CDNConfig|KeyRing|BuildId|VersionsName|ProductConfig
function Get-LiveBuild ($product, $region = 'us') {
    try {
        $r = Invoke-WebRequest -Uri "http://us.patch.battle.net:1119/$product/versions" `
                               -UseBasicParsing -TimeoutSec 20
        foreach ($line in ($r.Content -split "`r?`n")) {
            if ($line -match "^$region\|") {
                $f = $line -split '\|'
                if ($f.Count -ge 6) { return $f[5].Trim() }
            }
        }
    } catch { return $null }
    return $null
}

# Look for a tool next to this script, in the install dir, or on PATH.
function Find-Tool ($names, $extraDirs) {
    foreach ($n in $names) {
        $c = Get-Command $n -ErrorAction SilentlyContinue
        if ($c) { return $c.Source }
        foreach ($d in $extraDirs) {
            if (-not $d) { continue }
            $p = Join-Path $d $n
            if (Test-Path -LiteralPath $p) { return (Resolve-Path -LiteralPath $p).Path }
        }
    }
    return $null
}

Write-Host @"
=====================================================================
 WoW private-server client setup
 Build $Build   |   product '$Product'
=====================================================================
"@ -ForegroundColor Magenta

# ---------------------------------------------------------------- input
if (-not $InstallDir) { $InstallDir = Ask 'Install directory' 'C:\Games\WoW_Midnight' }
if (-not $ServerIP)   { $ServerIP   = Ask 'Server address (IP or hostname)' '' }
if ([string]::IsNullOrWhiteSpace($ServerIP)) { Fail 'A server address is required.' }
if (-not $PSBoundParameters.ContainsKey('Locale')) { $Locale = Ask 'Asset language' $Locale }

$ServerIP = $ServerIP.Trim()
if ($ServerIP -match '[\s"]') { Fail "Server address contains whitespace or quotes: '$ServerIP'" }

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

# ------------------------------------------------------- 1. install client
if (-not $SkipInstall) {
    Step 1 'Checking prerequisites'

    $dotnet = Get-Command dotnet -ErrorAction SilentlyContinue
    if ($dotnet) {
        $runtimes = & dotnet --list-runtimes 2>$null
        if ($runtimes -match 'Microsoft\.WindowsDesktop\.App 8\.|Microsoft\.NETCore\.App 8\.') { Ok '.NET 8 runtime present' }
        else { Warn '.NET 8 runtime not detected - Battle.Net-Installer needs it (https://dotnet.microsoft.com/download)' }
    } else {
        Warn 'dotnet not found - Battle.Net-Installer needs the .NET 8 runtime (https://dotnet.microsoft.com/download)'
    }

    $bnetApp = @(
        "$env:ProgramFiles(x86)\Battle.net\Battle.net.exe",
        "$env:ProgramFiles\Battle.net\Battle.net.exe"
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
    if ($bnetApp) { Ok 'Battle.net app found' }
    else { Warn 'Battle.net app not found. It must be installed and logged in once - the Agent does the actual download.' }

    Step 2 'Locating Battle.Net-Installer'
    $installer = Find-Tool @('BNetInstaller.exe','Battle.Net-Installer.exe') @($scriptDir, (Join-Path $scriptDir 'tools'), $PWD.Path)
    if (-not $installer) {
        Write-Host @"

    Battle.Net-Installer was not found.

    Download it and put BNetInstaller.exe next to this script (or in .\tools\):
      upstream : $BNET_INSTALLER_UPSTREAM
      fork     : $BNET_INSTALLER_FORK
                 ^ use this one if you hit Agent error 2310; the upstream
                   release has a missing User-Agent that breaks the download.

"@ -ForegroundColor Yellow
        Pause-For '    Press Enter once the file is in place (Ctrl+C to abort)'
        $installer = Find-Tool @('BNetInstaller.exe','Battle.Net-Installer.exe') @($scriptDir, (Join-Path $scriptDir 'tools'), $PWD.Path)
        if (-not $installer) { Fail 'Still not found. Place BNetInstaller.exe beside this script and re-run.' }
    }
    Ok "Using $installer"

    # The app fights the Agent for the same install if it is running.
    $running = Get-Process -Name 'Battle.net' -ErrorAction SilentlyContinue
    if ($running) {
        Warn 'The Battle.net app is running and will conflict with the Agent.'
        if ((Ask '    Close it now? (y/n)' 'y') -match '^[Yy]') {
            $running | Stop-Process -Force
            Start-Sleep -Seconds 3
            Ok 'Closed'
        }
    }

    # Check BEFORE the ~100 GB download, not after it.
    Step 3 'Checking which build Blizzard is currently shipping'
    $live = Get-LiveBuild $Product
    if (-not $live) {
        Warn 'Could not reach the Blizzard version endpoint - cannot tell what will be installed.'
    } elseif ($live -eq $Build) {
        Ok "Live build is $live - matches what this server expects"
    } else {
        Write-Host @"

    !!  THE INSTALL WILL NOT GIVE YOU THE BUILD YOU ASKED FOR  !!

        this server expects : $Build
        Blizzard ships now  : $live

    Battle.Net-Installer drives Blizzard's Agent, which only ever installs the
    CURRENT live build. There is no flag for an older one, and Blizzard does not
    serve arbitrary past builds. Downloading now gives you $live, and the
    client will be rejected at login with a version mismatch.

    Your options:
      1. Update the SERVER to $live - the sustainable answer. Update
         TrinityCore, re-extract the game data from the new client, and set the
         realm's gamebuild to $live.
      2. If you already own a $Build install, keep it and stop the Battle.net
         app from updating it. Re-run this script with -SkipInstall to point
         that existing client at your server.
      3. Run this anyway with -Build $live if you are building the server
         around the current client.

"@ -ForegroundColor Red
        if ((Ask '    Download anyway? (y/n)' 'n') -notmatch '^[Yy]') {
            Write-Host '    Stopped before downloading.' -ForegroundColor Yellow
            exit 1
        }
    }

    Step 4 "Installing '$Product' ($Locale) into $InstallDir"
    Write-Host '    This downloads ~100 GB from Blizzard and takes a long time.' -ForegroundColor DarkGray
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null

    # --verbose false is required: verbose progress reads the cursor position
    # and dies when there is no interactive desktop session.
    $args = @('--prod', $Product, '--lang', $Locale, '--dir', $InstallDir, '--uid', $Product, '--verbose', 'false')
    Write-Host "    > $installer $($args -join ' ')" -ForegroundColor DarkGray
    & $installer @args
    if ($LASTEXITCODE -ne 0) {
        Fail @"
Battle.Net-Installer exited with code $LASTEXITCODE.
Errors are bare numeric codes; the readable diagnostics are in the Battle.net
agent log. Error 2310 specifically means you need the fork:
  $BNET_INSTALLER_FORK
"@
    }
    Ok 'Installer finished'
}

# --------------------------------------------------------- 2. verify build
Step 5 'Verifying the build'
$buildInfo = Join-Path $InstallDir '.build.info'
if (-not (Test-Path -LiteralPath $buildInfo)) {
    Fail ".build.info not found in $InstallDir - that directory is not a client install root."
}
$found = Select-String -LiteralPath $buildInfo -Pattern ([regex]::Escape($Build)) -Quiet
if ($found) {
    Ok "Build $Build confirmed"
} else {
    $versions = (Get-Content -LiteralPath $buildInfo | Select-Object -Skip 1) -split '\|' |
                Where-Object { $_ -match '^\d+\.\d+\.\d+\.\d+$' } | Select-Object -Unique
    Warn "Expected $Build but .build.info reports: $($versions -join ', ')"
    Warn 'Login WILL fail with a version mismatch until the client and world database agree.'
    if ((Ask '    Continue anyway? (y/n)' 'n') -notmatch '^[Yy]') { exit 1 }
}

# ------------------------------------------------------------- 3. Arctium
Step 6 'Checking for the Arctium launcher'
$arctiumPath = Join-Path $InstallDir 'Arctium Game Launcher.exe'
if (Test-Path -LiteralPath $arctiumPath) {
    Ok 'Arctium Game Launcher.exe is in the install root'
} else {
    # A very common mistake: it gets dropped next to Wow.exe, where it does nothing.
    $misplaced = Join-Path $InstallDir '_retail_\Arctium Game Launcher.exe'
    if (Test-Path -LiteralPath $misplaced) {
        Warn 'Found it inside _retail_ - it belongs in the install root. Moving it.'
        Move-Item -LiteralPath $misplaced -Destination $arctiumPath -Force
        Ok 'Moved to the install root'
    } else {
        Write-Host @"

    Arctium Game Launcher.exe was not found.

    Download it from $ARCTIUM and place it here:
      $arctiumPath
    (the install ROOT, beside _retail_ and Data - NOT next to Wow.exe)

"@ -ForegroundColor Yellow
        Pause-For '    Press Enter once it is in place (Ctrl+C to abort)'
        if (-not (Test-Path -LiteralPath $arctiumPath)) { Warn 'Still missing - you will not be able to connect until it is there.' }
        else { Ok 'Found' }
    }
}

# --------------------------------------------------------- 4. set the portal
Step 7 "Pointing the client at $ServerIP"
$wtfDir     = Join-Path $InstallDir '_retail_\WTF'
$configPath = Join-Path $wtfDir 'Config.wtf'
New-Item -ItemType Directory -Force -Path $wtfDir | Out-Null

$portalLine = 'SET portal "{0}"' -f $ServerIP
if (Test-Path -LiteralPath $configPath) {
    Copy-Item -LiteralPath $configPath -Destination "$configPath.bak" -Force
    $lines = @(Get-Content -LiteralPath $configPath)
    # Replace an existing portal line rather than appending a second one.
    if ($lines -match '^\s*SET\s+portal\s') {
        $lines = $lines | ForEach-Object { if ($_ -match '^\s*SET\s+portal\s') { $portalLine } else { $_ } }
        Ok 'Existing portal line replaced (previous file kept as Config.wtf.bak)'
    } else {
        $lines += $portalLine
        Ok 'Portal line added (previous file kept as Config.wtf.bak)'
    }
    Set-Content -LiteralPath $configPath -Value $lines -Encoding UTF8
} else {
    Set-Content -LiteralPath $configPath -Value @($portalLine) -Encoding UTF8
    Ok 'Config.wtf created'
}
Write-Host "    $portalLine" -ForegroundColor DarkGray

# ----------------------------------------------------------------- 5. done
Write-Host @"

=====================================================================
 Ready.
=====================================================================
 Client   : $InstallDir
 Build    : $Build
 Server   : $ServerIP

 To play:
   1. Run "$arctiumPath"
      (NOT Battle.net.exe, and NOT Wow.exe directly)
   2. Log in with the account your server admin created for you.

 No account yet? On the SERVER run:
   scripts/create-account.sh -e you@example.com -p YourPassword

 Note it is an EMAIL, not a plain name: a retail client logs in through
 Battle.net, so the account must be a Battle.net account. A plain
 'account create' makes a game account with no Battle.net link, and you
 cannot log in with it.
=====================================================================
"@ -ForegroundColor Magenta

if ((Ask 'Launch the game now? (y/n)' 'n') -match '^[Yy]') {
    if (Test-Path -LiteralPath $arctiumPath) { Start-Process -FilePath $arctiumPath -WorkingDirectory $InstallDir }
    else { Fail 'Arctium Game Launcher.exe is not present.' }
}
