# A job that has already exited can still be waited for by pid or job spec.
sleep 0 &
p=$!
sleep 1
wait $p
echo "pid: $?"
sh -c 'exit 5' &
sleep 1
wait %%
echo "job: $?"
sleep 0 &
sleep 1
wait
wait %1
echo "after plain wait: $?"
