# DuckDB Brew Extension - Quick Start Guide

## Setup

### Create Structured Tables with Constraints

**What it does:** Creates properly structured tables with primary keys and foreign keys for better data integrity.

**When to use it:** Run this at the start of each session to set up your analysis environment.

**What the results tell you:** Ensures you have clean, normalized data ready for analysis.

```sql
-- Create brew_packages table with constraints
CREATE OR REPLACE TEMP TABLE brew_packages (
    tap VARCHAR,
    name VARCHAR PRIMARY KEY,
    version VARCHAR,
    type VARCHAR,
    description VARCHAR,
    homepage VARCHAR,
    license VARCHAR,
    installed_on_request BOOLEAN,
    installed_as_dependency BOOLEAN,
    installed_time TIMESTAMP,
    outdated BOOLEAN,
    pinned BOOLEAN,
    deprecated BOOLEAN,
    disabled BOOLEAN,
    poured_from_bottle BOOLEAN,
    built_as_bottle BOOLEAN,
    dependencies VARCHAR,
    aliases VARCHAR,
    deprecation_reason VARCHAR,
    disable_reason VARCHAR,
    caveats VARCHAR,
    size_bytes BIGINT
);

-- Populate from the function
INSERT INTO brew_packages
SELECT * FROM brew_packages();
```

```sql
-- Create brew_casks table
CREATE OR REPLACE TEMP TABLE brew_casks AS
SELECT *
FROM brew_casks();
```

```sql
-- Create brew_dependencies table with foreign key constraints
CREATE OR REPLACE TEMP TABLE brew_dependencies (
    name VARCHAR,
    dependency VARCHAR,
    FOREIGN KEY (name) REFERENCES brew_packages(name),
    FOREIGN KEY (dependency) REFERENCES brew_packages(name)
);

-- Populate from brew_packages function
INSERT INTO brew_dependencies
SELECT
    name,
    unnest(string_split(dependencies, ', ')) as dependency
FROM brew_packages()
WHERE dependencies IS NOT NULL
ORDER BY name;
```

## Global Homebrew Information

### Check Version and Configuration

**What it does:** Shows your current Homebrew version and all configuration variables.

**When to use it:** When auditing your Homebrew environment or checking paths.

**What the results tell you:** Confirms your Homebrew version and key settings like `HOMEBREW_PREFIX`.

```sql
-- Simple version check
SELECT brew_version();

-- View all configuration
SELECT * FROM brew_config();

-- Query specific paths
SELECT value 
FROM brew_config() 
WHERE name IN ('HOMEBREW_PREFIX', 'HOMEBREW_CELLAR');
```

## Basic Queries

### Package Distribution by Tap

**What it does:** Shows where your packages come from (which Homebrew taps/repositories).

**When to use it:** Understanding your package sources and identifying custom taps.

**What the results tell you:** Most packages typically come from `homebrew/core` and `homebrew/cask`.

```sql
SELECT 
    tap, 
    count(*) as package_count
FROM brew_packages
GROUP BY tap
ORDER BY package_count DESC;
```

### Explicitly Installed vs Auto-Installed Dependencies

**What it does:** Shows how many packages you manually installed vs dependencies.

**When to use it:** Understanding your installation footprint.

**What the results tell you:** A high dependency count is normal; these were installed automatically.

```sql
SELECT 
    installed_on_request, 
    count(*) as count
FROM brew_packages
GROUP BY installed_on_request;
```

### Installation Timeline

**What it does:** Shows when packages were installed over time, grouped by month.

**When to use it:** Understanding your Homebrew usage patterns.

**What the results tell you:** Spikes indicate periods of heavy development or system setup.

```sql
SELECT 
    date_trunc('month', installed_time) as month, 
    count(*) as packages_installed
FROM brew_packages
WHERE installed_time IS NOT NULL
GROUP BY month
ORDER BY month DESC;
```

### Disk Usage Analysis

**What it does:** Shows the largest packages consuming disk space.

**When to use it:** When running low on disk space or auditing storage usage.

**What the results tell you:** Identifies packages you might want to remove to free up space.

```sql
-- Top 10 largest packages
SELECT 
    name, 
    type,
    ROUND(size_bytes / 1024 / 1024) as size_mb
FROM brew_packages
ORDER BY size_bytes DESC
LIMIT 10;
```

**What it does:** Shows total disk usage by package type (formula vs cask).

**When to use it:** Understanding where your disk space is going.

**What the results tell you:** Whether formulas or casks consume more space.

```sql
-- Total disk usage by package type
SELECT 
    type, 
    ROUND(sum(size_bytes) / 1024.0 / 1024.0 / 1024.0, 1) as total_gb
FROM brew_packages
GROUP BY type;
```

**What it does:** Shows total disk usage by tap/repository.

**When to use it:** Identifying which taps consume the most disk space.

**What the results tell you:** Third-party taps may have larger packages than core.

```sql
-- Total disk usage by tap
SELECT 
    tap, 
    count(*) as package_count,
    ROUND(sum(size_bytes) / 1024.0 / 1024.0 / 1024.0, 1) as total_gb
FROM brew_packages
GROUP BY tap
ORDER BY total_gb DESC;
```

## Dependency Analysis

### Top 10 Packages with Most Dependencies

**What it does:** Finds the most complex packages (those requiring many other packages).

**When to use it:** Identifying heavyweight installations.

**What the results tell you:** These packages pull in many dependencies; uninstalling them frees significant space.

```sql
SELECT 
    name,
    count(*) as dependency_count
FROM brew_dependencies
GROUP BY name
ORDER BY dependency_count DESC
LIMIT 10;
```

### Top 10 Most Critical Packages

**What it does:** Finds packages that are most depended upon by other packages.

**When to use it:** Understanding which packages are foundational to your system.

**What the results tell you:** These are critical packages; removing them could break many other packages.

```sql
SELECT 
    dependency,
    count(*) as needed_by_count
FROM brew_dependencies
GROUP BY dependency
ORDER BY needed_by_count DESC
LIMIT 10;
```

### Full Dependency Chain (Recursive)

**What it does:** Shows the complete dependency tree for a specific package.

**When to use it:** Understanding all transitive dependencies before installing/removing a package.

**What the results tell you:** All packages that would be affected by changes to the target package.

```sql
-- Replace 'python@3.12' with any package name you want to analyze
WITH RECURSIVE dep_chain AS (
    -- Start with the package we're interested in
    SELECT name, dependency, 1 as level
    FROM brew_dependencies
    WHERE name = 'node'
    
    UNION ALL
    
    -- Recursively find dependencies of dependencies
    SELECT bd.name, bd.dependency, dc.level + 1
    FROM brew_dependencies bd
    JOIN dep_chain dc ON bd.name = dc.dependency
    WHERE dc.level < 10  -- Prevent infinite loops
)
SELECT DISTINCT dependency, min(level) as depth_level
FROM dep_chain
GROUP BY dependency
ORDER BY depth_level, dependency;
```

## Package Health & Maintenance

### Outdated Packages

**What it does:** Lists packages with newer versions available.

**When to use it:** Before running `brew upgrade` to see what will be updated.

**What the results tell you:** These packages are behind current releases.

```sql
SELECT name, version, tap
FROM brew_packages
WHERE outdated = true
ORDER BY name;
```

### Orphaned Packages (Safe to Remove)

**What it does:** Finds packages that were installed as dependencies but are no longer needed.

**When to use it:** Cleaning up your system after uninstalling packages.

**What the results tell you:** These can be safely removed with `brew autoremove`.

```sql
SELECT name, version
FROM brew_packages
WHERE installed_as_dependency = true
  AND name NOT IN (SELECT DISTINCT dependency FROM brew_dependencies);
```

### Packages Requiring Attention

**What it does:** Finds deprecated, disabled, or packages with important caveats.

**When to use it:** Regular maintenance checks to identify problematic packages.

**What the results tell you:** Action needed: deprecated packages should be replaced, disabled packages won't work.

```sql
SELECT name, deprecated, deprecation_reason, disabled, disable_reason, caveats
FROM brew_packages
WHERE deprecated = true OR disabled = true OR caveats IS NOT NULL;
```

## Advanced Analytics with PIVOT

### Installation Intent by Package Type

**What it does:** Cross-tabulates installation method (manual vs dependency) by type (formula vs cask).

**When to use it:** Understanding your installation patterns.

**What the results tell you:** Shows distribution across categories.

```sql
PIVOT brew_packages
ON installed_on_request
USING count(*)
GROUP BY type;
```

### Package Type Distribution by Tap

**What it does:** Shows how many formulas vs casks come from each tap.

**When to use it:** Understanding tap composition.

**What the results tell you:** Core taps typically have more formulas; cask taps have more casks.

```sql
PIVOT brew_packages
ON type
USING count(*)
GROUP BY tap;
```

### Outdated Packages by Tap

**What it does:** Shows which taps have the most outdated packages.

**When to use it:** Identifying taps that need attention.

**What the results tell you:** Some taps may update more frequently than others.

```sql
PIVOT brew_packages
ON outdated
USING count(*)
GROUP BY tap;
```

### Bottle vs Source Builds by Tap

**What it does:** Shows installation method (pre-built vs compiled) for formulas by tap.

**When to use it:** Understanding build performance and compatibility.

**What the results tell you:** Bottles are faster; source builds indicate no pre-built binary available.

```sql
PIVOT (SELECT * FROM brew_packages WHERE type = 'formula')
ON poured_from_bottle
USING count(*)
GROUP BY tap;
```

### Installation Timeline by Type

**What it does:** Shows when formulas vs casks were installed over time by year.

**When to use it:** Understanding installation trends.

**What the results tell you:** Package type preferences over time.

```sql
PIVOT (
    SELECT date_trunc('year', installed_time) as year, type, count(*) as cnt
    FROM brew_packages
    WHERE installed_time IS NOT NULL
    GROUP BY year, type
)
ON type
USING sum(cnt)
GROUP BY year;
```

## Data Visualization Examples

### Export to CSV for External Charting

**What it does:** Exports query results to CSV for use in Excel, Google Sheets, or BI tools.

**When to use it:** Creating charts, dashboards, or sharing data with others.

```sql
-- Export tap distribution to CSV
COPY (
    SELECT tap, count(*) as package_count
    FROM brew_packages
    GROUP BY tap
    ORDER BY package_count DESC
) TO 'tap_distribution.csv' (HEADER, DELIMITER ',');

-- Export installation timeline to CSV
COPY (
    SELECT date_trunc('month', installed_time) as month, count(*) as count
    FROM brew_packages
    WHERE installed_time IS NOT NULL
    GROUP BY month
    ORDER BY month
) TO 'installation_timeline.csv' (HEADER, DELIMITER ',');
```

### Using DuckDB's Built-in Charting (CLI)

**What it does:** Creates simple ASCII charts directly in the terminal.

**When to use it:** Quick visual insights without external tools.

```sql
-- Simple bar chart of package counts by tap
-- (Use DuckDB CLI's .mode markdown or similar for better formatting)
SELECT 
    tap,
    repeat('█', cast(count(*)/10 as integer)) as chart,
    count(*) as count
FROM brew_packages
GROUP BY tap
ORDER BY count DESC
LIMIT 15;
```

## Practical Workflows

### How to Clean Up Your Homebrew Installation

**Step 1: Identify orphaned packages**
```sql
SELECT name
FROM brew_packages
WHERE installed_as_dependency = true
  AND name NOT IN (SELECT DISTINCT dependency FROM brew_dependencies);
```

**Step 2: Review packages requiring attention**
```sql
SELECT name, deprecation_reason, disable_reason
FROM brew_packages
WHERE deprecated = true OR disabled = true;
```

**Step 3: Find outdated packages**
```sql
SELECT name, version
FROM brew_packages
WHERE outdated = true;
```

**Step 4: Execute cleanup (run in terminal)**
```bash
# Remove orphaned packages
brew autoremove

# Update outdated packages
brew upgrade

# Remove deprecated packages (review list first)
# brew uninstall <deprecated-package-name>
```

### Monthly Package Audit Checklist

```sql
-- 1. Overall package health
SELECT 
    count(*) as total_packages,
    sum(case when outdated then 1 else 0 end) as outdated,
    sum(case when deprecated then 1 else 0 end) as deprecated,
    sum(case when installed_as_dependency = false then 1 else 0 end) as explicitly_installed,
    ROUND(sum(size_bytes) / 1024.0 / 1024.0 / 1024.0, 1) as total_gb
FROM brew_packages;

-- 2. New packages this month
SELECT name, installed_time, ROUND(size_bytes / 1024 / 1024) as size_mb
FROM brew_packages
WHERE installed_time >= date_trunc('month', now())
ORDER BY installed_time DESC;

-- 3. Packages needing action
SELECT name, 
    case 
        when deprecated then 'DEPRECATED: ' || deprecation_reason
        when disabled then 'DISABLED: ' || disable_reason
        when outdated then 'OUTDATED'
        when caveats is not null then 'HAS CAVEATS'
    end as action_needed,
    ROUND(size_bytes / 1024 / 1024) as size_mb
FROM brew_packages
WHERE deprecated OR disabled OR outdated OR caveats IS NOT NULL;

-- 4. Largest packages (candidates for removal)
SELECT name, type, ROUND(size_bytes / 1024 / 1024) as size_mb
FROM brew_packages
ORDER BY size_bytes DESC
LIMIT 20;
```
