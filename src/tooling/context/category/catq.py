#!/usr/bin/env python3
"""Tiny executable wrapper for the context category query language."""
from __future__ import annotations
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from query_language import main
if __name__ == "__main__":
    raise SystemExit(main())
