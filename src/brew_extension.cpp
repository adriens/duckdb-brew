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
#include <algorithm>

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
	int64_t installed_time;
	string tap;
	string license;
	bool installed_as_dependency;
	bool outdated;
	bool pinned;
	bool deprecated;
	bool disabled;
	bool poured_from_bottle;
	bool built_as_bottle;
	string dependencies;
	string aliases;
	string deprecation_reason;
	string disable_reason;
	string caveats;
	int64_t size_bytes;
};

static string GetStringValue(const ComplexJSON &obj, const string &key) {
	try {
		string val = obj.GetValue(key);
		if (val == "null") {
			return "";
		}
		return val;
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

static int64_t GetInt64Value(const ComplexJSON &obj, const string &key) {
	try {
		string val = obj.GetValue(key);
		return std::stoll(val);
	} catch (...) {
		return 0;
	}
}

static string GetArrayAsCommaSeparated(ComplexJSON &obj, const string &key) {
	try {
		auto &arr = obj.GetObject(key);
		string result;
		idx_t i = 0;
		while (true) {
			try {
				auto &elem = arr.GetArrayElement(i);
				string val = ComplexJSON::GetValueRecursive(elem);
				if (!result.empty()) {
					result += ", ";
				}
				result += val;
				i++;
			} catch (...) {
				break;
			}
		}
		return result;
	} catch (...) {
		return "";
	}
}

static int64_t GetPackageSize(const string &package_name, const string &package_type) {
	try {
		string base_path_cmd;
		if (package_type == "formula") {
			base_path_cmd = "brew --cellar";
		} else {
			base_path_cmd = "brew --caskroom";
		}

		string command = base_path_cmd + " 2>/dev/null";
		string base_path = ExecuteCommand(command);
		StringUtil::Trim(base_path);

		string full_path = base_path + "/" + package_name;
		string du_command = "du -sb \"" + full_path + "\" 2>/dev/null | awk '{print $1}'";
		string size_str = ExecuteCommand(du_command);
		StringUtil::Trim(size_str);

		if (size_str.empty()) {
			return 0;
		}

		return std::stoll(size_str);
	} catch (...) {
		return 0;
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
					pkg.tap = GetStringValue(formula, "tap");
					pkg.license = GetStringValue(formula, "license");

					// Status flags
					pkg.outdated = GetBoolValue(formula, "outdated");
					pkg.pinned = GetBoolValue(formula, "pinned");
					pkg.deprecated = GetBoolValue(formula, "deprecated");
					pkg.disabled = GetBoolValue(formula, "disabled");

					// Deprecation/disable info
					pkg.deprecation_reason = GetStringValue(formula, "deprecation_reason");
					pkg.disable_reason = GetStringValue(formula, "disable_reason");
					pkg.caveats = GetStringValue(formula, "caveats");

					// Dependencies and aliases
					pkg.dependencies = GetArrayAsCommaSeparated(formula, "dependencies");
					pkg.aliases = GetArrayAsCommaSeparated(formula, "aliases");

					// Get version and installed_on_request from installed array
					try {
						auto &installed = formula.GetObject("installed");
						try {
							auto &inst_info = installed.GetArrayElement(0);
							pkg.version = GetStringValue(inst_info, "version");
							pkg.installed_on_request = GetBoolValue(inst_info, "installed_on_request");
							pkg.installed_time = GetInt64Value(inst_info, "time");
							pkg.installed_as_dependency = GetBoolValue(inst_info, "installed_as_dependency");
							pkg.poured_from_bottle = GetBoolValue(inst_info, "poured_from_bottle");
							pkg.built_as_bottle = GetBoolValue(inst_info, "built_as_bottle");
						} catch (...) {
							pkg.version = "";
							pkg.installed_on_request = false;
							pkg.installed_time = 0;
							pkg.installed_as_dependency = false;
							pkg.poured_from_bottle = false;
							pkg.built_as_bottle = false;
						}
					} catch (...) {
						pkg.version = "";
						pkg.installed_on_request = false;
						pkg.installed_time = 0;
						pkg.installed_as_dependency = false;
						pkg.poured_from_bottle = false;
						pkg.built_as_bottle = false;
					}

					// Calculate package size
					pkg.size_bytes = GetPackageSize(pkg.name, pkg.type);

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
					pkg.tap = GetStringValue(cask, "tap");
					pkg.installed_time = GetInt64Value(cask, "installed_time");

					// Cask-specific defaults
					pkg.license = "";
					pkg.installed_as_dependency = false;
					pkg.poured_from_bottle = false;
					pkg.built_as_bottle = false;
					pkg.dependencies = "";

					// Status flags
					pkg.outdated = GetBoolValue(cask, "outdated");
					pkg.pinned = false; // Casks don't get pinned
					pkg.deprecated = GetBoolValue(cask, "deprecated");
					pkg.disabled = GetBoolValue(cask, "disabled");
					pkg.deprecation_reason = GetStringValue(cask, "deprecation_reason");
					pkg.disable_reason = GetStringValue(cask, "disable_reason");
					pkg.caveats = GetStringValue(cask, "caveats");
					pkg.aliases = GetArrayAsCommaSeparated(cask, "old_tokens");

					// Calculate package size
					pkg.size_bytes = GetPackageSize(pkg.name, pkg.type);

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

struct BrewDependency {
	string name;
	string dependency;
};

struct BrewDependenciesBindData : public TableFunctionData {
	vector<BrewDependency> dependencies;
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

	names.emplace_back("tap");
	return_types.emplace_back(LogicalType::VARCHAR);

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

	names.emplace_back("license");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("installed_on_request");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("installed_as_dependency");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("installed_time");
	return_types.emplace_back(LogicalType::TIMESTAMP);

	names.emplace_back("outdated");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("pinned");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("deprecated");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("disabled");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("poured_from_bottle");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("built_as_bottle");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("dependencies");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("aliases");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("deprecation_reason");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("disable_reason");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("caveats");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("size_bytes");
	return_types.emplace_back(LogicalType::BIGINT);

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

	names.emplace_back("tap");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("version");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("description");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("homepage");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("license");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("installed_on_request");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("installed_as_dependency");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("installed_time");
	return_types.emplace_back(LogicalType::TIMESTAMP);

	names.emplace_back("outdated");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("pinned");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("deprecated");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("disabled");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("poured_from_bottle");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("built_as_bottle");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("dependencies");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("aliases");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("deprecation_reason");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("disable_reason");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("caveats");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("size_bytes");
	return_types.emplace_back(LogicalType::BIGINT);

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

	names.emplace_back("tap");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("version");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("description");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("homepage");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("installed_time");
	return_types.emplace_back(LogicalType::TIMESTAMP);

	names.emplace_back("outdated");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("pinned");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("deprecated");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("disabled");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("poured_from_bottle");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("built_as_bottle");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("dependencies");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("aliases");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("deprecation_reason");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("disable_reason");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("caveats");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("size_bytes");
	return_types.emplace_back(LogicalType::BIGINT);

	return std::move(result);
}

static void BrewPackagesFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->CastNoConst<BrewPackagesBindData>();
	auto &local_state = data_p.local_state->Cast<BrewLocalState>();
	idx_t count = 0;

	for (idx_t i = local_state.batch_index; i < data.packages.size() && count < STANDARD_VECTOR_SIZE; i++) {
		auto &pkg = data.packages[i];

		output.SetValue(0, count, pkg.tap);
		output.SetValue(1, count, pkg.name);
		output.SetValue(2, count, pkg.version);
		output.SetValue(3, count, pkg.type);
		output.SetValue(4, count, pkg.description);
		output.SetValue(5, count, pkg.homepage);
		output.SetValue(6, count, pkg.license);
		output.SetValue(7, count, pkg.installed_on_request);
		output.SetValue(8, count, pkg.installed_as_dependency);
		output.SetValue(9, count, Value::TIMESTAMP(Timestamp::FromEpochSeconds(pkg.installed_time)));
		output.SetValue(10, count, pkg.outdated);
		output.SetValue(11, count, pkg.pinned);
		output.SetValue(12, count, pkg.deprecated);
		output.SetValue(13, count, pkg.disabled);
		output.SetValue(14, count, pkg.poured_from_bottle);
		output.SetValue(15, count, pkg.built_as_bottle);
		output.SetValue(16, count, pkg.dependencies.empty() ? Value() : Value(pkg.dependencies));
		output.SetValue(17, count, pkg.aliases.empty() ? Value() : Value(pkg.aliases));
		output.SetValue(18, count, pkg.deprecation_reason.empty() ? Value() : Value(pkg.deprecation_reason));
		output.SetValue(19, count, pkg.disable_reason.empty() ? Value() : Value(pkg.disable_reason));
		output.SetValue(20, count, pkg.caveats.empty() ? Value() : Value(pkg.caveats));
		output.SetValue(21, count, pkg.size_bytes);

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

		output.SetValue(0, count, pkg.tap);
		output.SetValue(1, count, pkg.name);
		output.SetValue(2, count, pkg.version);
		output.SetValue(3, count, pkg.description);
		output.SetValue(4, count, pkg.homepage);
		output.SetValue(5, count, pkg.license);
		output.SetValue(6, count, pkg.installed_on_request);
		output.SetValue(7, count, pkg.installed_as_dependency);
		output.SetValue(8, count, Value::TIMESTAMP(Timestamp::FromEpochSeconds(pkg.installed_time)));
		output.SetValue(9, count, pkg.outdated);
		output.SetValue(10, count, pkg.pinned);
		output.SetValue(11, count, pkg.deprecated);
		output.SetValue(12, count, pkg.disabled);
		output.SetValue(13, count, pkg.poured_from_bottle);
		output.SetValue(14, count, pkg.built_as_bottle);
		output.SetValue(15, count, pkg.dependencies.empty() ? Value() : Value(pkg.dependencies));
		output.SetValue(16, count, pkg.aliases.empty() ? Value() : Value(pkg.aliases));
		output.SetValue(17, count, pkg.deprecation_reason.empty() ? Value() : Value(pkg.deprecation_reason));
		output.SetValue(18, count, pkg.disable_reason.empty() ? Value() : Value(pkg.disable_reason));
		output.SetValue(19, count, pkg.caveats.empty() ? Value() : Value(pkg.caveats));
		output.SetValue(20, count, pkg.size_bytes);

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

		output.SetValue(0, count, pkg.tap);
		output.SetValue(1, count, pkg.name);
		output.SetValue(2, count, pkg.version);
		output.SetValue(3, count, pkg.description);
		output.SetValue(4, count, pkg.homepage);
		output.SetValue(5, count, Value::TIMESTAMP(Timestamp::FromEpochSeconds(pkg.installed_time)));
		output.SetValue(6, count, pkg.outdated);
		output.SetValue(7, count, pkg.pinned);
		output.SetValue(8, count, pkg.deprecated);
		output.SetValue(9, count, pkg.disabled);
		output.SetValue(10, count, pkg.poured_from_bottle);
		output.SetValue(11, count, pkg.built_as_bottle);
		output.SetValue(12, count, pkg.dependencies.empty() ? Value() : Value(pkg.dependencies));
		output.SetValue(13, count, pkg.aliases.empty() ? Value() : Value(pkg.aliases));
		output.SetValue(14, count, pkg.deprecation_reason.empty() ? Value() : Value(pkg.deprecation_reason));
		output.SetValue(15, count, pkg.disable_reason.empty() ? Value() : Value(pkg.disable_reason));
		output.SetValue(16, count, pkg.caveats.empty() ? Value() : Value(pkg.caveats));
		output.SetValue(17, count, pkg.size_bytes);

		count++;
	}

	local_state.batch_index += count;
	output.SetCardinality(count);
}

static unique_ptr<FunctionData> BrewDependenciesBind(ClientContext &context, TableFunctionBindInput &input,
                                                     vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<BrewDependenciesBindData>();

	string brew_output;
	try {
		brew_output = ExecuteCommand("brew info --json=v2 --installed 2>/dev/null");
	} catch (IOException &e) {
		throw IOException("Failed to execute brew command. Is Homebrew installed?");
	}

	auto all_packages = ParseBrewJSON(brew_output);

	// Process each package and split its dependencies
	for (auto &pkg : all_packages) {
		if (!pkg.dependencies.empty()) {
			// Split dependencies by ", "
			string deps = pkg.dependencies;
			size_t start = 0;
			size_t end = deps.find(", ");

			while (end != string::npos) {
				BrewDependency dep;
				dep.name = pkg.name;
				dep.dependency = deps.substr(start, end - start);
				result->dependencies.push_back(dep);

				start = end + 2; // Skip ", "
				end = deps.find(", ", start);
			}

			// Add the last dependency
			BrewDependency dep;
			dep.name = pkg.name;
			dep.dependency = deps.substr(start);
			result->dependencies.push_back(dep);
		}
	}

	// Sort by name
	std::sort(result->dependencies.begin(), result->dependencies.end(),
	          [](const BrewDependency &a, const BrewDependency &b) { return a.name < b.name; });

	names.emplace_back("name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("dependency");
	return_types.emplace_back(LogicalType::VARCHAR);

	return std::move(result);
}

static void BrewDependenciesFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->CastNoConst<BrewDependenciesBindData>();
	auto &local_state = data_p.local_state->Cast<BrewLocalState>();
	idx_t count = 0;

	for (idx_t i = local_state.batch_index; i < data.dependencies.size() && count < STANDARD_VECTOR_SIZE; i++) {
		auto &dep = data.dependencies[i];

		output.SetValue(0, count, dep.name);
		output.SetValue(1, count, dep.dependency);

		count++;
	}

	local_state.batch_index += count;
	output.SetCardinality(count);
}

struct BrewConfigEntry {
	string name;
	string value;
	string category;
};

struct BrewConfigBindData : public TableFunctionData {
	vector<BrewConfigEntry> config;
};

static void BrewVersionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	string version_str = ExecuteCommand("brew --version 2>/dev/null | head -n 1 | cut -d' ' -f2");
	StringUtil::Trim(version_str);
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	auto result_data = ConstantVector::GetData<string_t>(result);
	result_data[0] = StringVector::AddString(result, version_str);
}

static string CategorizeConfig(const string &name) {
	if (StringUtil::StartsWith(name, "HOMEBREW_")) {
		return "ENVIRONMENT";
	}
	if (name == "OS" || name == "CPU" || name == "Kernel" || name == "Host glibc" || name == "Host libstdc++") {
		return "SYSTEM";
	}
	if (name == "ORIGIN" || name == "HEAD" || name == "Last commit" || name == "Branch" || name == "Core tap JSON") {
		return "META";
	}
	if (name.find("/") != string::npos || name == "Git" || name == "Curl" || name == "Clang" || name == "Homebrew Ruby" || name == "glibc" || name == "gcc" || name == "gcc@12" || name == "xorg") {
		return "BINARY";
	}
	return "OTHER";
}

static unique_ptr<FunctionData> BrewConfigBind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<BrewConfigBindData>();

	string config_output = ExecuteCommand("brew config 2>/dev/null");
	auto lines = StringUtil::Split(config_output, "\n");

	for (auto &line : lines) {
		if (line.empty()) {
			continue;
		}
		auto colon_pos = line.find(": ");
		if (colon_pos != string::npos) {
			BrewConfigEntry entry;
			entry.name = line.substr(0, colon_pos);
			entry.value = line.substr(colon_pos + 2);
			StringUtil::Trim(entry.name);
			StringUtil::Trim(entry.value);
			if (!entry.name.empty()) {
				entry.category = CategorizeConfig(entry.name);
				result->config.push_back(entry);
			}
		}
	}

	names.emplace_back("name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("value");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("category");
	return_types.emplace_back(LogicalType::VARCHAR);

	return std::move(result);
}

static void BrewConfigFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->CastNoConst<BrewConfigBindData>();
	auto &local_state = data_p.local_state->Cast<BrewLocalState>();
	idx_t count = 0;

	for (idx_t i = local_state.batch_index; i < data.config.size() && count < STANDARD_VECTOR_SIZE; i++) {
		auto &entry = data.config[i];

		output.SetValue(0, count, entry.name);
		output.SetValue(1, count, entry.value);
		output.SetValue(2, count, entry.category);

		count++;
	}

	local_state.batch_index += count;
	output.SetCardinality(count);
}

struct BrewDoctorEntry {
	string warning;
	string severity;
	string category;
};

struct BrewDoctorBindData : public TableFunctionData {
	vector<BrewDoctorEntry> entries;
};

static string CategorizeDoctor(const string &warning) {
	string lower = StringUtil::Lower(warning);
	if (lower.find("path") != string::npos) {
		return "PATH";
	}
	if (lower.find("permission") != string::npos || lower.find("writable") != string::npos) {
		return "PERMISSIONS";
	}
	if (lower.find("outdated") != string::npos || lower.find("update") != string::npos) {
		return "OUTDATED";
	}
	if (lower.find("dependency") != string::npos || lower.find("missing") != string::npos) {
		return "DEPENDENCIES";
	}
	if (lower.find("unlinked") != string::npos) {
		return "LINKING";
	}
	if (lower.find("config") != string::npos || lower.find("script") != string::npos) {
		return "CONFIGURATION";
	}
	return "OTHER";
}

static unique_ptr<FunctionData> BrewDoctorBind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<BrewDoctorBindData>();

	// brew doctor often returns non-zero if there are warnings, and outputs to stderr
	string doctor_output = ExecuteCommand("brew doctor 2>&1 || true");
	
	if (doctor_output.find("Your system is ready to brew") == string::npos) {
		// Split by "Warning: " to get individual issues
		auto parts = StringUtil::Split(doctor_output, "Warning: ");
		for (size_t i = 1; i < parts.size(); i++) {
			BrewDoctorEntry entry;
			entry.warning = "Warning: " + parts[i];
			StringUtil::Trim(entry.warning);
			if (!entry.warning.empty()) {
				entry.severity = "WARNING";
				entry.category = CategorizeDoctor(entry.warning);
				result->entries.push_back(entry);
			}
		}
		// Errors are rare but can happen
		auto error_parts = StringUtil::Split(doctor_output, "Error: ");
		for (size_t i = 1; i < error_parts.size(); i++) {
			BrewDoctorEntry entry;
			entry.warning = "Error: " + error_parts[i];
			StringUtil::Trim(entry.warning);
			if (!entry.warning.empty()) {
				entry.severity = "ERROR";
				entry.category = CategorizeDoctor(entry.warning);
				result->entries.push_back(entry);
			}
		}

		// If we couldn't parse but output is not empty and not "ready"
		if (result->entries.empty() && !doctor_output.empty()) {
			BrewDoctorEntry entry;
			entry.warning = doctor_output;
			StringUtil::Trim(entry.warning);
			entry.severity = "UNKNOWN";
			entry.category = CategorizeDoctor(entry.warning);
			result->entries.push_back(entry);
		}
	}

	names.emplace_back("warning");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("severity");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("category");
	return_types.emplace_back(LogicalType::VARCHAR);

	return std::move(result);
}

static void BrewDoctorFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->CastNoConst<BrewDoctorBindData>();
	auto &local_state = data_p.local_state->Cast<BrewLocalState>();
	idx_t count = 0;

	for (idx_t i = local_state.batch_index; i < data.entries.size() && count < STANDARD_VECTOR_SIZE; i++) {
		auto &entry = data.entries[i];
		output.SetValue(0, count, entry.warning);
		output.SetValue(1, count, entry.severity);
		output.SetValue(2, count, entry.category);
		count++;
	}

	local_state.batch_index += count;
	output.SetCardinality(count);
}

static void LoadInternal(ExtensionLoader &loader) {
	ScalarFunction brew_version("brew_version", {}, LogicalType::VARCHAR, BrewVersionFunction);
	loader.RegisterFunction(brew_version);

	TableFunction brew_config("brew_config", {}, BrewConfigFunction, BrewConfigBind);
	brew_config.init_local = BrewInitLocal;
	loader.RegisterFunction(brew_config);

	TableFunction brew_doctor("brew_doctor", {}, BrewDoctorFunction, BrewDoctorBind);
	brew_doctor.init_local = BrewInitLocal;
	loader.RegisterFunction(brew_doctor);

	TableFunction brew_packages("brew_packages", {}, BrewPackagesFunction, BrewPackagesBind);
	brew_packages.init_local = BrewInitLocal;
	loader.RegisterFunction(brew_packages);

	TableFunction brew_formulas("brew_formulas", {}, BrewFormulasFunction, BrewFormulasBind);
	brew_formulas.init_local = BrewInitLocal;
	loader.RegisterFunction(brew_formulas);

	TableFunction brew_casks("brew_casks", {}, BrewCasksFunction, BrewCasksBind);
	brew_casks.init_local = BrewInitLocal;
	loader.RegisterFunction(brew_casks);

	TableFunction brew_dependencies("brew_dependencies", {}, BrewDependenciesFunction, BrewDependenciesBind);
	brew_dependencies.init_local = BrewInitLocal;
	loader.RegisterFunction(brew_dependencies);
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
