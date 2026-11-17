sleep 30 &
kill %1
wait %1
echo "killed: $?"
sleep 30 &
pid=$!
kill -KILL $pid
wait $pid
echo "status: $?"
(exit 7) &
wait $!
echo "exit: $?"
sleep 30 &
kill -s HUP %%
wait
echo "wait: $?"
kill %5
echo "no job: $?"
kill -BOGUS 1
echo "bad signal: $?"
wait 1
echo "not a child: $?"
