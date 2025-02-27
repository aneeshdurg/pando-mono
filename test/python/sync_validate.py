# SPDX-License-Identifier: MIT
# Copyright (c) 2023. University of Texas at Austin. All rights reserved.

import sys

if len(sys.argv) != 2:
    print("Usage: python3 projector.py (number of pxns)")
    sys.exit(1)

numHosts = int(sys.argv[1])

localMirrors = []

for i in range(numHosts):
    localMirrors.append([])

for line in sys.stdin:
    line_split = line.strip().split()
    if len(line_split) != 7:
        continue

    if line_split[0] == "(Mirror)":
        hostID = int(line_split[2])
        mirrorTokenID = int(line_split[4])
        mirrorData = int(line_split[6])
        localMirrors[hostID].append([mirrorTokenID, mirrorData])

for hostID in range(numHosts):
    for index in range(len(localMirrors[hostID])):
        mirrorInfo = localMirrors[hostID][index]

        mirrorTokenID = mirrorInfo[0]
        mirrorData = mirrorInfo[1]

        if (mirrorData != mirrorTokenID+2):
            sys.exit(1)

print("PASS")
