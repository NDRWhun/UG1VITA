#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
sdk_root=${VITASDK:-"$PWD/.deps/vitasdk"}
if [ ! -f "$sdk_root/arm-vita-eabi/include/psp2/ctrl.h" ]; then
    echo 'Set VITASDK or download the headers with: python3 -I tools/deps.py vitasdk' >&2
    exit 1
fi
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -Iinclude \
    -idirafter "$sdk_root/arm-vita-eabi/include" \
    tests/controls_test.cpp src/lib/controls.cpp src/platform/config.cpp -o "$test_dir/controls-test"
"$test_dir/controls-test"
"$test_dir/controls-test" examples/nfsug1vita.cfg
printf 'rear_touch=on\n' > "$test_dir/rear-on.cfg"
"$test_dir/controls-test" "$test_dir/rear-on.cfg" on
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -Iinclude \
    tests/config_save_test.cpp src/platform/config.cpp -o "$test_dir/config-save-test"
"$test_dir/config-save-test" "$test_dir/save.cfg"
case "$(uname -s)" in
    Darwin) strip_sections=-Wl,-dead_strip ;;
    *) strip_sections=-Wl,--gc-sections ;;
esac
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -Iinclude \
    -idirafter "$sdk_root/arm-vita-eabi/include" \
    tests/controls_menu_test.cpp src/nfsu/controls_menu.cpp src/lib/controls.cpp src/platform/config.cpp \
    "$strip_sections" -o "$test_dir/controls-menu-test"
(cd "$test_dir" && ./controls-menu-test "$test_dir/menu.cfg")
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -Iinclude \
    -idirafter "$sdk_root/arm-vita-eabi/include" \
    tests/driving_input_test.cpp src/nfsu/driving_input.cpp src/lib/controls.cpp src/platform/config.cpp \
    "$strip_sections" -o "$test_dir/driving-input-test"
"$test_dir/driving-input-test"
