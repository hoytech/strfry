#!/bin/sh

die() 
{
    echo $1 && exit 1
}

which -s pkg || die "pkg(8) is not found. Aborting."
pwd=$(pwd)
cd "$(dirname "$0")"

if [ ! -z "$1" ];
then
    manifest_in="$1"
else
    manifest_in="MANIFEST.in"
fi

if [ ! -z "$2" ];
then
    root_dir="$2"
else
    root_dir="root"
fi

mkdir -p ${root_dir}/usr/local/bin
mkdir -p ${root_dir}/var/lib/strfry
mkdir -p ${root_dir}/etc/
mkdir -p ${root_dir}/usr/local/etc/rc.d

cp ../strfry ${root_dir}/usr/local/bin
cp ../strfry.conf ${root_dir}/etc/
cp rc.strfry ${root_dir}/usr/local/etc/rc.d/strfry
sed 's|./strfry-db/|/var/lib/strfry/|g' ${root_dir}/etc/strfry.conf > ${root_dir}/etc/strfry.conf.tmp && mv ${root_dir}/etc/strfry.conf.tmp ${root_dir}/etc/strfry.conf

VERSION=$(curl -s https://api.github.com/repos/hoytech/strfry/tags | grep -o '"name": "[^"]*' | awk -F'"' 'NR==1 {print $4}')

if [ -z "$VERSION" ];
then
    VERSION="0.9.0"
fi

DEP_LIST="openssl lmdb flatbuffers libuv libinotify zstd secp256k1 zlib-ng gcc"

DIR_SIZE=$(find ${root_dir} -type f -exec stat -f %z {} + | awk 'BEGIN {s=0} {s+=$1} END {print s}')

for P in ${DEP_LIST}; do
    O=$(echo $(pkg search -e -S name -Q full $P | grep Origin | cut -d: -f2))
    V=$(echo $(pkg search -e -S name -Q full $P | grep Version | cut -d: -f2))
    if [ ! -z $O ] && [ ! -z $V ]; then
	# not sure about ,1 in version string.  pkg no like.
	V=${V%,*}
        deps="$deps     $P: {origin: $O, version: $V},
"
    fi
done

DEPS="deps:{ 
$deps}"
export VERSION
export DEPS
export DIR_SIZE
manifest=$(envsubst < MANIFEST.in)
echo "$manifest" > +MANIFEST

# Create the package
pkg create -r ${root_dir} -m . -o ..

# Verify package
pkgfile=$(ls ../*.pkg)
if [ -f "$pkgfile" ];
then
  # pkg info -R -F ${pkgfile}
  pkg info -F ${pkgfile}
  tar -Jtvf ${pkgfile}
else
  echo "Package create failed."
  exit 1
fi

# Clean up
rm -rf +MANIFEST +COMPACT_MANIFEST ${root_dir}
cd "$pwd"
exit 0
