printf 'b\na\nb\nc\nb\n' | sort | uniq -c
echo hello | cat | cat | tr a-z A-Z
false | true
echo $?
true | false
echo $?
echo builtin in pipe | wc -w
