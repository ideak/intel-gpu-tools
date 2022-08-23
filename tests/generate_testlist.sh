#!/bin/sh

OUTPUT=$1
shift

echo TESTLIST > $OUTPUT

if [ $# -gt 0 ] ; then
	printf "$1" >> $OUTPUT
	shift
fi

while [ $# -gt 0 ] ; do
	printf " $1" >> $OUTPUT
	shift
done

printf "\nEND TESTLIST\n" >> $OUTPUT
