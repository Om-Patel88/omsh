cd /
pwd
cd /usr
pwd
cd -
echo "OLDPWD=$OLDPWD PWD=$PWD"
cd /nonexistent-dir
echo "status $?"
pwd
