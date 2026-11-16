sh -c 'kill -TERM $$'
echo $?
sh -c 'kill -KILL $$'
echo $?
