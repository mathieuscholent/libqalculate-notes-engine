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
	const std::vector<std::string> indexing_lines = {
		"values = [10, 20, 30, 40]",
		"values[0]",
		"values[3]",
		"values[-1]",
		"values[-4]",
		"values[-5]",
		"values[1:3]",
		"values[-3:-1]",
		"values[4]",
		"values[-5:-6]"
	};
	const auto indexing = qalc_notes::evaluate_note(calculator, indexing_lines);
	assert(indexing[1].display == "10");
	assert(indexing[2].display == "40");
	assert(indexing[3].display == "40");
	assert(indexing[4].display == "10");
	assert(indexing[5].display == "undefined");
	assert(indexing[6].display == "[20 30]");
	assert(indexing[7].display == "undefined");
	assert(indexing[8].display == "undefined");
	return 0;
}
