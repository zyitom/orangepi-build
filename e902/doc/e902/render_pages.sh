#!/bin/bash
cd /home/helios/Desktop/orangepi-build/e902/doc || exit 1
PDF="OrangePi_Zero3W_A733_用户手册_v1.0.pdf"
rm -rf /tmp/pinpages && mkdir -p /tmp/pinpages
# §3.14 is printed page 91; the PDF has front matter, so render a generous range
pdftoppm -png -r 130 -f 88 -l 106 "$PDF" /tmp/pinpages/p
ls -la /tmp/pinpages/ | head -30
echo "--- also render the schematic page with the 40PIN CONN ---"
rm -rf /tmp/schpages && mkdir -p /tmp/schpages
pdftoppm -png -r 130 -f 1 -l 4 "OPI ZERO 3W V1_2_原理图.pdf" /tmp/schpages/s
ls -la /tmp/schpages/ | head
echo "--- page count of schematic ---"
pdfinfo "OPI ZERO 3W V1_2_原理图.pdf" 2>/dev/null | grep Pages
