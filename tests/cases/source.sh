printf 'SOURCED=yes\necho in sourced file\n' > lib.sh
. ./lib.sh
echo "SOURCED=$SOURCED"
SOURCED=no
source lib.sh
echo "SOURCED=$SOURCED"
source missing.sh
echo "status $?"
