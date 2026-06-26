#!/bin/sh
set -eu

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

perform_cleanup() {
    echo "[+] Cleaning up..."
    [ -L "$DRIVER_STAGING_DIR/lunarkernel_sched_extention" ] && rm "$DRIVER_STAGING_DIR/lunarkernel_sched_extention" && echo "[-] Symlink removed."
    grep -q "lunarkernel_sched_extention" "$DRIVER_STAGING_MAKEFILE" && sed -i '/lunarkernel_sched_extention/d' "$DRIVER_STAGING_MAKEFILE" && echo "[-] Makefile reverted."
    grep -q "drivers/staging/lunarkernel_sched_extention/Kconfig" "$DRIVER_STAGING_KCONFIG" && sed -i '/drivers/staging\/lunarkernel_sched_extention\/Kconfig/d' "$DRIVER_STAGING_KCONFIG" && echo "[-] Kconfig reverted."
    if [ -d "$GKI_ROOT/lunarkernel_sched_extention" ]; then
        rm -rf "$GKI_ROOT/lunarkernel_sched_extention" && echo "[-] lunarkernel_sched_extention directory deleted."
    fi
}

setup_LSE() {
    echo "[+] Setting up lunarkernel_sched_extention..."

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

    grep -q "lunarkernel_sched_extention" "$DRIVER_STAGING_MAKEFILE" || printf "\nobj-\$(CONFIG_LUNAR_SCHED_EXT) += lunarkernel_sched_extention/\n" >> "$DRIVER_STAGING_MAKEFILE" && echo "[+] Modified Makefile."
    grep -q "source \"drivers/staging/lunarkernel_sched_extention/Kconfig\"" "$DRIVER_STAGING_KCONFIG" || sed -i "/endif/i\source \"drivers/staging/lunarkernel_sched_extention/Kconfig\"" "$DRIVER_STAGING_KCONFIG" && echo "[+] Modified Kconfig."

    MODULE_OUT="drivers/staging/lunarkernel_sched_extention/lunar_bsp_ext_sched.ko"
    for BUILD_BAZEL in "$GKI_ROOT/common/BUILD.bazel" "$GKI_ROOT/BUILD.bazel"; do
        if [ -f "$BUILD_BAZEL" ] && grep -q "module_outs" "$BUILD_BAZEL"; then
            if grep -q "$MODULE_OUT" "$BUILD_BAZEL"; then
                echo "[+] $BUILD_BAZEL already contains $MODULE_OUT"
                break
            fi

            BUILDOZER=""
            if command -v buildozer >/dev/null 2>&1; then
                BUILDOZER="buildozer"
            elif [ -f "/usr/local/bin/buildozer" ]; then
                BUILDOZER="/usr/local/bin/buildozer"
            fi

            if [ -z "$BUILDOZER" ]; then
                echo "[+] Downloading buildozer..."
                curl -sLo /tmp/buildozer "https://github.com/bazelbuild/buildtools/releases/download/v7.3.1/buildozer-linux-amd64" 2>/dev/null \
                || curl -sLo /tmp/buildozer "https://github.com/bazelbuild/buildtools/releases/latest/download/buildozer-linux-amd64" 2>/dev/null \
                || true
                if [ -f /tmp/buildozer ] && [ -s /tmp/buildozer ]; then
                    chmod +x /tmp/buildozer
                    BUILDOZER="/tmp/buildozer"
                fi
            fi

            if [ -n "$BUILDOZER" ]; then
                echo "[+] Using buildozer to add module_outs"
                cd "$GKI_ROOT"
                "$BUILDOZER" "add module_outs $MODULE_OUT" "//common:kernel_aarch64" && echo "[+] buildozer succeeded" || echo "[!] buildozer failed, falling back to sed"
                cd "$DRIVER_STAGING_DIR"
            fi

            if [ -z "$BUILDOZER" ] || ! grep -q "$MODULE_OUT" "$BUILD_BAZEL"; then
                echo "[+] Falling back to sed insertion"
                python3 - "$BUILD_BAZEL" "$MODULE_OUT" <<'PYEOF'
import sys
bazel_file = sys.argv[1]
module_out = sys.argv[2]
with open(bazel_file, 'r') as f:
    content = f.read()
idx = content.find('module_outs')
if idx < 0:
    print("[!] module_outs not found"); sys.exit(1)
bracket_start = content.find('[', idx)
if bracket_start < 0:
    print("[!] [ not found after module_outs"); sys.exit(1)
depth = 0
pos = bracket_start
while pos < len(content):
    if content[pos] == '[': depth += 1
    elif content[pos] == ']':
        depth -= 1
        if depth == 0:
            indent = "        "
            insert = f'{indent}"{module_out}",\n'
            content = content[:pos] + insert + content[pos:]
            with open(bazel_file, 'w') as f:
                f.write(content)
            print(f"[+] Added {module_out} to module_outs")
            break
    pos += 1
PYEOF
            fi
            break
        fi
    done

    echo '[+] Done.'
}

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
