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
    echo "[+] Kernel root: $GKI_ROOT"
    echo "[+] Module dir: $LSE_MODULE_DIR"

    # In ABK context, the module is already cloned. Skip re-cloning.
    if [ -f "$LSE_MODULE_DIR/Kconfig" ] && [ -f "$LSE_MODULE_DIR/Makefile" ]; then
        echo "[+] Using existing module directory."
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
                python3 - "$BUILD_BAZEL" "$MODULE_OUT" <<'PYEOF'
import sys, re

bazel_path = sys.argv[1]
module_out = sys.argv[2]

with open(bazel_path, 'r') as f:
    content = f.read()

# Find the kernel_aarch64 module_outs list and insert before the closing ]
# Pattern: module_outs = [\n  ...\n]
# We insert the new entry before the ] that closes this specific list
pattern = r'(module_outs\s*=\s*\[)([^\]]*)(\])'

def insert_module(m):
    header = m.group(1)
    items = m.group(2)
    footer = m.group(3)
    new_entry = '        "' + module_out + '",\n'
    # Check indentation of existing items for consistency
    existing_lines = [l for l in items.split('\n') if l.strip()]
    if existing_lines:
        indent = re.match(r'(\s*)', existing_lines[-1]).group(1)
    else:
        indent = '        '
    if items.rstrip():
        # There are existing entries - ensure trailing comma and add new entry
        if not items.rstrip().endswith(','):
            items = items.rstrip() + ',\n'
        else:
            items = items.rstrip() + '\n'
        return header + items + indent + '"' + module_out + '",\n' + footer
    else:
        # Empty list
        return header + '\n' + indent + '"' + module_out + '",\n' + footer

new_content = re.sub(pattern, insert_module, content, count=1)

with open(bazel_path, 'w') as f:
    f.write(new_content)

print(f"[+] Added {module_out} to module_outs in {bazel_path}")
PYEOF
            else
                echo "[+] $BUILD_BAZEL already contains module_outs entry."
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