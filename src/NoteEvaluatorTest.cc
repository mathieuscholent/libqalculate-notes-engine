#include "NoteEvaluator.h"

#include <cassert>
#include <string>
#include <vector>

int main() {
	Calculator calculator;
	calculator.loadGlobalDefinitions();
	const std::vector<std::string> lines = {
		"e",
		"cash = 400000",
		"Rt1(Pt, Pt1, Divt1) = ((Pt1 - Pt) / Pt)",
		"Rt1(100, 110, 1)"
	};
	const auto results = qalc_notes::evaluate_note(calculator, lines);
	assert(results.size() == lines.size());
	assert(!results[0].has_error && !results[0].display.empty());
	assert(!results[1].has_error && !results[1].display.empty());
	assert(!results[2].has_error && results[2].display == "defined");
	assert(!results[3].has_error && !results[3].display.empty());
	return 0;
}
