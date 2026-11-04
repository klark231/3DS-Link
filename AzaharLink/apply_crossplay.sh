#!/bin/bash
# Run from the AzaharLink folder, after this package's files are in place.
set -e
python3 crossplay_scaffold.py
python3 crossplay_phase2.py
echo
echo "OK. crossplay.cpp/.hpp, Sources/mhgu/, Includes/mhgu_compat/ are in place and the"
echo "Makefile + plugin are patched. Next: paste README_crossplay_section.md into the repo's"
echo "top README.md above '## Requirements', then commit and push."
