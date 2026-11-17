echo "before: [$!]"
sh -c 'echo $$ > pid' &
wait
read_pid=$!
cat pid > /dev/null
test -n "$read_pid" && echo "set after &"
sh -c 'echo $$' > child &
p=$!
wait $p
echo $p > expected
cmp -s child expected && echo "matches child pid"
