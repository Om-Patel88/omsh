sh -c 'echo to-err >&2' 2> e
echo "stdout is clean"
cat e
sh -c 'echo again >&2' 2>> e
cat e
