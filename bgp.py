#!/usr/bin/env python3
"""
bgp.py – BGP launcher with automatic dependency installation
Supports Linux (apt/yum/dnf) and Termux (pkg)
Usage:
    python3 bgp.py          # Interactive
    python3 bgp.py auto     # Automated test
    python3 bgp.py manual   # Custom args
"""

import os
import sys
import subprocess
import platform
import shutil

# --------------------------------------------
# Utility functions
# --------------------------------------------
def is_termux():
    """Detect if running in Termux environment."""
    return (os.path.exists('/data/data/com.termux/files/usr') or
            'TERMUX_VERSION' in os.environ)

def is_root():
    return os.geteuid() == 0

def check_command(cmd):
    return shutil.which(cmd) is not None

def ensure_jansson():
    """Check if Jansson library is available via pkg-config."""
    try:
        subprocess.check_call(['pkg-config', '--exists', 'jansson'],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return True
    except:
        return False

# --------------------------------------------
# Dependency installation
# --------------------------------------------
def install_packages():
    """Install missing packages using the appropriate package manager."""
    if is_termux():
        if is_root():
            print("\033[1;31mError:\033[0m Running as root in Termux is not allowed by 'pkg'.")
            print("Please run the script without 'sudo' to enable automatic installation.")
            print("Example: python3 bgp.py\n")
            return False
        cmd = ['pkg', 'install', '-y']
        packages = []
        if not check_command('gcc'):
            packages.append('gcc')
        if not check_command('pkg-config'):
            packages.append('pkg-config')
        if not ensure_jansson():
            packages.append('libjansson')
        if not check_command('make'):
            packages.append('make')
        if not packages:
            return True
        print("Installing missing packages:", ' '.join(packages))
        try:
            subprocess.check_call(cmd + packages)
            return True
        except Exception as e:
            print("Installation failed:", e)
            return False
    else:
        # Standard Linux
        system = platform.system().lower()
        if system != 'linux':
            print("Auto-install only supported on Linux and Termux.")
            return False

        use_sudo = not is_root()
        if shutil.which('apt-get'):
            cmd = ['apt-get', 'install', '-y'] if not use_sudo else ['sudo', 'apt-get', 'install', '-y']
            packages = []
            if not check_command('gcc'):
                packages.append('gcc')
            if not check_command('pkg-config'):
                packages.append('pkg-config')
            if not ensure_jansson():
                packages.append('libjansson-dev')
            if not check_command('make'):
                packages.append('make')
        elif shutil.which('yum'):
            cmd = ['yum', 'install', '-y'] if not use_sudo else ['sudo', 'yum', 'install', '-y']
            packages = []
            if not check_command('gcc'):
                packages.append('gcc')
            if not check_command('pkg-config'):
                packages.append('pkgconfig')
            if not ensure_jansson():
                packages.append('jansson-devel')
            if not check_command('make'):
                packages.append('make')
        elif shutil.which('dnf'):
            cmd = ['dnf', 'install', '-y'] if not use_sudo else ['sudo', 'dnf', 'install', '-y']
            packages = []
            if not check_command('gcc'):
                packages.append('gcc')
            if not check_command('pkg-config'):
                packages.append('pkgconfig')
            if not ensure_jansson():
                packages.append('jansson-devel')
            if not check_command('make'):
                packages.append('make')
        else:
            print("Unsupported package manager. Please install gcc, jansson, and pkg-config manually.")
            return False

        if not packages:
            return True
        print("Installing missing packages:", ' '.join(packages))
        try:
            subprocess.check_call(cmd + packages)
            return True
        except Exception as e:
            print("Installation failed:", e)
            return False

# --------------------------------------------
# Compilation and execution helpers
# --------------------------------------------
def compile_c_program():
    src = 'bgp.c'
    out = './bgp'
    if not os.path.exists(src):
        print("Error: bgp.c not found.")
        return False
    if os.path.exists(out):
        os.remove(out)
    compiler = 'gcc' if check_command('gcc') else 'clang'
    if not check_command(compiler):
        print(f"Compiler '{compiler}' not found. Please install gcc or clang.")
        return False
    flags = [compiler, '-o', out, src, '-ljansson', '-lpthread', '-lm', '-Wall', '-O2']
    print(f"Compiling {src} with {compiler}...")
    try:
        subprocess.check_call(flags)
        print("Compilation successful.")
        return True
    except subprocess.CalledProcessError as e:
        print("Compilation failed:", e)
        return False

def run_automated():
    """Run with a default test configuration."""
    args = ['./bgp', '-l', '3', '-R', '192.168.1.1,65001,TestPeer']
    print("Running automated test with:", ' '.join(args))
    subprocess.call(args)

def run_manual():
    if not os.path.exists('./bgp'):
        print("Binary not found. Please compile first.")
        return
    args = ['./bgp', '-h']
    subprocess.call(args)
    print("\nEnter your BGP command line arguments (e.g., -a 65000 10.0.0.1,65001):")
    user_input = input("> ").strip()
    if not user_input:
        print("No input, exiting.")
        return
    full_cmd = ['./bgp'] + user_input.split()
    subprocess.call(full_cmd)

# --------------------------------------------
# Main
# --------------------------------------------
def main():
    # Quick check for Termux + root
    if is_termux() and is_root():
        print("\033[1;31mError:\033[0m Running as root in Termux is not allowed by 'pkg'.")
        print("Please run the script without 'sudo' to enable automatic installation.")
        print("Example: python3 bgp.py\n")
        sys.exit(1)

    # Ensure dependencies
    if not check_command('gcc') and not check_command('clang'):
        print("No C compiler found. Attempting to install...")
        if not install_packages():
            print("Auto-install failed. Please install gcc or clang manually.")
            sys.exit(1)
    if not ensure_jansson():
        print("Jansson library not found. Attempting to install...")
        if not install_packages():
            print("Auto-install failed. Please install libjansson manually.")
            sys.exit(1)

    # Compile if needed
    if not os.path.exists('./bgp'):
        if not compile_c_program():
            sys.exit(1)

    # Determine mode
    if len(sys.argv) > 1:
        mode = sys.argv[1].lower()
        if mode == 'auto':
            run_automated()
        elif mode == 'manual':
            run_manual()
        else:
            print("Unknown mode. Use 'auto' or 'manual'.")
            sys.exit(1)
    else:
        print("BGP Launcher")
        print("1 - Automated test (connect to default peer)")
        print("2 - Manual (show usage and enter custom args)")
        choice = input("Choose (1/2): ").strip()
        if choice == '1':
            run_automated()
        elif choice == '2':
            run_manual()
        else:
            print("Invalid choice.")

if __name__ == '__main__':
    main()
