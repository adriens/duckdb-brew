#define DUCKDB_EXTENSION_MAIN

#include "brew_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>
#include <cstdio>
#include <memory>
#include <array>

namespace duckdb {

// Helper function to execute a shell command and capture output
static string ExecuteCommand(const string &command) {
std::array<char, 128> buffer;
std::string result;

std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"), pclose);
if (!pipe) {
throw IOException("Failed to execute command: " + command);
}

while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
result += buffer.data();
}

return result;
}

// Simple JSON parser for brew output
struct BrewPackage {
string name;
string version;
string description;
string homepage;
string type;
bool installed_on_request;
};

static string GetStringValue(const ComplexJSON &obj, const string &key) {
try {
return obj.GetValue(key);
} catch (...) {
return "";
}
}

static bool GetBoolValue(const ComplexJSON &obj, const string &key) {
try {
string val = obj.GetValue(key);
return val == "true";
} catch (...) {
return false;
}
}

static vector<BrewPackage> ParseBrewJSON(const string &json_output) {
vector<BrewPackage> packages;

try {
auto json = StringUtil::ParseJSONMap(json_output);

// Parse formulas
try {
auto &formulae = json->GetObject("formulae");
idx_t i = 0;
while (true) {
try {
BrewPackage pkg;
pkg.type = "formula";

auto &formula = formulae.GetArrayElement(i);
pkg.name = GetStringValue(formula, "name");
pkg.description = GetStringValue(formula, "desc");
pkg.homepage = GetStringValue(formula, "homepage");

// Get version and installed_on_request from installed array
try {
auto &installed = formula.GetObject("installed");
try {
auto &inst_info = installed.GetArrayElement(0);
pkg.version = GetStringValue(inst_info, "version");
pkg.installed_on_request = GetBoolValue(inst_info, "installed_on_request");
} catch (...) {
pkg.version = "";
pkg.installed_on_request = false;
}
} catch (...) {
pkg.version = "";
pkg.installed_on_request = false;
}

if (!pkg.name.empty()) {
packages.push_back(pkg);
}
i++;
} catch (...) {
// No more elements in array
break;
}
}
} catch (...) {
// No formulas or error parsing
}

// Parse casks
try {
auto &casks = json->GetObject("casks");
idx_t i = 0;
while (true) {
try {
BrewPackage pkg;
pkg.type = "cask";
pkg.installed_on_request = true;

auto &cask = casks.GetArrayElement(i);
pkg.name = GetStringValue(cask, "token");
pkg.description = GetStringValue(cask, "desc");
pkg.homepage = GetStringValue(cask, "homepage");
pkg.version = GetStringValue(cask, "version");

if (!pkg.name.empty()) {
packages.push_back(pkg);
}
i++;
} catch (...) {
// No more elements in array
break;
}
}
} catch (...) {
// No casks or error parsing
}

} catch (std::exception &e) {
throw IOException("Failed to parse brew JSON output: " + string(e.what()));
}

return packages;
}

struct BrewPackagesBindData : public TableFunctionData {
vector<BrewPackage> packages;
string filter_type;
};

struct BrewLocalState : public LocalTableFunctionState {
idx_t batch_index = 0;
};

static unique_ptr<LocalTableFunctionState> BrewInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                                                           GlobalTableFunctionState *global_state) {
auto result = make_uniq<BrewLocalState>();
result->batch_index = 0;
return std::move(result);
}

static unique_ptr<FunctionData> BrewPackagesBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
auto result = make_uniq<BrewPackagesBindData>();
result->filter_type = "all";

string brew_output;
try {
brew_output = ExecuteCommand("brew info --json=v2 --installed 2>/dev/null");
} catch (IOException &e) {
throw IOException("Failed to execute brew command. Is Homebrew installed?");
}

result->packages = ParseBrewJSON(brew_output);

names.emplace_back("name");
return_types.emplace_back(LogicalType::VARCHAR);

names.emplace_back("version");
return_types.emplace_back(LogicalType::VARCHAR);

names.emplace_back("type");
return_types.emplace_back(LogicalType::VARCHAR);

names.emplace_back("description");
return_types.emplace_back(LogicalType::VARCHAR);

names.emplace_back("homepage");
return_types.emplace_back(LogicalType::VARCHAR);

names.emplace_back("installed_on_request");
return_types.emplace_back(LogicalType::BOOLEAN);

return std::move(result);
}

static unique_ptr<FunctionData> BrewFormulasBind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<string> &names) {
auto result = make_uniq<BrewPackagesBindData>();
result->filter_type = "formula";

string brew_output;
try {
brew_output = ExecuteCommand("brew info --json=v2 --installed 2>/dev/null");
} catch (IOException &e) {
throw IOException("Failed to execute brew command. Is Homebrew installed?");
}

auto all_packages = ParseBrewJSON(brew_output);
for (auto &pkg : all_packages) {
if (pkg.type == "formula") {
result->packages.push_back(pkg);
}
}

names.emplace_back("name");
return_types.emplace_back(LogicalType::VARCHAR);

names.emplace_back("version");
return_types.emplace_back(LogicalType::VARCHAR);

names.emplace_back("description");
return_types.emplace_back(LogicalType::VARCHAR);

names.emplace_back("homepage");
return_types.emplace_back(LogicalType::VARCHAR);

names.emplace_back("installed_on_request");
return_types.emplace_back(LogicalType::BOOLEAN);

return std::move(result);
}

static unique_ptr<FunctionData> BrewCasksBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
auto result = make_uniq<BrewPackagesBindData>();
result->filter_type = "cask";

string brew_output;
try {
brew_output = ExecuteCommand("brew info --json=v2 --installed 2>/dev/null");
} catch (IOException &e) {
throw IOException("Failed to execute brew command. Is Homebrew installed?");
}

auto all_packages = ParseBrewJSON(brew_output);
for (auto &pkg : all_packages) {
if (pkg.type == "cask") {
result->packages.push_back(pkg);
}
}

names.emplace_back("name");
return_types.emplace_back(LogicalType::VARCHAR);

names.emplace_back("version");
return_types.emplace_back(LogicalType::VARCHAR);

names.emplace_back("description");
return_types.emplace_back(LogicalType::VARCHAR);

names.emplace_back("homepage");
return_types.emplace_back(LogicalType::VARCHAR);

return std::move(result);
}

static void BrewPackagesFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
auto &data = data_p.bind_data->CastNoConst<BrewPackagesBindData>();
auto &local_state = data_p.local_state->Cast<BrewLocalState>();
idx_t count = 0;

for (idx_t i = local_state.batch_index; i < data.packages.size() && count < STANDARD_VECTOR_SIZE; i++) {
auto &pkg = data.packages[i];

output.SetValue(0, count, pkg.name);
output.SetValue(1, count, pkg.version);
output.SetValue(2, count, pkg.type);
output.SetValue(3, count, pkg.description);
output.SetValue(4, count, pkg.homepage);
output.SetValue(5, count, pkg.installed_on_request);

count++;
}

local_state.batch_index += count;
output.SetCardinality(count);
}

static void BrewFormulasFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
auto &data = data_p.bind_data->CastNoConst<BrewPackagesBindData>();
auto &local_state = data_p.local_state->Cast<BrewLocalState>();
idx_t count = 0;

for (idx_t i = local_state.batch_index; i < data.packages.size() && count < STANDARD_VECTOR_SIZE; i++) {
auto &pkg = data.packages[i];

output.SetValue(0, count, pkg.name);
output.SetValue(1, count, pkg.version);
output.SetValue(2, count, pkg.description);
output.SetValue(3, count, pkg.homepage);
output.SetValue(4, count, pkg.installed_on_request);

count++;
}

local_state.batch_index += count;
output.SetCardinality(count);
}

static void BrewCasksFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
auto &data = data_p.bind_data->CastNoConst<BrewPackagesBindData>();
auto &local_state = data_p.local_state->Cast<BrewLocalState>();
idx_t count = 0;

for (idx_t i = local_state.batch_index; i < data.packages.size() && count < STANDARD_VECTOR_SIZE; i++) {
auto &pkg = data.packages[i];

output.SetValue(0, count, pkg.name);
output.SetValue(1, count, pkg.version);
output.SetValue(2, count, pkg.description);
output.SetValue(3, count, pkg.homepage);

count++;
}

local_state.batch_index += count;
output.SetCardinality(count);
}

static void LoadInternal(ExtensionLoader &loader) {
TableFunction brew_packages("brew_packages", {}, BrewPackagesFunction, BrewPackagesBind);
brew_packages.init_local = BrewInitLocal;
loader.RegisterFunction(brew_packages);

TableFunction brew_formulas("brew_formulas", {}, BrewFormulasFunction, BrewFormulasBind);
brew_formulas.init_local = BrewInitLocal;
loader.RegisterFunction(brew_formulas);

TableFunction brew_casks("brew_casks", {}, BrewCasksFunction, BrewCasksBind);
brew_casks.init_local = BrewInitLocal;
loader.RegisterFunction(brew_casks);
}

void BrewExtension::Load(ExtensionLoader &loader) {
LoadInternal(loader);
}

std::string BrewExtension::Name() {
return "brew";
}

std::string BrewExtension::Version() const {
#ifdef EXT_VERSION_BREW
return EXT_VERSION_BREW;
#else
return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(brew, loader) {
duckdb::LoadInternal(loader);
}
}
