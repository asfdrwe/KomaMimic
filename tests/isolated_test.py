#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run a test and its children with private standard OS directories."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from isolated_environment import isolated_environment as test_environment

if __name__ == '__main__':
    command = sys.argv[1:]
    if not command:
        raise SystemExit('A test command is required')
    with tempfile.TemporaryDirectory(prefix='k') as directory:
        raise SystemExit(subprocess.run(command, env=test_environment(directory), check=False).returncode)
