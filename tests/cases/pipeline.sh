printf 'b\na\nb\nc\nb\n' | sort | uniq -c | awk '{print $1, $2}'
echo hello | cat | cat | tr a-z A-Z
false | true
echo $?
true | false
echo $?
echo builtin in pipe | wc -w | tr -d " "
