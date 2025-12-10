import subprocess
import json
import os
import sys
import argparse
import re

def get_system_includes():
    """
    Query arm-none-eabi-gcc for its system include paths.
    """
    try:
        # Run gcc -v -E - to get include paths
        cmd = ["arm-none-eabi-gcc", "-v", "-E", "-", "-mcpu=cortex-m0plus", "-mthumb"]
        # Pass empty input to stdin
        result = subprocess.run(cmd, input="", capture_output=True, text=True, check=True)
        
        output = result.stderr
        includes = []
        parsing_includes = False
        
        for line in output.splitlines():
            line = line.strip()
            if line == "#include <...> search starts here:":
                parsing_includes = True
                continue
            if line == "End of search list.":
                parsing_includes = False
                continue
            
            if parsing_includes:
                includes.append(line)
                
        return includes
    except Exception as e:
        print(f"Warning: Could not determine system includes: {e}")
        return []

def main():
    parser = argparse.ArgumentParser(description='Generate compile_commands.json')
    parser.add_argument('--board', default='sensorwatch_pro', help='Target board')
    parser.add_argument('--display', default='classic', help='Display type')
    args = parser.parse_args()

    # Get system includes
    system_includes = get_system_includes()
    include_flags = " ".join([f"-isystem {inc}" for inc in system_includes])
    print(f"Detected system includes: {system_includes}")

    # command to run make dry-run
    cmd = ["make", f"BOARD={args.board}", f"DISPLAY={args.display}", "-B", "-n"]
    
    try:
        # Run the command and capture output
        process = subprocess.run(cmd, capture_output=True, text=True, check=True)
        lines = process.stdout.splitlines()
    except subprocess.CalledProcessError as e:
        print(f"Error running make: {e}")
        print(e.stderr)
        sys.exit(1)

    compile_commands = []
    cwd = os.getcwd()

    for line in lines:
        line = line.strip()
        # Filter for compiler commands. 
        # Typically they contain ' -c ' and end with the object file or have -o
        if line.startswith("arm-none-eabi-gcc"):
            # We need to extract the source file. It is usually the argument before -c or just one of the args.
            # In the output seen: ... source_file.c -c -o object_file.o
            # We can just use the full command.
            
            # Simple heuristic to find the source file: 
            # Look for arguments ending in .c
            parts = line.split()
            source_file = None
            for part in parts:
                if part.endswith(".c"):
                    source_file = part
                    break
            
            if source_file:
                # Remove gcc-specific flags that clangd doesn't understand
                line = line.replace("-fno-diagnostics-show-caret", "")

                # Add system includes to the command
                command_with_includes = f"{line} {include_flags}"
                
                entry = {
                    "directory": cwd,
                    "command": command_with_includes,
                    "file": source_file
                }
                compile_commands.append(entry)

    with open("compile_commands.json", "w") as f:
        json.dump(compile_commands, f, indent=4)

    print(f"Generated compile_commands.json with {len(compile_commands)} entries.")

if __name__ == "__main__":
    main()
