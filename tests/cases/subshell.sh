X=outer
(X=inner; echo "in $X")
echo "out $X"
(cd /; pwd)
(exit 4)
echo "status $?"
(echo b; echo a) | sort
(echo redirected) > f
cat f
