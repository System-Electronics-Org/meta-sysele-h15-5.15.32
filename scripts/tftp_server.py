#!/usr/bin/env python3

"""
Astrial H15 TFTP Server
System Electronics - Hailo-15 Development Platform

This script provides a TFTP server for serving Yocto image files to the Astrial H15 board
during eMMC programming. The server automatically serves files from the build directory.

Prerequisites:
- Python 3 with tftpy module installed (pip install tftpy)
- Built Yocto image artifacts available
- Root privileges (sudo) for binding to port 69

Usage: sudo python3 tftp_server.py [build_directory]
Example: sudo python3 tftp_server.py ../build/tmp/deploy/images/astrial-h15

The server will listen on all interfaces (0.0.0.0) port 69.
Access from board: tftp 10.0.0.2 (assuming host IP is 10.0.0.2)
"""

import sys
import os
import argparse
from pathlib import Path

try:
    import tftpy
except ImportError:
    print("Error: tftpy module not found.")
    print("Please install it with: pip install tftpy")
    sys.exit(1)


def find_build_directory():
    """Find the build directory automatically"""
    possible_paths = [
        "../build/tmp/deploy/images/astrial-h15",
        "build/tmp/deploy/images/astrial-h15", 
        "../../build/tmp/deploy/images/astrial-h15"
    ]
    
    for path in possible_paths:
        if os.path.isdir(path):
            return path
    
    return None


def main():
    parser = argparse.ArgumentParser(description='TFTP Server for Astrial H15 board programming')
    parser.add_argument('build_dir', nargs='?', help='Build directory path (auto-detected if not specified)')
    parser.add_argument('--port', type=int, default=69, help='TFTP port (default: 69)')
    parser.add_argument('--bind', default='0.0.0.0', help='Bind address (default: 0.0.0.0)')
    
    args = parser.parse_args()
    
    # Check if running as root (required for port 69)
    if args.port < 1024 and os.geteuid() != 0:
        print("Error: Root privileges required for ports below 1024.")
        print("Please run with sudo:")
        print(f"  sudo python3 {sys.argv[0]} {' '.join(sys.argv[1:])}")
        sys.exit(1)
    
    # Determine build directory
    if args.build_dir:
        build_dir = args.build_dir
    else:
        build_dir = find_build_directory()
        
    if not build_dir or not os.path.isdir(build_dir):
        print("Error: Build directory not found.")
        print("")
        if not args.build_dir:
            print("Searched in the following locations:")
            print("  - ../build/tmp/deploy/images/astrial-h15")
            print("  - build/tmp/deploy/images/astrial-h15")
            print("  - ../../build/tmp/deploy/images/astrial-h15")
            print("")
        print("Please ensure you have:")
        print("1. Built the Yocto image successfully")
        print("2. Run this script from the correct directory, or")
        print("3. Specify the build directory manually:")
        print(f"   sudo python3 {sys.argv[0]} /path/to/build/tmp/deploy/images/astrial-h15")
        sys.exit(1)
    
    # Convert to absolute path
    build_dir = os.path.abspath(build_dir)
    
    print("=" * 50)
    print("Astrial H15 TFTP Server")
    print("=" * 50)
    print(f"Serving directory: {build_dir}")
    print(f"Bind address: {args.bind}")
    print(f"Port: {args.port}")
    print("")
    print("Available files:")
    
    # List available .wic files
    wic_files = list(Path(build_dir).glob("*.wic"))
    expected_filename = "core-image-hailo-dev-astrial-h15.wic"
    
    if wic_files:
        for wic_file in wic_files:
            print(f"  - {wic_file.name}")
            
        # Check if we have the expected filename, if not create a symlink
        expected_path = Path(build_dir) / expected_filename
        if not expected_path.exists() and wic_files:
            # Use the first .wic file found
            source_file = wic_files[0]
            try:
                os.symlink(source_file.name, expected_path)
                print(f"  - Created symlink: {expected_filename} -> {source_file.name}")
            except OSError as e:
                print(f"  - Warning: Could not create symlink {expected_filename}: {e}")
    else:
        print("  - No .wic files found")
        print("  - Make sure you have built the image: bitbake core-image-hailo-dev")
    
    print("")
    print("Network Configuration:")
    print("  - Set host IP to: 10.0.0.2")
    print("  - Board will use IP: 10.0.0.1")
    print("")
    print("Starting TFTP server...")
    print("Press Ctrl+C to stop")
    print("")
    
    try:
        server = tftpy.TftpServer(build_dir)
        server.listen(args.bind, args.port)
    except KeyboardInterrupt:
        print("\nTFTP server stopped.")
    except PermissionError:
        print(f"Error: Permission denied binding to port {args.port}")
        if args.port < 1024:
            print("Try running with sudo for privileged ports")
    except Exception as e:
        print(f"Error starting TFTP server: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()