#!/bin/bash

OUTPUT=$1
shift

echo TESTLIST > $OUTPUT

while [[ $# -gt 0 ]] ; do
	echo $1 >> $OUTPUT
	shift
done

echo END TESTLIST >> $OUTPUT
