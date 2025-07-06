import os
import sys

# Path to the Abseil header file
BASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', 'subprojects', 'abseil-cpp-20240722.0'))
TARGET_FILE = os.path.join(BASE_DIR, 'absl', 'base', 'internal', 'per_thread_tls.h')

# Patch block to inject
PATCH_BLOCK = '''#elif defined(__INTEL_LLVM_COMPILER)
#define ABSL_PER_THREAD_TLS_KEYWORD __thread
#define ABSL_PER_THREAD_TLS 1
'''

# Line to look for as the insert marker
MARKER_LINE = '#elif defined(_MSC_VER)'

def inject_tls_patch():
    if not os.path.isfile(TARGET_FILE):
        print(f"❌ File not found: {TARGET_FILE}")
        sys.exit(1)

    with open(TARGET_FILE, 'r', encoding='utf-8') as f:
        lines = f.readlines()

    # Check if already patched
    if any('__INTEL_LLVM_COMPILER' in line for line in lines):
        print("⚠️ TLS patch already exists. Skipping.")
        return

    # Find the marker line for ABSL_HAVE_TLS
    for i, line in enumerate(lines):
        if MARKER_LINE in line:
            insert_index = i
            break
    else:
        print("❌ Marker not found: '#elif defined(ABSL_HAVE_TLS)'")
        sys.exit(1)

    # Inject the patch block before the marker
    lines.insert(insert_index, PATCH_BLOCK + '\n')

    with open(TARGET_FILE, 'w', encoding='utf-8') as f:
        f.writelines(lines)

    print("✅ Intel TLS patch injected above '#elif defined(ABSL_HAVE_TLS)'.")

if __name__ == "__main__":
    inject_tls_patch()