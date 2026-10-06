#include "NoteEvaluator.h"
#include "QalculateScript.h"
#include "../libqalculate/Function.h"

#include <cctype>
#include <algorithm>
#include <map>
#include <cstdio>
#include <cstdlib>

namespace qalc_notes {

static bool notes_debug_enabled() {
	const char *value = std::getenv("QALCULATE_NOTES_DEBUG");
	return value && value[0] == '1';
}

static void notes_debug(const char *path, const std::string &expression, bool calculated, bool undefined, bool unknowns) {
	if(!notes_debug_enabled()) return;
	std::fprintf(stderr, "[notes-debug] path=%s unknowns=%s calculated=%s undefined=%s expression=%s\n",
		path, unknowns ? "on" : "off", calculated ? "yes" : "no", undefined ? "yes" : "no", expression.c_str());
}

static unsigned int first_body_line(const qalc_script::Statement &statement) {
	if(statement.body.empty()) return statement.line;
	return first_body_line(statement.body.front());
}

static bool is_comment_or_empty(const std::string &line) {
	for(char c : line) {
		if(!std::isspace(static_cast<unsigned char>(c))) return c == '#' || (c == '/' && line.find("//") == 0);
	}
	return true;
}

static bool contains_string_result(const std::string &value) {
	if(value.find('"') != std::string::npos) return true;
	// Qalculate prints a symbolic unknown as a single-letter token in quotes
	// (for example 'w'). That is not a string literal. Keep real quoted text
	// and quoted lists classified as string results.
	if(value.size() >= 3 && value.front() == '\'' && value.back() == '\'') {
		const std::string inner = value.substr(1, value.size() - 2);
		if(inner.size() == 1 && (std::isalpha(static_cast<unsigned char>(inner[0])) || inner[0] == '_')) return false;
		return true;
	}
	return false;
}

static void normalize_symbolic_display(std::string &value) {
	for(size_t i = 0; i + 2 < value.size();) {
		if(value[i] == '\'' && value[i + 2] == '\'' && (std::isalpha(static_cast<unsigned char>(value[i + 1])) || value[i + 1] == '_')) {
			value.erase(i, 1);
			value.erase(i + 1, 1);
			i += 1;
		} else {
			++i;
		}
	}
}

static bool contains_identifier(const std::string &expression, const std::string &name) {
	size_t position = 0;
	while((position = expression.find(name, position)) != std::string::npos) {
		const bool left = position == 0 || !(std::isalnum(static_cast<unsigned char>(expression[position - 1])) || expression[position - 1] == '_');
		const size_t end = position + name.size();
		const bool right = end == expression.size() || !(std::isalnum(static_cast<unsigned char>(expression[end])) || expression[end] == '_');
		if(left && right) return true;
		position = end;
	}
	return false;
}

static bool define_function(Calculator &calculator, std::string source) {
	const size_t comment = source.find("//");
	if(comment != std::string::npos) source.erase(comment);
	const size_t open = source.find('('), close = source.find(')', open), equals = source.find('=', close);
	if(open == std::string::npos || close == std::string::npos || equals == std::string::npos) return false;
	std::string name = source.substr(0, open), parameters = source.substr(open + 1, close - open - 1), formula = source.substr(equals + 1);
	auto trim = [](std::string &value) { const size_t first = value.find_first_not_of(" \t"); const size_t last = value.find_last_not_of(" \t"); value = first == std::string::npos ? "" : value.substr(first, last - first + 1); };
	trim(name); trim(parameters); trim(formula);
	if(name.empty() || formula.empty() || !calculator.functionNameIsValid(name)) return false;
	std::vector<std::string> args;
	for(size_t start = 0; start <= parameters.size();) {
		const size_t comma = parameters.find(',', start);
		std::string arg = parameters.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
		trim(arg);
		if(arg.empty() || !calculator.variableNameIsValid(arg)) return false;
		args.push_back(arg);
		if(comma == std::string::npos) break;
		start = comma + 1;
	}
	std::vector<size_t> order(args.size());
	for(size_t i = 0; i < args.size(); ++i) order[i] = i;
	std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return args[a].size() > args[b].size(); });
	for(size_t index : order) {
		const std::string placeholder = "\\" + std::string(1, index < 3 ? char('x' + index) : char('a' + index - 3));
		size_t position = 0;
		while((position = formula.find(args[index], position)) != std::string::npos) { formula.replace(position, args[index].size(), placeholder); position += placeholder.size(); }
	}
	std::string validation_formula = formula;
	for(char placeholder : {'x', 'y', 'z', 'a', 'b', 'c'}) {
		const std::string token = "\\" + std::string(1, placeholder);
		size_t position = 0;
		while((position = validation_formula.find(token, position)) != std::string::npos) {
			validation_formula.replace(position, token.size(), "1");
			position += 1;
		}
	}
	for(size_t position = 0; position < validation_formula.size();) {
		if(!(std::isalpha(static_cast<unsigned char>(validation_formula[position])) || validation_formula[position] == '_')) { ++position; continue; }
		const size_t start = position++;
		while(position < validation_formula.size() && (std::isalnum(static_cast<unsigned char>(validation_formula[position])) || validation_formula[position] == '_')) ++position;
		const std::string identifier = validation_formula.substr(start, position - start);
		if(!calculator.variableNameTaken(identifier) && !calculator.functionNameTaken(identifier)) return false;
	}
	MathStructure validation;
	calculator.clearMessages();
	if(!calculator.calculate(&validation, validation_formula, 500, EvaluationOptions()) || validation.isUndefined()) return false;
	if(contains_string_result(validation.print(PrintOptions(), false, false, TAG_TYPE_TERMINAL))) return false;
	if(calculator.functionNameTaken(name)) {
		MathFunction *existing = calculator.getActiveFunction(name, true);
		if(!existing || !existing->isLocal() || existing->isBuiltin()) return false;
		UserFunction *user = dynamic_cast<UserFunction *>(existing);
		if(!user) return false;
		user->setFormula(formula, args.size());
		user->setChanged(true);
		return true;
	}
	calculator.addFunction(new UserFunction("", name, formula))->setChanged(true);
	return true;
}

std::vector<LineResult> evaluate_note(Calculator &calculator, const std::vector<std::string> &lines) {
	std::vector<LineResult> results;
	results.reserve(lines.size());
	// Re-evaluation must start from a clean note state. The GUI reuses one
	// Calculator instance, so user variables/functions from an older document
	// would otherwise turn symbols such as x and q into stale constants.
	calculator.resetVariables();
	calculator.resetFunctions();
	calculator.loadGlobalDefinitions();
	EvaluationOptions evaluation_options;
	// Treat symbols that have never been assigned as symbolic Qalculate
	// unknowns. Explicit empty assignments and invalid definitions are tracked
	// separately below and remain application-level undefined values.
	evaluation_options.parse_options.unknowns_enabled = true;
	PrintOptions print_options;
	std::vector<bool> function_lines(lines.size(), false), defined_functions(lines.size(), false);
	for(size_t i = 0; i < lines.size(); ++i) {
		const std::string &line = lines[i];
		const size_t open = line.find('(');
		const size_t close = open == std::string::npos ? std::string::npos : line.find(')', open);
		const size_t equals = line.find('=');
		if(open != std::string::npos && close != std::string::npos && equals != std::string::npos && open < close && close < equals) {
			function_lines[i] = true;
			defined_functions[i] = define_function(calculator, line);
			calculator.clearMessages();
		}
	}
	bool script = false;
	for(const auto &line : lines) {
		std::string trimmed = line;
		const size_t first = trimmed.find_first_not_of(" \t");
		if(first != std::string::npos) trimmed.erase(0, first);
		if(!trimmed.empty() && trimmed.back() == ':') { script = true; break; }
	}
	if(script) {
		results.resize(lines.size());
		std::vector<std::string> source = lines;
		for(size_t i = 0; i < source.size(); ++i) if(function_lines[i]) source[i] = "# function definition";
		qalc_script::Parser parser;
		std::vector<qalc_script::Statement> program;
		std::string error;
		if(!parser.parse(source, program, error)) {
			// While a user is typing a block header, the script is temporarily
			// incomplete. Preserve ordinary line results instead of erasing the
			// whole note until the indented body exists.
			std::vector<std::string> partial = lines;
			for(size_t i = 0; i < partial.size(); ++i) {
				std::string trimmed = partial[i];
				const size_t first = trimmed.find_first_not_of(" \t");
				if(first != std::string::npos) trimmed.erase(0, first);
				if((!trimmed.empty() && trimmed.back() == ':') || (!partial[i].empty() && (partial[i][0] == ' ' || partial[i][0] == '\t'))) partial[i].clear();
			}
			return evaluate_note(calculator, partial);
		}
		std::vector<std::string> baseline_source = lines;
		for(size_t i = 0; i < baseline_source.size(); ++i) {
			std::string trimmed = baseline_source[i];
			const size_t first = trimmed.find_first_not_of(" \t");
			if(first != std::string::npos) trimmed.erase(0, first);
			if((!trimmed.empty() && trimmed.back() == ':') || (!baseline_source[i].empty() && (baseline_source[i][0] == ' ' || baseline_source[i][0] == '\t'))) baseline_source[i].clear();
		}
		// The baseline pass is only used for ordinary, non-script lines. Reset
		// the shared calculator afterwards before executing the real script;
		// libqalculate expects one Calculator instance per process.
		const std::vector<LineResult> baseline = evaluate_note(calculator, baseline_source);
		calculator.resetVariables();
		calculator.resetFunctions();
		calculator.loadGlobalDefinitions();
		for(size_t i = 0; i < lines.size(); ++i) {
			if(function_lines[i]) defined_functions[i] = define_function(calculator, lines[i]);
		}
		std::vector<std::vector<std::string>> output(lines.size());
		qalc_script::Executor::Environment environment;
		qalc_script::Executor executor;
		unsigned int failed_line = 0;
		std::vector<std::vector<std::string>> *active_output = nullptr;
		qalc_script::Executor::Evaluate evaluate = [&](const std::string &expression, const qalc_script::Executor::Environment &scope, qalc_script::Value &value, std::string &evaluation_error, bool display) {
			std::string evaluated = expression;
			const bool differentiated_expression = evaluated.rfind("diff(", 0) == 0;
			for(const auto &entry : scope) {
				if(entry.second.undefined && contains_identifier(evaluated, entry.first)) {
					evaluation_error = "undefined";
					return false;
				}
				size_t position = 0;
				while((position = evaluated.find(entry.first, position)) != std::string::npos) {
					const size_t comma = differentiated_expression ? evaluated.find(',') : std::string::npos;
					if(comma != std::string::npos && position >= comma) break;
					const bool left = position == 0 || (!std::isalnum(static_cast<unsigned char>(evaluated[position - 1])) && evaluated[position - 1] != '_');
					size_t end = position + entry.first.size();
					const bool right = end == evaluated.size() || (!std::isalnum(static_cast<unsigned char>(evaluated[end])) && evaluated[end] != '_');
					if(left && right) { evaluated.replace(position, entry.first.size(), entry.second.scalar); position += entry.second.scalar.size(); } else position = end;
				}
			}
			MathStructure value_structure;
			bool calculated = calculator.calculate(&value_structure, evaluated, 500, evaluation_options);
			notes_debug("script", evaluated, calculated, value_structure.isUndefined(), evaluation_options.parse_options.unknowns_enabled);
			if((!calculated || value_structure.isUndefined()) && evaluated.rfind("diff(", 0) == 0 && evaluated.find(',') == std::string::npos) {
				const size_t begin = evaluated.find('(') + 1;
				const size_t end = evaluated.rfind(')');
				if(end > begin) {
					const std::string inner = evaluated.substr(begin, end - begin);
					std::string variable;
					for(size_t p = 0; p < inner.size(); ++p) {
						if(std::isalpha(static_cast<unsigned char>(inner[p])) || inner[p] == '_') {
							size_t e = p + 1;
							while(e < inner.size() && (std::isalnum(static_cast<unsigned char>(inner[e])) || inner[e] == '_')) ++e;
							const std::string candidate = inner.substr(p, e - p);
							if(candidate != "diff") { variable = candidate; break; }
							p = e - 1;
						}
					}
					if(!variable.empty()) {
						const bool aliased_variable = variable == "q" || variable == "w";
						const std::string differentiation_variable = aliased_variable ? "notes_diff_symbol" : variable;
						std::string differentiated_inner = inner;
						if(aliased_variable) {
							size_t qpos = 0;
							while((qpos = differentiated_inner.find(variable, qpos)) != std::string::npos) {
								const bool left = qpos == 0 || !(std::isalnum(static_cast<unsigned char>(differentiated_inner[qpos - 1])) || differentiated_inner[qpos - 1] == '_');
								const size_t qend = qpos + variable.size();
								const bool right = qend == differentiated_inner.size() || !(std::isalnum(static_cast<unsigned char>(differentiated_inner[qend])) || differentiated_inner[qend] == '_');
								if(left && right) { differentiated_inner.replace(qpos, variable.size(), differentiation_variable); qpos += differentiation_variable.size(); } else ++qpos;
							}
						}
						const std::string explicit_diff = "diff(" + differentiated_inner + "," + differentiation_variable + ")";
						calculator.clearMessages();
						calculated = calculator.calculate(&value_structure, explicit_diff, 500, evaluation_options);
					}
				}
			}
			if(!calculated || value_structure.isUndefined()) { evaluation_error = "undefined"; return false; }
			value.scalar = value_structure.print(print_options, false, false, TAG_TYPE_TERMINAL);
			normalize_symbolic_display(value.scalar);
			const std::string alias = "notes_diff_symbol";
			for(const std::string &symbol : {std::string("q"), std::string("w")}) {
				size_t alias_pos = 0;
				while((alias_pos = value.scalar.find(alias, alias_pos)) != std::string::npos) { value.scalar.replace(alias_pos, alias.size(), symbol); alias_pos += symbol.size(); }
			}
			value.sequence = false;
			if(display && active_output && !value.scalar.empty() && executor.currentLine() > 0 && executor.currentLine() <= output.size())
				(*active_output)[executor.currentLine() - 1].push_back(value.scalar);
			return true;
		};
		active_output = &output;
		if(!executor.execute(program, evaluate, error, environment)) {
			failed_line = executor.currentLine();
			if(failed_line > 0 && failed_line <= results.size()) {
				results[failed_line - 1].has_error = true;
				// Do not expose values calculated from the calculator's previous
				// pass after a script condition fails. Those values are stale.
			}
		}
		for(size_t i = 0; i < results.size(); ++i) {
			if(function_lines[i]) results[i].display = defined_functions[i] ? "defined" : "undefined";
			else if(!output[i].empty()) {
				results[i].display = output[i].size() == 1 ? output[i][0] : "[";
				for(size_t value = output[i].size() > 1 ? 0 : output[i].size(); value < output[i].size(); ++value) {
					if(value > 0) results[i].display += " · ";
					results[i].display += output[i][value];
				}
				if(output[i].size() > 1) results[i].display += "]";
			}
			else if(i < baseline.size()) results[i] = baseline[i];
		}
		// A failed script statement is local to that statement. Do not mark
		// later or unrelated baseline lines undefined: ordinary note lines have
		// independent scope and their already-computed results remain valid.
		for(size_t i = 0; i < results.size(); ++i) {
			if(results[i].has_error) results[i].display = "error";
			else if(contains_string_result(results[i].display)) results[i].display = "undefined";
		}
		return results;
	}
	std::vector<std::string> undefined_variables;
	// Keep the note's own assignments separate from Qalculate's global symbol
	// table.  Names such as `values` and `matrix` can also exist as built-ins;
	// in a note, the most recent local assignment must win.
	std::map<std::string, std::string> local_values;
	for(size_t i = 0; i < lines.size(); ++i) {
		const std::string &line = lines[i];
		LineResult result;
		if(function_lines[i]) {
			result.display = defined_functions[i] ? "defined" : "undefined";
		} else if(!is_comment_or_empty(line)) {
			const size_t comment = line.find("//");
			const std::string expression = comment == std::string::npos ? line : line.substr(0, comment);
			const size_t assignment = expression.find('=');
			const bool simple_assignment = assignment != std::string::npos && expression.find('=', assignment + 1) == std::string::npos;
			std::string lhs = simple_assignment ? expression.substr(0, assignment) : "";
			if(simple_assignment) {
				const size_t first = lhs.find_first_not_of(" \t");
				const size_t last = lhs.find_last_not_of(" \t");
				lhs = first == std::string::npos ? "" : lhs.substr(first, last - first + 1);
			}
			const std::string rhs = simple_assignment ? expression.substr(assignment + 1) : expression;
			const bool empty_assignment = simple_assignment && rhs.find_first_not_of(" \t") == std::string::npos;
			bool depends_on_undefined = false;
			for(const auto &name : undefined_variables) if(contains_identifier(rhs, name)) { depends_on_undefined = true; break; }
			if(empty_assignment || depends_on_undefined) {
				if(simple_assignment) undefined_variables.push_back(lhs);
				result.display = "undefined";
				results.push_back(result);
				continue;
			}
			std::string resolved_expression = expression;
			const bool differentiated_expression = resolved_expression.rfind("diff(", 0) == 0;
			for(const auto &entry : local_values) {
				size_t position = 0;
				while((position = resolved_expression.find(entry.first, position)) != std::string::npos) {
					const size_t comma = differentiated_expression ? resolved_expression.find(',') : std::string::npos;
					if(comma != std::string::npos && position >= comma) break;
					const bool left = position == 0 || !(std::isalnum(static_cast<unsigned char>(resolved_expression[position - 1])) || resolved_expression[position - 1] == '_');
					const size_t end = position + entry.first.size();
					const bool right = end == resolved_expression.size() || !(std::isalnum(static_cast<unsigned char>(resolved_expression[end])) || resolved_expression[end] == '_');
					if(left && right) {
						const bool structured = !entry.second.empty() && entry.second.front() == '[';
						const std::string replacement = structured ? entry.second : "(" + entry.second + ")";
						resolved_expression.replace(position, entry.first.size(), replacement);
						position += replacement.size();
					} else position = end;
				}
			}
			MathStructure direct_value;
			const bool direct_calculated = calculator.calculate(&direct_value, resolved_expression, 500, evaluation_options);
			notes_debug("ordinary", resolved_expression, direct_calculated, direct_value.isUndefined(), evaluation_options.parse_options.unknowns_enabled);
			result.display = direct_calculated && !direct_value.isUndefined()
				? direct_value.print(print_options, false, false, TAG_TYPE_TERMINAL)
				: "undefined";
			normalize_symbolic_display(result.display);
			if(notes_debug_enabled()) std::fprintf(stderr, "[notes-debug] formatted display=%s contains-string=%s\n", result.display.c_str(), contains_string_result(result.display) ? "yes" : "no");
			// With unknowns enabled, Qalculate can occasionally fail to infer the
			// differentiation variable for a one-argument diff() expression.  Infer
			// the first symbolic identifier from the expression and retry explicitly.
			if((result.display.empty() || result.display == "undefined") && resolved_expression.rfind("diff(", 0) == 0 && resolved_expression.find(',') == std::string::npos) {
				const size_t begin = resolved_expression.find('(') + 1;
				const size_t end = resolved_expression.rfind(')');
				if(end > begin) {
					const std::string inner = resolved_expression.substr(begin, end - begin);
					std::string variable;
					for(size_t p = 0; p < inner.size(); ++p) {
						if(std::isalpha(static_cast<unsigned char>(inner[p])) || inner[p] == '_') {
							size_t e = p + 1;
							while(e < inner.size() && (std::isalnum(static_cast<unsigned char>(inner[e])) || inner[e] == '_')) ++e;
							const std::string candidate = inner.substr(p, e - p);
							if(candidate != "diff") { variable = candidate; break; }
							p = e - 1;
						}
					}
					if(!variable.empty()) {
						MathStructure differentiated;
						const bool aliased_variable = variable == "q" || variable == "w";
						const std::string differentiation_variable = aliased_variable ? "notes_diff_symbol" : variable;
						std::string differentiated_inner = inner;
						if(aliased_variable) {
							size_t qpos = 0;
							while((qpos = differentiated_inner.find(variable, qpos)) != std::string::npos) {
								const bool left = qpos == 0 || !(std::isalnum(static_cast<unsigned char>(differentiated_inner[qpos - 1])) || differentiated_inner[qpos - 1] == '_');
								const size_t qend = qpos + variable.size();
								const bool right = qend == differentiated_inner.size() || !(std::isalnum(static_cast<unsigned char>(differentiated_inner[qend])) || differentiated_inner[qend] == '_');
								if(left && right) { differentiated_inner.replace(qpos, variable.size(), differentiation_variable); qpos += differentiation_variable.size(); } else ++qpos;
							}
						}
						const std::string explicit_diff = "diff(" + differentiated_inner + "," + differentiation_variable + ")";
						if(calculator.calculate(&differentiated, explicit_diff, 500, evaluation_options) && !differentiated.isUndefined()) {
							result.display = differentiated.print(print_options, false, false, TAG_TYPE_TERMINAL);
							if(aliased_variable) {
								size_t p = 0;
								while((p = result.display.find(differentiation_variable, p)) != std::string::npos) { result.display.replace(p, differentiation_variable.size(), variable); p += variable.size(); }
							}
						}
					}
				}
			}
			if(simple_assignment) result.display.clear();
			if(result.display.empty()) {
				MathStructure value;
				if(calculator.calculate(&value, resolved_expression, 500, evaluation_options) && !value.isUndefined())
					result.display = value.print(print_options, false, false, TAG_TYPE_TERMINAL);
			}
			// libqalculate stores assignments successfully but does not always
			// print the assigned value when the complete assignment is evaluated.
			// The CLI displays that value, so evaluate the RHS for display only.
			if(simple_assignment) {
				MathStructure value;
				calculator.clearMessages();
				const bool calculated = !rhs.empty() && calculator.calculate(&value, rhs, 500, evaluation_options);
					if(calculated && !value.isUndefined()) {
						result.display = value.print(print_options, false, false, TAG_TYPE_TERMINAL);
						if(!lhs.empty()) {
							std::string local_rhs = rhs;
							const size_t first_rhs = local_rhs.find_first_not_of(" \t");
							const size_t last_rhs = local_rhs.find_last_not_of(" \t");
							local_values[lhs] = first_rhs == std::string::npos ? "" : local_rhs.substr(first_rhs, last_rhs - first_rhs + 1);
						}
					}
			}
			if(result.display.empty()) {
				result.has_error = true;
				result.display = "error";
			}
			if(contains_string_result(result.display)) {
				result.display = "undefined";
			}
			if(notes_debug_enabled()) std::fprintf(stderr, "[notes-debug] ordinary-final display=%s line=%zu\n", result.display.c_str(), i + 1);
			if(simple_assignment && !lhs.empty()) {
				if(result.display == "undefined" || result.display == "error") {
					local_values.erase(lhs);
					if(std::find(undefined_variables.begin(), undefined_variables.end(), lhs) == undefined_variables.end()) undefined_variables.push_back(lhs);
				} else {
					undefined_variables.erase(std::remove(undefined_variables.begin(), undefined_variables.end(), lhs), undefined_variables.end());
				}
			}
		}
		results.push_back(result);
		if(notes_debug_enabled()) std::fprintf(stderr, "[notes-debug] returned display=%s line=%zu\n", results.back().display.c_str(), i + 1);
	}
	return results;
}

}
