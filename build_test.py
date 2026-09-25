#!/usr/bin/env python3
"""Deprecated: use ./matrix (build, flash, status, test). Kept so old notes still work."""
import subprocess
import sys
from pathlib import Path

sys.exit(subprocess.call([sys.executable, str(Path(__file__).resolve().parent / 'matrix'), 'build']))
