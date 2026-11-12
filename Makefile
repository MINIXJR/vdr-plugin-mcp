#
# Makefile for the VDR plugin mcp
#

PLUGIN = mcp

### The version number of this plugin (taken from the main source file):

VERSION = $(shell grep 'static const char \*VERSION' $(PLUGIN).cpp | awk '{ print $$6 }' | sed -e 's/[";]//g')

### The directory environment:

PKG_CONFIG ?= pkg-config
PKGCFG = $(if $(VDRDIR),$(shell $(PKG_CONFIG) --variable=$(1) $(VDRDIR)/vdr.pc),$(shell PKG_CONFIG_PATH="$$PKG_CONFIG_PATH:../../.." $(PKG_CONFIG) --variable=$(1) vdr))
LIBDIR = $(call PKGCFG,libdir)
PLGCFG = $(call PKGCFG,plgcfg)

TMPDIR ?= /tmp

### The compiler options:

export CFLAGS   = $(call PKGCFG,cflags) -fPIC
export CXXFLAGS = $(call PKGCFG,cxxflags) -fPIC

### The version number of VDR's plugin API:

APIVERSION = $(call PKGCFG,apiversion)

### Allow user defined options to overwrite defaults:

-include $(PLGCFG)

### The name of the distribution archive:

ARCHIVE = $(PLUGIN)-$(VERSION)
PACKAGE = vdr-$(ARCHIVE)

### The name of the shared object file:

SOFILE = libvdr-$(PLUGIN).so

### Includes, defines and libraries (nlohmann/json is header only):

DEFINES += -DPLUGIN_NAME_I18N='"$(PLUGIN)"'
INCLUDES += $(shell $(PKG_CONFIG) --cflags libsystemd openssl)
LIBS += $(shell $(PKG_CONFIG) --libs libsystemd openssl)

### The object files:

OBJS = $(PLUGIN).o http.o journal.o keys.o recording.o server.o session.o svdrp.o

### The main target:

all: $(SOFILE)

### Implicit rules:

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $(DEFINES) $(INCLUDES) -o $@ $<

$(OBJS): Makefile $(wildcard *.h)

### Targets:

$(SOFILE): $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -shared $(OBJS) $(LIBS) -o $@

install-lib: $(SOFILE)
	install -D -m644 $^ $(DESTDIR)$(LIBDIR)/$^.$(APIVERSION)

install: install-lib

dist: clean
	@-rm -rf $(TMPDIR)/$(ARCHIVE)
	@mkdir $(TMPDIR)/$(ARCHIVE)
	@cp -a * $(TMPDIR)/$(ARCHIVE)
	@tar czf $(PACKAGE).tgz -C $(TMPDIR) $(ARCHIVE)
	@-rm -rf $(TMPDIR)/$(ARCHIVE)
	@echo Distribution package created as $(PACKAGE).tgz

clean:
	@-rm -f $(OBJS) $(SOFILE) *.tgz
	@-rm -rf $(TMPDIR)/$(ARCHIVE)
