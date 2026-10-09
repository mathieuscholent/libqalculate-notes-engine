#include "NoteEvaluator.h"
#include "QalculateScript.h"
#include "../libqalculate/Function.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>

namespace qalc_notes {

static bool run_script_range(const std::vector<qalc_script::Statement> &program,
	 size_t first, size_t last, const qalc_script::Executor::Evaluate &evaluate,
	 std::string &error, qalc_script::Executor::Environment &environment,
	 const qalc_script::Executor::Checkpoint &checkpoint, qalc_script::Executor &executor) {
	return executor.executeRange(program, first, last, evaluate, error, environment, checkpoint);
}

std::string save_calculator_checkpoint(Calculator &calculator) {
	return calculator.saveTemporaryDefinitions();
}

bool restore_calculator_checkpoint(Calculator &calculator, const std::string &checkpoint) {
	if(checkpoint.empty()) return false;
	char path[] = "/tmp/qalc-notes-checkpoint-XXXXXX";
	const int fd = mkstemp(path);
	if(fd < 0) return false;
	const ssize_t written = write(fd, checkpoint.data(), checkpoint.size());
	close(fd);
	calculator.resetVariables();
	calculator.resetFunctions();
	calculator.loadGlobalDefinitions();
	const bool restored = written == static_cast<ssize_t>(checkpoint.size()) && calculator.loadDefinitions(path, true, false) >= 0;
	unlink(path);
	return restored;
}

bool restore_script_state(Calculator &calculator, const ScriptState &state,
	qalc_script::Executor::Environment &environment) {
	if(!restore_calculator_checkpoint(calculator, state.calculator_checkpoint)) return false;
	environment = state.environment;
	return true;
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
	// An empty identifier matches at every position and would make the search
	// loop below non-terminating. Blank editor rows are valid input and can
	// reach this helper during incremental structural edits.
	if(name.empty()) return false;
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

static std::string trim_copy(const std::string &value) {
	const size_t first = value.find_first_not_of(" \t");
	if(first == std::string::npos) return "";
	const size_t last = value.find_last_not_of(" \t");
	return value.substr(first, last - first + 1);
}

static std::string substitute_script_scope(const std::string &expression,
	const qalc_script::Executor::Environment &scope) {
	std::string evaluated = expression;
	const bool differentiated_expression = evaluated.rfind("diff(", 0) == 0;
	for(const auto &entry : scope) {
		size_t position = 0;
		while((position = evaluated.find(entry.first, position)) != std::string::npos) {
			const size_t comma = differentiated_expression ? evaluated.find(',') : std::string::npos;
			if(comma != std::string::npos && position >= comma) break;
			const bool left = position == 0 || (!std::isalnum(static_cast<unsigned char>(evaluated[position - 1])) && evaluated[position - 1] != '_');
			const size_t end = position + entry.first.size();
			const bool right = end == evaluated.size() || (!std::isalnum(static_cast<unsigned char>(evaluated[end])) && evaluated[end] != '_');
			if(left && right) {
				std::string replacement = entry.second.scalar;
				if(entry.second.sequence) {
					replacement = "[";
					for(size_t item = 0; item < entry.second.items.size(); ++item) {
						if(item > 0) replacement += ", ";
						replacement += entry.second.items[item].scalar;
					}
					replacement += "]";
				}
				evaluated.replace(position, entry.first.size(), replacement);
				position += replacement.size();
			} else position = end;
		}
	}
	return evaluated;
}

static void normalize_script_value(std::string &value) {
	normalize_symbolic_display(value);
	const std::string alias = "notes_diff_symbol";
	for(const std::string &symbol : {std::string("q"), std::string("w")}) {
		size_t position = 0;
		while((position = value.find(alias, position)) != std::string::npos) {
			value.replace(position, alias.size(), symbol);
			position += symbol.size();
		}
	}
}

static bool notes_vector_extension(Calculator &calculator, const std::string &expression,
	const EvaluationOptions &options, const PrintOptions &print_options, std::string &display);

struct ScriptCalculation {
	MathStructure value;
	std::string extension_display;
	bool extension_calculated = false;
	bool calculated = false;
};

static ScriptCalculation calculate_script_expression(Calculator &calculator,
	const std::string &expression, const EvaluationOptions &options,
	const PrintOptions &print_options) {
	ScriptCalculation result;
	result.extension_calculated = notes_vector_extension(calculator, expression, options, print_options, result.extension_display);
	result.calculated = result.extension_calculated || calculator.calculate(&result.value, expression, 500, options);
	return result;
}

static bool retry_script_differentiation(Calculator &calculator, const std::string &expression,
	MathStructure &value, const EvaluationOptions &options) {
	if(expression.rfind("diff(", 0) != 0 || expression.find(',') != std::string::npos) return false;
	const size_t begin = expression.find('(') + 1;
	const size_t end = expression.rfind(')');
	if(end <= begin) return false;
	const std::string inner = expression.substr(begin, end - begin);
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
	if(variable.empty()) return false;
	const bool aliased = variable == "q" || variable == "w";
	const std::string differentiation_variable = aliased ? "notes_diff_symbol" : variable;
	std::string differentiated_inner = inner;
	if(aliased) {
		size_t position = 0;
		while((position = differentiated_inner.find(variable, position)) != std::string::npos) {
			const bool left = position == 0 || !(std::isalnum(static_cast<unsigned char>(differentiated_inner[position - 1])) || differentiated_inner[position - 1] == '_');
			const size_t end_position = position + variable.size();
			const bool right = end_position == differentiated_inner.size() || !(std::isalnum(static_cast<unsigned char>(differentiated_inner[end_position])) || differentiated_inner[end_position] == '_');
			if(left && right) { differentiated_inner.replace(position, variable.size(), differentiation_variable); position += differentiation_variable.size(); }
			else position = end_position;
		}
	}
	calculator.clearMessages();
	return calculator.calculate(&value, "diff(" + differentiated_inner + "," + differentiation_variable + ")", 500, options);
}

static void merge_script_rows(std::vector<LineResult> &results,
	const std::vector<std::vector<std::string>> &output,
	const std::vector<LineResult> &baseline,
	const std::vector<bool> &function_lines,
	const std::vector<bool> &defined_functions,
	const std::vector<bool> &affected_rows) {
	for(size_t index = 0; index < results.size(); ++index) {
		if(function_lines[index]) results[index].display = defined_functions[index] ? "defined" : "undefined";
		else if(!output[index].empty()) {
			results[index].display = output[index].size() == 1 ? output[index][0] : "[";
			for(size_t value = output[index].size() > 1 ? 0 : output[index].size(); value < output[index].size(); ++value) {
				if(value > 0) results[index].display += " · ";
				results[index].display += output[index][value];
			}
			if(output[index].size() > 1) results[index].display += "]";
		} else if(index < affected_rows.size() && affected_rows[index]) {
			// This row was executed but produced no value. Do not resurrect a
			// stale cached result when a condition/loop now has no output.
			results[index] = LineResult{};
		} else if(index < baseline.size()) results[index] = baseline[index];
	}
}

class FastNumericParser {
public:
	 explicit FastNumericParser(const std::string &text) : text_(text) {}
	 double parse() {
		 value_ = parse_expression();
		 skip_space();
		 if(position_ != text_.size()) throw std::invalid_argument("not numeric");
		 return value_;
	 }
private:
	 const std::string &text_;
	 size_t position_ = 0;
	 double value_ = 0;
	 void skip_space() { while(position_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[position_]))) ++position_; }
	 double parse_expression() {
		 double result = parse_term();
		 while(true) {
			 skip_space();
			 if(position_ >= text_.size() || (text_[position_] != '+' && text_[position_] != '-')) return result;
			 const char op = text_[position_++];
			 const double right = parse_term();
			 result = op == '+' ? result + right : result - right;
		 }
	 }
	 double parse_term() {
		 double result = parse_power();
		 while(true) {
			 skip_space();
			 if(position_ >= text_.size() || (text_[position_] != '*' && text_[position_] != '/')) return result;
			 const char op = text_[position_++];
			 const double right = parse_power();
			 if(op == '/' && right == 0) throw std::invalid_argument("division by zero");
			 result = op == '*' ? result * right : result / right;
		 }
	 }
	 double parse_power() {
		 double result = parse_unary();
		 skip_space();
		 if(position_ < text_.size() && text_[position_] == '^') {
			 ++position_;
			 result = std::pow(result, parse_power());
		 }
		 return result;
	 }
	 double parse_unary() {
		 skip_space();
		 if(position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) {
			 const bool negative = text_[position_++] == '-';
			 const double result = parse_unary();
			 return negative ? -result : result;
		 }
		 if(position_ < text_.size() && text_[position_] == '(') {
			 ++position_;
			 const double result = parse_expression();
			skip_space();
			if(position_ >= text_.size() || text_[position_++] != ')') throw std::invalid_argument("unclosed expression");
			return result;
		 }
		 const char *start = text_.c_str() + position_;
		 char *end = nullptr;
		 const double result = std::strtod(start, &end);
		 if(end == start) throw std::invalid_argument("not numeric");
		 position_ += static_cast<size_t>(end - start);
		 return result;
	 }
};

static bool fast_numeric_result(const std::string &expression, std::string &display) {
	 try {
		 const double value = FastNumericParser(expression).parse();
		 if(!std::isfinite(value)) return false;
		 std::ostringstream output;
		 output << std::setprecision(15) << value;
		 display = output.str();
		 return true;
	 } catch(const std::exception &) {
		 return false;
	 }
}

static bool split_top_level(const std::string &text, std::vector<std::string> &parts) {
	int depth = 0;
	size_t start = 0;
	for(size_t i = 0; i < text.size(); ++i) {
		if(text[i] == '[' || text[i] == '(') ++depth;
		else if(text[i] == ']' || text[i] == ')') --depth;
		else if(text[i] == ',' && depth == 0) {
			parts.push_back(trim_copy(text.substr(start, i - start)));
			start = i + 1;
		}
	}
	if(depth != 0) return false;
	if(!text.empty() || start != 0) parts.push_back(trim_copy(text.substr(start)));
	return true;
}

static bool vector_literal(const std::string &text, std::vector<std::string> &items) {
	const std::string value = trim_copy(text);
	if(value.size() < 2 || value.front() != '[' || value.back() != ']') return false;
	return split_top_level(value.substr(1, value.size() - 2), items);
}

static bool notes_vector_extension(Calculator &calculator, const std::string &expression,
		const EvaluationOptions &options, const PrintOptions &print_options, std::string &display) {
	const std::string text = trim_copy(expression);
	const std::vector<std::string> reductions = {"sum", "mean", "median", "min", "max"};
	for(const std::string &name : reductions) {
		const std::string prefix = name + "(";
		if(text.rfind(prefix, 0) != 0 || text.size() < prefix.size() + 1 || text.back() != ')') continue;
		std::vector<std::string> items;
		if(!vector_literal(text.substr(prefix.size(), text.size() - prefix.size() - 1), items)) continue;
		if(items.empty()) { display = "undefined"; return true; }
		std::string joined;
		for(size_t i = 0; i < items.size(); ++i) {
			if(i) joined += ",";
			joined += items[i];
		}
		// Qalculate already implements the statistical functions correctly for
		// literal vectors. Sum is the one vector reduction it intentionally does
		// not provide, so lower it to an ordinary arithmetic expression.
		if(name == "sum") {
			joined.clear();
			for(size_t i = 0; i < items.size(); ++i) {
				if(i) joined += "+";
				joined += "(" + items[i] + ")";
			}
		}
		MathStructure result;
		if(!calculator.calculate(&result, name == "sum" ? joined : text, 500, options) || result.isUndefined()) {
			display = "undefined";
			return true;
		}
		display = result.print(print_options, false, false, TAG_TYPE_TERMINAL);
		return true;
	}

	// Note vectors use zero-based indexing. Handle literal vectors after local
	// assignments have been substituted by the evaluator.
	const size_t close = text.find(']');
	if(close != std::string::npos && close + 1 < text.size() && text[close + 1] == '[' && text.back() == ']') {
		std::vector<std::string> items;
		if(vector_literal(text.substr(0, close + 1), items)) {
			const std::string index_text = trim_copy(text.substr(close + 2, text.size() - close - 3));
			const size_t colon = index_text.find(':');
			if(colon != std::string::npos) {
				char *start_end = NULL;
				char *stop_end = NULL;
				const long start = std::strtol(trim_copy(index_text.substr(0, colon)).c_str(), &start_end, 10);
				const long stop = std::strtol(trim_copy(index_text.substr(colon + 1)).c_str(), &stop_end, 10);
				if(start_end && *start_end == '\0' && stop_end && *stop_end == '\0') {
					long normalized_start = start < 0 ? static_cast<long>(items.size()) + start : start;
					long normalized_stop = stop < 0 ? static_cast<long>(items.size()) + stop : stop;
					if(normalized_start >= 0 && normalized_stop >= normalized_start && static_cast<size_t>(normalized_stop) <= items.size()) {
					display = "[";
					for(long i = normalized_start; i < normalized_stop; ++i) {
						if(i > normalized_start) display += " ";
						display += items[static_cast<size_t>(i)];
					}
					display += "]";
					return true;
					}
				}
				if(start_end && *start_end == '\0' && stop_end && *stop_end == '\0') {
					display = "undefined";
					return true;
				}
			}
			char *end = NULL;
			const long index = std::strtol(index_text.c_str(), &end, 10);
			if(end && *end == '\0') {
				const long normalized_index = index < 0 ? static_cast<long>(items.size()) + index : index;
				if(normalized_index >= 0 && static_cast<size_t>(normalized_index) < items.size()) {
					display = items[static_cast<size_t>(normalized_index)];
					return true;
				}
				display = "undefined";
				return true;
			}
			if(end && *end == '\0') {
				display = "undefined";
				return true;
			}
		}
	}

	// Matrix times a vector is commonly written as [[...], [...]] * [...].
	// Qalculate accepts the opposite orientation, but notes should support the
	// conventional matrix-times-column-vector spelling as well.
	const size_t multiplication = text.find(" * ");
	if(multiplication != std::string::npos) {
		std::vector<std::string> rows, vector_items;
		const std::string matrix_text = trim_copy(text.substr(0, multiplication));
		const std::string vector_text = trim_copy(text.substr(multiplication + 3));
		if(vector_literal(vector_text, vector_items) && vector_literal(matrix_text, rows) && !rows.empty()) {
			std::vector<std::string> row_values;
			bool matrix = true;
			for(size_t row_index = 0; row_index < rows.size(); ++row_index) {
				const std::string &row = rows[row_index];
				row_values.clear();
				if(!vector_literal(row, row_values) || row_values.size() != vector_items.size()) { matrix = false; break; }
				std::string sum;
				for(size_t i = 0; i < row_values.size(); ++i) {
					if(i) sum += "+";
					sum += "(" + row_values[i] + ")*(" + vector_items[i] + ")";
				}
				MathStructure result;
				if(!calculator.calculate(&result, sum, 500, options) || result.isUndefined()) { matrix = false; break; }
				if(row_index == 0) display = "[";
				else display += " ";
				display += result.print(print_options, false, false, TAG_TYPE_TERMINAL);
			}
			if(matrix) { display += "]"; return true; }
			display.clear();
		}
	}

	// Element-wise exponentiation is the natural operation for note vectors.
	const size_t power = text.rfind('^');
	if(power != std::string::npos && power > 0 && power + 1 < text.size()) {
		std::vector<std::string> items;
		char *end = NULL;
		const long exponent = std::strtol(text.substr(power + 1).c_str(), &end, 10);
		if(end && *end == '\0' && vector_literal(text.substr(0, power), items)) {
			display = "[";
			for(size_t i = 0; i < items.size(); ++i) {
				MathStructure result;
				if(i) display += " ";
				if(!calculator.calculate(&result, "(" + items[i] + ")^" + std::to_string(exponent), 500, options) || result.isUndefined()) {
					display = "undefined";
					return true;
				}
				display += result.print(print_options, false, false, TAG_TYPE_TERMINAL);
			}
			display += "]";
			return true;
		}
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
	// Differentiate before replacing formal arguments. Replacing x with a
	// call-time value in diff(x^2) would turn f(5) into diff(5^2), whose
	// derivative is correctly—but undesirably here—zero.
	if(args.size() == 1 && formula.rfind("diff(", 0) == 0 && formula.back() == ')') {
		const std::string inner = formula.substr(5, formula.size() - 6);
		MathStructure differentiated;
		EvaluationOptions options;
		options.parse_options.unknowns_enabled = true;
		const std::string explicit_diff = "diff(" + inner + "," + args[0] + ")";
		if(calculator.calculate(&differentiated, explicit_diff, 500, options) && !differentiated.isUndefined())
			formula = differentiated.print(PrintOptions(), false, false, TAG_TYPE_TERMINAL);
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

std::vector<LineResult> evaluate_note(Calculator &calculator, const std::vector<std::string> &lines,
	ScriptState *script_state, const ScriptState *input_state, int changed_line,
	const Cancellation &cancelled) {
	std::fprintf(stderr, "[notes-engine] evaluate_note begin lines=%zu\n", lines.size());
	if(cancelled && cancelled()) return std::vector<LineResult>(lines.size());
	std::vector<LineResult> results;
	results.reserve(lines.size());
	// Re-evaluation must start from a clean note state. The GUI reuses one
	// Calculator instance, so user variables/functions from an older document
	// would otherwise turn symbols such as x and q into stale constants.
	if(input_state == nullptr) {
		calculator.resetVariables();
		calculator.resetFunctions();
		calculator.loadGlobalDefinitions();
	}
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
			defined_functions[i] = input_state == nullptr && define_function(calculator, line);
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
		// A script-covered document does not need a second ordinary-note pass:
		// the script executor already evaluates its assignments and expressions.
		// Keep the baseline only for mixed documents containing lines that the
		// script parser did not represent.
		std::vector<bool> script_lines(lines.size(), false);
		for(size_t statement_index = 0; statement_index < program.size(); ++statement_index) {
			const unsigned int begin = program[statement_index].line;
			const unsigned int end = statement_index + 1 < program.size()
				? program[statement_index + 1].line : static_cast<unsigned int>(lines.size() + 1);
			for(unsigned int line = begin; line < end && line <= lines.size(); ++line) script_lines[line - 1] = true;
		}
		bool needs_baseline = false;
		for(size_t index = 0; index < lines.size(); ++index) {
			if(function_lines[index] || is_comment_or_empty(lines[index])) continue;
			if(!script_lines[index]) { needs_baseline = true; break; }
		}
		const std::vector<LineResult> baseline = input_state == nullptr && needs_baseline
			? evaluate_note(calculator, baseline_source) : std::vector<LineResult>(lines.size());
		if(input_state == nullptr && needs_baseline) {
			calculator.resetVariables();
			calculator.resetFunctions();
			calculator.loadGlobalDefinitions();
		}
		for(size_t i = 0; i < lines.size(); ++i) {
			if(function_lines[i] && input_state == nullptr) defined_functions[i] = define_function(calculator, lines[i]);
		}
		ScriptExecutionContext execution;
		execution.output.resize(lines.size());
		qalc_script::Executor::Environment &environment = execution.environment;
		size_t execution_first = 0;
		size_t execution_last = program.size();
		if(input_state != nullptr && changed_line >= 0) {
			const auto selected = qalc_script::Executor::rangeForLine(program, static_cast<unsigned int>(changed_line + 1));
			execution_first = selected.first;
			execution_last = selected.second;
			std::set<std::string> changed_names;
			for(size_t index = execution_first; index < execution_last; ++index) {
				const size_t equals = program[index].text.find('=');
				if(equals == std::string::npos) continue;
				const std::string name = trim_copy(program[index].text.substr(0, equals));
				if(!name.empty() && name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") == std::string::npos) changed_names.insert(name);
			}
			while(execution_last < program.size()) {
				bool depends = false;
				const std::string &text = program[execution_last].text;
				for(const std::string &name : changed_names) if(contains_identifier(text, name)) { depends = true; break; }
				if(!depends) break;
				const size_t equals = text.find('=');
				if(equals != std::string::npos) {
					const std::string name = trim_copy(text.substr(0, equals));
					if(!name.empty()) changed_names.insert(name);
				}
				++execution_last;
			}
			const unsigned int boundary_line = program[execution_first].line;
			const auto boundary = input_state->boundaries.find(boundary_line);
			if(boundary != input_state->boundaries.end()) {
				if(!restore_script_state(calculator, ScriptState{
					boundary->second.environment, boundary->second.calculator_checkpoint, {}, {}
				}, environment)) {
					execution_first = 0;
					execution_last = program.size();
				}
			} else {
				execution_first = 0;
				execution_last = program.size();
			}
			for(size_t index = execution_first; index < execution_last; ++index) {
				const unsigned int begin_line = program[index].line;
				const unsigned int end_line = index + 1 < program.size() ? program[index + 1].line : static_cast<unsigned int>(lines.size() + 1);
				for(unsigned int line = begin_line; line < end_line; ++line) execution.executed_lines.push_back(line - 1);
			}
		}
		std::vector<std::vector<std::string>> &output = execution.output;
		qalc_script::Executor executor;
		unsigned int &failed_line = execution.failed_line;
		qalc_script::Executor::Evaluate evaluate = [&](const std::string &expression, const qalc_script::Executor::Environment &scope, qalc_script::Value &value, std::string &evaluation_error, bool display) {
			if(cancelled && cancelled()) { evaluation_error = "cancelled"; return false; }
			std::string evaluated = expression;
			for(const auto &entry : scope) {
				if(entry.second.undefined && contains_identifier(evaluated, entry.first)) {
					evaluation_error = "undefined";
					return false;
				}
			}
			evaluated = substitute_script_scope(evaluated, scope);
			ScriptCalculation calculation = calculate_script_expression(calculator, evaluated, evaluation_options, print_options);
			MathStructure &value_structure = calculation.value;
			std::string &extension_display = calculation.extension_display;
			const bool extension_calculated = calculation.extension_calculated;
			bool calculated = calculation.calculated;
			if((!calculated || value_structure.isUndefined()) && evaluated.rfind("diff(", 0) == 0)
				calculated = retry_script_differentiation(calculator, evaluated, value_structure, evaluation_options);
			if(!calculated || (!extension_calculated && value_structure.isUndefined())) { evaluation_error = "undefined"; return false; }
			value.scalar = extension_calculated ? extension_display : value_structure.print(print_options, false, false, TAG_TYPE_TERMINAL);
			normalize_script_value(value.scalar);
			value.sequence = false;
			if(display && execution.active_output && !value.scalar.empty() && executor.currentLine() > 0 && executor.currentLine() <= output.size())
				(*execution.active_output)[executor.currentLine() - 1].push_back(value.scalar);
			return true;
		};
		execution.active_output = &output;
		qalc_script::Executor::Checkpoint checkpoint = [&](unsigned int line, const qalc_script::Executor::Environment &scope) {
			if(script_state == nullptr) return;
			ScriptState::Boundary boundary;
			boundary.environment = scope;
			boundary.calculator_checkpoint = save_calculator_checkpoint(calculator);
			script_state->boundaries[line] = std::move(boundary);
		};
		if(cancelled && cancelled()) return std::vector<LineResult>(lines.size());
		if(!run_script_range(program, execution_first, execution_last, evaluate, error, environment, checkpoint, executor)) {
			failed_line = executor.currentLine();
			if(failed_line > 0 && failed_line <= results.size()) {
				results[failed_line - 1].has_error = true;
				// Do not expose values calculated from the calculator's previous
				// pass after a script condition fails. Those values are stale.
			}
		}
		std::vector<bool> affected_rows(lines.size(), input_state == nullptr);
		for(const unsigned int line : execution.executed_lines) {
			if(line < affected_rows.size()) affected_rows[line] = true;
		}
		merge_script_rows(results, output, baseline, function_lines, defined_functions, affected_rows);
		// A failed script statement is local to that statement. Do not mark
		// later or unrelated baseline lines undefined: ordinary note lines have
		// independent scope and their already-computed results remain valid.
		for(size_t i = 0; i < results.size(); ++i) {
			if(results[i].has_error) results[i].display = "error";
			else if(contains_string_result(results[i].display)) results[i].display = "undefined";
		}
		if(script_state != nullptr) {
			script_state->environment = environment;
			script_state->calculator_checkpoint = save_calculator_checkpoint(calculator);
			script_state->executed_lines = execution.executed_lines;
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
			const size_t substitution_start = simple_assignment ? resolved_expression.find('=') + 1 : 0;
			const bool differentiated_expression = resolved_expression.rfind("diff(", 0) == 0;
			for(const auto &entry : local_values) {
				size_t position = substitution_start;
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
			std::string extension_display;
			const bool extension_calculated = notes_vector_extension(calculator, resolved_expression, evaluation_options, print_options, extension_display);
			const bool direct_calculated = extension_calculated || calculator.calculate(&direct_value, resolved_expression, 500, evaluation_options);
			result.display = extension_calculated ? extension_display
				: (direct_calculated && !direct_value.isUndefined()
					? direct_value.print(print_options, false, false, TAG_TYPE_TERMINAL)
					: "undefined");
			normalize_symbolic_display(result.display);
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
				std::string extension_display;
				const std::string resolved_rhs = resolved_expression.substr(assignment + 1);
				const bool extension_calculated = !resolved_rhs.empty() && notes_vector_extension(calculator, resolved_rhs, evaluation_options, print_options, extension_display);
				const bool calculated = extension_calculated || (!resolved_rhs.empty() && calculator.calculate(&value, resolved_rhs, 500, evaluation_options));
					if(extension_calculated) {
						result.display = extension_display;
						if(!lhs.empty()) local_values[lhs] = resolved_rhs;
					} else if(calculated && !value.isUndefined()) {
						result.display = value.print(print_options, false, false, TAG_TYPE_TERMINAL);
						if(!lhs.empty()) {
							std::string local_rhs = resolved_rhs;
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
	}
	return results;
}

std::vector<LineResult> NoteEvaluationSession::evaluate(Calculator &calculator, const std::vector<std::string> &lines,
	int changed_line, const Cancellation &cancelled) {
	if(cancelled && cancelled()) return {};
	std::fprintf(stderr, "[notes-engine] session begin old=%zu new=%zu changed=%d\n", lines_.size(), lines.size(), changed_line);
	if(lines == lines_ && results_.size() == lines.size()) return results_;
	const bool ordinary_independent = std::all_of(lines.begin(), lines.end(), [](const std::string &line) {
		const size_t first = line.find_first_not_of(" \t");
		if(first == std::string::npos) return true;
		const std::string trimmed = line.substr(first);
		return first == 0 && trimmed.rfind("//", 0) != 0 && trimmed.rfind("#", 0) != 0 &&
			trimmed.back() != ':';
	});
	bool safe_expression_edit = changed_line >= 0 &&
		changed_line < static_cast<int>(lines.size()) && lines[changed_line].find('=') == std::string::npos &&
		(lines[changed_line].empty() || (lines[changed_line][0] != ' ' && lines[changed_line][0] != '\t')) &&
		lines[changed_line].find(':') == std::string::npos;
	if(safe_expression_edit) {
		std::vector<std::string> assigned;
		for(int i = 0; i < changed_line; ++i) {
			const size_t equals = lines[i].find('=');
			if(equals == std::string::npos) continue;
			const std::string name = trim_copy(lines[i].substr(0, equals));
			if(!name.empty()) assigned.push_back(name);
		}
		for(const std::string &name : assigned) {
			if(contains_identifier(lines[changed_line], name)) { safe_expression_edit = false; break; }
		}
		if(safe_expression_edit) {
			for(int i = changed_line + 1; i < static_cast<int>(lines.size()); ++i) {
				if(contains_identifier(lines[i], trim_copy(lines[changed_line]))) { safe_expression_edit = false; break; }
			}
		}
	}
	if(ordinary_independent && changed_line >= 0 && changed_line < static_cast<int>(lines.size()) && !safe_expression_edit) {
		const size_t equals = lines[changed_line].find('=');
		if(equals != std::string::npos && lines[changed_line].find('=', equals + 1) == std::string::npos) {
			const std::string name = trim_copy(lines[changed_line].substr(0, equals));
			const std::string rhs = lines[changed_line].substr(equals + 1);
			bool prefix_dependency = false;
			for(int i = 0; i < changed_line; ++i) {
				const size_t previous_equals = lines[i].find('=');
				if(previous_equals != std::string::npos && contains_identifier(rhs, trim_copy(lines[i].substr(0, previous_equals)))) {
					prefix_dependency = true;
					break;
				}
			}
			bool downstream_dependency = false;
			for(int i = changed_line + 1; i < static_cast<int>(lines.size()) && !downstream_dependency; ++i)
				downstream_dependency = contains_identifier(lines[i], name);
			safe_expression_edit = !name.empty() && !prefix_dependency && !downstream_dependency;
		}
	}
	const bool reusable = safe_expression_edit &&
		changed_line >= 0 &&
		changed_line < static_cast<int>(lines.size()) && lines_.size() == lines.size() && results_.size() == lines.size();
	std::fprintf(stderr, "[notes-engine] flags ordinary=%d safe=%d reusable=%d\n", ordinary_independent, safe_expression_edit, reusable);
	// Appending a line preserves the calculator context and all prior results.
	// Evaluate only the new line instead of falling back to a full-note pass.
	if(changed_line == static_cast<int>(lines.size()) - 1 &&
		lines_.size() + 1 == lines.size() && results_.size() + 1 == lines.size()) {
		std::vector<LineResult> updated = results_;
		const std::vector<LineResult> appended = evaluate_note(calculator, {lines.back()});
		updated.push_back(appended.front());
		lines_ = lines;
		results_ = updated;
		return results_;
	}
	if(reusable) {
		std::fprintf(stderr, "[notes-engine] reusable path\n");
		std::vector<LineResult> updated = results_;
		const std::vector<std::string> one_line = {lines[changed_line]};
		const std::vector<LineResult> changed = evaluate_note(calculator, one_line);
		updated[changed_line] = changed.front();
		lines_ = lines;
		results_ = updated;
		return results_;
	}
	if(changed_line >= 0 && changed_line < static_cast<int>(lines.size()) &&
		lines_.size() == lines.size() && results_.size() == lines.size()) {
		const size_t first = lines[changed_line].find_first_not_of(" \t");
		const std::string trimmed = first == std::string::npos ? "" : lines[changed_line].substr(first);
		if(trimmed.empty() || trimmed.rfind("//", 0) == 0 || trimmed.rfind("#", 0) == 0) {
			std::vector<LineResult> updated = results_;
			updated[changed_line] = LineResult();
			lines_ = lines;
			results_ = updated;
			return results_;
		}
	}
	// Adding/removing comments or blank lines changes row alignment but not
	// calculator state. Reuse cached results by exact source-line identity.
	if(!lines_.empty() && !results_.empty()) {
		std::fprintf(stderr, "[notes-engine] structure reuse path\n");
		std::map<std::string, std::vector<size_t>> previous_indices;
		for(size_t index = 0; index < lines_.size() && index < results_.size(); ++index) {
			if(!is_comment_or_empty(lines_[index])) previous_indices[lines_[index]].push_back(index);
		}
		std::map<std::string, size_t> consumed;
		std::vector<LineResult> updated;
		updated.reserve(lines.size());
		bool reusable_structure = true;
		for(const std::string &line : lines) {
			if(is_comment_or_empty(line)) {
				updated.push_back(LineResult());
				continue;
			}
			const auto found = previous_indices.find(line);
			const size_t occurrence = consumed[line]++;
			if(found == previous_indices.end() || occurrence >= found->second.size()) {
				reusable_structure = false;
				break;
			}
			updated.push_back(results_[found->second[occurrence]]);
		}
		if(reusable_structure) {
			lines_ = lines;
			results_ = updated;
			assignment_values_.clear();
			for(size_t index = 0; index < lines.size() && index < results_.size(); ++index) {
				const size_t equals = lines[index].find('=');
				if(equals == std::string::npos) continue;
				if(results_[index].display == "undefined" || results_[index].display == "error") continue;
				const std::string name = trim_copy(lines[index].substr(0, equals));
				if(!name.empty()) assignment_values_[name] = results_[index].display;
			}
			return results_;
		}
	}
	if(ordinary_independent && changed_line >= 0 && changed_line < static_cast<int>(lines.size()) &&
		lines_.size() == lines.size() && results_.size() == lines.size()) {
		const size_t equals = lines[changed_line].find('=');
		if(equals != std::string::npos && lines[changed_line].find('=', equals + 1) == std::string::npos) {
			std::vector<LineResult> updated = results_;
			std::string changed_display;
			const std::string changed_rhs = lines[changed_line].substr(equals + 1);
			if(fast_numeric_result(changed_rhs, changed_display)) updated[changed_line] = LineResult{changed_display, false};
			else updated[changed_line] = evaluate_note(calculator, {lines[changed_line]}).front();
			const std::string changed_name = trim_copy(lines[changed_line].substr(0, equals));
			// Function definitions are assignments syntactically, but changing one
			// changes the evaluator's callable environment rather than one scalar
			// dependency. Keep those edits on the full-note path.
			if(changed_name.empty() || changed_name.find('(') != std::string::npos) {
				results_ = evaluate_note(calculator, lines);
				lines_ = lines;
				assignment_values_.clear();
				for(size_t i = 0; i < lines.size() && i < results_.size(); ++i) {
					const size_t line_equals = lines[i].find('=');
					if(line_equals != std::string::npos && results_[i].display != "undefined" && results_[i].display != "error")
						assignment_values_[trim_copy(lines[i].substr(0, line_equals))] = results_[i].display;
				}
				return results_;
			}
			if(updated[changed_line].display != "undefined" && updated[changed_line].display != "error") assignment_values_[changed_name] = updated[changed_line].display;
			else assignment_values_.erase(changed_name);
			// Only propagate through assignments which depend on the edited one.
			// In particular, do not force the first unrelated line after the edit
			// through the evaluator on every keystroke. Collect the affected block
			// and evaluate it in one pass; repeated single-line Qalculate calls are
			// significantly more expensive than one local note evaluation.
			std::set<std::string> changed_names = {changed_name};
			std::vector<int> dependent_indices;
			std::vector<std::string> dependent_expressions;
			for(int i = changed_line + 1; i < static_cast<int>(lines.size()); ++i) {
				std::string expression = lines[i];
				bool depends = false;
				// Walk downstream in source order. A dependent assignment must be
				// committed before the next line is inspected, otherwise a chain such
				// as x -> y -> z substitutes the cached (old) value of y into z.
				for(const auto &entry : assignment_values_) {
					if(changed_names.find(entry.first) == changed_names.end()) continue;
					if(contains_identifier(expression, entry.first)) {
						depends = true;
					}
				}
				if(!depends) continue;
				dependent_indices.push_back(i);
				dependent_expressions.push_back(expression);
				const size_t next_equals = lines[i].find('=');
				if(next_equals != std::string::npos) {
					const std::string next_name = trim_copy(lines[i].substr(0, next_equals));
					changed_names.insert(next_name);
				}
			}
		if(!dependent_expressions.empty()) {
				std::vector<LineResult> recalculated;
				for(const std::string &expression : dependent_expressions) {
					std::string evaluated_expression = expression;
					for(const auto &entry : assignment_values_) {
						size_t position = 0;
						while((position = evaluated_expression.find(entry.first, position)) != std::string::npos) {
							const bool left = position == 0 || (!std::isalnum(static_cast<unsigned char>(evaluated_expression[position - 1])) && evaluated_expression[position - 1] != '_');
							const size_t end = position + entry.first.size();
							const bool right = end == evaluated_expression.size() || (!std::isalnum(static_cast<unsigned char>(evaluated_expression[end])) && evaluated_expression[end] != '_');
							if(left && right) { evaluated_expression.replace(position, entry.first.size(), "(" + entry.second + ")"); position += entry.second.size() + 2; }
							else position = end;
						}
					}
					std::string display;
					if(!fast_numeric_result(evaluated_expression, display)) {
						const std::vector<LineResult> evaluated = evaluate_note(calculator, {evaluated_expression});
						display = evaluated.front().display;
					}
					recalculated.push_back(LineResult{display, false});
					const size_t line_index = dependent_indices[recalculated.size() - 1];
					const size_t next_equals = lines[line_index].find('=');
					if(next_equals != std::string::npos) {
						const std::string next_name = trim_copy(lines[line_index].substr(0, next_equals));
						if(display == "undefined" || display == "error") assignment_values_.erase(next_name);
						else assignment_values_[next_name] = display;
					}
				}
				for(size_t index = 0; index < dependent_indices.size() && index < recalculated.size(); ++index) {
					const int line_index = dependent_indices[index];
					updated[line_index] = recalculated[index];
					const size_t next_equals = lines[line_index].find('=');
					if(next_equals == std::string::npos) continue;
					const std::string next_name = trim_copy(lines[line_index].substr(0, next_equals));
					if(recalculated[index].display == "undefined" || recalculated[index].display == "error") assignment_values_.erase(next_name);
					else assignment_values_[next_name] = recalculated[index].display;
				}
			}
			lines_ = lines;
			results_ = updated;
			return results_;
		}
	}
	std::fprintf(stderr, "[notes-engine] full evaluate path\n");
	bool unchanged_context = changed_line >= 0 && lines_.size() == lines.size();
	if(unchanged_context) {
		for(size_t index = 0; index < lines.size(); ++index) {
			if(static_cast<int>(index) != changed_line && lines_[index] != lines[index]) {
				unchanged_context = false;
				break;
			}
		}
	}
	const bool can_resume_script = unchanged_context && !script_state_.boundaries.empty();
	ScriptState next_script_state;
	const std::vector<LineResult> recalculated = evaluate_note(calculator, lines, &next_script_state,
		can_resume_script ? &script_state_ : nullptr, changed_line, cancelled);
	if(can_resume_script) {
		std::vector<LineResult> merged = results_;
		for(const unsigned int index : next_script_state.executed_lines)
			if(index < recalculated.size() && index < merged.size()) merged[index] = recalculated[index];
		results_ = merged;
		// Boundaries at or after the edited source line may depend on values
		// changed by this run. Retain only the valid prefix and replace the
		// invalidated suffix with freshly captured snapshots.
		for(auto boundary = script_state_.boundaries.begin(); boundary != script_state_.boundaries.end();) {
			if(static_cast<int>(boundary->first) > changed_line) boundary = script_state_.boundaries.erase(boundary);
			else ++boundary;
		}
		for(const auto &entry : next_script_state.boundaries) script_state_.boundaries[entry.first] = entry.second;
		script_state_.environment = next_script_state.environment;
		script_state_.calculator_checkpoint = next_script_state.calculator_checkpoint;
	} else {
		script_state_ = std::move(next_script_state);
		results_ = recalculated;
	}
	lines_ = lines;
	assignment_values_.clear();
	for(size_t i = 0; i < lines.size() && i < results_.size(); ++i) {
		const size_t equals = lines[i].find('=');
		if(equals != std::string::npos && results_[i].display != "undefined" && results_[i].display != "error")
			assignment_values_[trim_copy(lines[i].substr(0, equals))] = results_[i].display;
	}
	return results_;
}

}
