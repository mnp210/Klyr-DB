<#
.SYNOPSIS
    Reproducible portfolio demonstration runner for KlyrDB.
.DESCRIPTION
    Executes a fully automated, clean-state demonstration of KlyrDB v0.12.0
    capabilities including table creation, slotted-page storage, B+ tree indexing,
    cost-based query planning (EXPLAIN ANALYZE), transaction rollback atomicity,
    durable commits, persistence across restarts, and storage integrity audits.
.PARAMETER CliPath
    Optional path to the compiled klyrdb.exe executable.
.PARAMETER KeepDatabase
    If specified, preserves the temporary database directory after completion.
#>
[CmdletBinding()]
param(
    [string]$CliPath = "",
    [switch]$KeepDatabase
)

$ErrorActionPreference = "Stop"

# 1. Locate klyrdb.exe
if (-not $CliPath) {
    $candidatePaths = @(
        "$PSScriptRoot\..\..\build_audit_release\Release\klyrdb.exe",
        "$PSScriptRoot\..\..\build\Release\klyrdb.exe",
        "$PSScriptRoot\..\..\build_release\Release\klyrdb.exe",
        "$PSScriptRoot\..\..\dist\bin\klyrdb.exe"
    )
    foreach ($candidate in $candidatePaths) {
        if (Test-Path $candidate) {
            $CliPath = (Resolve-Path $candidate).Path
            break
        }
    }
}

if (-not $CliPath -or -not (Test-Path $CliPath)) {
    Write-Error "[-] Could not find klyrdb.exe. Please build KlyrDB or provide -CliPath <path/to/klyrdb.exe>"
    exit 1
}

Write-Host "======================================================================" -ForegroundColor Cyan
Write-Host "                KLYRDB v0.12.0 PORTFOLIO DEMONSTRATION                " -ForegroundColor Cyan
Write-Host "======================================================================" -ForegroundColor Cyan
Write-Host "CLI Executable: $CliPath" -ForegroundColor Gray

# 2. Setup isolated temporary directory
$tempDir = Join-Path $env:TEMP ("klyrdb_portfolio_demo_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null
$dbPath = Join-Path $tempDir "portfolio.kdb"

Write-Host "Workspace:      $tempDir" -ForegroundColor Gray
Write-Host "Database File:  $dbPath" -ForegroundColor Gray
Write-Host ""

try {
    # ------------------------------------------------------------------
    # Step 1: Engine Version & Architecture
    # ------------------------------------------------------------------
    Write-Host "[Step 1/7] Engine Version & Architecture Information:" -ForegroundColor Yellow
    & $CliPath version
    if ($LASTEXITCODE -ne 0) { throw "Step 1 failed with exit code $LASTEXITCODE" }
    Write-Host ""

    # ------------------------------------------------------------------
    # Step 2: DDL Table Creation & Seed Records
    # ------------------------------------------------------------------
    Write-Host "[Step 2/7] Creating Table and Seeding Relational Records..." -ForegroundColor Yellow
    & $CliPath sql $dbPath "CREATE TABLE products (sku INTEGER PRIMARY KEY, name TEXT NOT NULL, price INTEGER, in_stock INTEGER);"
    if ($LASTEXITCODE -ne 0) { throw "Table creation failed" }

    & $CliPath sql $dbPath "INSERT INTO products VALUES (101, 'Mechanical Keyboard', 120, 15);"
    & $CliPath sql $dbPath "INSERT INTO products VALUES (102, 'Ergonomic Mouse', 65, 30);"
    & $CliPath sql $dbPath "INSERT INTO products VALUES (103, 'USB-C Dock', 180, 8);"
    if ($LASTEXITCODE -ne 0) { throw "Row seeding failed" }
    Write-Host ""

    # ------------------------------------------------------------------
    # Step 3: Query Execution with Filtering & Ordering
    # ------------------------------------------------------------------
    Write-Host "[Step 3/7] Querying Filtered & Sorted Records (Table Scan Baseline)..." -ForegroundColor Yellow
    & $CliPath sql $dbPath "SELECT sku, name, price, in_stock FROM products WHERE price <= 150 ORDER BY price ASC;"
    if ($LASTEXITCODE -ne 0) { throw "Query failed" }
    Write-Host ""

    # ------------------------------------------------------------------
    # Step 4: B+ Tree Indexing & Cost-Based Query Plan (EXPLAIN ANALYZE)
    # ------------------------------------------------------------------
    Write-Host "[Step 4/7] Creating B+ Tree Index & Inspecting Query Execution Plan..." -ForegroundColor Yellow
    & $CliPath sql $dbPath "CREATE INDEX idx_price ON products(price);"
    if ($LASTEXITCODE -ne 0) { throw "Index creation failed" }

    Write-Host "`n>> Cost-Based Query Plan (EXPLAIN):" -ForegroundColor Cyan
    & $CliPath sql $dbPath "EXPLAIN SELECT sku, name, price, in_stock FROM products WHERE price >= 65 AND price <= 150;"

    Write-Host "`n>> Runtime Execution Telemetry (EXPLAIN ANALYZE):" -ForegroundColor Cyan
    & $CliPath sql $dbPath "EXPLAIN ANALYZE SELECT sku, name, price, in_stock FROM products WHERE price >= 65 AND price <= 150;"
    if ($LASTEXITCODE -ne 0) { throw "EXPLAIN failed" }
    Write-Host ""

    # ------------------------------------------------------------------
    # Step 5: Transaction Atomicity: Rollback vs Commit
    # ------------------------------------------------------------------
    Write-Host "[Step 5/7] Demonstrating Transaction Rollback Atomicity & Safe Commit..." -ForegroundColor Yellow
    $txnScript = @"
BEGIN;
INSERT INTO products VALUES (999, 'Temporary Prototype', 10, 0);
SELECT sku, name, price FROM products WHERE sku = 999;
ROLLBACK;
SELECT sku, name, price FROM products WHERE sku = 999;
BEGIN;
INSERT INTO products VALUES (104, '4K UltraSharp Monitor', 350, 12);
COMMIT;
.quit
"@
    $txnScript | & $CliPath sql $dbPath
    if ($LASTEXITCODE -ne 0) { throw "Transaction demo failed" }
    Write-Host ""

    # ------------------------------------------------------------------
    # Step 6: Verify Persistence Across Independent Process Reopen
    # ------------------------------------------------------------------
    Write-Host "[Step 6/7] Verifying Durable Persistence Across New CLI Process..." -ForegroundColor Yellow
    & $CliPath sql $dbPath "SELECT sku, name, price, in_stock FROM products ORDER BY sku ASC;"
    if ($LASTEXITCODE -ne 0) { throw "Persistence verification failed" }
    Write-Host ""

    # ------------------------------------------------------------------
    # Step 7: Storage & Relational Integrity Audit
    # ------------------------------------------------------------------
    Write-Host "[Step 7/7] Running Storage & Relational Integrity Audit..." -ForegroundColor Yellow
    & $CliPath integrity-check $dbPath
    if ($LASTEXITCODE -ne 0) { throw "Integrity check failed" }
    Write-Host ""

    Write-Host "======================================================================" -ForegroundColor Green
    Write-Host "       [+] ALL PORTFOLIO DEMONSTRATION STEPS COMPLETED WITH EXIT 0    " -ForegroundColor Green
    Write-Host "======================================================================" -ForegroundColor Green
}
finally {
    if ($KeepDatabase) {
        Write-Host "`n[Preserved] Temporary database retained at: $tempDir" -ForegroundColor Magenta
    } else {
        Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue
        Write-Host "`n[Cleaned] Isolated temporary database safely removed." -ForegroundColor Gray
    }
}
