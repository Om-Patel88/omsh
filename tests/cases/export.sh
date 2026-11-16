export X=hi
sh -c 'echo "X=$X"'
Y=local
sh -c 'echo "Y=[$Y]"'
export Y
sh -c 'echo "Y=$Y"'
Z=prefix sh -c 'echo "Z=$Z"'
echo "Z after=[$Z]"
unset X
sh -c 'echo "X=[$X]"'
