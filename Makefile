CXX = g++
# Define the resource compiler
RC = windres

TARGET = CyzonVSMM.exe

CXXFLAGS = -std=c++17 -O2 -Wall -Wextra

SDL_DIR = C:/Users/fkachouh/Documents/SDL3-3.4.12/x86_64-w64-mingw32

IMGUI_DIR = external/imgui
JSON_DIR = external

INCLUDES = -I"$(SDL_DIR)/include" \
           -I"$(IMGUI_DIR)" \
           -I"$(JSON_DIR)"

LIB_DIRS = -L"$(SDL_DIR)/lib"

LIBS = -lSDL3 \
       -lopengl32 \
       -lwinhttp \
       -lshell32 \
       -lole32 \
       -luuid

SOURCES = main.cpp \
          $(IMGUI_DIR)/imgui.cpp \
          $(IMGUI_DIR)/imgui_draw.cpp \
          $(IMGUI_DIR)/imgui_tables.cpp \
          $(IMGUI_DIR)/imgui_widgets.cpp \
          $(IMGUI_DIR)/backends/imgui_impl_sdl3.cpp \
          $(IMGUI_DIR)/backends/imgui_impl_opengl3.cpp

OBJECTS = $(SOURCES:.cpp=.o)

# 1. Define your resource source and compiled output object
RC_SOURCE = resource.rc
RC_OBJECT = resource.res

build: $(TARGET)

# 2. Append $(RC_OBJECT) to both the dependencies and the compiler execution command
$(TARGET): $(OBJECTS) $(RC_OBJECT)
	$(CXX) $(OBJECTS) $(RC_OBJECT) $(LIB_DIRS) $(LIBS) -o $(TARGET)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# 3. Add a pattern rule to compile your .rc file into a COFF object (.res)
$(RC_OBJECT): $(RC_SOURCE)
	$(RC) $(RC_SOURCE) -O coff -o $(RC_OBJECT)

clean:
	del /Q $(TARGET) 2>NUL || exit 0
	del /Q $(RC_OBJECT) 2>NUL || exit 0
	for /R %%f in (*.o) do del /Q "%%f" 2>NUL

rebuild: clean build

.PHONY: build clean rebuild
