#!/bin/bash

input=$1
output=$2
basename=$(basename $1 .c)

echo "#define IGT_LOG_DOMAIN \"$basename\"" > $output
cat $input >> $output
