#!/bin/bash
M=/tmp/opi_zero3w_man.txt
echo "########## SPI3 段（3.16.3）##########"
N=$(grep -n '3\.16\.3\. 40 pin SPI 测试' "$M" | tail -1 | cut -d: -f1)
sed -n "${N},$((N+60))p" "$M"

echo
echo "########## I2C 段（3.16.4）##########"
N=$(grep -n '3\.16\.4\. 40 pin I2C 测试' "$M" | tail -1 | cut -d: -f1)
sed -n "${N},$((N+50))p" "$M"

echo
echo "########## UART 段（3.16.5）##########"
N=$(grep -n '3\.16\.5\. 40 pin 的 UART 测试' "$M" | tail -1 | cut -d: -f1)
sed -n "${N},$((N+50))p" "$M"

echo
echo "########## 手册里出现的所有「N 号引脚」映射 ##########"
grep -n -oE '[0-9]+ ?号引脚[^，。；]*' "$M" | sort -u -t: -k2 | head -40
echo
echo "########## 手册里出现的所有「PB4/PB7/PE...」样式 ##########"
grep -n -oE 'P[BLEKMD][0-9]+' "$M" | sort -u -t: -k2 | head -40
