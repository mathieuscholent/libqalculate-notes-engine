#ifndef QALCULATE_NOTE_EVALUATOR_H
#define QALCULATE_NOTE_EVALUATOR_H

#include "../libqalculate/Calculator.h"
#include "QalculateScript.h"

#include <string>
#include <map>
#include <vector>

namespace qalc_notes {

struct LineResult {
	std::string display;
	bool has_error = false;
};

// Displayed results alone cannot resume a script safely: loops and branches
// also depend on the executor environment and calculator definitions.
struct ScriptState {
	struct Boundary {
		qalc_script::Executor::Environment environment;
		std::string calculator_checkpoint;
	};
	qalc_script::Executor::Environment environment;
	std::string calculator_checkpoint;
	std::map<unsigned int, Boundary> boundaries;
	std::vector<unsigned int> executed_lines;
};

// Shared mutable context for script execution. Keeping this separate from
// NoteEvaluationSession lets whole-note and block execution use the same
// evaluator without duplicating loop/condition/function semantics.
struct ScriptExecutionContext {
	qalc_script::Executor::Environment environment;
	std::vector<std::vector<std::string>> output;
	std::vector<std::vector<std::string>> *active_output = nullptr;
	std::vector<unsigned int> executed_lines;
	unsigned int failed_line = 0;
	std::string error;
};

// Evaluates a complete note using the notes-engine semantics and returns one
// display result for every source line. Presentation layers should render the
// returned strings without reinterpreting expressions or errors.
std::vector<LineResult> evaluate_note(Calculator &calculator, const std::vector<std::string> &lines,
	ScriptState *script_state = nullptr, const ScriptState *input_state = nullptr,
	int changed_line = -1);

// XML checkpoints contain only temporary user definitions, leaving global
// Qalculate definitions untouched. They can be captured before a block and
// restored when an incremental evaluation resumes from that block.
std::string save_calculator_checkpoint(Calculator &calculator);
bool restore_calculator_checkpoint(Calculator &calculator, const std::string &checkpoint);
bool restore_script_state(Calculator &calculator, const ScriptState &state,
	qalc_script::Executor::Environment &environment);

// Stateful front-end for repeated editor evaluations. Independent ordinary
// lines are reused when only one line changes; structured notes fall back to
// the complete evaluator until their stateful execution can be invalidated
// safely.
class NoteEvaluationSession {
public:
	std::vector<LineResult> evaluate(Calculator &calculator, const std::vector<std::string> &lines, int changed_line = -1);
private:
	std::vector<std::string> lines_;
	std::vector<LineResult> results_;
	std::map<std::string, std::string> assignment_values_;
	ScriptState script_state_;
};

}

#endif
