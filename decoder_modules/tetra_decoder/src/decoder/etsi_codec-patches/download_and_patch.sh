#!/bin/sh

URL=https://www.etsi.org/deliver/etsi_en/300300_300399/30039502/01.03.01_60/en_30039502v010301p0.zip
HTTPURL=http://www.etsi.org/deliver/etsi_en/300300_300399/30039502/01.03.01_60/en_30039502v010301p0.zip
MD5_EXP=a8115fe68ef8f8cc466f4192572a1e3e
LOCAL_FILE=en_30039502v010301p0.zip

PATCHDIR=`pwd`
CODECDIR=`pwd`/../codec

echo Deleting $CODECDIR ...
[ -e $CODECDIR ] && rm -rf $CODECDIR

echo Creating $CODECDIR ...
mkdir $CODECDIR

if [ ! -f $LOCAL_FILE ]; then
	echo Downloading $URL ...
	curl -kLSs $URL -o $LOCAL_FILE
else
	echo Skipping download, file $LOCAL_FILE exists
fi
MD5=`md5sum $LOCAL_FILE | awk '{ print $1 }'`

echo Checking MD5SUM ...
if [ "$MD5" != "$MD5_EXP" ]; then
	FILESIZE=$(stat -c%s "$LOCAL_FILE")
	echo "MD5sum of ETSI reference codec file doesn't match($MD5, $MD5_EXP). File size is $FILESIZE"
	rm $LOCAL_FILE
	curl -kLSs $HTTPURL -o $LOCAL_FILE
	MD5=`md5sum $LOCAL_FILE | awk '{ print $1 }'`
	if [ "$MD5" != "$MD5_EXP" ]; then
		FILESIZE=$(stat -c%s "$LOCAL_FILE")
		echo "MD5sum of HTTP ETSI reference codec file also doesn't match($MD5, $MD5_EXP). File size is $FILESIZE"
		exit 1
	fi
fi

# ---------------------------------------------------------------------------
# The ETSI archive is a multi-stage zip: the top-level zip contains another
# zip (and a PDF), and that inner zip is what holds the actual c-code/ and
# amr-code/ trees expected by the patches.
# ---------------------------------------------------------------------------
echo Unpacking outer ZIP ...
cd $CODECDIR
unzip -o -L "$PATCHDIR/$LOCAL_FILE"

echo "Contents after outer unzip:"
ls -lah

INNER_ZIPS=$(find . -maxdepth 2 -type f -iname '*.zip')
if [ -n "$INNER_ZIPS" ]; then
	for z in $INNER_ZIPS; do
		echo "Unpacking inner ZIP: $z"
		unzip -o -L "$z"
	done
fi

echo "Contents after inner unzip:"
ls -lah

if [ ! -d "$CODECDIR/c-code" ]; then
	echo ""
	echo "ERROR: expected directory '$CODECDIR/c-code' was not found after"
	echo "extraction. Listing of $CODECDIR follows so you can diagnose:"
	find "$CODECDIR" -maxdepth 3
	exit 1
fi

echo Fixing filenames in c-code...
cd "$CODECDIR/c-code"
for f in *; do
	new="$(echo "$f" | tr 'I' 'i')"
	if [ "$f" != "$new" ]; then
		mv -i -- "$f" "$new"
	fi
done

if [ -d "$CODECDIR/amr-code" ]; then
	echo Fixing filenames in amr-code...
	cd "$CODECDIR/amr-code"
	for f in *; do
		new="$(echo "$f" | tr 'I' 'i')"
		if [ "$f" != "$new" ]; then
			mv -i -- "$f" "$new"
		fi
	done
fi

cd $CODECDIR

echo Applying Patches ...
cat "$PATCHDIR/series"
for p in `cat "$PATCHDIR/series"`; do
	echo "=> Applying patch '$PATCHDIR/$p'..."
	patch -p1 -d "$CODECDIR" < "$PATCHDIR/$p"
done

echo "Done! ETSI codec extracted to $CODECDIR"
echo "Verifying critical headers:"
for h in c-code/channel.h c-code/source.h; do
	if [ -f "$CODECDIR/$h" ]; then
		echo "  OK   $h"
	else
		echo "  MISS $h"
	fi
done
