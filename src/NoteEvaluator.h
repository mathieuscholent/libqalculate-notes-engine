#ifndef QALCULATE_NOTE_EVALUATOR_H
#define QALCULATE_NOTE_EVALUATOR_H

#include "../libqalculate/Calculator.h"

#include <string>
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

}

#endif
