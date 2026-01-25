-- Example usage of the DuckDB Brew Extension
-- This file demonstrates how to use the extension once it's loaded

-- Load the extension (path will depend on your build)
-- LOAD './build/release/extension/brew/brew.duckdb_extension';

-- Example 1: List all installed packages
SELECT * FROM brew_packages() LIMIT 10;

-- Example 2: Count packages by type
SELECT 
    type, 
    COUNT(*) as count,
    COUNT(*) FILTER (WHERE installed_on_request) as explicit_installs
FROM brew_packages() 
GROUP BY type;

-- Example 3: Find Python-related packages
SELECT name, version, type, description
FROM brew_packages() 
WHERE name LIKE '%python%' OR description LIKE '%Python%'
ORDER BY type, name;

-- Example 4: List all explicitly installed formulas
SELECT name, version, description
FROM brew_formulas()
WHERE installed_on_request = true
ORDER BY name;

-- Example 5: Show all casks
SELECT name, version, homepage
FROM brew_casks()
ORDER BY name;

-- Example 6: Find packages without a description
SELECT name, version, type
FROM brew_packages()
WHERE description IS NULL OR description = ''
ORDER BY type, name;

-- Example 7: Create a report of your installed tools
SELECT 
    type,
    COUNT(*) as total_packages,
    COUNT(DISTINCT version) as unique_versions
FROM brew_packages()
GROUP BY type;

-- Example 8: Export package list to CSV
COPY (
    SELECT name, version, type, homepage 
    FROM brew_packages() 
    ORDER BY type, name
) TO 'my_brew_packages.csv' (HEADER, DELIMITER ',');

-- Example 9: Find outdated packages (would need additional brew commands)
-- This is just the list of what you have installed
SELECT name, version FROM brew_packages() ORDER BY name;
