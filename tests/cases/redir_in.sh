printf 'one\ntwo\n' > in
wc -l < in | tr -d " "
cat < in | sort -r
