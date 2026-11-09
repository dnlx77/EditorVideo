# Avvia un eseguibile Release (di solito EditorVideoTests.exe, le prove; EditorVideo.exe solo per --smoke-test) con gli
# argomenti dati, aspetta che finisca, mostra cosa ha scritto e controlla il codice di uscita. Usato da tutte le prove della
# build automatica.
#
#   & ci\run-editorvideo.ps1 -Name 'unione' -Arguments @('--join-test', ...) [-Expected 4] [-TimeoutSeconds 60] [-Exe EditorVideo.exe]
#
# Gli argomenti con spazi vanno già racchiusi fra virgolette (come per Start-Process).
param(
    [Parameter(Mandatory = $true)] [string] $Name,
    [Parameter(Mandatory = $true)] [string[]] $Arguments,
    [int] $Expected = 0,
    [int] $TimeoutSeconds = 60,
    [string] $Exe = 'EditorVideoTests.exe'
)

$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot "..\bin\x64\Release\$Exe"
$out = New-TemporaryFile
$err = New-TemporaryFile
try {
    $p = Start-Process $exe -ArgumentList $Arguments -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
    $null = $p.Handle  # necessario per leggere ExitCode
    if (-not $p.WaitForExit($TimeoutSeconds * 1000)) {
        $p.Kill()
        Get-Content $out, $err
        throw "$Name : $Exe non si è chiuso entro $TimeoutSeconds secondi"
    }
    Get-Content $out, $err
    if ($p.ExitCode -ne $Expected) {
        throw "$Name : codice $($p.ExitCode) invece di $Expected"
    }
} finally {
    Remove-Item $out, $err -ErrorAction SilentlyContinue
}
