X="a b c"
printf '[%s]\n' $X
printf '[%s]\n' "$X"
Y="  lead  and  trail  "
printf '<%s>\n' $Y
printf '<%s>\n' x$Y"y"
E=""
printf '{%s}\n' $E "" $E
IFS=:
P="a::b:"
printf '(%s)\n' $P
Q="a : b"
IFS=" :"
printf '(%s)\n' $Q
IFS=
printf '|%s|\n' $X
unset IFS
printf '+%s+\n' $(echo "one  two"; echo three)
printf '+%s+\n' "$(echo "one  two"; echo three)"
Z="a "
printf '=%s=\n' $Z""
G="*.nomatch x"
printf '%s\n' $G
v=$X
printf '[%s]\n' "$v"
echo $(echo "multi
line")
