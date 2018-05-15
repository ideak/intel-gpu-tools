#!/bin/sh

output=$1
filter=$2
testlist=$3
testdir=$(dirname $testlist)

KEYWORDS="(invalid|hang|swap|thrash|crc|tiled|tiling|rte|ctx|render|blt|bsd|vebox|exec|rpm)"

echo "<?xml version=\"1.0\"?>" > $output
echo "<!DOCTYPE refsect1 PUBLIC \"-//OASIS//DTD DocBook XML V4.3//EN\"" >> $output
echo "               \"http://www.oasis-open.org/docbook/xml/4.3/docbookx.dtd\"" >> $output
echo "[" >> $output
echo "  <!ENTITY % local.common.attrib \"xmlns:xi  CDATA  #FIXED 'http://www.w3.org/2003/XInclude'\">" >> $output
echo "  <!ENTITY version SYSTEM \"version.xml\">" >> $output
echo "]>" >> $output
echo "<refsect1>" >> $output
echo "<title>Description</title>" >> $output
for test in `cat $testlist | tr ' ' '\n' | grep "^$filter" | sort`; do
	echo "<refsect2 id=\"$test\"><title>" >> $output;
	echo "$test" | perl -pe "s/(?<=_)$KEYWORDS(?=(_|\\W))/<acronym>\\1<\\/acronym>/g" >> $output;
	echo "</title><para><![CDATA[" >> $output;
	testprog=$testdir/$test;
	 ./$testprog --help-description >> $output;
	echo "]]></para>" >> $output;
	if ./$testprog --list-subtests > /dev/null ; then
		echo "<refsect3><title>Subtests</title>" >> $output;
		subtest_list=`./$testprog --list-subtests`;
		subtest_count=`echo $subtest_list | wc -w`;
		if [ $subtest_count -gt 100 ]; then
			echo "<para>This test has over 100 subtests. " >> $output;
			echo "Run <command>$test</command> <option>--list-subtests</option> to list them.</para>" >> $output;
		else
			echo "<simplelist>" >> $output;
			for subtest in $subtest_list; do
				echo "<member>" >> $output;
				echo "$subtest" | perl -pe "s/\\b$KEYWORDS\\b/<acronym>\\1<\\/acronym>/g" >> $output;
				echo "</member>" >> $output;
			done;
			echo "</simplelist>" >> $output;
		fi;
		echo "</refsect3>" >> $output;
	fi;
	echo "</refsect2>" >> $output;
done;
echo "</refsect1>" >> $output
