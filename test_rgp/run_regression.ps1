param(
    [string]$ImporterPath
)

$ErrorActionPreference = 'Stop'

function Invoke-CapturedProcess
{
    param(
        [string]$Executable,
        [string[]]$Arguments
    )

    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $Executable
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $quotedArguments = foreach( $argument in $Arguments )
    {
        '"' + $argument.Replace( '"', '\"' ) + '"'
    }
    $startInfo.Arguments = $quotedArguments -join ' '

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    if( -not $process.Start() )
    {
        throw "Failed to start $Executable"
    }

    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    $result = [pscustomobject]@{
        ExitCode = $process.ExitCode
        StdOut = $stdoutTask.GetAwaiter().GetResult()
        StdErr = $stderrTask.GetAwaiter().GetResult()
    }
    $process.Dispose()
    return $result
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$testRoot = Join-Path $root 'test_rgp'
$sampleRoot = Join-Path $testRoot 'samples'
$importer = if( [string]::IsNullOrWhiteSpace( $ImporterPath ) ) {
    Join-Path $root 'import\build-msvc\tracy-import-rgp.exe'
} else {
    (Resolve-Path -LiteralPath $ImporterPath).Path
}
$csvexport = Join-Path $root 'csvexport\build-msvc\tracy-csvexport.exe'
$expectedPath = Join-Path $testRoot 'expected_stats.json'

foreach( $requiredPath in @($importer, $csvexport, $expectedPath) )
{
    if( -not (Test-Path -LiteralPath $requiredPath -PathType Leaf) )
    {
        throw "Required file is missing: $requiredPath"
    }
}

$expected = Get-Content -LiteralPath $expectedPath -Raw | ConvertFrom-Json
$expectedByPath = @{}
foreach( $entry in $expected.files )
{
    $expectedByPath[$entry.path] = $entry
}

$primaryCapture = 'viewer_2024.03.04_20.17.09.rgp'
$primaryPath = Join-Path $testRoot $primaryCapture
if( -not (Test-Path -LiteralPath $primaryPath -PathType Leaf) )
{
    throw "Required capture is missing: $primaryPath"
}

$cases = [System.Collections.Generic.List[object]]::new()
$cases.Add( [pscustomobject]@{ RelativePath = $primaryCapture; InputPath = $primaryPath } )
$sampleFiles = @(Get-ChildItem -LiteralPath $sampleRoot -Filter '*.rgp' -File | Sort-Object Name)
foreach( $sample in $sampleFiles )
{
    $relativePath = 'samples/' + $sample.Name
    if( -not $expectedByPath.ContainsKey( $relativePath ) )
    {
        throw "No expected stats are recorded for $relativePath"
    }
    $cases.Add( [pscustomobject]@{ RelativePath = $relativePath; InputPath = $sample.FullName } )
}

foreach( $entry in $expected.files )
{
    if( $entry.path.StartsWith( 'samples/', [System.StringComparison]::OrdinalIgnoreCase ) )
    {
        $samplePath = Join-Path $sampleRoot $entry.path.Substring( 8 )
        if( -not (Test-Path -LiteralPath $samplePath -PathType Leaf) )
        {
            Write-Host "SKIP missing sample: $($entry.path)"
        }
    }
}

$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('tracy-rgp-regression-' + [guid]::NewGuid().ToString( 'N' ))
[System.IO.Directory]::CreateDirectory( $tempRoot ) | Out-Null
$results = [System.Collections.Generic.List[object]]::new()
$failures = 0

try
{
    foreach( $case in $cases )
    {
        $expectedStats = $expectedByPath[$case.RelativePath]
        $tracePath = Join-Path $tempRoot ([System.IO.Path]::GetFileNameWithoutExtension( $case.RelativePath ) + '.tracy')
        $importResult = Invoke-CapturedProcess $importer @('--stats', $case.InputPath, $tracePath)
        $text = $importResult.StdOut + "`n" + $importResult.StdErr
        $gfxMatch = [regex]::Match( $text, 'GFX level (0x[0-9A-Fa-f]+)' )
        $wavesMatch = [regex]::Match( $text, 'Decoded (\d+) concurrent wavefronts' )
        $unknownMatch = [regex]::Match( $text, 'unknown tokens: (\d+)' )
        $markerMatch = [regex]::Match( $text, 'SQTT markers: SE \d+, (\d+) decoded, (\d+) decode errors' )
        $hasStats = $gfxMatch.Success -and $wavesMatch.Success -and $unknownMatch.Success

        $markerCount = 0
        $decodeErrors = 0
        if( $markerMatch.Success )
        {
            $markerCount = [int64]$markerMatch.Groups[1].Value
            $decodeErrors = [int64]$markerMatch.Groups[2].Value
        }
        elseif( $text -notmatch 'SQTT markers: no SQTT streams available' )
        {
            $hasStats = $false
        }

        $actual = [ordered]@{
            gfxLevel = if( $gfxMatch.Success ) { $gfxMatch.Groups[1].Value } else { '' }
            waves = if( $wavesMatch.Success ) { [int64]$wavesMatch.Groups[1].Value } else { -1 }
            unknownTokens = if( $unknownMatch.Success ) { [int64]$unknownMatch.Groups[1].Value } else { -1 }
            markers = $markerCount
            decodeErrors = $decodeErrors
        }

        $status = 'PASS'
        $mismatches = [System.Collections.Generic.List[string]]::new()
        if( $importResult.ExitCode -ne 0 )
        {
            $mismatches.Add( "importer exited $($importResult.ExitCode)" )
        }
        if( -not $hasStats )
        {
            $mismatches.Add( 'required stats were not present in importer output' )
        }
        foreach( $key in @('gfxLevel', 'waves', 'unknownTokens', 'markers', 'decodeErrors') )
        {
            if( [string]$actual[$key] -ne [string]$expectedStats.$key )
            {
                $mismatches.Add( "$key expected $($expectedStats.$key), got $($actual[$key])" )
            }
        }

        if( -not (Test-Path -LiteralPath $tracePath -PathType Leaf) )
        {
            $mismatches.Add( 'importer did not create a trace' )
        }
        else
        {
            $csvResult = Invoke-CapturedProcess $csvexport @('--unwrap', $tracePath)
            if( $csvResult.ExitCode -ne 0 )
            {
                $mismatches.Add( "csvexport exited $($csvResult.ExitCode)" )
                if( $csvResult.StdErr ) { Write-Host $csvResult.StdErr }
            }
        }

        if( $mismatches.Count -gt 0 )
        {
            $status = 'FAIL'
            $failures++
            Write-Host ("FAIL {0}: {1}" -f $case.RelativePath, ($mismatches -join '; '))
            if( $importResult.ExitCode -ne 0 -or -not $hasStats )
            {
                Write-Host $text
            }
        }

        $results.Add( [pscustomobject]@{
            File = $case.RelativePath
            GFX = $actual.gfxLevel
            Waves = $actual.waves
            Unknown = $actual.unknownTokens
            Markers = $actual.markers
            Errors = $actual.decodeErrors
            Result = $status
        } )
    }
}
finally
{
    Remove-Item -LiteralPath $tempRoot -Recurse -Force
}

$results | Format-Table -AutoSize | Out-String | Write-Host
Write-Host ("Regression cases: {0}; failures: {1}" -f $results.Count, $failures)
if( $failures -gt 0 ) { exit 1 }