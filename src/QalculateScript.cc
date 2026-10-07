#include "QalculateScript.h"

#include <cctype>
#include <cstdlib>
#include <sstream>

namespace qalc_script {

static unsigned int indentation(const std::string &line) {
	unsigned int n = 0;
	for(char c : line) {
		if(c == ' ') n++;
		else if(c == '\t') n += 2;
		else break;
	}
	return n;
}

static std::string trim(const std::string &value) {
	size_t first = value.find_first_not_of(" \t\r");
	if(first == std::string::npos) return "";
	size_t last = value.find_last_not_of(" \t\r");
	return value.substr(first, last - first + 1);
}

static bool truth_value(const Value &value, bool &truth) {
	const std::string scalar = trim(value.scalar);
	if(scalar == "true" || scalar == "True") { truth = true; return true; }
	if(scalar == "false" || scalar == "False") { truth = false; return true; }
	char *end = NULL;
	const long double number = std::strtold(scalar.c_str(), &end);
	if(end == scalar.c_str()) return false;
	while(end && *end && std::isspace(static_cast<unsigned char>(*end))) ++end;
	if(end && *end != '\0') return false;
	truth = number != 0;
	return true;
}

static bool split_sequence(const std::string &text, std::vector<std::string> &parts) {
	int depth = 0;
	size_t start = 0;
	for(size_t i = 0; i < text.length(); i++) {
		if(text[i] == '(' || text[i] == '[') depth++;
		else if(text[i] == ')' || text[i] == ']') depth--;
		else if(text[i] == ',' && depth == 0) {
			parts.push_back(trim(text.substr(start, i - start)));
			start = i + 1;
		}
	}
	if(depth != 0) return false;
	parts.push_back(trim(text.substr(start)));
	return true;
}

bool Parser::parse(const std::vector<std::string> &lines, std::vector<Statement> &program, std::string &error) const {
	program.clear();
	std::vector<std::pair<unsigned int, std::vector<Statement>*> > stack;
	stack.push_back(std::make_pair(0, &program));
	for(size_t i = 0; i < lines.size(); i++) {
		std::string text = trim(lines[i]);
		const size_t comment = text.find("//");
		if(comment != std::string::npos) text = trim(text.substr(0, comment));
		if(text.empty() || text[0] == '#' || (text.size() > 1 && text[0] == '/' && text[1] == '/')) continue;
		unsigned int level = indentation(lines[i]);
		while(stack.size() > 1 && level < stack.back().first) stack.pop_back();
		if(level > stack.back().first && stack.back().second->empty()) {
			error = "unexpected indentation on line " + std::to_string(i + 1);
			return false;
		}
		if(level > stack.back().first) {
			error = "indentation must follow a block on line " + std::to_string(i + 1);
			return false;
		}
		if(level < stack.back().first || level != stack.back().first) {
			error = "inconsistent indentation on line " + std::to_string(i + 1);
			return false;
		}
		Statement statement;
		statement.text = text;
		statement.line = i + 1;
		stack.back().second->push_back(statement);
		if(text.back() == ':') {
			stack.push_back(std::make_pair(level + 2, &stack.back().second->back().body));
		}
	}
	return true;
}

bool Executor::execute(const std::vector<Statement> &program, const Evaluate &evaluate, std::string &error) const {
	Environment environment;
	return execute(program, evaluate, error, environment, Checkpoint());
}

bool Executor::execute(const std::vector<Statement> &program, const Evaluate &evaluate, std::string &error, Environment &environment) const {
	return execute(program, evaluate, error, environment, Checkpoint());
}

bool Executor::execute(const std::vector<Statement> &program, const Evaluate &evaluate, std::string &error, Environment &environment, const Checkpoint &checkpoint) const {
	// Notes are reevaluated after every keystroke; keep runaway loops from
	// blocking the GUI thread while still allowing normal interactive scripts.
	const unsigned int max_iterations = 10000;
	std::function<bool(const std::vector<Statement> &, Environment &)> run;
		run = [&](const std::vector<Statement> &statements, Environment &scope) {
		for(size_t statement_index = 0; statement_index < statements.size(); statement_index++) {
			const Statement &statement = statements[statement_index];
			current_line_ = statement.line;
			if(checkpoint) checkpoint(current_line_, scope);
			std::string text = statement.text;
			if(text.size() > 1 && text.back() == ':') {
				std::string header = trim(text.substr(0, text.length() - 1));
				if(header.find("if ") == 0 || header == "else" || header.find("else if ") == 0) {
					bool condition = header == "else";
					if(header.find("if ") == 0 || header.find("else if ") == 0) {
						std::string expression = header.find("if ") == 0 ? trim(header.substr(3)) : trim(header.substr(8));
						Value result;
						if(!evaluate(expression, scope, result, error, false)) {
							error.clear();
							while(statement_index + 1 < statements.size()) {
								const std::string next = trim(statements[statement_index + 1].text);
								if(next != "else:" && next.find("else if ") != 0) break;
								++statement_index;
							}
							continue;
						}
						bool truth = false;
						if(!truth_value(result, truth)) {
							error = "if condition must evaluate to a number on line " + std::to_string(statement.line);
							return false;
						}
						condition = truth;
					}
					if(condition) {
						if(!run(statement.body, scope)) return false;
						while(statement_index + 1 < statements.size()) {
							const std::string next = trim(statements[statement_index + 1].text);
							if(next != "else:" && next.find("else if ") != 0) break;
							statement_index++;
						}
					} else if(statement_index + 1 < statements.size()) {
						const std::string alternative_header = trim(statements[statement_index + 1].text);
						if(alternative_header == "else:" || alternative_header.find("else if ") == 0) {
							// Pass the complete else-if chain to the executor. This lets a
							// false else-if continue to a later else-if or final else.
							std::vector<Statement> branch;
							size_t next = statement_index + 1;
							while(next < statements.size()) {
								const std::string next_header = trim(statements[next].text);
								if(next_header != "else:" && next_header.find("else if ") != 0) break;
								branch.push_back(statements[next]);
								statement_index = next;
								++next;
							}
							if(!run(branch, scope)) return false;
						}
					}
					continue;
				}
				if(header.find("while ") == 0) {
					std::string condition = trim(header.substr(6));
					if(condition.empty()) {
						error = "while loop requires a condition on line " + std::to_string(statement.line);
						return false;
					}
					for(unsigned int iteration = 0; ; iteration++) {
						if(iteration >= max_iterations) {
							error = "while loop exceeded iteration limit on line " + std::to_string(statement.line);
							return false;
						}
						Value result;
						if(!evaluate(condition, scope, result, error, false)) {
							error.clear();
							break;
						}
						bool truth = false;
						if(!truth_value(result, truth)) {
							error = "while condition must evaluate to a number on line " + std::to_string(statement.line);
							return false;
						}
						if(!truth) break;
						if(!run(statement.body, scope)) return false;
					}
					continue;
				}
				if(header.find("for ") != 0) {
					error = "unsupported block on line " + std::to_string(statement.line);
					return false;
				}
				size_t in = header.find(" in ", 4);
				if(in == std::string::npos) {
					error = "expected 'in' in for loop on line " + std::to_string(statement.line);
					return false;
				}
				std::string name = trim(header.substr(4, in - 4));
				std::string source = trim(header.substr(in + 4));
				Value values;
				if(source.find("range(") == 0 && source.back() == ')') {
					std::string args = source.substr(6, source.length() - 7);
					std::vector<long long> numbers;
					std::stringstream ss(args);
					std::string part;
					while(std::getline(ss, part, ',')) {
						part = trim(part);
						char *end = NULL;
						long long value = std::strtoll(part.c_str(), &end, 10);
						if(!end || *end != '\0') {
							error = "range arguments must be integer literals on line " + std::to_string(statement.line);
							return false;
						}
						numbers.push_back(value);
					}
					if(numbers.empty() || numbers.size() > 3) {
						error = "range expects one to three arguments on line " + std::to_string(statement.line);
						return false;
					}
					long long start = numbers.size() == 1 ? 0 : numbers[0];
					long long stop = numbers.size() == 1 ? numbers[0] : numbers[1];
					long long step = numbers.size() == 3 ? numbers[2] : 1;
					if(step == 0) { error = "range step cannot be zero"; return false; }
					values.sequence = true;
					for(long long i = start; step > 0 ? i < stop : i > stop; i += step) {
						Value item; item.scalar = std::to_string(i); values.items.push_back(item);
					}
				} else if(!source.empty() && scope.count(source) && scope[source].sequence) {
					values = scope[source];
				} else {
					error = "for source must be a range or sequence variable on line " + std::to_string(statement.line);
					return false;
				}
				for(const Value &item : values.items) {
					scope[name] = item;
					if(!run(statement.body, scope)) return false;
				}
				continue;
			}
			size_t assignment = text.find('=');
			if(assignment != std::string::npos && (assignment == 0 || text.find("==") == std::string::npos)) {
				std::string name = trim(text.substr(0, assignment));
				std::string value = trim(text.substr(assignment + 1));
				if(!name.empty() && name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") == std::string::npos) {
					if(value.empty()) {
						Value undefined;
						undefined.undefined = true;
						undefined.scalar = "undefined";
						scope[name] = undefined;
						continue;
					}
					bool tuple = value.size() >= 2 && value.front() == '(' && value.back() == ')';
					bool list = value.size() >= 2 && value.front() == '[' && value.back() == ']';
					if(list || tuple) {
						std::vector<std::string> parts;
						if(!split_sequence(value.substr(1, value.length() - 2), parts)) {
							error = "unbalanced sequence on line " + std::to_string(statement.line);
							return false;
						}
						if(parts.size() == 1 && parts[0].empty()) parts.clear();
						Value sequence;
						sequence.sequence = true;
						sequence.tuple = tuple;
						for(const std::string &part : parts) {
							Value item;
							item.scalar = part;
							sequence.items.push_back(item);
						}
						 scope[name] = sequence;
						continue;
					}
					Value result;
					if(!evaluate(value, scope, result, error, true)) {
						Value undefined;
						undefined.undefined = true;
						undefined.scalar = "undefined";
						scope[name] = undefined;
						continue;
					}
					scope[name] = result;
					continue;
				}
			}
			Value result;
			if(!evaluate(text, scope, result, error, true)) continue;
		}
		return true;
	};
	return run(program, environment);
}

}
