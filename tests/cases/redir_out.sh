echo first > f
echo second > f
cat f
echo third >> f
cat f
> empty
wc -c < empty | tr -d " "
