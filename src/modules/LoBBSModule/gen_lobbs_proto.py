#!/usr/bin/env python3
# trunk-ignore-all(ruff/F821)
# trunk-ignore-all(flake8/F821): For SConstruct imports
"""
Custom build script to generate LoBBS protobuf files.
Can be run standalone or imported by platformio-custom.py.
"""
import os
import subprocess
import sys


def generate_lobbs_protobufs(source=None, target=None, env=None):
    """Generate LoBBS protobuf C++ files using nanopb."""

    # Determine script directory - handle both standalone and SCons execution
    if source and len(source) > 0:
        # When called from SCons, use the source file path
        source_path = str(source[0])
        script_dir = os.path.dirname(os.path.abspath(source_path))
    else:
        # When run standalone, use __file__
        script_dir = os.path.dirname(os.path.abspath(__file__))
    lobbs_module_dir = script_dir
    # Go up 3 levels to get to project root: LoBBSModule -> modules -> src -> project_root
    project_dir = os.path.dirname(os.path.dirname(os.path.dirname(script_dir)))
    proto_file = os.path.join(lobbs_module_dir, "lobbs.proto")
    options_file = os.path.join(lobbs_module_dir, "lobbs.options")
    nanopb_dir = os.path.join(project_dir, "nanopb-0.4.9")

    # Check if proto file exists
    if not os.path.exists(proto_file):
        print(f"Warning: LoBBS proto file not found at {proto_file}")
        return

    # Check if nanopb exists
    if not os.path.exists(nanopb_dir):
        print(f"Warning: nanopb-0.4.9 not found at {nanopb_dir}")
        print("Please download nanopb 0.4.9 from https://jpa.kapsi.fi/nanopb/download/")
        return

    # Determine protoc executable based on platform
    if sys.platform.startswith("win"):
        protoc_exe = os.path.join(nanopb_dir, "generator-bin", "protoc.exe")
    else:
        protoc_exe = os.path.join(nanopb_dir, "generator-bin", "protoc")

    if not os.path.exists(protoc_exe):
        print(f"Warning: protoc executable not found at {protoc_exe}")
        return

    # Check if generated files already exist and are up to date
    pb_header = os.path.join(lobbs_module_dir, "lobbs.pb.h")
    pb_source = os.path.join(lobbs_module_dir, "lobbs.pb.cpp")

    if os.path.exists(pb_header) and os.path.exists(pb_source):
        proto_mtime = os.path.getmtime(proto_file)
        header_mtime = os.path.getmtime(pb_header)
        source_mtime = os.path.getmtime(pb_source)

        if header_mtime > proto_mtime and source_mtime > proto_mtime:
            if os.path.exists(options_file):
                options_mtime = os.path.getmtime(options_file)
                if header_mtime > options_mtime and source_mtime > options_mtime:
                    print("LoBBS protobuf files are up to date")
                    return
            else:
                print("LoBBS protobuf files are up to date")
                return

    print("=" * 60)
    print("Generating LoBBS protobuf files...")
    print("=" * 60)

    # Build the protoc command
    # nanopb output format: -S.cpp generates .cpp instead of .c
    # -v is verbose
    cmd = [
        protoc_exe,
        "--experimental_allow_proto3_optional",
        f"--nanopb_out=-S.cpp -v:{lobbs_module_dir}",
        f"-I={lobbs_module_dir}",
        proto_file,
    ]

    print(f"Running: {' '.join(cmd)}")

    try:
        # Change to the module directory so nanopb can find the .options file
        original_dir = os.getcwd()
        os.chdir(lobbs_module_dir)

        result = subprocess.run(cmd, check=True, capture_output=True, text=True)

        if result.stdout:
            print(result.stdout)
        if result.stderr:
            print(result.stderr)

        print(f"✓ Generated {pb_header}")
        print(f"✓ Generated {pb_source}")
        print("=" * 60)

    except subprocess.CalledProcessError as e:
        print(f"Error generating LoBBS protobufs: {e}")
        if e.stdout:
            print(e.stdout)
        if e.stderr:
            print(e.stderr)
        sys.exit(1)
    finally:
        os.chdir(original_dir)


# When run as a standalone script
if __name__ == "__main__":
    print("Running LoBBS protobuf generator in standalone mode...")
    generate_lobbs_protobufs()
    sys.exit(0)

# When imported by platformio-custom.py as an SConscript
try:
    Import("env")
    # Add pre-build action to generate protobuf files
    env.AddPreAction(
        "$BUILD_DIR/src/modules/LoBBSModule/LoBBSModule.cpp.o", generate_lobbs_protobufs
    )
    print("LoBBS protobuf generation script loaded")
except Exception:
    # Not running as an SConscript, ignore
    pass
