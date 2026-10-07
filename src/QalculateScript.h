#ifndef QALCULATE_SCRIPT_H
#define QALCULATE_SCRIPT_H

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace qalc_script {

struct Statement {
	std::string text;
	std::vector<Statement> body;
	unsigned int line;
};

struct Value {
	bool sequence;
	bool tuple;
	bool undefined;
	std::string scalar;
	std::vector<Value> items;
	Value() : sequence(false), tuple(false), undefined(false) {}
};

class Parser {
public:
	bool parse(const std::vector<std::string> &lines, std::vector<Statement> &program, std::string &error) const;
};

class Executor {
public:
	typedef std::map<std::string, Value> Environment;
	typedef std::function<bool(const std::string &, const Environment &, Value &, std::string &, bool)> Evaluate;
	typedef std::function<void(unsigned int, const Environment &)> Checkpoint;
	bool execute(const std::vector<Statement> &program, const Evaluate &evaluate, std::string &error) const;
	bool execute(const std::vector<Statement> &program, const Evaluate &evaluate, std::string &error, Environment &environment) const;
	bool execute(const std::vector<Statement> &program, const Evaluate &evaluate, std::string &error, Environment &environment, const Checkpoint &checkpoint) const;
	unsigned int currentLine() const { return current_line_; }

private:
	mutable unsigned int current_line_ = 0;
};

}

#endif
