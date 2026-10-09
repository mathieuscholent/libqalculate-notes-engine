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
	const auto differentiated_function = qalc_notes::evaluate_note(calculator, {
		"f(x) = diff(x^2)", "f(x)", "f(5)", "x = 5", "f(x)"
	});
	assert(differentiated_function[0].display == "defined");
	assert(differentiated_function[2].display == "10");
	assert(differentiated_function[4].display == "10");
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
	assert(indexing[7].display == "[20 30]");
	assert(indexing[8].display == "undefined");
	assert(indexing[9].display == "undefined");
	const auto compact_indexing = qalc_notes::evaluate_note(calculator, {
		"values=[1, 2, 3]", "values", "values[0]"
	});
	assert(compact_indexing[2].display == "1");
	qalc_notes::NoteEvaluationSession session;
	Calculator session_calculator;
	session_calculator.loadGlobalDefinitions();
	const std::vector<std::string> dependent_lines = {"x = 1", "y = x + 1", "z = 100", "w = y + 1", "7"};
	const auto first_session = session.evaluate(session_calculator, dependent_lines);
	assert(first_session[1].display == "2");
	assert(first_session[2].display == "100");
	assert(first_session[3].display == "3");
	const auto second_session = session.evaluate(session_calculator, {"x = 2", "y = x + 1", "z = 100", "w = y + 1", "7"}, 0);
	assert(second_session[0].display == "2");
	assert(second_session[1].display == "3");
	assert(second_session[2].display == "100");
	assert(second_session[3].display == "4");
	assert(second_session[4].display == "7");
	const auto with_blank = session.evaluate(session_calculator, {
		"x = 2", "", "y = x + 1", "z = y + 1"
	});
	assert(with_blank.size() == 4);
	assert(with_blank[2].display == "3");
	const auto after_blank_removal = session.evaluate(session_calculator, {
		"x = 2", "y = x + 1", "z = y + 1"
	}, 1);
	assert(after_blank_removal.size() == 3);
	assert(after_blank_removal[1].display == "3");
	const auto after_variable_edit = session.evaluate(session_calculator, {
		"x = 4", "y = x + 1", "z = y + 1"
	}, 0);
	assert(after_variable_edit[0].display == "4");
	assert(after_variable_edit[1].display == "5");
	assert(after_variable_edit[2].display == "6");
	const auto transitive_initial = session.evaluate(session_calculator, {
		"x = 1", "y = x + 1", "z = y + 1", "answer = z + 1"
	});
	assert(transitive_initial[3].display == "4");
	const auto after_transitive_variable_edit = session.evaluate(session_calculator, {
		"x = 5", "y = x + 1", "z = y + 1", "answer = z + 1"
	}, 0);
	assert(after_transitive_variable_edit[0].display == "5");
	assert(after_transitive_variable_edit[1].display == "6");
	assert(after_transitive_variable_edit[2].display == "7");
	assert(after_transitive_variable_edit[3].display == "8");
	qalc_notes::ScriptState script_state;
	Calculator script_calculator;
	script_calculator.loadGlobalDefinitions();
	const auto script_results = evaluate_note(script_calculator, {
		"values = [1, 2, 3]", "for elt in values:", "  elt", "answer = 9"
	}, &script_state);
	assert(script_results.size() == 4);
	assert(script_results[2].display == "[1 · 2 · 3]");
	assert(script_state.boundaries.find(1) != script_state.boundaries.end());
	assert(script_state.boundaries.find(2) != script_state.boundaries.end());
	qalc_script::Executor::Environment restored_environment;
	Calculator restored_calculator;
	assert(qalc_notes::restore_script_state(
		restored_calculator, script_state, restored_environment));
	qalc_script::Parser script_parser;
	std::vector<qalc_script::Statement> script_program;
	std::string script_error;
	assert(script_parser.parse({
		"values = [1, 2, 3]", "for elt in values:", "  if elt > 1:", "    elt"
	}, script_program, script_error));
	const auto loop_range = qalc_script::Executor::rangeForLine(script_program, 4);
	assert(loop_range.first == 1 && loop_range.second == 2);
	qalc_notes::NoteEvaluationSession script_session;
	Calculator script_session_calculator;
	script_session_calculator.loadGlobalDefinitions();
	const std::vector<std::string> script_lines = {
		"values = [1, 2, 3]", "for elt in values:", "  elt", "unrelated = 42"
	};
	const auto script_first = script_session.evaluate(script_session_calculator, script_lines);
	assert(script_first[2].display == "[1 · 2 · 3]");
	assert(script_first[3].display == "42");
	const auto script_second = script_session.evaluate(script_session_calculator, {
		"values = [1, 2, 3]", "for elt in values:", "  elt + 1", "unrelated = 42"
	}, 2);
	assert(script_second[2].display == "[2 · 3 · 4]");
	assert(script_second[3].display == "42");
	const auto script_third = script_session.evaluate(script_session_calculator, {
		"values = [1, 2, 3]", "for elt in values:", "  elt + 2", "unrelated = 42"
	}, 2);
	assert(script_third[2].display == "[3 · 4 · 5]");
	assert(script_third[3].display == "42");
	const std::vector<std::string> independent_lines = {"x = 5", "2 + 2", "y = x + 1"};
	const auto independent_first = session.evaluate(session_calculator, independent_lines);
	assert(independent_first[1].display == "4");
	assert(independent_first[2].display == "6");
	const auto independent_second = session.evaluate(session_calculator, {"x = 5", "3 + 3", "y = x + 1"}, 1);
	assert(independent_second[0].display == "5");
	assert(independent_second[1].display == "6");
	assert(independent_second[2].display == "6");
	return 0;
}
