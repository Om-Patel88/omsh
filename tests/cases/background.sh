echo bg-out > f &
sleep 0 &
echo after
wait
cat f
