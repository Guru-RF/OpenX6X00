# Host test of the GUI's languages (src/i18n.cpp, test/: its own language files in share/ and user/) and the check of
# po/ (potool.py check): `make test`.
# SPDX-License-Identifier: LGPL-2.1-or-later
SRC      := ../../src
CXX      ?= g++
CXXFLAGS ?= -O1 -g -Wall -Wextra -std=c++17
DEFS     := -DI18N_NO_BOARD -DI18N_DIR='"share"' -DI18N_USER_DIR='"user"' -DI18N_FONT_DIR='"fonts"'

test: test/i18n_test
	cd test && ASAN_OPTIONS=detect_leaks=0 ./i18n_test   # the languages are kept on purpose (i18n.cpp)
	python3 -I potool.py check   # po/: no translation whose conversions differ from the English

test/i18n_test: test/i18n_test.cpp $(SRC)/i18n.cpp $(SRC)/i18n.h
	$(CXX) $(CXXFLAGS) $(DEFS) -fsanitize=address,undefined -I$(SRC) -o $@ test/i18n_test.cpp $(SRC)/i18n.cpp

clean:
	rm -f test/i18n_test

.PHONY: test clean
