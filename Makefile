# ============================================================
# CyzonVSModManager - Makefile
# ============================================================

CXX = g++
WINDRES = windres

TARGET = CyzonVSMM.exe

SDL_DIR = C:/Users/fkachouh/Documents/SDL3-3.4.12/x86_64-w64-mingw32
IMGUI_DIR = external/imgui
BUILD_DIR = build

RC_FILE = resource.rc
RES_FILE = $(BUILD_DIR)/resource.res

ISCC = C:/Users/fkachouh/AppData/Local/Programs/Inno Setup 7/ISCC.exe
ISS_FILE = installer.iss

# ============================================================
# COMPILER FLAGS
# ============================================================

CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -I$(SDL_DIR)/include -Iexternal -I$(IMGUI_DIR)

LDFLAGS = -L$(SDL_DIR)/lib

LIBS = -lSDL3 -lopengl32 -lwinhttp -lshell32 -lole32 -luuid -lcomdlg32

# ============================================================
# SOURCE FILES
# ============================================================

SOURCES = main.cpp \
	  $(IMGUI_DIR)/imgui.cpp \
	  $(IMGUI_DIR)/imgui_draw.cpp \
	  $(IMGUI_DIR)/imgui_tables.cpp \
	  $(IMGUI_DIR)/imgui_widgets.cpp \
	  $(IMGUI_DIR)/backends/imgui_impl_sdl3.cpp \
	  $(IMGUI_DIR)/backends/imgui_impl_opengl3.cpp

OBJECTS = $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(SOURCES))
OBJECT_DIR = $(BUILD_DIR)/external/imgui/backends

# ============================================================
# TARGETS
# ============================================================

.PHONY: all clean rebuild run installer release copy-dll

all: $(TARGET) copy-dll

# ============================================================
# CREATE BUILD DIRECTORIES
# ============================================================

$(BUILD_DIR):
ifeq ($(OS),Windows_NT)
	if not exist "$(subst /,\,$@)" mkdir "$(subst /,\,$@)"
else
	mkdir -p "$@"
endif

$(OBJECT_DIR): | $(BUILD_DIR)
ifeq ($(OS),Windows_NT)
	if not exist "$(subst /,\,$@)" mkdir "$(subst /,\,$@)"
else
	mkdir -p "$@"
endif

# ============================================================
# COMPILE CPP FILES
# ============================================================

$(BUILD_DIR)/%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJECTS): | $(OBJECT_DIR)

# ============================================================
# COMPILE WINDOWS RESOURCE
# ============================================================

$(RES_FILE): $(RC_FILE) | $(BUILD_DIR)
	$(WINDRES) $(RC_FILE) -O coff -o $(RES_FILE)

# ============================================================
# LINK EXECUTABLE
# ============================================================

$(TARGET): $(OBJECTS) $(RES_FILE)
	$(CXX) $(OBJECTS) $(RES_FILE) -o $@ $(LDFLAGS) $(LIBS)
	@echo Build successful: $(TARGET)

# ============================================================
# COPY SDL3 DLL
# ============================================================

copy-dll:
ifeq ($(OS),Windows_NT)
	copy /Y "$(subst /,\,$(SDL_DIR))\bin\SDL3.dll" SDL3.dll
else
	cp -f "$(SDL_DIR)/bin/SDL3.dll" SDL3.dll
endif

# ============================================================
# BUILD INSTALLER
# ============================================================

installer: all
ifeq ($(OS),Windows_NT)
	if not exist "installer" mkdir "installer"
else
	mkdir -p "installer"
endif
	"$(ISCC)" "$(ISS_FILE)"

# ============================================================
# FULL RELEASE
# ============================================================

release: installer
	@echo Release build completed!
	@echo Installer: installer/CyzonVSModManager-Setup.exe

# ============================================================
# RUN
# ============================================================

run: all
ifeq ($(OS),Windows_NT)
	.\$(TARGET)
else
	./$(TARGET)
endif

# ============================================================
# CLEAN
# ============================================================

clean:
ifeq ($(OS),Windows_NT)
	if exist "$(subst /,\,$(BUILD_DIR))" rmdir /S /Q "$(subst /,\,$(BUILD_DIR))"
	if exist "$(TARGET)" del /Q "$(TARGET)"
else
	rm -rf $(BUILD_DIR)
	rm -f $(TARGET)
endif

rebuild: clean all