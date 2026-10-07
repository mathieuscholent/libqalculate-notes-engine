#ifndef QALCULATE_NOTE_EVALUATOR_H
#define QALCULATE_NOTE_EVALUATOR_H

#include "../libqalculate/Calculator.h"

#include <string>
#include <map>
#include <vector>

namespace qalc_notes {

struct LineResult {
	std::string display;
	bool has_error = false;
};

// Evaluates a complete note using the notes-engine semantics and returns one
// display result for every source line. Presentation layers should render the
// returned strings without reinterpreting expressions or errors.
std::vector<LineResult> evaluate_note(Calculator &calculator, const std::vector<std::string> &lines);

// XML checkpoints contain only temporary user definitions, leaving global
// Qalculate definitions untouched. They can be captured before a block and
// restored when an incremental evaluation resumes from that block.
std::string save_calculator_checkpoint(Calculator &calculator);
bool restore_calculator_checkpoint(Calculator &calculator, const std::string &checkpoint);

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
};

}

#endif
