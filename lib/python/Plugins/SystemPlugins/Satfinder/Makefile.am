installdir = $(pkglibdir)/python/Plugins/SystemPlugins/Satfinder

SUBDIRS = meta

install_PYTHON = \
	__init__.py \
	plugin.py

# Remove dvbreader.so from install_PYTHON as it's not a Python file
# but a binary module that will be built and installed separately

# Define the Python extension module
pyexecdir = $(pkglibdir)/python/Plugins/SystemPlugins/Satfinder
pyexec_LTLIBRARIES = dvbreader.la

# Source files for the module
dvbreader_la_SOURCES = dvbreader.c

# Compiler flags - these will be automatically set by the build system
# to include the correct Python headers for the target architecture
dvbreader_la_CFLAGS = $(PYTHON_CPPFLAGS) -Wall -Wextra -O2
dvbreader_la_LDFLAGS = -module -avoid-version -shared
dvbreader_la_LIBADD = $(PYTHON_LIBS)

# For manual compilation outside the build system (for testing only):
dvbreader.so: dvbreader.c
	$(CC) $(CFLAGS) $(PYTHON_CPPFLAGS) -fPIC -Wall -Wextra -O2 -shared $< -o $@ $(PYTHON_LIBS)

# Clean up
clean-local:
	rm -f *.so *.o *.lo *.la
	rm -rf .libs

.PHONY: clean-local
