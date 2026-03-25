$ErrorActionPreference = "Stop"

function Invoke-CMake {
	param(
		[Parameter(Mandatory = $true)]
		[string[]]$Args
	)

	& cmake @Args
	if ($LASTEXITCODE -ne 0) {
		throw "cmake failed: cmake $($Args -join ' ')"
	}
}

function Get-GppMajorVersion {
	$gpp = Get-Command g++ -ErrorAction SilentlyContinue
	if ($null -eq $gpp) {
		return $null
	}

	$versionText = (& g++ --version | Select-Object -First 1)
	if ($versionText -match '(\d+)\.\d+\.\d+') {
		return [int]$matches[1]
	}

	return $null
}

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
	throw "CMake is not installed or not in PATH. Install CMake first: https://cmake.org/download/"
}

$ProjectRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
Push-Location $ProjectRoot

try {
	$generatorCandidates = @(
		@{ Name = "Ninja"; MultiConfig = $false },
		@{ Name = "Visual Studio 18 2026"; MultiConfig = $true },
		@{ Name = "Visual Studio 17 2022"; MultiConfig = $true },
		@{ Name = "Visual Studio 16 2019"; MultiConfig = $true },
		@{ Name = "MinGW Makefiles"; MultiConfig = $false }
	)

	$selected = $null

	foreach ($candidate in $generatorCandidates) {
		Write-Host "Trying generator: $($candidate.Name)"

		if ($candidate.Name -eq "MinGW Makefiles") {
			$gppMajor = Get-GppMajorVersion
			if ($null -eq $gppMajor) {
				Write-Host "Skipping MinGW Makefiles: g++ not found in PATH."
				continue
			}
			if ($gppMajor -lt 9) {
				Write-Host "Skipping MinGW Makefiles: g++ $gppMajor.x is too old. Require GCC 9+ for C++17 stdlib support."
				continue
			}
		}

		if (Test-Path build) {
			Remove-Item build -Recurse -Force
		}

		$configureArgs = @("-S", ".", "-B", "build", "-G", $candidate.Name)
		if (-not $candidate.MultiConfig) {
			$configureArgs += @("-DCMAKE_BUILD_TYPE=Release")
		}

		try {
			Invoke-CMake -Args $configureArgs
			$selected = $candidate
			break
		}
		catch {
			Write-Host "Generator failed: $($candidate.Name)"
			Write-Host $_.Exception.Message
		}
	}

	if ($null -eq $selected) {
		throw @"
No usable CMake generator/toolchain found.

Install one of the following and rerun:
1) Visual Studio 2022 Build Tools with "Desktop development with C++"
2) Ninja + MSVC/Clang/GCC toolchain in PATH
3) MinGW-w64 (g++) and use MinGW Makefiles
"@
	}

	Write-Host "Selected generator: $($selected.Name)"
	Write-Host "Building benchmark target..."

	$buildArgs = @("--build", "build", "--target", "cache_benchmark")
	if ($selected.MultiConfig) {
		$buildArgs += @("--config", "Release")
	}
	Invoke-CMake -Args $buildArgs

	$exeMulti = "build/Release/cache_benchmark.exe"
	$exeSingle = "build/cache_benchmark.exe"

	Write-Host "Running benchmark..."
	if (Test-Path $exeMulti) {
		& $exeMulti
	}
	elseif (Test-Path $exeSingle) {
		& $exeSingle
	}
	else {
		throw "Benchmark executable not found. Expected '$exeMulti' or '$exeSingle'."
	}
}
finally {
	Pop-Location
}
