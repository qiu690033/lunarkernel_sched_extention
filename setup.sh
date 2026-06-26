#!/bin/sh
set -eu

# Use KERNEL_ROOT from ABK environment if available, otherwise fall back to pwd
GKI_ROOT="${KERNEL_ROOT:-$(pwd)}"
LSE_MODULE_DIR="$(cd "$(dirname "$0")" && pwd)"

display_usage() {
    echo "Usage: $0 [--cleanup | <commit-or-tag>]"
    echo "  --cleanup:              Cleans up previous modifications made by the script."
    echo "  -h, --help:             Displays this usage information."
    echo "  (no args):              Sets up the lunarkernel_sched_extention environment to the latest commit."
}

initialize_variables() {
    if test -d "$GKI_ROOT/drivers/staging"; then
         DRIVER_STAGING_DIR="$GKI_ROOT/drivers/staging"
    elif test -d "$GKI_ROOT/common/drivers/staging"; then
         DRIVER_STAGING_DIR="$GKI_ROOT/common/drivers/staging"
    else
         echo '[ERROR] "drivers/staging/" directory not found.'
         exit 127
    fi

    DRIVER_STAGING_MAKEFILE=$DRIVER_STAGING_DIR/Makefile
    DRIVER_STAGING_KCONFIG=$DRIVER_STAGING_DIR/Kconfig
}

# Reverts modifications made by this script
perform_cleanup() {
    echo "[+] Cleaning up..."
    [ -L "$DRIVER_STAGING_DIR/lunarkernel_sched_extention" ] && rm "$DRIVER_STAGING_DIR/lunarkernel_sched_extention" && echo "[-] Symlink removed."
    grep -q "lunarkernel_sched_extention" "$DRIVER_STAGING_MAKEFILE" && sed -i '/lunarkernel_sched_extention/d' "$DRIVER_STAGING_MAKEFILE" && echo "[-] Makefile reverted."
    grep -q "drivers/staging/lunarkernel_sched_extention/Kconfig" "$DRIVER_STAGING_KCONFIG" && sed -i '/drivers/staging\/lunarkernel_sched_extention\/Kconfig/d' "$DRIVER_STAGING_KCONFIG" && echo "[-] Kconfig reverted."
    if [ -d "$GKI_ROOT/lunarkernel_sched_extention" ]; then
        rm -rf "$GKI_ROOT/lunarkernel_sched_extention" && echo "[-] lunarkernel_sched_extention directory deleted."
    fi
}

# Sets up or update lunarkernel_sched_extention environment
setup_LSE() {
    echo "[+] Setting up lunarkernel_sched_extention..."

    # In ABK context, the module is already cloned. Skip re-cloning.
    if [ -f "$LSE_MODULE_DIR/Kconfig" ] && [ -f "$LSE_MODULE_DIR/Makefile" ]; then
        echo "[+] Using existing module directory: $LSE_MODULE_DIR"
    elif [ -d "$GKI_ROOT/lunarkernel_sched_extention" ]; then
        echo "[+] Using existing clone in kernel root."
        LSE_MODULE_DIR="$GKI_ROOT/lunarkernel_sched_extention"
    else
        git clone https://github.com/qiu690033/lunarkernel_sched_extention "$GKI_ROOT/lunarkernel_sched_extention" && echo "[+] Repository cloned."
        LSE_MODULE_DIR="$GKI_ROOT/lunarkernel_sched_extention"
    fi

    cd "$DRIVER_STAGING_DIR"
    ln -sf "$(realpath --relative-to="$DRIVER_STAGING_DIR" "$LSE_MODULE_DIR")" "lunarkernel_sched_extention" && echo "[+] Symlink created."

    # Add entries in Makefile and Kconfig if not already existing
    grep -q "lunarkernel_sched_extention" "$DRIVER_STAGING_MAKEFILE" || printf "\nobj-\$(CONFIG_LUNAR_SCHED_EXT) += lunarkernel_sched_extention/\n" >> "$DRIVER_STAGING_MAKEFILE" && echo "[+] Modified Makefile."
    grep -q "source \"drivers/staging/lunarkernel_sched_extention/Kconfig\"" "$DRIVER_STAGING_KCONFIG" || sed -i "/endif/i\source \"drivers/staging/lunarkernel_sched_extention/Kconfig\"" "$DRIVER_STAGING_KCONFIG" && echo "[+] Modified Kconfig."

    # Add module to Bazel BUILD.bazel module_outs if present (GKI 6.12+ uses Bazel)
    MODULE_OUT="drivers/staging/lunarkernel_sched_extention/lunar_bsp_ext_sched.ko"
    for BUILD_BAZEL in "$GKI_ROOT/common/BUILD.bazel" "$GKI_ROOT/BUILD.bazel"; do
        if [ -f "$BUILD_BAZEL" ] && grep -q "module_outs" "$BUILD_BAZEL"; then
            if ! grep -q "$MODULE_OUT" "$BUILD_BAZEL"; then
                echo "[+] Adding module_outs entry to $BUILD_BAZEL"
                python3 -c "
import sys, re
bazel_file = '$BUILD_BAZEL'
module_line = '        \"$MODULE_OUT\",\n'
with open(bazel_file, 'r') as f:
    lines = f.readlines()
new_lines = []
depth = 0
in_module_outs = False
inserted = False
for i, line in enumerate(lines):
    if not in_module_outs and re.match(r'\s*module_outs\s*=', line):
        in_module_outs = True
        depth = line.count('[') - line.count(']')
        new_lines.append(line)
        if depth == 0 and ']' in line and not inserted:
            idx = line.rindex(']')
            new_lines[-1] = line[:idx] + module_line.strip() + ',' + line[idx:]
            inserted = True
        continue
    if in_module_outs and not inserted:
        depth += line.count('[') - line.count(']')
        new_lines.append(line)
        if depth <= 0:
            new_lines.insert(-1, module_line)
            inserted = True
    else:
        new_lines.append(line)
with open(bazel_file, 'w') as f:
    f.writelines(new_lines)
print('[+] Added $MODULE_OUT to module_outs')
"
            else
                echo "[+] Already contains $MODULE_OUT"
            fi
            break
        fi
    done

    echo '[+] Done.'
}

# Process command-line arguments
if [ "$#" -eq 0 ]; then
    initialize_variables
    setup_LSE
elif [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    display_usage
elif [ "$1" = "--cleanup" ]; then
    initialize_variables
    perform_cleanup
else
    initialize_variables
    setup_LSE "$@"
fi
