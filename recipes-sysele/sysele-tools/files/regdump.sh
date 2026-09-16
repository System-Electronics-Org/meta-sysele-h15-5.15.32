#!/bin/sh
# Uso: regdump.sh <base esadecimale> <numero registri> <file>
b=$1; n=$2; out=$3; i=0
: > $out
while [ $i -lt $n ]; do
  o=$((i*4))
  v=$(devmem $((b + o)) 32 2>&1 | tr "\n" " ")
  printf "+0x%03x %s\n" $o "${v:-VUOTO}" >> $out
  i=$((i+1))
done
