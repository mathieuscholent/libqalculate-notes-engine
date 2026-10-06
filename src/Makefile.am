#
# src/Makefile.am for qalculate
#

AM_CPPFLAGS = \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-I$(top_srcdir) \
	-I$(top_srcdir)/libqalculate \
	@LIBCURL_CFLAGS@ @LIBXML_CFLAGS@ @ICU_CFLAGS@

bin_PROGRAMS = @QALCULATE_TEXT@
noinst_PROGRAMS = @QALCULATE_DEFS2DOC@ @QALCULATE_TEST@ @QALCULATE_UNITTEST@ note-evaluator-test
EXTRA_PROGRAMS = qalc defs2doc test unittest
check_PROGRAMS = threadtest
TESTS_ENVIRONMENT = QALCULATE_DEFINITIONS_DIR=$(abs_top_builddir)/data
TESTS = unittest threadtest

qalc_SOURCES = qalc.cc QalculateScript.cc QalculateScript.h NoteEvaluator.cc NoteEvaluator.h

qalc_LDADD = \
	@LIBXML_LIBS@ \
	@LIBCURL_LIBS@ \
	@ICU_LIBS@ \
	@READLINE_LIBS@ \
	@LTLIBINTL@ \
	@LTLIBICONV@ \
	../libqalculate/libqalculate.la

note_evaluator_test_SOURCES = NoteEvaluatorTest.cc NoteEvaluator.cc NoteEvaluator.h QalculateScript.cc QalculateScript.h
note_evaluator_test_LDADD = $(qalc_LDADD)

defs2doc_SOURCES = defs2doc.cc

defs2doc_LDADD = \
	@LIBXML_LIBS@ \
	@LIBCURL_LIBS@ \
	@ICU_LIBS@ \
	@LTLIBINTL@ \
	@LTLIBICONV@ \
	../libqalculate/libqalculate.la

test_SOURCES = test.cc

test_LDADD = \
	@LIBXML_LIBS@ \
	@LIBCURL_LIBS@ \
	@ICU_LIBS@ \
	@LTLIBINTL@ \
	@LTLIBICONV@ \
	../libqalculate/libqalculate.la

unittest_SOURCES = unittest.cc

threadtest_SOURCES = threadtest.cc
threadtest_LDADD = $(test_LDADD)

#install-exec-local:
#	cd $(DESTDIR)$(bindir) && rm -f qalculate; $(LN_S) @LN_QALCULATE@ qalculate
