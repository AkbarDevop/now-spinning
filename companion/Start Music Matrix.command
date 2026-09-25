#!/bin/zsh
cd "${0:A:h:h}"
exec .venv/bin/python companion/bridge.py
