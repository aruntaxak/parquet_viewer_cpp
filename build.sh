#!/bin/sh
# Build the C++ port. No installs: clang++ from the Xcode command line tools,
# Arrow C++ headers + dylibs taken from the pyarrow wheel already on this
# machine, Dear ImGui vendored in imgui/.
#
#   ./build.sh          -> parquet-viewer (repo root, objects in build/)
#   PYARROW_DIR=... ./build.sh
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
OBJ="$HERE/build"
mkdir -p "$OBJ"

PA=${PYARROW_DIR:-$(python3 -c 'import pyarrow, os; print(os.path.dirname(pyarrow.__file__))')}
if [ ! -d "$PA/include/arrow" ] || [ ! -d "$PA/include/parquet" ]; then
  echo "arrow headers not found under $PA/include" >&2
  exit 1
fi
ARROW_LIB=$(ls "$PA"/libarrow.*.dylib 2>/dev/null | head -1)
PARQUET_LIB=$(ls "$PA"/libparquet.*.dylib 2>/dev/null | head -1)
if [ -z "$ARROW_LIB" ] || [ -z "$PARQUET_LIB" ]; then
  echo "libarrow/libparquet dylibs not found under $PA" >&2
  exit 1
fi

CXX=clang++
INC="-I$HERE/imgui -I$HERE/imgui/backends -I$PA/include"
FLAGS="-std=c++20 -O2 -Wall -Wno-deprecated-declarations -Wno-unused-parameter"
FRAMEWORKS="-framework Cocoa -framework AppKit -framework Metal -framework QuartzCore -framework Foundation -framework GameController"

echo "arrow: $PA"

# 1. Dear ImGui core (plain C++)
for f in imgui imgui_draw imgui_tables imgui_widgets; do
  $CXX $FLAGS $INC -c "$HERE/imgui/$f.cpp" -o "$OBJ/$f.o"
done

# 2. Dear ImGui macOS backends (Objective-C++, not ARC: the backends keep
#    Objective-C pointers inside C structs, which ARC rejects)
for f in imgui_impl_osx imgui_impl_metal; do
  $CXX $FLAGS -fno-objc-arc $INC -c "$HERE/imgui/backends/$f.mm" -o "$OBJ/$f.o"
done

# 3. the viewer itself (Objective-C++ so the platform layer can live in it)
$CXX $FLAGS -fobjc-arc -c -x objective-c++ "$HERE/main.cpp" -o "$OBJ/main.o" $INC

# 4. link
$CXX $FLAGS -o "$HERE/parquet-viewer" \
  "$OBJ/main.o" "$OBJ/imgui.o" "$OBJ/imgui_draw.o" "$OBJ/imgui_tables.o" \
  "$OBJ/imgui_widgets.o" "$OBJ/imgui_impl_osx.o" "$OBJ/imgui_impl_metal.o" \
  "$PARQUET_LIB" "$ARROW_LIB" \
  $FRAMEWORKS \
  -Wl,-rpath,"$PA"

ls -l "$HERE/parquet-viewer"
echo "run: $HERE/parquet-viewer [file.parquet]"
