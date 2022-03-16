#!/bin/bash

MERGED_INFO="merged"
GATHER_ON_BUILD="code_cov_gather_on_build.sh"
PARSE_INFO="code_cov_parse_info.pl"

trap 'catch $LINENO' ERR
catch() {
	echo "$0: error on line $1. HTML report not generated."
	exit $1
}

usage() {
    printf >&2 "\
Usage:
    $(basename $0)
	--read <file or dir> --kernel-source <dir> --kernel-object <dir>
	--output-dir <dir> [--info or --tar] [--force-override]

--kernel-object is only needed when Kernel was built with make O=dir
"
    exit $1
}

MODE=
RESULTS=
KSRC=
KOBJ=
DEST_DIR=
FORCE=

while [ "$1" != "" ]; do
	case $1 in
	--info|-i)
		MODE=info
		;;
	--tar|--tarball|-t)
		MODE=tar.gz
		;;
	--kernel-source|-k)
		if [ "$2" == "" ]; then
			usage 1
		else
			KSRC=$(realpath $2)
			shift
		fi
		;;
	--kernel-object|-O)
		if [ "$2" == "" ]; then
			usage 1
		else
			KOBJ=$(realpath $2)
			shift
		fi
		;;
	--output-dir|-o)
		if [ "$2" == "" ]; then
			usage 1
		else
			DEST_DIR=$(realpath $2)
			shift
		fi
		;;
	--read|-r)
		if [ "$2" == "" ]; then
			usage 1
		else
			RESULTS=$(realpath $2)
			shift
		fi
		;;
	--force-override|-f)
		FORCE=1
		;;
	--help)
		usage 0
		;;

	*)
		echo "Unknown argument '$1'"
		usage 1
		;;
	esac
	shift
done

if [ "x$RESULTS" == "x" -o "x$KSRC" == "x" -o "x$DEST_DIR" == "x" -o "x$MODE" == "x" ]; then
	echo "Missing a mandatory argument"
	usage 1
fi

if [ -z "$KOBJ" ]; then
	KOBJ=$KSRC
fi

SCRIPT_DIR=$(dirname $(realpath $0))
RESULTS=$(realpath $RESULTS)
KSRC=$(realpath $KSRC)
KOBJ=$(realpath $KOBJ)
DEST_DIR=$(realpath $DEST_DIR)

if [ -e "$DEST_DIR" ]; then
	if [ "x$FORCE" != "x" -a -d "$DEST_DIR" ]; then
		rm -rf $DEST_DIR/
	else
		echo "Directory exists. Won't override."
		exit 1
	fi
fi

mkdir -p $DEST_DIR
cd $DEST_DIR

if [ "$MODE" != "info" ]; then
	echo "Generating source tarball from $KSRC (O=$KOBJ)..."
	${SCRIPT_DIR}/${GATHER_ON_BUILD} $KSRC $KOBJ source.tar.gz

	echo "Adding source files..."
	tar xf source.tar.gz

	if [ -d "$RESULTS" ]; then
		echo "Creating per-file info files..."
		echo -n "" >${MERGED_INFO}.info
		for i in $RESULTS/*.tar.gz; do
			TITLE=$(basename $i)
			TITLE=${TITLE/.tar.gz/}

			echo "Adding results from $i..."
			tar xf $i

			echo "Generating $TITLE.info..."
			lcov -q -t ${TITLE} --rc lcov_branch_coverage=1 -o $TITLE.info -c -d .

			cat $TITLE.info >>${MERGED_INFO}.info

			# Remove the contents of the results tarball
			rm -rf sys/
		done

		TITLE=${MERGED_INFO}
	else
		TITLE=$(basename $RESULTS)
		TITLE=${TITLE/.tar.gz/}

		echo "Adding results from $RESULTS..."
		tar xf $RESULTS

		echo "Generating $TITLE.info..."
		lcov -q -t ${TITLE} --rc lcov_branch_coverage=1 -o $TITLE.info -c -d .
	fi
else
	if [ -d "$RESULTS" ]; then
		echo "Merging info files..."
		echo -n "" >${MERGED_INFO}.info
		for i in $RESULTS/*.info; do
			cat $i >>${MERGED_INFO}.info
		done

		TITLE=${MERGED_INFO}
	else
		echo "Copying $RESULTS to $DEST_DIR..."
		cp $RESULTS .

		TITLE=$(basename $RESULTS)
		TITLE=${TITLE/.info/}
	fi
fi

echo "Generating HTML files..."
genhtml -q ${TITLE}.info
